/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Gadlele, based on Felucca (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* GOLANA: the oscillator section of Golana (github.com/Gadlele/Golana, 1.3, eng_golana.c), an analog-style synth after the classic
 * microKORG layout, as an engine of sloop_custom. Four pages of four:
 *
 *   OSC 1   WAVE: SAW (CTRL1: a second saw an octave up blended in), PULSE (the width, 50 % .. ~6 %), TRI (folds the wave), SINE
 *           (OSC 2 moves its phase: cross modulation), MORPH (the saw to the square). DEPTH: a triangle LFO on CTRL1 (PWM, a
 *           moving fold ...) at RATE (Golana's CTRL2 and its LFO 1, one LFO for the part)
 *   OSC 2   WAVE2 SAW / SQR / TRI / MIX (a saw and a square at once), MOD OFF / RING / SYNC / R+S, SEMI +-24, TUNE +-63 ct
 *   MIXER   OSC 1, OSC 2 and NOISE levels, FOLD: a wavefolder on the mix (Buchla / Serge style: up to ~8x into the folds)
 *   SUB     a square, sine, saw or a mix an octave or two below OSC 1, in step with it, its level; DRIFT: analog age, every note's
 *           oscillators a little out of tune, each its own way, slowly wandering, the oscillators free running
 *
 * Not ported: Golana's own filters (five types), its filter EG, the second LFO and the patch bay, its effects: the sound goes
 * through the track's amp ADSR, FILTER (FX > FILTER: cutoff, RES, KEY, DIST) and sends, which the factory sounds set to what
 * Golana's did (tools/golana_presets.py). Up to GOL_POLY voices. */

static inline int32_t mulq13(int32_t a, int32_t b) { return (a * b) >> 13; }   /* 4 x mulq15: the mixer keeps 2 bits more */
#define GOL_POLY 6               /* engine_t.poly */

static const char *const N_GO1WAVE[] = {"SAW", "PULSE", "TRI", "SINE", "MORPH"};
static const char *const N_GO2WAVE[] = {"SAW", "SQR", "TRI", "MIX"};
static const char *const N_GO2MOD[] = {"OFF", "RING", "SYNC", "R+S"};
static const char *const N_GSUB[] = {"-1 SQR", "-2 SQR", "-1 SINE", "-2 SINE", "-1 SAW", "-2 SAW", "-1 MIX", "-2 MIX"};
enum { GO_SAW, GO_PULSE, GO_TRI, GO_SINE, GO_MORPH };
enum { GM_OFF, GM_RING, GM_SYNC, GM_RSYNC };
/* the slots past EDIT 1 and 2 (P_FM1_ATK ..; engine_t.tdesc) */
#define GL_MIX1 P_FM1_ATK
#define GL_MIX2 P_FM1_DEC
#define GL_NOISE P_FM1_SUS
#define GL_FOLD P_FM1_REL
#define GL_SUBTYPE P_FM1_LEVEL
#define GL_SUB P_FM2_ATK
#define GL_DRIFT P_FM2_DEC

#define GPD(l, f, mn, mx, df) {l, f, mn, mx, df, 0, 0}
static const param_desc_t GOL_TD[P_FM4_LEVEL - P_FM1_ATK + 1] = {
    [GL_MIX1 - P_FM1_ATK] = GPD("OSC1", F_PCT, 0, 127, 127),
    [GL_MIX2 - P_FM1_ATK] = GPD("OSC2", F_PCT, 0, 127, 0),
    [GL_NOISE - P_FM1_ATK] = GPD("NOISE", F_PCT, 0, 127, 0),
    [GL_FOLD - P_FM1_ATK] = GPD("FOLD", F_PCT, 0, 127, 0),
    [GL_SUBTYPE - P_FM1_ATK] = {"TYPE", F_ENUM, 0, 7, 0, N_GSUB, 0},
    [GL_SUB - P_FM1_ATK] = GPD("SUB", F_PCT, 0, 127, 0),
    [GL_DRIFT - P_FM1_ATK] = GPD("DRIFT", F_PCT, 0, 127, 0),
};
#undef GPD
static const param_desc_t *gol_tdesc(uint32_t id)
{
    const param_desc_t *d = &GOL_TD[id - P_FM1_ATK];
    return d->label ? d : 0;
}

typedef struct {
    int32_t nst;                 /* noise */
    int32_t jl;                  /* OSC 2 sync: 1 + the step of a restart a sample ago (16 bits), 0 none */
    uint8_t sc;                  /* SUB: OSC 1's cycles (its low 2 bits: the sub's phase above OSC 1's) */
    uint16_t dn;                 /* DRIFT: blocks until the wander's next target */
    int16_t d1, d2;              /* DRIFT: this note's offsets, OSC 1 and OSC 2 (Q15, -1..1) */
    int16_t dw, dt;              /* DRIFT: the slow wander and its target (Q15) */
    int32_t fx, fg, tx, tg;      /* the folds' ADAA: FOLD's and TRI's last input and its antiderivative */
    int32_t dc;                  /* SYNC: the DC a restarted ramp leaves, followed and taken off (a leaky integrator) */
} gol_slot_t;
typedef struct {
    gol_slot_t v[GOL_POLY];
    uint32_t lfo;                /* the part's LFO phase (CTRL1's) */
    int32_t seed;
} gol_part_t;
static gol_part_t *gol_part(const track_t *t) { return (gol_part_t *)eng_arena_of(t, ENGI_GOLANA); }
static gol_slot_t *gol_slot_of(const track_t *t, const voice_t *v)
{
    gol_part_t *g;
    uint32_t i = (uint32_t)(v - t->v);
    if (t < &trk[0] || t >= &trk[NPART] || i >= GOL_POLY)
        return 0;
    g = gol_part(t);
    return g ? &g->v[i] : 0;
}

/* a phase increment for pitch (1/16 semitone), cents and the residual fine (1/4096 of the pitch) */
static inline uint32_t gol_inc(int32_t pitch16, int32_t cents, int32_t fine)
{
    uint32_t inc = pitch_inc(clamp(pitch16, 0, 2047));
    fine += cents * 2367 / 1000;
    if (fine)
        inc += (uint32_t)((int32_t)(inc >> 12) * fine);
    return inc;
}

/* the folders without their aliasing, ADAA (antiderivative anti-aliasing, first order). The fold is the triangle T(x) = a - |w|,
 * w = x - a wrapped into [-2a, 2a) (period 4a, a = 1 << lg); the output is T's mean between the last input and this one */
static inline int32_t gl_tri_w(int32_t x, uint32_t lg)
{
    return (int32_t)(((uint32_t)x + (1u << lg)) & ((4u << lg) - 1u)) - (int32_t)(2u << lg);
}
static inline int32_t gl_tri_int(int32_t x, uint32_t lg)
{
    int32_t w = gl_tri_w(x, lg);
    return (w * ((2 << lg) - (w < 0 ? -w : w))) >> 1;
}
static inline int32_t gl_fold_aa(int32_t x, int32_t *xp, int32_t *gp, uint32_t lg)
{
    int32_t g = gl_tri_int(x, lg), d = x - *xp, y;
    if (d > 3 || d < -3) {
        y = (g - *gp) / d;
    } else {
        int32_t w = gl_tri_w((x + *xp) >> 1, lg);
        y = (1 << lg) - (w < 0 ? -w : w);
    }
    *xp = x;
    *gp = g;
    return y;
}

static void gol_block(track_t *t)
{
    gol_part_t *g = gol_part(t);
    if (!g)
        return;
    g->lfo += LFO_INC[clamp(t->p[P_E3], 0, 127)];
    if (!g->seed)
        g->seed = 0x5EED1234;
}

static void gol_note_on(track_t *t, voice_t *v)
{
    gol_part_t *g = gol_part(t);
    gol_slot_t *s = gol_slot_of(t, v);
    if (!g || !s)
        return;
    if (v->env == 0 && v->env_out == 0) {                  /* a fresh note: the oscillators free running, each from its own point */
        v->ph[0] = noise32(&g->seed);
        v->ph[1] = noise32(&g->seed);
        s->sc = (uint8_t)(v->ph[1] >> 30);
        s->jl = 0;
        s->dc = 0;
        s->fx = s->tx = 0;
        s->fg = gl_tri_int(0, 14);
        s->tg = gl_tri_int(0, 15);
        if (t->p[GL_DRIFT] > 0) {                          /* DRIFT: each note its own offsets */
            s->d1 = (int16_t)(noise32(&g->seed) >> 16);
            s->d2 = (int16_t)(noise32(&g->seed) >> 16);
        }
    }
    if (!s->nst)
        s->nst = 0x2468ACE1 + (int32_t)v->age;
}

/* UNISON's start-phase spread (voice.c unison_spread): both oscillators moved by r, the noise from its own place */
static void gol_spread(track_t *t, voice_t *v, uint32_t r)
{
    gol_slot_t *s = gol_slot_of(t, v);
    v->ph[0] += r;
    v->ph[1] += r;
    if (s)
        s->nst = (int32_t)(((uint32_t)s->nst ^ (r * 0x9E3779B9u)) | 1u);
}

/* a saw and a square at once, half each (OSC 2's MIX, the SUB's) */
static inline int32_t gl_mix(uint32_t ph, uint32_t inc) { return (osc_saw(ph, inc) >> 1) + osc_pulse(ph, inc, 0x80000000u); }
static inline int32_t gl_osc2(uint32_t w, uint32_t ph, uint32_t inc)
{
    return w == 1u ? osc_pulse(ph, inc, 0x80000000u) : w == 2u ? osc_tri(ph) : w == 3u ? gl_mix(ph, inc) : osc_saw(ph, inc);
}

/* DRIFT: the note's offsets and the wander (a new target every ~0.37 s, followed over about as long), in 1/4096 of the
 * pitch (2.37 a cent: up to +-12 ct each oscillator, +-5 ct the wander) -> *df1, *df2 */
static __attribute__((noinline)) void gl_drift(gol_part_t *g, gol_slot_t *s, int32_t drift, int32_t *df1, int32_t *df2)
{
    int32_t dw;
    if (!s->dn--) {
        s->dn = 511;
        s->dt = (int16_t)(noise32(&g->seed) >> 16);
    }
    s->dw = (int16_t)(s->dw + ((s->dt - s->dw) >> 9));
    dw = (s->dw * drift) >> 15;
    *df1 = (((s->d1 * drift) >> 15) * 28 >> 7) + (dw * 12 >> 7);
    *df2 = (((s->d2 * drift) >> 15) * 28 >> 7) + (dw * 12 >> 7);
}

static void gol_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    gol_part_t *g = gol_part(t);
    gol_slot_t *s = gol_slot_of(t, v);
    const int16_t *p = t->p;
    uint32_t w1, w2, md, i, st, so;
    int32_t ctrl1, x1, x2, xn, xs, fold, drift, df1 = 0, df2 = 0, ring, sync;
    int32_t pitch1, pitch2;
    uint32_t inc1, inc2;
    if (!g || !s)
        return;
    w1 = (uint32_t)clamp(p[P_E0], 0, 4), w2 = (uint32_t)clamp(p[P_E4], 0, 3), md = (uint32_t)clamp(p[P_E5], 0, 3);
    ring = md == GM_RING || md == GM_RSYNC, sync = md == GM_SYNC || md == GM_RSYNC;
    drift = clamp(p[GL_DRIFT], 0, 127);
    st = (uint32_t)clamp(p[GL_SUBTYPE], 0, 7), so = 1u + (st & 1u);   /* SUB: its type, its octaves down */
    if (drift)
        gl_drift(g, s, drift, &df1, &df2);
    /* pitches (1/16 semitone): the voice's (glide, unison, tune, bend in m) */
    pitch1 = m->pitch16;
    pitch2 = pitch1 + clamp(p[P_E6], -24, 24) * 16;
    inc1 = df1 ? gol_inc(pitch1, 0, m->fine + df1) : m->inc;
    /* UNISON: OSC 2 detuned the other way from OSC 1 (each voice two detuned oscillators) */
    inc2 = gol_inc(pitch2, clamp(p[P_E7], -63, 63), (p[P_VOICE] == V_UNISON ? m->fine - 2 * v->fine : m->fine) + df2);
    if (inc1 > 0x40000000u)
        inc1 = 0x40000000u;
    if (inc2 > 0x40000000u)
        inc2 = 0x40000000u;
    /* OSC 1's CTRL1, moved by the LFO (DEPTH) */
    ctrl1 = clamp(clamp(p[P_E1], 0, 127) + ((osc_tri(g->lfo) * clamp(p[P_E2], 0, 127)) >> 15), 0, 127);
    /* the mixer: levels Q15 (a full oscillator at about a quarter of the voice's scale) */
    x1 = clamp(p[GL_MIX1], 0, 127) * 258;
    x2 = clamp(p[GL_MIX2], 0, 127) * 258;
    xn = clamp(p[GL_NOISE], 0, 127) * 258;
    xs = clamp(p[GL_SUB], 0, 127) * 258;
    fold = clamp(p[GL_FOLD], 0, 127) ? 4096 + clamp(p[GL_FOLD], 0, 127) * 230 : 0;   /* FOLD: the gain into the folds, Q12 */
    {
        uint32_t ph0 = v->ph[0], ph1 = v->ph[1], pw, sc = s->sc;
        int32_t nst = s->nst, jl = s->jl, dc = s->dc;
        int32_t fx = s->fx, fg = s->fg, tx = s->tx, tg = s->tg;
        int32_t xm = ctrl1 * 380;                       /* SINE: the cross modulation's depth (phase, Q16 of a cycle) */
        if (m->pitch16 > 60 * 16)                       /* halved an octave above C4, a third two above: in tune high up */
            xm = xm * 192 / (192 + m->pitch16 - 60 * 16);
        int32_t tfold = 32768 + ctrl1 * 768;            /* TRI: the fold's gain, Q15 (1 .. 4) */
        int32_t oct = ctrl1 * 129;                      /* SAW: the octave-up saw's share */
        int32_t mrph = ctrl1 * 258;                     /* MORPH: the square's share, Q15 */
        pw = 0x80000000u + (uint32_t)ctrl1 * 0xF00000u; /* PULSE: 50 % .. ~6 % */
        for (i = 0; i < n; i++) {
            int32_t o1, o2, sx, y;
            /* OSC 2 first (SINE's cross modulation reads it); its sync restart rounded */
            if (sync && w2 == 0u) {
                int32_t bl = blep(ph1, inc2);
                if (jl) {
                    bl = (bl * ((jl - 1) >> 1)) >> 15;
                    jl = 0;
                }
                o2 = (int32_t)(ph1 >> 16) - 32768 - bl;
                if (ph0 > 0xFFFFFFFFu - inc1)
                    o2 -= (blep(ph0, inc1) * (int32_t)(ph1 >> 17)) >> 15;
            } else {
                o2 = gl_osc2(w2, ph1, inc2);
            }
            switch (w1) {
            case GO_PULSE:
                o1 = osc_pulse(ph0, inc1, pw);
                break;
            case GO_TRI:                                /* reflected at the edges: a triangle of period 131072 */
                o1 = gl_fold_aa((osc_tri(ph0) * (tfold >> 4)) >> 11, &tx, &tg, 15u);
                break;
            case GO_SINE:
                o1 = osc_sine(ph0 + (((uint32_t)o2 * (uint32_t)xm) << 1));   /* (wraps: a phase) */
                break;
            case GO_MORPH: {                            /* CTRL 1 from the saw to the square */
                int32_t sw = osc_saw(ph0, inc1);
                o1 = sw + mulq15((osc_pulse(ph0, inc1, 0x80000000u) << 1) - sw, mrph);
                break;
            }
            default:
                o1 = osc_saw(ph0, inc1);
                if (oct)
                    o1 = mulq15(o1, 32767 - (oct >> 1)) + mulq15(osc_saw(ph0 << 1, inc1 << 1), oct >> 1);
                break;
            }
            if (ring)
                o2 = mulq15(o1, o2);
            sx = mulq13(o1, x1) + mulq13(o2, x2);     /* (sx runs at 4 x the voice's scale to the end: the fades keep their bits) */
            if (xs) {                                   /* SUB: OSC 1's phase an octave or two down, OSC 1's cycles above it */
                uint32_t sp = sc << (32u - so) | ph0 >> so, si = inc1 >> so;
                int32_t u = st < 2u ? osc_pulse(sp, si, 0x80000000u) : st < 4u ? osc_sine(sp)
                          : st < 6u ? osc_saw(sp, si) : gl_mix(sp, si);   /* SQR, SINE, SAW, MIX */
                sx += mulq13(u, xs);
            }
            ph0 += inc1;
            ph1 += inc2;
            sc += ph0 < inc1;
            if (sync && ph0 < inc1) {                   /* OSC 1 wrapped: OSC 2 restarts with it */
                uint32_t f = ph0 / ((inc1 >> 16) | 1u);
                uint32_t at = ph1 - f * (inc2 >> 16);
                jl = (int32_t)(at >> 16) + 1;
                ph1 = f * (inc2 >> 16);
            }
            if (xn)
                sx += mulq13((int32_t)(noise32(&nst) >> 17) - 16384, xn);
            sx = (sx * 3) >> 3;                         /* one oscillator ~12 k, all three ~32 k */
            if (fold) {                                 /* FOLD: the wave reflected at +-16384, more folds the harder */
                int32_t u = gl_fold_aa((clamp(sx, -250000, 250000) * (fold >> 2)) >> 12, &fx, &fg, 14u);
                sx = (u + (u >> 2)) << 2;                      /* (x 1.25: about the level it came in at) */
            }
            if (sync) {                                 /* a synced ramp is lopsided: the DC off (~7 Hz) */
                dc += (sx - dc) >> 10;
                sx -= dc;
            }
            y = soft_knee(sx, 64000);
            out[i] += voice_amp(clamp(y, -262140, 262140), m, i);
        }
        v->ph[0] = ph0;
        v->ph[1] = ph1;
        s->sc = (uint8_t)sc;
        s->nst = nst;
        s->fx = fx;
        s->fg = fg;
        s->tx = tx;
        s->tg = tg;
        s->jl = jl;
        s->dc = dc;
    }
}

/* factory sounds (tools/golana_presets.py from Golana's 25): the engine values, then the slots of the other pages */
typedef struct { int8_t mix[3], fold, sub[2], drift; } gol_x_t;       /* OSC1 OSC2 NOISE, FOLD, SUB TYPE and level, DRIFT */
#include "golana_presets.h"

static void gol_track_loaded(track_t *t)
{
    const gol_x_t *x;
    if (t->eng_req != ENGI_GOLANA)
        return;
    x = &GOL_X[(uint32_t)t->preset % NELEM(GOL_X)];
    t->p[GL_MIX1] = x->mix[0];
    t->p[GL_MIX2] = x->mix[1];
    t->p[GL_NOISE] = x->mix[2];
    t->p[GL_FOLD] = x->fold;
    t->p[GL_SUBTYPE] = x->sub[0];
    t->p[GL_SUB] = x->sub[1];
    t->p[GL_DRIFT] = x->drift;
}

static const engine_t ENG_GOLANA = {
    .name = "GOLANA",
    .page_title = {"OSC 1", "OSC 2"},
    .edit = {
        {"WAVE", F_ENUM, 0, 4, 0, N_GO1WAVE, 0},
        {"CTRL1", F_INT, 0, 127, 0, 0, 0},
        {"DEPTH", F_INT, 0, 127, 0, 0, 0},
        {"RATE", F_LFOHZ, 0, 127, 60, 0, 0},
        {"WAVE2", F_ENUM, 0, 3, 0, N_GO2WAVE, 0},
        {"MOD", F_ENUM, 0, 3, 0, N_GO2MOD, 0},
        {"SEMI", F_SEMI, -24, 24, 0, 0, 0},
        {"TUNE", F_INT, -63, 63, 0, 0, "ct"},
    },
    .presets = GOLANA_PRESETS,
    .npresets = NELEM(GOLANA_PRESETS),
    .note_on = gol_note_on,
    .render = gol_render,
    .block = gol_block,
    .tdesc = gol_tdesc,
    .macro = {P_E1, GL_MIX2, GL_FOLD, P_E7},
    .poly = GOL_POLY,
    .fil_page = -1,
    .loaded = gol_track_loaded,
};
