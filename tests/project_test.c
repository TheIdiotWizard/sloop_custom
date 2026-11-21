/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of the project format (firmware/src/project.c, -DPROJ_HOST part). sloop_custom writes and reads format 7
 * ("FUN7": every parameter before P_E0 in a signed byte, the 20 operator slots of ANALOG4 and FMTONE among them, P_E0..
 * P_E7 in 16 bits); SLOOP's own formats are not read (a clean slate: their engine numbers are other engines here), nor
 * is a damaged image. The working project round trip, sections, the globals a project keeps, and an image's values
 * brought back inside their ranges. Run by tests/run_tests.sh (needs build/gen). */
#define main hostsim_main
#include <stddef.h>
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i)       /* ui.c TRK_DEF: ANALOG4, PAR, SAW */
{
    static const uint8_t E[NPART] = {ENGI_ANA4, ENGI_PAR, ENGI_SAW};
    return i < NPART ? E[i] : 0u;
}
#include "../firmware/src/project.c"

static int check(const char *what, int ok)
{
    printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(void)
{
    static project_t q, q2;
    static union {
        project_t v7;
        uint8_t raw[3840];
    } buf;
    uint32_t i, t, e;
    int bad = 0, ok;

    bad += check("layout: the ten of 2.6, then the 20 operator slots, RES, KEY and DIST just before P_E0 (86), P_COUNT 94",
                 P_VLEAD + 1 == P_ITYPE && P_LPOL + 1 == P_FM1_ATK && P_FM4_LEVEL + 1 == P_TRES && P_TRES + 1 == P_TKEY && P_TKEY + 1 == P_TDIST && P_TDIST + 1 == P_E0 && P_E0 == 86 &&
                 P_COUNT == 94 && P_SLDEPTH + 1 == P_CHORD);
    printf("  format 7: %u bytes\n", (unsigned)sizeof(project_t));
    bad += check("format 7 fits one flash object (3840 bytes); 4 slots fit .noinit (with panel, settings, dbg, bootguard)",
                 sizeof(project_t) <= 3840u && 4u * sizeof(project_t) <= 0x3D50u - 256u);
    {   /* every parameter kept in a byte holds every value of its range: TP's, and each engine's operator slots */
        uint32_t k, fits = 1;
        for (k = 0; k < P_E0; k++) {
            fits &= TP[k].min >= -128 && TP[k].max <= 127;
            for (e = 0; e < NENGINES; e++)
                fits &= eng_pdesc(e, k)->min >= -128 && eng_pdesc(e, k)->max <= 127;
        }
        bad += check("format 7: every parameter before P_E0 fits a signed byte (TP's and every engine's slots)", fits);
    }
    {   /* the operator slots go with the project: ANALOG4's on track 1, FMTONE's (its FINE, -64..63) on track 2 */
        host_tracks_init();
        trk[0].eng_req = trk[0].engine = ENGI_ANA4, trk[1].eng_req = trk[1].engine = ENGI_DYN;
        trk[0].p[P_FM3_LEVEL] = 37, trk[0].p[P_FM4_DEC] = 5, trk[0].p[P_FM2_REL] = 4, trk[0].p[P_FTYPE] = 2;
        trk[1].p[P_FM3_REL] = -64, trk[1].p[P_FM4_ATK] = 63, trk[1].p[P_FM4_DEC] = -7, trk[1].p[P_FM1_LEVEL] = 99;
        trk[1].p[P_E4] = -50;                      /* (FMTONE HARM) */
        lock_set(&trk[1], 3, P_FM2_LEVEL, 12);     /* a lock on an operator slot */
        proj_capture(&q);
        host_tracks_init();
        proj_apply(&q, 1);
        ok = trk[0].engine == ENGI_ANA4 && trk[1].engine == ENGI_DYN && trk[0].p[P_FM3_LEVEL] == 37 &&
             trk[0].p[P_FM4_DEC] == 5 && trk[0].p[P_FM2_REL] == 4 && trk[0].p[P_FTYPE] == 2 && trk[1].p[P_FM3_REL] == -64 &&
             trk[1].p[P_FM4_ATK] == 63 && trk[1].p[P_FM4_DEC] == -7 && trk[1].p[P_FM1_LEVEL] == 99 && trk[1].p[P_E4] == -50 &&
             lock_find(&trk[1], 3, P_FM2_LEVEL, 0) >= 0 && trk[1].lock[lock_find(&trk[1], 3, P_FM2_LEVEL, 0)].val == 12;
        bad += check("operator slots: ANALOG4's and FMTONE's (FINE -64 .. 63) and a lock on one, capture -> apply", ok);
    }
    {   /* format 7 as stored: read back; damaged, cut short, or another format's magic: no project */
        host_tracks_init();
        proj_capture(&q);
        memcpy(&buf, &q, sizeof q);
        ok = proj_import(&q2, &buf, (int)sizeof q) && !memcmp(&q2, &q, sizeof q);
        bad += check("FUN7 -> FUN7: as stored", ok);
        buf.v7.t[2].p[P_LEVEL] ^= 1;
        bad += check("FUN7 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof q));
        memcpy(&buf, &q, sizeof q);
        bad += check("FUN7 cut short: refused", !proj_import(&q2, &buf, (int)sizeof q - 2));
        buf.v7.magic = 0x46554E36u;                /* "FUN6": SLOOP 2.6 .. 2.8's, with a checksum that holds */
        buf.v7.sum = proj_hash(&buf, sizeof q - 4u);
        bad += check("SLOOP's FUN6 (a valid checksum): not read (a clean slate)", !proj_import(&q2, &buf, (int)sizeof q));
        memset(&buf, 0, sizeof buf);
        buf.v7.magic = 0x46554E36u, buf.v7.size = 3816u;
        bad += check("SLOOP's FUN6, its own 3816 bytes: not read", !proj_import(&q2, &buf, 3816));
    }
    /* capture / apply: the working project round trip (nudges and locks too) */
    host_tracks_init();
    for (t = 0; t < NTRK; t++)
        trk[t].p[P_SLEN] = (int16_t)(5 + t);
    trk[1].step[2].n = 2, trk[1].step[2].note[0] = 60, trk[1].step[2].note[1] = 64, trk[1].step[2].time = ST_NOTE;
    trk[1].step[2].lvl = 0x0D;
    dstep_set(&TDRUM->dstep[9], 4, LV_SOFT, 1);
    song.g[G_DUST] = 33;
    song.g[G_DRDLY] = 77;
    trk[1].micro[2] = -20;
    TDRUM->micro[9] = 12;
    lock_set(&trk[1], 2, P_ED_FLT, -30);
    lock_set(&trk[1], 2, P_E1, 5);
    lock_set(TDRUM, 9, P_DIST, 64);
    step_fill_set(&trk[1], 2, FC_FILL);
    step_fill_set(TDRUM, 9, FC_NOFILL);
    step_fill_set(TDRUM, 63, FC_FILL);
    trk[2].p[P_ITYPE] = 7, trk[2].p[P_IA] = 11, trk[2].p[P_IMIX] = 77, TDRUM->p[P_ITYPE] = 4;   /* 2.6 */
    trk[0].p[P_LSYNC] = 6, trk[0].p[P_LTRIG] = 1, trk[0].p[P_LPOL] = 1, trk[0].p[P_ESYNC] = 1, trk[0].p[P_FTYPE] = 1;
    proj_capture(&q);
    host_tracks_init();
    song.g[G_DRDLY] = 0;
    proj_apply(&q, 1);
    ok = q.drdly == 77 && song.g[G_DRDLY] == 77 && trk[2].p[P_SLEN] == 7 && trk[1].step[2].n == 2 && trk[1].step[2].lvl == 0x0D && song.g[G_DUST] == 33 &&
         dstep_has(&TDRUM->dstep[9], 4) && dstep_lvl(&TDRUM->dstep[9], 4) == LV_SOFT && dstep_rat(&TDRUM->dstep[9], 4) == 1u &&
         trk[1].micro[2] == -20 && TDRUM->micro[9] == 12 && trk[1].micro[3] == 0 &&
         lock_find(&trk[1], 2, P_ED_FLT, 0) >= 0 && trk[1].lock[lock_find(&trk[1], 2, P_ED_FLT, 0)].val == -30 &&
         lock_find(&trk[1], 2, P_E1, 0) >= 0 && lock_find(TDRUM, 9, P_DIST, 0) >= 0 && lock_find(TDRUM, 9, P_E0, 0) < 0 &&
         step_fill(&trk[1], 2) == FC_FILL && step_fill(&trk[1], 3) == FC_NORM && step_fill(TDRUM, 9) == FC_NOFILL &&
         step_fill(TDRUM, 63) == FC_FILL && step_fill(TDRUM, 8) == FC_NORM &&
         trk[2].p[P_ITYPE] == 7 && trk[2].p[P_IA] == 11 && trk[2].p[P_IMIX] == 77 && TDRUM->p[P_ITYPE] == 4 &&
         trk[0].p[P_LSYNC] == 6 && trk[0].p[P_LTRIG] == 1 && trk[0].p[P_LPOL] == 1 && trk[0].p[P_ESYNC] == 1 &&
         trk[0].p[P_FTYPE] == 1;
    bad += check("the working project: capture -> apply round trip (levels, lanes, DUST, drum DLY, nudges, locks, fill conditions, the 2.6 parameters)", ok);
    q2 = q;
    q2.drdly = 0;                                  /* a 2.4 project: the byte was 0 */
    proj_apply(&q2, 0);                            /* (as a song section) */
    ok = song.g[G_DRDLY] == 0;
    q2.drdly = 200;
    proj_apply(&q2, 1);
    ok &= song.g[G_DRDLY] == 127;
    bad += check("2.5 drum DLY: kept in the byte after sel, a 2.4 project: 0, a section sets it, 200 -> 127",
                 ok && G_DRDLY == PROJ_NG && offsetof(project_t, drdly) == 8u + 2u * PROJ_NG + 1u);
    {   /* 2.6: GLO > TEXTURE in the project (and in a section); a project before it: OFF; the drum channel (a setting of
         * the FM-1 now, GLO > MIDI) not taken from a project */
        song.g[G_TXSRC] = 5, song.g[G_TXLVL] = 101, song.g[G_TXTONE] = -37, song.g[G_TXMOVE] = 90;
        song.g[G_TXRATE] = 3, song.g[G_TXWHEN] = 1, song.g[G_TXDUCK] = 1, song.g[G_DRCH] = 4;
        proj_capture(&q2);
        song.g[G_TXSRC] = 0, song.g[G_TXLVL] = 0, song.g[G_TXTONE] = 0, song.g[G_TXMOVE] = 0;
        song.g[G_TXRATE] = 0, song.g[G_TXWHEN] = 0, song.g[G_TXDUCK] = 0;
        proj_apply(&q2, 1);
        ok = song.g[G_TXSRC] == 5 && song.g[G_TXLVL] == 101 && song.g[G_TXTONE] == -37 && song.g[G_TXMOVE] == 90 &&
             song.g[G_TXRATE] == 3 && song.g[G_TXWHEN] == 1 && song.g[G_TXDUCK] == 1 && song.g[G_DRCH] == 4;
        song.g[G_DRCH] = 9;
        song.g[G_TXSRC] = 0;
        proj_apply(&q2, 0);                        /* a section: its texture too */
        ok &= song.g[G_TXSRC] == 5 && song.g[G_DRCH] == 9;
        q2.rsv[0] = 0;                             /* a project before 2.6 (converted): no texture */
        proj_apply(&q2, 1);
        ok &= song.g[G_TXSRC] == 0 && song.g[G_TXLVL] == GP[G_TXLVL].def && song.g[G_TXMOVE] == GP[G_TXMOVE].def && song.g[G_DRCH] == 9;
        bad += check("2.6 TEXTURE: kept in a project (SRC LEVEL TONE MOVE RATE WHEN DUCK), a section sets it, an older one: OFF; "
                     "the drum channel not taken from a project", ok);
    }
    {   /* 2.7: KBD HOLD (ARP HOLD with the arp OFF) sounds now: a project saved before 2.7 (no 0x40 in rsv[0]) opens
         * with HOLD OFF where the arp is OFF, so no note hangs; one saved by 2.7 keeps it; the arp ON: kept either way */
        trk[0].p[P_AMODE] = 0, trk[0].p[P_AHOLD] = 1;
        trk[1].p[P_AMODE] = 1, trk[1].p[P_AHOLD] = 1;
        proj_capture(&q2);
        ok = (q2.rsv[0] & 0x40u) != 0u;
        trk[0].p[P_AHOLD] = 0, trk[1].p[P_AHOLD] = 0;
        proj_apply(&q2, 1);
        ok &= trk[0].p[P_AHOLD] == 1 && trk[1].p[P_AHOLD] == 1;
        q2.rsv[0] &= (uint8_t)~0x40u;
        proj_apply(&q2, 1);
        ok &= trk[0].p[P_AHOLD] == 0 && trk[1].p[P_AHOLD] == 1;
        trk[0].p[P_AHOLD] = 0, trk[1].p[P_AMODE] = 0, trk[1].p[P_AHOLD] = 0;
        bad += check("2.7 KBD HOLD: saved by 2.7 (rsv 0x40) kept; an older project: HOLD OFF where the arp is OFF", ok);
    }
    {   /* 2.7: the DRUMS screen's SOUND page, each sound's TUNE and DECAY: kept in a project and a section (the drum
         * track's free E1..E7, a check in G_SAVE); a project of an older SLOOP (no check): every sound as the kit has it */
        uint32_t l;
        for (l = 0; l < DRUM_LANES; l++)
            drum_tune[l] = (int8_t)((int32_t)(l * 5u % 16u) - 8), drum_dec[l] = (int8_t)((int32_t)(l * 3u % 8u) - 4);
        proj_capture(&q2);
        memset(drum_tune, 0, sizeof drum_tune), memset(drum_dec, 0, sizeof drum_dec);
        proj_apply(&q2, 0);                        /* (a section) */
        ok = 1;
        for (l = 0; l < DRUM_LANES; l++)
            ok &= drum_tune[l] == (int8_t)((int32_t)(l * 5u % 16u) - 8) && drum_dec[l] == (int8_t)((int32_t)(l * 3u % 8u) - 4);
        q2.g[G_SAVE] = 0;                           /* (as an older SLOOP saves it) */
        proj_apply(&q2, 1);
        for (l = 0; l < DRUM_LANES; l++)
            ok &= !drum_tune[l] && !drum_dec[l];
        drum_tune[3] = 7, drum_dec[9] = -4;
        proj_capture(&q2);
        q2.t[TRK_DRUM].e[2] ^= 0x10;                 /* (a value of an older SLOOP's engine there: the check fails) */
        proj_apply(&q2, 1);
        ok &= !drum_tune[3] && !drum_dec[9];
        memset(drum_tune, 0, sizeof drum_tune), memset(drum_dec, 0, sizeof drum_dec);
        bad += check("2.7 drum SOUND page: TUNE and DECAY of the 16 sounds kept in a project and a section; an older one: none", ok);
    }
    {   /* what format 7 packs in fewer bits holds every value: TEXTURE's SRC (4 bits), WHEN and DUCK (1), RATE (a byte),
         * LEVEL and TONE + 64 (7 bits each); (the parameters' bytes: above) */
        int ok = 1;
        ok &= GP[G_TXSRC].min >= 0 && GP[G_TXSRC].max <= 15 && GP[G_TXWHEN].min >= 0 && GP[G_TXWHEN].max <= 1 &&
              GP[G_TXDUCK].min >= 0 && GP[G_TXDUCK].max <= 1 && GP[G_TXRATE].min >= 0 && GP[G_TXRATE].max <= 255 &&
              GP[G_TXLVL].min >= 0 && GP[G_TXLVL].max <= 127 && GP[G_TXTONE].min >= -64 && GP[G_TXTONE].max <= 63;
        bad += check("format 7: TEXTURE's packed fields hold every value of their range", ok);
    }
    /* a damaged image: a nudge out of range, a lock on a parameter that cannot lock, on a step past the end,
     * with a value past the range: clamped, freed, freed, clamped */
    q.t[1].lock[5].step = 4, q.t[1].lock[5].param = P_SLEN, q.t[1].lock[5].val = 8;
    q.t[1].lock[6].step = 64, q.t[1].lock[6].param = P_E0, q.t[1].lock[6].val = 1;
    q.t[1].lock[7].step = 4, q.t[1].lock[7].param = P_LEVEL, q.t[1].lock[7].val = 999;
    q.t[1].lock[8].step = 4, q.t[1].lock[8].param = 200, q.t[1].lock[8].val = 1;
    pt_set_mf(&q.t[1], 4, 0, FC_FILL), pt_set_mf(&q.t[1], 5, 0, FC_NOFILL);   /* steps 5, 6: fill only, no fill; */
    pt_set_mf(&q.t[1], 6, 0, 3), pt_set_mf(&q.t[1], 7, 0, 3);                    /* 7, 8: 3 (-> normal) */
    pt_set(&q.t[1], P_ITYPE, 200);                 /* an INSERT type past the list: clamped */
    proj_apply(&q, 1);
    ok = trk[1].p[P_ITYPE] == TP[P_ITYPE].max && trk[1].lock[5].step == LOCK_FREE &&
         trk[1].lock[6].step == LOCK_FREE && trk[1].lock[8].step == LOCK_FREE && trk[1].lock[7].step == 4 &&
         trk[1].lock[7].val == 127 && lock_find(&trk[1], 2, P_ED_FLT, 0) >= 0 &&
         step_fill(&trk[1], 4) == FC_FILL && step_fill(&trk[1], 5) == FC_NOFILL && trk[1].fill[1] == 0x09;
    bad += check("apply: INSERT 200 -> the last type, a lock on LEN / step 64 / param 200 is freed, LEVEL 999 -> 127, condition 3 -> normal", ok);

    q2 = q;
    bad += check("2.6 SAVE > PROJECT > ERASE: an object of no bytes is no project (an empty slot, older SLOOP too)",
                 !proj_import(&q2, &q, 0));
    printf("%s\n", bad ? "PROJECT FORMAT TEST FAILED" : "project format test passed");
    return bad != 0;
}
