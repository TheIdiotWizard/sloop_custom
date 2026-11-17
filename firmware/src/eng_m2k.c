/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 * Portions Copyright (C) 2026 Sequencist (InigoHernandez), MONO2K: https://github.com/InigoHernandez/mono2k */
/* SAW, PULS, ENS and WAV: MONO2K's SWAV-SAW, SWAV-PUL, SWAV-ENS and DIGI-WAV machines
 * (MONO2K firmware/src/m2k_dsp.c mach_block, GPL-3.0-only), each voice of a part running its own copy. The machines'
 * sample loops, laws and tables are MONO2K's (m2k_tables.h, extracted unchanged); what changed for Melodee:
 *  - a machine is a voice: Melodee's ADSR, velocity, glide, unison and LFO play it (MONO2K's own track chain, its
 *    filters and amp, is not part of it); its pitch is the voice's (tuning, bend, fine detune), TUNE and the
 *    intervals a ratio on top;
 *  - its 16-sample blocks run two to Melodee's 32 (their per-block laws: MONO2K's own);
 *  - ENS's chorus is the part's (eng_m2k_post: one line over all its voices, CHRW across SPREAD's two sides);
 *  - WAV plays the owner's 64 waves (MONO2K's WAVE bank 64; in the resource region, res.c: silent without it), its
 *    sync master a free phase per voice;
 *  - ENS's intervals are a list (an index each: the names MONO2K shows);
 *  - MONO2K's chain filter is not ported: the FILTER page (TRIO's filter, eng_uflt.c) takes its place, OFF by default;
 *  - FLT (Melodee's LFO / ENV to the filter) and SHP move one control each, named in the table below;
 *  - FM+STAT, FM+PAR and FM+DYN (engines 24..26) were retired in 1.2.6 for Mini-Tone's FMTONE (eng_mt.c), GND-NOIS
 *    (NOIS, 28) in 1.2.8 for NOISE (eng_noise.c); PULS (SWAV-PUL, 1.3.6) took STAT's number, 24 (a STAT sound of
 *    1.2.0..1.2.5 plays as PULS);
 *  - PULS's PWRS OFF: each voice's sweep runs on over its notes (MONO2K's track: one sweep).
 * The 32 x 32 -> 64 multiplies sit in out-of-line helpers on the device (M2K_MUL_OUT, as MONO2K's FM-1 build). */
/* sloop_custom: Melodee Custom's eng_m2k.c (https://github.com/TheIdiotWizard/melodee_custom), its machines unchanged,
 * without Melodee's FILTER page and its DRIVE (the track's FX > FILTER after the part instead): no uflt, FLT always moves
 * its control. For SLOOP: the voices' state in the part's arena (engines.c eng_arena_t; Melodee's resource_get), the
 * waves in the app image (m2k_tables.h; no resource region), no SPREAD (ENS's chorus: both taps in the middle), the
 * bipolar controls (TUNE, SCAN, DTUN) stored -64..63 (SLOOP has no F_OFS: Melodee's 0..127 less 64), a retriggered
 * WAV voice told by its level (Melodee's voice_was). Presets: SAW Melodee Custom 1.2.0's (before any filter), PULS,
 * ENS, WAV as Melodee Custom ships them. */
#include "m2k_tables.h"

#define M2K_POLY 6u                  /* voices a part (POLY, UNISON) */
#define M2K_ENS_POLY 3u              /* ENS: up to four saw pairs a voice */
#define M2K_SUB 16u                  /* MONO2K's control block */
#define M2K_ENS_LEN 1024u            /* ENS's chorus line: 46 ms at half rate */
#define M2K_ENS_GAIN 7119            /* a voice against the others' levels (MONO2K: calibrated, chain_ens20) */
#define M2K_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
enum { M2K_SAW, M2K_ENS, M2K_PUL, M2K_PAR, M2K_DYN, M2K_WAV };    /* (engine - ENGI_SAW; PAR DYN retired, 1.2.6; PULS in
                                                                  * STAT's place, 1.3.6) */
#define M2K_KINDS 6

#if !defined(__x86_64__) && !defined(__i386__) && !defined(__aarch64__) && !defined(__wasm__)   /* the FM-1 */
#define M2K_MUL_FN static __attribute__((noinline))
#else
#define M2K_MUL_FN static inline
#endif
M2K_MUL_FN int64_t m2k_mul_sh(int32_t a, int32_t b, int sh) { return ((int64_t)a * b) >> sh; }
M2K_MUL_FN uint32_t m2k_mulu_hi(uint32_t a, uint32_t b, int sh) { return (uint32_t)(((uint64_t)a * b) >> sh); }
M2K_MUL_FN int64_t m2k_mul64_sh(int64_t a, int32_t b, int sh) { return (a * b) >> sh; }
M2K_MUL_FN uint64_t m2k_mul2_q15(int32_t s, int32_t a, int32_t b)
{
    return (uint64_t)(uint32_t)(((int64_t)s * b) >> 15) << 32 | (uint32_t)(((int64_t)s * a) >> 15);
}
static inline int32_t m2k_sat16(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }
static inline int32_t m2k_sinp(uint32_t ph)
{
    uint32_t i = ph >> 22, f = (ph >> 6) & 0xFFFFu;
    int32_t a = T_SIN[i], b = T_SIN[i + 1];
    return a + (((b - a) * (int32_t)f) >> 16);
}
static inline uint32_t m2k_noise(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x; }
static inline int32_t m2k_blep_saw(uint32_t ph, uint32_t inc, uint32_t inv)
{
    int32_t s = (int32_t)(ph >> 16) - 32768;
    if (ph < inc) {
        uint32_t t = (ph * inv) >> 16;
        int32_t p = 2 * (int32_t)t - (int32_t)((t * t) >> 16) - 65536;
        s -= p >> 1;
    } else if (~ph < inc) {
        uint32_t u = (~ph * inv) >> 16, w = 65536 - u;
        s -= (int32_t)((w * w) >> 16) >> 1;
    }
    return s;
}
static uint32_t m2k_tq_mid(const uint32_t *T, int32_t q8)
{
    uint32_t a = T[q8 >> 8], b = T[(q8 >> 8) + 1], f = (uint32_t)q8 & 255u;
    return b >= a ? a + m2k_mulu_hi(b - a, f, 8) : a - m2k_mulu_hi(a - b, f, 8);
}
static inline uint32_t m2k_tq32(const uint32_t *T, int32_t q8) { return q8 & 255 ? m2k_tq_mid(T, q8) : T[q8 >> 8]; }
static inline int32_t m2k_tq16(const uint16_t *T, int32_t q8)
{
    int32_t a = T[q8 >> 8], f = q8 & 255;
    return f ? a + ((((int32_t)T[(q8 >> 8) + 1] - a) * f) >> 8) : a;
}
/* 2^(y / 256), Q16 */
static inline uint32_t m2k_exp2(int32_t y)
{
    int32_t i = y >> 8;
    uint32_t f = T_EXP2F[y & 255];
    if (i >= 15) return 0x7FFFFFFFu;
    if (i < -16) return 0;
    return i >= 0 ? f << i : f >> -i;
}
/* the voice's increment moved by o 1/64 semitones (TUNE, an interval, the drift) */
static inline uint32_t m2k_inc(uint32_t inc, int32_t o) { return o ? m2k_mulu_hi(inc, m2k_exp2(o / 3), 16) : inc; }
/* a voice's machine state, as MONO2K's track's */
typedef struct {
    uint32_t ph[7];
    int32_t ms[6];
    uint32_t menv[1];                /* Q30 envelope from the note (WAV's) */
    uint32_t drng;                   /* SAW's drift */
    int32_t dcx, dcy;                /* sloop_custom: PULS's DC blocker (its last input, its output Q8) */
} m2k_voice_t;
typedef struct {                     /* ENS: the part's chorus line, after its voices */
    int32_t line[M2K_ENS_LEN];
    uint32_t w, lph;                 /* the write index, the chorus's sine */
    int32_t acc;                     /* a sample pair's sum (half rate) */
} m2k_chorus_t;
typedef struct {
    m2k_voice_t v[M2K_POLY];
    m2k_chorus_t ch;                 /* (eng_state_size: ENS's chorus, the others none) */
} m2k_part_t;
/* the machine of a track's engine (core.h ENGI_SAW .. ENGI_WAV) */
static int m2k_kind(const track_t *t)
{
    switch (t->engine) {
    case ENGI_PULS: return M2K_PUL;
    case ENGI_ENS: return M2K_ENS;
    case ENGI_WAV: return M2K_WAV;
    default: return M2K_SAW;
    }
}
static m2k_part_t *m2k_part_of(const track_t *t) { return (m2k_part_t *)eng_arena_of(t, t->engine); }

/* the controls with their modulation, Q8 (0 .. 127 x 256); FLT moves control flt, SHP control shp */
/* the controls stored -64..63 (MONO2K's 64 the middle), per machine: bit k = EDIT k (TUNE; WAV's SCAN and DTUN) */
static const uint8_t M2K_BIP[M2K_KINDS] = {[M2K_SAW] = 0x80, [M2K_ENS] = 0x80, [M2K_PUL] = 0x80, [M2K_WAV] = 0x22};
static void m2k_params(const track_t *t, const vmod_t *m, int32_t *q, int flt, int shp)
{
    uint32_t bip = M2K_BIP[m2k_kind(t)];
    for (uint32_t k = 0; k < 8u; k++) q[k] = ((int32_t)t->p[P_E0 + k] + ((bip >> k) & 1u ? 64 : 0)) * 256;
    if (flt >= 0) q[flt] += m->cutoff;                 /* (no FILTER page: the LFO / ENV -> FLT move this control) */
    if (shp >= 0) q[shp] += m->shape - (64 << 8);
    for (uint32_t k = 0; k < 8u; k++) q[k] = M2K_CLAMP(q[k], 0, 127 * 256);
}
#define Q7(k) (q[k] >> 8)
#define QB(k) (q[k] - 64 * 256)

/* SWAV-SAW: UNIL UNIW UNIX DRFT SUBX SUB1 SUB2 TUNE (MONO2K's note: its saw against GND-SIN's sine at 0.158) */
static void m2k_saw(m2k_voice_t *s, const int32_t *q, uint32_t base, int32_t note, int16_t *o, uint32_t seed)
{
    int32_t unil = q[0], uniw = q[1], drift = q[3];
    int32_t off = QB(7) >> 7;
    if (drift) {
        if (!s->drng) s->drng = 0x6C8E9CF5u ^ (seed + 1u) * 0x9E3779B9u;
        s->ms[4] += (((int32_t)(m2k_noise(&s->drng) >> 20) - 2048) * drift) >> 17;
        s->ms[4] = M2K_CLAMP(s->ms[4], -(drift >> 5), drift >> 5);
        off += s->ms[4] >> 6;
    }
    uint32_t inc[5], inv[5];
    inc[0] = m2k_inc(base, off);
    inc[1] = m2k_mulu_hi(inc[0], m2k_tq32(T_SW1LO, uniw), 24);
    inc[2] = m2k_mulu_hi(inc[0], m2k_tq32(T_SW1HI, uniw), 24);
    inc[3] = m2k_mulu_hi(inc[0], m2k_tq32(T_SW2LO, uniw), 24);
    inc[4] = m2k_mulu_hi(inc[0], m2k_tq32(T_SW2HI, uniw), 24);
    int nv = unil ? 5 : 1;
    for (int k = 0; k < nv; k++) inv[k] = 0xFFFFFFFFu / (inc[k] | 1);
    int32_t g1 = 5166 * unil / (127 * 256), g2 = (g1 * q[2]) / (127 * 256);
    int32_t gx = (3369 * m2k_tq16(T_SWSUBX, q[4])) >> 15;
    int32_t gs1 = 7097 * q[5] / (127 * 256), gs2 = 2701 * q[6] / (127 * 256);
    uint32_t ih = inc[0] >> 1, invh = 0xFFFFFFFFu / (ih | 1);
    int32_t a = T_SWHP[M2K_CLAMP(note + (off >> 6), 0, 127)];
    for (uint32_t i = 0; i < M2K_SUB; i++) {
        int32_t x = m2k_blep_saw(s->ph[0], inc[0], inv[0]) * 5166;
        if (unil) {
            x += (m2k_blep_saw(s->ph[1], inc[1], inv[1]) + m2k_blep_saw(s->ph[2], inc[2], inv[2])) * g1;
            if (g2) x += (m2k_blep_saw(s->ph[3], inc[3], inv[3]) + m2k_blep_saw(s->ph[4], inc[4], inv[4])) * g2;
        }
        x >>= 15;
        if (gx) x += ((m2k_blep_saw(s->ph[5], ih, invh) - m2k_blep_saw(s->ph[5] + 0x80000000u, ih, invh)) * gx) >> 15;
        for (int k = 0; k < nv; k++) s->ph[k] += inc[k];
        s->ms[0] = (int32_t)m2k_mul_sh(a, s->ms[0] + x - s->ms[1], 15);
        s->ms[1] = x;
        int32_t y = s->ms[0];
        if (gs1) y += (m2k_sinp(s->ph[5]) * gs1) >> 15;
        if (gs2) y += (m2k_sinp(s->ph[6]) * gs2) >> 15;
        s->ph[5] += ih;
        s->ph[6] += inc[0] >> 2;
        o[i] = (int16_t)m2k_sat16(y);
    }
}

/* SWAV-PUL: UNIL UNIW SUB1 SUB2 PW PWAD PWRS TUNE (MONO2K's: a pulse, its voices started together, its duty PW / 128,
 * a square at 64, silent at 0 and 127, against GND-SIN's sine at 0.105; PWAD sweeps PW on, 5.38 steps a second a step,
 * back and forth between 4 % and 96 %; a unison pair -+0.211 c a step of UNIW off, each as loud as UNIL says; sines an
 * octave and two down) */
#ifndef PULS_GAIN
#define PULS_GAIN 5800         /* (Q12: its presets near SAW's and ENS's) */
#endif
#define M2K_PUL_A 3430
#define M2K_PUL_S1 32767
#define M2K_PUL_S2 31500
static __attribute__((noinline)) void m2k_pul(m2k_voice_t *s, const int32_t *q, uint32_t base, int16_t *o)
{
    int32_t unil = q[0], uniw = q[1];
    uint32_t inc[3], inv[3];
    inc[0] = m2k_inc(base, QB(7) >> 7);
    inc[1] = m2k_mulu_hi(inc[0], m2k_tq32(T_SW2LO, uniw), 24);     /* SWAV-SAW's extended pair's spread */
    inc[2] = m2k_mulu_hi(inc[0], m2k_tq32(T_SW2HI, uniw), 24);
    int nv = unil ? 3 : 1;
    for (int k = 0; k < nv; k++) inv[k] = 0xFFFFFFFFu / (inc[k] | 1);
    s->ms[4] = (int32_t)(((uint32_t)s->ms[4] + (uint32_t)((q[5] * 128) >> 8)) & 0x7FFFFFu);   /* the sweep, Q16 */
    uint32_t duty = (uint32_t)(((q[4] << 8) + s->ms[4]) & 0x7FFFFF) << 9;
    if (q[5] >> 8) {                 /* swept: back and forth between 4 % and 96 % (no dropout where it wraps) */
        uint32_t x = (uint32_t)(((q[4] << 8) + s->ms[4]) & 0x7FFFFF);
        uint32_t tri = x < 0x400000u ? x << 1 : (0x800000u - x) << 1;
        duty = 0x0A3D70A4u + m2k_mulu_hi(tri, 0xEB851EB8u, 23);
    }
    int32_t g1 = M2K_PUL_A * unil / (127 * 256);
    int32_t gs1 = M2K_PUL_S1 * q[2] / (127 * 256), gs2 = M2K_PUL_S2 * q[3] / (127 * 256);
    int32_t dc = 32768 - (int32_t)(duty >> 16);
    for (uint32_t i = 0; i < M2K_SUB; i++) {
        int32_t x = (m2k_blep_saw(s->ph[0], inc[0], inv[0]) - m2k_blep_saw(s->ph[0] - duty, inc[0], inv[0]) + dc) * M2K_PUL_A;
        if (unil)
            for (int k = 1; k < 3; k++)
                x += (m2k_blep_saw(s->ph[k], inc[k], inv[k]) - m2k_blep_saw(s->ph[k] - duty, inc[k], inv[k]) + dc) * g1;
        x >>= 15;
        for (int k = 0; k < nv; k++) s->ph[k] += inc[k];
        if (gs1) x += (m2k_sinp(s->ph[5]) * gs1) >> 15;
        if (gs2) x += (m2k_sinp(s->ph[6]) * gs2) >> 15;
        s->ph[5] += inc[0] >> 1;
        s->ph[6] += inc[0] >> 2;
        o[i] = (int16_t)m2k_sat16(x);
    }
}

/* SWAV-ENS's voices: PCH2 PCH3 PCH4 WAVE PW CHRL CHRW TUNE (the chorus: m2k_post). The root and up to three voices
 * off its intervals, each two saws an octave under its note, the second PW / 128 of a period on */
static void m2k_ens(m2k_voice_t *s, const int32_t *q, const int16_t *p, uint32_t base, int16_t *o)
{
    int32_t root = (QB(7) >> 7) - 12 * 64;
    int32_t w8 = q[3];
    int32_t wa = 32768 - (w8 * 12124) / (127 * 256), wb = 32768 - (w8 * 53412) / (127 * 256);
    uint32_t off = (uint32_t)q[4] << 17;
    uint32_t inc[4], inv[4];
    for (int k = 0; k < 4; k++) {
        int32_t iv = 0;
        if (k) {                     /* PCH: one of T_ENS_PNAME's 33 (16 OFF) */
            uint32_t x = (uint32_t)M2K_CLAMP(p[P_E0 + k - 1], 0, 32), j = 0;
            while (j < 127u && T_ENS_PIDX[j] < x) j++;
            iv = T_ENS_PCH[j];
        }
        inc[k] = iv == ENS_OFF ? 0 : m2k_inc(base, root + iv);
        inv[k] = 0xFFFFFFFFu / (inc[k] | 1);
    }
    for (uint32_t i = 0; i < M2K_SUB; i++) {
        int32_t x = 0;
        for (int k = 0; k < 4; k++) {
            if (!inc[k]) continue;
            x += (m2k_blep_saw(s->ph[k], inc[k], inv[k]) * wa + m2k_blep_saw(s->ph[k] + off, inc[k], inv[k]) * wb) >> 15;
            s->ph[k] += inc[k];
        }
        o[i] = (int16_t)m2k_sat16((x * M2K_ENS_GAIN) >> 15);
    }
}

/* DIGI-WAV on the owner's 64: WAVE SCAN STIM SYNC BITS DTUN MIX2 STEP */
static void m2k_wav(m2k_voice_t *s, const int32_t *q, uint32_t base, int16_t *o)
{
    int32_t env = (int32_t)(s->menv[0] >> 15);
    if (q[2] < 127 * 256) s->menv[0] = m2k_mulu_hi(s->menv[0], m2k_tq32(T_ENV_COEF, q[2]), 31);
    int32_t scan = (QB(1) * 2 * env) >> 15;
    int32_t pos = (q[0] >> 1) + scan * 63 / 128;
    pos = M2K_CLAMP(pos, 0, 63 * 256);
    int ia = pos >> 8, ib = M2K_CLAMP(ia + 1, 0, 63);
    const int16_t *wa = &T_WAVES64[ia * 256], *wb = &T_WAVES64[ib * 256];
    int32_t dca = T_WAVES64_DC[ia], dcb = T_WAVES64_DC[ib], ga = T_WAVES64_G[ia], gb = T_WAVES64_G[ib];
    int32_t dc = dca + (((dcb - dca) * (pos & 255)) >> 8), g = ga + (((gb - ga) * (pos & 255)) >> 8);
    int32_t wf = pos & 255;
    uint32_t inc = base;
    uint32_t sinc = m2k_mulu_hi(inc, 256 + (uint32_t)((q[3] * 14) >> 8), 8);
    uint32_t inc2 = m2k_inc(base, QB(5) >> 5);
    int bits = 16 - (Q7(4) * 14 >> 7);
    int32_t mix2 = m2k_tq16(T_VOL, q[6]);
    int stp = Q7(7) * 6 >> 7;
    uint32_t qmask = stp ? ~((1u << (24 + stp)) - 1) : 0xFFFFFFFFu;
    for (uint32_t i = 0; i < M2K_SUB; i++) {
        uint32_t pm = s->ph[2];
        s->ph[2] += inc;
        if (s->ph[2] < pm) s->ph[0] = 0;
        uint32_t ph = s->ph[0] & qmask;
        int32_t j = (int32_t)(ph >> 24), f = stp ? 0 : (int32_t)((ph >> 16) & 255);
        int32_t a = wa[j] + (((wa[(j + 1) & 255] - wa[j]) * f) >> 8);
        int32_t b = wb[j] + (((wb[(j + 1) & 255] - wb[j]) * f) >> 8);
        int32_t x = a + (((b - a) * wf) >> 8);
        x = ((x - dc) * g) >> 14;
        if (mix2) {
            uint32_t q2 = s->ph[1] & qmask;
            int32_t j2 = (int32_t)(q2 >> 24);
            x += ((((wa[j2] - dca) * ga) >> 14) * mix2) >> 15;
            x = (x * 3) >> 2;
        }
        s->ph[0] += sinc;
        s->ph[1] += inc2;
        if (bits < 16) x = (x >> (16 - bits)) * (1 << (16 - bits));
        o[i] = (int16_t)m2k_sat16(x);
    }
}

/* each machine's level into the voice (Q12: a voice's peak about 0.9 of full scale, no clip) and the
 * controls FLT and SHP move */
static const struct { int16_t gain; int8_t flt, shp; } M2K_ROLE[M2K_KINDS] = {
    [M2K_SAW] = {10240, 0, 1},       /* FLT UNIL, SHP UNIW */
    [M2K_ENS] = {6144, 3, 4},        /* FLT WAVE, SHP PW */
    [M2K_PUL] = {PULS_GAIN, 0, 4},   /* FLT UNIL, SHP PW */
    [M2K_WAV] = {4500, 1, 0},        /* FLT SCAN, SHP WAVE */
};


static m2k_voice_t *m2k_voice_of(track_t *t, voice_t *v)
{
    uint32_t i = (uint32_t)(v - t->v);
    if (t < &trk[0] || t >= &trk[NPART] || i >= M2K_POLY) return 0;
    return &m2k_part_of(t)->v[i];
}

static void m2k_note_on(track_t *t, voice_t *v)
{
    m2k_voice_t *s = m2k_voice_of(t, v);
    int kind = m2k_kind(t);
    if (!s) return;
    uint32_t keep = s->ph[2], drng = s->drng;
    int32_t sweep = s->ms[4], was = v->env || v->env_out;   /* (voice.c voice_start: a sounding voice keeps its level) */
    memset(s, 0, sizeof *s);
    s->menv[0] = 1u << 30;
    if (kind == M2K_WAV && was) s->ph[2] = keep;    /* a retrigger or steal: the sync's master runs on */
    s->drng = drng;
    if (kind == M2K_PUL && !t->p[P_E6]) s->ms[4] = sweep;     /* PWRS OFF: the sweep runs on */
    if (kind == M2K_ENS) {                       /* the first note after silence: the chorus starts empty */
        for (uint32_t k = 0; k < NVOICE; k++) if (&t->v[k] != v && t->v[k].active) return;
        memset(&m2k_part_of(t)->ch, 0, sizeof(m2k_chorus_t));
    }
}

static void m2k_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    m2k_voice_t *s = m2k_voice_of(t, v);
    int kind = m2k_kind(t);
    int32_t q[8];
    int16_t o[M2K_SUB];
    if (!s) return;
    m2k_params(t, m, q, M2K_ROLE[kind].flt, M2K_ROLE[kind].shp);
    int32_t gain = M2K_ROLE[kind].gain, note = m->pitch16 >> 4;
    for (uint32_t b = 0; b < n; b += M2K_SUB) {
        switch (kind) {
        case M2K_SAW: m2k_saw(s, q, m->inc, note, o, (uint32_t)(v - t->v) + (uint32_t)(t - trk) * 16u); break;
        case M2K_ENS: m2k_ens(s, q, t->p, m->inc, o); break;
        case M2K_PUL: m2k_pul(s, q, m->inc, o); break;
        default: m2k_wav(s, q, m->inc, o); break;
        }
        for (uint32_t i = 0; i < M2K_SUB && b + i < n; i++) {
            int32_t x = m2k_sat16(((int32_t)o[i] * gain) >> 12);
            if (kind == M2K_PUL) {   /* sloop_custom: a swept pulse has a mean, which Melodee's FILTER M2K took out (its
                                      * high-pass); here a DC blocker at ~15 Hz (FMTONE's) */
                s->dcy = mulq16(s->dcy, 65396u) + ((x - s->dcx) << 8);
                s->dcx = x;
                x = (s->dcy + 128) >> 8;
            }
            out[b + i] += voice_amp(x, m, b + i);
        }
    }
}

/* ENS's chorus over the part (MONO2K's: two taps off a half-rate line, each delayed by a sine at 0.2 Hz swinging
 * +-16.8 c, the second a quarter turn on; CHRL their mix, dry 1 - 0.83 x, wet 0.85 x). With SPREAD its CHRW sets them
 * apart across the two sides as MONO2K's; SLOOP has no SPREAD: they meet in the middle) */
static void m2k_ens_post(track_t *t, int32_t *out, uint32_t n, uint32_t nr)
{
    if (t < &trk[0] || t >= &trk[NPART]) return;
    m2k_chorus_t *c = &m2k_part_of(t)->ch;
    /* A silent part (nr 0) comes here too, with a buffer the mixer did not clear (its scratch: other audio). The
     * line takes silence then and the buffer is left alone; taking that scratch into the line played it back,
     * delayed, as a burst at the first note after a release had ended. */
    if (!nr) {
        for (uint32_t i = 0; i < n; i++) {
            if (i & 1u) { c->line[c->w++ & (M2K_ENS_LEN - 1u)] = c->acc; c->acc = 0; }
            c->lph += ENS_CINC;
        }
        return;
    }
    int32_t chrl = (int32_t)t->p[P_E5] * 256, chrw = (int32_t)t->p[P_E6] * 256;
    int32_t dry = 32768 - (chrl * 27197) / (127 * 256), wet = (chrl * 27853) / (127 * 256);
    wet = (wet * m2k_tq16(T_ENS_G, chrw)) >> 15;
    for (uint32_t i = 0; i < n; i++) {
        int32_t x = out[i];
        c->acc += x >> 1;
        if (i & 1u) { c->line[c->w++ & (M2K_ENS_LEN - 1u)] = c->acc; c->acc = 0; }
        c->lph += ENS_CINC;
        if (!chrl) continue;
        int32_t y[2];
        for (uint32_t k = 0; k < 2u; k++) {
            int32_t d8 = (2 << 8) + ENS_CDEP + ((ENS_CDEP * m2k_sinp(c->lph + (k << 30))) >> 15);
            uint32_t pos = (c->w << 8) + ((i & 1u) << 7) - (uint32_t)d8;
            uint32_t j = pos >> 8;
            int32_t f = (int32_t)(pos & 255u);
            int32_t a = c->line[j & (M2K_ENS_LEN - 1u)], b = c->line[(j + 1u) & (M2K_ENS_LEN - 1u)];
            y[k] = a + (int32_t)m2k_mul_sh(b - a, f, 8);
        }
        int32_t mid = (int32_t)m2k_mul_sh((y[0] + y[1]) >> 1, wet, 15);
        out[i] = (int32_t)m2k_mul_sh(x, dry, 15) + mid;   /* (SLOOP has no SPREAD: the taps meet in the middle) */
    }
}

/* the controls' lists */
static const char *const N_M2K_PCH[34] = {"-12", "-11", "-10", "-9", "-8", "-3:2", "-7", "-6", "-5", "-4:3", "-4", "-5:4",
    "-6:5", "-3", "-2", "-1", "OFF", "+1", "+2", "+3", "+6:5", "+5:4", "+4", "+4:3", "+5", "+6", "+7", "+3:2", "+8", "+9",
    "+10", "+11", "+12", 0};
static const char *const N_M2K_ONOFF[3] = {"OFF", "ON", 0};
#define M2K_P(l) {l, F_INT, 0, 127, 0, 0, 0}
#define M2K_PD(l, d) {l, F_INT, 0, 127, d, 0, 0}
#define M2K_BI(l) {l, F_INT, -64, 63, 0, 0, 0}     /* (MONO2K's bipolar, its 64 the middle: m2k_params M2K_BIP) */
#define M2K_PCH(l) {l, F_ENUM, 0, 32, 16, N_M2K_PCH, 0}

/* presets: MONO2K's controls at their values (a ratio its step of 24, an interval its place of 33; the bipolar ones less
 * 64), the ADSR Melodee's. SAW: Melodee Custom 1.2.0's, made before any FILTER; PULS, ENS, WAV: as Melodee Custom ships
 * them (PULS's are MONO2K's library sounds, which there run through FILTER M2K: here they play without it) */
static const preset_t M2K_SAW_PRESETS[] = {
    {"SUPERSAW", {100, 40, 64, 20, 0, 40, 0, 0}, {0, 64, 110, 50}, 0, 0},
    {"BIG PAD", {127, 70, 100, 40, 0, 60, 20, 0}, {60, 80, 120, 80}, 0, 0},
    {"MONO SAW", {0, 0, 0, 0, 60, 80, 0, 0}, {0, 50, 100, 30}, 0, 1},
    {"DRIFT LD", {60, 20, 30, 90, 0, 0, 0, 0}, {2, 60, 110, 40}, 0, 1},
    {"SUB BASS", {0, 0, 0, 10, 100, 127, 60, 0}, {0, 40, 90, 20}, 0, 1},
};
static const preset_t M2K_PUL_PRESETS[] = {
    {"PWMBASS", {70, 40, 60, 0, 64, 40, 0, 0}, {26, 127, 126, 62}, 14, 1},
    {"SQUARE", {0, 0, 30, 0, 64, 0, 0, 0}, {5, 127, 77, 59}, -1, 1},
    {"PWMPAD", {80, 50, 20, 0, 64, 12, 0, 0}, {93, 101, 5, 99}, -2, 0},
    {"REED", {0, 0, 0, 0, 64, 0, 0, 0}, {28, 127, 127, 82}, 0, 1},
};
static const preset_t M2K_ENS_PRESETS[] = {
    {"MAJOR", {22, 26, 32, 0, 64, 80, 64, 0}, {10, 70, 110, 60}, 0, 0},
    {"MINOR", {19, 26, 32, 0, 64, 80, 64, 0}, {10, 70, 110, 60}, 0, 0},
    {"FIFTHS", {26, 32, 16, 40, 64, 60, 90, 0}, {0, 64, 110, 50}, 0, 0},
    {"OCT PULS", {32, 16, 16, 127, 32, 50, 40, 0}, {0, 60, 100, 40}, 0, 1},
    {"STRINGS", {26, 32, 22, 20, 90, 110, 110, 0}, {70, 80, 120, 90}, 0, 0},
};
static const preset_t M2K_WAV_PRESETS[] = {
    {"WAVE PAD", {26, 16, 60, 0, 0, 0, 0, 0}, {50, 70, 110, 70}, 0, 0},
    {"SCAN LEAD", {60, 46, 40, 0, 0, 6, 40, 0}, {0, 60, 100, 40}, 0, 1},
    {"SYNC LEAD", {10, 0, 127, 70, 0, 0, 0, 0}, {0, 60, 100, 40}, 0, 1},
    {"LOFI KEYS", {90, 0, 127, 0, 70, 0, 0, 40}, {0, 70, 40, 50}, 0, 0},
    {"DIGI BASS", {120, -24, 50, 20, 0, 0, 60, 0}, {0, 50, 70, 20}, 0, 1},
};

static const engine_t ENG_M2K_SAW = {
    .name = "SAW",
    .page_title = {"UNISON", "SUBS"},
    .edit = {M2K_PD("UNIL", 100), M2K_PD("UNIW", 40), M2K_PD("UNIX", 64), M2K_P("DRFT"), M2K_P("SUBX"), M2K_P("SUB1"),
             M2K_P("SUB2"), M2K_BI("TUNE")},
    .presets = M2K_SAW_PRESETS, .npresets = NELEM(M2K_SAW_PRESETS),
    .macro = {P_E0, P_E1, P_E5, P_REL},
    .poly = M2K_POLY,
    .note_on = m2k_note_on, .render = m2k_render, .fil_page = -1,
};
static const engine_t ENG_M2K_PULS = {
    .name = "PULS",
    .page_title = {"UNISON", "PULSE"},
    .edit = {M2K_P("UNIL"), M2K_PD("UNIW", 40), M2K_P("SUB1"), M2K_P("SUB2"), M2K_PD("PW", 64), M2K_P("PWAD"),
             {"PWRS", F_ENUM, 0, 1, 0, N_M2K_ONOFF, 0}, M2K_BI("TUNE")},
    .presets = M2K_PUL_PRESETS, .npresets = NELEM(M2K_PUL_PRESETS),
    .macro = {P_E4, P_E5, P_E0, P_REL},
    .poly = M2K_POLY,
    .note_on = m2k_note_on, .render = m2k_render, .fil_page = -1,
};
static const engine_t ENG_M2K_ENS = {
    .name = "ENS",
    .page_title = {"CHORD", "CHORUS"},
    .edit = {M2K_PCH("PCH2"), M2K_PCH("PCH3"), M2K_PCH("PCH4"), M2K_P("WAVE"), M2K_PD("PW", 64), M2K_PD("CHRL", 80),
             M2K_PD("CHRW", 64), M2K_BI("TUNE")},
    .presets = M2K_ENS_PRESETS, .npresets = NELEM(M2K_ENS_PRESETS),
    .macro = {P_E3, P_E4, P_E5, P_REL},
    .poly = M2K_ENS_POLY,
    .note_on = m2k_note_on, .render = m2k_render, .fil_page = -1, .post = m2k_ens_post,
};
static const engine_t ENG_M2K_WAV = {
    .name = "WAV",
    .page_title = {"WAVE", "SHAPE"},
    .edit = {{"WAVE", F_INT, 0, 127, 26, 0, 0}, M2K_BI("SCAN"), M2K_PD("STIM", 60), M2K_P("SYNC"), M2K_P("BITS"),
             M2K_BI("DTUN"), M2K_P("MIX2"), M2K_P("STEP")},
    .presets = M2K_WAV_PRESETS, .npresets = NELEM(M2K_WAV_PRESETS),
    .macro = {P_E0, P_E1, P_E3, P_REL},
    .poly = M2K_POLY,
    .note_on = m2k_note_on, .render = m2k_render, .fil_page = -1,
};
