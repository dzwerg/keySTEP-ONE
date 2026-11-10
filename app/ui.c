/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Ctl UI: a MIDI controller. Part of the unity build (after gfx.c, project.c and engine.c); the host
 * simulator includes it too.
 *
 *   keys          chromatic from F3; with a scale: white keys the scale's steps, black keys its chords
 *   KNOB 1-4      the page's four values (on Play: the knob bank's four controllers)
 *   SELECT the page's pages (Play: the banks; Sequencer: the tracks)  ALGORITHM MIDI channel
 *   PRESETS program change  (the tempo: Setup, KNOB 1)
 *   FX            the next knob bank (A..D); held on Play, KNOB 1-4 set the bank's controller numbers
 *   ARP           the Arp page (KNOB 1 there turns it on: Off / On / Latch)
 *   PLAY          start / stop (the sequencer, MIDI start / stop)    REC  panic (all notes off)
 *   ENV           sustain while held; a double tap locks it on (a press unlocks)    OCT- / OCT+  the octave (-3..+3)
 *   LFO           the mod wheel (CC 1): up over a sixteenth while held, back down after
 *   pages         one button each: HOME Play, SEL Keys, ARP Arp, SEQ Sequencer, EDIT Sound, GLO Setup
 *   SAVE          saves (it also saves by itself, a few seconds after a change)
 *
 * Touch (SELECT on Setup): velocity and aftertouch from the keys themselves (MONK-1's sensing, from Velo:
 * sense.c), each optional and off by default. On, the 20 keys on the matrix's analog rows (F3..C5) play
 * when sense.c has their velocity (a few ms after the contact), the seven above them (the PA6 row, no
 * converter) at the digital contact with the PA10 probe's velocity; aftertouch per note or the channel's.
 *
 * On the Sequencer page the keys edit instead of playing: white keys the selected track's 16 steps,
 * black keys 1-4 select a track, 5-8 mute one.
 *
 * Out: everything played goes to USB and Bluetooth. Thru: USB in -> Bluetooth, Bluetooth in -> USB,
 * the TRS jack in -> USB and Bluetooth. The USB-only build (FM1B_BLE 0) shows no Bluetooth. */

enum { V_PLAY, V_SCALE, V_ARP, V_SEQ, V_SOUND, V_SETUP, V_ARR, V_STEP, V_JUKE, V_LIVE, NVIEWS };
static const char *const VIEW_NAME[NVIEWS] = {"Home", "Keys", "Arp", "Sequencer", "Sound", "Setup", "Arranger", "Step edit", "Player", "Live play"};
static const uint8_t VIEW_BTN[NVIEWS] = {B_HOME, B_SEL, B_ARP, B_SEQ, B_EDIT, B_GLO, B_SEQ, B_REC, B_SEQ, B_LFO};
static const char *const PORT_NAME[NPORTS] = {"USB", "BLE", "TRS"};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const SCALE_NAME[NSCALES] = {"Chromatic", "Major", "Minor", "Dorian", "Mixolydian",
                                                "Harm. minor", "Pentatonic", "Pent. minor", "Blues"};
static const uint8_t SCALE_LEN[NSCALES] = {12, 7, 7, 7, 7, 7, 5, 5, 6};
static const uint8_t SCALE_IV[NSCALES][7] = {{0}, {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10},
                                             {0, 2, 4, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 11}, {0, 2, 4, 7, 9},
                                             {0, 3, 5, 7, 10}, {0, 3, 5, 6, 7, 10}};
static const char *const ARP_NAME[NARP] = {"Up", "Down", "Up-down", "Random", "As played"};
static const char *const RATE_NAME[NRATE] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const char *const CLOCK_NAME[NCLK] = {"Int", "Int, quiet", "Ext"};
static const char *const GLOW_NAME[3] = {"Off", "Low", "High"};
static const char *const AT_NAME[NAT] = {"Off", "Poly", "Channel"};
#include "gm_names.h"
#include "fastmath.h"
#define KEY0_NOTE 53                   /* chromatic: F3 is key 0 at octave 0 */
#define SCALE0_NOTE 48                 /* a scale: its root from C3 at octave 0 */
#define FLASH_MS 120u                  /* a port's light after a message */

/* the FM-1's keys: 16 white, 11 black, as the keyboard runs F3..G5 */
static const uint8_t WHITE_K[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_K[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};

/* the screen's colours, paper and ink: warm white paper, ink-blue type, vermilion for what is played,
 * mustard for controllers and connections (_T: their tints; K_COOL_D: mustard dark enough for type) */
#define K_BG RGB(15, 20, 29)
#define K_PANEL RGB(32, 42, 56)
#define K_LINE RGB(61, 76, 95)
#define K_DIM RGB(158, 173, 190)
#define K_TEXT RGB(241, 245, 249)
#define K_WHITE RGB(255, 255, 255)
#define K_HOT RGB(229, 72, 46)
#define K_HOT_T RGB(94, 48, 45)
#define K_COOL RGB(241, 187, 78)
#define K_COOL_T RGB(73, 60, 37)
#define K_COOL_D RGB(230, 178, 73)
#define K_KEYW RGB(182, 196, 212)
#define K_KEYB RGB(39, 50, 67)

enum { KM_NONE, KM_NOTES, KM_ARP };    /* what a held key did: sent notes, gave them to the arpeggiator */
static struct {
    uint8_t view;
    uint8_t live_bank; /* 0..10 selected by black keys */
    uint8_t home_pg; /* KSTEP_HOME_DUAL_V0511: 0 pattern lanes, 1 original piano; volatile */
    uint8_t arr_pg;  /* ARRANGER page, SELECT cycles */
    uint8_t arr_row; /* highlighted row, ALGO moves it */
    uint8_t setup_pg;                  /* GLO: 0 SETUP, 2 BLUETOOTH, 3 PROGRAM CHANGE (1 legacy touch) */
    uint8_t last_vel;                  /* the last note's velocity from the keys (Touch shows it) */
    uint8_t track;                     /* the sequencer track the Sequencer page edits */
    uint32_t btn, keys;
    uint32_t enc_t[NE];
    uint8_t kmode[NKEYS], knotes[NKEYS][3], kn[NKEYS];   /* per held key: how, which notes */
    /* KSTEP_HOME_LIVE_NOTE_V0513: display-only state, not flash-persisted. */
    uint32_t live_serial, live_key_stamp[NKEYS];
    uint32_t live_midi_bits[16][4], live_midi_stamp[16];
    uint8_t live_midi_last[16];
    uint32_t heard[4];                 /* notes held from MIDI in (any port), for the key lights */
    uint8_t playing;
    uint8_t sustain;
    uint8_t sus_lock;                  /* ENV tapped twice: sustain stays on after the release */
    uint32_t env_t;                    /* when ENV last went down */
    uint32_t save_t;                   /* when SAVE went down; save_long: held long enough (Setup: Bluetooth) */
    uint8_t save_long;
    uint8_t fx_used;                   /* a knob turned while FX was held: no bank change at its release */
    uint32_t fx_t;                     /* when FX went down */
    uint8_t dirty;
    uint32_t act_t;
    int32_t master_raw;                /* the MASTER pot, smoothed, and the value last sent */
    int16_t master_sent;
    char last[24];                     /* the last message played here, in words */
    char msg[2][24];
    uint32_t msg_until;
    int8_t touched;                    /* the knob turned last (0..3), shown bright for a moment */
    uint32_t touch_until;
    uint32_t frame;
    uint32_t sig[4];                   /* what each band shows: redrawn when it changes */
} ui;

/* the screen reader (announce.c; the "screen reader" section below): what the key handlers did, said at the
 * next frame (text is built there, not in the handlers) */
#ifdef PLAT_HOST
#define SP_POOL
#else
#define SP_POOL __attribute__((section(".pool")))
#endif
enum { SE_NONE, SE_STEP, SE_STEP_OUT, SE_MUTE, SE_PANIC };
static struct {
    uint32_t ctx;                      /* the page, hashed: a change is said */
    int16_t val[4];                    /* the four knobs' values */
    uint8_t chan, program, playing, sus_lock, ble, started;
    uint32_t touch_until;              /* a knob turned (even when its value could not change) */
    uint8_t ev, ev_arg, ev_on;         /* the last key event: SE_* */
} sp SP_POOL;
static uint8_t sp_said;                /* a message went to the screen reader this frame: speak() leaves it be */
static void sp_event(int ev, int arg, int on)
{
    sp.ev = (uint8_t)ev;
    sp.ev_arg = (uint8_t)arg;
    sp.ev_on = (uint8_t)on;
}

static void msg_text(const char *a, const char *b, char *t)   /* the message as a reader says it: "a b" */
{
    uint32_t i, k = 0;
    for (i = 0; i < 23u && a && a[i]; i++)
        t[k++] = a[i];
    if (b && b[0])
        t[k++] = ' ';
    for (i = 0; i < 23u && b && b[i]; i++)
        t[k++] = b[i];
    while (k && t[k - 1] == '.')               /* "UPDATE MODE IN 3..": the dots are for the eye */
        k--;
    t[k] = 0;
}
/* a message on the top bar; flags: how a screen reader gets it (0: not at all, the page change says it) */
static void say_as(const char *a, const char *b, uint32_t flags)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    if (flags && ann_on()) {
        msg_text(a, b, t);
        ann_say(t, 0, flags);
        sp_said = 1;
    }
}
static void say(const char *a, const char *b) { say_as(a, b, ANN_NOW); }
/* something did not happen: a screen reader says it whole (what follows does not cut it off) */
static void warn(const char *a, const char *b) { say_as(a, b, ANN_NOW | ANN_KEEP); }
void ui_say(const char *a, const char *b) { say(a, b); }
int ui_dirty(void) { return ui.dirty != 0; }
static void mark_dirty(void) { ui.dirty = 1; }

static char *put_s(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}
static char *put_u(char *b, uint32_t v)
{
    char t[12];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *b++ = t[--n];
    *b = 0;
    return b;
}
static char *put_note(char *b, int n)    /* C4 = 60 */
{
    b = put_s(b, NOTE_NAME[n % 12]);
    if (n < 12)
        return put_s(b, "-1");
    return put_u(b, (uint32_t)(n / 12 - 1));
}

/* KSTEP_TRACK_LOCAL_SOUND_V053: selected ALGO track's instrument,
 * never the global CTL-1 program or the MIDI OUT channel. */
static char *prog_name(char *b, uint32_t p)
{
    if (proj.sound == SOUND_OFF)
        return put_u(put_s(b, "Program "), p + 1u);
    return put_s(b, GM_PROGRAM[p & 127u]);
}

/* ------------------------------------------------------------- MIDI --- */
static uint32_t pkt3(uint32_t st, uint32_t d1, uint32_t d2) { return (st >> 4) | st << 8 | d1 << 16 | d2 << 24; }
/* Restore all four independent programs; NEVER send an external PC that
 * could overwrite another track sharing the same MIDI output channel. */
static void synth_program(void)
{
    uint32_t tr;
    for(tr=0;tr<KS_TRACKS;tr++)
        ctl_send(pkt3(0xC0u|tr,ks_program_get((uint8_t)tr),0),TO_SYNTH);
}

static void send(uint32_t pkt, uint32_t to) { ctl_send(pkt, to); }   /* (fm1-base counts each port's messages) */
static void play(uint32_t st, uint32_t d1, uint32_t d2)
{
    /* Feed the recorder an INTERNAL track tag, not its external MIDI channel.
     * Otherwise an output channel of 10 would be mistaken for drum input. */
    uint32_t pkt=pkt3((st&0xF0u)|ks_selected_track,d1,d2);
    if (ks_recording) ks_arr_record_packet(pkt);
    ks_arr_track_send(ks_selected_track,st,d1,d2);
}

static void cc(uint32_t num, uint32_t v)
{
    char *b;
    play(0xB0u, num, v);
    b = put_u(put_s(ui.last, "CC "), num);
    put_u(put_s(b, " = "), v);
}
/* KSTEP_HOME_SUST_TEMPO_V059: K2 = Sustain ON/OFF (CC64) for selected track.
 * OFF releases pedal on the ORIGINAL track, even before ALGO changes track.
 * Not persisted: never boot with CC64 logically stuck on. */
static uint8_t ks_sustain_track = 255u;
static void ks_sustain_apply(int on)
{
    if (ui.sustain && ks_sustain_track < KS_TRACKS)
        ks_arr_track_send(ks_sustain_track, 0xB0u, 64u, 0u);
    ks_sustain_track = 255u;
    ui.sustain = ui.sus_lock = 0u;
    if (on) {
        ks_sustain_track = ks_selected_track;
        ks_arr_track_send(ks_sustain_track, 0xB0u, 64u, 127u);
        ui.sustain = ui.sus_lock = 1u;
    }
}

/* the notes a key plays: chromatic, or with a scale the white keys its steps and the black keys the
 * triads on its steps */
static int key_white(int k)
{
    int i;
    for (i = 0; i < 16; i++)
        if (WHITE_K[i] == k)
            return i;
    return -1;
}
static int key_black(int k)
{
    int i;
    for (i = 0; i < 11; i++)
        if (BLACK_K[i] == k)
            return i;
    return -1;
}
static int scale_note(int deg)
{
    int len = SCALE_LEN[proj.scale];
    return SCALE0_NOTE + proj.root + 12 * proj.octave + 12 * (deg / len) + SCALE_IV[proj.scale][deg % len];
}
static int key_notes(int k, uint8_t *out)
{
    int n = 0, i, w, v;
    if (!proj.scale) {
        v = KEY0_NOTE + k + 12 * proj.octave;
        if (v >= 0 && v < 128)
            out[n++] = (uint8_t)v;
        return n;
    }
    if ((w = key_white(k)) >= 0) {
        v = scale_note(w);
        if (v >= 0 && v < 128)
            out[n++] = (uint8_t)v;
        return n;
    }
    w = key_black(k);
    for (i = 0; i < 3; i++) {
        v = scale_note(w + 2 * i);
        if (v >= 0 && v < 128)
            out[n++] = (uint8_t)v;
    }
    return n;
}

static void key_down(int k, uint32_t vel)
{
    uint8_t *q = ui.knotes[k];
    int n = key_notes(k, q), i;
    char *b;
    ui.kn[k] = (uint8_t)n;
    if (!n)
        return;
    ui.kmode[k] = proj.arp_on ? KM_ARP : KM_NOTES;
    ui.live_key_stamp[k]=++ui.live_serial; /* LIVE note visible at STOP too */
    for (i = 0; i < n; i++) {
        if (proj.arp_on)
            arp_press(q[i]);
        else
            play(0x90u, q[i], vel);
    }
    b = put_note(put_s(ui.last, n > 1 ? "Chord " : "Note "), q[0]);
    if (!proj.arp_on)
        put_u(put_s(b, "  "), vel);
}

static void key_up(int k)
{
    int i;
    for (i = 0; i < ui.kn[k]; i++) {
        if (ui.kmode[k] == KM_ARP)
            arp_release(ui.knotes[k][i]);
        else if (ui.kmode[k] == KM_NOTES)
            play(0x80u, ui.knotes[k][i], 0);
    }
    ui.kmode[k] = KM_NONE;
    ui.kn[k] = 0;
    ui.live_key_stamp[k]=0u; /* Note Off must update HOME 2 */
}

static void all_keys_up(void)
{
    int k;
    for (k = 0; k < NKEYS; k++)
        key_up(k);
}

static void panic(void)
{
    ks_sustain_apply(0); /* release any held pedal before panic/USB role switch */
    all_keys_up();
    memset(ui.live_midi_bits,0,sizeof ui.live_midi_bits);
    memset(ui.live_midi_stamp,0,sizeof ui.live_midi_stamp);
    engine_panic();
    play(0xB0u, 64, 0);
    play(0xB0u, 123, 0);                 /* all notes off */
    if (proj.seq_ch != proj.chan)
        send(pkt3(0xB0u | proj.seq_ch, 123, 0), TO_USB | TO_BLE);
    ui.sustain = ui.sus_lock = 0;
    put_s(ui.last, "All notes off");
}

/* MIDI in: forwarded (USB -> BLE, BLE -> USB, TRS -> both), noted for the lights; clock and
 * start / stop drive the engine when the clock is external */
/* a controller heard on the channel: the knobs set to it show its value (a device that sends back, as the
 * OXI E16, stays in step). It counts as activity: the autosave waits for the device to rest too (each save stops
 * everything for a moment, and the lights flickered while a device's knob turned) */
static void follow_cc(uint32_t num, uint32_t v)
{
    uint32_t b, k;
    for (b = 0; b < NBANKS; b++)
        for (k = 0; k < NKNOBS; k++)
            if (proj.cc[b][k] == num && proj.val[b][k] != v) {
                proj.val[b][k] = (uint8_t)v;
                mark_dirty();
                ui.act_t = plat_ms();
            }
}

static const uint32_t THRU[NPORTS] = {TO_BLE, TO_USB, TO_USB | TO_BLE};
/* HOME 2 display-only note tracking; status channel is the incoming
 * MIDI channel, not the destination track. Velocity-zero Note On is Off. */
static void ks_home_midi_observe(uint32_t st,uint32_t note,uint32_t velocity)
{
    uint32_t ch=st&15u,kind=st&0xF0u;
    if(kind==0x90u && velocity){
        ui.live_midi_bits[ch][note>>5] |= 1u<<(note&31u);
        ui.live_midi_last[ch]=(uint8_t)note;
        ui.live_midi_stamp[ch]=++ui.live_serial;
    }else if(kind==0x80u || kind==0x90u){
        ui.live_midi_bits[ch][note>>5] &= ~(1u<<(note&31u));
    }else if(kind==0xB0u && (note==120u || note==123u)){
        memset(ui.live_midi_bits[ch],0,sizeof ui.live_midi_bits[ch]);
        ui.live_midi_stamp[ch]=0u;
    }
}
static void midi_in(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        if(ksm2_rx_packet(pkt,port))continue; /* dedicated M2 SysEx RX */
        uint32_t st = (pkt >> 8) & 0xFFu, n = (pkt >> 16) & 0x7Fu, v = pkt >> 24;
        if (port < 0 || port >= NPORTS)
            continue;
        if(st<0xF0u)ks_home_midi_observe(st,n,v); /* display-only */
        if (ks_recording) ks_arr_record_packet(pkt);
        send(pkt & ~0xF0u, THRU[port] | TO_NOSYNTH);  /* cable 0 */
        if (!proj.in_mute)
            send(pkt & ~0xF0u, TO_SYNTH);
        if (st >= 0xF8u) {
            engine_midi_clock(st);
            if (proj.clock == CLK_EXT && (st == 0xFAu || st == 0xFBu || st == 0xFCu))
                ui.playing = st != 0xFCu;
        } else if ((st & 0xF0u) == 0x90u && v) {
            ui.heard[n >> 5] |= 1u << (n & 31u);
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            ui.heard[n >> 5] &= ~(1u << (n & 31u));
        } else if ((st & 0xF0u) == 0xB0u && (n == 123u || n == 120u)) {
            memset(ui.heard, 0, sizeof ui.heard);
        }
        if ((st & 0xF0u) == 0xB0u && (st & 0x0Fu) == proj.chan)
            follow_cc(n, v);
    }
}

/* KSTEP_DIGITAL_KEYS_ONLY_V044: FM-1 keys are DIGITAL ONLY.
 * APP_TOUCH is absent: no sense.c or touch.c is linked, and no ADC
 * sampling for key velocity/pressure is enabled.
 * MIDI input retains incoming velocity; own key presses always send 100.
 * The obsolete source API is a no-op to keep the project settings ABI. */
#define touch_live 0
static void touch_apply(void) {}
static void at_off(void) {}
static uint32_t edge_vel(uint32_t k) { (void)k; return 100u; }

/* KSTEP_V050_LOOM_EDITOR: four tracks, 512 logical pattern IDs, AB16 persistence. */
static kse_editor_t ks_ed;
static uint32_t ks_ed_revision;
static uint32_t ks_ed_key_t[NKEYS];
static uint8_t ks_ed_key_changed[NKEYS];
static uint8_t ks_ed_note_buf[KSE_MAX_CHORD],ks_ed_note_count;
/* KSTEP_LOOM_EDITOR_FIX_V052: one anchor WHITE step and a second length endpoint. */
static int ks_ed_anchor_key=-1,ks_ed_end_key=-1;
static int ks_ed_read_step(void *unused,unsigned tr,unsigned pat,unsigned step,kse_step_t*out){
    (void)unused;
    return (pat!=ks_pattern_num||tr>=4u||step>=ks_arr_len())?-1:ks_arr_editor_get((uint8_t)tr,(uint8_t)step,out);
}
static int ks_ed_write_step(void *unused,unsigned tr,unsigned pat,unsigned step,const kse_step_t*in){
    int r;(void)unused;
    if(pat!=ks_pattern_num||tr>=4u||step>=ks_arr_len())return -1;
    r=ks_arr_editor_set((uint8_t)tr,(uint8_t)step,in);
    if(!r)ks_ed_revision++;
    return r;
}
static int ks_ed_read_len(void *unused,unsigned tr,unsigned pat,unsigned *len){
    (void)unused;
    if(pat!=ks_pattern_num||tr>=4u||!len)return -1;
    *len=ks_arr_len();return 0;
}
static int ks_ed_write_len(void *unused,unsigned tr,unsigned pat,unsigned len){
    (void)unused;
    if(pat!=ks_pattern_num||tr>=4u||len<8u||len>128u)return -1;
    ks_arr_set_length((uint8_t)len);
    if(ks_arr_len()!=len)return -1;
    ks_ed_revision++;return 0;
}
static void ks_ed_sync_pattern(void){
    unsigned i;for(i=0;i<KSE_TRACKS;++i)ks_ed.pattern[i]=ks_pattern_num;
    kse_select_track(&ks_ed,ks_selected_track); /* fix: opening editor on TR2/3/4 */
    ks_ed.bank=0;ks_ed.held_step=-1;ks_ed.undo_valid=0;
    ks_ed_anchor_key=-1;ks_ed_end_key=-1;
    memset(ks_ed_key_t,0,sizeof ks_ed_key_t);
    ks_ed_revision++;
}
static void ks_ed_key_start(int key){
    int white=key_white(key),pitch=KEY0_NOTE+key+12*proj.octave;
    if(pitch<0)pitch=0;
    if(pitch>127)pitch=127;
    if((plat_buttons()>>B_SEQ)&1u){
        if(ks_ed_note_count<KSE_MAX_CHORD)ks_ed_note_buf[ks_ed_note_count++]=(uint8_t)pitch;
    } else if(white>=0){
        ks_ed_key_t[key]=plat_ms()+1u;ks_ed_key_changed[key]=0;
        if(ks_ed_anchor_key>=0 && ks_ed_anchor_key!=key){
            /* LOOM gesture: hold first white, press another white to extend
             * the FIRST note's duration. Never toggle either on release. */
            int anchor=ks_ed_anchor_key;
            ks_ed_key_changed[anchor]=1;
            ks_ed_key_changed[key]=1;
            ks_ed_end_key=key;
            if(kse_extend_held_to(&ks_ed,(unsigned)white)==0)ks_ed_revision++;
        } else {
            ks_ed_anchor_key=key;
            (void)kse_hold_white(&ks_ed,white);
            ks_ed_revision++;
        }
    } else {
        uint8_t n=(uint8_t)pitch;
        (void)kse_arm_notes(&ks_ed,&n,0,1);
        ks_ed_revision++;
    }
}
static void ks_ed_key_end(int key){
    int white=key_white(key);
    if(white>=0 && ks_ed_key_t[key]){
        if(key==ks_ed_anchor_key){
            if(plat_ms()-(ks_ed_key_t[key]-1u)<400u && !ks_ed_key_changed[key]){
                if(kse_toggle_white(&ks_ed,(unsigned)white)==0)ks_ed_revision++;
            }
            ks_ed_anchor_key=-1;
            (void)kse_hold_white(&ks_ed,-1);
        } else if(key==ks_ed_end_key){
            ks_ed_end_key=-1; /* release of endpoint never toggles a step */
        }
        ks_ed_key_t[key]=0;
        ks_ed_revision++;
    }
}
static void ks_ed_drop_key_holds(void){
    unsigned j;for(j=0;j<NKEYS;++j){ks_ed_key_t[j]=0;ks_ed_key_changed[j]=1;}
    ks_ed_anchor_key=-1;ks_ed_end_key=-1;
    (void)kse_hold_white(&ks_ed,-1);
    ks_ed_note_count=0;
}
static void ks_ed_init(void){
    kse_backend_t io={ks_ed_read_step,ks_ed_write_step,ks_ed_read_len,ks_ed_write_len};
    kse_init(&ks_ed,io,0);
    ks_ed_sync_pattern();
}

/* ------------------------------------------------------------ values --- */
/* KSTEP_HOME_TRACK_KNOBS_V054: HOME K1=MIDI CH1..16/ALL, K4=-/SOLO/MUTE. */
/* KSTEP_USB_BT_K2_V1: volatile USB role, always CLIENT at boot. */
/* KSTEP_USB_HOST_DIAG_V055: volatile evidence, NEVER saved to the project. */
static uint8_t usb_host_requested;
static uint8_t usb_host_reason; /* 0 none, 1 no MIDI interface, 2 low speed, 3 link lost */
static const char *usb_host_status(void)
{
    int state = plat_usb_host_state();
    if (!usb_host_requested)
        return "DEVICE";
    if (state == USBH_CONNECTED)
        return "MIDI READY";
    if (state == USBH_OFF)
        return "SWITCHING";
    if (usb_host_reason == 1) {
        switch (plat_usb_host_diag() & 255u) {
        case 1: return "E1 DEVICE DESC";
        case 2: return "E2 SET ADDRESS";
        case 3: return "E3 CONFIG HEAD";
        case 4: return "E4 CONFIG DATA";
        case 5: return "E5 NO MIDI IF";
        case 6: return "E6 SET CONFIG";
        case 7: return "E7 SET IFACE";
        case 8: return "E8 HUB";
        default: return "E0 ENUM FAILED";
        }
    }
    if (usb_host_reason == 2)
        return "LOW SPEED";
    if (usb_host_reason == 3)
        return "LINK LOST";
    return "WAITING";
}
static void usb_host_events(void)
{
    /* Must consume this one-shot event: the original CTL UI ignored it. */
    int ev = plat_usb_host_msg();
    switch (ev) {
    case USBH_MSG_ON:
        usb_host_reason = 0;
        say("USB HOST", "WAITING");
        break;
    case USBH_MSG_OFF:
        usb_host_reason = 0;
        say("USB", "DEVICE MODE");
        break;
    case USBH_MSG_MIDI:
        usb_host_reason = 0;
        say("USB HOST", "MIDI READY");
        break;
    case USBH_MSG_NOMIDI:
        usb_host_reason = 1;
        say("USB HOST", "ENUM CHECK");
        break;
    case USBH_MSG_LOW:
        usb_host_reason = 2;
        say("USB HOST", "LOW SPEED");
        break;
    case USBH_MSG_LOST:
        usb_host_reason = 3;
        say("USB HOST", "LINK LOST");
        break;
    default: break;
    }
}

/* what a knob edits: one of these */
enum { K_VAL0, K_VAL1, K_VAL2, K_VAL3, K_CC0, K_CC1, K_CC2, K_CC3, K_ROOT, K_SCALE, K_VEL, K_HUMAN,
       K_AMODE, K_ARATE, K_AOCT, K_ARP, K_STEPS, K_HITS, K_ROT, K_NOTE, K_CLOCK, K_SEQCH, K_MASTER, K_GLOW,
       K_TEMPO, K_SOUND, K_SNDIN, K_TOUCH, K_CURVE, K_ATM, K_ATS, K_DEV, K_BT, K_USBMODE, K_ARRTRACK, K_ARRPAT, K_ARRLEN, K_ARRCHAN, K_ARRMENU, K_EPAT, K_ELEN, K_EGATE, K_ECHANCE, K_ENOTE, K_EVELOCITY, K_HOMECHAN, K_HOMEMODE, K_HOMESUST, K_ARRMODE, K_ASLOT, K_ASONG, K_ACOUNT, K_SOVERLAY, K_PMPAT, K_PMLEN, K_PMTRACK, K_SGSONG, K_SGPAT, K_JPSLOT, K_JPLOOP, K_AGALBUM, K_AGSONG, K_AGVALUE, K_LVOL1, K_LVOL2, K_LVOL3, K_LVOL4, K_PCEXT, K_PCINT, K_PCCHAN, K_NONE };
static const uint8_t VIEW_KNOB[NVIEWS][4] = {
    {K_HOMECHAN, K_HOMESUST, K_TEMPO, K_HOMEMODE},
    {K_ROOT, K_SCALE, K_NONE, K_NONE},
    {K_ARP, K_AMODE, K_ARATE, K_AOCT},
    {K_STEPS, K_HITS, K_ROT, K_NOTE},
    {K_SOUND, K_SNDIN, K_MASTER, K_NONE},
    {K_TEMPO, K_CLOCK, K_SEQCH, K_GLOW},
    {K_NONE, K_NONE, K_NONE, K_NONE},
    {K_EPAT, K_ELEN, K_EGATE, K_ECHANCE},
    {K_JPSLOT, K_NONE, K_NONE, K_JPLOOP},
};
/* Setup's third page (FM1B_BLE): a Bluetooth MIDI device (FM1B_BLE_CENTRAL), and Bluetooth on or off */
static const uint8_t BT_KNOB[4] = {FM1B_BLE_CENTRAL ? K_DEV : K_NONE, K_USBMODE, K_NONE, K_BT};
/* GLO short cycles the displayed pages: SETUP -> BLUETOOTH -> PROGRAM.
 * setup_pg == 1 (legacy TOUCH) is retained for compatibility, not visited. */
#define SETUP_PAGES (FM1B_BLE ? 3 : 2)
/* Bluetooth in a word, for the ports, Setup and a screen reader: "off" (by the player), "stopped" (after a crash:
 * SAVE starts it), "starting", "failed", "connected" (a computer, or a device), "ready" */
static int bt_stopped(void) { return FM1B_BLE && !proj.ble_off && plat_ble_state() == BLE_OFF; }
static const char *bt_status(void)
{
    int s = plat_ble_state();
    if (proj.ble_off)
        return "off";
    if (s == BLE_OFF)
        return "stopped";
    if (s == BLE_STARTING)
        return "starting";
    if (s == BLE_FAILED)
        return "failed";
    return plat_ble_linked() || plat_ble_device() == BLE_DEV_READY ? "connected" : "ready";   /* (linked: it holds
                                                                                                * the FM-1, listening or not) */
}
/* FX held on Play: the knobs set the bank's controller numbers, shown once it has been held a moment
 * (a tap to change banks does not flash them) or a knob turns */
/* KSTEP_KEYS_FOOTER_V034: long-press threshold and per-key timing.
 * Keep MIDI CC1/CC64 handling intact; only the LFO/ENV BUTTON actions go away. */
#define KSTEP_HOLD_MS 650u
static uint32_t kstep_press_ms[NB];
static uint32_t kstep_held_once;
static uint8_t kstep_fx_long;
static uint8_t kstep_paused; /* pause remembers sequencer position; no flash persistence */
static uint8_t ks_album_slot; /* ALBUM EDIT selection, not stored in flash */
/* V0522: The three PATTERN commands are directly actionable on the menu page.
 * SELECT scrolls CLEAR/LOAD/SAVE, SEL executes; knobs only edit their OWN
 * bottom readout fields. All four workspaces are independent and unsaved.
 * The ONLY submenu is the explicit overwrite confirmation on SAVE. */
enum { PM_MAIN, PM_OVERWRITE };
static struct {
    uint8_t stage, row, track, len, have_pattern;
    uint16_t pat;
    uint32_t revision;
} pm;
/* Player: SELECT chooses SONG/ALBUM; K1 picks EXISTING S/M or album;
 * K4 ONCE/LOOP. Track is not encoded into saved pattern data. */
static struct {uint8_t category,kind,loop,has_song,has_album;uint16_t song,album;
    uint16_t album_pos,album_len,once_remaining,once_total,once_started;
    uint8_t album_native,once_done,manual_bpm,manual_set,base_bpm,running,program_active,program_channel,saved_program[16];
} jp;
#ifdef PLAT_HOST
static uint8_t jp_host_album_fixture; /* host-only mock of stored S-song reads */
#endif
static void jp_refresh(void){ui.sig[0]=ui.sig[1]=ui.sig[2]=ui.sig[3]=0;}
/* Enumerate saved new albums and legacy albums in one ordered list. */
static int jp_album_next(uint16_t from,int dir,uint16_t *out){
    unsigned i;int n=(int)from;
    if(!out||(dir!=1&&dir!=-1))return -1;
    for(i=0;i<32u;i++){
        n=(n+dir+32)%32;
        if(ksal_exists((uint16_t)n)||ks_arr_album_exists((uint16_t)n)){
            *out=(uint16_t)n;return 0;
        }
    }
    return -1;
}
static void jp_first(void){
    uint16_t id;
    jp.has_song=jp.has_album=0;
    if(ks_arr_song_next(63u,1,&id)==0){jp.kind=0;jp.song=id;jp.has_song=1;}
    else if(ksm2_select(65535u,1,&id)==0){jp.kind=1;jp.song=id;jp.has_song=1;}
    if(jp_album_next(31u,1,&id)==0){jp.album=id;jp.has_album=1;}
    jp_refresh();
}
static void jp_move(int delta){
    int dir=delta>0?1:-1,n=delta>0?delta:-delta,i;
    uint16_t id;
    if(eng_running()||ks_recording||!delta)return;
    if(n>32)n=32;
    if(jp.category){
        for(i=0;i<n;i++)if(jp_album_next(jp.has_album?jp.album:31u,dir,&id)==0){jp.album=id;jp.has_album=1;}
        else break;
    }else for(i=0;i<n;i++){
        if(!jp.has_song){
            if(ks_arr_song_next(63u,1,&id)==0){jp.kind=0;jp.song=id;jp.has_song=1;}
            else if(ksm2_select(65535u,1,&id)==0){jp.kind=1;jp.song=id;jp.has_song=1;}
            else break;
            continue;
        }
        if(jp.kind==0u){
            if(ks_arr_song_next(jp.song,dir,&id)==0 &&
                ((dir>0&&id>jp.song)||(dir<0&&id<jp.song))){jp.song=id;continue;}
            if(ksm2_select(dir>0?65535u:0u,dir,&id)==0){jp.kind=1;jp.song=id;continue;}
            if(ks_arr_song_next(jp.song,dir,&id)==0){jp.song=id;continue;}
        }else{
            if(ksm2_select(jp.song,dir,&id)==0 &&
                ((dir>0&&id>jp.song)||(dir<0&&id<jp.song))){jp.song=id;continue;}
            if(ks_arr_song_next(dir>0?63u:0u,dir,&id)==0){jp.kind=0;jp.song=id;continue;}
            if(ksm2_select(jp.song,dir,&id)==0){jp.song=id;continue;}
        }
        break;
    }
    jp_refresh();
}
/* Album references saved S-Songs. Each may be a pattern grid OR native MIDI
 * event song. Sequencing is coordinated from main loop, never AB16 in ISR. */
static int jp_prepare_s(uint16_t id){
    int grid_rc;
#ifdef PLAT_HOST
    /* The host fixture drives the genuine UI song-transition machinery.
     * Skip only AB16 reads, which are not part of this timing test. */
    if(jp_host_album_fixture){(void)id;ks_gp_enabled=1u;ks_gp_finished=0u;ks_gp_underrun=0u;return 0;}
#endif
    if(!ks_arr_song_exists(id))return -1;
    if(ks_arr_set_mode(1u)<0||ks_arr_load_song(id)<0)return -1;
    ks_arr_grid_set_loop(0); /* Player controls album/song repeat itself. */
    grid_rc=ks_arr_grid_play_prepare();
    if(grid_rc==-4 && ks_arr_song_events()==0u)return -1;
    if(grid_rc<0 && grid_rc!=-4)return -1;
    return 0;
}
/* A control bubble applies to the NEXT S/M song only, and is not played. */
static void jp_restore_program(void){
    unsigned ch;
    if(!jp.program_active)return;
    jp.program_active=0u;
    for(ch=0;ch<16u;ch++){
        if(jp.program_channel<16u && ch!=jp.program_channel)continue;
        ks_player_program_send(pkt3(0xc0u|ch,jp.saved_program[ch],0),TO_USB|TO_BLE|TO_SYNTH);
    }
}
static int jp_album_song(int reset){
    uint8_t bpm=0,program=0,has_bpm=0,has_program=0;
    /* ONCE is a one-way cursor. Under no circumstances wrap when exhausted. */
    if(jp.once_done && !jp.loop)return -1;
    unsigned visited=0;
    if(reset){jp.album_pos=0;}
    jp_restore_program(); /* previous instrument block must not leak */
    while(visited++ <= KSAL_MAX_ENTRIES){
        ksal_entry_t item;int rc;
        if(jp.album_pos>=ksal.count){
            if(!jp.loop)return -1;
            jp.album_pos=0;
            if(!ksal.count)return -1;
        }
        item=ksal.e[jp.album_pos++];
        if(item.kind==KSAL_BPM){bpm=(uint8_t)item.value;has_bpm=1;continue;}
        if(item.kind==KSAL_INST){program=(uint8_t)item.value;has_program=1;continue;}
        if(item.kind==KSAL_M){rc=ksm2_prepare(item.value);jp.kind=1;}
        else if(item.kind==KSAL_S){rc=jp_prepare_s(item.value);jp.kind=0;}
        else return -1;
        if(rc<0)return rc;
        jp.manual_set=0u; /* a new album song starts its own override scope */
        if(has_bpm){
            if(jp.kind==1)ksm2_override_tempo(bpm);
            else if(proj.clock!=CLK_EXT)proj.tempo=bpm;
        }else if(proj.clock!=CLK_EXT && jp.kind==0){
            proj.tempo=jp.base_bpm;
        }
        if(has_program){
            unsigned ch;
            jp.program_channel=ks_pc_player_channel();
            for(ch=0;ch<16u;ch++)jp.saved_program[ch]=ksal_program_current((uint8_t)ch);
            jp.program_active=1u;
            if(jp.kind==1)ksm2_override_program(program);
            else for(ch=0;ch<16u;ch++){
                if(jp.program_channel<16u && ch!=jp.program_channel)continue;
                ks_player_program_send(pkt3(0xc0u|ch,program,0),TO_USB|TO_BLE|TO_SYNTH);
            }
        }
        return 0;
    }
    return -1; /* cannot spin forever on an all-control album */
}
/* Number of songs AFTER the currently playing song, ignoring BPM/INST blocks.
 * Count is latched when PLAYER starts; it is not inferred from a wrapped cursor
 * or re-probed from flash after a transition. */
static uint16_t jp_album_songs_after(uint16_t pos)
{
    uint16_t i,n=0;
    if (jp.album_native) {
        return pos+1u<jp.album_len ? jp.album_len-pos-1u : 0u;
    }
    for(i=pos;i<ksal.count;i++)
        if(ksal.e[i].kind==KSAL_S || ksal.e[i].kind==KSAL_M) n++;
    return n;
}
static int jp_prepare(void){
    jp.once_remaining=0;
    jp.once_total=0;
    jp.once_started=0;
    jp.once_done=0;
    if(jp.category){
        if(!jp.has_album)return -1;
        if(ksal_exists(jp.album)){
            uint16_t i,all=0;
            if(ksal_load(jp.album)<0)return -1;
            jp.album_native=0;
            jp.base_bpm=proj.tempo;
            jp.album_len=ksal.count;
            for(i=0;i<ksal.count;i++)
                if(ksal.e[i].kind==KSAL_S || ksal.e[i].kind==KSAL_M) all++;
            if(!all)return -1;
            jp.once_total=all;
            jp.once_started=1u;
            jp.once_remaining=(uint16_t)(all-1u);
            return jp_album_song(1);
        }
        /* Legacy native albums are read-only-compatible, S-only. */
        if(ks_arr_set_mode(2u)<0||ks_arr_load_album(jp.album)<0)return -1;
        jp.album_native=1;
        jp.album_len=ks_arr_album_count();jp.album_pos=0u;
        if(!jp.album_len)return -1;
        jp.once_total=jp.album_len;
        jp.once_started=1u;
        jp.once_remaining=(uint16_t)(jp.album_len-1u);
        return jp_prepare_s(ks_arr_album_get(0u));
    }
    if(!jp.has_song)return -1;
    if(jp.kind==1)return ksm2_prepare(jp.song);
    return jp_prepare_s(jp.song);
}
/* The SONG player may repeat; ALBUM ONCE is a finite pass, independent of
 * BPM/INSTRUMENT blocks and both S-grid and imported M2 completion paths. */
static int jp_album_has_next(void){
    if(jp.once_done && !jp.loop)return 0;
    if(!jp.category)return !!jp.loop;
    /* Two independent conditions must permit the next song. Even if one
     * counter/cursor is stale, ONCE cannot wrap after the declared total. */
    return jp.loop || (jp.once_remaining>0u && jp.once_started<jp.once_total);
}
static int jp_next_after_end(void){
    int rc;
    /* A hard final-song guard shared by M2 and native S/grid completion. */
    if(!jp_album_has_next())return -1;
    if(jp.category){
        if(!jp.album_native){
            rc=jp_album_song(0);
        } else {
            if(jp.album_pos+1u<jp.album_len)jp.album_pos++;
            else if(jp.loop && jp.album_len)jp.album_pos=0u;
            else return -1;
            rc=jp_prepare_s(ks_arr_album_get((uint8_t)jp.album_pos));
        }
        if(!rc){
            if(jp.once_started<65535u)jp.once_started++;
            if(!jp.loop && jp.once_remaining)jp.once_remaining--;
        }
        return rc;
    }
    return jp_prepare();
}

static void pm_refresh(void){ pm.revision++;ui.sig[1]=0u;ui.sig[2]=0u;ui.sig[3]=0u; }
/* v0.5.23: second SEQ press enters EDIT MODE: SONG / ALBUM.
 * Pattern list is a separate 4-row SONG grid; SELECT always moves horizontally
 * inside the highlighted (ALGO) row, and KNOB 2 only edits its numeric field. */
enum { SG_BROWSE, SG_ACTION, SG_MOVE, SG_ADD };
static struct {
    uint8_t active_menu, menu_row, stage, action, have_pattern;
    uint16_t column[4], move_from, move_target, pat;
    uint32_t revision;
} sg;
static void sg_refresh(void){sg.revision++;ui.sig[1]=ui.sig[2]=ui.sig[3]=0u;}
static void sg_pattern_pick(void){
    uint16_t p;
    if(ks_arr_pattern_next(511u,1,&p)==0){sg.pat=p;sg.have_pattern=1u;}
    else {sg.pat=0u;sg.have_pattern=0u;}
}
static void sg_open(void){
    unsigned tr;
    sg.active_menu=0;sg.stage=SG_BROWSE;sg.action=0;
    for(tr=0;tr<4u;tr++)sg.column[tr]=0u;
    sg_pattern_pick();sg_refresh();
}
static void sg_track_sync(void){
    unsigned tr=ks_selected_track;
    if(sg.column[tr]>ks_arr_grid_count((uint8_t)tr))
        sg.column[tr]=ks_arr_grid_count((uint8_t)tr);
    sg_refresh();
}
static void sg_scroll(int delta){
    unsigned tr=ks_selected_track,count=ks_arr_grid_count((uint8_t)tr);
    int col;
    if(sg.stage==SG_ACTION){sg.action=(uint8_t)clamp((int)sg.action+delta,0,1);sg_refresh();return;}
    if(sg.stage==SG_ADD)return;
    if(sg.stage==SG_MOVE){sg.move_target=(uint16_t)clamp((int)sg.move_target+delta,0,count?(int)count-1:0);sg_refresh();return;}
    col=clamp((int)sg.column[tr]+delta,0,(int)count);
    sg.column[tr]=(uint16_t)col;sg_refresh();
}
static void sg_enter(void){
    uint8_t tr=ks_selected_track;
    uint16_t at=sg.column[tr];int rc;
    if(eng_running()||ks_recording||kstep_paused){warn("SONG","STOP / REC OFF");return;}
    if(ks_arr_mode()!=1u){warn("SONG","MODE SONG FIRST");return;}
    if(sg.stage==SG_MOVE){
        rc=ks_arr_grid_move(tr,sg.move_from,sg.move_target);
        if(rc<0){warn("MOVE","FAILED");return;}
        sg.column[tr]=sg.move_target;sg.stage=SG_BROWSE;sg_refresh();return;
    }
    if(sg.stage==SG_ADD){
        rc=ks_arr_grid_put(tr,at,KSG_SILENT);
        if(rc<0){warn("SONG",rc==-2?"SONG DISK LIMIT":"SILENT FAILED");return;}
        sg.stage=SG_BROWSE;sg_refresh();return;
    }
    if(sg.stage==SG_ACTION){
        if(sg.action==0u){
            rc=ks_arr_grid_del(tr,at);
            if(rc<0){warn("DEL","FAILED");return;}
            if(sg.column[tr]>ks_arr_grid_count(tr))sg.column[tr]=ks_arr_grid_count(tr);
            sg.stage=SG_BROWSE;say("SONG","DELETED");
        }else{sg.move_from=at;sg.move_target=at;sg.stage=SG_MOVE;}
        sg_refresh();return;
    }
    if(at>=ks_arr_grid_count(tr)){sg.stage=SG_ADD;sg_refresh();return;}
    sg.stage=SG_ACTION;sg.action=0u;sg_refresh();
}
static void sg_pattern_change(int v){
    uint8_t tr=ks_selected_track;uint16_t next;int delta=v-((int)sg.pat+1),n,dir,i,rc;
    if(eng_running()||ks_recording||kstep_paused||ks_arr_mode()!=1u||sg.stage!=SG_BROWSE)return;
    if(!delta)return;
    n=delta<0?-delta:delta;if(n>32)n=32;dir=delta>0?1:-1;
    next=sg.have_pattern?sg.pat:(dir>0?511u:0u);
    for(i=0;i<n;i++){
        uint16_t target;
        if(ks_arr_pattern_next(next,dir,&target)<0)break;
        next=target;
    }
    if(ks_arr_pattern_exists(next)!=1)return;
    sg.pat=next;sg.have_pattern=1u;
    rc=ks_arr_grid_put(tr,sg.column[tr],sg.pat);
    if(rc<0){warn("SONG",rc==-2?"SONG DISK LIMIT":"ADD FAILED");return;}
    /* Cursor remains on the newly selected pattern; an extra + is now visible. */
    sg_refresh();
}

static void pm_load_pick(void){
    uint16_t id;
    /* The iterator excludes obsolete/invalid pattern versions, unlike raw exists. */
    uint16_t before=(uint16_t)((pm.pat+KS_PATTERNS-1u)%KS_PATTERNS);
    if(ks_arr_pattern_next(before,1,&id)==0){pm.pat=id;pm.have_pattern=1u;}
    else pm.have_pattern=0u;
}

/* One-row album arranger; SELECT moves columns, KNOB2 picks existing S/M.
 * SEL on + opens BPM/INSTRUMENT; SEL on a block opens DEL/MOVE. */
enum {AG_BROWSE,AG_MENU_ADD,AG_MENU_ACTION,AG_VALUE,AG_MOVE};
static struct {uint16_t col,from,to,song,album;uint8_t stage,menu,kind,has_song,value_kind;
    uint16_t value;uint32_t revision;} ag;
static void ag_refresh(void){ag.revision++;ui.sig[1]=ui.sig[2]=ui.sig[3]=0u;}
static void ag_first_song(void){
    uint16_t id;
    ag.has_song=0;
    if(ks_arr_song_next(63u,1,&id)==0){ag.kind=KSAL_S;ag.song=id;ag.has_song=1;}
    else if(ksm2_select(65535u,1,&id)==0){ag.kind=KSAL_M;ag.song=id;ag.has_song=1;}
}
static void ag_open(void){
    ag.stage=AG_BROWSE;ag.menu=0;ag.col=0;ag.album=ksal.album_id;
    ag_first_song();ag_refresh();
}
static void ag_song_pick(int d){
    int dir=d>0?1:-1,n=d>0?d:-d,i;uint16_t id;
    if(!d||ag.stage!=AG_BROWSE||eng_running()||kstep_paused||ks_recording)return;
    if(n>32)n=32;
    for(i=0;i<n;i++){
        if(!ag.has_song){ag_first_song();continue;}
        if(ag.kind==KSAL_S){
            if(ks_arr_song_next(ag.song,dir,&id)==0&&
               (dir>0?id>ag.song:id<ag.song)){ag.song=id;continue;}
            if(ksm2_select(dir>0?65535u:0u,dir,&id)==0){ag.kind=KSAL_M;ag.song=id;continue;}
        }else{
            if(ksm2_select(ag.song,dir,&id)==0&&
               (dir>0?id>ag.song:id<ag.song)){ag.song=id;continue;}
            if(ks_arr_song_next(dir>0?63u:0u,dir,&id)==0){ag.kind=KSAL_S;ag.song=id;continue;}
        }
    }
    if(ag.has_song){
        /* Like SONG EDIT: rotating KNOB2 on + materializes that cell, then + follows. */
        if(ag.col>=ksal.count){
            if(ksal_insert(ag.col,ag.kind,ag.song)<0){warn("ALBUM","ADD FAILED");return;}
            ag.col++; /* highlight the new trailing +, exactly like SONG EDIT */
        }else{
            ksal_entry_t it=ksal.e[ag.col];
            if(it.kind==KSAL_S || it.kind==KSAL_M){
                ksal.e[ag.col].kind=ag.kind;
                ksal.e[ag.col].value=ag.song;
                ksal.dirty=1u;
            }
        }
    }
    ag_refresh();
}
static void ag_scroll(int d){
    if(ag.stage==AG_MENU_ADD||ag.stage==AG_MENU_ACTION){ag.menu=(uint8_t)clamp((int)ag.menu+d,0,1);}
    else if(ag.stage==AG_MOVE){ag.to=(uint16_t)clamp((int)ag.to+d,0,ksal.count?(int)ksal.count-1:0);}
    else if(ag.stage==AG_BROWSE){ag.col=(uint16_t)clamp((int)ag.col+d,0,ksal.count);}
    ag_refresh();
}
static void ag_enter(void){
    int rc;
    if(eng_running()||ks_recording||kstep_paused){warn("ALBUM","STOP FIRST");return;}
    if(ag.stage==AG_VALUE){
        rc=ksal_insert(ag.col,ag.value_kind,ag.value);
        if(rc<0){warn("ALBUM","BLOCK FAILED");return;}
        ag.stage=AG_BROWSE;ag.col++;ag_refresh();return;
    }
    if(ag.stage==AG_MOVE){
        if(ksal_move(ag.from,ag.to)<0){warn("ALBUM","MOVE FAILED");return;}
        ag.col=ag.to;ag.stage=AG_BROWSE;ag_refresh();return;
    }
    if(ag.stage==AG_MENU_ADD){
        ag.value_kind=ag.menu==0?KSAL_BPM:KSAL_INST;
        ag.value=(uint16_t)(ag.menu==0?120:0);
        ag.stage=AG_VALUE;ag_refresh();return;
    }
    if(ag.stage==AG_MENU_ACTION){
        if(ag.menu==0){
            rc=ksal_delete(ag.col);
            if(rc<0){warn("ALBUM","DEL FAILED");return;}
            if(ag.col>ksal.count)ag.col=ksal.count;
            ag.stage=AG_BROWSE;
        }else{ag.from=ag.to=ag.col;ag.stage=AG_MOVE;}
        ag_refresh();return;
    }
    ag.menu=0;
    ag.stage=ag.col>=ksal.count?AG_MENU_ADD:AG_MENU_ACTION;
    ag_refresh();
}
static void pm_open(void){
    pm.stage=PM_MAIN;pm.row=0u;pm.track=ks_selected_track;
    pm.len=(uint8_t)(((unsigned)ks_arr_work_length(ks_selected_track)+15u)/16u*16u);
    pm.pat=ks_pattern_num;pm.have_pattern=0u;pm_refresh();
}
static void pm_scroll(int d){
    int n=pm.stage==PM_OVERWRITE?2:3;
    int v=(int)pm.row+d;
    if(v<0)v=0;
    if(v>=n)v=n-1;
    if(v==(int)pm.row)return;
    pm.row=(uint8_t)v;
    if(pm.stage==PM_MAIN){
        /* ALL belongs only to CLEAR; LOAD and SAVE need one target track. */
        if(pm.row!=0u && pm.track>=KS_TRACKS)pm.track=ks_selected_track;
        if(pm.row==1u)pm_load_pick();
    }
    pm_refresh();
}
static void ks_okp_release(void); /* defined below: ONE KEY note-off */
static void pm_enter(void){
    int rc;
    if(eng_running()||ks_recording||kstep_paused){warn("PATTERN","STOP / REC OFF");return;}
    if(ks_arr_mode()!=0u && ks_arr_set_mode(0u)<0){warn("PATTERN","MODE CHANGE ERROR");return;}
    if(pm.stage==PM_OVERWRITE){
        if(pm.row==0u){
            rc=ks_arr_work_save(pm.track,pm.pat,1);
            if(rc<0){warn("SAVE","WRITE FAILED");return;}
            ks_ed_sync_pattern();say("SAVE","OVERWRITTEN");
        }
        pm.stage=PM_MAIN;pm.row=2u;pm_refresh();return;
    }
    switch(pm.row){
    case 0: /* CLEAR: K1 = TR1..TR4/ALL; K4 = steps; no submenu. */
        if(pm.track==KS_TRACKS){
            uint8_t t;
            for(t=0u;t<KS_TRACKS;t++){
                rc=ks_arr_work_clear(t,pm.len);
                if(rc<0){warn("CLEAR","FAILED / STOP");return;}
            }
            /* Keep ALGO selection; the four independent TEMP buffers are empty. */
            ks_ed_sync_pattern();say("CLEAR","ALL UNSAVED");
        }else{
            rc=ks_arr_work_clear(pm.track,pm.len);
            if(rc<0){warn("CLEAR","FAILED / STOP");return;}
            ks_arr_set_track(pm.track);ks_ed_sync_pattern();
            say("CLEAR","UNSAVED");
        }
        break;
    case 1: /* LOAD: K1 = target track, K2 = existing pattern only. */
        if(!pm.have_pattern){warn("LOAD","NO SAVED PATTERNS");return;}
        ks_okp_release();
        rc=ks_arr_work_load(pm.track,pm.pat);
        if(rc<0){warn("LOAD","INVALID PATTERN");return;}
        ks_arr_set_track(pm.track);ks_ed_sync_pattern();say("LOAD","TRACK READY");
        break;
    case 2: /* SAVE: K1 = source track, K2 = destination ID. */
        rc=ks_arr_pattern_exists(pm.pat);
        if(rc<0){warn("SAVE","DISK READ ERROR");return;}
        if(rc>0){pm.stage=PM_OVERWRITE;pm.row=1u;/* NO is default */}
        else{
            rc=ks_arr_work_save(pm.track,pm.pat,0);
            if(rc<0){warn("SAVE","WRITE FAILED");return;}
            ks_ed_sync_pattern();say("SAVE","PATTERN STORED");
        }
        break;
    }
    pm_refresh();
}

/* v0.5.30: ONE TOUCH supports PATTERN, prepared S song and polyphonic M2.
 * 16 separate WHITE key owners; S/M seek reads happen on UI key edges only. */
#include "onekey_play.h"
static okp_t ks_okp;
static uint8_t ks_okp_ready,ks_okp_kind=255u,ks_okp_tr=255u;
static uint16_t ks_okp_pat=65535u,ks_okp_song=65535u;
static uint32_t ks_okp_revision=~0u;
static uint8_t ks_okp_length;
static int16_t ks_okp_lit_step=-1;
static int ks_okp_read(void *unused,uint32_t index,okp_note_t notes[OKP_MAX_CHORD])
{
    unsigned step,found=0,i;
    const uint8_t tr=ks_okp_tr;
    (void)unused;
    for(step=0;step<(unsigned)ks_arr_play_len(tr);step++){
        kse_step_t s;unsigned n;
        if(ks_arr_play_get(tr,(uint8_t)step,&s)!=0||!s.count)continue;
        if(found++!=index)continue;
        n=s.count;if(n>KSE_MAX_CHORD)n=KSE_MAX_CHORD;
        if(n>OKP_MAX_CHORD)n=OKP_MAX_CHORD;
        for(i=0;i<n;i++){
            notes[i].track=tr;notes[i].channel=tr;
            notes[i].pitch=s.note[i];notes[i].velocity=s.velocity[i]?s.velocity[i]:100u;
        }
        ks_okp_lit_step=(int16_t)step;
        return (int)n;
    }
    return 0;
}
static int ks_okp_seek(void *unused,uint32_t cursor,int valid,int direction,
                        uint32_t *next,okp_note_t notes[OKP_MAX_CHORD])
{
    (void)unused;
    if(ks_okp_kind==2u)
        return ksm2_onetouch_seek(cursor,valid,direction,next,notes);
    if(ks_okp_kind==1u)
        return ks_arr_onetouch_seek(cursor,valid,direction,next,notes);
    return -1;
}
static void ks_okp_emit(void *unused,const okp_note_t *n,int on)
{
    (void)unused;
    if(ks_okp_kind==2u){
        /* M2 channel is authoritative: never remap through an S track. */
        uint8_t st=(uint8_t)((on?0x90u:0x80u)|n->channel);
        uint32_t pkt=(st>>4)|((uint32_t)st<<8)|((uint32_t)n->pitch<<16)|
                      ((uint32_t)(on?n->velocity:0u)<<24);
        ctl_send(pkt,TO_USB|TO_BLE|TO_SYNTH); /* includes HOME 2 observer */
    }else{
        ks_arr_track_send(n->track,on?0x90u:0x80u,n->pitch,on?n->velocity:0u);
    }
}
static void ks_okp_ensure(void)
{
    if(!ks_okp_ready){
        okp_io_t io={ks_okp_read,ks_okp_emit,0,ks_okp_seek};
        okp_init(&ks_okp,&io);ks_okp_ready=1u;
    }
}
static void ks_okp_release(void)
{
    if(ks_okp_ready)okp_release_all(&ks_okp);
}
static void ks_okp_refresh(int force)
{
    uint8_t kind,tr=ks_selected_track,length=0u;
    uint16_t pat=65535u,song=65535u;
    unsigned step,count=0u;
    if(!kstep_paused)return;
    ks_okp_ensure();
    /* Prepared M2 has priority over stale S-mode state. */
    kind=ksm2_active()?2u:((ks_arr_mode()!=0u)?1u:0u);
    if(kind==0u){pat=ks_arr_assigned(tr);length=ks_arr_play_len(tr);}
    else if(kind==1u) song=ks_arr_song_id();
    else song=ksm2_playing_id();
    if(!force && ks_okp_kind==kind && ks_okp_song==song &&
       (kind!=0u || (ks_okp_tr==tr && ks_okp_pat==pat &&
                      ks_okp_revision==ks_ed_revision && ks_okp_length==length))){
        /* KNOB 4 toggles ONCE/LOOP without resetting the musical cursor. */
        if(kind!=0u)okp_set_seek_loop(&ks_okp,jp.loop);
        return;
    }
    /* Close all old note owners BEFORE switching routing source. */
    ks_okp_release();
    ks_okp_kind=kind;ks_okp_tr=tr;ks_okp_pat=pat;ks_okp_song=song;
    ks_okp_revision=ks_ed_revision;ks_okp_length=length;ks_okp_lit_step=-1;
    if(kind!=0u){
        okp_set_seek_source(&ks_okp);
        okp_set_seek_loop(&ks_okp,jp.loop); /* PLAYER K4: ONCE or LOOP */
        return;
    }
    for(step=0;step<(unsigned)length;step++){
        kse_step_t s;
        if(ks_arr_play_get(tr,(uint8_t)step,&s)==0&&s.count)count++;
    }
    okp_set_source(&ks_okp,count,1);
}
static void ks_okp_pause(int on)
{
    ks_okp_ensure();
    if(on){ks_okp_refresh(1);okp_set_paused(&ks_okp,1);}
    else okp_set_paused(&ks_okp,0);
}
static void ks_okp_key(int physical,int down)
{
    int white=key_white(physical);
    if(white<0||white>=16)return; /* black keys remain silent */
    /* Only PATTERN selection uses one selected track's mute/solo state. */
    if(down && ks_okp_kind==0u && !ks_track_audible(ks_selected_track))return;
    (void)okp_key(&ks_okp,white+1,down);
}
/* END ONE TOUCH v0.5.30 */

#define FX_HOLD_MS KSTEP_HOLD_MS
static int fx_cc(void)
{
    return ui.view == V_PLAY && (ui.btn >> B_FX & 1u) && kstep_fx_long;
}
static int view_knob(int i)
{
    if (fx_cc())
        return K_CC0 + i;
    if (ui.view == V_SETUP && ui.setup_pg == 3)
        return i == 0 ? K_PCEXT : i == 1 ? K_PCINT : i == 2 ? K_PCCHAN : K_NONE;
    if (ui.view == V_SETUP && ui.setup_pg == 2)   /* (a device only while Bluetooth runs) */
        return i == 0 && (proj.ble_off || plat_ble_state() != BLE_READY) ? K_NONE : BT_KNOB[i];
    if (ui.view == V_ARR) {
        static const int pat_knob[4]={K_ARRMENU,K_ARRPAT,K_ARRLEN,K_ARRCHAN};
        static const int play_knob[4]={K_ARRMENU,K_ARRPAT,K_ARRMODE,K_NONE};
        static const int song_knob[4]={K_SGSONG,K_SGPAT,K_NONE,K_NONE};
        static const int album_knob[4]={K_AGALBUM,K_AGSONG,K_NONE,K_AGVALUE};
        if(sg.active_menu)return K_NONE;
        if(ui.arr_pg==2u && sg.stage!=SG_BROWSE && i==1)return K_NONE;
        if(ui.arr_pg==3u && i==1 && ag.stage!=AG_BROWSE)return K_NONE;
        if(ui.arr_pg==3u && i==3 && ag.stage!=AG_VALUE)return K_NONE;
        if(ui.arr_pg==1u){
            if(i==0)return K_PMTRACK;          /* K1: TRACK 1..4 / CLEAR ALL */
            if(i==1 && (pm.stage==PM_OVERWRITE||pm.row!=0u))return K_PMPAT; /* K2: pattern */
            if(i==3 && pm.stage==PM_MAIN && pm.row==0u)return K_PMLEN;      /* K4: length */
            return K_NONE;
        }
        /* PATTERN knob readouts are always below the text menu. */
        return (ui.arr_pg==0u?play_knob:ui.arr_pg==1u?pat_knob:ui.arr_pg==2u?song_knob:album_knob)[i];
    }
    if(ui.view==V_LIVE)return K_LVOL1+i;
    if(ui.view==V_STEP && ks_ed.held_step>=0){
        static const int held[4]={K_ENOTE,K_EVELOCITY,K_EGATE,K_ECHANCE};
        return held[i];
    }
    return VIEW_KNOB[ui.view][i];
}

static track_t *trk(void) { return &proj.trk[ui.track]; }
static void track_name(int n, char *b);

static int knob_get(int k)
{
    if (k <= K_VAL3)
        return proj.val[proj.bank][k - K_VAL0];
    if (k <= K_CC3)
        return proj.cc[proj.bank][k - K_CC0];
    switch (k) {
    case K_LVOL1: case K_LVOL2: case K_LVOL3: case K_LVOL4:
        return ks_arr_live_volume_get((uint8_t)(k-K_LVOL1));
    /* KSTEP_SAMPLE_ONLY_ARRANGER_V040 knob values: 4 MIDI tracks / 16 patterns / 1..8 blocks / channels. */
    case K_JPSLOT: return jp.category?(jp.has_album?(int)jp.album+1:0):(jp.has_song?(int)jp.song+1:0);
    case K_JPLOOP: return (int)jp.loop;
    case K_AGALBUM: return (int)ksal.album_id+1;
    case K_AGSONG: return ag.kind==KSAL_M?(int)ag.song+65:(int)ag.song+1;
    case K_AGVALUE: return ag.value_kind==KSAL_INST?(int)ag.value+1:(int)ag.value;
    case K_ARRMENU: return (int)ui.arr_pg+1;
    case K_SGSONG: return (int)ks_arr_song_id()+1;
    case K_SGPAT: return sg.have_pattern?(int)sg.pat+1:0;
    case K_PMPAT: return (int)pm.pat+1;
    case K_PMLEN: return (int)pm.len/16;
    case K_PMTRACK: return (int)pm.track+1;
    case K_ENOTE: case K_EVELOCITY: {
        kse_step_t s;
        if(ks_ed.held_step>=0 && !ks_arr_editor_get(ks_selected_track,(uint8_t)ks_ed.held_step,&s) && s.count)
            return k==K_ENOTE?s.note[0]:s.velocity[0];
        return k==K_ENOTE?ks_ed.chord[0]:ks_ed.chord_vel[0];
    }
    case K_EPAT: return (int)ks_pattern_num+1;
    case K_ELEN: return ks_arr_len();
    case K_EGATE: {
        kse_step_t s;
        if(ks_ed.held_step>=0 && !ks_arr_editor_get(ks_selected_track,(uint8_t)ks_ed.held_step,&s) && s.count)return s.gate;
        return ks_ed.default_gate;
    }
    case K_ECHANCE: {
        kse_step_t s;
        if(ks_ed.held_step>=0 && !ks_arr_editor_get(ks_selected_track,(uint8_t)ks_ed.held_step,&s) && s.count)return s.chance;
        return ks_ed.default_chance;
    }
    case K_ARRTRACK: return (int)ks_selected_track + 1;
    case K_ARRPAT: return ui.arr_pg==2u?(int)ks_arr_song_id()+1:ui.arr_pg==0u?(ks_arr_mode()==0u?(int)ks_arr_assigned(ks_selected_track)+1:ks_arr_mode()==1u?(int)ks_arr_song_id()+1:(int)ks_arr_album_id()+1):(int)ks_pattern_num+1;
    case K_ARRMODE: return (int)ks_arr_mode();
    case K_ASLOT: return (int)ks_album_slot+1;
    case K_ASONG: return (int)ks_arr_album_get(ks_album_slot)+1;
    case K_ACOUNT: return ks_arr_album_count();
    case K_SOVERLAY: return ks_arr_song_overlay_get(ks_selected_track);
    case K_ARRLEN: return (int)ks_arr_blocks();
    case K_ARRCHAN: return (int)ks_arr_channel() + 1;
    case K_HOMECHAN: return (int)ks_arr_channel() + 1;
    case K_HOMEMODE: return ks_track_mode_get(ks_selected_track);
    case K_HOMESUST: return ui.sustain ? 1 : 0;
    case K_ROOT: return proj.root;
    case K_SCALE: return proj.scale;
    case K_VEL: return proj.vel;
    case K_HUMAN: return proj.human;
    case K_AMODE: return proj.arp_mode;
    case K_ARATE: return proj.arp_rate;
    case K_AOCT: return proj.arp_oct;
    case K_ARP: return proj.arp_on ? 1 + proj.arp_latch : 0;
    case K_STEPS: return trk()->steps;
    case K_HITS: return trk()->hits;
    case K_ROT: return trk()->rot;
    case K_NOTE: return trk()->note;
    case K_CLOCK: return proj.clock;
    case K_SEQCH: return proj.seq_ch;
    case K_MASTER: return proj.master_cc;
    case K_GLOW: return proj.glow;
    case K_TEMPO: return proj.tempo;
    case K_SOUND: return proj.sound != SOUND_OFF;
    case K_SNDIN: return !proj.in_mute;
    case K_TOUCH: return proj.touch;
    case K_CURVE: return proj.vel_curve;
    case K_ATM: return proj.at_mode;
    case K_ATS: return proj.at_sens;
    case K_DEV: return btdev_pick();
    case K_BT: return !proj.ble_off;
    case K_USBMODE: return usb_host_requested;
    case K_PCEXT: return ks_pc_out_external();
    case K_PCINT: return ks_pc_out_internal();
    case K_PCCHAN: return (int)ks_pc_player_channel()+1;
    default: return 0;
    }
}
static void knob_range(int k, int *lo, int *hi)
{
    *lo = 0;
    switch (k) {
    case K_LVOL1: case K_LVOL2: case K_LVOL3: case K_LVOL4:
        *lo=0;*hi=100;break;
    case K_JPSLOT: *lo=0;*hi=65535;break;
    case K_JPLOOP: *lo=0;*hi=1;break;
    case K_AGALBUM: *lo=1;*hi=32;break;
    case K_AGSONG: *lo=1;*hi=65535;break;
    case K_AGVALUE: *lo=ag.value_kind==KSAL_BPM?40:1;*hi=ag.value_kind==KSAL_BPM?240:128;break;
    case K_ARRMENU: *lo=1; *hi=4; break;
    case K_SGSONG: *lo=1; *hi=64; break;
    case K_SGPAT: *lo=0; *hi=KS_PATTERNS; break;
    case K_PMPAT: *lo=1; *hi=KS_PATTERNS; break;
    case K_PMLEN: *lo=1; *hi=8; break;  /* displayed as 16..128 steps */
    case K_PMTRACK: *lo=1; *hi=pm.stage==PM_MAIN&&pm.row==0u?5:4; break;
    case K_ENOTE: *hi=127; break;
    case K_EVELOCITY: *lo=1; *hi=127; break;
    case K_EPAT: *lo=1; *hi=512; break;
    case K_ELEN: *lo=8; *hi=128; break;
    case K_EGATE: *lo=1; *hi=100; break;
    case K_ECHANCE: *lo=0; *hi=100; break;
    case K_ARRTRACK: *lo=1; *hi=4; break;
    case K_ARRPAT: *lo=1;*hi=ui.arr_pg==2u?64:ui.arr_pg==0u?(ks_arr_mode()==0u?KS_PATTERNS:ks_arr_mode()==1u?64:32):KS_PATTERNS;break;
    case K_ASLOT: *lo=1;*hi=32;break;
    case K_ASONG: *lo=1;*hi=64;break;
    case K_ACOUNT: *lo=0;*hi=32;break;
    case K_SOVERLAY: *lo=0;*hi=1;break;
    case K_ARRMODE: *lo=0;*hi=2;break;
    case K_ARRLEN: *lo=1; *hi=8; break;
    case K_PCCHAN: *lo=1; *hi=17; break;
    case K_ARRCHAN: *lo=1; *hi=17; break;
    case K_HOMECHAN: *lo=1; *hi=17; break;
    case K_HOMEMODE: *hi=2; break;
    case K_HOMESUST: *hi=1; break;
    case K_ROOT: *hi = 11; break;
    case K_SCALE: *hi = NSCALES - 1; break;
    case K_VEL: *lo = 1; *hi = 127; break;
    case K_HUMAN: *hi = 40; break;
    case K_AMODE: *hi = NARP - 1; break;
    case K_ARATE: *hi = NRATE - 1; break;
    case K_AOCT: *lo = 1; *hi = 3; break;
    case K_SOUND: case K_SNDIN: case K_TOUCH: case K_BT: case K_USBMODE: case K_PCEXT: case K_PCINT: *hi = 1; break;
    case K_DEV: *hi = btdev_count() > 1 ? btdev_count() - 1 : 0; break;
    case K_CURVE: case K_ATS: *lo = -5; *hi = 5; break;
    case K_ATM: *hi = NAT - 1; break;
    case K_TEMPO: *lo = 40; *hi = 240; break;
    case K_ARP: *hi = 2; break;
    case K_STEPS: *lo = 1; *hi = 16; break;
    case K_HITS: *hi = trk()->steps; break;
    case K_ROT: *hi = trk()->steps - 1; break;
    case K_CLOCK: *hi = NCLK - 1; break;
    case K_SEQCH: *hi = 15; break;
    case K_GLOW: *hi = 2; break;
    case K_NOTE: case K_VAL0: case K_VAL1: case K_VAL2: case K_VAL3: *hi = 127; break;
    default: *hi = 119; break;           /* controller numbers, MASTER */
    }
}
static void knob_name(int k, char *b)
{
    static const char *const N[] = {"Root", "Scale", "Velocity", "Humanize", "Mode", "Rate", "Octaves", "Arp",
                                    "Steps", "Hits", "Rotate", "Note", "Clock", "Seq ch", "MASTER", "Glow",
                                    "Tempo", "Sound", "MIDI in", "Velocity", "Curve", "Aftertouch", "AT sens", "Device",
                                    "Bluetooth", "USB mode"};
    if (k <= K_VAL3)
        put_u(put_s(b, "CC "), proj.cc[proj.bank][k - K_VAL0]);
    else if (k <= K_CC3)
        put_u(put_s(b, "Knob "), (uint32_t)(k - K_CC0 + 1));
    else if(k>=K_LVOL1&&k<=K_LVOL4)
        put_s(put_u(put_s(b,"TR"),(uint32_t)(k-K_LVOL1+1u))," VOL");
    else if (k == K_JPSLOT) put_s(b, jp.category?"ALBUM":"SONG");
    else if (k == K_JPLOOP) put_s(b, "PLAY");
    else if (k == K_AGALBUM) put_s(b,"ALBUM");
    else if (k == K_AGSONG) put_s(b,"SONG");
    else if (k == K_AGVALUE) put_s(b,ag.value_kind==KSAL_BPM?"BPM":"INSTRUMENT");
    else if (k == K_ARRMENU) put_s(b, "MENU");
    else if (k == K_SGSONG) put_s(b, "SONG");
    else if (k == K_SGPAT) put_s(b, "PATTERN");
    else if (k == K_PMPAT) put_s(b, "PATTERN");
    else if (k == K_PMLEN) put_s(b, "LENGTH");
    else if (k == K_PMTRACK) put_s(b, "TRACK");
    else if (k == K_ENOTE) put_s(b, "NOTE");
    else if (k == K_EVELOCITY) put_s(b, "VELO");
    else if (k == K_EPAT) put_s(b, "PATTERN");
    else if (k == K_ELEN) put_s(b, "LENGTH");
    else if (k == K_EGATE) put_s(b, "GATE");
    else if (k == K_ECHANCE) put_s(b, "CHANCE");
    else if (k == K_ARRTRACK) put_s(b, "TRACK");
    else if (k == K_ARRPAT) put_s(b,"PATTERN");
    else if (k == K_ARRMODE) put_s(b,"MODE");
    else if (k == K_ASLOT) put_s(b,"ALB SLOT");
    else if (k == K_ASONG) put_s(b,"SONG ID");
    else if (k == K_ACOUNT) put_s(b,"SONGS");
    else if (k == K_SOVERLAY) put_s(b,"PAT LAYER");
    else if (k == K_ARRLEN) put_s(b, "LENGTH");
    else if (k == K_ARRCHAN) put_s(b, "CHANNEL");
    else if (k == K_PCEXT) put_s(b, "PC EXT");
    else if (k == K_PCINT) put_s(b, "PC INT");
    else if (k == K_PCCHAN) put_s(b, "SONG CH");
    else if (k == K_HOMECHAN) put_s(b, "MIDI CH");
    else if (k == K_HOMEMODE) put_s(b, "TRACK");
    else if (k == K_HOMESUST) put_s(b, "SUST");
    else if (k < K_NONE)
        put_s(b, N[k - K_ROOT]);
    else
        b[0] = 0;
}
static void knob_text(int k, char *b)
{
    int v = knob_get(k);
    switch (k) {
    case K_LVOL1: case K_LVOL2: case K_LVOL3: case K_LVOL4:
        put_s(put_u(b,(uint32_t)v),"%");break;
    case K_SGPAT: if(!sg.have_pattern)put_s(b,"--");else put_u(b,(uint32_t)v);break;
    case K_SGSONG: put_u(b,(uint32_t)v);break;
    case K_JPSLOT: if(!v)put_s(b,"--");else put_u(b,(uint32_t)v);break;
    case K_JPLOOP: put_s(b,v?"LOOP":"ONCE");break;
    case K_AGALBUM: put_u(b,(uint32_t)v);break;
    case K_AGSONG:
        if(!ag.has_song)put_s(b,"--");
        else put_u(put_s(b,ag.kind==KSAL_S?"S":"M"),ag.kind==KSAL_S?ag.song+1u:ag.song);
        break;
    case K_AGVALUE:put_u(b,(uint32_t)v);break;
    case K_PMPAT:
        if(ui.view==V_ARR && ui.arr_pg==1u && pm.stage==PM_MAIN && pm.row==1u && !pm.have_pattern)
            put_s(b,"--");
        else put_u(b,(uint32_t)v); /* Numeric only: NO P prefix. */
        break;
    case K_PMLEN: put_u(b,(uint32_t)v*16u); break;
    case K_PMTRACK: if(v==5)put_s(b,"ALL");else put_u(b,(uint32_t)v);break;
    case K_ENOTE: put_note(b,v); break;
    case K_EVELOCITY: put_u(b,(uint32_t)v); break;
    case K_ARRLEN: put_u(b, (uint32_t)v*16u); break;
    case K_ARRMENU: case K_EPAT: case K_ELEN: case K_EGATE: case K_ECHANCE: put_u(b,(uint32_t)v); break;
    case K_ARRTRACK: case K_ARRPAT: case K_ARRMODE: case K_ASLOT: case K_ASONG: case K_ACOUNT: case K_SOVERLAY: put_u(b,(uint32_t)v); break;
    case K_ARRCHAN: case K_HOMECHAN:
        if(v==17)put_s(b,"ALL");else put_u(b,(uint32_t)v);
        break;
    case K_HOMEMODE: put_s(b,v==1?"SOLO":v==2?"MUTE":"-");break;
    case K_HOMESUST: put_s(b,v?"ON":"OFF");break;
    case K_ROOT: put_s(b, NOTE_NAME[v]); break;
    case K_SCALE: put_s(b, SCALE_NAME[v]); break;
    case K_AMODE: put_s(b, ARP_NAME[v]); break;
    case K_ARATE: put_s(b, RATE_NAME[v]); break;
    case K_ARP: put_s(b, v == 2 ? "Latch" : v ? "On" : "Off"); break;
    case K_NOTE: track_name(v, b); break;
    case K_CLOCK: put_s(b, CLOCK_NAME[v]); break;
    case K_SEQCH: put_u(b, (uint32_t)v + 1u); break;
    case K_MASTER:
        if (v)
            put_u(put_s(b, "CC "), (uint32_t)v);
        else
            put_s(b, "Volume");
        break;
    case K_GLOW: put_s(b, GLOW_NAME[v]); break;
    case K_SOUND: put_s(b, v ? "On" : "Off"); break;
    case K_SNDIN: put_s(b, v ? "Plays" : "Ignored"); break;
    case K_TOUCH: put_s(b, v ? "Keys" : "Fixed"); break;
    case K_BT: put_s(b, v ? "On" : "Off"); break;
    case K_PCEXT: case K_PCINT: put_s(b, v ? "ON" : "OFF"); break;
    case K_PCCHAN: if(v==17)put_s(b,"ALL");else put_u(b,(uint32_t)v);break;
    case K_USBMODE: put_s(b, v ? (plat_usb_host_state() == USBH_CONNECTED ? "MIDI" :
                    usb_host_reason == 1 ? "CHECK" :
                    usb_host_reason == 2 ? "LOW" :
                    usb_host_reason == 3 ? "LOST" : "WAIT") : "CLIENT"); break;
    case K_DEV:
        if (btdev_busy())
            put_s(b, btdev_name());      /* the one connected, or being connected */
        else if (!btdev_found(v, b, 0))
            put_s(b, plat_ble_device() == BLE_DEV_SCANNING ? "Looking" : "None");
        b[10] = 0;                       /* (the knob row's width) */
        break;
    case K_ATM: put_s(b, AT_NAME[v]); break;
    case K_CURVE: case K_ATS:
        if (v > 0)
            put_u(put_s(b, "+"), (uint32_t)v);
        else if (v < 0)
            put_u(put_s(b, "-"), (uint32_t)-v);
        else
            put_s(b, "0");
        break;
    case K_TEMPO:
        if (proj.clock == CLK_EXT)
            put_s(b, "Ext");
        else
            put_u(b, (uint32_t)v);
        break;
    case K_CC0: case K_CC1: case K_CC2: case K_CC3: put_u(put_s(b, "#"), (uint32_t)v); break;
    default: put_u(b, (uint32_t)v); break;
    }
}

static void track_regen(void)
{
    track_t *t = trk();
    if (t->hits > t->steps)
        t->hits = t->steps;
    if (t->rot >= t->steps)
        t->rot = (uint8_t)(t->steps - 1u);
    t->pat = euclid(t->steps, t->hits, t->rot);
}

/* SEQ navigation: a page is not a selection row, and ALGO selects a row/track.
 * Changing SEQ pages may request a SONG/ALBUM backend mode but is never
 * mistaken for proof that the backend successfully switched. */
static void ks_arr_page_to(int page)
{
    int requested=page;
    if(requested<0)requested=0;
    if(requested>3)requested=3;
    /* A visible SONG editor MUST match the actual backend mode.  Never show
     * a SONG/ALBUM menu over a failed mode transition. */
    if(requested==1 && ks_arr_mode()!=0u){
        if(ks_arr_set_mode(0u)<0){warn("MODE","STOP / SAVE FIRST");return;}
    }else if(requested>=2 && ks_arr_mode()!=(uint8_t)(requested-1)){
        if(ks_arr_set_mode((uint8_t)(requested-1))<0){
            warn("MODE","STOP / SAVE FIRST");return;
        }
    }
    ui.arr_pg=(uint8_t)requested;
    if(ui.arr_pg==1u)pm_open();
    else if(ui.arr_pg==2u)sg_open();
    else if(ui.arr_pg==3u){
        if(!ksal.dirty && ksal_load(ksal.album_id)<0)warn("ALBUM","DISK ERROR");
        ag_open();
    }
    ui.arr_row=ui.arr_pg==3u?(uint8_t)(ks_album_slot%4u):ks_selected_track;
    ui.sig[1]=ui.sig[2]=0u;
    ks_ed_revision++;
}
static void ks_arr_page_spin(int delta)
{
    int page = ((int)ui.arr_pg + delta) % 4;
    if (page < 0) page += 4;
    ks_arr_page_to(page);
}

static void knob_set(int k, int v)
{
    int lo, hi;
    knob_range(k, &lo, &hi);
    v = v < lo ? lo : v > hi ? hi : v;
    if (v == knob_get(k))
        return;
    if (k <= K_VAL3) {
        proj.val[proj.bank][k - K_VAL0] = (uint8_t)v;
        cc(proj.cc[proj.bank][k - K_VAL0], (uint32_t)v);
    } else if (k <= K_CC3) {
        proj.cc[proj.bank][k - K_CC0] = (uint8_t)v;
    } else {
        switch (k) {
        case K_LVOL1: case K_LVOL2: case K_LVOL3: case K_LVOL4:
            ks_arr_live_volume_set((uint8_t)(k-K_LVOL1),(uint8_t)v);
            ui.sig[2]=0u; /* value + fill-bar update, unchanged menu text */
            return;
        case K_JPSLOT:
            if(ui.view==V_JUKE)jp_move(v-knob_get(K_JPSLOT));
            return;
        case K_AGALBUM:
            if(ui.view==V_ARR && ui.arr_pg==3u){
                if(ksal.dirty){warn("ALBUM","SAVE FIRST");return;}
                if(ksal_load((uint16_t)(v-1))<0){warn("ALBUM","LOAD FAILED");return;}
                ag_open();
            }
            return;
        case K_AGSONG:
            if(ui.view==V_ARR && ui.arr_pg==3u)ag_song_pick(v-knob_get(K_AGSONG));
            return;
        case K_AGVALUE:
            if(ui.view==V_ARR && ui.arr_pg==3u && ag.stage==AG_VALUE){
                ag.value=(uint16_t)(ag.value_kind==KSAL_INST?v-1:v);ag_refresh();
            }
            return;
        case K_JPLOOP:
            if(ui.view==V_JUKE){
                jp.loop=(uint8_t)!!v;
                /* Switching LOOP -> ONCE during playback means finish THIS
                 * album pass, never wrap to the first song again. */
                if(!jp.loop && jp.category && jp.running){
                    jp.once_remaining=jp_album_songs_after(jp.album_pos);
                    jp.once_total=(uint16_t)(jp.once_started+jp.once_remaining);
                    jp.once_done=0u;
                }
                /* SONG grid has a separate EDITOR repeat flag. In PLAYER, even
                 * an ALBUM LOOP must advance songs rather than re-loop a grid.
                 * Ensure an editor's stale flag cannot ignore K4. */
                if(jp.running)ks_arr_grid_set_loop(0);
                /* Updating LOOP during PAUSE must retain cursor and held notes. */
                if(kstep_paused && ks_okp_ready && ks_okp_kind!=0u)
                    okp_set_seek_loop(&ks_okp,jp.loop);
                jp_refresh();
            }
            return;
        case K_ARRMENU:
            ks_arr_page_to(v-1);
            return;
        case K_SGSONG:
            if(ui.view==V_ARR && ui.arr_pg==2u && !sg.active_menu){
                if(ks_arr_mode()!=1u||ks_arr_load_song((uint16_t)(v-1))<0){
                    warn("SONG",ks_arr_grid_dirty()?"SAVE FIRST":"LOAD FAILED / STOP");return;
                }
                sg_open();say("SONG","LOADED");
            }
            return;
        case K_SGPAT:
            if(ui.view==V_ARR && ui.arr_pg==2u && !sg.active_menu)sg_pattern_change(v);
            return;
        case K_PMTRACK:
            if(ui.view==V_ARR && ui.arr_pg==1u && pm.stage==PM_MAIN){
                pm.track=(uint8_t)(v-1);
                pm_refresh();
            }
            return;
        case K_PMPAT:
            if(ui.view==V_ARR && ui.arr_pg==1u && pm.stage==PM_MAIN){
                if(pm.row==1u){
                    /* K2 LOAD: only stored, valid V3 patterns; skip empty IDs. */
                    uint16_t next=pm.have_pattern?pm.pat:511u;
                    int d=v-((int)pm.pat+1),dir=d>0?1:-1,j,n=d>0?d:-d;
                    if(n>32)n=32;
                    for(j=0;j<n;j++)if(ks_arr_pattern_next(next,dir,&next)<0)break;
                    if(ks_arr_pattern_exists(next)>0){pm.pat=next;pm.have_pattern=1u;}
                }else if(pm.row==2u){
                    /* K2 SAVE: full range, overwrite confirmation on SEL. */
                    pm.pat=(uint16_t)(v-1);
                }
                pm_refresh();
            }
            return;
        case K_PMLEN:
            if(ui.view==V_ARR && ui.arr_pg==1u && pm.stage==PM_MAIN && pm.row==0u){
                pm.len=(uint8_t)(v*16);
                pm_refresh();
            }
            return;
        case K_ENOTE: case K_EVELOCITY: {
            int rc=k==K_ENOTE?kse_edit_held_note(&ks_ed,0,(unsigned)v):
                             kse_edit_held_velocity(&ks_ed,0,(unsigned)v);
            if(!rc){
                int j;for(j=0;j<NKEYS;++j)if(ks_ed_key_t[j])ks_ed_key_changed[j]=1;
                ks_ed_revision++;
            }else warn("STEP",ks_recording?"REC OFF":"EDIT FAILED");
            return;
        }
        case K_EPAT:
            ks_okp_release();
            if(ks_arr_load((uint16_t)(v-1))<0)warn("DISK","SAVE FIRST");
            else ks_ed_sync_pattern();
            return;
        case K_ELEN:
            (void)kse_set_length(&ks_ed,(unsigned)v);return;
        case K_EGATE:
            if(ks_ed.held_step>=0){
                if(kse_edit_held_gate(&ks_ed,(unsigned)v)==0){
                    int i;for(i=0;i<NKEYS;i++)if(ks_ed_key_t[i])ks_ed_key_changed[i]=1;
                    ks_ed_revision++;
                }
            }else{ks_ed.default_gate=(uint8_t)v;ks_ed_revision++;}
            return;
        case K_ECHANCE:
            if(ks_ed.held_step>=0){
                if(kse_edit_held_chance(&ks_ed,(unsigned)v)==0){
                    int i;for(i=0;i<NKEYS;i++)if(ks_ed_key_t[i])ks_ed_key_changed[i]=1;
                    ks_ed_revision++;
                }
            }else{ks_ed.default_chance=(uint8_t)v;ks_ed_revision++;}
            return;
        case K_ARRTRACK: ks_okp_release();ks_arr_set_track((uint8_t)(v-1)); return;
        case K_SOVERLAY:
            if(ks_arr_song_overlay_set(ks_selected_track,v)<0)
                warn("SONG", "STOP / SONG MODE");
            return;
        case K_ASLOT: ks_album_slot=(uint8_t)(v-1);return;
        case K_ASONG:
            if(ks_arr_mode()!=2u || ks_arr_album_set(ks_album_slot,(uint8_t)(v-1))<0)
                warn("ALBUM","SELECT ALBUM / STOP");
            return;
        case K_ACOUNT:
            if(ks_arr_mode()!=2u || ks_arr_album_length((uint8_t)v)<0)
                warn("ALBUM","SELECT ALBUM / STOP");
            return;
        case K_ARRPAT:
            if(ui.arr_pg==2u){
                if(ks_arr_mode()!=1u || ks_arr_load_song((uint16_t)(v-1))<0)
                    warn("SONG","MODE SONG / SAVE FIRST");
                return;
            }
            if(ui.arr_pg==0u){
                int rc;
                if(eng_running()||kstep_paused){warn("PATTERN","STOP FIRST");return;}
                if(ks_dirty){warn("PATTERN","SAVE FIRST");return;}
                ks_okp_release();
                rc=ks_arr_mode()==0u ? ks_arr_assign(ks_selected_track,(uint16_t)(v-1)) : ks_arr_mode()==1u ? ks_arr_load_song((uint16_t)(v-1)) : ks_arr_load_album((uint16_t)(v-1));
                if(rc<0)warn("PATTERN",rc==-3?"DISK PROTECTED":"LOAD ERROR");
                else if(ks_arr_mode()==0u){
                    if(ks_arr_load((uint16_t)(v-1))==0){
                        ks_ed_sync_pattern();
                        say("PATTERN","TRACK INDEPENDENT");
                    }else warn("EDITOR","LOAD FAILED");
                }else say("PLAYER SOURCE","SELECTED");
                ui.sig[1]=0u;
                return;
            }
            ks_okp_release();
            if (ks_arr_load((uint16_t)(v-1)) < 0) warn("DISK", "SAVE FIRST / PROTECT");
            else ks_ed_sync_pattern();
            return;
        case K_ARRMODE:
            if(eng_running()||kstep_paused){warn("MODE","STOP FIRST");return;}
            ks_okp_release();
            if(ks_arr_set_mode((uint8_t)v)<0)warn("MODE","SAVE FIRST / DISK");
            else say("PLAYER",v==0?"PATTERN":v==1?"SONG":"ALBUM");
            ui.sig[1]=0u;return;
        case K_ARRLEN: ks_arr_set_length((uint8_t)(v*16)); return;
        case K_ARRCHAN: case K_HOMECHAN:
            ks_okp_release();
            ks_arr_set_channel((uint8_t)(v-1));return;
        case K_HOMESUST:
            ks_sustain_apply(v);
            say("SUSTAIN", v ? "ON" : "OFF");
            return; /* momentary performance setting: not stored in flash */
        case K_HOMEMODE:
            ks_okp_release();
            all_keys_up(); /* prevent a held live keyboard note being orphaned */
            ks_track_mode_set(ks_selected_track,(uint8_t)v);
            ks_arr_silence_inaudible();
            mark_dirty(); /* project rsv[6] saved by existing project SAVE */
            return;
        case K_ROOT: all_keys_up(); proj.root = (uint8_t)v; break;
        case K_SCALE: all_keys_up(); proj.scale = (uint8_t)v; break;
        case K_VEL: proj.vel = (uint8_t)v; break;
        case K_HUMAN: proj.human = (uint8_t)v; break;
        case K_AMODE: proj.arp_mode = (uint8_t)v; break;
        case K_ARATE: proj.arp_rate = (uint8_t)v; break;
        case K_AOCT: proj.arp_oct = (uint8_t)v; break;
        case K_ARP:
            if (!v != !proj.arp_on) {
                all_keys_up();
                arp_set(v != 0);
            }
            if (v) {
                proj.arp_latch = (uint8_t)(v == 2);
                arp_latch_changed();
            }
            break;
        case K_STEPS: trk()->steps = (uint8_t)v; track_regen(); break;
        case K_HITS: trk()->hits = (uint8_t)v; track_regen(); break;
        case K_ROT: trk()->rot = (uint8_t)v; track_regen(); break;
        case K_NOTE: trk()->note = (uint8_t)v; break;
        case K_CLOCK: proj.clock = (uint8_t)v; break;
        case K_SEQCH: engine_panic(); proj.seq_ch = (uint8_t)v; break;
        case K_MASTER: proj.master_cc = (uint8_t)v; ui.master_sent = -1; break;
        case K_GLOW: proj.glow = (uint8_t)v; break;
        case K_SOUND:
            if (!v)
                gm_panic();
            proj.sound = (uint8_t)(v ? SOUND_IN : SOUND_OFF);
            break;
        case K_SNDIN:
            if (!v)
                gm_panic();              /* notes from MIDI in may be sounding */
            proj.in_mute = (uint8_t)!v;
            break;
        case K_TEMPO:
            if (proj.clock == CLK_EXT)
                return;                  /* following a clock from outside */
            proj.tempo = (uint8_t)v;
            if(ui.playing && jp.running && proj.clock!=CLK_EXT){
                jp.manual_set=1u;jp.manual_bpm=(uint8_t)v;
                if(ksm2_active())ksm2_override_tempo((uint8_t)v);
            }
            break;
        case K_TOUCH: all_keys_up(); proj.touch = (uint8_t)v; touch_apply(); break;
        case K_ATM: at_off(); proj.at_mode = (uint8_t)v; touch_apply(); break;
        case K_CURVE: proj.vel_curve = (int8_t)v; touch_apply(); break;
        case K_ATS: proj.at_sens = (int8_t)v; break;
        case K_USBMODE:
            /* Send all-notes-off before USB changes its role. */
            panic();
            ks_okp_release();
            usb_host_requested = (uint8_t)v;
            usb_host_reason = 0;
            plat_usb_host(v);
            say("USB", v ? "HOST REQUESTED" : "DEVICE REQUESTED");
            return;  /* Do NOT save as a project setting. */
        case K_PCEXT: ks_pc_out_set(1u,(uint8_t)v); break;
        case K_PCINT: ks_pc_out_set(0u,(uint8_t)v); break;
        case K_PCCHAN: ks_pc_player_channel_set((uint8_t)(v-1)); break;
        case K_DEV: btdev_choose(v); return;      /* (a choice, not a setting) */
        case K_BT:
            proj.ble_off = (uint8_t)!v;
            plat_ble_enable(v);
            say("BLUETOOTH", v ? "ON" : "OFF");
            break;
        default: return;
        }
    }
    mark_dirty();
}

/* knob acceleration by speed: a slow turn is one step a detent, a quick one up to 6 */
#ifdef PLAT_HOST
#define accel_off plat_host_exact      /* the host simulator's "spin": exact steps */
#else
#define accel_off 0
#endif
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), m;
    ui.enc_t[role] = now;
    if (range <= 24 || !a || accel_off)
        return s;
    if (a > 1)
        dt /= a;
    m = dt < 15u ? 6u : dt < 30u ? 4u : dt < 60u ? 2u : 1u;
    return s * (int32_t)m;
}

/* ------------------------------------------------------------ buttons --- */
static void set_view(int v)
{
    if (v == V_SEQ || ui.view == V_SEQ)
        all_keys_up();                   /* the keys change meaning */
    ui.view = (uint8_t)v;
    ui.touched = -1;
}

static void octave(int d)
{
    char t[8];
    int o = proj.octave + d;
    if (o < -3 || o > 3)
        return;
    proj.octave = (int8_t)o;
    put_note(t, proj.scale ? SCALE0_NOTE + proj.root + 12 * o : KEY0_NOTE + 12 * o);
    say("LOWEST KEY", t);
    mark_dirty();
}

#define ENV_TAP_MS 300u                  /* ENV: two presses this close lock sustain */
#define SAVE_HOLD_MS 1000u               /* SAVE held this long on Setup: Bluetooth by hand */
static void bank_to(int b)                     /* the knob bank, with a word on the top bar */
{
    char t[2] = {(char)('A' + b), 0};
    proj.bank = (uint8_t)b;
    say_as("KNOB BANK", t, 0);                  /* a screen reader hears the page: the bank, its knobs */
    ui.touched = -1;
    mark_dirty();
}

/* LFO live key matrix: 11 black bank keys and 16 white pattern keys.
 * A missing saved pattern is never silently treated as an empty file. */
static void live_key(int physical){
    int bank=key_black(physical),white=key_white(physical),rc;
    uint16_t id;
    if(bank>=0){
        ui.live_bank=(uint8_t)bank;
        ui.sig[1]=0u;
        return;
    }
    if(white<0)return;
    id=(uint16_t)(ui.live_bank*16u+(unsigned)white);
    if(ks_recording){warn("LIVE","REC OFF FIRST");return;}
    rc=ks_arr_live_queue(ks_selected_track,id);
    if(rc<0){warn("LIVE",rc==-2?"PATTERN NOT SAVED":"CANNOT QUEUE");return;}
    ui.sig[1]=0u;
    {char t[16];put_u(put_s(t,"NEXT "),(uint32_t)id+1u);say("PATTERN",t);}
}
static void button_down(int b)
{
    switch (b) {
    case B_ENV: case B_LFO:
        /* v0.3.4: freed for Arranger – no Sustain/Mod from the panel. */
        break;
    case B_SEL: case B_EDIT: case B_GLO: case B_ARP: case B_SEQ:
        kstep_press_ms[b] = plat_ms();
        kstep_held_once &= ~(1u << b);
        break;
    case B_PLAY: case B_REC:
        /* KSTEP_PLAYREC_RELEASE_PAUSE_V034: short action on RELEASE only. */
        kstep_press_ms[b] = plat_ms();
        kstep_held_once &= ~(1u << b);
        break;
    case B_OCTDN: if(ui.view==V_STEP){ks_ed_drop_key_holds();(void)kse_page(&ks_ed,-1);ks_ed_revision++;}else octave(-1); break;
    case B_OCTUP: if(ui.view==V_STEP){ks_ed_drop_key_holds();(void)kse_page(&ks_ed,1);ks_ed_revision++;}else octave(1); break;
    case B_SAVE:
        ui.save_t = plat_ms();
        ui.save_long = 0;
        break;
    case B_FX:
        /* Short press is deliberately inert. Real long-press turns on CC edit. */
        kstep_press_ms[b] = plat_ms();
        kstep_held_once &= ~(1u << b);
        kstep_fx_long = 0;
        ui.fx_used = 0;
        ui.fx_t = plat_ms();
        break;
    default: break;
    }
}

/* Press-and-hold dispatch; one event per press, no accidental short actions. */
static int kstep_hold_button(int b)
{
    return b == B_FX || b == B_SEL || b == B_EDIT || b == B_GLO || b == B_ARP ||
           b == B_SEQ || b == B_PLAY || b == B_REC;
}
static void kstep_fire_hold(int b)
{
    uint32_t bit = 1u << b;
    if (!kstep_hold_button(b) || (kstep_held_once & bit)) return;
    kstep_held_once |= bit;
    switch (b) {
    case B_FX:
        kstep_fx_long = 1;
        set_view(V_PLAY);   /* preserve original FX-held CC-number editor */
        break;
    case B_SEL: set_view(V_SCALE); break;
    case B_EDIT: set_view(V_SOUND); break;
    case B_GLO: set_view(V_SETUP); break;
    case B_ARP: set_view(V_ARP); break;
    case B_SEQ:
        /* SEQ long: all formerly short-press ARRANGER pages. */
        if(ui.view!=V_STEP){
            if(ui.view!=V_ARR){set_view(V_ARR);sg.active_menu=0;ks_arr_page_to(1);}
            else if(ui.arr_pg==1u&&!sg.active_menu){sg.active_menu=1;sg.menu_row=0;sg_refresh();}
            else if(sg.active_menu){sg.active_menu=0;ks_arr_page_to(1);}
            else {sg.active_menu=1;sg.menu_row=0;sg_refresh();}
        }
        break;
    case B_REC:
        /* KSTEP_LIVE_EDITOR_V051: REC long is EDITOR, never pauses PLAY. */
        if(ks_recording){warn("EDITOR","REC OFF FIRST");break;}
        if(ui.view==V_STEP){kse_open(&ks_ed,0);set_view(V_PLAY);}
        else {ks_ed_sync_pattern();kse_open(&ks_ed,1);set_view(V_STEP);}
        break;
    case B_PLAY:
        if (ui.playing) {
            ks_sustain_apply(0); /* ONE KEY manual gates must end on release */
            all_keys_up(); /* stop normal keyboard notes before PAUSE mode */
            ks_ed_drop_key_holds();
            engine_pause(1);
            ui.playing = 0;
            kstep_paused = 1;
            ks_okp_pause(1);
            put_s(ui.last, "ONE KEY");
            say("ONE KEY", "16 KEYS ACTIVE");
        }
        break;
    default: break;
    }
}
static void kstep_check_holds(uint32_t buttons)
{
    static const uint8_t target[] = {B_FX, B_SEL, B_EDIT, B_GLO, B_ARP, B_SEQ, B_PLAY, B_REC};
    uint32_t i, now = plat_ms();
    for (i=0; i<sizeof(target)/sizeof(target[0]); ++i) {
        int b=target[i];
        if ((buttons >> b & 1u) && now-kstep_press_ms[b] >= KSTEP_HOLD_MS)
            kstep_fire_hold(b);
    }
}

/* KSTEP_HIDDEN_ERASE_240K_V043
 * LFO + ENV + SAVE simultaneously for 5 seconds, release ALL, then repeat
 * within 20 seconds. NEVER erase from boot, short SAVE or autosave. The
 * first hold ONLY arms; the second hold initiates a destructive 60-sector
 * erase. An incomplete chord is harmless; all releases from a chord are
 * swallowed to avoid accidentally executing ordinary SAVE afterwards. */
#define KSTEP_ERASE_CHORD ((1u<<B_LFO)|(1u<<B_ENV)|(1u<<B_SAVE))
#define KSTEP_ERASE_HOLD_MS 5000u
#define KSTEP_ERASE_WINDOW_MS 20000u
static uint32_t kstep_erase_down_ms, kstep_erase_armed_ms;
static uint8_t kstep_erase_chord_seen, kstep_erase_fired, kstep_erase_armed;
static int kstep_erase_swallow_release(int b)
{
    return kstep_erase_chord_seen &&
           (b==B_LFO || b==B_ENV || b==B_SAVE);
}
static void kstep_erase_poll(uint32_t buttons)
{
    uint32_t now=plat_ms();
    if (kstep_erase_armed && now-kstep_erase_armed_ms > KSTEP_ERASE_WINDOW_MS)
        kstep_erase_armed=0;
    if ((buttons & KSTEP_ERASE_CHORD)==KSTEP_ERASE_CHORD &&
        (buttons & ~KSTEP_ERASE_CHORD)==0u && ui.keys==0u) {
        if (!kstep_erase_chord_seen) {
            kstep_erase_chord_seen=1;
            kstep_erase_down_ms=now;
            kstep_erase_fired=0;
        }
        if (!kstep_erase_fired && now-kstep_erase_down_ms>=KSTEP_ERASE_HOLD_MS) {
            kstep_erase_fired=1;
            if (ks_recording || eng_running() || ui.playing) {
                kstep_erase_armed=0;
                warn("ERASE DISK", "STOP / REC OFF");
            } else if (!kstep_erase_armed) {
                kstep_erase_armed=1;
                kstep_erase_armed_ms=now;
                warn("ERASE 240K?", "REPEAT CHORD");
            } else {
                int r;
                kstep_erase_armed=0;
                r=ks_arr_erase_all_samples();
                if (r==0) say("DISK", "ERASED 240K");
                else warn("DISK", "ERASE FAILED");
            }
        }
    } else if ((buttons & KSTEP_ERASE_CHORD)==0u) {
        /* A complete release is required before a second hold can count. */
        kstep_erase_chord_seen=0;
        kstep_erase_fired=0;
    }
}

static void button_up(int b)
{
    if (kstep_erase_swallow_release(b)) return;
    /* Dispatch a held key also when it is released exactly at the threshold. */
    if (kstep_hold_button(b) && plat_ms() - kstep_press_ms[b] >= KSTEP_HOLD_MS)
        kstep_fire_hold(b);
    switch (b) {
    case B_ENV:
        /* Editor: paste copied step into the held white step key. */
        if(ui.view==V_STEP){
            if(!kse_paste_held(&ks_ed)){
                unsigned j;for(j=0;j<NKEYS;++j)if(ks_ed_key_t[j])ks_ed_key_changed[j]=1;
                ks_ed_revision++;
            } else warn("STEP","HOLD STEP TO PASTE");
        }
        break;
    case B_LFO:
        /* Keep STEP editor COPY intact; elsewhere LFO opens LIVE PLAY. */
        if(ui.view==V_STEP){
            if(!kse_copy_held(&ks_ed))say("STEP","COPIED");
            else warn("STEP","HOLD STEP TO COPY");
        }else if(ui.view==V_LIVE){
            set_view(V_PLAY); /* live queue remains active while playback runs */
        }else{
            if(ks_recording || (ui.playing && (ksm2_active() || jp.running))){
                warn("LIVE","STOP SONG / REC FIRST");break;
            }
            if(ks_arr_mode()!=0u && ks_arr_set_mode(0u)<0){
                warn("LIVE","STOP SONG / SAVE EDIT");break;
            }
            ks_arr_live_enable();
            {
                uint16_t pat=ks_arr_assigned(ks_selected_track);
                if(pat<176u)ui.live_bank=(uint8_t)(pat/16u);
            }
            set_view(V_LIVE);ui.sig[1]=ui.sig[3]=0u;
            say("LIVE PLAY","ALGO: TRACK");
        }
        break;
    case B_FX:
        /* No bank change; in editor short FX is single-step UNDO. */
        if(ui.view==V_STEP && !(kstep_held_once&(1u<<B_FX))){
            if(!kse_undo(&ks_ed)){
                unsigned j;for(j=0;j<NKEYS;++j)if(ks_ed_key_t[j])ks_ed_key_changed[j]=1;
                ks_ed_revision++;
            } else warn("STEP","NOTHING TO UNDO");
        }
        /* End CC-edit modifier on release. */
        kstep_fx_long = 0;
        ui.touched = -1;
        break;
    case B_HOME:
        /* KSTEP_HOME_DUAL_V0511: tapping HOME while on HOME flips between
         * PATTERNS and PIANO. Returning from another page starts on HOME 1. */
        if (ui.view == V_PLAY) ui.home_pg ^= 1u;
        else {
            if(ui.view==V_STEP)kse_open(&ks_ed,0);
            set_view(V_PLAY);
            ui.home_pg=0u;
        }
        ui.sig[1]=0u; /* force immediate main redraw on view flip */
        break;
    case B_SEL:
        if(!(kstep_held_once & (1u << B_SEL)) && ui.view==V_ARR){
            if(sg.active_menu){
                sg.active_menu=0u;
                if(sg.menu_row==0u)ks_arr_page_to(2);
                else ks_arr_page_to(3);
            }else if(ui.arr_pg==1u)pm_enter();
            else if(ui.arr_pg==2u)sg_enter();
            else if(ui.arr_pg==3u)ag_enter();
        }
        break;
    case B_SEQ:
        if(kstep_held_once & (1u<<B_SEQ))break;
        /* SEQ short is the PLAYER, not the original drum sequencer.
         * Never inherit an actively looping SONG EDITOR transport: that
         * player uses ks_gp_repeat, whereas PLAYER uses jp.loop. */
        if(ui.playing && !jp.running && ks_arr_grid_play_active()){
            engine_play(0);
            ks_arr_grid_play_stop();
            ksm2_stop();
            ks_live_notes_clear();
            ui.playing=0u;
            kstep_paused=0u;
            ks_arr_grid_set_loop(0);
        }
        if(ui.view==V_STEP)ks_ed_drop_key_holds();
        set_view(V_JUKE);jp_first();
        break;
    case B_ARP: /* ARP only on longpress */ break;
    case B_EDIT: /* SOUND only on longpress */ break;
    case B_GLO:
        /* Long GLO opens SETUP; its release must not skip page 1. */
        if(kstep_held_once & (1u << B_GLO))break;
        if(ui.view==V_SETUP){
            ui.setup_pg=(uint8_t)(ui.setup_pg==0u?(FM1B_BLE?2u:3u):
                     ui.setup_pg==2u?3u:0u);
            ui.sig[1]=ui.sig[2]=0u;
        }
        break;
    case B_PLAY: case B_REC:
        if (!(kstep_held_once & (1u << b))) {
            if (b == B_REC) {
                int rec = ks_arr_rec_toggle();
                put_s(ui.last, rec ? "REC ON" : "REC OFF");
                say(rec ? "REC" : "REC", rec ? "ON" : "OFF");
            } else if (kstep_paused) {
                ks_okp_pause(0); /* release every held VL-1 note before resume */
                engine_pause(0);           /* MIDI Continue: never reset pattern */
                kstep_paused = 0;
                ui.playing = 1;
                put_s(ui.last, "Continue");
            } else {
                int start=!ui.playing;
                if(start && ui.view==V_JUKE){
                    /* Clear any editor-only repeat state BEFORE preparing a
                     * SONG or ALBUM. K4 governs repetition of whole objects. */
                    ks_arr_grid_set_loop(0);
                    ks_arr_grid_play_stop();
                    jp.manual_set=0u; /* new START resets manual tempo policy */
                    if(jp_prepare()<0){warn("PLAYER","EMPTY / SAVE FIRST");break;}
                    jp.running=1u;
                    ks_arr_grid_set_loop(0);
                }
                if(start && ui.view==V_ARR && ui.arr_pg==2u && !sg.active_menu){
                    int rc;
                    jp.running=0u; /* explicit editor playback is NOT PLAYER */
                    if(sg.stage!=SG_BROWSE){warn("SONG","EXIT MENU FIRST");break;}
                    ks_arr_grid_set_loop(1); /* EDITOR PLAY always loops */
                    rc=ks_arr_grid_play_prepare();
                    if(rc<0){warn("SONG",rc==-4?"EMPTY":"PATTERN LOAD FAILED");break;}
                }
                if(start && ks_arr_mode()==0u)ks_arr_live_transport_reset();
                ui.playing=(uint8_t)start;
                engine_play(ui.playing);   /* original Start/Stop */
                if(!start){jp.running=0u;ks_arr_grid_play_stop();ks_arr_offs();ksm2_stop();ks_live_notes_clear();jp_restore_program();}
                put_s(ui.last, ui.playing ? "Start" : "Stop");
            }
        }
        break;
    case B_SAVE:
        if (ui.save_long)
            break;                       /* held: started Bluetooth, no save */
        if (ui.view == V_SETUP && ui.setup_pg == 2 && bt_stopped()) {   /* stopped after a crash: start it */
            plat_ble_start();
            say("BLUETOOTH", "STARTING");
            break;
        }
        if (FM1B_BLE_CENTRAL && ui.view == V_SETUP && ui.setup_pg == 2 && plat_ble_state() == BLE_READY &&
            !proj.ble_off) {                                                /* a device: connect, or let it go */
            int ev = btdev_save();
            say(btdev_word(ev), ev == BTDEV_NO_DEVICE ? 0 : btdev_name());
            break;
        }
        if(ui.view==V_ARR && ui.arr_pg==3u && ksal.dirty){
            int rc=ksal_save();
            if(rc<0)warn("ALBUM","SAVE FAILED");
            else say("ALBUM","SAVED");
            ag_refresh();break;
        }
        /* PATTERN workspaces are unsaved until PATTERN > SAVE. */
        if(ks_arr_work_dirty(0)||ks_arr_work_dirty(1)||
           ks_arr_work_dirty(2)||ks_arr_work_dirty(3)){
            warn("UNSAVED","SEQ > PATTERN > SAVE");
            break;
        }
        /* KSTEP_DISK_ALL_240K_V043: no ordinary SAVE can destroy sample data. */
        if (ks_arr_any_dirty()) {
            int r=ks_arr_save();
            if(r<0) {
                if(r==-1) warn("DISK", "STOP / REC OFF");
                else if(ks_disk_status==2) warn("DISK", "ERASE SAMPLES");
                else warn("DISK", "WRITE ERROR");
                break;
            }
        }
        if (project_save() == 0) {
            ui.dirty = 0;
            say("SAVED", 0);
        } else {
            warn("NOT SAVED", "FLASH ERROR");
        }
        break;
    default: break;
    }
}

/* the Sequencer page: the keys edit */
static void seq_key(int k)
{
    int w = key_white(k), b = key_black(k);
    if (w >= 0) {
        if (w < trk()->steps) {
            trk()->pat ^= (uint16_t)(1u << w);
            mark_dirty();
            sp_event(SE_STEP, w, trk()->pat >> w & 1u);
        } else {
            sp_event(SE_STEP_OUT, w, 0);         /* past the track's end: nothing (a screen reader says so) */
        }
    } else if (b >= 0 && b < 4) {
        ui.track = (uint8_t)b;
        ui.touched = -1;
    } else if (b >= 4 && b < 8) {
        proj.trk[b - 4].mute = (uint8_t)!proj.trk[b - 4].mute;
        mark_dirty();
        sp_event(SE_MUTE, b - 4, proj.trk[b - 4].mute);
    }
}

/* the MASTER pot as a controller: smoothed, sent when it moves by more than its noise */
static void master_pot(void)
{
    int32_t raw = (int32_t)plat_master(), v;
    ui.master_raw += (raw - ui.master_raw) / 4;
    if (!proj.master_cc)                 /* 0: the synth's volume (main_fm1.c) */
        return;
    v = ui.master_raw * 127 / 4000;
    if (v > 127)
        v = 127;
    if (ui.master_sent < 0 || (v != ui.master_sent && (v - ui.master_sent > 1 || ui.master_sent - v > 1 ||
                                                       v == 0 || v == 127))) {
        ui.master_sent = (int16_t)v;
        cc(proj.master_cc, (uint32_t)v);
    }
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, i;
    int32_t e;
    ch = keys ^ ui.keys;                 /* keys first: a note is the most urgent thing here */
    ui.keys = keys;
    if(kstep_paused)ks_okp_refresh(0); /* keySTEP ONEKEY: update before key edges */
    for (i = 0; i < NKEYS; i++)
        if (ch >> i & 1u) {
            if(kstep_paused){
                ks_okp_key((int)i,(int)((keys>>i)&1u));
                continue; /* even STEP editor/normal keys must be silent */
            }
            if(ui.view==V_LIVE){
                if(keys >> i & 1u)live_key((int)i);
                continue; /* release is silent, no MIDI keyboard notes */
            }
            if(ui.view==V_STEP){
                if(keys >> i & 1u)ks_ed_key_start((int)i);
                else ks_ed_key_end((int)i);
                continue;
            }
            if (keys >> i & 1u) {
                if (ui.view == V_SEQ)
                    seq_key((int)i);
                else
                    key_down((int)i, edge_vel(i));
            } else {
                key_up((int)i);
            }
        }
    ch = btn ^ ui.btn;
    ui.btn = btn;
    for (i = 0; i < NB; i++)
        if (ch >> i & 1u) {
            if (btn >> i & 1u)
                button_down((int)i);
            else
                button_up((int)i);
        }
    kstep_check_holds(btn);
    kstep_erase_poll(btn); /* deliberate destructive chord, main loop ONLY */
    if (btn || keys || ch)
        ui.act_t = plat_ms();
    midi_in();
    /* PLAY owns the album/song boundary; never let an editor's stale grid
     * repeat bit keep a song alive indefinitely behind the ONCE display. */
    if(jp.running)ks_arr_grid_set_loop(0);
    {int m2_end=ksm2_poll();
    if(m2_end && ui.playing){
        int rc=-1;
        uint8_t held=jp.manual_bpm,had_manual=jp.manual_set;
        engine_play(0);ksm2_stop();ks_live_notes_clear();
        /* Only a NORMAL M2 end may advance. Decoder/flash errors STOP. */
        if(m2_end>0 && jp.running && (jp.category==1u || jp.loop) && jp_album_has_next()){
            rc=jp_next_after_end();
            if(rc==0 && jp.category==0u && had_manual){
                if(ksm2_active())ksm2_override_tempo(held);
                else if(proj.clock!=CLK_EXT)proj.tempo=held;
            }
        }
        if(rc==0){engine_play(1);ui.playing=1;}
        else{
            ui.playing=0;jp.running=0u;
            if(jp.category && !jp.loop && m2_end>0)jp.once_done=1u;
            jp_restore_program();warn(m2_end<0?"M-SONG ERROR":"ALBUM / SONG", "END / STOP");
        }
    }}
    if(!ksm2_active() && ks_arr_poll() && ui.playing){
        int fail=ks_arr_grid_play_underrun();
        if(fail)warn("SONG","PREFETCH ERROR");
        engine_play(0);ui.playing=0;ks_arr_grid_play_stop();ks_arr_offs();ks_live_notes_clear();
        if(!fail && jp.running && (jp.category==1u || jp.kind==0u) && jp_album_has_next()){
            if(jp_next_after_end()==0){ui.playing=1;engine_play(1);}
            else{jp.running=0u;if(jp.category && !jp.loop && !fail)jp.once_done=1u;jp_restore_program();}
        }else{
            jp.running=0u;
            if(jp.category && !jp.loop && !fail)jp.once_done=1u;
            jp_restore_program();
        }
    }
    master_pot();
    if ((e = plat_enc(EN_SELECT)) != 0) {
        /* SELECT navigates the four SEQ pages, not a dead encoder. */
        if(ui.view==V_JUKE){
            if(!ui.playing){jp.category=(uint8_t)clamp((int)jp.category+(e>0?1:-1),0,1);jp_refresh();}
        }else if(ui.view==V_ARR && sg.active_menu){
            sg.menu_row=(uint8_t)clamp((int)sg.menu_row+e,0,1);sg_refresh();
        }else if(ui.view==V_ARR && ui.arr_pg==1u)pm_scroll(e);
        else if(ui.view==V_ARR && ui.arr_pg==2u)sg_scroll(e);
        else if(ui.view==V_ARR && ui.arr_pg==3u)ag_scroll(e);
        else if (ui.view == V_ARR) ks_arr_page_spin(e);
        else if (ui.view == V_SEQ) {
            set_view(V_ARR);
            ks_arr_page_spin(e);
        }
    }
    if (FM1B_BLE && ui.view == V_SETUP && (btn >> B_SAVE & 1u) && !kstep_erase_chord_seen && !ui.save_long &&
        plat_ms() - ui.save_t >= SAVE_HOLD_MS) {  /* SAVE held on Setup: Bluetooth, off after a crash, by hand */
        ui.save_long = 1;
        if (plat_ble_state() == BLE_OFF) {
            plat_ble_start();
            say("BLUETOOTH", "STARTING");
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        if(ui.view==V_ARR && ui.arr_pg==2u && sg.stage!=SG_BROWSE && !sg.active_menu){
            /* Keep DEL/MOVE attached to the selected track until SEL. */
         } else if (ui.view == V_ARR && ui.arr_pg == 3u) {
            /* One ALBUM row: ALGO has no second row to select. */
        } else {
        int tr=(int)ks_selected_track+e;
        if(tr<0)tr=0;
        if(tr>3)tr=3;
        if(tr!=(int)ks_selected_track){
            ks_sustain_apply(0); /* pedal OFF on old track before ALGO changes */
            ks_okp_release();
            all_keys_up(); /* note-off to OLD track before ALGO changes */
            ks_arr_set_track((uint8_t)tr);
            if(ui.view==V_LIVE){
                uint16_t pat=ks_arr_assigned((uint8_t)tr);
                if(pat<176u)ui.live_bank=(uint8_t)(pat/16u);
            }
            if(ui.view==V_STEP){ks_ed_drop_key_holds();kse_select_track(&ks_ed,(unsigned)tr);}
            ui.arr_row=(uint8_t)tr;
            if(ui.view==V_ARR && ui.arr_pg==2u)sg_track_sync();
            ui.sig[1]=0u;
            ks_ed_revision++;
        }
        }
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        const uint8_t tr=ks_selected_track;
        int old=(int)ks_program_get(tr),p=clamp(old+e,0,127);
        if(p!=old){
            ks_program_set(tr,(uint8_t)p); /* saved with existing project settings */
            ctl_send(pkt3(0xC0u|tr,(uint32_t)p,0),TO_SYNTH); /* NOT MIDI OUT */
            prog_name(put_s(put_u(ui.last,(uint32_t)p+1u)," "),(uint32_t)p);
            mark_dirty();
        }
    }
    for (i = 0; i < 4; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0) {
            int k, lo, hi;
            if ((ui.btn >> B_FX & 1u) && !kstep_fx_long)
                continue;    /* an unqualified FX press must not edit anything */
            if ((ui.btn >> B_FX & 1u) && kstep_fx_long)
                ui.fx_used = 1;
            if(ui.view==V_ARR && ui.arr_pg==3u && i==1){
                ag_song_pick((int)e);ui.touched=(int8_t)i;ui.touch_until=plat_ms()+1200u;
                continue;
            }
            k = view_knob((int)i);
            if (k != K_NONE) {
                knob_range(k, &lo, &hi);
                knob_set(k, knob_get(k) + accel(EN_K1 + (int)i, e, hi - lo));
            }
            ui.touched = (int8_t)i;
            ui.touch_until = plat_ms() + 1200u;
            ui.act_t = plat_ms();
        }
}

/* ------------------------------------------------------------- screen --- */
/* four bands (X0X's top bar and knob row): the top bar (20 px), the page (120), the status line (18),
 * the knob row (82: its tag on a rule, four knobs, the footer) */
#define HDR_H 20
#define MAIN_Y 20
#define MAIN_H 120
#define STAT_Y 140
#define STAT_H 18
#define KNB_Y 158
#define KNB_H 82

static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t hash_s(uint32_t h, const char *s)
{
    while (*s)
        h = hash(h, (uint8_t)*s++);
    return h;
}

/* a rectangle with rounded corners (radius r, up to 6) */
static void cv_round(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    static const uint8_t IN[7][6] = {{0}, {1}, {1, 0}, {2, 1, 0}, {2, 1, 0, 0}, {3, 2, 1, 0, 0}, {4, 2, 1, 1, 0, 0}};
    int32_t j;
    if (r > 6)
        r = 6;
    if (r > h / 2)
        r = h / 2;
    cv_rect(x, y + r, w, h - 2 * r, c);
    for (j = 0; j < r; j++) {
        int32_t d = IN[r][j];
        cv_rect(x + d, y + j, w - 2 * d, 1, c);
        cv_rect(x + d, y + h - 1 - j, w - 2 * d, 1, c);
    }
}

static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void text_r(int32_t rx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(rx - text_w(f, s), y, f, s, c);
}

/* ---- from X0X: text centred on its capitals, and anti-aliased shapes (coverage 0..16, blended over
 * what is there) */
typedef struct { const felucca_font_t *f; int8_t top, bot; } capm_t;
static capm_t capm[6];
static const capm_t *caps(const felucca_font_t *f)   /* the first and last rows of 'H', measured once */
{
    int i;
    for (i = 0; i < 6 && capm[i].f; i++)
        if (capm[i].f == f)
            return &capm[i];
    if (i == 6)
        i = 5;
    {
        uint32_t gi = ((uint32_t)'H' <= f->last ? 'H' : '0') - f->first, w = f->bw[gi], bpr = (w + 1u) / 2u, x, y;
        const uint8_t *gd = f->data + f->off[gi];
        int top = -1, bot = 0;
        for (y = 0; y < f->h; y++)
            for (x = 0; x < w; x++) {
                uint32_t a = gd[y * bpr + x / 2u];
                a = (x & 1u) ? (a & 15u) : (a >> 4);
                if (a >= 8u) {
                    if (top < 0)
                        top = (int)y;
                    bot = (int)y;
                }
            }
        capm[i].f = f;
        capm[i].top = (int8_t)(top < 0 ? 0 : top);
        capm[i].bot = (int8_t)bot;
    }
    return &capm[i];
}
static int32_t vc(const felucca_font_t *f, int32_t y0, int32_t h)   /* the y that centres f's capitals in [y0, y0+h) */
{
    const capm_t *m = caps(f);
    return y0 + (h - (m->bot - m->top + 1) + 1) / 2 - m->top;
}
static void px_blend(int32_t x, int32_t y, uint16_t c, uint32_t a)
{
    uint16_t *q;
    uint32_t bg;
    y += cv_oy;
    if ((uint32_t)x >= cv_w || y < cv_y0 || y >= cv_y1 || !a)
        return;
    q = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    if (a >= 16u) {
        *q = swap16(c);
        return;
    }
    bg = swap16(*q);
    *q = swap16((uint32_t)(((((c >> 11) * a + (bg >> 11) * (16u - a)) >> 4) << 11) |
                           (((((c >> 5) & 63u) * a + ((bg >> 5) & 63u) * (16u - a)) >> 4) << 5) |
                           (((c & 31u) * a + (bg & 31u) * (16u - a)) >> 4)));
}
static void disc(float cx, float cy, float r, uint16_t c)
{
    int32_t x0 = (int32_t)fm_floorf(cx - r - 1.0f), x1 = (int32_t)(cx + r + 1.0f);
    int32_t y0 = (int32_t)fm_floorf(cy - r - 1.0f), y1 = (int32_t)(cy + r + 1.0f), x, y;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = fm_sqrtf(dx * dx + dy * dy);
            px_blend(x, y, c, (uint32_t)(fm_clampf(r - d + 0.5f, 0.0f, 1.0f) * 16.0f + 0.5f));
        }
}
static void dot(int32_t x, int32_t y, int32_t r, uint16_t c) { disc((float)x + 0.5f, (float)y + 0.5f, (float)r + 0.3f, c); }
static float turns(float y, float x)              /* atan2 in turns, 0..1 from +x clockwise (y down) */
{
    float ax = fm_fabsf(x), ay = fm_fabsf(y), a = ax < ay ? ax / (ay + 1e-9f) : ay / (ax + 1e-9f), q = a * a, r;
    r = ((-0.0464964749f * q + 0.15931422f) * q - 0.327622764f) * q * a + a;
    if (ay > ax)
        r = 1.57079637f - r;
    if (x < 0.0f)
        r = FM_PI - r;
    if (y < 0.0f)
        r = FM_TWO_PI - r;
    return r * (1.0f / FM_TWO_PI);
}
/* a knob: a 270-degree track from 7:30 to 4:30, the value filled from the start, round ends and a handle */
static void arc(int32_t cx, int32_t cy, int32_t r, float v, uint16_t track, uint16_t fill)
{
    const float th = 3.4f, a0 = 0.375f, sweep = 0.75f;
    float ro = (float)r, ri = ro - th, rm = ro - th * 0.5f, fx = (float)cx + 0.5f, fy = (float)cy + 0.5f;
    int32_t x, y;
    v = fm_clampf(v, 0.0f, 1.0f);
    for (y = -r - 1; y <= r + 1; y++)
        for (x = -r - 1; x <= r + 1; x++) {
            float dx = (float)x, dy = (float)y, d = fm_sqrtf(dx * dx + dy * dy), t, a;
            a = fm_clampf(ro + 0.5f - d, 0.0f, 1.0f) * fm_clampf(d - ri + 0.5f, 0.0f, 1.0f);
            if (a <= 0.0f)
                continue;
            t = turns(dy, dx) - a0;
            if (t < 0.0f)
                t += 1.0f;
            t /= sweep;
            if (t > 1.0f)
                continue;
            px_blend(cx + x, cy + y, t <= v ? fill : track, (uint32_t)(a * 16.0f + 0.5f));
        }
    {
        float e0 = a0 * FM_TWO_PI, e1 = (a0 + sweep) * FM_TWO_PI, ev = (a0 + sweep * v) * FM_TWO_PI;
        disc(fx + fm_cosf(e0) * rm - 0.5f, fy + fm_sinf(e0) * rm - 0.5f, th * 0.5f, v > 0.0f ? fill : track);
        disc(fx + fm_cosf(e1) * rm - 0.5f, fy + fm_sinf(e1) * rm - 0.5f, th * 0.5f, v >= 1.0f ? fill : track);
        disc(fx + fm_cosf(ev) * rm - 0.5f, fy + fm_sinf(ev) * rm - 0.5f, th * 0.5f + 1.3f, K_TEXT);
        disc(fx + fm_cosf(ev) * rm - 0.5f, fy + fm_sinf(ev) * rm - 0.5f, th * 0.5f + 0.6f, K_WHITE);
    }
}
static void play_icon(int32_t x, int32_t y, uint16_t c)   /* a triangle 9 x 11, edges anti-aliased */
{
    int32_t i, j;
    for (j = 0; j < 11; j++)
        for (i = 0; i < 9; i++) {
            float fy = (float)j + 0.5f - 5.5f, fx = (float)i + 0.5f, edge = 8.0f - fx - fm_fabsf(fy) * 1.3f;
            float a = fm_clampf(edge * 0.7f + 0.5f, 0.0f, 1.0f) * fm_clampf(fx + 0.5f, 0.0f, 1.0f);
            px_blend(x + i, y + j, c, (uint32_t)(a * 16.0f + 0.5f));
        }
}

static int32_t tag(int32_t x, int32_t y, const char *s, uint16_t bg, uint16_t fg)   /* a little label */
{
    int32_t w = text_w(&FONT_XS, s) + 10;
    cv_round(x, y, w, 16, 4, bg);
    cv_text(x + 5, vc(&FONT_XS, y, 16), &FONT_XS, s, fg);
    return w;
}

static int lit(int p, int out) { return plat_midi_ago(p, out) < FLASH_MS; }

/* the top bar (X0X's): the page's name, on Play the knob bank in a chip; the channel in the middle;
 * Bluetooth, the tempo and the play triangle on the right, a dot while unsaved. A message takes the bar. */
static void draw_header(void)
{
    char t[24];
    int msg = plat_ms() < ui.msg_until, bs = plat_ble_state(), bc = plat_ble_linked(), beat = eng_beat_on();
    int32_t x;
    uint32_t h = hash(hash(hash(2166136261u, ui.view), (uint32_t)msg | ui.dirty << 1 | (uint32_t)bs << 2 |
                                                           (uint32_t)bc << 4 | ui.playing << 5 | proj.arp_on << 6 |
                                                           (uint32_t)beat << 7 | proj.bank << 8),
                      ks_arr_channel() | (uint32_t)ks_selected_track<<4 |
                      (uint32_t)proj.tempo << 8 | (uint32_t)proj.clock << 16 |
                      (uint32_t)ui.home_pg << 24);
    if (msg)
        h = hash(hash_s(hash_s(h, ui.msg[0]), ui.msg[1]), ui.msg_until);
    if (bs == BLE_STARTING)
        h = hash(h, ui.frame / 8u);
    if (ui.view == V_ARR) h = hash(h, ui.arr_pg);
    if (ui.view == V_JUKE) h=hash(h,jp.category|(uint32_t)jp.kind<<4|(uint32_t)jp.song<<8|(uint32_t)jp.album<<24);
    if (h == ui.sig[0])
        return;
    ui.sig[0] = h;
    cv_begin(240, HDR_H, K_BG);
    cv_rect(0, HDR_H - 1, 240, 1, K_LINE);
    if (msg) {                                   /* what a button just did: the whole bar */
        int32_t w = text_w(&FONT_B, ui.msg[0]) + 8 + text_w(&FONT_B, ui.msg[1]);
        cv_round(0, 0, 240, HDR_H - 1, 4, K_HOT);
        x = cv_text(120 - w / 2, vc(&FONT_B, 0, HDR_H - 1), &FONT_B, ui.msg[0], K_WHITE);
        cv_text(x + 8, vc(&FONT_B, 0, HDR_H - 1), &FONT_B, ui.msg[1], K_WHITE);
        cv_blit(0, 0);
        return;
    }
    {
        static const char *const seq_title[4] = {"PLAY", "PATTERN", "SONG", "ALBUM"};
        const char *name = ui.view == V_ARR ? (sg.active_menu?"EDIT MODE":seq_title[ui.arr_pg]) : VIEW_NAME[ui.view];
        x = cv_text(4, vc(&FONT_B, 1, 17), &FONT_B, name, K_TEXT) + 6;
    }
    if (ui.view == V_PLAY) {                     /* the knob bank, in a chip */
        char b[2] = {ui.home_pg ? '2' : '1', 0};
        int32_t w = text_w(&FONT_B, b) + 10;
        cv_round(x, 1, w, 17, 4, K_COOL);
        cv_text(x + 5, vc(&FONT_B, 1, 17), &FONT_B, b, K_TEXT);
    }
    if(ks_arr_channel()==16u)put_s(t,"CH ALL");
    else put_u(put_s(t, "CH "), (uint32_t)ks_arr_channel() + 1u);
    text_c(132, vc(&FONT_B, 1, 17), &FONT_B, t, K_TEXT);
    /* v0.3.4: tempo moved to fixed footer; keep the transport play icon. */
    play_icon(217, 4, ui.playing ? (beat ? K_HOT : K_TEXT) : K_LINE);
    if (ui.dirty)
        dot(233, 9, 1, K_DIM);
    /* v0.3.4: BLE indicator moved to fixed footer. */
    cv_blit(0, 0);
}

/* the keyboard: held keys coral, notes heard teal; with a scale the white keys' roots marked */
static int key_heard(int k)
{
    uint8_t q[3];
    int n = key_notes(k, q);
    return n && (ui.heard[q[0] >> 5] >> (q[0] & 31) & 1u);
}
static int key_root(int k)
{
    int w = key_white(k);
    return proj.scale && w >= 0 && w % SCALE_LEN[proj.scale] == 0;
}
static void draw_keys(int32_t y0, int32_t h)
{
    int i, k;
    cv_round(3, y0 - 1, 16 * 14 + 1, h + 2, 3, K_LINE);
    for (i = 0; i < 16; i++) {
        int32_t x = 4 + i * 14;
        k = WHITE_K[i];
        cv_round(x, y0, 13, h, 2, ui.keys >> k & 1u ? K_HOT : key_heard(k) ? K_COOL_T : K_KEYW);
        if (key_root(k))
            cv_round(x + 4, y0 + h - 9, 5, 5, 2, K_HOT);
    }
    for (i = 0; i < 15; i++) {           /* a black key after each white one that has one */
        int32_t x = 4 + i * 14 + 9;
        k = WHITE_K[i] + 1;
        if (WHITE_K[i + 1] != k + 1)
            continue;
        cv_round(x, y0, 9, h * 3 / 5, 2, ui.keys >> k & 1u ? K_HOT : key_heard(k) ? K_COOL : K_KEYB);
    }
}

static void draw_ports(int32_t y0)
{
    int p, slot = 0;
    for (p = 0; p < NPORTS; p++) {
        int32_t x = 8 + slot * 78;
        if (!FM1B_BLE && p == PORT_BLE)
            continue;                    /* the USB-only build: USB and TRS */
        slot++;
        int i = lit(p, 0), o = p < 2 && lit(p, 1);
        const char *st = "";
        uint16_t sc = K_DIM;
        cv_round(x, y0, 70, 34, 6, K_PANEL);
        cv_text(x + 8, y0 + 2, &FONT_S, PORT_NAME[p], K_TEXT);
        cv_round(x + 52, y0 + 5, 8, 8, 4, i ? K_COOL : K_LINE);   /* in */
        if (p < 2)
            cv_round(x + 52, y0 + 18, 8, 8, 4, o ? K_HOT : K_LINE);   /* out */
        if (p == PORT_BLE) {
            st = bt_status();                /* a computer, or a device */
            sc = st[0] == 'c' ? K_COOL_D : K_DIM;
        } else {
            st = p == PORT_TRS ? "in only" : "";
        }
        cv_text(x + 8, y0 + 18, &FONT_XS, st, sc);
    }
}

/* KSTEP_HOME_4_ONLY_V031: exactly four MIDI track lanes on HOME.
 * The original GM drum sequencer stays intact on its SEQUENCER page,
 * and stays on CH10 by default. Only the fifth visual lane is removed. */
/* KSTEP_HOME_DUAL_V0511: same position drives physical LEDs and screen.
 * HOME 1 shows pattern blocks with a golden playhead; HOME 2 the original
 * keyboard layout for ALGO's selected track. */
static int ks_home_playhead_step(void); /* implementation beside physical LEDs */
static int ks_home_display_block(void)
{
    int step=ks_home_playhead_step();
    return step>=0 ? step/16 : (int)ks_arr_block();
}
/* KSTEP_HOME_LIVE_NOTE_V0513: HOME 2 shows ACTUALLY HELD notes, even
 * while transport is stopped. Input is display-only; MIDI routing unchanged.
 * Most recently played key/port wins; incoming MIDI is filtered by ALGO
 * track's assigned MIDI channel, with CH ALL accepting all channels. */
static int ks_home_live_note(int *count)
{
    uint32_t newest=0u;
    unsigned total=0u, i, j, ch, word;
    int pitch=-1;
    int selected_channel=(int)ks_arr_channel();
    for(i=0;i<NKEYS;i++){
        if(ui.kn[i] && ui.kmode[i]!=KM_NONE){
            total+=(unsigned)ui.kn[i];
            if(ui.live_key_stamp[i] && ui.live_key_stamp[i]>=newest){
                newest=ui.live_key_stamp[i];
                pitch=(int)ui.knotes[i][0];
            }
        }
    }
    for(ch=0;ch<16u;ch++){
        int candidate=-1, last=(int)ui.live_midi_last[ch];
        unsigned active=0;
        if(selected_channel!=16 && (int)ch!=selected_channel)continue;
        for(word=0;word<4u;word++){
            uint32_t bits=ui.live_midi_bits[ch][word];
            if(!bits)continue;
            for(j=0;j<32u;j++)if(bits & (1u<<j)){
                active++;
                if(candidate<0)candidate=(int)(word*32u+j);
            }
        }
        if(!active)continue;
        total+=active;
        if(ui.live_midi_bits[ch][(unsigned)last>>5] & (1u<<((unsigned)last & 31u)))
            candidate=last;
        if(ui.live_midi_stamp[ch] && ui.live_midi_stamp[ch]>=newest){
            newest=ui.live_midi_stamp[ch];
            pitch=candidate;
        }
    }
    *count=(int)total;
    return pitch;
}
/* HOME 2 priority: live keyboard/USB/BLE/TRS > sequencer/ONE KEY.
 * If no notes are held, display the currently playing pattern event. */
static int ks_home_selected_note(int *count)
{
    kse_step_t s;
    int live=ks_home_live_note(count);
    int step;
    if(live>=0)return live;
    step=ks_home_playhead_step();
    *count=0;
    if(step<0 || step >= (int)ks_arr_play_len(ks_selected_track)) return -1;
    if(ks_arr_play_get(ks_selected_track,(uint8_t)step,&s)!=0 || !s.count) return -1;
    *count=s.count;
    return s.note[0]; /* chord uses its first MIDI note as label */
}
/* KSTEP_HOME_PIANO_COMPACT_V0512: CHROMA-size 46px keyboard, big readable
 * note name and MIDI number. Leave four knob rows and HOME 1 unchanged. */
static void draw_home_piano(void)
{
    int midi, n=0, i, k, base=KEY0_NOTE;
    char label[16], number[12], chord[12], *p;
    uint32_t played_bits[4];
    int played_voices=0,played_last=-1,show_played=0;
    int song_running=ui.playing && (ksm2_active() || ks_arr_mode()!=0u);
    ks_live_notes_snapshot(played_bits,&played_voices,&played_last);
    midi=ks_home_selected_note(&n);
    if(played_voices>0){
        midi=played_last;n=played_voices;show_played=1;
    }else if(song_running){
        /* A silent block really displays silence, not a stale TEMP note. */
        midi=-1;n=0;
    }
    /* As in v0.5.11, keep the selected note visible by shifting the
     * original F3-based 16-white/11-black keyboard by whole octaves. */
    if(midi>=0){
        while(midi<base)base-=12;
        while(midi>base+26)base+=12;
    }
    /* Exactly the 46px key height of draw_scale()'s CHROMA keyboard.
     * No PIANO title, track/pattern text or USB/TRS status clutter. */
    cv_round(3, 6, 225, 48, 3, K_LINE);
    for(i=0;i<16;++i){
        int32_t x=4+i*14;
        k=WHITE_K[i];
        cv_round(x,7,13,46,2,((base+k>=0 && base+k<128) && (show_played ? ((played_bits[(base+k)>>5]>>( (base+k)&31))&1u) : (midi>=0 && base+k==midi)))?K_HOT:K_KEYW);
    }
    for(i=0;i<15;++i){
        int32_t x=4+i*14+9;
        k=WHITE_K[i]+1;
        if(WHITE_K[i+1]!=k+1)continue;
        cv_round(x,7,9,27,2,((base+k>=0 && base+k<128) && (show_played ? ((played_bits[(base+k)>>5]>>((base+k)&31))&1u) : (midi>=0 && base+k==midi)))?K_HOT:K_KEYB);
    }
    /* Use separate columns so an optional +N chord count does not reduce
     * the note font or collide with MIDI note number 0..127. */
    if(midi>=0){
        put_note(label,midi);
        put_u(number,(uint32_t)midi);
    } else {
        put_s(label,"--");
        put_s(number,"--");
    }
    cv_text(9,60,&FONT_S,"NOTE",K_DIM);
    text_r(231,60,&FONT_S,"MIDI",K_DIM);
    cv_text(9,82,&FONT_L,label,midi>=0?K_HOT:K_DIM);
    text_r(231,82,&FONT_L,number,midi>=0?K_COOL:K_DIM);
    if(n>1){
        p=put_s(chord,"+");put_u(p,(uint32_t)n-1u);
        cv_text(9+text_w(&FONT_L,label)+5,97,&FONT_XS,chord,K_TEXT);
    }
}
static void draw_live(void){
    char t[40],*p;uint16_t pat=ks_arr_assigned(ks_selected_track),next;
    int has_pattern=ks_arr_live_current(ks_selected_track,&pat);
    p=put_u(put_s(t,"TRACK "),(uint32_t)ks_selected_track+1u);
    cv_text(9,4,&FONT_B,t,K_TEXT);
    if(has_pattern && pat<176u){
        p=put_u(put_s(t,"PLAYING "),(uint32_t)pat+1u);
        cv_text(9,31,&FONT_M,t,K_HOT);
        p=put_u(put_s(t,"BANK "),(uint32_t)pat/16u+1u);
        put_u(put_s(p,"  KEY "),(uint32_t)pat%16u+1u);
        cv_text(9,65,&FONT_S,t,K_DIM);
    }else{
        cv_text(9,31,&FONT_M,"PLAYING --",K_DIM);
        cv_text(9,65,&FONT_S,has_pattern?"PATTERN 177+: NO KEY":"EMPTY TRACK",K_DIM);
    }
    if(ks_arr_live_queued(ks_selected_track,&next)){
        put_u(put_s(t,"NEXT LOOP: "),(uint32_t)next+1u);
        cv_text(9,90,&FONT_S,t,K_COOL);
    }else cv_text(9,90,&FONT_XS,"BLACK: BANK  WHITE: PATTERN",K_DIM);
    (void)p;
}
static void draw_play(void)
{
    int i, s, step=ks_home_playhead_step();
    int block=ks_home_display_block();
    if(ui.home_pg){draw_home_piano();return;}
    for (i = 0; i < KSTEP_MIDI_TRACKS; i++) {
        const kstep_midi_track_t *k = &kstep_midi_tracks[i];
        int32_t y = 5 + i * 30;  /* identical row pitch to draw_seq */
        char label[16], *q;
        /* EXACT same gray rounded selection bar as original draw_seq(). */
        if((uint8_t)i==ks_selected_track)
            cv_round(2,y-3,236,28,6,K_PANEL);
        uint8_t mode=ks_track_mode_get((uint8_t)i);
        int audible=ks_track_audible((uint8_t)i);
        /* A mute/solo flag controls SOUND, never whether a saved pattern exists.
         * Every assigned track shows its own actual notes, even when silent. */
        uint16_t pat = ks_arr_mask((uint8_t)i,(uint8_t)block);
        q = put_s(label, "T");
        q = put_u(q, (uint32_t)i + 1u);
        q = put_s(q, "/");
        if(k->channel==16u)q=put_s(q,"ALL");
        else q=put_u(q, (uint32_t)k->channel + 1u);
        cv_text(6, y + 2, &FONT_XS, label, audible ? K_TEXT : K_LINE);
        if(mode)cv_text(6,y+14,&FONT_XS,mode==1?"S":"M",mode==1?K_HOT:K_LINE);
        for (s = 0; s < 16; s++) {
            int32_t x = 40 + s * 12;
            int on = (pat >> s) & 1u;
            uint16_t c;
            if ((unsigned)(block*16+s) >= (unsigned)ks_arr_play_len((uint8_t)i))
                c = K_LINE;
            else
                c = on ? K_HOT : (s % 4 ? K_KEYW : K_HOT_T);
            if(step>=0 && step/16==block && step%16==s &&
               (ui.playing || (kstep_paused && (uint8_t)i==ks_selected_track)))
                cv_round(x-1,y-1,13,24,3,K_COOL); /* display RUNLIGHT */
            cv_round(x, y, 11, 22, 3, c);
        }
    }
}

static void draw_scale(void)
{
    char t[40], *b;
    int i, len = SCALE_LEN[proj.scale];
    if (!proj.scale) {
        cv_text(12, 2, &FONT_B, "Chromatic", K_TEXT);
        cv_text(12, 26, &FONT_XS, "The keys as a keyboard, every note.", K_DIM);
        cv_text(12, 42, &FONT_XS, "KNOB 2 picks a scale.", K_DIM);
        draw_keys(70, 46);
        return;
    }
    b = put_s(t, NOTE_NAME[proj.root]);
    put_s(put_s(b, " "), SCALE_NAME[proj.scale]);
    cv_text(12, 2, &FONT_B, t, K_TEXT);
    b = t;
    for (i = 0; i < len; i++) {
        if (i)
            b = put_s(b, " ");
        b = put_s(b, NOTE_NAME[(proj.root + SCALE_IV[proj.scale][i]) % 12]);
    }
    cv_text(12, 22, &FONT_S, t, K_COOL_D);
    cv_text(12, 42, &FONT_XS, "White keys: the scale. Black keys: its chords.", K_DIM);
    draw_keys(70, 46);
}

static void draw_arp(void)
{
    char t[40], *b;
    uint8_t pool[16];
    int n = eng_pool(pool), i, on = eng_arp_note();
    cv_text(12, 2, &FONT_B, proj.arp_on ? "Arp on" : "Arp off", proj.arp_on ? K_HOT : K_TEXT);
    b = put_s(t, ARP_NAME[proj.arp_mode]);
    b = put_s(put_s(b, "  "), RATE_NAME[proj.arp_rate]);
    b = put_u(put_s(b, "  "), proj.arp_oct);
    b = put_s(b, proj.arp_oct > 1 ? " octaves" : " octave");
    if (proj.arp_latch)
        put_s(b, "  latch");
    cv_text(12, 22, &FONT_XS, t, K_DIM);
    if (!proj.arp_on) {
        cv_text(12, 50, &FONT_XS, "KNOB 1 turns it on; then hold notes or chords.", K_DIM);
        return;
    }
    if (!n)
        cv_text(12, 50, &FONT_XS, "Hold notes or a chord.", K_DIM);
    for (i = 0; i < n; i++) {            /* the notes it plays; the one sounding bright */
        int32_t x = 8 + (i % 8) * 29, y = 46 + (i / 8) * 30;
        int hot = on >= 0 && on % 12 == pool[i] % 12;
        put_note(t, pool[i]);
        cv_round(x, y, 27, 24, 5, hot ? K_HOT : K_HOT_T);
        text_c(x + 13, y + 4, &FONT_XS, t, hot ? K_WHITE : K_TEXT);
    }
}

/* on channel 10, the General MIDI drum names for the common notes */
static const uint8_t DRUM_N[21] = {35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 54, 56, 70, 75};
static int drum_index(int n)                     /* on channel 10, the drum's place in DRUM_N; else -1 */
{
    uint32_t i;
    if (proj.seq_ch == 9)
        for (i = 0; i < sizeof DRUM_N; i++)
            if (DRUM_N[i] == n)
                return (int)i;
    return -1;
}
static void track_name(int n, char *b)
{
    static const char *const S[21] = {"Kick", "Kick", "Rim", "Snare", "Clap", "Snare", "Tom", "Hat", "Tom", "Pedal",
                                      "Tom", "Open", "Tom", "Tom", "Crash", "Tom", "Ride", "Tamb", "Bell", "Shake",
                                      "Clave"};
    int i = drum_index(n);
    if (i >= 0)
        put_s(b, S[i]);
    else
        put_note(b, n);
}

static void draw_seq(void)
{
    int t, s;
    for (t = 0; t < NTRACKS; t++) {
        const track_t *k = &proj.trk[t];
        int32_t y = 5 + t * 30, pos = eng_track_pos(t);
        int sel = t == ui.track;
        char nm[12];
        track_name(k->note, nm);
        if (sel)
            cv_round(2, y - 3, 236, 28, 6, K_PANEL);
        cv_text(6, y + 2, &FONT_XS, nm, k->mute ? K_LINE : sel ? K_TEXT : K_DIM);
        for (s = 0; s < 16; s++) {
            int32_t x = 40 + s * 12;
            int on = k->pat >> s & 1u, cur = s == pos;
            uint16_t c;
            if (s >= k->steps)
                continue;
            c = on ? (k->mute ? K_LINE : cur ? K_TEXT : K_HOT) : cur ? K_DIM : (s % 4 ? K_KEYW : K_HOT_T);
            cv_round(x, y, 11, 22, 3, c);
        }
    }
}

static int bt_bars(int rssi) { return rssi > -60 ? 3 : rssi > -75 ? 2 : 1; }   /* a device's signal */
static void bt_signal(int32_t x, int32_t y, int bars, uint16_t c)
{
    int i;
    for (i = 0; i < 3; i++)
        cv_rect(x + i * 4, y + 8 - i * 3, 3, 3 + i * 3, i < bars ? c : K_LINE);
}

/* Setup's Bluetooth page: a computer or tablet connecting to the FM-1, and (FM1B_BLE_CENTRAL) the MIDI devices
 * around, the one connected. Off, starting or stopped: a word on what to do instead. (Its log: fm1_remote.py.) */
static void draw_bt(void)
{
    char t[40];
    int d = plat_ble_device(), n = btdev_count(), s = plat_ble_state(), i;
    if (proj.ble_off || s != BLE_READY) {
        const char *a, *b;
        if (proj.ble_off) {
            a = "Bluetooth off";
            b = "KNOB 4 turns it on.";
        } else if (s == BLE_OFF) {
            a = "Bluetooth stopped";
            b = "It stopped after a crash. SAVE starts it.";
        } else if (s == BLE_STARTING) {
            a = "Bluetooth starting";
            b = "A moment.";
        } else {
            a = "Bluetooth failed";
            b = "Turn the FM-1 off and on again.";
        }
        cv_text(10, 2, &FONT_S, a, K_TEXT);
        cv_text(10, 24, &FONT_XS, b, K_DIM);
        return;
    }
    cv_text(10, 2, &FONT_S, "Computer or tablet", K_TEXT);
    if (plat_ble_connected()) {
        text_r(230, 4, &FONT_XS, "connected", K_COOL_D);
    } else if (plat_ble_linked()) {                  /* linked, its MIDI never started: it hides the FM-1 */
        text_r(230, 4, &FONT_XS, "connected", K_COOL_D);
        cv_text(10, 20, &FONT_XS, "No MIDI yet. Mac: reopen Audio MIDI Setup.", K_HOT);
    } else {
        text_r(230, 4, &FONT_XS, "not connected", K_DIM);
        cv_text(10, 20, &FONT_XS, "Connect to \"keySTEP ONE\" in its Bluetooth MIDI.", K_DIM);
    }
    if (!FM1B_BLE_CENTRAL)
        return;
    cv_rect(10, 38, 220, 1, K_LINE);
    btdev_title(t);
    cv_text(10, 43, &FONT_S, t, d == BLE_DEV_READY ? K_COOL_D : K_TEXT);
    if (btdev_busy())
        return;
    for (i = 0; i < n && i < 4; i++) {           /* the devices found; KNOB 1's choice marked */
        char name[21];
        int rssi = 0, on = i == btdev_pick();
        plat_ble_found(i, name, &rssi);
        if (on)
            cv_round(6, 61 + i * 14, 228, 13, 4, K_PANEL);
        cv_text(12, 61 + i * 14, &FONT_XS, name, on ? K_TEXT : K_DIM);
        bt_signal(216, 63 + i * 14, bt_bars(rssi), on ? K_TEXT : K_DIM);
    }
    if (!n)
        cv_text(10, 61, &FONT_XS, "Put the device in pairing mode.", K_DIM);
}

static void draw_setup(void)                     /* the settings below; what the ports have carried here */
{
    char t[40], *b;
    int32_t y = 38;
    if (ui.setup_pg == 3) {
        cv_text(10, 2, &FONT_B, "PROGRAM CHANGE", K_TEXT);
        cv_text(10, 28, &FONT_S, "MIDI program routing", K_DIM);
        cv_text(10, 52, &FONT_XS, "KNOB 1 EXT: USB + BLE", K_COOL_D);
        cv_text(10, 69, &FONT_XS, "KNOB 2 INT: SONIVOX GM", K_COOL_D);
        cv_text(10, 86, &FONT_XS, "KNOB 3 SONG CH: 1-16 / ALL", K_COOL_D);
        cv_text(10, 109, &FONT_XS, "SAVE: store settings", K_DIM);
        return;
    }
    if (ui.setup_pg == 2) {
        draw_bt();
        return;
    }
    cv_text(10, 2, &FONT_S, "MIDI", K_TEXT);
    cv_text(236 - text_w(&FONT_XS, APP_NAME " " APP_VERSION), 4, &FONT_XS, APP_NAME " " APP_VERSION, K_DIM);
    b = put_s(t, "USB: ");
    put_s(b, usb_host_status());
    cv_text(10, 20, &FONT_XS, t, usb_host_requested ? K_HOT : K_DIM);
    if (usb_host_requested && usb_host_reason == 1) {
        uint32_t e = plat_usb_host_diag(), err = (e >> 8) & 255u;
        b = put_s(t, "EP0 ERR ");
        if (err) {
            b = put_s(b, "-");
            b = put_u(b, 256u - err);
        } else
            b = put_s(b, "0");
        put_u(put_s(b, "  TRY "), (e >> 16) & 255u);
        cv_text(10, 104, &FONT_XS, t, K_DIM);
    }
    b = put_u(put_s(t, "USB   in "), plat_midi_count(PORT_USB, 0));
    put_u(put_s(b, "   out "), plat_midi_count(PORT_USB, 1));
    cv_text(10, y, &FONT_XS, t, K_DIM);
    if (FM1B_BLE) {
        y += 16;
        if (plat_ble_state() == BLE_OFF || plat_ble_state() == BLE_STARTING) {
            put_s(put_s(t, "Bluetooth   "), bt_stopped() ? "stopped after a crash" : bt_status());
        } else {
            b = put_u(put_s(t, "Bluetooth   in "), plat_midi_count(PORT_BLE, 0));
            put_u(put_s(b, "   out "), plat_midi_count(PORT_BLE, 1));
        }
        cv_text(10, y, &FONT_XS, t, K_DIM);
    }
    put_u(put_s(t, "TRS   in "), plat_midi_count(PORT_TRS, 0));
    cv_text(10, y + 16, &FONT_XS, t, K_DIM);
    cv_text(10, 88, &FONT_XS, usb_host_requested ? "HOST requires a POWERED USB hub!" : "USB device mode; HOST via Setup KNOB 2", K_DIM);
    if (!(usb_host_requested && usb_host_reason == 1))
        cv_text(10, 104, &FONT_XS, usb_host_requested ? "MIDI 1.0 full-speed only" : "OCT-/OCT+ long: update mode", K_DIM);
}

static void draw_sound(void)                     /* the onboard GM synth: what it plays, and its load */
{
    char t[40];
    uint32_t p = ks_program_get(ks_selected_track);
    if (proj.sound == SOUND_OFF) {
        cv_text(12, 2, &FONT_B, "Off", K_TEXT);
        cv_text(12, 24, &FONT_XS, "KNOB 1 turns the onboard GM sounds on.", K_DIM);
        return;
    }
    cv_text(12, 2, &FONT_B, GM_PROGRAM[p], K_TEXT);
    put_u(put_s(t, "GM program "), p + 1u);
    cv_text(12, 22, &FONT_XS, t, K_DIM);
    cv_text(12, 46, &FONT_XS, proj.in_mute ? "Plays the keys (not MIDI in)" : "Plays the keys and MIDI in", K_DIM);
    cv_text(12, 64, &FONT_XS, "PRESETS picks the sound.", K_DIM);
    put_s(put_u(put_s(put_u(put_s(t, "Voices "), gm_voices), "    CPU "), plat_cpu_pct()), "%");
    cv_text(12, 90, &FONT_XS, t, K_DIM);
}


/* DZ ARRANGER - CTL native GUI */
/* KSTEP_TEXTMENU_TEMPLATE_V032: standard text menu for keySTEP ONE.
 * Header supplies the title. No duplicate title, hint line, arc, or empty knob fields.
 * A later menu may reuse this helper with a different list and selection index.
 * Four visible rows, with automatic scrolling if more options are supplied. */
static void kstep_text_menu_rows(const char *const *items, int count, int selection)
{
    int first = 0, i;
    if (count < 1) return;
    if (selection < 0) selection = 0;
    if (selection >= count) selection = count - 1;
    if (count > 4 && selection >= 4) first = selection - 3;
    if (first > count - 4 && count > 4) first = count - 4;
    for (i = first; i < count && i < first + 4; ++i) {
        int32_t y = 2 + (i - first) * 30;
        int is_sel = i == selection;
        cv_round(3, y, 234, 27, 4, is_sel ? K_HOT_T : K_PANEL);
        cv_text(12, vc(&FONT_B, y, 27), &FONT_B, items[i], is_sel ? K_HOT : K_TEXT);
        if (is_sel) text_r(226, vc(&FONT_B, y, 27), &FONT_B, ">", K_HOT);
    }
}

static void draw_step_editor(void)
{
    char t[48],*p;uint32_t i,page=(uint32_t)ks_ed.bank;
    uint16_t mask=ks_arr_edit_mask(ks_selected_track,(uint8_t)page);
    p=put_s(t,"TR");p=put_u(p,(uint32_t)ks_selected_track+1u);
    p=put_s(p,"   P");put_u(p,(uint32_t)ks_pattern_num+1u);
    cv_text(9,2,&FONT_B,t,K_TEXT);
    p=put_u(t,page+1u);p=put_s(p,"/");put_u(p,ks_arr_blocks());
    text_r(232,2,&FONT_S,t,K_HOT);
    (void)mask; (void)i; /* the real 16 white KEY LEDs ARE the step matrix */
    cv_text(9,30,&FONT_XS,"WHITE KEYS = STEP GRID",K_COOL);
    cv_text(9,47,&FONT_XS,"TAP: ON/OFF   HOLD: EDIT",K_TEXT);
    cv_text(9,64,&FONT_XS,"OCT: PAGE   ALGO: TR1-4",K_DIM);
    cv_text(9,81,&FONT_XS,"SEQ + KEYS: NOTE / CHORD",K_DIM);
    if(ks_ed.held_step>=0){
        kse_step_t s;
        if(!ks_arr_editor_get(ks_selected_track,(uint8_t)ks_ed.held_step,&s)){
            p=put_s(t,"STEP ");p=put_u(p,(uint32_t)ks_ed.held_step+1u);
            p=put_s(p,"  NOTES ");put_u(p,s.count);
            if(s.count){
                p=put_s(t,"S");p=put_u(p,(uint32_t)ks_ed.held_step+1u);
                p=put_s(p," N");p=put_note(p,s.note[0]);
                p=put_s(p," V");p=put_u(p,s.velocity[0]);
                p=put_s(p," LEN");put_u(p,s.span?s.span:1u);
                cv_text(9,102,&FONT_XS,t,K_HOT);
            } else cv_text(9,102,&FONT_XS,"STEP EMPTY",K_DIM);
        }
    }
}

static void draw_jukebox(void){
    char a[96],b[96],c[96],name[64],*q;
    const char *items[3];
    put_s(a,"SONG");put_s(b,"ALBUM");
    if(jp.category){
        q=put_s(c,"ALBUM ");if(jp.has_album)put_u(q,jp.album+1u);else put_s(q,"EMPTY");
    }else if(!jp.has_song){put_s(c,"SONG EMPTY");}
    else if(jp.kind==0u){q=put_s(c,"SONG ");put_u(q,jp.song+1u);}
    else {
        q=put_s(c,"M");q=put_u(q,jp.song);q=put_s(q," ");
        if(ksm2_name(jp.song,name,sizeof name)==0){
            unsigned n=0;while(name[n] && n<29u)n++;name[n]=0;put_s(q,name);
        }else put_s(q,"INVALID");
    }
    items[0]=a;items[1]=b;items[2]=c;
    kstep_text_menu_rows(items,3,jp.category);
}

static void draw_arranger(void)
{
    /* Every SEQ page uses exactly the four-row highlighted menu-2 template.
     * ALGO picks the visible row; SELECT rotates the four menu pages. */
    char label[4][48],*p;
    const char *items[4];
    unsigned i;
    int highlighted=(int)ks_selected_track;
    if(sg.active_menu){
        static const char *const ed[] = {"SONG", "ALBUM"};
        kstep_text_menu_rows(ed,2,sg.menu_row);
        return;
    }
    if(ui.arr_pg==2u){
        /* SG_ACTION: the normal SONG editor is replaced by a readable, dark
         * two-row menu, never a yellow overlay. SELECT selects; SEL confirms.
         * White text on K_LINE/K_PANEL has high contrast even on the small LCD. */
        if(sg.stage==SG_ADD){
            /* Same high-contrast dark action panel as the readable DEL/MOVE. */
            cv_round(3,2,234,27,4,K_LINE);
            cv_text(12,vc(&FONT_B,2,27),&FONT_B,"SILENT BLOCK",K_WHITE);
            text_r(226,vc(&FONT_B,2,27),&FONT_B,">",K_WHITE);
            return;
        }
        if(sg.stage==SG_ACTION){
            static const char *const commands[2]={"DEL","MOVE"};
            unsigned cmd;
            for(cmd=0u;cmd<2u;cmd++){
                int y=2+(int)cmd*30;
                int selected=cmd==(unsigned)sg.action;
                cv_round(3,y,234,27,4,selected?K_LINE:K_PANEL);
                cv_text(12,vc(&FONT_B,y,27),&FONT_B,commands[cmd],K_WHITE);
                if(selected)text_r(226,vc(&FONT_B,y,27),&FONT_B,">",K_WHITE);
            }
            return;
        }
        /* Four rows of horizontally scrolling bubbles.  Each ends in a +.
         * The move preview is visual only; SEL commits the reordered data. */
        unsigned tr,j,count,begin;
        const int x0=43,w=44,visible=4;
        for(tr=0;tr<4u;tr++){
            char name[12],cell[14];uint16_t selected=sg.column[tr];
            int y=tr*28+4,active=tr==ks_selected_track;
            count=ks_arr_grid_count((uint8_t)tr);
            begin=selected>=visible?selected-(visible-1u):0u;
            put_u(put_s(name,"TR"),tr+1u);
            cv_text(3,y+6,&FONT_XS,name,active?K_HOT:K_TEXT);
            for(j=0;j<(unsigned)visible && begin+j<=count;j++){
                unsigned col=begin+j;uint16_t pat=ks_arr_grid_get((uint8_t)tr,(uint16_t)col);
                int x=x0+(int)j*w,hi=active&&((sg.stage==SG_MOVE)?col==sg.move_target:col==selected);
                if(active && sg.stage==SG_MOVE && col<count){
                    if(col==sg.move_target)pat=ks_arr_grid_get((uint8_t)tr,sg.move_from);
                    else if(sg.move_from<sg.move_target && col>=sg.move_from && col<sg.move_target)
                        pat=ks_arr_grid_get((uint8_t)tr,(uint16_t)(col+1u));
                    else if(sg.move_from>sg.move_target && col>sg.move_target && col<=sg.move_from)
                        pat=ks_arr_grid_get((uint8_t)tr,(uint16_t)(col-1u));
                }
                cv_round(x,y,40,23,6,hi?K_HOT_T:K_PANEL);
                if(pat==KSG_EMPTY)put_s(cell,"+");
                else if(pat==KSG_SILENT)put_s(cell,"II");
                else put_u(cell,(uint32_t)pat+1u);
                text_c(x+20,vc(&FONT_B,y,23),&FONT_B,cell,hi?K_HOT:K_TEXT);
            }
            if(begin>0u)cv_text(32,y+6,&FONT_XS,"<",K_DIM);
            if(begin+visible<=count)cv_text(225,y+6,&FONT_XS,">",K_DIM);
        }
        return;
    }
    if(ui.arr_pg==3u){
        if(ag.stage==AG_MENU_ADD || ag.stage==AG_MENU_ACTION){
            static const char *const plus[2]={"BPM","INSTRUMENT"};
            static const char *const action[2]={"DEL","MOVE"};
            kstep_text_menu_rows(ag.stage==AG_MENU_ADD?plus:action,2,ag.menu);
        }else if(ag.stage==AG_VALUE){
            const char *label[1]={ag.value_kind==KSAL_BPM?"SET BPM - KNOB 4":"SET INSTRUMENT - KNOB 4"};
            kstep_text_menu_rows(label,1,0);
        }else{
            unsigned i,start=ag.col>=4u?ag.col-3u:0u;
            int y=42;
            char head[28],cell[18];
            put_u(put_s(head,"ALBUM "),ksal.album_id+1u);
            cv_text(4,8,&FONT_B,head,K_TEXT);
            for(i=0;i<4u && i+start<=ksal.count;i++){
                unsigned pos=i+start;int x=8+(int)i*57;
                ksal_entry_t e={0,0,0};
                int hi=(ag.stage==AG_MOVE ? pos==ag.to : pos==ag.col);
                if(pos==ksal.count)put_s(cell,"+");
                else{
                    e=ksal.e[pos];
                    if(ag.stage==AG_MOVE && pos<ksal.count){
                        if(pos==ag.to)e=ksal.e[ag.from];
                        else if(ag.from<ag.to && pos>=ag.from && pos<ag.to)e=ksal.e[pos+1u];
                        else if(ag.from>ag.to && pos>ag.to && pos<=ag.from)e=ksal.e[pos-1u];
                    }
                    if(e.kind==KSAL_S)put_u(put_s(cell,"S"),e.value+1u);
                    else if(e.kind==KSAL_M)put_u(put_s(cell,"M"),e.value);
                    else if(e.kind==KSAL_BPM)put_u(put_s(cell,"BPM"),e.value);
                    else put_u(put_s(cell,"I"),e.value+1u);
                }
                cv_round(x,y,53,31,6,hi?K_HOT_T:K_PANEL);
                text_c(x+26,vc(&FONT_S,y,31),&FONT_S,cell,hi?K_HOT:K_WHITE);
            }
            if(start)cv_text(1,y+8,&FONT_XS,"<",K_DIM);
            if(start+4u<=ksal.count)cv_text(233,y+8,&FONT_XS,">",K_DIM);
        }
        return;
    }
    if(ui.arr_pg==1u){
        char *a[4];
        for(i=0;i<4u;i++){label[i][0]=0;a[i]=label[i];}
        if(pm.stage==PM_OVERWRITE){
            put_s(label[0],"PATTERN EXISTS");
            put_s(label[1],"OVERWRITE? YES");
            put_s(label[2],"OVERWRITE? NO");
            kstep_text_menu_rows((const char *const*)a,3,(int)pm.row+1);
        }else{
            int count=3;
            /* Menu labels remain unchanged while either KNOB spins. */
            put_s(label[0],"CLEAR");put_s(label[1],"LOAD");put_s(label[2],"SAVE");
            if(pm.track<KS_TRACKS){
                if(ks_arr_work_unsaved(pm.track))put_s(label[3],"(UNSAVED)");
            }else if(ks_arr_work_unsaved(0u)||ks_arr_work_unsaved(1u)||
                     ks_arr_work_unsaved(2u)||ks_arr_work_unsaved(3u)){
                put_s(label[3],"(UNSAVED)");
            }
            if(label[3][0])count=4;
            kstep_text_menu_rows((const char *const*)a,count,pm.row);
        }
        return;
    }
    for (i=0;i<4u;i++) {
        items[i]=label[i];
        if (ui.arr_pg == 0u || ui.arr_pg == 1u) {
            p=put_u(put_s(label[i],"TR"),i+1u);
            p=put_u(put_s(p,"    P"),(uint32_t)ks_arr_assigned((uint8_t)i)+1u);
            p=put_u(put_s(p,"   LEN "),ks_arr_play_len((uint8_t)i));
        } else if (ui.arr_pg == 2u) {
            p=put_u(put_s(label[i],"TR"),i+1u);
            p=put_s(p,"   PAT LAYER ");
            p=put_s(p,ks_arr_song_overlay_get((uint8_t)i)?"ON":"OFF");
            (void)p;
        } else {
            unsigned slot=(unsigned)(ks_album_slot/4u)*4u+i;
            p=put_u(put_s(label[i],"SLOT "),slot+1u);
            p=put_s(p,"    SONG ");
            p=put_u(p,(uint32_t)ks_arr_album_get((uint8_t)slot)+1u);
        }
    }
    if (ui.arr_pg == 3u) highlighted=(int)(ks_album_slot%4u);
    ui.arr_row=(uint8_t)highlighted;
    kstep_text_menu_rows(items,4,highlighted);
}


static void draw_main(void)
{
    uint32_t h = hash(2166136261u, ui.view), p;
    int i;
    uint8_t pool[16];
    h = hash(h, ui.keys);
    h = hash(hash(h, ui.heard[0] ^ ui.heard[1] * 3u ^ ui.heard[2] * 5u ^ ui.heard[3] * 7u),
             proj.vel | proj.program << 8 | (uint32_t)(proj.octave + 3) << 16 | ui.sustain << 20 | proj.bank << 21);
    h = hash(hash(h, proj.root | proj.scale << 4 | proj.arp_on << 8 | proj.arp_mode << 9 | proj.arp_rate << 12 |
                         proj.arp_oct << 15 | proj.arp_latch << 17 | ui.track << 18),
             (uint32_t)eng_arp_note());
    h = hash(hash_s(h, ui.last), proj.sound | (uint32_t)proj.chan << 4);
    for (p = 0; p < NPORTS; p++)
        h = hash(h, (uint32_t)lit((int)p, 0) | (uint32_t)lit((int)p, 1) << 1);
    h = hash(h, (uint32_t)plat_ble_state() | (uint32_t)plat_ble_connected() << 4 | (uint32_t)plat_ble_device() << 5 |
                    (uint32_t)plat_ble_linked() << 8);
    for (i = 0, p = (uint32_t)eng_pool(pool); i < (int)p; i++)
        h = hash(h, pool[i]);
    if (ui.view == V_PLAY) {
        const int home_step=ks_home_playhead_step();
        const int block=ks_home_display_block();
        h=hash(h,(uint32_t)ks_selected_track | (uint32_t)ui.home_pg<<8 |
                 (uint32_t)ks_pattern_num<<12);
        if(ui.home_pg){
            int live_count=0,live_note=ks_home_live_note(&live_count);
            h=hash(h,(uint32_t)(live_note+1) | (uint32_t)live_count<<16);
        }
        h=hash(h,(uint32_t)(home_step+1) | (uint32_t)block<<16 |
                 (uint32_t)ks_ed_revision<<20 | (uint32_t)kstep_paused<<31);
        for (i = 0; i < KSTEP_MIDI_TRACKS; i++)
            h = hash(h, (uint32_t)ks_arr_mask((uint8_t)i,(uint8_t)block) |
                        (uint32_t)kstep_midi_tracks[i].channel << 16 |
                        (uint32_t)ks_track_mode_get((uint8_t)i) << 24 |
                        (uint32_t)ks_track_audible((uint8_t)i) << 28);
    }
    if(ui.view==V_LIVE){
        uint16_t next=0u;
        h=hash(h,(uint32_t)ks_selected_track|((uint32_t)ui.live_bank<<8)|
               ((uint32_t)ks_arr_assigned(ks_selected_track)<<12));
        h=hash(h,(uint32_t)ks_arr_live_queued(ks_selected_track,&next)<<31|next);
        h=hash(h,(uint32_t)ks_arr_live_current(ks_selected_track,0));
    }
    if(ui.view==V_STEP)
        h=hash(hash(h,ks_ed_revision), (uint32_t)ks_ed.bank | (uint32_t)ks_selected_track<<8 | (uint32_t)ks_pattern_num<<16);
    if (ui.view == V_SEQ)
        for (i = 0; i < NTRACKS; i++)
            h = hash(hash(h, proj.trk[i].pat | (uint32_t)proj.trk[i].steps << 16 | proj.trk[i].mute << 21 |
                                 (uint32_t)proj.trk[i].note << 22),
                     (uint32_t)eng_track_pos(i));
    if (ui.view == V_SOUND)
        h = hash(hash(h, gm_voices | proj.sound << 8 | proj.in_mute << 12 |
                     (uint32_t)ks_program_get(ks_selected_track) << 16 |
                     (uint32_t)ks_selected_track<<24),plat_cpu_pct());
    if (ui.view == V_SETUP) {
        h = hash(h, (uint32_t)usb_host_requested | (uint32_t)plat_usb_host_state() << 8 |
                       (uint32_t)usb_host_reason << 16);
        h = hash(h, plat_usb_host_diag());
    }
    if (ui.view == V_SETUP && ui.setup_pg == 2) {
        char name[21];
        int k, rssi = 0;
        h = hash(h, (uint32_t)plat_ble_device() | (uint32_t)proj.ble_off << 4 | (uint32_t)btdev_pick() << 8);
        for (k = 0; k < 4 && plat_ble_found(k, name, &rssi); k++)
            h = hash(hash_s(h, name), (uint32_t)bt_bars(rssi));
    } else if (ui.view == V_SETUP) {
        h = hash(hash(h, (uint32_t)proj.ble_off), plat_midi_count(PORT_BLE, 0) ^ plat_midi_count(PORT_BLE, 1) << 16);
        h = hash(hash(h, plat_midi_count(PORT_USB, 0) ^ plat_midi_count(PORT_USB, 1) << 16), plat_midi_count(PORT_TRS, 0));
    }
    h = hash(h, ui.arr_pg | (uint32_t)ks_arr_mode()<<8 | (uint32_t)ks_arr_song_id()<<16);
    h = hash(h, (uint32_t)ks_selected_track | (uint32_t)ui.arr_row<<8 |
                 (uint32_t)ks_album_slot<<16 | (uint32_t)ks_arr_album_count()<<24);
    if (ui.view == V_ARR)
        h=hash(h,(uint32_t)ks_arr_song_events() | (uint32_t)ks_arr_song_length()<<16);
    if(ui.view==V_ARR)h=hash(hash(hash(h,pm.revision),sg.revision),ag.revision);
    if(ui.view==V_ARR){unsigned tr;for(tr=0;tr<KS_TRACKS;tr++)
        h=hash(h,(uint32_t)ks_arr_assigned((uint8_t)tr)|
                    (uint32_t)ks_arr_play_len((uint8_t)tr)<<16);}

    if (h == ui.sig[1])
        return;
    ui.sig[1] = h;
    cv_begin(240, MAIN_H, K_BG);
    switch (ui.view) {
    case V_PLAY: draw_play(); break;
    case V_SCALE: draw_scale(); break;
    case V_ARP: draw_arp(); break;
    case V_SEQ: draw_seq(); break;
    case V_SOUND: draw_sound(); break;
    case V_ARR: draw_arranger(); break;
    case V_STEP: draw_step_editor(); break;
    case V_JUKE: draw_jukebox(); break;
    case V_LIVE: draw_live(); break;
    default: draw_setup(); break;
    }
    /* KSTEP_HOME_TOP_STATUS_V033: HOME keeps the SAME four lanes.
     * Its main band moves 18 px down, replacing the former status position. */
    cv_blit(0, ui.view == V_PLAY ? MAIN_Y + STAT_H : MAIN_Y);
}

/* the status line, on every page: the lowest key, the arp and sustain, and the program */
static void draw_status(void)
{
    char t[48];
    int32_t x;
    uint32_t h = hash(hash(hash(2166136261u, (uint32_t)(proj.octave + 3) | proj.scale << 4 | proj.root << 8 |
                                                 ui.sustain << 12 | ui.sus_lock << 16 | proj.arp_on << 13 | proj.arp_latch << 14 |
                                                 (uint32_t)(eng_mod_val() != 0) << 15),
                           (uint32_t)ks_program_get(ks_selected_track) | proj.sound << 8 |
                           (uint32_t)ks_selected_track<<12 | (uint32_t)ks_arr_channel()<<16), ui.view);
    if (ui.view == V_ARR)
        h = hash(h, (uint32_t)ui.arr_pg | (uint32_t)ks_disk_status<<8 |
                    (uint32_t)ks_arr_song_events()<<12 | (uint32_t)ks_arr_album_count()<<24);
    if (ui.view == V_PLAY)
        h = hash(h, (uint32_t)ks_home_display_block() | (uint32_t)ks_arr_blocks()<<8 |
                     (uint32_t)ks_pattern_num<<16 | (uint32_t)ks_recording<<24);
    /* DIAG v0.5.41: status redraw MUST follow player state. In v0.5.40
     * the draw body checked jp.running, but the status hash ignored it.
     * As a result the live trace could remain completely invisible. */
    if (ui.view == V_JUKE) {
        h = hash(h, (uint32_t)jp.category | (uint32_t)jp.running<<1 |
                       (uint32_t)jp.loop<<2 | (uint32_t)jp.kind<<3 |
                       (uint32_t)ui.playing<<4 | (uint32_t)eng_running()<<5 |
                       (uint32_t)ksm2_active()<<6 |
                       (uint32_t)ks_gp_repeat<<7 | (uint32_t)ks_gp_finished<<8);
        h = hash(h, (uint32_t)jp.once_started | (uint32_t)jp.once_total<<16);
        h = hash(h, (uint32_t)jp.once_remaining | (uint32_t)jp.once_done<<16 |
                       (uint32_t)jp.album_pos<<17);
    }
    if (h == ui.sig[3])
        return;
    ui.sig[3] = h;
    cv_begin(240, STAT_H, K_BG);
    if (ui.view == V_PLAY) {
        /* HOME status: instrument on the left, 16-step block 1/8 on the right.
         * UI PREVIEW ONLY: arranger tracks still have no 32/128-step playback. */
        prog_name(t, ks_program_get(ks_selected_track));
        if (text_w(&FONT_B, t) <= 178)
            cv_text(6, vc(&FONT_B, 0, STAT_H), &FONT_B, t, K_TEXT);
        else
            cv_text(6, vc(&FONT_XS, 0, STAT_H), &FONT_XS, t, K_TEXT);
        {
            char block[12], *q=put_u(block,(uint32_t)ks_home_display_block()+1u);
            q=put_s(q,"/");
            put_u(q,(uint32_t)ks_arr_blocks());
            text_r(234, vc(&FONT_B, 0, STAT_H), &FONT_B, block, K_HOT);
        }
        cv_blit(0, MAIN_Y);
        return;
    }
    if(ui.view==V_JUKE){
        /* v0.5.42: normal status label; leave all v0.5.41 transport,
         * ISR synchronization and redraw logic exactly unchanged. */
        cv_text(8,vc(&FONT_XS,0,STAT_H),&FONT_XS,
                jp.category?"ALBUM / SELECT":"SONG / SELECT",K_TEXT);
        cv_blit(0,STAT_Y);return;
    }
    if (ui.view == V_ARR) {
        char *q;
        if (ks_disk_status != 1u) {
            cv_text(8,vc(&FONT_XS,0,STAT_H),&FONT_XS,
                    ks_disk_status == 2u ? "AB16 NOT MOUNTED / PROTECTED" : "AB16 FLASH ERROR", K_HOT);
            cv_blit(0, STAT_Y);
            return;
        }
        if (ui.arr_pg == 2u) {
            q=put_u(put_s(t,"SONG "),(uint32_t)ks_arr_song_id()+1u);
            q=put_u(put_s(q,"  EVENTS "),ks_arr_song_events());
            q=put_u(put_s(q,"  STEPS "),ks_arr_song_length());
            if(ks_arr_grid_dirty())put_s(q," (UNSAVED)");
        } else if (ui.arr_pg == 3u) {
            q=put_u(put_s(t,"ALBUM "),(uint32_t)ksal.album_id+1u);
            q=put_u(put_s(q,"  BLOCKS "),ksal.count);
            if(ksal.dirty)put_s(q," (UNSAVED)");
        } else if (ui.arr_pg == 1u) {
            q=put_u(put_s(t,"EDIT PATTERN "),(uint32_t)ks_pattern_num+1u);
        } else {
            q=put_s(t,"PLAY ");
            q=put_s(q,ks_arr_mode()==0u?"PATTERN":ks_arr_mode()==1u?"SONG":"ALBUM");
        }
        cv_text(8,vc(&FONT_XS,0,STAT_H),&FONT_XS,t,K_DIM);
        cv_blit(0, STAT_Y);
        return;
    }
    put_note(put_s(t, "Lowest "), proj.scale ? SCALE0_NOTE + proj.root + 12 * proj.octave : KEY0_NOTE + 12 * proj.octave);
    x = cv_text(4, vc(&FONT_XS, 0, 15), &FONT_XS, t, K_DIM) + 8;
    if (proj.arp_on)
        x += tag(x, 0, proj.arp_latch ? "LATCH" : "ARP", K_HOT_T, K_HOT) + 4;
    if (ui.sustain)
        x += tag(x, 0, ui.sus_lock ? "SUS LOCK" : "SUSTAIN", K_HOT_T, K_HOT) + 4;
    if (eng_mod_val())
        tag(x, 0, "MOD", K_HOT_T, K_HOT);
    prog_name(t, ks_program_get(ks_selected_track));
    text_r(236, vc(&FONT_XS, 0, 15), &FONT_XS, t, K_TEXT);
    cv_blit(0, STAT_Y);
}

/* the knob row (X0X's): on a rule, a tag naming what the knobs edit, and dots for the page's sets (Play:
 * the four banks; Sequencer: the four tracks); four knobs, each an arc (a switch: a pip per position) with
 * its value; under them, the footer: what the page's buttons and keys do */
static void knob_tag(char *t, int *dots, int *cur)
{
    static const char *const T[NVIEWS] = {"BANK", "KEYS", "ARP", "TRACK", "SOUND", "SETUP", "ARRANGER", "STEP EDIT", "PLAYER", "LIVE PLAY"};
    char *b = put_s(t, T[ui.view]);
    *dots = 0;
    if (ui.view == V_PLAY) {
        if (fx_cc()) {
            b[0] = ' ';b[1] = (char)('A' + proj.bank);b[2] = 0;
            put_s(b + 2, ": CC NUMBERS");
            *dots=NBANKS;*cur=proj.bank;
        } else {
            put_u(put_s(t,"TRACK TR"),(uint32_t)ks_selected_track+1u);
            *dots=KS_TRACKS;*cur=ks_selected_track;
        }
    } else if (ui.view == V_LIVE) {
        put_u(put_s(t,"TRACK TR"),(uint32_t)ks_selected_track+1u);
        *dots=KS_TRACKS;*cur=ks_selected_track;
    } else if (ui.view == V_SEQ) {
        put_u(put_s(b, " "), ui.track + 1u);
        *dots = NTRACKS;
        *cur = ui.track;
    } else if(ui.view==V_STEP){
        *dots=4;*cur=ks_selected_track;
    } else if (ui.view == V_ARR) {
        *dots = 4;
        *cur = ui.arr_pg;
    } else if (ui.view == V_SETUP) {
        if (ui.setup_pg == 2)
            put_s(t, "BLUETOOTH");
        else if (ui.setup_pg == 3)
            put_s(t, "PROGRAM CHANGE");
        else if (ui.setup_pg == 1)
            put_s(t, "TOUCH");
        *dots = SETUP_PAGES;
        *cur = ui.setup_pg == 3 ? SETUP_PAGES-1 : ui.setup_pg == 2 ? 1 : 0;
    }
}
static void footer_text(const char **a, const char **b, const char **c)
{
    *a = *b = *c = 0;
    switch (ui.view) {
    case V_PLAY: *a = "K1 MIDI CH / K2 SUST"; *b = "K3 TEMPO"; *c = "K4 SOLO/MUTE"; break;
    case V_SCALE:
        if (proj.scale) {
            *a = "WHITE: THE SCALE";
            *b = "BLACK: ITS CHORDS";
        } else {
            *a = "KNOB 2: A SCALE";
        }
        break;
    case V_ARP: *a = "HOLD NOTES OR A CHORD"; *b = "KNOB 1: OFF, ON, LATCH"; break;
    case V_SEQ: *a = "WHITE: STEPS"; *b = "SELECT, BLACK 1-4: TRACK"; *c = "5-8: MUTE"; break;
    case V_SOUND: *a = "PRESETS: THE SOUND"; *b = "MASTER: VOLUME"; break;
    case V_ARR:
        if(ui.arr_pg==3u){
            *a="SELECT: ALBUM COLUMNS";*b="SEL: ADD / DEL / MOVE";
            *c=ag.stage==AG_VALUE?"KNOB4: VALUE":"KNOB1: ALBUM / KNOB2: SONG";
        }else if(ui.arr_pg==1u){
            *a="SELECT: SCROLL";*b="SEL: ENTER";
            *c=pm.stage==PM_OVERWRITE?"SELECT: YES/NO":pm.row==0u?"K1: TRACK / K4: LENGTH":"K1: TRACK / K2: PATTERN";
        }else{
            *a="SELECT: SEQ PAGE";*b="ALGO: TRACK";*c="K1: MENU PAGE";
        }
        break;
    case V_LIVE: *a="BLACK: BANK 1-11";*b="WHITE: PAT 1-16";*c="KNOBS: TR1-4 VOL";break;
    case V_STEP: *a="STEP LEDS"; *b="HOLD: NOTE/VELO/GATE"; *c="REC HOLD: EXIT"; break;
    default:
        if (ui.view == V_SETUP && ui.setup_pg == 3) {
            *a = "GLO: NEXT PAGE";
            *b = "K1: EXT PC / K2: INT PC";
            *c = "K3: SONG CH / SAVE";
            *c = "SAVE: STORE";
            break;
        }
        if (ui.setup_pg == 2) {
            *a = "SELECT: SETUP";
            if (bt_stopped())
                *b = "SAVE: START BLUETOOTH";
            else if (FM1B_BLE_CENTRAL && !proj.ble_off && plat_ble_state() == BLE_READY) {
                *b = "KNOB 1: A DEVICE";
                *c = btdev_busy() ? "SAVE: DISCONNECT" : "SAVE: CONNECT";
            }
            break;
        }
        if (ui.setup_pg) {
            *a = "SELECT: SETUP";
            *b = "CURVE +: FIRMER";
            *c = "AT SENS +: LIGHTER";
            break;
        }
        *a = "SELECT: TOUCH";
        *b = bt_stopped() ? "HOLD SAVE: BLUETOOTH" : "SAVE: NOW";
        break;
    }
}
static int knob_switch(int k)                    /* a few named positions: pips, not an arc */
{
    return k == K_HOMEMODE || k == K_HOMESUST || k == K_ARP || k == K_CLOCK || k == K_GLOW || k == K_SOUND || k == K_SNDIN || k == K_AOCT ||
           k == K_TOUCH || k == K_ATM || k == K_BT || k == K_USBMODE || k == K_PCEXT || k == K_PCINT;
}

static void draw_knobs(void)
{
    uint32_t h = hash(2166136261u, ui.view | (uint32_t)ui.touched << 8 | proj.bank << 16 | ui.track << 20 |
                                       (uint32_t)fx_cc() << 24 | (uint32_t)ui.setup_pg << 25), i;
    int now_touch = plat_ms() < ui.touch_until, dots, cur = 0;
    char t[32];
    for (i = 0; i < 4; i++)
        h = hash(hash(h, (uint32_t)knob_get(view_knob((int)i))), proj.cc[proj.bank][i]);
    h = hash(hash(h, (uint32_t)now_touch), (uint32_t)plat_ble_state() | proj.scale << 4);
    h = hash(h, (uint32_t)proj.tempo | (uint32_t)proj.clock << 16 |
                   (uint32_t)plat_ble_linked() << 20 | (uint32_t)plat_ble_device() << 24);
    if (ui.view == V_SETUP) /* K2 stays HOST/DEVICE; state appears in the main view. */
        h = hash(h, (uint32_t)plat_usb_host_state() | (uint32_t)usb_host_reason << 8);
    if (ui.view == V_SETUP && ui.setup_pg == 2)   /* the Device knob names what the scan finds */
        h = hash(h, (uint32_t)plat_ble_device() | (uint32_t)btdev_count() << 4 | (uint32_t)view_knob(0) << 8);
    h = hash(h, ui.arr_pg | ks_ed_revision<<8);
    h = hash(h, (uint32_t)ks_selected_track | (uint32_t)proj.rsv[6]<<8);
    if (h == ui.sig[2])
        return;
    ui.sig[2] = h;
    cv_begin(240, KNB_H, K_BG);
    /* KSTEP_KNOBS_ALL_MENUS_V034: same bottom knob area on every page.
     * Empty knobs stay empty, assigned knobs show menu-specific values. */
    {   /* the rule; at its right end the tag, then the dots */
        int32_t xd, xt;
        knob_tag(t, &dots, &cur);
        xd = 237 - dots * 5;
        xt = xd - (dots ? 5 : 0) - text_w(&FONT_XS, t);
        cv_rect(0, 5, xt - 5, 1, K_LINE);
        cv_text(xt, vc(&FONT_XS, 0, 11), &FONT_XS, t, K_HOT);
        for (i = 0; i < (uint32_t)dots; i++)
            cv_rect(xd + (int32_t)i * 5, 4, 3, 3, (int)i == cur ? K_HOT : K_LINE);
    }
    /* KSTEP_NO_TINY_HELP_V033: remove only visual three-pixel footer help.
     * Existing knobs, bar meters and their labels/values stay unchanged. */
    for (i = 0; i < 4; i++) {
        int k = view_knob((int)i), lo, hi, v, fill;
        int32_t x = (int32_t)i * 60, cx = x + 30;
        int hot = now_touch && ui.touched == (int)i;
        
        if (i)
            cv_rect(x, 14, 1, 51, K_LINE);
        if (k == K_NONE)
            continue;
        if (hot)
            cv_round(x + 1, 12, 58, 53, 4, K_PANEL);
        knob_range(k, &lo, &hi);
        v = knob_get(k);
        knob_name(k, t);
        text_c(cx, vc(&FONT_XS, 15, 15), &FONT_XS, t, hot ? K_TEXT : K_DIM);
        knob_text(k, t);
        if (text_w(&FONT_M, t) <= 54)
            text_c(cx, vc(&FONT_M, 31, 27), &FONT_M, t, hot ? K_HOT : K_TEXT);
        else if (text_w(&FONT_B, t) <= 54)
            text_c(cx, vc(&FONT_B, 31, 27), &FONT_B, t, hot ? K_HOT : K_TEXT);
        else
            text_c(cx, vc(&FONT_XS, 31, 27), &FONT_XS, t, hot ? K_HOT : K_TEXT);
        fill = hi > lo ? 54 * (v - lo) / (hi - lo) : 54;
        if (fill < 0) fill = 0;
        if (fill > 54) fill = 54;
        cv_rect(x + 3, 60, 54, 4, K_LINE);
        if (fill)
            cv_rect(x + 3, 60, fill, 4, K_HOT);
    }
    /* KSTEP_FOOTER_BPM_BLE_V034: global status, y=68..81. */
    {
        int32_t yy = vc(&FONT_XS, 68, 14);
        put_s(put_u(t, proj.tempo), " BPM");
        cv_text(5, yy, &FONT_XS, t, K_TEXT);
        /* KSTEP_LIVE_EDITOR_V051: always show ALGO's selected MIDI track
         * in the BOTTOM status field, centered between BPM and BLE. */
        put_u(put_s(t,"TR"),(uint32_t)ks_selected_track+1u);
        text_c(120,yy,&FONT_XS,t,K_HOT);
        if (FM1B_BLE) {
            int conn = plat_ble_linked() || plat_ble_device() == BLE_DEV_READY;
            const char *bl = proj.ble_off ? "BLE OFF" : conn ? "BLE LINK" :
                             plat_ble_state() == BLE_READY ? "BLE READY" : "BLE WAIT";
            text_r(236, yy, &FONT_XS, bl, conn ? K_COOL : K_DIM);
        } else {
            text_r(236, yy, &FONT_XS, "BLE OFF", K_DIM);
        }
    }
    cv_blit(0, KNB_Y);
}

/* the lights: bright for what is on, dim (Glow) for the other function buttons so they can be read
 * in the dark; on the Sequencer page the keys show the pattern */
/* HOME: physical 16-key playhead, no screen changes. In PLAY, follow the
 * arranger's step. In PAUSE, follow the REAL source step last triggered by
 * ONE KEY PLAY (not its dense note-event index). Before first tap, freeze. */
static int ks_home_playhead_step(void)
{
    if(kstep_paused && ks_okp.cursor != OKP_NOTE_NONE && ks_okp_lit_step >= 0)
        return ks_okp_lit_step;
    if(ui.playing || kstep_paused)
        return (int)ks_visual_step;
    return -1;
}
static uint32_t ks_home_playhead_mask(void)
{
    int step=ks_home_playhead_step();
    return step < 0 ? 0u : (1u << WHITE_K[(unsigned)step % 16u]);
}
static uint32_t ks_home_block_mask(void)
{
    int step=ks_home_playhead_step();
    unsigned block;
    if(step<0) return 0u;
    block=(unsigned)step/16u;
    return block<11u ? 1u<<BLACK_K[block] : 0u;
}
static void leds(void)
{
    uint32_t b = 1u << VIEW_BTN[ui.view], k = 0, db, dk = 0, i;
    /* KSTEP_SAMPLE_ONLY_ARRANGER_V040: LED REC when recorder is armed. */
    if (ks_recording) b |= 1u << B_REC;
    if (ui.playing && eng_beat_on())             /* playing: PLAY green, bright on the beat */
        b |= LED_PLAY_GREEN;
    if (ui.sustain)
        b |= 1u << B_ENV;
    if (eng_mod_val())
        b |= 1u << B_LFO;
    if (proj.octave < 0)
        b |= 1u << B_OCTDN;
    if (proj.octave > 0)
        b |= 1u << B_OCTUP;
    /* PLAY is green, never its red (that would mean recording, which Ctl does not do): dim green when
     * stopped and between beats */
    db = (((1u << NB) - 1u) & ~b & ~(1u << B_PLAY)) | LED_PLAY_GREEN;
    if(ui.view==V_LIVE){
        uint16_t pat=0u;
        int valid=ks_arr_live_current(ks_selected_track,&pat);
        /* PLAYING position only: no bank/key indication for P177+. */
        if(valid&&pat<176u){
            k=(1u<<BLACK_K[pat/16u])|(1u<<WHITE_K[pat%16u]);
        }
        plat_lights(b,k,db,0,proj.glow?proj.glow:2);
        /* NEXT is text only, avoiding confusion with currently PLAYING. */
        plat_lights_mid(0u,0u);
        return;
    }
    if(ui.view == V_PLAY && (ui.playing || kstep_paused)) {
        /* KSTEP_HOME_DUAL_V0511: white = step within bank; black = 16-step block. */
        k=ks_home_playhead_mask() | ks_home_block_mask();
        plat_lights(b,k,db,0,proj.glow);
        plat_lights_mid(0u,0u); /* no residual editor LEDs */
        return;
    }
    if (ui.view == V_STEP) {
        /* KSTEP_LIVE_EDITOR_V051: LOOM-style physical LED steps, even Glow=OFF.
         * occupied=steady MID, playhead+pressed=BRIGHT; empty=OFF. */
        uint32_t mid=0u;
        uint16_t mask=ks_arr_edit_mask(ks_selected_track,ks_ed.bank);
        uint32_t pos=(uint32_t)ks_visual_step;
        for(i=0;i<16u;++i){
            uint32_t abs=(uint32_t)ks_ed.bank*16u+i;
            uint32_t bit=1u<<WHITE_K[i];
            if(abs>=ks_arr_len())continue;
            if(mask>>i & 1u)mid|=bit;
            if(ui.keys & bit)k|=bit;
            if(ui.playing && pos==abs)k|=bit;
        }
        /* KSTEP_LOOM_EDITOR_FIX_V052: NO black-key track LEDs.
         * ALGO selection is shown in the center footer instead. */
        plat_lights(b,k,db,0,proj.glow?proj.glow:2);
        plat_lights_mid(0u,mid & ~k);
        return;
    }
    if (ui.view == V_SEQ) {
        const track_t *t = trk();
        int pos = eng_track_pos(ui.track);
        for (i = 0; i < 16; i++) {
            uint32_t m = 1u << WHITE_K[i];
            int on = t->pat >> i & 1u;
            if (i >= t->steps)
                continue;
            if ((int)i == pos)
                on = !on;                /* the playhead: the step turned over */
            if (on)
                k |= m;
            else
                dk |= m;
        }
        for (i = 0; i < 4; i++) {
            if (i == ui.track)
                k |= 1u << BLACK_K[i];
            else
                dk |= 1u << BLACK_K[i];
            if (!proj.trk[i].mute)
                k |= 1u << BLACK_K[4 + i];
        }
    } else {
        for (i = 0; i < NKEYS; i++) {
            if ((ui.keys >> i & 1u) || key_heard((int)i))
                k |= 1u << i;
            else if (key_root((int)i))
                dk |= 1u << i;
        }
    }
    plat_lights(b, k, db, dk, proj.glow);
    plat_lights_mid(0u,0u); /* remove stale STEP mid LEDs when leaving editor */
}

/* AUTOSAVE: a few seconds after a change, when nothing is held or touched (a flash erase stops
 * everything for a moment) */
#define AUTOSAVE_QUIET 4000u
static void autosave(void)
{
    uint32_t now = plat_ms();
    if (!ui.dirty || ui.btn || ui.keys || ui.playing || now - ui.act_t < AUTOSAVE_QUIET)
        return;
    if (project_save() == 0)
        ui.dirty = 0;
}

/* ------------------------------------------------------ screen reader --- */
/* The screen as text for a screen reader on the computer (announce.c; fm1-reader): a page once it settles,
 * with its knobs; a knob's value as it turns (a controller with its name: "CC 74 cutoff 64"); the channel and
 * the program; the Sequencer's steps and mutes; play and stop; what a button did (say). WHERE (the reader asks, or has
 * just started listening) is everything on the screen. The pressure bar is never said (it changes all the
 * time), nor the MIDI counters but on WHERE. Nothing is built while no reader listens. The screen's short
 * words are said in full by the words file (firmware/words.tsv); the rest is said here in words. */
static char sp_buf[ANN_MAX + 1] SP_POOL;
static uint32_t sp_n;
static const char *const SCALE_SAID[NSCALES] = {"chromatic", "major", "minor", "dorian", "mixolydian",
                                                "harmonic minor", "pentatonic", "pentatonic minor", "blues"};
static const char *const CLOCK_SAID[NCLK] = {"internal and sent out", "internal only", "external"};
static const char *const AT_SAID[NAT] = {"off", "per note", "for the channel"};
static const char *const DRUM_SAID[21] = {"acoustic kick", "kick", "rimshot", "snare", "clap", "electric snare",
                                          "low floor tom", "closed hat", "high floor tom", "pedal hat", "low tom",
                                          "open hat", "low mid tom", "high mid tom", "crash", "high tom", "ride",
                                          "tambourine", "cowbell", "maracas", "claves"};
/* the controllers with a name of their own (MIDI 1.0) */
static const uint8_t CCN_N[25] = {0, 1, 2, 4, 5, 6, 7, 8, 10, 11, 64, 65, 66, 67, 68, 71, 72, 73, 74, 75, 76, 77,
                                  78, 91, 93};
static const char *const CCN_S[25] = {"bank select", "mod wheel", "breath", "foot pedal", "portamento time",
                                      "data entry", "volume", "balance", "pan", "expression", "sustain",
                                      "portamento", "sostenuto", "soft pedal", "legato", "resonance", "release",
                                      "attack", "cutoff", "decay", "vibrato rate", "vibrato depth", "vibrato delay",
                                      "reverb", "chorus"};

static void sp_cat(const char *s)
{
    while (*s && sp_n < ANN_MAX)
        sp_buf[sp_n++] = *s++;
    sp_buf[sp_n] = 0;
}
static void sp_num(uint32_t v)
{
    char t[12];
    put_u(t, v);
    sp_cat(t);
}
static void sp_note(int n)                       /* "C4", "C sharp minus 1" (the screen's "C#-1") */
{
    char t[8];
    if (n < 12) {
        sp_cat(NOTE_NAME[n % 12]);
        sp_cat(" minus 1");
        return;
    }
    put_note(t, n);
    sp_cat(t);
}
static int sp_cc(uint32_t n)                     /* "CC 74 cutoff"; 1 if it has a name */
{
    uint32_t i;
    sp_cat("CC ");
    sp_num(n);
    for (i = 0; i < sizeof CCN_N; i++)
        if (CCN_N[i] == n) {
            sp_cat(" ");
            sp_cat(CCN_S[i]);
            return 1;
        }
    return 0;
}
static void sp_knob(int k)                       /* a knob's readout: its name, then its value */
{
    char t[24];
    int v = knob_get(k), d;
    if (k <= K_VAL3) {
        sp_cat(sp_cc(proj.cc[proj.bank][k - K_VAL0]) ? " " : " value ");
        sp_num((uint32_t)v);
        return;
    }
    if (k == K_MASTER) {
        sp_cat("Master ");
        if (v)
            sp_cc((uint32_t)v);
        else
            sp_cat("volume");
        return;
    }
    knob_name(k, t);
    sp_cat(t);
    sp_cat(k <= K_CC3 ? ", " : " ");
    switch (k) {
    case K_CC0: case K_CC1: case K_CC2: case K_CC3: sp_cc((uint32_t)v); return;
    case K_SCALE: sp_cat(SCALE_SAID[v]); return;
    case K_CLOCK: sp_cat(CLOCK_SAID[v]); return;
    case K_TOUCH: sp_cat(v ? "from the keys" : "fixed"); return;
    case K_ATM: sp_cat(AT_SAID[v]); return;
    case K_TEMPO:
        if (proj.clock == CLK_EXT) {
            sp_cat("external clock");
            return;
        }
        break;
    case K_NOTE:
        if ((d = drum_index(v)) >= 0) {
            sp_cat(DRUM_SAID[d]);
            return;
        }
        break;
    default: break;
    }
    knob_text(k, t);
    sp_cat(t);
}

static void sp_program(void)                     /* "Program 53, Choir Aahs" */
{
    sp_n = 0;
    sp_cat("Program ");
    sp_num(proj.program + 1u);
    if (proj.sound != SOUND_OFF) {
        sp_cat(", ");
        sp_cat(proj.chan == 9 ? "drum kit" : GM_PROGRAM[proj.program]);
    }
}
static void sp_channel(void)
{
    sp_n = 0;
    sp_cat("Channel ");
    sp_num(proj.chan + 1u);
    if (proj.chan == 9 && proj.sound != SOUND_OFF)
        sp_cat(", drum kit");
}

/* the page's name: "Play page, bank A", "Sequencer page, track 2", "Touch page" */
static void sp_page(void)
{
    char t[2] = {(char)('A' + proj.bank), 0};
    if (fx_cc()) {
        sp_cat("Controller numbers, bank ");
        sp_cat(t);
        return;
    }
    if (ui.view == V_SETUP && ui.setup_pg == 3) {
        sp_cat("Program Change page");
        return;
    }
    if (ui.view == V_SETUP && ui.setup_pg == 2) {
        int d = plat_ble_device();
        sp_cat("Bluetooth page");
        if (proj.ble_off || plat_ble_state() != BLE_READY) {
            sp_cat(", Bluetooth ");
            sp_cat(bt_stopped() ? "stopped after a crash. SAVE starts it" : bt_status());
            return;
        }
        sp_cat(plat_ble_connected() ? ", a computer connected" : plat_ble_linked() ?
               ", a computer connected, its MIDI not started. On a Mac, reopen Audio MIDI Setup" : ", no computer connected");
        if (d == BLE_DEV_READY) {
            sp_cat(", connected to ");
            sp_cat(btdev_name());
        } else if (d == BLE_DEV_CONNECTING || d == BLE_DEV_SEARCHING) {
            sp_cat(", connecting to ");
            sp_cat(btdev_name());
        } else if (FM1B_BLE_CENTRAL) {
            sp_cat(", ");
            sp_num((uint32_t)btdev_count());
            sp_cat(btdev_count() == 1 ? " device found" : " devices found");
        }
        return;
    }
    if (ui.view == V_SETUP && ui.setup_pg) {
        sp_cat("Touch page");
        return;
    }
    sp_cat(VIEW_NAME[ui.view]);
    sp_cat(" page");
    if (ui.view == V_PLAY) {
        sp_cat(", bank ");
        sp_cat(t);
    } else if (ui.view == V_SEQ) {
        sp_cat(", track ");
        sp_num(ui.track + 1u);
    }
}

/* the page and its knobs; all: what the page shows, the top bar and the status line too */
static void sp_screen(int all)
{
    int i, n = 0;
    sp_n = 0;
    sp_buf[0] = 0;
    sp_page();
    for (i = 0; i < 4; i++) {
        int k = view_knob(i);
        if (k == K_NONE)
            continue;
        sp_cat(n++ ? ", " : ": ");
        sp_knob(k);
    }
    if (!all)
        return;
    switch (ui.view) {
    case V_SCALE:
        if (!proj.scale) {
            sp_cat(". The keys play every note");
            break;
        }
        sp_cat(". Notes ");                      /* the white keys' (the black keys: the chords on them) */
        for (i = 0; i < SCALE_LEN[proj.scale]; i++) {
            const char *nm = NOTE_NAME[(proj.root + SCALE_IV[proj.scale][i]) % 12];
            if (i)                               /* "D and D sharp": a note and its sharp, together */
                sp_cat(nm[0] == NOTE_NAME[(proj.root + SCALE_IV[proj.scale][i - 1]) % 12][0] ? " and " : ", ");
            sp_cat(nm);
        }
        break;
    case V_SEQ: {
        const track_t *t = trk();
        sp_cat(". Steps on: ");
        for (i = 0, n = 0; i < t->steps; i++)
            if (t->pat >> i & 1u) {
                if (n++)
                    sp_cat(", ");
                sp_num((uint32_t)i + 1u);
            }
        if (!n)
            sp_cat("none");
        for (i = 0; i < NTRACKS; i++)
            if (proj.trk[i].mute) {
                sp_cat(". Track ");
                sp_num((uint32_t)i + 1u);
                sp_cat(" muted");
            }
        break;
    }
    case V_SOUND:
        if (proj.sound == SOUND_OFF) {
            sp_cat(". The onboard sounds are off");
            break;
        }
        sp_cat(". Sound ");
        if (proj.chan == 9) {
            sp_cat("drum kit");
        } else {
            sp_cat(GM_PROGRAM[gm_program[proj.chan]]);
            sp_cat(", program ");
            sp_num(gm_program[proj.chan] + 1u);
        }
        sp_cat(proj.in_mute ? ". It plays the keys, not MIDI in" : ". It plays the keys and MIDI in");
        break;
    case V_SETUP:
        if (ui.setup_pg == 2 || ui.setup_pg == 3)
            break;
        if (ui.setup_pg) {
            if (!touch_live)
                sp_cat(". Every note plays at the velocity set on the Keys page");
            else if (ui.last_vel) {
                sp_cat(". Last velocity ");
                sp_num(ui.last_vel);
            }
            break;
        }
        if (FM1B_BLE) {
            int s = plat_ble_state();
            sp_cat(". Bluetooth ");
            sp_cat(bt_stopped() ? "stopped after a crash" : bt_status());
            if (s != BLE_OFF && s != BLE_STARTING) {
                sp_cat(", in ");
                sp_num(plat_midi_count(PORT_BLE, 0));
                sp_cat(", out ");
                sp_num(plat_midi_count(PORT_BLE, 1));
            }
        }
        sp_cat(". USB in ");
        sp_num(plat_midi_count(PORT_USB, 0));
        sp_cat(", out ");
        sp_num(plat_midi_count(PORT_USB, 1));
        sp_cat(". TRS in ");
        sp_num(plat_midi_count(PORT_TRS, 0));
        sp_cat(". CTL-1 " APP_VERSION);
        break;
    default: break;
    }
    sp_cat(". Channel ");                        /* the top bar */
    sp_num(proj.chan + 1u);
    if (proj.clock == CLK_EXT) {
        sp_cat(", external clock");
    } else {
        sp_cat(", tempo ");
        sp_num(proj.tempo);
    }
    sp_cat(ui.playing ? ", playing" : ", stopped");
    if (ui.view == V_SEQ || ui.view == V_SETUP)       /* the status line where it matters (and fits) */
        return;
    if (ui.view != V_SOUND) {
        sp_cat(". Lowest key ");
        sp_note(proj.scale ? SCALE0_NOTE + proj.root + 12 * proj.octave : KEY0_NOTE + 12 * proj.octave);
    }
    if (ui.view == V_PLAY || ui.view == V_ARP) {
        sp_cat(". Program ");
        sp_num(proj.program + 1u);
        if (proj.sound != SOUND_OFF) {
            sp_cat(", ");
            sp_cat(proj.chan == 9 ? "drum kit" : GM_PROGRAM[proj.program]);
        }
    }
    if (proj.arp_on && ui.view != V_ARP)
        sp_cat(proj.arp_latch ? ". Arp latched" : ". Arp on");
    if (ui.sustain)
        sp_cat(ui.sus_lock ? ". Sustain locked" : ". Sustain on");
}

static uint32_t sp_ctx_sig(void)
{
    uint32_t h = hash(2166136261u, ui.view | (uint32_t)fx_cc() << 8);
    if (ui.view == V_PLAY)
        h = hash(h, proj.bank);
    else if (ui.view == V_SEQ)
        h = hash(h, ui.track);
    else if (ui.view == V_SETUP)
        h = hash(h, ui.setup_pg);
    return h;
}
static void sp_snap(void)
{
    int i;
    for (i = 0; i < 4; i++)
        sp.val[i] = (int16_t)(view_knob(i) == K_NONE ? 0 : knob_get(view_knob(i)));
    sp.chan = proj.chan;
    sp.program = proj.program;
    sp.playing = ui.playing;
    sp.sus_lock = ui.sus_lock;
    sp.ble = (uint8_t)plat_ble_connected();
    sp.touch_until = ui.touch_until;
}

static void speak(void)
{
    uint32_t now = plat_ms(), c;
    int where = ann_where(), i, changed = -1, ev = sp.ev;
    if (!ann_on()) {
        sp.started = 0;
        sp_said = 0;
        sp.ev = SE_NONE;
        return;
    }
    sp.ev = SE_NONE;
    if (!sp.started || where) {                       /* everything on the screen */
        sp.started = 1;
        sp.ctx = sp_ctx_sig();
        sp_snap();
        sp_screen(1);
        ann_say(sp_buf, 0, ANN_NOW);
        sp_said = 0;
        return;
    }
    if (sp_said) {                                    /* a button said what it did (and what it changed) */
        sp_said = 0;
        sp_snap();
        return;
    }
    if (ev == SE_PANIC)
        sp.sus_lock = ui.sus_lock;                    /* (all notes off says it: sustain off too) */
    if (ui.playing != sp.playing) {
        sp.playing = ui.playing;
        ann_say(ui.playing ? "Play" : "Stop", 0, proj.clock == CLK_EXT ? 0u : ANN_NOW);   /* from outside: queued */
    }
    if (FM1B_BLE && plat_ble_connected() != sp.ble) {   /* not the user's doing: queued */
        sp.ble = (uint8_t)plat_ble_connected();
        ann_say(sp.ble ? "Computer connected" : "Computer disconnected", 0, 0);
    }
    if (ui.sus_lock != sp.sus_lock) {                 /* locking says SUSTAIN LOCKED itself */
        sp.sus_lock = ui.sus_lock;
        if (!ui.sus_lock)
            ann_say("Sustain unlocked", 0, ANN_NOW);
    }
    c = sp_ctx_sig();
    if (c != sp.ctx) {                                /* a new page: said when it settles */
        sp.ctx = c;
        sp_snap();
        sp_screen(0);
        ann_later(sp_buf, ANN_NOW, 200u, now);
        return;
    }
    if (proj.chan != sp.chan) {                       /* ALGORITHM */
        sp_snap();
        sp_channel();
        ann_later(sp_buf, ANN_NOW, 120u, now);
        return;
    }
    if (proj.program != sp.program) {                 /* PRESETS */
        sp_snap();
        sp_program();
        ann_later(sp_buf, ANN_NOW, 120u, now);
        return;
    }
    for (i = 0; i < 4; i++) {                         /* a knob turning */
        int k = view_knob(i), v = k == K_NONE ? 0 : knob_get(k);
        if (v != sp.val[i] && changed < 0)
            changed = i;
    }
    if (changed < 0 && ui.touch_until != sp.touch_until && ui.touched >= 0 && view_knob(ui.touched) != K_NONE)
        changed = ui.touched;                         /* turned, but at its end (or Tempo on an outside clock) */
    if (changed >= 0) {
        sp_snap();
        sp_n = 0;
        sp_knob(view_knob(changed));
        ann_later(sp_buf, ANN_NOW, 120u, now);
        return;
    }
    switch (ev) {                                     /* the keys */
    case SE_STEP:
        sp_n = 0;
        sp_cat("Step ");
        sp_num(sp.ev_arg + 1u);
        sp_cat(sp.ev_on ? " on" : " off");
        ann_say(sp_buf, 0, ANN_NOW);
        break;
    case SE_STEP_OUT:
        sp_n = 0;
        sp_cat("Step ");
        sp_num(sp.ev_arg + 1u);
        sp_cat(" not set: track ");
        sp_num(ui.track + 1u);
        sp_cat(" has ");
        sp_num(trk()->steps);
        sp_cat(" steps");
        ann_say(sp_buf, 0, ANN_NOW | ANN_KEEP);
        break;
    case SE_MUTE:
        sp_n = 0;
        sp_cat("Track ");
        sp_num(sp.ev_arg + 1u);
        sp_cat(sp.ev_on ? " muted" : " unmuted");
        ann_say(sp_buf, 0, ANN_NOW);
        break;
    case SE_PANIC: ann_say("All notes off", 0, ANN_NOW); break;
    default: break;
    }
}

#ifdef PLAT_HOST
/* every page, knob value, chord and message as a screen reader gets them (the "speakdump" command):
 * KIND <tab> the text sent (fm1-reader/tools/audit.mjs puts it in words with web/words/ and checks it) */
static void sp_print(const char *kind, const char *t) { printf("%s\t%s\n", kind, t); }
static void sp_dump_page(void)
{
    project_t keep = proj;
    int i, x, lo, hi;
    sp_screen(0);
    sp_print("SCREEN", sp_buf);
    sp_screen(1);
    sp_print("WHERE", sp_buf);
    for (i = 0; i < 4; i++) {
        int k = view_knob(i);
        if (k == K_NONE)
            continue;
        knob_range(k, &lo, &hi);
        for (x = lo; x <= hi; x++) {
            knob_set(k, x);
            sp_n = 0;
            sp_knob(k);
            sp_print("KNOB", sp_buf);
        }
        proj = keep;
        touch_apply();
    }
}
static void speak_dump(void)
{
    static const char *const MSG[][2] = {{"LOWEST KEY", "F#3"}, {"LOWEST KEY", "C0"}, {"SUSTAIN", "LOCKED"},
                                         {"SAVED", 0}, {"BLUETOOTH", "STARTING"}, {"BLUETOOTH OFF", "AFTER A CRASH"},
                                         {"BLUETOOTH", "ON"}, {"BLUETOOTH", "OFF"}, {"UPDATE MODE IN", "3.."},
                                         {"UPDATE MODE", "CANCELLED"}};
    static uint8_t keep_ui[sizeof ui];
    project_t keep = proj;
    int v, q, x, i, k;
    memcpy(keep_ui, &ui, sizeof ui);
    for (v = 0; v < NVIEWS; v++) {                    /* every page (Play: each bank, and FX held; Sequencer:
                                                       * each track, on channel 10 and on another) */
        ui.view = (uint8_t)v;
        for (q = 0; q < (v == V_PLAY ? 2 * NBANKS : v == V_SEQ ? 2 * NTRACKS : v == V_SETUP ? SETUP_PAGES : 1); q++) {
            ui.btn = 0;
            ui.setup_pg = (uint8_t)(v == V_SETUP ? (q==0 ? 0 : FM1B_BLE && q==1 ? 2 : 3) : 0);
            if (v == V_PLAY) {
                proj.bank = (uint8_t)(q % NBANKS);
                ui.btn = q >= NBANKS ? 1u << B_FX : 0u;
                ui.fx_used = 1;
            } else if (v == V_SEQ) {
                ui.track = (uint8_t)(q % NTRACKS);
                proj.seq_ch = (uint8_t)(q < NTRACKS ? 9 : 0);
            }
            sp_dump_page();
            proj = keep;
        }
    }
    ui.btn = 0;
    ui.view = V_PLAY;                                 /* WHERE in other states */
    for (x = 0; x < 8; x++) {
        proj.sound = (uint8_t)(x & 1 ? SOUND_OFF : SOUND_IN);
        proj.chan = (uint8_t)(x & 2 ? 9 : 0);
        proj.clock = (uint8_t)(x & 4 ? CLK_EXT : CLK_INT_OUT);
        proj.arp_on = (uint8_t)(x & 1);
        proj.arp_latch = (uint8_t)(x >> 1 & 1);
        ui.playing = (uint8_t)(x >> 2 & 1);
        ui.sustain = (uint8_t)(x >> 1 & 1);
        ui.sus_lock = (uint8_t)(x & 1);
        sp_screen(1);
        sp_print("WHERE", sp_buf);
        sp_n = 0;
        sp_knob(K_TEMPO);
        sp_print("KNOB", sp_buf);
    }
    proj = keep;
    ui.playing = ui.sustain = ui.sus_lock = 0;
    ui.view = V_SCALE;
    for (x = 0; x < NSCALES; x++)
        for (i = 0; i < 12; i++) {
            proj.scale = (uint8_t)x;
            proj.root = (uint8_t)i;
            sp_screen(1);
            sp_print("WHERE", sp_buf);
        }
    proj = keep;
    ui.view = V_SEQ;                                  /* the steps: none, all, muted */
    for (x = 0; x < 3; x++) {
        proj.trk[0].pat = x == 1 ? 0xFFFFu : 0u;
        proj.trk[1].mute = proj.trk[3].mute = (uint8_t)(x == 2);
        sp_screen(1);
        sp_print("WHERE", sp_buf);
    }
    proj = keep;
    ui.view = V_SETUP;                                /* the Touch page: off, on with a velocity */
    ui.setup_pg = 1;
    for (x = 0; x < 2; x++) {
        proj.touch = (uint8_t)x;
        touch_apply();
        ui.last_vel = 87;
        sp_screen(1);
        sp_print("WHERE", sp_buf);
    }
    proj = keep;
    touch_apply();
    for (x = 0; x < 128; x++) {                       /* PRESETS, ALGORITHM */
        proj.program = (uint8_t)x;
        proj.sound = SOUND_IN;
        sp_program();
        sp_print("VALUE", sp_buf);
    }
    proj.sound = SOUND_OFF;
    sp_program();
    sp_print("VALUE", sp_buf);
    proj.sound = SOUND_IN;
    for (x = 0; x < 16; x++) {
        proj.chan = (uint8_t)x;
        sp_channel();
        sp_print("VALUE", sp_buf);
        proj.chan = 9;
        sp_program();
        sp_print("VALUE", sp_buf);
    }
    proj = keep;
    ui.view = V_SOUND;
    k = gm_program[0];
    for (x = 0; x < 16; x++) {
        gm_program[0] = (uint8_t)(x * 8 + 3);
        proj.in_mute = (uint8_t)(x & 1);
        sp_screen(1);
        sp_print("WHERE", sp_buf);
    }
    gm_program[0] = (uint8_t)k;
    for (i = 0; i < (int)(sizeof MSG / sizeof MSG[0]); i++) {
        msg_text(MSG[i][0], MSG[i][1], sp_buf);
        sp_print("MSG", sp_buf);
    }
    msg_text("NOT SAVED", "FLASH ERROR", sp_buf);
    sp_print("WARN", sp_buf);
    sp_print("WARN", "Step 13 not set: track 4 has 12 steps");
    {
        static const char *const SAID[] = {"Play", "Stop", "Computer connected", "Computer disconnected",
                                           "Sustain unlocked", "All notes off", "Step 5 on", "Step 5 off",
                                           "Track 2 muted", "Track 2 unmuted"};
        for (i = 0; i < (int)(sizeof SAID / sizeof SAID[0]); i++)
            sp_print("MSG", SAID[i]);
    }
    proj = keep;
    memcpy(&ui, keep_ui, sizeof ui);
    touch_apply();
}
#endif

void ui_init(void)
{
    usb_host_requested = 0;  /* Every power-on / reboot starts in CLIENT. */
    usb_host_reason = 0;
    ks_okp_release(); ks_okp_ready=0;ks_okp_kind=255u;ks_okp_song=65535u;ks_okp_tr=255u;ks_okp_pat=65535u;kstep_paused=0;ks_okp_lit_step=-1;
    memset(&ui, 0, sizeof ui);
    ks_sustain_track = 255u;
    ui.touched = -1;
    ui.master_sent = -1;
    ui.master_raw = (int32_t)plat_master();
    lcd_fill(0, 0, 240, 240, K_BG);
    ks_ed_init();
    ui.live_bank=0u;
    jp.loop=1u;jp.category=0u;jp_first();
    /* digital keys only; no touch ADC setup */
    synth_program();                     /* the saved program, which the screen shows */
}

static void dev_scan(void);
void ui_frame(void)
{
    input();
    usb_host_events();
    dev_scan();
    autosave();
    draw_header();
    plat_yield();
    draw_main();
    plat_yield();
    draw_status();
    draw_knobs();
    leds();
    speak();                             /* a screen reader: what changed (announce.c) */
    ann_tick(plat_ms());
    ui.frame++;
}

/* the Bluetooth page scans for devices while it is up (fm1-base's lib/btdev); a device connected, given up or lost
 * is said */
static void dev_scan(void)
{
    int ev;
    btdev_page(ui.view == V_SETUP && ui.setup_pg == 2 && !proj.ble_off);
    while ((ev = btdev_event()) != BTDEV_NONE)
        if (ev == BTDEV_CONNECTED || ev == BTDEV_NO_ANSWER || ev == BTDEV_LOST)
            say(btdev_word(ev), btdev_name());
        else if ((ev == BTDEV_COMPUTER || ev == BTDEV_COMPUTER_GONE) && !proj.ble_off)   /* (not switching off) */
            say_as("COMPUTER", ev == BTDEV_COMPUTER ? "CONNECTED" : "DISCONNECTED", 0);
}
void ui_input_only(void)
{
    input();
    usb_host_events();
    dev_scan();
}
