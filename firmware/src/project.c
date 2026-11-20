/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Projects: four slots in .noinit RAM (the song sections A..D), so they survive resets and UBOOT
 * entry. With FELUCCA_FLASH every save also goes to flash through storage.c, and an empty RAM slot
 * is filled from flash on load. The working project is also kept in flash by itself (autosave, when
 * the transport is stopped and nothing sounds) and comes back at power-on: SLOOP starts where you
 * left it.
 *
 * Format: 7 ("FUN7", sloop_custom): every parameter before P_E0 in a signed byte (each fits -128..127: P_LEVEL ..
 * P_LPOL, then the 20 operator slots of ANALOG4 and FMTONE), P_E0 .. P_E7 in 16 bits (the drum track keeps its
 * sounds' TUNE and DECAY packed there: drum_tw_put), the engine and preset, the steps, each step's nudge (6 bits) and
 * fill condition (2 bits) in a byte, the parameter locks: 3688 bytes, one flash object. sloop_custom is a clean slate:
 * SLOOP's own formats (FUN1 .. FUN6) are not read, their engine numbers are other engines here; such a project loads
 * as an empty one.
 *
 * Built on the host too (tests/project_test.c, -DPROJ_HOST): the part above the #ifndef
 * PROJ_HOST needs core.h, params.c (TP), drums.c (the lanes), the engines and trk_def_engine (ui.c). */
#ifndef UI_SMALL                 /* (2.7) compiled for size: not the audio path (gfx.c UI_SMALL); the host tests: nothing */
#define UI_SMALL
#endif
#define PROJ_MAGIC 0x46554E37u                 /* "FUN7": sloop_custom (the parameters in bytes, the operator slots) */
#define PROJ_NG 32u                            /* the globals a project holds (G_COUNT until 2.4). SLOOP 2.5's
                                                * G_DRDLY rides in the byte after sel, so the format and its size do
                                                * not change */
_Static_assert(G_DRDLY == PROJ_NG && G_TXSRC == PROJ_NG + 5u,
               "the globals a project holds, then G_DRDLY (then 2.6's GLO > MIDI: settings of the FM-1)");
_Static_assert(G_FXFADE == G_CLKOUT + 1u && G_COUNT == G_FXFADE + 1u, "G_FXFADE (2.7.2) is the last global, a setting of the FM-1");
typedef struct {                               /* one track; the drum track ignores engine / preset */
    int8_t p[P_TRES];                          /* P_LEVEL .. P_FM4_LEVEL, a byte each (pt_get, pt_set) */
    uint8_t tkey;                              /* sloop_custom: P_TKEY (low nibble) and P_TDIST (high), 0..10 each: the byte that padded p[] to the e[] alignment */
    int16_t e[8];                              /* P_E0 .. P_E7 */
    uint8_t engine, preset;                    /* engine: its low nibble (NENGINES <= 16); sloop_custom: P_TRES in the high one */
    union {
        step_t step[NSTEP];
        dstep_t dstep[NSTEP];                  /* (the drum track: 16 lanes, the same size) */
    };
    uint8_t mf[NSTEP];                         /* each step's nudge (core.h MICRO_*, 6 bits) | its fill condition << 6 */
    plock_t lock[NLOCK];                       /* and its parameter locks (step LOCK_FREE = none) */
} proj_trk_t;
typedef struct {
    uint32_t magic, size;
    int16_t g[PROJ_NG];
    uint8_t sel, drdly, rsv[2];                /* the selected track; 2.5: G_DRDLY */
    proj_trk_t t[NTRK];
    uint32_t sum;
} project_t;
static int16_t pt_get(const proj_trk_t *t, uint32_t id)   /* p[id] of a stored track */
{
    return id < P_TRES ? (int16_t)t->p[id] : id == P_TRES ? (int16_t)(t->engine >> 4) : id == P_TKEY ? (int16_t)(t->tkey & 15u) : id == P_TDIST ? (int16_t)(t->tkey >> 4) : t->e[(id - P_E0) & 7u];
}
UI_SMALL static void pt_set(proj_trk_t *t, uint32_t id, int32_t v)
{
    if (id < P_TRES)
        t->p[id] = (int8_t)clamp(v, -128, 127);
    else if (id == P_TRES)
        t->engine = (uint8_t)((t->engine & 15u) | (uint32_t)clamp(v, 0, 15) << 4);
    else if (id == P_TKEY)
        t->tkey = (uint8_t)((t->tkey & 0xF0u) | (uint32_t)clamp(v, 0, 10));
    else if (id == P_TDIST)
        t->tkey = (uint8_t)((t->tkey & 15u) | (uint32_t)clamp(v, 0, 10) << 4);
    else
        t->e[(id - P_E0) & 7u] = (int16_t)v;
}
static int32_t pt_micro(const proj_trk_t *t, uint32_t i) { int32_t m = t->mf[i] & 63; return m >= 32 ? m - 64 : m; }
static uint32_t pt_fill(const proj_trk_t *t, uint32_t i) { return (uint32_t)t->mf[i] >> 6; }
UI_SMALL static void pt_set_mf(proj_trk_t *t, uint32_t i, int32_t micro, uint32_t fill)
{
    t->mf[i] = (uint8_t)(((uint32_t)micro & 63u) | (fill & 3u) << 6);
}
_Static_assert(sizeof(project_t) <= 3840u, "format 7 fits one flash object (storage.c ST_PAYLOAD_MAX)");
project_t proj_slot[4] __attribute__((section(".noinit")));

UI_SMALL static uint32_t proj_hash(const void *p, uint32_t n)   /* FNV-1a over n bytes */
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t i, s = 0x811C9DC5u;
    for (i = 0; i < n; i++)
        s = (s ^ b[i]) * 16777619u;
    return s;
}
UI_SMALL static uint32_t proj_sum(const project_t *p) { return proj_hash(p, sizeof *p - 4u); }
UI_SMALL static int proj_ok(const project_t *q) { return q->magic == PROJ_MAGIC && q->size == sizeof *q && q->sum == proj_sum(q); }

/* n bytes of a stored project -> slot q; 0 = not a project of this format (sloop_custom: SLOOP's own are not read) */
UI_SMALL static int proj_import(project_t *q, const void *b, int n)
{
    if (n == (int)sizeof *q && proj_ok((const project_t *)b)) {
        memcpy(q, b, sizeof *q);
        return 1;
    }
    return 0;
}

/* ---- the working project <-> a project_t */
/* 2.6: GLO > TEXTURE in a project (a song section has its own): in the slots of two globals a project no longer
 * applies (G_DRCH: a setting of the FM-1 now; G_INFO: the CPU readout) and the spare bytes. rsv[0]: 0x80 (there) | SRC
 * | WHEN << 4 | DUCK << 5, rsv[1]: RATE, g[G_DRCH]: LEVEL | (TONE + 64) << 7, g[G_INFO]: MOVE. Without the 0x80 (a
 * project before it, converted): the defaults, OFF */
UI_SMALL static void proj_tx_put(project_t *p)
{
    p->rsv[0] = (uint8_t)(0x80u | ((uint32_t)song.g[G_TXSRC] & 15u) | ((uint32_t)song.g[G_TXWHEN] & 1u) << 4 |
                          ((uint32_t)song.g[G_TXDUCK] & 1u) << 5 | 0x40u);   /* (0x40: saved by 2.7 or later) */
    p->rsv[1] = (uint8_t)song.g[G_TXRATE];
    p->g[G_DRCH] = (int16_t)((song.g[G_TXLVL] & 127) | ((song.g[G_TXTONE] + 64) & 127) << 7);
    p->g[G_INFO] = song.g[G_TXMOVE];
}
static void proj_tx_get(const project_t *p)
{
    static const uint8_t ID[7] = {G_TXSRC, G_TXLVL, G_TXTONE, G_TXMOVE, G_TXRATE, G_TXWHEN, G_TXDUCK};
    int32_t v[7];
    uint32_t i;
    if (!(p->rsv[0] & 0x80u)) {
        for (i = 0; i < 7u; i++)
            v[i] = GP[ID[i]].def;
    } else {
        v[0] = p->rsv[0] & 15u, v[5] = (p->rsv[0] >> 4) & 1u, v[6] = (p->rsv[0] >> 5) & 1u, v[4] = p->rsv[1];
        v[1] = p->g[G_DRCH] & 127, v[2] = ((p->g[G_DRCH] >> 7) & 127) - 64, v[3] = p->g[G_INFO];
    }
    for (i = 0; i < 7u; i++)
        song.g[ID[i]] = (int16_t)clamp(v[i], GP[ID[i]].min, GP[ID[i]].max);
}

/* (2.7) the DRUMS screen's SOUND page in a project: each sound's TUNE (4 bits) and DECAY (3 bits), two's complement
 * (0: as the kit has it), 16 x 7 bits in the drum track's engine values E1..E7 (the drum track has no engine: they
 * were free), and a check of them in g[G_SAVE] (never loaded: SAVE is a GO button). An older SLOOP keeps neither
 * (it limits E1..E7 to an engine's ranges and saves G_SAVE as 0): its project opens with every sound as the kit has it */
#define DRUM_TW_MARK 0x5400
UI_SMALL static uint32_t drum_tw_sum(const int16_t *e)
{
    uint32_t i, s = 0x5Au;
    for (i = 1; i < 8u; i++)
        s += ((uint32_t)(uint16_t)e[i] & 0xFFu) * (i * 2u + 1u) + ((uint32_t)(uint16_t)e[i] >> 8) * (i * 2u + 2u);
    return s & 0xFFu;
}
UI_SMALL static void drum_tw_put(project_t *p)
{
    uint16_t w[7] = {0};
    uint32_t l, b;
    for (l = 0; l < DRUM_LANES; l++) {
        uint32_t v = ((uint32_t)drum_tune[l] & 15u) | ((uint32_t)drum_dec[l] & 7u) << 4;
        for (b = 0; b < 7u; b++)
            if ((v >> b) & 1u)
                w[(l * 7u + b) / 16u] |= (uint16_t)(1u << ((l * 7u + b) % 16u));
    }
    for (l = 0; l < 7u; l++)
        p->t[TRK_DRUM].e[l + 1u] = (int16_t)w[l];
    p->g[G_SAVE] = (int16_t)(DRUM_TW_MARK | drum_tw_sum(p->t[TRK_DRUM].e));
}
static void drum_tw_get(const project_t *p)
{
    const int16_t *e = p->t[TRK_DRUM].e;
    uint32_t l, b, ok = (uint16_t)p->g[G_SAVE] == (DRUM_TW_MARK | drum_tw_sum(e));
    for (l = 0; l < DRUM_LANES; l++) {
        uint32_t v = 0;
        for (b = 0; ok && b < 7u; b++)
            if (((uint16_t)e[1u + (l * 7u + b) / 16u] >> ((l * 7u + b) % 16u)) & 1u)
                v |= 1u << b;
        drum_tune[l] = (int8_t)((int32_t)((v & 15u) ^ 8u) - 8);
        drum_dec[l] = (int8_t)((int32_t)(((v >> 4) & 7u) ^ 4u) - 4);
    }
}

UI_SMALL static void proj_capture(project_t *p)        /* what is playing now, as a project */
{
    uint32_t i;
    memset(p, 0, sizeof *p);
    p->magic = PROJ_MAGIC;
    p->size = sizeof *p;
    for (i = 0; i < PROJ_NG; i++)
        p->g[i] = song.g[i];
    p->sel = song.sel;
    p->drdly = (uint8_t)song.g[G_DRDLY];
    proj_tx_put(p);
    for (i = 0; i < NTRK; i++) {
        uint32_t k;
        p->t[i].engine = trk[i].eng_req;               /* (before the values: P_TRES goes in its high nibble) */
        for (k = 0; k < P_COUNT; k++)
            pt_set(&p->t[i], k, trk[i].p[k]);
        p->t[i].preset = trk[i].preset;
        memcpy(p->t[i].step, trk[i].step, sizeof trk[i].step);
        for (k = 0; k < NSTEP; k++)
            pt_set_mf(&p->t[i], k, trk[i].micro[k], (uint32_t)trk[i].fill[k / 4u] >> (2u * (k % 4u)));
        memcpy(p->t[i].lock, trk[i].lock, sizeof trk[i].lock);
    }
    drum_tw_put(p);                                     /* (2.7) the drum sounds' TUNE and DECAY */
    p->sum = proj_sum(p);
}

/* a project's tracks (and its globals, all: a load; or only the drum level / reverb / delay: a song
 * section) into the working one, every value back inside its range. The audio ISR must not run
 * meanwhile (the song sections: called from it; a load: IRQ off) */
static void proj_apply(const project_t *p, int all)
{
    uint32_t i, k;
    for (i = 0; i < PROJ_NG; i++)
        if (all ? i != G_SLOT && i != G_LOAD && i != G_SAVE && i != G_SYNC && i != G_MIDI && i != G_ROUTE && i != G_DRCH &&
                      i != G_INFO
                : i == G_DRLVL || i == G_DRREV)                 /* (2.6: the drum channel a setting of the FM-1, GLO > MIDI;
                                                                 * its slot and G_INFO's hold the TEXTURE) */
            song.g[i] = (int16_t)clamp(p->g[i], GP[i].min, GP[i].max);
    song.g[G_DRDLY] = (int16_t)clamp(p->drdly, GP[G_DRDLY].min, GP[G_DRDLY].max);   /* (a load and a section, as REV) */
    proj_tx_get(p);                                     /* GLO > TEXTURE (2.6): a load and a section */
    drum_tw_get(p);                                     /* (2.7) the drum sounds' TUNE and DECAY: a load and a section */
    for (k = 0; k < NTRK; k++) {
        track_t *t = &trk[k];
        const proj_trk_t *s = &p->t[k];
        uint32_t e = k < NPART ? (s->engine & 15u) % NENGINES : 0u;
        t->eng_req = (uint8_t)e;
        t->user = 0;                                    /* (no user preset slot is saved) */
        t->lk_n = 0;                                    /* (the locks in force: the values come from the project) */
        for (i = 0; i < P_COUNT; i++) {                 /* every value back inside its range */
            const param_desc_t *d = k == TRK_DRUM && i == P_E0 ? &DRUM_KIT_DESC :   /* the drum kit */
                                    eng_pdesc(e, i);
            t->p[i] = (int16_t)clamp(pt_get(s, i), d->min, d->max);
        }
        if (!(p->rsv[0] & 0x40u) && !t->p[P_AMODE])
            t->p[P_AHOLD] = 0;                          /* (2.7) saved before 2.7: HOLD with the arp OFF did nothing then; it
                                                         * is KBD HOLD now, so it starts OFF (no note held by surprise) */
        t->preset = (uint8_t)(ENGINES[e]->npresets ? (s->preset == 0xFFu ? 0u : s->preset) % ENGINES[e]->npresets : 0u);
        memcpy(t->step, s->step, sizeof t->step);
        memset(t->fill, 0, sizeof t->fill);
        for (i = 0; i < NSTEP; i++) {                   /* the nudges and conditions (3 means normal: 0) */
            uint32_t c = pt_fill(s, i);
            t->micro[i] = (int8_t)clamp(pt_micro(s, i), MICRO_MIN, MICRO_MAX);
            if (c != 3u)
                t->fill[i / 4u] |= (uint8_t)(c << (2u * (i % 4u)));
        }
        for (i = 0; i < NLOCK; i++) {
            const plock_t *l = &s->lock[i];
            uint32_t id = l->param;
            if (l->step < NSTEP && id < P_COUNT && p_lockable(id)) {
                const param_desc_t *d = k == TRK_DRUM && id == P_E0 ? &DRUM_KIT_DESC :
                                        eng_pdesc(e, id);
                t->lock[i].step = l->step;
                t->lock[i].param = (uint8_t)id;
                t->lock[i].val = (int16_t)clamp(l->val, d->min, d->max);
            } else {
                t->lock[i].step = LOCK_FREE;            /* no such step or parameter: the slot is free */
                t->lock[i].param = 0;
                t->lock[i].val = 0;
            }
        }
        if (k != TRK_DRUM)
            for (i = 0; i < NSTEP; i++) {
                step_t *st = &t->step[i];
                uint32_t j;
                if (st->n > 4u)
                    st->n = 4;
                if (st->time > ST_REST)
                    st->time = ST_REST;
                st->vel &= 127u;                    /* (after 2.8.3: past 127 from a file, up to twice as loud) */
                for (j = 0; j < 4u; j++)
                    st->note[j] &= 127u;
            }
        fm6_track_loaded(t);                            /* FM6: the project keeps PTCH, not the patch: its slot's */
    }
}

#ifndef PROJ_HOST
#if FELUCCA_ARRANGER
#include "arranger_scene.c"
#endif
static uint8_t sec_dirty, song_dirty;           /* live sections / the song: in RAM, not yet in flash */
#if FELUCCA_FLASH
/* slot from flash into RAM (format 7; anything else: an empty slot). A whole flash payload: upreset.c and fm6_bank.c
 * stage their writes here too */
static union {
    project_t v6;
    uint8_t raw[ST_PAYLOAD_MAX];
} proj_tmp;
UI_SMALL static void *up_stage(void) { return &proj_tmp; }   /* upreset.c: a user preset bank staged for a write (sloop-lab) */

/* ---- songs (sloop_custom): the sections A..D belong to one of ST_NSONG songs, song_cur. Song 0's sections are the
 * original objects, the others' are the freed sample flash (storage.c). The order (arrangement) of each song is in
 * song_arr, written to OBJ_SONGTAB when the song is left; the current song's is `arrangement`, which the settings keep
 * (its spare byte `reserved[0]` is song_cur: a settings object from before reads as song 0) */
static uint8_t song_cur;
static uint16_t song_used;                         /* the songs, the current one not counted, that hold a section */
#if FELUCCA_ARRANGER
static arr_config_t song_arr[ST_NSONG];
#endif
UI_SMALL static uint32_t sec_obj_of(uint32_t song, uint32_t slot)
{
    return song ? OBJ_SONG0 + (song - 1u) * 4u + (slot & 3u) : OBJ_PROJECT0 + (slot & 3u);
}
UI_SMALL static uint32_t sec_obj(uint32_t slot) { return sec_obj_of(song_cur, slot); }
#if FELUCCA_ARRANGER
UI_SMALL static void song_tab_load(void)               /* each song's order from OBJ_SONGTAB; one not stored or not valid: the default */
{
    uint32_t i;
    int n = st_load(OBJ_SONGTAB, &proj_tmp, sizeof proj_tmp);
    for (i = 0; i < ST_NSONG; i++) {
        arr_config_t *c = &song_arr[i];
        if (n >= (int)((i + 1u) * sizeof *c))
            memcpy(c, proj_tmp.raw + i * sizeof *c, sizeof *c);
        else
            c->count = 0;
        if (!arr_valid(c, 15u))
            arr_defaults(c);
        c->reserved[0] = c->reserved[1] = 0;
    }
}
UI_SMALL static int song_tab_save(void) { return st_save(OBJ_SONGTAB, song_arr, sizeof song_arr); }
UI_SMALL static int obj_filled(uint32_t obj)           /* a valid object of some bytes (an erased section is one of none) */
{
    st_hdr_t h;
    return st_current(obj, &h) >= 0 && h.len > 0;
}
UI_SMALL static void song_scan(void)                   /* power-on: which of the other songs hold a section */
{
    uint32_t s, i;
    song_used = 0;
    for (s = 0; s < ST_NSONG; s++)
        for (i = 0; i < 4u; i++)
            if (s != song_cur && obj_filled(sec_obj_of(s, i)))
                song_used |= (uint16_t)(1u << s);
}
#endif
UI_SMALL static void proj_fetch(uint32_t slot)
{
    project_t *q = &proj_slot[slot & 3u];
    int n = st_load(sec_obj(slot), &proj_tmp, sizeof proj_tmp);
    if (!proj_import(q, &proj_tmp, n))
        q->magic = 0;
}
#include "fm6_bank.c"                           /* the FM6 patch bank (eng_fm6.c PTCH B1..B27): staged in proj_tmp */
#endif

static const char *const SEC_NAME[4] = {"A", "B", "C", "D"};   /* (2.7: the slots named as the song's sections) */
UI_SMALL static void project_save(uint32_t slot)
{
    project_t *p = &proj_slot[slot & 3u];
#if FELUCCA_ARRANGER
    if (song.playing || transport_req) { ui_message("STOP BEFORE SAVE"); return; }
#endif
    proj_capture(p);
#if FELUCCA_FLASH
    if (flash_ok) {
        if (st_save(sec_obj(slot), p, sizeof *p))
            ui_message("SAVE ERROR");
        else
            ui_say("SAVED SECTION ", SEC_NAME[slot & 3u]);
        return;
    }
#endif
    ui_message("SAVED (RAM)");
}

/* SAVE > PROJECT > ERASE (SLOOP 2.6, after a YouTube comment): the slot (section A..D) empty again. In flash as the
 * editor's restore leaves an empty slot (project_restore): an object of no bytes, which proj_fetch (older SLOOP too)
 * reads as no project. The song's steps that played that section are taken out (the default song if none is left),
 * so song mode still plays. Stopped only: a flash erase stops the audio ~50 ms */
UI_SMALL static void project_erase(uint32_t slot)
{
    slot &= 3u;
#if FELUCCA_ARRANGER
    if (song.playing || transport_req) { ui_message("STOP BEFORE ERASE"); return; }
#endif
    if (!proj_ok(&proj_slot[slot])) {
        ui_say("EMPTY SECTION ", SEC_NAME[slot]);
        return;
    }
#if FELUCCA_FLASH
    if (flash_ok && st_save(sec_obj(slot), &proj_slot[slot], 0)) {
        ui_message("ERASE ERROR");
        return;
    }
#endif
    fm1_irq_off();
    memset(&proj_slot[slot], 0, sizeof proj_slot[slot]);
    sec_dirty &= (uint8_t)~(1u << slot);
    if (live_sec == (int8_t)slot)
        live_sec = -1;
#if FELUCCA_ARRANGER
    if (arr_drop_scene(&arrangement, slot))
        song_dirty = 1;                                     /* (saved with the settings, when quiet) */
#endif
    fm1_irq_on();
    ui_say("ERASED SECTION ", SEC_NAME[slot]);
}

/* (2.8.4) a part whose sound is on an engine this firmware does not have (a project Clipper wrote: HYBRID,
 * FMX, VECTOR...; left in the flash when SLOOP is installed after Clipper): proj_apply wrapped the engine number round
 * onto another engine with the other one's values (Clipper's HYBRID played as 808 BOOM without its TRN -24: two
 * octaves high, until another sound was chosen). The part's default sound instead; its pattern and mix stay */
UI_SMALL static void proj_foreign_parts(const project_t *p)
{
    uint32_t k;
    for (k = 0; k < NPART; k++)
        if (p->t[k].preset != 0xFFu && (p->t[k].engine & 15u) >= NENGINES) {
            set_engine_of(&trk[k], TRK_DEF[k][0]);
            apply_preset_to(&trk[k], TRK_DEF[k][1]);
        }
}

/* a project into the working one: the transport stops, everything sounding is released */
UI_SMALL static void project_apply(const project_t *p)
{
    uint32_t k;
    transport_req = 2;
    panic_req = (1u << NTRK) - 1u;
    drum_mute = 0;                                      /* (the live mutes: a performance's, not the project's) */
    for (k = 0; k < NTRK; k++)
        ex_off[k] = mw_dep[k] = 0;                                  /* (2.7.1) MIDI EXPRESSION: full again, as after CC 121 */
    fm1_irq_off();                                      /* the audio ISR must not see half a project */
    proj_apply(p, 1);
    song.sel = (uint8_t)(p->sel < NTRK ? p->sel : 0u);
    fm1_irq_on();
    for (k = 0; k < NPART; k++)                         /* a format 1 project: the default sounds of tracks 2, 3 */
        if (p->t[k].preset == 0xFFu) {
            apply_preset_to(&trk[k], TRK_DEF[k][1]);
            steps_clear(&trk[k]);
        }
    proj_foreign_parts(p);
    sync_reload = 1;
    ui.force = 1;
}

UI_SMALL static void project_load(uint32_t slot)
{
    project_t *p = &proj_slot[slot & 3u];
#if FELUCCA_ARRANGER
    if (song.playing || transport_req) { ui_message("STOP BEFORE LOAD"); return; }
#endif
#if FELUCCA_FLASH
    if (flash_ok && !proj_ok(p))
        proj_fetch(slot);
#endif
    if (!proj_ok(p)) {
        ui_say("EMPTY SECTION ", SEC_NAME[slot & 3u]);
        return;
    }
    project_apply(p);
    ui_say("LOADED SECTION ", SEC_NAME[slot & 3u]);
}

/* ---- the working project, kept in flash by itself: saved when it changed, the transport is stopped,
 * nothing sounds and the panel was not touched for AUTOSAVE_IDLE (a flash erase stops the audio for
 * ~50 ms: never while something plays); loaded at power-on (autosave_resume) */
#define AUTOSAVE_IDLE 2500u                    /* ms without input */
#define AUTOSAVE_GAP 20000u                    /* ms between two saves at least */
static project_t autosave_buf __attribute__((section(".pool")));
static uint32_t autosave_hash, autosave_ms, autosave_checked;

UI_SMALL static int audio_quiet(void)
{
    uint32_t p, i;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            if (trk[p].v[i].active)
                return 0;
    for (i = 0; i < NDRUM; i++)
        if (drums.v[i].active)
            return 0;
    return 1;
}

UI_SMALL static void autosave_tick(void)                /* main loop */
{
#if FELUCCA_FLASH
    uint32_t h, now = fm1_ms;
    if (!flash_ok || song.playing || transport_req || rec_wait || ft_on || ui.menu ||
        now - ui_input_ms < AUTOSAVE_IDLE || now - autosave_ms < AUTOSAVE_GAP || now - autosave_checked < 1000u)
        return;
    autosave_checked = now;
    proj_capture(&autosave_buf);
    h = autosave_buf.sum;
    if (h == autosave_hash || !audio_quiet())
        return;
    if (st_save(OBJ_AUTOSAVE, &autosave_buf, sizeof autosave_buf) == 0)
        autosave_hash = h;
    autosave_ms = fm1_ms;
#endif
}

static uint8_t drch_seed;                           /* a settings object as 2.5 wrote it: the drum channel was the
                                                     * project's then (autosave_resume takes it from a 2.5 autosave) */
UI_SMALL static void autosave_resume(void)              /* power-on: the project as it was left (felucca_init) */
{
#if FELUCCA_FLASH
    project_t *q = &autosave_buf;
    int n;
    if (!flash_ok)
        return;
    n = st_load(OBJ_AUTOSAVE, &proj_tmp, sizeof proj_tmp);
    if (!proj_import(q, &proj_tmp, n))
        return;
    autosave_hash = q->sum;
    if (drch_seed && !(q->rsv[0] & 0x80u) && q->g[G_DRCH] >= 0 && q->g[G_DRCH] <= 16 && q->g[G_DRCH] != midi_set[3]) {
        midi_set[3] = (uint8_t)q->g[G_DRCH];          /* (2.6) updated from 2.5: GLO > DRUMS > CH as it was there (a
                                                     * project's then, a setting of the FM-1 now), kept in the settings */
        settings_later = 1;
    }
    drch_seed = 0;
    proj_apply(q, 1);
    song.sel = (uint8_t)(q->sel < NTRK ? q->sel : 0u);
    proj_foreign_parts(q);                         /* (2.8.4) Clipper's working project: its own sounds, default */
    for (n = 0; n < NPART; n++)
        trk[n].engine = trk[n].eng_req;        /* (nothing sounds yet: no fade) */
#endif
}

/* settings + learned panel table: one flash object. The flash copy wins at
 * boot (the .noinit copies are garbage after a power-off). */
typedef struct {
    uint32_t magic, palette, lowcut, zoom;
    panel_t panel;
#if FELUCCA_ARRANGER
    arr_config_t arrangement;
#endif
    uint32_t lights;                               /* SLOOP 2.3: the backlight (panel.c lights_word); appended,
                                                    * so 2.2 still reads its part (st_load cuts at its size) */
} persist_t;
_Static_assert(sizeof(side_rec_t) == 80u && sizeof(side_rec_t) <= ST_SIDE_MAX, "the side record: theme, favourites");
static side_rec_t side_out;
#define PERSIST_SIZE_V22 __builtin_offsetof(persist_t, lights)   /* the settings as 2.2 wrote them (no lights) */
_Static_assert(sizeof(persist_t) == PERSIST_SIZE_V22 + 4u, "lights: the last word, no padding before it");
#if FELUCCA_ARRANGER
_Static_assert(__builtin_offsetof(persist_t, arrangement) == 48u && sizeof(arr_config_t) == 36u,
               "the web editor's Song page edits bytes 48..83 of the settings (web/editor.html ARR)");
#endif
#if FELUCCA_ARRANGER
#define PERSIST_MAGIC 0x50455233u                  /* "PER3": includes the song order */
#else
#define PERSIST_MAGIC 0x50455232u
#endif
#if FELUCCA_FLASH
static persist_t persist_saved;
static void dsu_boot(void);
#endif

UI_SMALL static void persist_boot(void)                    /* before settings_init / panel_init */
{
#if FELUCCA_ARRANGER
    arr_defaults(&arrangement);
#endif
#if FELUCCA_FLASH
    persist_t p;
    uint32_t f = irq_save();
    flash_ok = FL_JEDEC_OK(FL_FAR(fl_jedec_ram)());      /* a known 1 MiB part (fm1_flash_id.h), else stay RAM-only */
    irq_restore(f);
    if (!flash_ok)
        return;
    fl_plain_window_init();                        /* flash above 0x93000 reads as plaintext through XIP
                                                    * (user sample sets are played from there) */
    {
        uint32_t k;
        for (k = 0; k < SMP_USER_SLOTS; k++)
            smp_user_scan(k);
    }
    {
        int n = st_load(OBJ_SETTINGS, &p, sizeof p);
        if (n == (int)PERSIST_SIZE_V22 && p.magic == PERSIST_MAGIC)
            p.lights = 0;                          /* from 2.2: backlight off */
        if (((n == (int)sizeof p || n == (int)PERSIST_SIZE_V22) && p.magic == PERSIST_MAGIC)
#if FELUCCA_ARRANGER
            || (n == (int)(16u + sizeof(panel_t)) && p.magic == 0x50455232u)
#endif
            ) {
            settings.magic = SETTINGS_MAGIC;
            settings.palette = p.palette;
            settings.lowcut = p.lowcut;
            settings.zoom = p.zoom;
            if (p.panel.magic == PANEL_MAGIC)
                panel = p.panel;
            if (p.magic == PERSIST_MAGIC)
                lights_from_word(p.lights);
            else
                p.lights = 0;
#if FELUCCA_ARRANGER
            if (p.magic == PERSIST_MAGIC && arr_valid(&p.arrangement, 15u))
                arrangement = p.arrangement;
            else
                p.arrangement = arrangement;
#endif
#if FELUCCA_ARRANGER
            song_cur = arrangement.reserved[0] < ST_NSONG ? arrangement.reserved[0] : 0u;     /* (sloop_custom) */
            arrangement.reserved[0] = p.arrangement.reserved[0] = song_cur;
#endif
            persist_saved = p;
            {   /* 2.7: the theme and the favourites, in the side record of the same copy (gfx.c, storage.c st_side) */
                side_rec_t r;
                if (st_side_load(OBJ_SETTINGS, &r, sizeof r) == 0)
                    side_take(&r);
            }
        } else if (n == (int)(8u + sizeof(panel_t)) && p.magic == 0x50455231u) {   /* "PER1": palette, panel */
            const uint32_t *w = (const uint32_t *)&p;
            panel_t old;
            memcpy(&old, w + 2, sizeof old);
            settings.magic = SETTINGS_MAGIC;
            settings.palette = w[1];
            settings.lowcut = 0;
            settings.zoom = 0;
            if (old.magic == PANEL_MAGIC)
                panel = old;
        }
    }
#if FELUCCA_ARRANGER
    song_tab_load();                               /* sloop_custom: the songs' orders, and which songs hold something */
    song_scan();
#endif
    {   /* projects: fill empty RAM slots from flash, so the slot list is right after power-on. A slot
         * still valid in RAM (a warm reset: an update, UPDATE MODE, a crash) may never have reached
         * flash (a live section stored while playing): marked to be written when quiet */
        uint32_t i;
        for (i = 0; i < 4u; i++)
            if (!proj_ok(&proj_slot[i])) {
                proj_fetch(i);
            } else {
                int n = st_load(sec_obj(i), &proj_tmp, sizeof proj_tmp);
                if (n != (int)sizeof proj_slot[i] || memcmp(&proj_tmp.v6, &proj_slot[i], sizeof proj_slot[i]))
                    sec_dirty |= (uint8_t)(1u << i);
            }
    }
    dsu_boot();                                    /* SLOOP 2.5: the SYN kits (after the settings) */
    up_boot();                                     /* user presets */
    fm6_bank_boot();                               /* the FM6 patch bank (fm6_bank.c) */
#endif
}

UI_SMALL static int project_used(uint32_t slot) { return proj_ok(&proj_slot[slot & 3u]); }

#if FELUCCA_FLASH
/* SLOOP 2.5: the settings object holds the SYN kits after persist_t (drum_synth.c dsu): firmware before 2.5 reads
 * its persist_t and leaves the rest (st_load cuts at its size); its own next save drops them. 2.6: then the MIDI LEARN
 * map (seq.c ml_tab, ML_N entries), only while something is learned; 2.5 reads an object with the map as one without
 * its SYN kits (their defaults), one without it as its own */
/* the settings object's third part (2.6): the MIDI LEARN map (seq.c ml_tab, ML_N entries, 2 bytes each, low first),
 * then GLO > MIDI: CH1 CH2 CH3, the drum channel | OTHER << 7, GLO > MIDI 2's BEND, 3 spare. Written only while it is not
 * the default (nothing learned, channels 1 2 3 10, OTHER SEL, BEND 2): else the object is exactly 2.5's. 2.6 test
 * builds wrote 32 (the map) or 36 bytes (and the channels) */
#define MIDI_PART (2u * ML_N + 8u)
_Static_assert(sizeof(persist_t) + sizeof(dsu_bank_t) + MIDI_PART <= ST_PAYLOAD_MAX, "the settings object fits its sector");
static uint8_t midi_saved[MIDI_PART];
static uint8_t drch_seed;                           /* a settings object as 2.5 wrote it: the drum channel was the
                                                     * project's then (autosave_resume takes it from a 2.5 autosave) */               /* the part as flash has it (settings_save: written when it differs) */
UI_SMALL static void midi_part_fill(uint8_t *b)
{
    uint32_t k;
    for (k = 0; k < ML_N; k++)
        b[2u * k] = (uint8_t)ml_tab[k], b[2u * k + 1u] = (uint8_t)(ml_tab[k] >> 8);
    b[2u * ML_N] = midi_set[0], b[2u * ML_N + 1u] = midi_set[1], b[2u * ML_N + 2u] = midi_set[2];
    b[2u * ML_N + 3u] = (uint8_t)(midi_set[3] | (drum_preview != 0u) << 5 | (step_adv_off != 0u) << 6 | midi_set[4] << 7);
                                                    /* (2.7.1: bit 5 DRUM PREVIEW, bit 6 STEP ADVANCE OFF: bits 2.7 does not read) */
    b[2u * ML_N + 4u] = (uint8_t)(midi_set[5] | (fxfade > 15u ? 15u : fxfade) << 4);   /* BEND (bits 0-3); 2.7.2: FX FADE bits 4-7 (0 = none: the byte reads as 2.6's BEND) */
    b[2u * ML_N + 5u] = midi_set[6];                /* (2.7) GLO > MIDI 3: FX CH (2.6 read this spare byte as nothing) */
    b[2u * ML_N + 6u] = (uint8_t)(midi_set[7] | midi_set[9] << 1 |
                                  0u << 2);   /* (2.7) GLO > MIDI 3: DR MAP, bit 1 CLK OUT
                                                                      * (a spare byte to 2.6); 2.7.2: FILTER RESO in bits
                                                                      * 2..5 (0 = none: the byte then reads as 2.7's) */
    b[2u * ML_N + 7u] = midi_set[8];                /* (2.7) GLO > MIDI 3: REMOTE (the last spare byte) */
}
UI_SMALL static int midi_part_default(const uint8_t *b)
{
    static const uint8_t def[10] = MIDI_SET_DEF;
    uint32_t k;
    for (k = 0; k < 2u * ML_N; k++)
        if (b[k])
            return 0;
    return b[2u * ML_N] == def[0] && b[2u * ML_N + 1u] == def[1] && b[2u * ML_N + 2u] == def[2] &&
           b[2u * ML_N + 3u] == (uint8_t)(def[3] | def[4] << 7) && b[2u * ML_N + 4u] == def[5] && b[2u * ML_N + 5u] == def[6] &&
           b[2u * ML_N + 6u] == (uint8_t)(def[7] | def[9] << 1) && b[2u * ML_N + 7u] == def[8];
}
/* n: 32 (the map), 36 (and the channels) or MIDI_PART; every entry checked (a CC that learns, a parameter this knows, a
 * channel 0..16, a range 1..12) */
UI_SMALL static void midi_part_read(const uint8_t *b, uint32_t n)
{
    uint32_t k, e;
    fm1_irq_off();                                  /* (the ISR reads the map and the channels: never half of them) */
    for (k = 0; k < ML_N; k++) {
        e = (uint32_t)b[2u * k] | (uint32_t)b[2u * k + 1u] << 8;
        ml_tab[k] = (uint16_t)((e >> 9) && ml_free_cc(e & 127u) && ml_id(e >> 9) < P_COUNT ? e : 0u);
    }
    if (n >= 2u * ML_N + 4u) {
        for (k = 0; k < 3u; k++)
            midi_set[k] = (uint8_t)(b[2u * ML_N + k] <= 16u ? b[2u * ML_N + k] : k + 1u);
        midi_set[3] = (uint8_t)((b[2u * ML_N + 3u] & 31u) <= 16u ? b[2u * ML_N + 3u] & 31u : 10u);
        midi_set[4] = (uint8_t)(b[2u * ML_N + 3u] >> 7);
        drum_preview = (uint8_t)((b[2u * ML_N + 3u] >> 5) & 1u);   /* (2.7.1) menu DRUM PREVIEW; older: OFF */
        step_adv_off = (uint8_t)((b[2u * ML_N + 3u] >> 6) & 1u);   /* (2.7.1) menu STEP ADVANCE; older: ON */
        song.g[G_CH1] = midi_set[0], song.g[G_CH2] = midi_set[1], song.g[G_CH3] = midi_set[2];
        song.g[G_DRCH] = midi_set[3], song.g[G_CHOTH] = midi_set[4];
    }
    if (n >= MIDI_PART) {
        midi_set[5] = (uint8_t)((b[2u * ML_N + 4u] & 15u) >= 1u && (b[2u * ML_N + 4u] & 15u) <= 12u ? b[2u * ML_N + 4u] & 15u : 2u);
        song.g[G_BEND] = midi_set[5];
        fxfade = (uint8_t)((b[2u * ML_N + 4u] >> 4) & 15u);         /* (2.7.2) FX FADE (bits 4-7); older: 0 */
        song.g[G_FXFADE] = fxfade;
        midi_set[6] = (uint8_t)(b[2u * ML_N + 5u] <= 16u ? b[2u * ML_N + 5u] : 0u);   /* (2.7) FX CH */
        song.g[G_FXCH] = midi_set[6];
        midi_set[7] = (uint8_t)(b[2u * ML_N + 6u] & 1u);             /* (2.7) DR MAP */
        midi_set[9] = (uint8_t)((b[2u * ML_N + 6u] >> 1) & 1u);      /* (2.7) CLK OUT */
        song.g[G_DRMAP] = midi_set[7];
        song.g[G_CLKOUT] = midi_set[9];
        midi_set[8] = (uint8_t)(b[2u * ML_N + 7u] <= 16u ? b[2u * ML_N + 7u] : 0u);   /* (2.7) REMOTE */
        song.g[G_RMCH] = midi_set[8];
    }
    fm1_irq_on();
}
UI_SMALL static int settings_write(const persist_t *p)
{
    uint8_t mp[MIDI_PART];
    int rc;
    if (!dsu_valid(&dsu))
        dsu_defaults();
    midi_part_fill(mp);
    /* the third part only when it is not the default: without, the object is exactly 2.5's (2.5 reads it whole, SYN
     * kits included; with it, it takes the object for one without them). 2.7: the theme in the side record */
    side_fill(&side_out);
    st_side = &side_out;
    st_side_len = sizeof side_out;
    rc = st_save3(OBJ_SETTINGS, p, sizeof *p, &dsu, sizeof dsu, mp, midi_part_default(mp) ? 0u : sizeof mp);
    st_side = 0;
    if (rc)
        return -1;
    dsu_dirty = 0;
    fav_dirty = 0;
    memcpy(midi_saved, mp, sizeof midi_saved);
    return 0;
}
static void dsu_boot(void)                          /* the SYN kits (and the MIDI part) from the settings object, else */
{                                                   /* the defaults */
    st_hdr_t h;
    uint32_t len = sizeof(persist_t) + sizeof(dsu_bank_t), k;
    dsu_defaults();
    for (k = 0; k < ML_N; k++)
        ml_tab[k] = 0;
    if (st_current(OBJ_SETTINGS, &h) >= 0 &&
        (h.len == len || h.len == len + 2u * ML_N || h.len == len + 2u * ML_N + 4u || h.len == len + MIDI_PART) &&
        dsu_valid((const dsu_bank_t *)(st_buf + sizeof(persist_t)))) {
        memcpy(&dsu, st_buf + sizeof(persist_t), sizeof dsu);
        for (k = 0; k < DSU_N; k++)
            dsu_fix_kit(&dsu.k[k]);
        if (h.len > len)
            midi_part_read(st_buf + len, h.len - len);
        drch_seed = h.len == len;
    }
    midi_part_fill(midi_saved);
    dsu_dirty = 0;
}
#endif

UI_SMALL static void persist_fill(persist_t *p)              /* the settings as they are now */
{
    memset(p, 0, sizeof *p);
    p->magic = PERSIST_MAGIC;
    p->palette = settings.palette;
    p->lowcut = settings.lowcut;
    p->zoom = settings.zoom;
    p->panel = panel;
    p->lights = lights_word();
#if FELUCCA_ARRANGER
    p->arrangement = arrangement;
#endif
}

UI_SMALL static void settings_save(void)
{
#if FELUCCA_FLASH
    persist_t p;
    if (!flash_ok)
        return;
    persist_fill(&p);
    {
        uint8_t mp[MIDI_PART];
        midi_part_fill(mp);
        if (!memcmp(&p, &persist_saved, sizeof p) && !memcmp(mp, midi_saved, sizeof mp) && !fav_dirty)
            return;                                /* unchanged: no erase cycle */
    }
    if (settings_write(&p) == 0)
        persist_saved = p;
#endif
}

#if FELUCCA_FLASH
_Static_assert(sizeof(project_t) <= ST_PAYLOAD_MAX, "project does not fit one flash sector");

/* ---- backup restore (editor.c BK_PUT): each object checked as a load checks it, then written through the
 * same A/B commit as a save. rc: 0 ok, 2 not a valid object, 3 stop the song first, 4 flash */
UI_SMALL static int panel_valid(const panel_t *q)           /* a permutation of the buttons and of the knobs */
{
    uint32_t i, b = 0, e = 0;
    if (q->magic != PANEL_MAGIC)
        return 0;
    for (i = 0; i < NB; i++) {
        if (q->btn[i] >= 14u || (b >> q->btn[i]) & 1u)
            return 0;
        b |= 1u << q->btn[i];
    }
    for (i = 0; i < NE; i++) {
        if (q->enc[i] >= 7u || (e >> q->enc[i]) & 1u || (q->dir[i] != 1 && q->dir[i] != -1))
            return 0;
        e |= 1u << q->enc[i];
    }
    return 1;
}

UI_SMALL static uint32_t settings_restore(const void *raw, uint32_t n)
{
    persist_t p;
    if (n != sizeof p && n != PERSIST_SIZE_V22)
        return 2;
    memset(&p, 0, sizeof p);
    memcpy(&p, raw, n);
    if (p.magic != PERSIST_MAGIC || p.palette >= NPALETTES || p.lowcut > 1u || p.zoom > 1u || !panel_valid(&p.panel))
        return 2;
#if FELUCCA_ARRANGER
    if (!arr_valid(&p.arrangement, 15u))
        return 2;
    p.arrangement.reserved[0] = song_cur;            /* (sloop_custom: a backup never changes the song the FM-1 is on) */
    if ((song.playing || transport_req) && memcmp(&p.arrangement, &arrangement, sizeof arrangement))
        return 3;                                    /* SLOOP 2.5: a new song order only when stopped (as on the SONG screen) */
#endif
    if (!flash_ok || settings_write(&p))
        return 4;
    persist_saved = p;
    settings.palette = p.palette;
    settings.lowcut = p.lowcut;
    settings.zoom = p.zoom;
    panel = p.panel;
#if FELUCCA_ARRANGER
    arrangement = p.arrangement;
#endif
    lights_from_word(p.lights);
    song.g[G_SYNC] = (int16_t)lights_sync;
    song.g[G_MIDI] = (int16_t)lights_mout;
    song.g[G_ROUTE] = (int16_t)lights_min;
    palette_set(settings.palette);
    fx_lowcut = (uint8_t)(settings.lowcut != 0);
    ui.force = 1;
    return 0;
}

/* slot 0..3 (n 0: empty), or 4: the working project (loaded now) */
UI_SMALL static uint32_t project_restore(uint32_t slot, const void *raw, uint32_t n)
{
    if (song.playing || transport_req)
        return 3;
    if (slot < 4u && !n) {
        if (!flash_ok || st_save(sec_obj(slot), raw, 0))
            return 4;
        memset(&proj_slot[slot], 0, sizeof proj_slot[slot]);
        sec_dirty &= (uint8_t)~(1u << slot);
        return 0;
    }
    if (!proj_import(&autosave_buf, raw, (int)n))
        return 2;
    if (slot == 4u) {
        project_apply(&autosave_buf);
        return 0;
    }
    if (!flash_ok || st_save(sec_obj(slot), &autosave_buf, sizeof autosave_buf))
        return 4;
    memcpy(&proj_slot[slot], &autosave_buf, sizeof proj_slot[slot]);
    sec_dirty &= (uint8_t)~(1u << slot);
    return 0;
}
#endif
#if FELUCCA_ARRANGER
UI_SMALL static void arrangement_save(void)
{
    if (song.playing || transport_req) { ui_message("STOP BEFORE SAVE"); return; }
    settings_save();
#if FELUCCA_FLASH
    if (flash_ok) {
        ui_message(memcmp(&persist_saved.arrangement, &arrangement, sizeof arrangement) ? "SAVE ERROR" : "SONG SAVED");
        return;
    }
#endif
    ui_message("SONG IN RAM ONLY");
}

/* ---- live sections (SAVE + key, ui_layers.c). A section is a project slot (A..D = 1..4): stored into RAM
 * at once (playing too), written to flash once the transport is stopped and nothing sounds (an erase
 * stops the audio for ~50 ms); a song recorded with SONG REC is saved the same way. */
UI_SMALL static void section_store(uint32_t s)
{
    s &= 3u;
    fm1_irq_off();                                      /* (the audio ISR may be applying a section) */
    proj_capture(&proj_slot[s]);
    live_sec = (int8_t)s;
    fm1_irq_on();
    sec_dirty |= (uint8_t)(1u << s);
}
UI_SMALL static void section_load(uint32_t s)                    /* stopped: the section is the loop now */
{
    s &= 3u;
    project_apply(&proj_slot[s]);
    live_sec = (int8_t)s;
}
UI_SMALL static void sections_write(void)                        /* the dirty sections and song into flash */
{
    uint32_t i;
#if FELUCCA_FLASH
    if (flash_ok)
        for (i = 0; i < 4u; i++)
            if (((sec_dirty >> i) & 1u) && st_save(sec_obj(i), &proj_slot[i], sizeof proj_slot[i]) == 0)
                sec_dirty &= (uint8_t)~(1u << i);       /* (a failed write stays dirty: tried again later) */
    if (!flash_ok)
#endif
        sec_dirty = 0;
    (void)i;
    if (song_dirty || fav_dirty) {                      /* (2.7: the favourites too) */
        song_dirty = 0;
        settings_save();
    }
}
/* before an intentional reset (an update, UPDATE MODE, UBOOT from the host): the audio is stopped, so
 * whatever is only in RAM goes to flash now: the live sections, the song, the working project */
UI_SMALL static void persist_flush_now(void)
{
    sections_write();
#if FELUCCA_FLASH
    if (flash_ok && !arrangement_clock.running) {     /* (a song playing: the tracks hold a section) */
        proj_capture(&autosave_buf);
        if (autosave_buf.sum != autosave_hash && st_save(OBJ_AUTOSAVE, &autosave_buf, sizeof autosave_buf) == 0)
            autosave_hash = autosave_buf.sum;
    }
#endif
}
#if FELUCCA_FLASH
/* ---- songs (SAVE > SONG): stopped only. A song is four sections and an order; the current one is in RAM (and its
 * sections in flash as they were saved), so leaving it writes what is dirty and its order, then the other's four sections
 * are read into the slots and its order becomes the song's. Section A of it is the loop, if it holds one */
UI_SMALL static int song_is_used(uint32_t s)
{
    uint32_t i;
    if (s >= ST_NSONG)
        return 0;
    if (s != song_cur)
        return (song_used >> s) & 1u;
    for (i = 0; i < 4u; i++)
        if (project_used(i))
            return 1;
    return 0;
}
UI_SMALL static int song_idle(const char *what)
{
    if (song.playing || transport_req) {
        ui_say("STOP BEFORE ", what);
        return 0;
    }
    if (!flash_ok) {
        ui_message("NO FLASH");
        return 0;
    }
    return 1;
}
UI_SMALL static void song_switch(uint32_t n)
{
    uint32_t i;
    char nb[4];
    if (n >= ST_NSONG || !song_idle("LOAD"))
        return;
    fmt_int(nb, (int32_t)n + 1);
    if (n == song_cur) {
        ui_say("ALREADY SONG ", nb);
        return;
    }
    sections_write();                                  /* the sections not yet in flash, the order if it changed */
    if (sec_dirty) {
        ui_message("SAVE ERROR");
        return;
    }
    song_arr[song_cur] = arrangement;
    song_arr[song_cur].reserved[0] = 0;
    if (song_tab_save()) {
        ui_message("SAVE ERROR");
        return;
    }
    if (song_is_used(song_cur))
        song_used |= (uint16_t)(1u << song_cur);
    else
        song_used &= (uint16_t)~(1u << song_cur);
    fm1_irq_off();
    for (i = 0; i < 4u; i++)
        memset(&proj_slot[i], 0, sizeof proj_slot[i]);
    live_sec = -1;
    sec_dirty = 0;
    song_cur = (uint8_t)n;
    arrangement = song_arr[n];
    arrangement.reserved[0] = (uint8_t)n;
    fm1_irq_on();
    song_used &= (uint16_t)~(1u << n);
    for (i = 0; i < 4u; i++)
        proj_fetch(i);
    song.g[G_SLOT] = 1;
    settings_save();                                   /* the song and its order: where the FM-1 comes back to */
    if (proj_ok(&proj_slot[0])) {
        section_load(0);
        ui_say("SONG ", nb);
    } else {
        ui_say("EMPTY SONG ", nb);
    }
}
UI_SMALL static void song_copy_to(uint32_t n)           /* the current song's four sections and order over song n's */
{
    uint32_t i;
    char nb[4];
    if (n >= ST_NSONG || !song_idle("COPY"))
        return;
    fmt_int(nb, (int32_t)n + 1);
    if (n == song_cur) {
        ui_say("THIS IS SONG ", nb);
        return;
    }
    sections_write();
    if (sec_dirty) {
        ui_message("SAVE ERROR");
        return;
    }
    for (i = 0; i < 4u; i++)
        if (st_save(sec_obj_of(n, i), &proj_slot[i], proj_ok(&proj_slot[i]) ? sizeof proj_slot[i] : 0u)) {
            ui_message("COPY ERROR");
            return;
        }
    song_arr[n] = arrangement;
    song_arr[n].reserved[0] = 0;
    if (song_tab_save()) {
        ui_message("COPY ERROR");
        return;
    }
    if (song_is_used(song_cur))
        song_used |= (uint16_t)(1u << n);
    else
        song_used &= (uint16_t)~(1u << n);
    ui_say("COPIED TO SONG ", nb);
}
UI_SMALL static void song_erase(uint32_t n)             /* another song: its four sections gone, its order the default */
{
    uint32_t i;
    char nb[4];
    if (n >= ST_NSONG || !song_idle("ERASE"))
        return;
    fmt_int(nb, (int32_t)n + 1);
    if (n == song_cur) {
        ui_say("CURRENT SONG ", nb);
        return;
    }
    if (!song_is_used(n)) {
        ui_say("EMPTY SONG ", nb);
        return;
    }
    for (i = 0; i < 4u; i++)
        if (obj_filled(sec_obj_of(n, i)) && st_save(sec_obj_of(n, i), 0, 0)) {
            ui_message("ERASE ERROR");
            return;
        }
    arr_defaults(&song_arr[n]);
    song_tab_save();
    song_used &= (uint16_t)~(1u << n);
    ui_say("ERASED SONG ", nb);
}
#endif /* FELUCCA_FLASH (songs) */
UI_SMALL static void sections_flush(void)                        /* main loop */
{
    static uint32_t tried;
    if (srec_done) {
        song_dirty = srec_done != 0xFFu;
        if (song_dirty) {
            char b[8];
            fmt_int(b, srec_done);
            ui_say("SONG PARTS ", b);
        } else {
            ui_message("NO SONG");
        }
        srec_done = 0;
    }
    if ((uint32_t)song.g[G_SYNC] != lights_sync) {      /* GLO > SYSTEM > SYNC: kept with the settings */
        lights_sync = (uint8_t)song.g[G_SYNC];
        settings_later = 1;
    }
    if ((uint32_t)(song.g[G_MIDI] != 0) != lights_mout) {   /* GLO > SYSTEM > MIDI: the same */
        lights_mout = (uint8_t)(song.g[G_MIDI] != 0);
        settings_later = 1;
    }
    if ((uint32_t)song.g[G_ROUTE] != lights_min) {        /* GLO > SYSTEM > IN: the same (2.6: CH n too) */
        lights_min = (uint8_t)song.g[G_ROUTE];
        settings_later = 1;
    }
    {   /* 2.6: GLO > MIDI (CH1 CH2 CH3 OTHER) and GLO > DRUMS > CH: the same */
        static const uint8_t gid[10] = {G_CH1, G_CH2, G_CH3, G_DRCH, G_CHOTH, G_BEND, G_FXCH, G_DRMAP, G_RMCH, G_CLKOUT};
        uint32_t k;
        for (k = 0; k < 10u; k++)
            if ((uint32_t)song.g[gid[k]] != midi_set[k]) {
                midi_set[k] = (uint8_t)song.g[gid[k]];
                settings_later = 1;
            }
        if ((uint32_t)song.g[G_FXFADE] != fxfade) {   /* (2.7.2) GLO > MASTER 2 > FADE: a setting of the FM-1 */
            fxfade = (uint8_t)song.g[G_FXFADE];
            settings_later = 1;
        }
    }
    if (settings_later) {                               /* the menu closed while playing */
        settings_later = 0;
        song_dirty = 1;                                 /* (settings_save when quiet, with the song) */
    }
    if ((!sec_dirty && !song_dirty) || song.playing || transport_req || !audio_quiet() || fm1_ms - ui_input_ms < 1500u ||
        fm1_ms - tried < 5000u)
        return;
    tried = fm1_ms;                                     /* (a failed write: again in 5 s, not every frame) */
    sections_write();
    if (sec_dirty)
        ui_message("SAVE ERROR: RETRYING");
}
#endif
#endif /* PROJ_HOST */
