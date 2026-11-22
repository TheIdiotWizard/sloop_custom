/* SPDX-License-Identifier: GPL-3.0-only */
/* Headroom probe: every factory preset plays one note (the loudest of C2, C3, C4) and a six-note chord at velocity 127 (a mono / legato sound plays the chord in POLY), and the peak of the mix as it enters the master limiter is printed
 * in dBFS (32768 = 0 dBFS; the limiter's threshold is -5.2), and the loudest 23 ms of it (RMS). The lines: engine preset name
 * peak-1-note peak-chord rms-1-note rms-chord (dB).
 *   cc -O2 -DPROBE_PRELIM -Ibuild/gen -Ifirmware/src -Ifirmware/hal tests/chord_probe.c -lm -o build/host/chord_probe
 *   build/host/chord_probe [ENGINE] */
#define main hostsim_main
#include "hostsim.c"
#undef main

static double db(int32_t pk) { return pk > 0 ? 20.0 * log10(pk / 32768.0) : -99.0; }

static double probe_rms;                                 /* the loudest 1024 samples (23 ms) of the run, dBFS RMS */
static int32_t run(uint32_t e, uint32_t pi, const uint8_t *notes, uint32_t nn)
{
    double ring[32], win = 0, best = 0;
    uint32_t k = 0;
    int32_t o[2 * CTL];
    uint32_t b, i;
    host_tracks_init();
    song.g[G_BPM] = 100;
    host_preset(&trk[0], e, pi);
    if (nn > 1u && (trk[0].p[P_VOICE] == V_MONO || trk[0].p[P_VOICE] == V_LEGATO))
        trk[0].p[P_VOICE] = V_POLY;                        /* (a mono sound can be played as a chord: VOICE POLY) */
    song.sel = 0;
    for (b = 0; b < FS / CTL; b++)
        mix_block(o, CTL);
    probe_pk = 0;
    for (i = 0; i < nn; i++)
        trk_note_on(&trk[0], notes[i], 127);
    memset(ring, 0, sizeof ring);
    for (b = 0; b < (FS * 3u / 2u) / CTL; b++) {
        probe_ss = 0;
        mix_block(o, CTL);
        win += probe_ss - ring[k % 32u], ring[k++ % 32u] = probe_ss;
        if (b >= 32u && win > best)
            best = win;
    }
    probe_rms = best > 0 ? 10.0 * log10(best / (32.0 * CTL) / (32768.0 * 32768.0)) : -99.0;
    return probe_pk;
}

int main(int argc, char **argv)
{
    static const uint8_t ONE[3] = {48, 60, 72}, CH6[] = {48, 55, 60, 64, 67, 72};
    uint32_t e, pi;
    for (e = 0; e < NENGINES; e++) {
        if (argc > 1 && (uint32_t)atoi(argv[1]) != e)
            continue;
        for (pi = 0; pi < ENGINES[e]->npresets; pi++) {
            const char *n = ENGINES[e]->presets[pi].name;
            if (!strcmp(n, "GM KIT"))
                continue;
            int32_t one = 0, k, pk;
            double r1 = -99, r6;
            for (k = 0; k < 3; k++) {                           /* (how loud a note is depends on its pitch: filters, FM) */
                pk = run(e, pi, ONE + k, 1), one = pk > one ? pk : one;
                r1 = probe_rms > r1 ? probe_rms : r1;
            }
            pk = run(e, pi, CH6, 6), r6 = probe_rms;
            printf("%u %u %-12s %6.1f %6.1f %6.1f %6.1f\n", e, pi, n, db(one), db(pk), r1, r6);
        }
    }
    return 0;
}
