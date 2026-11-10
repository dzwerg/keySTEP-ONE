/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Ctl settings: one flash object (OBJ_PROJ). Settings of another format, or none, give the defaults.
 * Part of the unity build, after app.h. */
project_t proj;
/* KSTEP_TRACK_LOCAL_SOUND_V053: reuse six ALREADY RESERVED project bytes.
 * No project struct/flash layout change. Marker keeps old CTL settings valid.
 * rsv[0,1] = "KS", rsv[2..5] = GM presets for TR1..TR4. */
void ks_program_init(void)
{
    unsigned t;
    if(proj.rsv[0]=='K' && proj.rsv[1]=='S')return;
    proj.rsv[0]='K';proj.rsv[1]='S';
    for(t=0;t<4u;t++)proj.rsv[2u+t]=(uint8_t)(t==0u?proj.program:0u);
    proj.rsv[6]=0u; /* four 2-bit normal/solo/mute states, default normal */
}
uint8_t ks_program_get(uint8_t tr)
{
    if(tr>=4u)return 0u;
    if(proj.rsv[0]!='K'||proj.rsv[1]!='S')
        return tr==0u?proj.program:0u;
    return proj.rsv[2u+tr];
}
void ks_program_set(uint8_t tr,uint8_t program)
{
    if(tr>=4u||program>127u)return;
    ks_program_init();
    proj.rsv[2u+tr]=program;
}
/* KSTEP_HOME_TRACK_KNOBS_V054: four two-bit mixer states, stored in existing project byte. */
uint8_t ks_track_mode_get(uint8_t track)
{
    uint8_t v;
    if(track>=4u || proj.rsv[0]!='K'||proj.rsv[1]!='S')return 0u;
    v=(uint8_t)((proj.rsv[6]>>(track*2u))&3u);
    return v<3u?v:0u;
}
int ks_track_audible(uint8_t track)
{
    unsigned t;int solo=0;
    if(track>=4u)return 0;
    for(t=0;t<4u;++t)if(ks_track_mode_get((uint8_t)t)==1u)solo=1;
    return solo ? ks_track_mode_get(track)==1u : ks_track_mode_get(track)!=2u;
}
void ks_track_mode_set(uint8_t track,uint8_t mode)
{
    uint8_t shift,mask;
    if(track>=4u||mode>2u)return;
    ks_program_init();
    shift=(uint8_t)(track*2u);mask=(uint8_t)(3u<<shift);
    proj.rsv[6]=(uint8_t)((proj.rsv[6]&~mask)|((uint8_t)(mode<<shift)));
}

/* KSTEP_PC_OUT_V036: two separate, persistent Program Change switches.
 * Reuse only project_t's remaining reserved bytes, preserving the exact
 * existing flash object size, all 4 instrument presets and mute/solo state.
 * rsv[8] marks migration; old project settings default EXT OFF, INT ON.
 * rsv[7] bit0 = external USB/BLE PC; bit1 = internal Sonivox PC.
 * rsv[7] bits2..6 = SONGPLAYER ALBUM INSTRUMENT PC channel (0..15, 16=ALL).
 */
#define KS_PC_MARKER 0x36u
#define KS_PC_EXT 0x01u
#define KS_PC_INT 0x02u
static void ks_pc_defaults_if_needed(void)
{
    if(proj.rsv[8] != KS_PC_MARKER){
        proj.rsv[7] = (uint8_t)(KS_PC_INT | (16u << 2)); /* defaults EXT OFF, INT ON, ALL */
        proj.rsv[8] = KS_PC_MARKER;
    }
}
uint8_t ks_pc_out_external(void)
{
    return proj.rsv[8] == KS_PC_MARKER ? !!(proj.rsv[7] & KS_PC_EXT) : 0u;
}
uint8_t ks_pc_out_internal(void)
{
    return proj.rsv[8] == KS_PC_MARKER ? !!(proj.rsv[7] & KS_PC_INT) : 1u;
}
void ks_pc_out_set(uint8_t external, uint8_t on)
{
    uint8_t bit = external ? KS_PC_EXT : KS_PC_INT;
    ks_pc_defaults_if_needed();
    if(on)proj.rsv[7] |= bit;
    else proj.rsv[7] &= (uint8_t)~bit;
}
uint8_t ks_pc_player_channel(void)
{
    uint8_t ch;
    if(proj.rsv[8]!=KS_PC_MARKER)return 16u; /* old project: ALL */
    ch=(uint8_t)((proj.rsv[7]>>2)&31u);
    return ch<=16u?ch:16u; /* invalid reserved value: safe default */
}
void ks_pc_player_channel_set(uint8_t ch)
{
    if(ch>16u)return;
    ks_pc_defaults_if_needed();
    proj.rsv[7]=(uint8_t)((proj.rsv[7]&0x83u)|(ch<<2));
}

/* KSTEP_HOME_TRACKS_V03: independent, initially empty MIDI tracks.
 * Drum sequencing remains proj.trk[0..3] / proj.seq_ch, default CH10.
 * No persistent-storage migration, no MIDI output from these tracks yet. */
kstep_midi_track_t kstep_midi_tracks[KSTEP_MIDI_TRACKS] = {
    { .channel = 0, .length = 16, .velocity = 100 },
    { .channel = 1, .length = 16, .velocity = 100 },
    { .channel = 2, .length = 16, .velocity = 100 },
    { .channel = 3, .length = 16, .velocity = 100 },
};

/* the knob banks: A filter cutoff, resonance, modulation, volume; B attack, release, decay, vibrato;
 * C and D the general-purpose controllers 16-23 */
static const uint8_t CC_DEFAULT[NBANKS][NKNOBS] = {{74, 71, 1, 7}, {73, 72, 75, 76}, {16, 17, 18, 19}, {20, 21, 22, 23}};
/* the sequencer: General MIDI drums (kick, snare, closed hat, clap) */
static const uint8_t TRK_DEFAULT[NTRACKS][4] = {{16, 4, 0, 36}, {16, 2, 4, 38}, {16, 8, 0, 42}, {12, 3, 2, 39}};

/* hits spread as evenly as possible over steps (the Euclidean rhythm), turned right by rot */
uint16_t euclid(int steps, int hits, int rot)
{
    uint16_t p = 0;
    int i;
    if (steps < 1 || steps > 16)
        return 0;
    if (hits > steps)
        hits = steps;
    for (i = 0; i < steps; i++)
        if ((i * hits) % steps < hits)
            p |= (uint16_t)(1u << ((i + rot) % steps));
    return p;
}

void project_defaults(void)
{
    int b, k;
    memset(&proj, 0, sizeof proj);
    proj.magic = PROJ_MAGIC;
    proj.format = PROJ_FORMAT;
    proj.vel = 100;
    for (b = 0; b < NBANKS; b++)
        for (k = 0; k < NKNOBS; k++) {
            proj.cc[b][k] = CC_DEFAULT[b][k];
            proj.val[b][k] = CC_DEFAULT[b][k] == 7 ? 100 : CC_DEFAULT[b][k] == 1 ? 0 : 64;
        }
    proj.scale = 0;                              /* chromatic: the keys as a keyboard (root C for a scale) */
    proj.tempo = 120;
    proj.arp_rate = RATE_16;
    proj.arp_oct = 1;
    proj.seq_ch = 9;                             /* channel 10 */
    proj.glow = 1;
    proj.touch = 1;                              /* velocity from the keys */
    proj.at_mode = AT_POLY;                      /* and aftertouch, per note */
    for (k = 0; k < NTRACKS; k++) {
        track_t *t = &proj.trk[k];
        t->steps = TRK_DEFAULT[k][0];
        t->hits = TRK_DEFAULT[k][1];
        t->rot = TRK_DEFAULT[k][2];
        t->note = TRK_DEFAULT[k][3];
        t->pat = euclid(t->steps, t->hits, t->rot);
    }
    ks_program_init();
    ks_pc_defaults_if_needed();
}

static int project_valid(void)
{
    int b, k;
    if (proj.magic != PROJ_MAGIC || proj.format != PROJ_FORMAT)
        return 0;
    if (proj.chan > 15u || proj.vel < 1u || proj.vel > 127u || proj.octave < -3 || proj.octave > 3 ||
        proj.program > 127u || proj.bank >= NBANKS || proj.root > 11u || proj.scale >= NSCALES ||
        proj.human > 40u || proj.tempo < 40u || proj.tempo > 240u || proj.clock >= NCLK || proj.arp_on > 1u ||
        proj.arp_mode >= NARP || proj.arp_rate >= NRATE || proj.arp_oct < 1u || proj.arp_oct > 3u ||
        proj.arp_latch > 1u || proj.seq_ch > 15u || proj.master_cc > 119u || proj.glow > 2u ||
        proj.sound >= NSOUND || proj.in_mute > 1u || proj.touch > 1u || proj.at_mode >= NAT ||
        proj.vel_curve < -5 || proj.vel_curve > 5 || proj.at_sens < -5 || proj.at_sens > 5)
        return 0;
    for (b = 0; b < NBANKS; b++)
        for (k = 0; k < NKNOBS; k++)
            if (proj.cc[b][k] > 119u || proj.val[b][k] > 127u)
                return 0;
    for (k = 0; k < NTRACKS; k++) {
        const track_t *t = &proj.trk[k];
        if (t->steps < 1u || t->steps > 16u || t->hits > t->steps || t->rot >= t->steps || t->note > 127u ||
            t->mute > 1u)
            return 0;
    }
    return 1;
}

int project_load(void)
{
    int n;
    memset(&proj, 0, sizeof proj);
    n = plat_store_load(OBJ_PROJ, &proj, sizeof proj);
    if (n >= 8 && project_valid()) {
        if (proj.sound == SOUND_KEYS) {          /* an older project's "Keys": on, MIDI in not played */
            proj.sound = SOUND_IN;
            proj.in_mute = 1;
        }
        ks_program_init(); /* old stored projects: no forced flash write */
        ks_pc_defaults_if_needed(); /* no project migration/format bump */
        return 0;
    }
    project_defaults();
    return -1;
}

int project_save(void) { return plat_store_save(OBJ_PROJ, &proj, sizeof proj); }
