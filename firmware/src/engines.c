/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Engine table: order = PRESETS browsing order (and the engine numbers of the editor protocol). */
#include "dsp.c"
/* SLOOP 2.5: the big per-part state of the engines (sloop_custom: DRAWBAR, FMTONE, SAW PULS ENS WAV) shares one arena per synth part (a part plays one
 * engine at a time: on a switch the old engine fades out, its voices end, then the new one starts, voice.c
 * engine_block). The first engine to ask for it after another one gets it cleared, as at power-on. */
static void *eng_arena_of(const track_t *t, uint32_t eng);
static inline int32_t voice_amp(int32_t s, const vmod_t *m, uint32_t i) { return mulq15(mulq15(s, amp_at(m, i)), VOICE_FS); }
static inline int32_t soft_knee(int32_t y, int32_t k)   /* linear up to k, then only the peaks saturate */
{
    int32_t a = y < 0 ? -y : y;
    if (a <= k)
        return y;
    a = k + (softclip((a - k) * 2) >> 1);
    return y < 0 ? -a : a;
}
/* sloop_custom: Mini-Tone's resonant SVF (its dsp.c, unchanged): ANALOG4's two filters, FMTONE's voice filter */
/* The resonant filter of the Elektron-style engines (SWARM, WAVETONE, FMTONE, CHIP, DUALVCO, ANALOG4): Simper's trapezoidal SVF
 * again, with a resonance that runs on past the usual peak into self-oscillation. The state keeps 8 more bits than the
 * signal and the coefficients are Q24 (a cutoff of 30 Hz is g = 0.002: Q12 coefficients are 0 below ~700 Hz and the filter
 * would die there), the products are 64-bit (as FM6's). The low-pass state saturates (a soft knee at 6500, the signal's
 * full scale being 16384) and the band-pass state a good deal later: a high Q gets rounder and fatter, as an analog
 * filter's peak does, and at the top of RES (k below 0) the filter rings on its own, a near-sine at the cutoff whose level
 * the low-pass state's knee holds, at every pitch. Above RES 100 the input falls (to 0.4 at RES 127): the closer to the ring, the
 * more the filter is the sound's source, and a loud voice does not quench the ring (the way a driven analog oscillator does).
 * res 0..127 (RES_K: Q 0.5 .. 32, self-oscillating from 118), cut 0..127 << 8 as tsvf_coef (a semitone is 299 of the 1/256
 * steps, so a key-tracked cutoff can put the ring on the note: rsvf_key) */
typedef struct { int32_t a1, a2, a3, k, ig; } rsvf_t;
static inline void rsvf_coef(rsvf_t *c, int32_t cut, int32_t res)
{
    int32_t i, g, k = RES_K[clamp(res, 0, 127)];
    uint32_t den;
    cut = clamp(cut, 0, 127 << 8);
    i = cut >> 8;
    g = (int32_t)RSVF_G[i] + (int32_t)(((int64_t)((int32_t)RSVF_G[i + 1] - (int32_t)RSVF_G[i]) * (cut & 255)) >> 8);
    den = (1u << 24) + (uint32_t)(((int64_t)g * (g + (k << 12))) >> 24);     /* 1 + g (g + k), Q24 */
    c->a1 = (int32_t)(0xFFFFFFFFu / (den >> 8)) << 8;                        /* 1 / den, Q24 */
    c->a2 = (int32_t)(((int64_t)c->a1 * g) >> 24);
    c->a3 = (int32_t)(((int64_t)c->a2 * g) >> 24);
    c->k = k;
    c->ig = res <= 100 ? 4096 : 4096 - (clamp(res, 0, 127) - 100) * 91;          /* the input, Q12: from RES 100 it falls to 0.4 */
}
/* the cutoff of a note: cut (knob, 0..127) plus the modulation m (1/256), and key / 127 of the pitch above middle C (pitch16
 * 960) at 18.7 of 1/256 knob steps a 1/16 semitone (2411 / 16384 per unit of key): key 127 follows the note one to one, so
 * CUT 44 (262 Hz) rings on C4 */
static inline int32_t rsvf_key(int32_t cut, int32_t m, int32_t key, int32_t pitch16)
{
    return (cut << 8) + m + (((pitch16 - 960) * key * 2411) >> 14);
}
/* a note on a self-oscillating filter (RES from 119): the low-pass state starts off zero, so that a silent voice has something
 * to ring from (Q8; 2000 is an eighth of full scale, then the ring grows to its level within a few cycles of the cutoff) */
static inline int32_t rsvf_seed(int32_t res) { return res >= 119 ? 2000 << 8 : 0; }
/* a state (Q8) through the soft knee at K (Q0): exact below it */
static inline int32_t rsvf_sat(int32_t x, int32_t k)
{
    int32_t a = x >> 8;
    if (a < 0 ? -a <= k : a <= k)
        return x;
    return soft_knee(a, k) << 8;
}
/* one sample (in: +-16384 for a full-scale signal), type 0 LP, 1 BP (constant skirt: a gain of Q at the cutoff), 2 HP; the result in the same scale.
 * ic1 ic2: the state, Q8 */
static inline int32_t rsvf_run(const rsvf_t *c, int32_t in, int32_t *ic1, int32_t *ic2, uint32_t type)
{
    int32_t x = (in * c->ig) >> 4, v3 = x - *ic2, v1, v2;                   /* in << 8, times ig / 4096 */
    v1 = (int32_t)(((int64_t)c->a1 * *ic1 + (int64_t)c->a2 * v3) >> 24);
    v2 = *ic2 + (int32_t)(((int64_t)c->a2 * *ic1 + (int64_t)c->a3 * v3) >> 24);
    *ic1 = rsvf_sat(2 * v1 - *ic1, 120000);
    *ic2 = rsvf_sat(2 * v2 - *ic2, 6500);
    if (type == 0)
        return (v2 + 128) >> 8;
    if (type == 1)
        return (v1 + 128) >> 8;                           /* constant skirt: the peak rises with Q (1 / k), as the analog one does */
    return (x - (int32_t)(((int64_t)v1 * c->k) >> 12) - v2 + 128) >> 8;
}

/* (pitch_inc: felucca_tables.h, SLOOP 2.5: from the top octave) */
/* (after Clipper 1.2) UNISON's start-phase spread (voice.c unison_spread): r (Q32: a turn of the note at pitch p16,
 * 1/16 semitone) in turns of an oscillator d16 (1/16 semitone) above it, the share of the same time; per note */
static uint32_t uni_turns(uint32_t r, int32_t p16, int32_t d16)
{
    uint32_t b = pitch_inc((uint32_t)clamp(p16, 0, 2047));
    return b ? (uint32_t)(((uint64_t)r * pitch_inc((uint32_t)clamp(p16 + d16, 0, 2047))) / b) : r;
}

/* sloop_custom: the engines (core.h ENGI_*): Mini-Tone's FMTONE, MONO2K's ENS WAV (Melodee Custom's
 * port), SLOOP's LOFI, VOICE, DRAWBAR, NOISE. eng_sample.c stays for what the drums, the texture and the editor need of it
 * (the GM kit's samples, the user sample slots); eng_fm6.c stays compiled (the editor's FM6 bank, the stores), no track
 * plays it (ENGI_FM6 is past NENGINES). ANALOG, DIGITAL, PHASE, SAMPLE, TRIO, GRAIN, PHYS and SLICE are gone. */
#include "eng_lofi.c"
#include "eng_sample.c"
#include "eng_formant.c"
#include "eng_drawbar.c"
#include "eng_fm6.c"            /* FM6: 6-operator FM, msfa ported (fm6_core.c, Apache-2.0); SLOOP 2.4 (not played) */
#include "eng_noise.c"          /* NOISE: from Felucca 1.0; SLOOP 2.5 */
#include "eng_fmtone.c"         /* FMTONE: Mini-Tone's */
#include "eng_m2k.c"            /* SAW PULS ENS WAV: MONO2K's machines (Sequencist), Melodee Custom's port */
#include "eng_golana.c"        /* GOLANA: the oscillator section of Golana (Gadlele) */

typedef union {
    drw_part_t drawbar;                  /* eng_drawbar.c: the part's wheels, percussion and rotary (sloop-lab) */
    fm6_part_t fm6;                      /* eng_fm6.c: six notes and the operator buses (compiled, not played) */
    fmt_slot_t fmtone[FMT_POLY];         /* eng_fmtone.c: its voices (Mini-Tone: ENG_MEM) */
    m2k_part_t m2k;                      /* eng_m2k.c: the machines of its voices, ENS's chorus line */
    gol_part_t golana;                   /* eng_golana.c: its voices' noise, drift and folds, the LFO */
} eng_arena_t;
static eng_arena_t eng_arena[NPART] __attribute__((section(".pool")));
static uint8_t eng_arena_own[NPART];            /* the engine + 1 whose state is in it, 0 = none */
#ifdef ARENA_STATS
static uint32_t eng_arena_claims, eng_arena_bad;   /* host tests: claims, accesses by an engine the part does not play */
#endif
static void *eng_arena_of(const track_t *t, uint32_t eng)
{
    uint32_t p = (uint32_t)(t - trk);
    if (p >= NPART)
        return 0;
#ifdef ARENA_STATS
    if (t->engine != eng)
        eng_arena_bad++;
#endif
    if (eng_arena_own[p] != eng + 1u) {
        memset(&eng_arena[p], 0, sizeof eng_arena[p]);
        eng_arena_own[p] = (uint8_t)(eng + 1u);
#ifdef ARENA_STATS
        eng_arena_claims++;
#endif
    }
    return &eng_arena[p];
}

static const engine_t *const ENGINES[NENGINES] = {
    &ENG_FMTONE,                 /* 0 ENGI_FMTONE */
    &ENG_M2K_ENS,                /* 1 ENGI_ENS */
    &ENG_M2K_WAV,                /* 2 ENGI_WAV */
    &ENG_LOFI,                   /* 3 ENGI_LOFI */
    &ENG_FORMANT,                /* 4 ENGI_FORMANT (VOICE) */
    &ENG_DRAWBAR,                /* 5 ENGI_DRAWBAR */
    &ENG_NOISE,                  /* 6 ENGI_NOISE */
    &ENG_GOLANA,                 /* 7 ENGI_GOLANA */
};

/* sloop_custom: UNISON's start-phase spread (voice.c unison_spread, after Clipper 1.2) for the engines ported here; their
 * originals start every voice in phase. r: Q32 of a turn of the note; each oscillator moved by as many of its own turns
 * as it makes in that time (uni_turns), so a voice sounds as if started that much sooner */
static void fmt_spread(track_t *t, voice_t *v, uint32_t r)
{
    fmt_slot_t *S = fmt_slot_of(t, v);
    const int16_t *p = t->p;
    uint32_t rt[4], k;
    if (!S)
        return;
    rt[0] = FMT_RATIO_Q8[clamp(p[P_E1], 0, FMT_NRATIO - 1)];   /* C, A, B1, B2 as fmt_render: their ratios, Q8 */
    rt[1] = FMT_RATIO_Q8[clamp(p[P_E2], 0, FMT_NRATIO - 1)];
    rt[2] = FMT_RATIO_Q8[clamp(p[P_E3], 0, FMT_NRATIO - 1)];
    rt[3] = rt[2] * 2u > 4096u ? 4096u : rt[2] * 2u;
    for (k = 0; k < 4u; k++)
        S->ph[k] += (uint32_t)(((uint64_t)r * rt[k]) >> 8);
}
static void m2k_spread(track_t *t, voice_t *v, uint32_t r)
{
    m2k_voice_t *s = m2k_voice_of(t, v);
    uint32_t k;
    if (!s)
        return;
    for (k = 0; k < 7u; k++)                           /* (each oscillator a share r of its own cycle on) */
        s->ph[k] += r;
}

/* every factory sound as loud as the others: a level trim per preset, 1/2 dB, measured on a phrase
 * that fits the sound (tools/level_presets.py writes preset_trim.h); a track keeps it in P_ED_FX */
#include "preset_trim.h"
static int16_t preset_trim(uint32_t e, uint32_t pi)
{
    return e < PT_ENGINES && pi < PT_MAX ? PRESET_TRIM[e][pi] : 0;
}
