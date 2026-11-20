/* SPDX-License-Identifier: GPL-3.0-only */
/* sloop_custom: songs (project.c song_switch / song_copy_to / song_erase, storage.c OBJ_SONG0..OBJ_SONGTAB) on the
 * simulated flash: each song keeps its four sections and its order across a switch and a power-on, a copy lands in the
 * other song, an erase empties it, the old song's objects and the user presets' flash are not touched. */
#include "editor_host.c"
static int fails;
static void check(int c, const char *what) { printf("song: %-84s %s\n", what, c ? "ok" : "FAIL"); fails += !c; }
static void boot_as_main(void)
{
    uint32_t i;
    persist_boot();
    settings_init();
    panel_init();
    for (i = 0; i < G_COUNT; i++) song.g[i] = GP[i].def;
    fm6_init();
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        track_defaults(t);
        if (i < NPART) { set_engine_of(t, TRK_DEF[i][0]); apply_preset_to(t, TRK_DEF[i][1]); t->engine = t->eng_req; }
        track_defaults_steps(t);
    }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    song.sel = 0;
    autosave_resume();
    layers_init();
    go_home();
}
/* (project_apply asks the audio side to stop, which a running device answers at once: the host clears it) */
static void sw(uint32_t n) { transport_req = 0; song_switch(n); }
static void cp(uint32_t n) { transport_req = 0; song_copy_to(n); }
static void er(uint32_t n) { transport_req = 0; song_erase(n); }
static void put_note(uint32_t sect, uint32_t note)     /* track 1's step 0 holds `note`, stored as section sect */
{
    trk[0].step[0].time = ST_NOTE; trk[0].step[0].n = 1; trk[0].step[0].note[0] = (uint8_t)note;
    section_store(sect);
    sections_write();
}
static int has_note(uint32_t sect, uint32_t note)
{
    return proj_ok(&proj_slot[sect]) && proj_slot[sect].t[0].step[0].time == ST_NOTE && proj_slot[sect].t[0].step[0].note[0] == note;
}
int main(void)
{
    uint32_t i;
    static uint8_t prs[4 * 4096], songs0[8 * 4096];
    harness_init();
    memset(nor, 0xFF, sizeof nor);
    boot_as_main();
    check(song_cur == 0 && flash_ok, "a first power-on: song 1, flash found");
    put_note(0, 48);
    put_note(1, 50);
    check(song_is_used(0) && !song_is_used(3), "song 1 holds sections, song 4 none");
    memcpy(prs, nor + 0xDC000, sizeof prs);                       /* the user presets' flash, to see it untouched */
    memcpy(songs0, nor + 0x97000, sizeof songs0);                 /* song 1's flash */
    sw(3);
    check(song_cur == 3 && !proj_ok(&proj_slot[0]) && !proj_ok(&proj_slot[1]), "switch to song 4: its sections are empty");
    check(song_is_used(0) && !song_is_used(3), "... song 1 is still used, song 4 (current, empty) is not");
    check(!memcmp(songs0, nor + 0x97000, sizeof songs0), "... song 1's sections in the flash are untouched");
    arrangement.count = 2;                                       /* song 4's order: two steps */
    put_note(0, 60);
    put_note(2, 62);
    sw(0);
    check(song_cur == 0 && has_note(0, 48) && has_note(1, 50) && !proj_ok(&proj_slot[2]), "back to song 1: its sections as they were");
    check(arrangement.count == 4, "... and its own order (the default 4)");
    check(song_is_used(3), "... song 4 is used now");
    sw(3);
    check(has_note(0, 60) && has_note(2, 62) && !proj_ok(&proj_slot[1]), "song 4 again: A and C as stored, B empty");
    check(arrangement.count == 2, "... and its order of two steps");
    trk[0].p[P_TRES] = 13; trk[1].p[P_TRES] = 4;                 /* per-track filter RES (kept in the engine byte's high nibble) */
    section_store(0); sections_write();
    sw(0); sw(3);
    check(trk[0].p[P_TRES] == 13 && trk[1].p[P_TRES] == 4, "per-track RES survives a song switch there and back");
    boot_as_main();                                              /* a power-on: the FM-1 comes back on song 4 */
    check(song_cur == 3 && has_note(0, 60) && has_note(2, 62), "a power-on keeps the song and its sections");
    check(proj_ok(&proj_slot[0]) && pt_get(&proj_slot[0].t[0], P_TRES) == 13 && pt_get(&proj_slot[0].t[1], P_TRES) == 4 &&
          (proj_slot[0].t[0].engine & 15u) < NENGINES, "... and the stored section keeps the RES, the engine unharmed");
    check(arrangement.count == 2 && arrangement.reserved[0] == 3, "... and its order");
    cp(5);
    check(song_is_used(5) && song_cur == 3, "copy to song 6: used, still on song 4");
    sw(5);
    check(has_note(0, 60) && has_note(2, 62) && arrangement.count == 2, "song 6 holds the copy (sections and order)");
    sw(3);
    er(5);
    check(!song_is_used(5) && song_is_used(3) && song_is_used(0), "erased song 6: empty; the others unchanged");
    sw(5);
    check(song_cur == 5 && !proj_ok(&proj_slot[0]) && arrangement.count == 4, "an erased song loads empty, with the default order");
    check(!memcmp(prs, nor + 0xDC000, sizeof prs), "the user presets' flash was never touched");
    sw(9);
    put_note(3, 70);
    sw(0);
    sw(9);
    check(has_note(3, 70), "the last song (10) works too");
    {   /* every song's flash is its own: song 4 still has its two sections */
        sw(3);
        check(has_note(0, 60) && has_note(2, 62), "songs do not overwrite each other");
    }
    (void)i;
    printf("song: %d failed\n", fails);
    return fails != 0;
}
