/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Ctl in fm1-base's host simulator (host/plat_host.c, which includes app.c and so this): Ctl's checks for "expect",
 * its commands, a reboot and the screen reader's dump. The keys' readings: fm1-base's lib/touch.c.
 *
 * Ctl's checks: voices sound view setup_pg touch last_vel at chan vel octave program synth_program dirty sustain
 * sus_lock playing bank tempo arp scale track patN muteN ccN valN, and clocks_out notes_out notes_on (since the
 * script's last "out"). Its command: fixed (Touch off: the keys at the fixed velocity, no aftertouch; Touch is on
 * by default, and scenarios about the notes tap too quickly to be sensed). */

/* ------------------------------------------------------------ checks --- */
static int out_count(int what)           /* since the script's last "out": 0 clocks, 1 note-ons, 2 notes still on */
{
    uint32_t i;
    int n = 0;
    for (i = 0; i < n_out; i++) {
        uint32_t st = (out_pkt[i] >> 8) & 0xFFu, on = (st & 0xF0u) == 0x90u && (out_pkt[i] >> 24);
        if (what == 0)
            n += st == 0xF8u;
        else if (what == 1)
            n += (int)on;
        else
            n += on ? 1 : (st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u ? -1 : 0;
    }
    return n;
}

int app_expect(const char *what, int *got)
{
    int g;
    if (!strcmp(what, "pc_ext"))
        g = ks_pc_out_external();
    else if (!strcmp(what, "pc_int"))
        g = ks_pc_out_internal();
    else if (!strcmp(what, "pc_chan"))
        g = ks_pc_player_channel();
    else if (!strcmp(what, "once_left"))
        g = jp.once_remaining;
    else if (!strcmp(what, "once_next"))
        g = jp_album_has_next();
    else if (!strcmp(what, "once_done"))
        g = jp.once_done;
    else if (!strcmp(what, "once_started"))
        g = jp.once_started;
    else if (!strcmp(what, "album_pos"))
        g = jp.album_pos;
    else if (!strcmp(what, "album_loop"))
        g = jp.loop;
    else if (!strcmp(what, "grid_repeat"))
        g = ks_gp_repeat;
    else if (!strcmp(what, "player_running"))
        g = jp.running;
    else if (!strcmp(what, "engine_running"))
        g = eng_running();
    else if (!strcmp(what, "usb_mode"))
        g = usb_host_requested;
    else if (!strcmp(what, "voices"))         /* the onboard synth's voices sounding */
        g = gm_voices;
    else if (!strcmp(what, "sound"))
        g = proj.sound;
    else if (!strcmp(what, "view"))
        g = ui.view;
    else if (!strcmp(what, "setup_pg"))  /* Setup's pages: 1 = Touch */
        g = ui.setup_pg;
    else if (!strcmp(what, "touch"))     /* the keys are sensed (velocity or aftertouch on) */
        g = touch_live;
    else if (!strcmp(what, "last_vel"))  /* the last velocity sensed */
        g = ui.last_vel;
    else if (!strcmp(what, "at")) {      /* the most aftertouch sent for a held key */
        g = 0; /* digital keys: no aftertouch */
    } else if (!strcmp(what, "ble_dev"))   /* the Bluetooth MIDI device: BLE_DEV_* (4 connected) */
        g = plat_ble_device();
    else if (!strcmp(what, "ble_off"))
        g = proj.ble_off;
    else if (!strcmp(what, "chan"))
        g = proj.chan;
    else if (!strcmp(what, "vel"))
        g = proj.vel;
    else if (!strcmp(what, "octave"))
        g = proj.octave;
    else if (!strcmp(what, "program"))
        g = proj.program;
    else if (!strcmp(what, "synth_program"))   /* what the onboard synth plays on the channel */
        g = gm_program[proj.chan];
    else if (!strcmp(what, "dirty"))
        g = ui.dirty;
    else if (!strcmp(what, "sustain"))
        g = ui.sustain;
    else if (!strcmp(what, "sus_lock"))
        g = ui.sus_lock;
    else if (!strcmp(what, "playing"))
        g = ui.playing;
    else if (!strcmp(what, "bank"))
        g = proj.bank;
    else if (!strcmp(what, "tempo"))
        g = proj.tempo;
    else if (!strcmp(what, "arp"))
        g = proj.arp_on;
    else if (!strcmp(what, "scale"))
        g = proj.scale;
    else if (!strcmp(what, "track"))
        g = ui.track;
    else if (!strncmp(what, "pat", 3))   /* patN: track N's step bits */
        g = proj.trk[atoi(what + 3) & 3].pat;
    else if (!strncmp(what, "mute", 4))
        g = proj.trk[atoi(what + 4) & 3].mute;
    else if (!strcmp(what, "clocks_out"))
        g = out_count(0);
    else if (!strcmp(what, "notes_out"))
        g = out_count(1);
    else if (!strcmp(what, "notes_on"))
        g = out_count(2);
    else if (!strncmp(what, "cc", 2))    /* ccN: knob N's controller number */
        g = proj.cc[proj.bank][atoi(what + 2) & 3];
    else if (!strncmp(what, "val", 3))   /* valN: knob N's value */
        g = proj.val[proj.bank][atoi(what + 3) & 3];
    else
        return 0;
    *got = g;
    return 1;
}

int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)a, (void)b, (void)c, (void)d;
    if (!strcmp(cmd, "once_live_handoff")) {
        /* LIVE PLAY is pattern transport; it must not be stopped by
         * the SONG-EDITOR-only isolation on SEQ. */
        ui.view=V_LIVE;ui.playing=1u;
        eng.running=1u;jp.running=0u;
        ks_gp_enabled=0u;ks_gp_repeat=0u;
        kstep_paused=0u;
        return 1;
    }
    if (!strcmp(cmd, "once_editor_handoff")) {
        /* An editor had been looping when user opened PLAYER. */
        ui.view=V_ARR;ui.arr_pg=2u;ui.playing=1u;
        eng.running=1u;jp.running=0u;
        ks_gp_enabled=1u;ks_gp_repeat=1u;
        kstep_paused=0u;
        return 1;
    }
    if (!strcmp(cmd, "once_grid_stale_repeat")) {
        ks_gp_repeat=1u;return 1;
    }
    if (!strcmp(cmd, "once_real_album_start")) {
        /* Three real transitions through UI's S-player end machinery.
         * Only flash reads are mocked; BPM and INST in between are real. */
        jp_host_album_fixture=1u;
        memset(&ksal,0,sizeof ksal);
        ksal.count=5u;
        ksal.e[0].kind=KSAL_S; ksal.e[0].value=0u;
        ksal.e[1].kind=KSAL_BPM; ksal.e[1].value=100u;
        ksal.e[2].kind=KSAL_S; ksal.e[2].value=1u;
        ksal.e[3].kind=KSAL_INST; ksal.e[3].value=3u;
        ksal.e[4].kind=KSAL_S; ksal.e[4].value=2u;
        jp.category=1u; jp.loop=0u; jp.album_native=0u;
        ui.view=V_JUKE;
        jp.once_total=3u; jp.once_started=1u;
        jp.once_remaining=2u; jp.once_done=0u;
        jp.album_pos=1u; jp.album_len=5u; jp.running=1u;
        jp.program_active=0u; jp.base_bpm=120u;
        ks_gp_enabled=1u; ks_gp_finished=0u; ks_gp_underrun=0u;
        proj.clock=CLK_EXT; eng.running=1u;ui.playing=1u;
        return 1;
    }
    if (!strcmp(cmd, "once_real_album_end")) {
        ks_gp_enabled=1u;ks_gp_finished=1u;ks_gp_underrun=0u;
        return 1;
    }
        /* Host-only synthetic album end paths: no MUSIC DISK writes. */
    if (!strcmp(cmd, "once_grid_end")) {
        jp.category=1u; jp.loop=0u; jp.album_native=0u;
        jp.once_remaining=0u; jp.once_total=1u; jp.once_started=1u; jp.once_done=0u; jp.running=1u;
        ks_gp_enabled=1u; ks_gp_finished=1u; ks_gp_underrun=0u;
        ui.playing=1u;
        return 1;
    }
    if (!strcmp(cmd, "once_m2_end")) {
        jp.category=1u; jp.loop=0u; jp.album_native=0u;
        jp.once_remaining=0u; jp.once_total=1u; jp.once_started=1u; jp.once_done=0u; jp.running=1u;
        proj.clock=CLK_EXT;
        memset(&m2_play,0,sizeof m2_play);
        m2_play.active=1u; m2_play.have=0u;
        m2_play.saved_tempo=proj.tempo;
        ks_arr_external_player_set(1);
        eng.running=1u; ui.playing=1u;
        return 1;
    }
    if (!strcmp(cmd, "once_count_fixture")) {
        jp.category=1u; jp.loop=0u; jp.album_native=0u;
        memset(&ksal,0,sizeof ksal);
        ksal.count=4u;
        ksal.e[0].kind=KSAL_S;
        ksal.e[1].kind=KSAL_BPM;
        ksal.e[2].kind=KSAL_M;
        ksal.e[3].kind=KSAL_INST;
        jp.album_pos=1u;
        jp.once_remaining=jp_album_songs_after(jp.album_pos);
        jp.once_total=2u; jp.once_started=1u; jp.once_done=0u;
        return 1;
    }
    if (!strcmp(cmd, "splash")) {
        app_splash();
        return 1;
    }
    if (!strcmp(cmd, "fixed")) {         /* the keys at the fixed velocity, no aftertouch */
        proj.touch = 0;
        proj.at_mode = AT_OFF;
        touch_apply();
        return 1;
    }
    return 0;
}

/* a restart: the settings from the simulated flash again, the synth as at power-on (program 0 everywhere) */
void app_host_reboot(void)
{
    int32_t silence[512];
    uint32_t ch;
    memset(&proj, 0, sizeof proj);
    for (ch = 0; ch < 16; ch++) {
        gm_midi(0xC0u | ch, 0, 0);
        gm_midi(0xB0u | ch, 121, 0);
    }
    gm_panic();
    gm_render(silence, 256, 0);
}

void app_speak_dump(void) { speak_dump(); }

#ifdef PLAT_WEB
#undef __attribute__
__attribute__((used, visibility("default"))) uint32_t web_voices(void) { return gm_voices; }   /* for the tests */
#define __attribute__(x)
#endif
