/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* CTL-1 on fm1-base: the app's unity build (compiled into fm1-base's, after the platform and its lib/touch.c) and
 * the glue between fm1-base's contract (app.h) and Ctl's parts: the settings (project.c), the engine (engine.c), the
 * UI (ui.c) and the GM synth (dsp/gm.c, its own -O2 unit). */
#include "project.c"
#include "engine.c"
/* KSTEP_SAMPLE_ONLY_ARRANGER_V040: MIDI arranger, own sample-slot flash disk, no original CTL settings migration. */
#include "ab16_fs.c" /* original analogBEAST v1.2 AB16 source, sample window 240 KiB */
#include "ks_pattern_mix.c"  /* four independent cached AB16 player lanes */
#include "ks_music.c"  /* Native SONG / ALBUM without demo ROM music */
#include "ks_song_grid_core.c"
#include "ks_arranger.c"
#include "ks_m2.c" /* transactional polyphonic M2 MIDI import/player */
#include "ks_album_grid.c" /* 1-row S/M/BPM/INSTRUMENT ALBUM editor */
#include "ks_live_notes.c" /* output-note mirror for HOME 2; no MIDI route changes */
#include "ks_step_editor.c"
#include "onekey_play.c"  /* KSTEP_ONEKEY_V057 */
#include "ui.c"

/* MIDI out: to the ports, and to the onboard synth (channel messages only: its queue is single-writer, and this
 * runs under the lock or in the tick; with Sound off no notes, but programs and controllers still set) */
static volatile uint8_t in_tick;
void ctl_send(uint32_t pkt, uint32_t to)
{
    uint32_t f = 0;
    ksal_program_observe(pkt); /* remember emitted program state for album overrides */
    if (!in_tick)
        f = plat_lock();
    if (to & (TO_USB | TO_BLE))
        plat_midi_send(pkt, to & (TO_USB | TO_BLE));
    if (to & TO_SYNTH)
        ks_live_notes_packet(pkt); /* a single update per played note */
    if ((to & TO_SYNTH) || ((to & (TO_USB | TO_BLE)) && !(to & TO_NOSYNTH))) {
        uint32_t st = (pkt >> 8) & 0xFFu;
        if (st >= 0x80u && st < 0xF0u && (proj.sound != SOUND_OFF || st >= 0xB0u))
            gm_midi(st, (pkt >> 16) & 0x7Fu, (pkt >> 24) & 0x7Fu);
    }
    if (!in_tick)
        plat_unlock(f);
}

/* ---- the contract */
static uint8_t synth_ok;
void app_init(void)
{
    project_load();                      /* defaults when nothing (or another format) is saved */
    /* KSTEP_DIGITAL_KEYS_ONLY_V044: old touch settings remain in project_t
     * for storage compatibility, but have no effect. Fixed local velocity;
     * incoming MIDI retains its original velocity. */
    proj.touch = 0;
    proj.at_mode = AT_OFF;
    proj.vel = 100;
    proj.human = 0;
    ks_live_notes_clear();
    ks_arr_init();                       /* read-only probe; never autoformat sample memory */
    ksm2_init();
    if (!synth_ok)
        synth_ok = gm_init() == 0;       /* the onboard GM synth, once */
}
void app_render(int32_t *lr, uint32_t n, uint32_t gain_q12)
{
    if (proj.sound == SOUND_OFF)
        gm_skip(lr, n);
    else
        gm_render(lr, n, gain_q12);
}
void app_quiet(int32_t *lr, uint32_t n) { gm_skip(lr, n); }
void app_overload(void) { gm_overload(); }
void app_panic(void) { ks_live_notes_clear(); gm_panic(); }
void app_tick(void)
{
    in_tick = 1;
    engine_tick();
    in_tick = 0;
}
void app_ui_init(void)
{
    static uint8_t booted;
    ui_init();
    if (!booted && FM1B_BLE && plat_ble_state() == BLE_OFF)
        ui_say("BLUETOOTH OFF", "AFTER A CRASH");   /* not twice in a row: SAVE on Setup's Bluetooth page starts it */
    booted = 1;
}
int app_sysex(const uint8_t *p,uint32_t n) { return ksm2_rx_sysex(p,n); }
void app_frame(void) { ui_frame(); }
void app_input_only(void) { ui_input_only(); }
void app_say(const char *a, const char *b) { ui_say(a, b); }
void app_splash(void)                    /* ui.c's paper and ink */
{
    lcd_fill(0, 0, 240, 240, K_BG);
    cv_begin(240, 64, K_BG);
    {
        int32_t w = text_w(&FONT_L, APP_NAME);
        cv_text((240 - w) / 2, 0, &FONT_L, APP_NAME, K_TEXT);
        cv_rect((240 - w) / 2, 37, w, 2, K_HOT);
        w = text_w(&FONT_S, APP_TAGLINE);
        cv_text((240 - w) / 2, 44, &FONT_S, APP_TAGLINE, K_DIM);
    }
    cv_blit(0, 92);
}

#ifdef PLAT_HOST
#include "host.c"                        /* the host simulator's part: checks and commands */
#endif
