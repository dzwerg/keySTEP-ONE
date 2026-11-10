/* KSTEP_DISK_ALL_240K_V043: keySTEP ONE v0.4.3: MIDI arranger and dedicated sample-only disk.
 * Adapted to fm1-base's documented storage API. Not binary-compatible with
 * analogBEAST MUSIC DISK. This module does not overwrite the old disk region.
 * One loaded pattern, 4 melodic tracks, 16..128 sixteenths, polyphonic events.
 * DISKONLY: read existing flash at boot; never automatically erase/format.
 * v0.4.3: all THREE 80KiB sample slots are the dedicated disk (60 patterns).
 * NEVER silently erase data. Double-confirmed key chord unlocks explicit wipe.
 * Executed as part of the app.c unity build, after engine.c and project.c.
 */
#include "ks_arranger.h"
#include "ks_pattern_mix.h"
#include "ks_music.h"
#include "ks_song_grid_core.h"
#include "ab16_fs.h"
#include "ks_step_editor.h"
/* KSTEP_AB16_240K_V050: actual analogBEAST AB16 append-only filesystem. */
#include <stddef.h>
#include <string.h>

#ifndef PLAT_POOL
#define PLAT_POOL
#endif
#define KS_ACTIVE 32u
#define KS_REC_NONE 0xFFFFu
_Static_assert(sizeof(ks_event_t)==6u, "ks_event_t packed layout changed");
_Static_assert(offsetof(ks_pattern_t,event)+KS_MAX_EVENTS*sizeof(ks_event_t)+sizeof(uint32_t) <= AB16_MAX_DATA, "max compact pattern must fit a record");
_Static_assert(KS_FLASH_SECTORS*KS_FLASH_SECTOR == KS_SAMPLE_ALL_END-KS_FLASH_BASE,
               "AB16 must fill all three sample slots and nothing else");

_Static_assert(KS_SAMPLE_SLOT_BASE >= PLAT_DATA_LO &&
               KS_SAMPLE_ALL_END <= PLAT_DATA_HI,
               "all three sample slots must be inside fm1-base data APIs");
static ks_pattern_t ks_pat PLAT_POOL;
static ks_pattern_t ks_tmp PLAT_POOL;
/* V0520: one independent, unsaved musical working pattern per performance track.
 * There is no track/channel/instrument data in any saved ks_pattern_t. */
static ks_pattern_t ks_work[KS_TRACKS] PLAT_POOL;
static uint16_t ks_work_source[KS_TRACKS]; /* 0xffff means CLEAR/TEMP */
static uint8_t ks_work_dirty[KS_TRACKS];
static uint16_t ks_masks[KS_TRACKS][8] PLAT_POOL;
static struct {uint8_t note, track, channel;uint16_t ticks;} ks_active[KS_ACTIVE];
static uint16_t ks_pending[KS_TRACKS][128] PLAT_POOL;
static uint32_t ks_rec_start[KS_TRACKS][128] PLAT_POOL; /* last captured note per track; index + 1 */
static uint32_t ks_clock;
uint16_t ks_pattern_num=0;
uint8_t ks_selected_track=0, ks_recording=0, ks_dirty=0;
volatile uint8_t ks_visual_step;
uint8_t ks_disk_status;
uint8_t ks_disk_slot=255u;
static ab16_fs ks_fs;
static uint8_t ks_wire[AB16_MAX_DATA] PLAT_POOL;
/* V0515: independent read-only track pattern sources. RAM cache, not AB16 writes. */
static ks_mix_state_t ks_mix PLAT_POOL;
static uint8_t ks_mix_ready;
/* LFO LIVE PLAY: each track has one inactive preloaded RAM slot.
 * AB16 reads occur ONLY in the main loop. The step ISR swaps a 1-byte
 * slot selector at the exact lane boundary and never copies music data. */
static ks_mix_slot_t ks_live_shadow[KS_TRACKS] PLAT_POOL;
static volatile uint8_t ks_live_enabled, ks_live_shadow_active[KS_TRACKS];
static volatile uint8_t ks_live_pending[KS_TRACKS],ks_live_valid[KS_TRACKS];
static uint32_t ks_live_origin[KS_TRACKS];
static uint16_t ks_live_next_id[KS_TRACKS];
/* LIVE mixing is independent from the saved pattern data and MIDI routing.
 * Main/UI loop sets these byte-sized levels, clock ISR only reads them.
 * Reset on mode exit; the UI may leave LIVE temporarily without resetting. */
static volatile uint8_t ks_live_volume[KS_TRACKS]={100u,100u,100u,100u};
static const ks_mix_slot_t *ks_live_slot(unsigned tr){
    return ks_live_enabled && ks_live_shadow_active[tr] ?
        &ks_live_shadow[tr] : &ks_mix.slot[tr];
}

/* Native SONG/ALBUM objects. The two song buffers enable album changes
 * without blocking the 1 kHz timer for a flash read. NO ROM demos. */
static ksm_song_t ks_songs[2] PLAT_POOL;
static ksm_album_t ks_album PLAT_POOL;
static ksm_runtime_t ks_song_run PLAT_POOL;
/* v0.5.23: the four separate SONG pattern-reference rows. */
static ksg_grid_t ks_grid PLAT_POOL;
static ksg_grid_t ks_grid_stage PLAT_POOL;
static uint8_t ks_grid_dirty;
/* SONG loop player: 3 slots per track (immutable loop-first + live +
 * prefetched next). The ISR changes only buffer indexes; all AB16 reads
 * and potentially large note copies are done in the UI/main loop. */
typedef struct {
    uint16_t col, ready_col;
    uint32_t begin;
    uint8_t current, ready, finished;
} ks_gp_lane_t;
static ks_mix_slot_t ks_gp_first[KS_TRACKS] PLAT_POOL;
static ks_mix_slot_t ks_gp_other[KS_TRACKS][2] PLAT_POOL;
static ksg_grid_t ks_gp_snapshot PLAT_POOL;
/* Shared by the audio-tick ISR and UI/main loop. Without volatile the
 * target optimizer is free to reuse stale stop/repeat and prefetch flags.
 * Multi-field ready/slot publication remains protected by plat_lock(). */
static volatile ks_gp_lane_t ks_gp_lane[KS_TRACKS];
static volatile uint8_t ks_gp_enabled,ks_gp_underrun,ks_gp_repeat=1u,ks_gp_finished;
static volatile uint8_t ks_external_player;
static volatile uint32_t ks_gp_loop_begin;

#define KS_GRID_TYPE 12u
#define KS_GRID_MAGIC 0x31475343u /* CSG1 */
#define KS_GRID_VERSION 1u
static uint8_t ks_song_ix,ks_song_dirty,ks_album_dirty,ks_mode,ks_assign_dirty,ks_song_full;
static uint8_t ks_album_index,ks_album_pending,ks_transport_ended;
static uint32_t ks_song_origin;
static uint16_t ks_song_id,ks_album_id;
#define KS_MODE_PATTERN 0u
#define KS_MODE_SONG 1u
#define KS_MODE_ALBUM 2u
uint8_t ks_arr_live_volume_get(uint8_t tr){
    return tr<KS_TRACKS?ks_live_volume[tr]:100u;
}
/* MIDI CC7 is sent to the independent internal Sonivox part only.
 * USB/BLE CC7 is purposefully NOT broadcast: two LIVE tracks might route to
 * the same external channel, but their Note-On velocities stay independent. */
static void ks_arr_live_synth_level(uint8_t tr,uint8_t pct){
    uint32_t cc=(uint32_t)((pct*127u+50u)/100u);
    uint32_t pkt=0xBu|((0xB0u|tr)<<8)|(7u<<16)|(cc<<24);
    ctl_send(pkt,TO_SYNTH);
}
void ks_arr_live_volume_set(uint8_t tr,uint8_t pct){
    if(tr>=KS_TRACKS||!ks_live_enabled||ks_mode!=KS_MODE_PATTERN)return;
    if(pct>100u)pct=100u;
    if(ks_live_volume[tr]==pct)return;
    ks_live_volume[tr]=pct;
    ks_arr_live_synth_level(tr,pct);
}
static uint8_t ks_arr_live_velocity(uint8_t tr,uint8_t vel){
    uint32_t v;
    if(!ks_live_enabled||ks_mode!=KS_MODE_PATTERN||tr>=KS_TRACKS||vel==0u)
        return vel;
    v=((uint32_t)vel*ks_live_volume[tr]+50u)/100u;
    /* Preserve a sounding Note-On for any positive fader setting. */
    if(v==0u && ks_live_volume[tr])v=1u;
    return (uint8_t)(v>127u?127u:v);
}

#define KS_SONG_COUNT 64u
#define KS_ALBUM_COUNT 32u
#define KS_ASSIGN_TYPE 11u
#define KS_ASSIGN_MAGIC 0x31474e4bu /* KNG1 */
typedef struct{uint32_t magic;uint16_t version,pattern[4];}ks_assign_legacy_t;
typedef struct{uint32_t magic;uint16_t version,pattern[4];uint8_t midi_channel[4];}ks_assign_file_t;
/* Track MIDI channels belong to performance tracks, never to a changing editor file. */
static uint8_t ks_route_channel[KS_TRACKS]={0u,1u,2u,3u};

/* Pattern v3 has no recorded-track bits. Duration is plain 16-bit. */
#define KS_GATE_TICKS_PER_STEP 6u
#define KS_MAX_GATE_TICKS (KS_MAX_STEPS*KS_GATE_TICKS_PER_STEP)
static uint16_t ks_event_ticks(const ks_event_t *e){
    return (uint16_t)(((uint16_t)e->gate_hi<<8)|e->gate_lo);
}
static void ks_event_set_ticks(ks_event_t *e,uint16_t ticks){
    e->gate_hi=(uint8_t)(ticks>>8);
    e->gate_lo=(uint8_t)ticks;
}
static uint32_t ks_chance_rng=0x123489AFu;
static int ks_flash_read(void*ctx,uint32_t off,void*dst,uint32_t n){
    const uint8_t *p; (void)ctx;
    if(off>AB16_SIZE || n>AB16_SIZE-off)return -1;
    p=plat_data(AB16_BASE+off);if(!p)return -1;
    memcpy(dst,p,n);return 0;
}
static int ks_flash_prog(void*ctx,uint32_t off,const void*src,uint32_t n){
    const uint8_t *p=src; (void)ctx;
    if(off>AB16_SIZE||n>AB16_SIZE-off)return -1;
    while(n){uint32_t k=256u-((AB16_BASE+off)&255u);if(k>n)k=n;
        if(plat_data_write(AB16_BASE+off,p,k))return -1;
        off+=k;p+=k;n-=k;
    }return 0;
}
static int ks_flash_erase(void*ctx,uint32_t off){
    (void)ctx;
    if(off%4096u || off>=AB16_SIZE)return -1;
    return plat_data_erase(AB16_BASE+off);
}
static ab16_io ks_fs_io(void){ab16_io io={0,ks_flash_read,ks_flash_prog,ks_flash_erase};return io;}
static int ks_fill_wire(void*ctx,uint32_t off,void*dst,uint32_t n){
    (void)ctx; if(off>sizeof ks_wire||n>sizeof ks_wire-off)return -1;
    memcpy(dst,ks_wire+off,n);return 0;
}

/* The CRC intentionally includes padding/reserved bytes (initially zero). */
static uint32_t ks_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c=0xFFFFFFFFu, i; int b;
    for(i=0;i<n;++i){c^=p[i];for(b=0;b<8;++b)c=(c>>1)^((c&1u)?0xEDB88320u:0u);}
    return ~c;
}
static uint32_t ks_pat_crc(const ks_pattern_t *p)
{
    return ks_crc((const uint8_t*)p,(uint32_t)(offsetof(ks_pattern_t,event)+p->count*sizeof(ks_event_t)));
}
static void ks_masks_refresh(void)
{
    uint32_t i,j;
    memset(ks_masks,0,sizeof ks_masks);
    for(i=0;i<ks_pat.count && i<KS_MAX_EVENTS;++i){
        const ks_event_t *e=&ks_pat.event[i];
        if(e->step<ks_pat.length)
            for(j=0;j<KS_TRACKS;++j)
                ks_masks[j][e->step/16u]|=(uint16_t)(1u<<(e->step%16u));
    }
    for(j=0;j<KS_TRACKS;++j){
        kstep_midi_tracks[j].channel=ks_route_channel[j];
        kstep_midi_tracks[j].length=16;
        kstep_midi_tracks[j].steps_on=ks_masks[j][ks_arr_block()];
    }
}
static void ks_empty(void)
{
    memset(&ks_pat,0,sizeof ks_pat);
    ks_pat.magic=KS_PAT_MAGIC;
    ks_pat.version=KS_PAT_VERSION;
    ks_pat.length=16;
    /* Track channels are NOT stored in a Pattern. */
    ks_dirty=0;
    ks_masks_refresh();
}
static int ks_valid(const ks_pattern_t *p)
{
    uint32_t i;
    if(p->magic!=KS_PAT_MAGIC || p->version!=KS_PAT_VERSION ||
       p->count>KS_MAX_EVENTS || p->length<8u || p->length>128u)
        return 0;
    for(i=0;i<p->count;++i) {
        const ks_event_t *e=&p->event[i];
        if(e->step>=p->length || ks_event_ticks(e)<1u ||
           ks_event_ticks(e)>KS_MAX_GATE_TICKS || e->note>127u ||
           e->vel<1u || e->vel>127u || e->chance>100u)return 0;
    }
    return ks_pat_crc(p)==p->crc;
}
/* AB16 uses types 1..7 for analogBEAST objects (PAT, SONG, ALBUM, MBOX, ...).
 * The four-track polyphonic keySTEP patterns use type 8: their payload cannot be
 * confused with analogBEAST's monophonic AB13 pattern codec. */
/* Read-only AB16 decode. Never touch the editable pattern or other tracks. */
static int ks_read_pattern_into(uint16_t pat,ks_pattern_t *tmp){
    ab16_file file; uint32_t prefix,n; int rc;
    if(!ks_fs.mounted)return -3;
    rc=ab16_lookup(&ks_fs,AB16_KSTEP4,(uint32_t)pat+1u,&file);
    if(rc==AB16_ENOTFOUND){
        memset(tmp,0,sizeof *tmp);tmp->magic=KS_PAT_MAGIC;
        tmp->version=KS_PAT_VERSION;tmp->length=16u;
        tmp->crc=ks_pat_crc(tmp);return 0;
    }
    if(rc)return -2;
    prefix=(uint32_t)offsetof(ks_pattern_t,event);
    if(file.length<8u||file.length>AB16_MAX_DATA)return -3;
    rc=ab16_read(&ks_fs,AB16_KSTEP4,(uint32_t)pat+1u,0,ks_wire,file.length);
    if(rc)return -2;
    /* Old v2 test-patterns were four-lane and carry a channel map. The user
     * explicitly discarded them. Treat ONLY that known legacy version as
     * empty, without touching other records or formatting the MUSIC DISK. */
    {
        uint32_t magic;uint16_t version;
        memcpy(&magic,ks_wire,4u);memcpy(&version,ks_wire+4u,2u);
        if(magic==KS_PAT_MAGIC && version==2u){
            memset(tmp,0,sizeof *tmp);tmp->magic=KS_PAT_MAGIC;
            tmp->version=KS_PAT_VERSION;tmp->length=16u;
            tmp->crc=ks_pat_crc(tmp);return 0;
        }
    }
    if(file.length<prefix+sizeof(uint32_t))return -3;
    memset(tmp,0,sizeof *tmp);
    memcpy(tmp,ks_wire,prefix);
    if(tmp->count>KS_MAX_EVENTS)return -3;
    n=prefix+(uint32_t)tmp->count*sizeof(ks_event_t);
    if(n+sizeof(uint32_t)!=file.length)return -3;
    memcpy(tmp->event,ks_wire+prefix,n-prefix);
    memcpy(&tmp->crc,ks_wire+n,sizeof(uint32_t));
    if(!ks_valid(tmp))return -3;
    return 0;
}
static int ks_read_pattern(uint16_t pat){
    int rc=ks_read_pattern_into(pat,&ks_tmp);
    if(!rc){ks_pat=ks_tmp;ks_dirty=0;ks_masks_refresh();}
    return rc;
}

/* All ids here are ZERO-BASED. The UI maps black bank + white key to
 * (bank*16+key). Neither queued nor live data is written to MUSIC DISK. */
void ks_arr_live_enable(void){
    uint32_t f=plat_lock();unsigned tr;
    if(!ks_live_enabled){
        for(tr=0;tr<KS_TRACKS;tr++){
            ks_live_shadow_active[tr]=0u;
            ks_live_pending[tr]=0u;
            ks_live_origin[tr]=0u;
            ks_live_valid[tr]=(ks_work_source[tr]!=0xffffu);
        }
        ks_live_enabled=1u;
    }
    plat_unlock(f);
}
void ks_arr_live_transport_reset(void){
    uint32_t f=plat_lock();unsigned tr;
    for(tr=0;tr<KS_TRACKS;tr++)ks_live_origin[tr]=0u;
    plat_unlock(f);
}
void ks_arr_live_disable(void){
    unsigned tr;uint32_t f;
    if(!ks_live_enabled)return;
    /* Called only when STOPPED; preserve current per-track playing sources. */
    for(tr=0;tr<KS_TRACKS;tr++){
        if(ks_live_shadow_active[tr])ks_mix.slot[tr]=ks_live_shadow[tr];
    }
    f=plat_lock();
    for(tr=0;tr<KS_TRACKS;tr++){
        ks_live_pending[tr]=0u;ks_live_shadow_active[tr]=0u;ks_live_origin[tr]=0u;
    }
    ks_live_enabled=0u;
    plat_unlock(f);
    for(tr=0;tr<KS_TRACKS;tr++){
        if(ks_live_volume[tr]!=100u){
            ks_live_volume[tr]=100u;
            ks_arr_live_synth_level((uint8_t)tr,100u);
        }
    }
}
int ks_arr_live_queue(uint8_t tr,uint16_t pat){
    ks_mix_slot_t *dst;unsigned i;int rc;uint32_t f;
    if(!ks_live_enabled||ks_mode!=KS_MODE_PATTERN||tr>=KS_TRACKS||
       pat>=KS_PATTERNS||ks_recording)return -1;
    if(ks_arr_pattern_exists(pat)!=1)return -2;
    /* Cancel any previous pending switch BEFORE flash access; the ISR may
     * not claim the inactive slot while it is being filled. */
    f=plat_lock();ks_live_pending[tr]=0u;plat_unlock(f);
    rc=ks_read_pattern_into(pat,&ks_tmp);if(rc)return rc;
    dst=ks_live_shadow_active[tr]?&ks_mix.slot[tr]:&ks_live_shadow[tr];
    dst->pattern=pat;dst->length=ks_tmp.length;dst->count=ks_tmp.count;
    dst->reserved=0u;memset(dst->mask,0,sizeof dst->mask);
    for(i=0;i<ks_tmp.count;i++){
        const ks_event_t *e=&ks_tmp.event[i];
        dst->event[i]=*e;
        dst->mask[e->step/16u]|=(uint16_t)(1u<<(e->step%16u));
    }
    f=plat_lock();ks_live_next_id[tr]=pat;ks_live_pending[tr]=1u;plat_unlock(f);
    return 0;
}
int ks_arr_live_current(uint8_t tr,uint16_t *id){
    if(tr>=KS_TRACKS||!ks_live_enabled||!ks_live_valid[tr])return 0;
    if(id)*id=ks_live_slot(tr)->pattern;
    return 1;
}
int ks_arr_live_queued(uint8_t tr,uint16_t *id){
    if(tr>=KS_TRACKS||!ks_live_enabled||!ks_live_pending[tr])return 0;
    if(id)*id=ks_live_next_id[tr];
    return 1;
}
static void ks_arr_live_boundary(unsigned tr,uint32_t pos){
    const ks_mix_slot_t *src=ks_live_slot(tr);
    if(!ks_live_enabled||!ks_live_pending[tr]||
       ((pos-ks_live_origin[tr])%src->length)!=0u)return;
    /* Prevent a note from the outgoing pattern from hanging across switch. */
    {
        unsigned n;
        for(n=0;n<KS_ACTIVE;n++)if(ks_active[n].ticks&&ks_active[n].track==tr){
            ks_arr_track_send((uint8_t)tr,0x80u,ks_active[n].note,0u);
            ks_active[n].ticks=0u;
        }
    }
    ks_live_shadow_active[tr]^=1u;
    ks_live_origin[tr]=pos;
    ks_live_valid[tr]=1u;
    ks_live_pending[tr]=0u;
}
uint8_t ks_arr_blocks(void){return (uint8_t)((ks_pat.length+15u)/16u);}
uint8_t ks_arr_block(void){return (uint8_t)((ks_visual_step/16u)%ks_arr_blocks());}
uint8_t ks_arr_len(void){return ks_pat.length;}
/* KSTEP_TRACK_LOCAL_SOUND_V053: four independent Sonivox parts regardless
 * of duplicate external MIDI output channels. This function handles a
 * selected track note and preserves its per-pattern MIDI OUT assignment. */
void ks_arr_track_send(uint8_t tr,uint32_t status,uint32_t note,uint32_t velocity)
{
    uint32_t midi,synth,cmd=status&0xF0u;
    if(tr>=KS_TRACKS||cmd<0x80u||cmd>=0xF0u)return;
    /* KSTEP_HOME_TRACK_KNOBS_V054: MUTE/SOLO block new events but NEVER block
     * note-offs (including note-on velocity zero), preventing hanging notes. */
    if(!ks_track_audible(tr) && cmd!=0x80u && !(cmd==0x90u&&velocity==0u))return;
    synth=(cmd>>4)|((cmd|tr)<<8)|(note<<16)|(velocity<<24);
    /* LIVE: external Note-On velocity is per performance track, NOT shared
     * channel CC7. Internal sound gets separate instantaneous CC7 fader,
     * with its original velocity (otherwise gain would be applied twice). */
    if(cmd==0x90u && velocity!=0u)
        velocity=ks_arr_live_velocity(tr,(uint8_t)velocity);
    /* ALL = deliberate broadcast to external CH1..CH16. Internal CH stays tr. */
    if(ks_route_channel[tr]==16u){
        uint32_t ch;
        for(ch=0;ch<16u;++ch){
            midi=(cmd>>4)|((cmd|ch)<<8)|(note<<16)|(velocity<<24);
            ctl_send(midi,TO_USB|TO_BLE|TO_NOSYNTH);
        }
    }else{
        midi=(cmd>>4)|((cmd|ks_route_channel[tr])<<8)|(note<<16)|(velocity<<24);
        ctl_send(midi,TO_USB|TO_BLE|TO_NOSYNTH);
    }
    ctl_send(synth,TO_SYNTH);
}
uint8_t ks_arr_channel(void){return ks_route_channel[ks_selected_track];}
uint16_t ks_arr_assigned(uint8_t tr){return tr<KS_TRACKS?ks_live_slot(tr)->pattern:0u;}
uint8_t ks_arr_play_len(uint8_t tr){return tr<KS_TRACKS&&ks_mix_ready?ks_live_slot(tr)->length:ks_pat.length;}
uint16_t ks_arr_edit_mask(uint8_t tr,uint8_t block){
    return tr<KS_TRACKS&&block<8u?ks_masks[tr][block]:0u;
}
/* Change one PLAY source, never swapping the live step-editor buffer. */
int ks_arr_assign(uint8_t tr,uint16_t pat){
    int rc;uint32_t f;
    if(tr>=KS_TRACKS||pat>=KS_PATTERNS||ks_recording||eng_running())return -1;
    rc=ks_read_pattern_into(pat,&ks_tmp);if(rc)return rc;
    f=plat_lock();
    if(ks_mix.slot[tr].pattern!=pat)ks_assign_dirty=1u;
    rc=ks_mix_assign(&ks_mix,tr,pat,&ks_tmp);
    ks_mix_ready=1u;
    plat_unlock(f);
    return rc;
}
/* The TEMP editor must NEVER write into another track just because both
 * tracks originally loaded the same saved pattern ID. */
static void ks_mix_sync_edit(void){
    if(!ks_mix_ready)return;
    (void)ks_mix_assign(&ks_mix,ks_selected_track,
                        ks_mix.slot[ks_selected_track].pattern,&ks_pat);
}
int ks_arr_play_get(uint8_t tr,uint8_t step,void *out_step){
    kse_step_t *out=out_step;const ks_mix_slot_t *s;
    unsigned i;
    if(!out||tr>=KS_TRACKS||!ks_mix_ready)return -1;
    s=ks_live_slot(tr);
    if(step>=s->length)return -1;
    memset(out,0,sizeof *out);out->gate=100;out->chance=100;out->span=1;
    for(i=0;i<s->count;i++){
        const ks_event_t *e=&s->event[i];
        uint16_t ticks,partial;
        if(e->step!=step)continue;
        if(out->count>=KSE_MAX_CHORD)return -1;
        out->note[out->count]=e->note;
        out->velocity[out->count]=e->vel;
        ticks=ks_event_ticks(e);
        partial=(uint16_t)((ticks-1u)%KS_GATE_TICKS_PER_STEP+1u);
        out->span=(uint8_t)((ticks-1u)/KS_GATE_TICKS_PER_STEP+1u);
        out->gate=(uint8_t)((partial*100u+3u)/KS_GATE_TICKS_PER_STEP);
        out->chance=e->chance;out->count++;
    }
    return 0;
}
uint16_t ks_arr_mask(uint8_t tr,uint8_t block){
    if(ks_mix_ready && tr<KS_TRACKS && block<8u){
        const ks_mix_slot_t *s=ks_live_slot(tr);
        return block<(s->length+15u)/16u?s->mask[block]:0u;
    }
    return ks_arr_edit_mask(tr,block);
}
uint16_t ks_arr_event_count(void){return ks_pat.count;}
/* Switch editable RAM workspaces, not the saved pattern object. */
static void ks_work_flush(void){
    ks_work[ks_selected_track]=ks_pat;
    ks_work_dirty[ks_selected_track]=ks_dirty;
}
void ks_arr_set_track(uint8_t tr){
    if(tr>=KS_TRACKS||tr==ks_selected_track||ks_recording)return;
    ks_work_flush();
    ks_selected_track=tr;
    ks_pat=ks_work[tr];
    ks_dirty=ks_work_dirty[tr];
    ks_pattern_num=ks_work_source[tr]==0xffffu?0u:ks_work_source[tr];
    ks_masks_refresh();
}
void ks_arr_set_length(uint8_t len){
    uint32_t i,j=0;
    if(len<8u||len>128u||ks_recording||eng_running())return;
    if(ks_pat.length==len)return;
    for(i=0;i<ks_pat.count;++i)if(ks_pat.event[i].step<len)ks_pat.event[j++]=ks_pat.event[i];
    ks_pat.count=(uint16_t)j;
    ks_pat.length=len;
    ks_visual_step=0;
    ks_dirty=1;
    ks_mix_sync_edit();
    ks_masks_refresh();
}
/* KSTEP_HOME_TRACK_KNOBS_V054: only silence this track when channel changes. */
static void ks_arr_off_track(uint8_t tr)
{
    uint32_t i;
    for(i=0;i<KS_ACTIVE;++i)if(ks_active[i].ticks && ks_active[i].track==tr){
        ks_arr_track_send(tr,0x80u,ks_active[i].note,0u);
        ks_active[i].ticks=0;
    }
}
void ks_arr_set_channel(uint8_t ch){
    uint32_t f;
    if(ch>16u||ks_recording||ks_route_channel[ks_selected_track]==ch)return;
    /* Change while PLAY runs: terminate prior-channel notes before switching. */
    f=plat_lock();
    ks_arr_off_track(ks_selected_track);
    ks_route_channel[ks_selected_track]=ch;
    ks_assign_dirty=1u; /* track configuration, not pattern data */
    ks_masks_refresh();
    plat_unlock(f);
}
void ks_arr_offs(void)
{
    uint32_t i;
    if(ks_mode==KS_MODE_SONG && ks_recording){
        ksm_record_finish(&ks_songs[ks_song_ix],&ks_song_run,eng.seq_tick);
        ks_recording=0;ks_song_dirty=1;
    }
    for(i=0;i<KS_ACTIVE;++i)if(ks_active[i].ticks){
        ks_arr_track_send(ks_active[i].track,0x80u,ks_active[i].note,0u);
        ks_active[i].ticks=0;
    }
}
/* KSTEP_HOME_TRACK_KNOBS_V054: turn off active notes that became inaudible. */
void ks_arr_silence_inaudible(void)
{
    uint32_t i,f=plat_lock();
    for(i=0;i<KS_ACTIVE;++i)if(ks_active[i].ticks && !ks_track_audible(ks_active[i].track)){
        ks_arr_track_send(ks_active[i].track,0x80u,ks_active[i].note,0u);
        ks_active[i].ticks=0;
    }
    plat_unlock(f);
}
int ks_arr_load(uint16_t pat)
{
    int rc;
    if(pat>=KS_PATTERNS||ks_recording||eng_running()||ks_dirty)return -1;
    if(!ks_fs.mounted)return -3;
    ks_arr_offs();
    ks_pattern_num=pat;ks_visual_step=0;
    memset(ks_active,0,sizeof ks_active);
    memset(ks_pending,0,sizeof ks_pending);
    rc=ks_read_pattern(pat);
    if(rc)ks_disk_status=rc==-2?3:2;
    else {
        ks_work_source[ks_selected_track]=pat;
        ks_work_flush();
        ks_mix_sync_edit();
    }
    return rc;
}
void ks_arr_init(void)
{
    int rc;
    memset(ks_active,0,sizeof ks_active);
    memset(ks_pending,0,sizeof ks_pending);
    ks_clock=0;ks_recording=0;ks_pattern_num=0;
    ksm_song_clear(&ks_songs[0]);ksm_song_clear(&ks_songs[1]);
    ksm_album_clear(&ks_album);ksm_runtime_clear(&ks_song_run);
    ksg_clear(&ks_grid);ks_grid_dirty=0u;
    ks_gp_enabled=0u;ks_gp_underrun=0u;ks_gp_finished=0u;ks_external_player=0u;
    ks_live_enabled=0u;
    {unsigned tr;for(tr=0;tr<KS_TRACKS;tr++)ks_live_volume[tr]=100u;}
    memset((void*)ks_live_pending,0,sizeof ks_live_pending);
    memset((void*)ks_live_shadow_active,0,sizeof ks_live_shadow_active);
    memset((void*)ks_live_valid,0,sizeof ks_live_valid);
    memset(ks_live_origin,0,sizeof ks_live_origin);
    ks_song_id=ks_album_id=0;ks_mode=KS_MODE_PATTERN;
    ks_song_ix=ks_song_dirty=ks_album_dirty=ks_album_pending=ks_song_full=0;
    ks_album_index=ks_transport_ended=0;ks_song_origin=0;
    ks_empty();ks_disk_slot=255u;
    {
        unsigned t;
        for(t=0;t<KS_TRACKS;t++){
            ks_work[t]=ks_pat;ks_work_source[t]=0xffffu;ks_work_dirty[t]=0;
        }
        ks_selected_track=0;
    }
    rc=ab16_mount(&ks_fs,ks_fs_io()); /* READ ONLY: never format on boot */
    if(rc){ks_disk_status=(rc==AB16_ENOFS||rc==AB16_EFOREIGN)?2:3;}
    else {ks_disk_slot=0;ks_disk_status=1;} /* Boot: mount only; never load P001 or assign its contents. */
    {
        unsigned t;
        for(t=0;t<KS_TRACKS;t++){
            ks_route_channel[t]=(uint8_t)t; /* no channel is read from pattern */
            (void)ks_mix_assign(&ks_mix,t,0u,&ks_pat);
        }
        ks_mix_ready=1u;
        ks_masks_refresh();
        /* Restore per-track MIDI routing ONLY. Saved pattern assignment IDs
         * are intentionally ignored on boot. All four working slots and
         * performance slots stay blank until the user explicitly LOADs.
         * No MUSIC DISK write or format happens here. */
        if(ks_fs.mounted){
            ab16_file f;ks_assign_file_t cfg;
            if(!ab16_lookup(&ks_fs,KS_ASSIGN_TYPE,1u,&f) &&
               f.length==sizeof cfg &&
               !ab16_read(&ks_fs,KS_ASSIGN_TYPE,1u,0,&cfg,sizeof cfg) &&
               cfg.magic==KS_ASSIGN_MAGIC && cfg.version==2u){
                for(t=0;t<KS_TRACKS;t++)
                    if(cfg.midi_channel[t]<=16u)
                        ks_route_channel[t]=cfg.midi_channel[t];
            }
        }
        ks_assign_dirty=0u;
        ks_selected_track=0u;
        ks_pattern_num=0u;
        ks_dirty=0u;
        ks_visual_step=0u;
        ks_masks_refresh();
    }
}
/* --- SONG/ALBUM native AB16 persistence --- */
static int ks_native_song_load(uint16_t id,ksm_song_t *dst){
    ab16_file f;int rc;uint16_t len;uint32_t prefix=offsetof(ksm_song_t,event);
    if(!ks_fs.mounted)return -3;
    rc=ab16_lookup(&ks_fs,KSM_AB16_SONG,(uint32_t)id+1u,&f);
    if(rc==AB16_ENOTFOUND){ksm_song_clear(dst);return 0;}
    if(rc||f.length<prefix||f.length>AB16_MAX_DATA)return -2;
    if(ab16_read(&ks_fs,KSM_AB16_SONG,(uint32_t)id+1u,0,ks_wire,f.length))return -2;
    memset(dst,0,sizeof *dst);memcpy(dst,ks_wire,prefix);
    if(dst->count>KSM_MAX_EVENTS)return -2;
    len=(uint16_t)ksm_song_wire_size(dst);
    if(len!=f.length)return -2;
    memcpy(dst->event,ks_wire+prefix,f.length-prefix);
    if(!ksm_song_valid(dst))return -2;
    return 0;
}
static int ks_native_album_load(uint16_t id){
    ab16_file f;int rc;uint32_t prefix=offsetof(ksm_album_t,song);
    if(!ks_fs.mounted)return -3;
    rc=ab16_lookup(&ks_fs,KSM_AB16_ALBUM,(uint32_t)id+1u,&f);
    if(rc==AB16_ENOTFOUND){ksm_album_clear(&ks_album);return 0;}
    if(rc||f.length<prefix||f.length>sizeof(ks_album))return -2;
    if(ab16_read(&ks_fs,KSM_AB16_ALBUM,(uint32_t)id+1u,0,ks_wire,f.length))return -2;
    memset(&ks_album,0,sizeof ks_album);memcpy(&ks_album,ks_wire,f.length);
    if(ksm_album_wire_size(&ks_album)!=f.length||!ksm_album_valid(&ks_album))return -2;
    return 0;
}
/* The serialized grid is a distinct AB16 object type. Song MIDI events type 9
 * remain intact, and PATTERN type 8 is never rewritten by the grid editor. */
static int ks_grid_load(uint16_t id){
    ab16_file f;uint32_t n,wire_crc,calc;unsigned j,total=0u;int rc;
    if(!ks_fs.mounted)return -3;
    rc=ab16_lookup(&ks_fs,KS_GRID_TYPE,(uint32_t)id+1u,&f);
    if(rc==AB16_ENOTFOUND){ksg_clear(&ks_grid);ks_grid_dirty=0;return 0;}
    if(rc || f.length<18u || f.length>AB16_MAX_DATA)return -2;
    if(ab16_read(&ks_fs,KS_GRID_TYPE,(uint32_t)id+1u,0,ks_wire,f.length))return -2;
    {
        uint32_t magic;uint16_t version;
        memcpy(&magic,ks_wire,4u);memcpy(&version,ks_wire+4u,2u);
        if(magic!=KS_GRID_MAGIC||version!=KS_GRID_VERSION)return -2;
    }
    ksg_clear(&ks_grid_stage);
    memcpy(ks_grid_stage.count,ks_wire+6u,8u);
    for(j=0;j<4u;j++)total+=ks_grid_stage.count[j];
    n=14u+2u*total;
    if(total>KSG_MAX_REFS||n+4u!=f.length)return -2;
    memcpy(&wire_crc,ks_wire+n,4u);
    calc=ks_crc(ks_wire,n);
    if(wire_crc!=calc)return -2;
    memcpy(ks_grid_stage.pat,ks_wire+14u,total*2u);
    if(!ksg_valid(&ks_grid_stage))return -2;
    ks_grid=ks_grid_stage;ks_grid_dirty=0;return 0;
}
static int ks_grid_store(void){
    ab16_file f;uint32_t n,crc;unsigned total=ksg_total(&ks_grid);int rc;
    if(!ks_fs.mounted||!ksg_valid(&ks_grid)||eng_running()||ks_recording)return -1;
    n=14u+total*2u;
    if(n+4u>AB16_MAX_DATA)return -2;
    {
        uint32_t magic=KS_GRID_MAGIC;uint16_t version=KS_GRID_VERSION;
        memcpy(ks_wire,&magic,4u);memcpy(ks_wire+4u,&version,2u);
    }
    memcpy(ks_wire+6u,ks_grid.count,8u);
    memcpy(ks_wire+14u,ks_grid.pat,total*2u);
    crc=ks_crc(ks_wire,n);memcpy(ks_wire+n,&crc,4u);
    rc=ab16_put(&ks_fs,KS_GRID_TYPE,(uint32_t)ks_song_id+1u,(uint16_t)(n+4u),ks_fill_wire,0);
    if(rc)return -3;
    rc=ab16_lookup(&ks_fs,KS_GRID_TYPE,(uint32_t)ks_song_id+1u,&f);
    if(rc||f.length!=n+4u)return -3;
    ks_grid_dirty=0;return 0;
}
/* -------- SONG pattern-grid loop transport (not the native MIDI-event song) -------- */
static ks_mix_slot_t *ks_gp_slot(unsigned tr,unsigned idx){
    return idx==0u?&ks_gp_first[tr]:&ks_gp_other[tr][idx-1u];
}
static int ks_gp_read(unsigned tr,unsigned col,ks_mix_slot_t *dst){
    uint16_t id=ksg_get(&ks_gp_snapshot,tr,col);
    if(id==KSG_EMPTY)return -1;
    if(id==KSG_SILENT){
        dst->pattern=KSG_SILENT;dst->count=0;dst->length=KSG_SILENT_STEPS;
        memset(dst->mask,0,sizeof dst->mask);
        return 0;
    }
    if(id>=KS_PATTERNS || ks_arr_pattern_exists(id)!=1)return -1;
    if(ks_read_pattern_into(id,&ks_tmp)!=0)return -1;
    {
        unsigned i;dst->pattern=id;dst->count=ks_tmp.count;dst->length=ks_tmp.length;
        memset(dst->mask,0,sizeof dst->mask);
        for(i=0;i<ks_tmp.count;i++){
            const ks_event_t *e=&ks_tmp.event[i];
            dst->event[i]=*e;
            dst->mask[e->step/16u]|=(uint16_t)(1u<<(e->step%16u));
        }
        return 0;
    }
}
static void ks_gp_rewind(void){
    unsigned tr;
    for(tr=0u;tr<KS_TRACKS;tr++){
        volatile ks_gp_lane_t *l=&ks_gp_lane[tr];
        l->col=0;l->ready=0;l->ready_col=0;l->begin=0;
        l->current=0;l->finished=ks_gp_snapshot.count[tr]==0u;
    }
}
int ks_arr_grid_play_prepare(void){
    unsigned tr;int active=0;
    if(ks_mode!=KS_MODE_SONG||eng_running()||ks_recording||!ks_fs.mounted)return -1;
    ks_gp_enabled=0;ks_gp_underrun=0;ks_gp_finished=0;
    ks_gp_snapshot=ks_grid;
    if(!ksg_valid(&ks_gp_snapshot))return -2;
    for(tr=0u;tr<KS_TRACKS;tr++){
        if(!ks_gp_snapshot.count[tr])continue;
        active=1;
        if(ks_gp_read(tr,0u,&ks_gp_first[tr])<0)return -3;
    }
    if(!active)return -4;
    ks_gp_rewind();ks_gp_loop_begin=0;
    ks_gp_enabled=1;
    return 0;
}
void ks_arr_grid_play_stop(void){ks_gp_enabled=0;ks_gp_finished=0;}
void ks_arr_grid_set_loop(int on){ks_gp_repeat=!!on;}
void ks_arr_external_player_set(int on){ks_external_player=!!on;ks_arr_offs();}
int ks_arr_grid_play_active(void){return !!ks_gp_enabled;}
int ks_arr_grid_play_underrun(void){return !!ks_gp_underrun;}
/* Main-loop prefetch. Keep a future slot invisible to the 1kHz ISR until
 * its complete contents have been populated and the lock publishes ready. */
static void ks_gp_poll_prefetch(void){
    unsigned tr;
    if(!ks_gp_enabled)return;
    for(tr=0u;tr<KS_TRACKS;tr++){
        volatile ks_gp_lane_t *l=&ks_gp_lane[tr];
        uint16_t col;uint8_t buffer;uint32_t f;
        if(l->finished||l->ready||l->col+1u>=ks_gp_snapshot.count[tr])continue;
        col=(uint16_t)(l->col+1u);
        buffer=l->current==1u?2u:1u;
        if(ks_gp_read(tr,col,ks_gp_slot(tr,buffer))<0){ks_gp_underrun=1u;return;}
        f=plat_lock();
        if(ks_gp_enabled && !l->finished && l->col+1u==col && !l->ready){
            l->ready_col=col;l->ready=buffer;
        }
        plat_unlock(f);
    }
}
static void ks_gp_quiet_track(unsigned tr){
    unsigned a;
    for(a=0;a<KS_ACTIVE;a++){
        if(ks_active[a].ticks && ks_active[a].track==tr){
            ks_arr_track_send((uint8_t)tr,0x80u,ks_active[a].note,0u);
            ks_active[a].ticks=0u;
        }
    }
}
static void ks_gp_emit_step(uint32_t pos){
    unsigned tr;int all_finished=1;
    uint32_t local=pos-ks_gp_loop_begin;
    /* Transition phase; no disk operations and no large memory copies. */
    for(tr=0u;tr<KS_TRACKS;tr++){
        volatile ks_gp_lane_t *l=&ks_gp_lane[tr];
        if(l->finished)continue;
        while(local>=l->begin+ks_gp_slot(tr,l->current)->length){
            l->begin+=ks_gp_slot(tr,l->current)->length;
            if(l->col+1u>=ks_gp_snapshot.count[tr]){
                ks_gp_quiet_track(tr);l->finished=1;break;
            }
            if(!l->ready || l->ready_col!=(uint16_t)(l->col+1u)){
                ks_gp_underrun=1u;return; /* stop instead of playing wrong notes */
            }
            l->col++;l->current=l->ready;l->ready=0;
            if(ks_gp_slot(tr,l->current)->pattern==KSG_SILENT)
                ks_gp_quiet_track(tr); /* II is genuinely silent */
        }
        if(!l->finished)all_finished=0;
    }
    /* The longest row defines the loop length; shorter rows stay silent to
     * its end. Restore the preloaded first phrase on precisely this step. */
    if(all_finished){
        if(!ks_gp_repeat){ks_gp_finished=1u;return;}
        ks_gp_loop_begin=pos;ks_gp_rewind();local=0u;
    }
    for(tr=0u;tr<KS_TRACKS;tr++){
        const volatile ks_gp_lane_t *l=&ks_gp_lane[tr];
        const ks_mix_slot_t *slot;
        unsigned n,j;uint32_t step;
        if(l->finished)continue;
        slot=ks_gp_slot(tr,l->current);
        step=local-l->begin;
        if(step>=slot->length)continue;
        for(n=0;n<slot->count;n++){
            const ks_event_t *e=&slot->event[n];
            if(e->step!=step)continue;
            if(e->chance<100u){
                ks_chance_rng^=ks_chance_rng<<13;
                ks_chance_rng^=ks_chance_rng>>17;
                ks_chance_rng^=ks_chance_rng<<5;
                if(ks_chance_rng%100u>=e->chance)continue;
            }
            for(j=0;j<KS_ACTIVE&&ks_active[j].ticks;j++){}
            if(j==KS_ACTIVE)continue;
            ks_active[j].note=e->note;ks_active[j].track=(uint8_t)tr;
            ks_active[j].channel=ks_route_channel[tr];ks_active[j].ticks=ks_event_ticks(e);
            ks_arr_track_send((uint8_t)tr,0x90u,e->note,e->vel);
        }
    }
    ks_visual_step=(uint8_t)(local&127u);
}
/* v0.5.30: read-only SONG ONE TOUCH. Called ONLY from UI key edges while
 * transport is paused, never under the 1 kHz engine ISR. A song-grid hit is
 * identified by its absolute 24 PPQN tick, across all four parallel rows.
 * Silent blocks only advance the row time. Existing TEMPs stay untouched. */
static ks_mix_slot_t ks_okp_grid_scan PLAT_POOL;
static void ks_okp_candidate(uint32_t at, uint8_t tr, uint8_t note,
                              uint8_t velocity, uint32_t cursor, int valid,
                              int direction, uint32_t *best, unsigned *count,
                              okp_note_t out[OKP_MAX_CHORD])
{
    int preferable;
    if(!velocity||note>=128u||tr>=KS_TRACKS)return;
    if(valid && (direction>0 ? at<=cursor : at>=cursor))return;
    preferable=*best==OKP_NOTE_NONE || (direction>0 ? at<*best : at>*best);
    if(preferable){*best=at;*count=0u;}
    if(at==*best && *count<OKP_MAX_CHORD){
        okp_note_t *n=&out[(*count)++];
        n->track=tr;n->channel=tr;n->pitch=note;n->velocity=velocity;
    }
}
int ks_arr_onetouch_seek(uint32_t cursor,int valid,int direction,
                         uint32_t *next_tick,okp_note_t notes[OKP_MAX_CHORD])
{
    uint32_t best=OKP_NOTE_NONE;
    unsigned count=0u,tr;
    if(!next_tick||!notes||(direction!=1 && direction!=-1)||eng_running()||
       ks_recording||ks_mode==KS_MODE_PATTERN)return -1;
    if(ks_gp_enabled){
        /* Preserve the immutable PLAY snapshot, not the editable TEMP buffers. */
        for(tr=0u;tr<KS_TRACKS;tr++){
            unsigned col;uint32_t base=0u;
            for(col=0u;col<ks_gp_snapshot.count[tr];col++){
                unsigned i;
                if(ks_gp_read(tr,col,&ks_okp_grid_scan)<0)return -1;
                for(i=0;i<ks_okp_grid_scan.count;i++){
                    const ks_event_t *e=&ks_okp_grid_scan.event[i];
                    uint32_t at=(base+(uint32_t)e->step)*KSM_TICKS_STEP;
                    ks_okp_candidate(at,(uint8_t)tr,e->note,e->vel,cursor,
                        valid,direction,&best,&count,notes);
                }
                base+=ks_okp_grid_scan.length;
            }
        }
    }else{
        /* Native S song stores explicit note onsets (not fixed steps). */
        const ksm_song_t *song=&ks_songs[ks_song_ix];
        unsigned i;
        for(i=0u;i<song->count;i++){
            const ksm_event_t *e=&song->event[i];
            ks_okp_candidate(e->at,e->track,e->note,e->vel,cursor,
                valid,direction,&best,&count,notes);
        }
    }
    if(!count||best==OKP_NOTE_NONE)return 0;
    *next_tick=best;return (int)count;
}
uint16_t ks_arr_grid_count(uint8_t tr){return tr<4u?ks_grid.count[tr]:0u;}
uint16_t ks_arr_grid_get(uint8_t tr,uint16_t col){return ksg_get(&ks_grid,tr,col);}
int ks_arr_grid_dirty(void){return !!ks_grid_dirty;}
int ks_arr_grid_put(uint8_t tr,uint16_t col,uint16_t pat){
    int rc;
    if(ks_mode!=KS_MODE_SONG||eng_running()||ks_recording||
       (pat!=KSG_SILENT && ks_arr_pattern_exists(pat)!=1))return -1;
    rc=ksg_put(&ks_grid,tr,col,pat);
    if(!rc)ks_grid_dirty=1;
    return rc;
}
int ks_arr_grid_del(uint8_t tr,uint16_t col){
    int rc;
    if(ks_mode!=KS_MODE_SONG||eng_running()||ks_recording)return -1;
    rc=ksg_del(&ks_grid,tr,col);if(!rc)ks_grid_dirty=1;return rc;
}
int ks_arr_grid_move(uint8_t tr,uint16_t from,uint16_t to){
    int rc;
    if(ks_mode!=KS_MODE_SONG||eng_running()||ks_recording)return -1;
    rc=ksg_move(&ks_grid,tr,from,to);if(!rc)ks_grid_dirty=1;return rc;
}
uint8_t ks_arr_mode(void){return ks_mode;}
int ks_arr_any_dirty(void){return ks_dirty||ks_song_dirty||ks_grid_dirty||ks_album_dirty||ks_assign_dirty;}
/* Unsaved TEMP pattern data must never block an unrelated S/M song. */
int ks_arr_songdata_dirty(void){return ks_song_dirty||ks_grid_dirty||ks_album_dirty;}
/* The PLAYER sees only existing stored S-Songs and albums.
 * S may have the native MIDI event record (9) or the grid record (12). */
int ks_arr_song_exists(uint16_t id){
    ab16_file f;
    if(id>=KS_SONG_COUNT||!ks_fs.mounted)return 0;
    return !ab16_lookup(&ks_fs,KS_GRID_TYPE,(uint32_t)id+1u,&f) ||
           !ab16_lookup(&ks_fs,KSM_AB16_SONG,(uint32_t)id+1u,&f);
}
int ks_arr_album_exists(uint16_t id){
    ab16_file f;
    return id<KS_ALBUM_COUNT && ks_fs.mounted &&
           !ab16_lookup(&ks_fs,KSM_AB16_ALBUM,(uint32_t)id+1u,&f);
}
int ks_arr_song_next(uint16_t cur,int dir,uint16_t *out){
    unsigned i;int c=cur;
    if(!out||(dir!=1&&dir!=-1))return -1;
    for(i=0;i<KS_SONG_COUNT;i++){
        c=(c+dir+(int)KS_SONG_COUNT)%(int)KS_SONG_COUNT;
        if(ks_arr_song_exists((uint16_t)c)){*out=(uint16_t)c;return 0;}
    }
    return -1;
}
int ks_arr_album_next(uint16_t cur,int dir,uint16_t *out){
    unsigned i;int c=cur;
    if(!out||(dir!=1&&dir!=-1))return -1;
    for(i=0;i<KS_ALBUM_COUNT;i++){
        c=(c+dir+(int)KS_ALBUM_COUNT)%(int)KS_ALBUM_COUNT;
        if(ks_arr_album_exists((uint16_t)c)){*out=(uint16_t)c;return 0;}
    }
    return -1;
}
uint16_t ks_arr_song_id(void){return ks_song_id;}
uint16_t ks_arr_album_id(void){return ks_album_id;}
uint16_t ks_arr_song_length(void){return ks_songs[ks_song_ix].length;}
uint16_t ks_arr_song_events(void){return ks_songs[ks_song_ix].count;}
int ks_arr_song_full(void){return ks_song_full;}
uint8_t ks_arr_album_count(void){return (uint8_t)ks_album.count;}
int ks_arr_song_overlay_get(uint8_t tr){
    return tr<KS_TRACKS ? !!(ks_songs[ks_song_ix].flags&(1u<<tr)):0;
}
int ks_arr_song_overlay_set(uint8_t tr,int on){
    ksm_song_t *s=&ks_songs[ks_song_ix];
    if(ks_mode!=KS_MODE_SONG||tr>=KS_TRACKS||eng_running()||ks_recording)return -1;
    if(on)s->flags|=(uint16_t)(1u<<tr);else s->flags&=(uint16_t)~(1u<<tr);
    ks_song_dirty=1;return 0;
}
uint8_t ks_arr_album_get(uint8_t index){return index<ks_album.count?(uint8_t)ks_album.song[index]:0u;}
int ks_arr_load_song(uint16_t id){
    int rc;
    if(id>=KS_SONG_COUNT||eng_running()||ks_recording||ks_song_dirty||ks_grid_dirty)return -1;
    rc=ks_native_song_load(id,&ks_songs[0]);if(rc)return rc;
    rc=ks_grid_load(id);if(rc)return rc;
    ks_gp_enabled=0;
    ks_song_ix=0;ks_song_id=id;ks_song_origin=0;ks_song_full=0;
    ksm_runtime_clear(&ks_song_run);ksm_prepare_play(&ks_songs[0],&ks_song_run);
    return 0;
}
int ks_arr_load_album(uint16_t id){
    int rc;
    if(id>=KS_ALBUM_COUNT||eng_running()||ks_recording||ks_album_dirty)return -1;
    rc=ks_native_album_load(id);if(!rc){
        if(ks_album.count){
            rc=ks_native_song_load(ks_album.song[0],&ks_songs[0]);
            if(!rc){ks_song_ix=0;ksm_runtime_clear(&ks_song_run);ksm_prepare_play(&ks_songs[0],&ks_song_run);}
        }
        if(!rc)ks_album_id=id;
    }
    return rc;
}
int ks_arr_set_mode(uint8_t mode){
    int rc=0;
    if(mode>KS_MODE_ALBUM||eng_running()||ks_recording||ks_song_dirty||ks_grid_dirty||ks_album_dirty)return -1;
    if(mode==KS_MODE_SONG)rc=ks_arr_load_song(ks_song_id);
    else if(mode==KS_MODE_ALBUM)rc=ks_arr_load_album(ks_album_id);
    if(rc)return rc;
    ks_gp_enabled=0;
    if(mode!=KS_MODE_PATTERN || ks_mode!=KS_MODE_PATTERN)ks_arr_live_disable();
    ks_mode=mode;ks_arr_offs();return 0;
}
int ks_arr_album_set(uint8_t slot,uint8_t song){
    if(ks_mode!=KS_MODE_ALBUM||eng_running()||ks_recording||slot>=KSM_MAX_ALBUM||song>=KS_SONG_COUNT)return -1;
    if(slot==0u){int rc=ks_native_song_load(song,&ks_songs[0]);if(rc)return rc;
        ks_song_ix=0;ksm_runtime_clear(&ks_song_run);ksm_prepare_play(&ks_songs[0],&ks_song_run);}
    ks_album.song[slot]=song;
    if(slot>=ks_album.count)ks_album.count=(uint16_t)(slot+1u);
    ks_album_dirty=1;return 0;
}
int ks_arr_album_length(uint8_t len){
    if(ks_mode!=KS_MODE_ALBUM||eng_running()||ks_recording||len>KSM_MAX_ALBUM)return -1;
    if(len && !ks_album.count){int rc=ks_native_song_load(ks_album.song[0],&ks_songs[0]);
        if(rc)return rc;
        ks_song_ix=0;}
    ks_album.count=len;ks_album_dirty=1;return 0;
}
static int ks_assign_store(void){
    ks_assign_file_t cfg;ab16_file check;uint32_t tr;int rc;
    if(!ks_fs.mounted||eng_running()||ks_recording)return -1;
    memset(&cfg,0,sizeof cfg);cfg.magic=KS_ASSIGN_MAGIC;cfg.version=2u;
    for(tr=0;tr<KS_TRACKS;tr++){
        cfg.pattern[tr]=ks_arr_assigned((uint8_t)tr);
        cfg.midi_channel[tr]=ks_route_channel[tr];
    }
    memcpy(ks_wire,&cfg,sizeof cfg);
    rc=ab16_put(&ks_fs,KS_ASSIGN_TYPE,1u,(uint16_t)sizeof cfg,ks_fill_wire,0);
    if(rc)return -3;
    rc=ab16_lookup(&ks_fs,KS_ASSIGN_TYPE,1u,&check);
    if(rc||check.length!=sizeof cfg)return -4;
    ks_assign_dirty=0;return 0;
}
static int ks_song_store(void){
    ksm_song_t*s=&ks_songs[ks_song_ix];ab16_file check;int rc;uint32_t n;
    if(ks_recording||eng_running()||!ks_fs.mounted)return -1;
    if(!ksm_song_valid(s))return -2;
    n=ksm_song_wire_size(s);memcpy(ks_wire,s,n);
    rc=ab16_put(&ks_fs,KSM_AB16_SONG,(uint32_t)ks_song_id+1u,(uint16_t)n,ks_fill_wire,0);
    if(rc)return -3;
    rc=ab16_lookup(&ks_fs,KSM_AB16_SONG,(uint32_t)ks_song_id+1u,&check);
    if(rc||check.length!=n)return -4;
    ks_song_dirty=0;return 0;
}
static int ks_album_store(void){
    ab16_file check;int rc;uint32_t n;
    if(ks_recording||eng_running()||!ks_fs.mounted||!ksm_album_valid(&ks_album))return -1;
    n=ksm_album_wire_size(&ks_album);memcpy(ks_wire,&ks_album,n);
    rc=ab16_put(&ks_fs,KSM_AB16_ALBUM,(uint32_t)ks_album_id+1u,(uint16_t)n,ks_fill_wire,0);
    if(rc)return -3;
    rc=ab16_lookup(&ks_fs,KSM_AB16_ALBUM,(uint32_t)ks_album_id+1u,&check);
    if(rc||check.length!=n)return -4;
    ks_album_dirty=0;return 0;
}
/* Incoming MIDI uses the currently selected performance track; source channel
 * is intentionally NOT used to reassign the four internal Sonivox parts. */
static void ks_song_capture(uint32_t packet){
    uint8_t st=(uint8_t)(packet>>8), cmd=st&0xf0u;
    uint8_t note=(uint8_t)((packet>>16)&127u),vel=(uint8_t)((packet>>24)&127u);
    int rc;uint32_t f;
    if(!ks_recording||!eng_running()||(cmd!=0x80u&&cmd!=0x90u))return;
    f=plat_lock();
    rc=ksm_record(&ks_songs[ks_song_ix],&ks_song_run,eng.seq_tick,ks_selected_track,cmd,note,vel);
    if(!rc)ks_song_dirty=1;
    else if(rc==-2){ /* hard capacity: complete all pending notes, do not silently truncate */
        ksm_record_finish(&ks_songs[ks_song_ix],&ks_song_run,eng.seq_tick);
        ks_recording=0;ks_song_dirty=1;ks_song_full=1;
    }
    plat_unlock(f);
}
static void ks_song_emit(void*ctx,uint8_t tr,uint8_t note,uint8_t vel,uint16_t gate){
    uint32_t i;(void)ctx;
    for(i=0;i<KS_ACTIVE&&ks_active[i].ticks;i++){}
    if(i==KS_ACTIVE)return;
    ks_active[i].track=tr;ks_active[i].note=note;ks_active[i].channel=ks_route_channel[tr];
    ks_active[i].ticks=gate;
    ks_arr_track_send(tr,0x90u,note,vel);
}
/* Called under the engine lock, possibly by external MIDI Start.
 * MUST NOT touch flash: album song 1 is preloaded by the UI selector. */
void ks_arr_transport_begin(void){
    ks_song_origin=0;ks_transport_ended=(ks_mode==KS_MODE_ALBUM && !ks_album.count);
    if(ks_gp_enabled){ks_gp_rewind();ks_gp_loop_begin=0;ks_gp_underrun=0;}
    ks_album_pending=0;ks_album_index=0;
    if(!ks_recording){
        ksm_runtime_clear(&ks_song_run);
        ksm_prepare_play(&ks_songs[ks_song_ix],&ks_song_run);
    }
}
/* Audio ISR requests transitions; flash I/O is deferred to ui main loop. */
int ks_arr_poll(void){
    if(ks_external_player)return 0;
    uint8_t next;int rc;uint32_t f;
    if(ks_gp_enabled){
        if(ks_gp_underrun || ks_gp_finished)return 1;
        ks_gp_poll_prefetch();
        return !!ks_gp_underrun;
    }
    if(ks_transport_ended)return 1;
    if(!ks_album_pending)return 0;
    if(!eng_running()){ks_album_pending=0;return 0;}
    next=(uint8_t)(ks_album_index+1u);
    if(next>=ks_album.count){ks_transport_ended=1;return 1;}
    rc=ks_native_song_load(ks_album.song[next],&ks_songs[1u-ks_song_ix]);
    if(rc){ks_transport_ended=1;return 1;}
    f=plat_lock();
    ks_arr_offs(); /* release all voices from previous album song */
    ks_song_ix=1u-ks_song_ix;ks_album_index=next;
    ks_song_origin=eng.seq_tick+1u;ks_album_pending=0;
    ksm_runtime_clear(&ks_song_run);
    ksm_prepare_play(&ks_songs[ks_song_ix],&ks_song_run);
    plat_unlock(f);
    return 0;
}
static void ks_song_transport_tick(uint32_t tick){
    ksm_song_t*s=&ks_songs[ks_song_ix];uint32_t local;
    if(ks_gp_enabled || ks_mode==KS_MODE_PATTERN||!eng_running()||ks_album_pending||ks_transport_ended)return;
    if(tick<ks_song_origin)return;
    local=tick-ks_song_origin;
    if(local>=(uint32_t)s->length*KSM_TICKS_STEP){
        if(ks_mode==KS_MODE_ALBUM&&ks_album.count){ks_album_pending=1;}
        else ks_transport_ended=1;
        return;
    }
    ks_visual_step=(uint8_t)((local/KSM_TICKS_STEP)&255u);
    if(!ks_recording)ksm_play_tick(s,&ks_song_run,local,ks_song_emit,0);
}
/* The physical SAVE button saves project/track config, not a TEMP pattern.
 * Only PATTERN > SAVE with explicit destination/overwrite may call the writer. */
static int ks_write_current_pattern(void)
{
    uint32_t n;int rc;ab16_file verify;
    if(ks_recording||eng_running())return -1;
    if(!ks_fs.mounted){ks_disk_status=2;return -3;}
    if(ks_pat.count>KS_MAX_EVENTS)return -4;
    ks_pat.magic=KS_PAT_MAGIC;ks_pat.version=KS_PAT_VERSION;
    ks_pat.crc=ks_pat_crc(&ks_pat);
    n=(uint32_t)offsetof(ks_pattern_t,event)+ks_pat.count*sizeof(ks_event_t);
    if(n+4u>AB16_MAX_DATA)return -4;
    memcpy(ks_wire,&ks_pat,n);
    memcpy(ks_wire+n,&ks_pat.crc,4u);
    rc=ab16_put(&ks_fs,AB16_KSTEP4,(uint32_t)ks_pattern_num+1u,(uint16_t)(n+4u),ks_fill_wire,0);
    if(rc){ks_fs.mounted=0;ks_disk_slot=255u;ks_disk_status=3;return -4;}
    rc=ab16_lookup(&ks_fs,AB16_KSTEP4,(uint32_t)ks_pattern_num+1u,&verify);
    if(rc||verify.length!=n+4u){ks_fs.mounted=0;ks_disk_slot=255u;ks_disk_status=3;return -5;}
    ks_dirty=0;ks_disk_status=1;ks_work_flush();ks_mix_sync_edit();return 0;
}
int ks_arr_save(void){
    if(ks_mode==KS_MODE_SONG && ks_song_dirty){int r=ks_song_store();if(r)return r;}
    if(ks_mode==KS_MODE_SONG && ks_grid_dirty){int r=ks_grid_store();if(r)return r;}
    if(ks_mode==KS_MODE_ALBUM && ks_album_dirty){int r=ks_album_store();if(r)return r;}
    if(ks_assign_dirty){int r=ks_assign_store();if(r)return r;}
    return 0; /* never silently commit an unsaved TEMP pattern */
}
/* Explicit menu-only operations: all flash reads/writes happen in main/UI loop. */
int ks_arr_work_unsaved(uint8_t tr){
    if(tr>=KS_TRACKS)return 0;
    return (tr==ks_selected_track?ks_dirty:ks_work_dirty[tr]) || ks_work_source[tr]==0xffffu;
}
int ks_arr_work_dirty(uint8_t tr){
    return tr<KS_TRACKS ? (tr==ks_selected_track ? ks_dirty : ks_work_dirty[tr]) : 0;
}
uint8_t ks_arr_work_length(uint8_t tr){
    if(tr>=KS_TRACKS)return 0;
    return tr==ks_selected_track?ks_pat.length:ks_work[tr].length;
}
int ks_arr_pattern_exists(uint16_t id){
    ab16_file f;int rc;
    if(id>=KS_PATTERNS||!ks_fs.mounted)return -1;
    rc=ab16_lookup(&ks_fs,AB16_KSTEP4,(uint32_t)id+1u,&f);
    return rc==0?1:rc==AB16_ENOTFOUND?0:-1;
}
/* Navigates committed, valid V3 patterns, never numeric holes or V2 objects. */
int ks_arr_pattern_next(uint16_t current,int dir,uint16_t *out){
    uint32_t found=(uint32_t)current+1u;
    unsigned n;uint32_t first=0u;
    if(!out||!ks_fs.mounted||!dir||ks_recording||eng_running())return -1;
    for(n=0;n<KS_PATTERNS;n++){
        uint32_t next;
        if(ab16_select_existing(&ks_fs,AB16_KSTEP4,found,dir>0?1:-1,&next)!=0)return -1;
        if(next==0u||next>KS_PATTERNS)return -1;
        if(n==0u)first=next;
        else if(next==first)return -1;
        found=next;
        {
            ab16_file f;uint8_t head[6];uint32_t magic;uint16_t ver;
            if(ab16_lookup(&ks_fs,AB16_KSTEP4,found,&f)!=0 || f.length<10u)continue;
            if(ab16_read(&ks_fs,AB16_KSTEP4,found,0u,head,6u)!=0)continue;
            memcpy(&magic,head,4u);memcpy(&ver,head+4u,2u);
            if(magic!=KS_PAT_MAGIC||ver!=KS_PAT_VERSION)continue;
            if(ks_read_pattern_into((uint16_t)(found-1u),&ks_tmp)==0){
                *out=(uint16_t)(found-1u);return 0;
            }
        }
    }
    return -1;
}
int ks_arr_work_clear(uint8_t tr,uint8_t length){
    ks_pattern_t blank;
    if(tr>=KS_TRACKS||length<8u||length>KS_MAX_STEPS||ks_recording||eng_running()||ks_mode!=KS_MODE_PATTERN)return -1;
    memset(&blank,0,sizeof blank);
    blank.magic=KS_PAT_MAGIC;blank.version=KS_PAT_VERSION;blank.length=length;
    blank.crc=ks_pat_crc(&blank);
    if(tr==ks_selected_track){ks_pat=blank;ks_dirty=1;ks_work_flush();ks_masks_refresh();}
    else {ks_work[tr]=blank;ks_work_dirty[tr]=1;}
    ks_work_source[tr]=0xffffu;
    (void)ks_mix_assign(&ks_mix,tr,ks_mix.slot[tr].pattern,&blank);
    ks_visual_step=0;
    return 0;
}
int ks_arr_work_load(uint8_t tr,uint16_t pat){
    int rc;
    if(tr>=KS_TRACKS||pat>=KS_PATTERNS||ks_recording||eng_running()||ks_mode!=KS_MODE_PATTERN)return -1;
    rc=ks_arr_pattern_exists(pat);
    if(rc<=0)return rc==0?-2:-3;
    {
        uint8_t h[6];uint32_t m;uint16_t v;
        if(ab16_read(&ks_fs,AB16_KSTEP4,(uint32_t)pat+1u,0u,h,6u)!=0)return -3;
        memcpy(&m,h,4u);memcpy(&v,h+4u,2u);
        if(m!=KS_PAT_MAGIC||v!=KS_PAT_VERSION)return -3;
    }
    rc=ks_read_pattern_into(pat,&ks_tmp);
    if(rc)return rc;
    if(ks_tmp.magic!=KS_PAT_MAGIC||ks_tmp.version!=KS_PAT_VERSION)return -3;
    /* save the other track's pending edits before switching workspaces */
    ks_work_flush();
    if(ks_mix.slot[tr].pattern!=pat)ks_assign_dirty=1;
    if(ks_mix_assign(&ks_mix,tr,pat,&ks_tmp))return -4;
    ks_work[tr]=ks_tmp;ks_work_source[tr]=pat;ks_work_dirty[tr]=0;
    if(tr==ks_selected_track){ks_pat=ks_tmp;ks_dirty=0;ks_pattern_num=pat;ks_masks_refresh();}
    return 0;
}
int ks_arr_work_save(uint8_t tr,uint16_t pat,int overwrite){
    int existed,rc;uint16_t old_id;
    if(tr>=KS_TRACKS||pat>=KS_PATTERNS||ks_recording||eng_running()||ks_mode!=KS_MODE_PATTERN)return -1;
    if(!ks_fs.mounted)return -3;
    existed=ks_arr_pattern_exists(pat);
    if(existed<0)return -3;
    if(existed&&!overwrite)return -6; /* explicit confirmation required */
    ks_arr_set_track(tr);
    old_id=ks_pattern_num;
    ks_pattern_num=pat;
    ks_dirty=1; /* save even if TEMP has no notes or is copied unchanged */
    rc=ks_write_current_pattern();
    if(rc){ks_pattern_num=old_id;ks_work_dirty[tr]=1;ks_dirty=1;return rc;}
    ks_work_source[tr]=pat;
    if(ks_mix.slot[tr].pattern!=pat)ks_assign_dirty=1;
    (void)ks_mix_assign(&ks_mix,tr,pat,&ks_pat);
    ks_work_flush();
    return 0;
}
/* Destructive: called ONLY from the double-confirmed hidden button gesture.
 * Erases ALL THREE sample slots, then explicitly creates AB16's superblock.
 * Boot and ordinary SAVE never format any medium. */
int ks_arr_erase_all_samples(void)
{
    uint32_t sec,i,off;const uint8_t *raw;int rc;
    if(ks_recording||eng_running())return -1;
    for(sec=0;sec<KS_FLASH_SECTORS;++sec){
        off=KS_FLASH_BASE+sec*KS_FLASH_SECTOR;
        if(!plat_in(off,KS_FLASH_SECTOR,KS_FLASH_BASE,KS_SAMPLE_ALL_END)||!plat_data(off))return -2;
    }
    ks_arr_offs();ks_fs.mounted=0;ks_disk_slot=255u;
    for(sec=0;sec<KS_FLASH_SECTORS;++sec){
        off=KS_FLASH_BASE+sec*KS_FLASH_SECTOR;
        if(plat_data_erase(off)!=0){ks_disk_status=3;return -4;}
        raw=plat_data(off);
        if(!raw){ks_disk_status=3;return -5;}
        for(i=0;i<KS_FLASH_SECTOR;++i)if(raw[i]!=0xFFu){ks_disk_status=3;return -5;}
    }
    rc=ab16_create_erased(&ks_fs,ks_fs_io());
    if(rc){ks_disk_status=3;return -6;}
    ks_pattern_num=0;ks_visual_step=0;
    ks_disk_slot=0u;ks_empty();ks_disk_status=1;
    ksm_song_clear(&ks_songs[0]);ksm_song_clear(&ks_songs[1]);ksm_album_clear(&ks_album);
    ks_song_dirty=ks_album_dirty=ks_assign_dirty=ks_grid_dirty=0;
    ksg_clear(&ks_grid);ks_mode=KS_MODE_PATTERN;
    {unsigned t;for(t=0;t<KS_TRACKS;t++)(void)ks_mix_assign(&ks_mix,t,0u,&ks_pat);}
    ks_mix_ready=1u;
    return 0;
}

int ks_arr_rec_toggle(void)
{
    if(ks_mode==KS_MODE_ALBUM)return 0;
    if(ks_mode==KS_MODE_SONG){
        uint32_t f=plat_lock();
        if(ks_recording){ksm_record_finish(&ks_songs[ks_song_ix],&ks_song_run,eng.seq_tick);ks_recording=0;ks_song_dirty=1;}
        else if(!ks_song_dirty){ks_arr_offs();ksm_song_clear(&ks_songs[ks_song_ix]);ksm_runtime_clear(&ks_song_run);ks_song_full=0;ks_recording=1;}
        plat_unlock(f);return ks_recording;
    }
    uint32_t f=plat_lock();
    ks_recording=!ks_recording;
    memset(ks_pending,0,sizeof ks_pending); /* stale notes cannot affect later takes */
    plat_unlock(f);
    return ks_recording;
}
void ks_arr_record_packet(uint32_t packet)
{
    if(ks_mode==KS_MODE_SONG){ks_song_capture(packet);return;}
    if(ks_mode==KS_MODE_ALBUM)return;
    uint8_t st=(uint8_t)(packet>>8), cmd=st&0xF0u;
    uint8_t note=(uint8_t)((packet>>16)&0x7Fu),vel=(uint8_t)((packet>>24)&0x7Fu);
    uint16_t *pending;
    ks_event_t *event;
    uint32_t i,tick=ks_clock;
    if(!ks_recording || !eng_running() || (st&0x0Fu)==9u || note>127u)return;
    if(cmd!=0x90u && cmd!=0x80u)return;
    if(cmd==0x90u && vel==0u)cmd=0x80u;
    {
    uint32_t f=plat_lock(); /* CPU0 audio/timer ISR also reads these events. */
    pending=&ks_pending[ks_selected_track][note];
    if(cmd==0x80u){
        if(*pending){
            event=&ks_pat.event[*pending-1u];
            i=tick-(uint32_t)ks_rec_start[ks_selected_track][note];
            ks_event_set_ticks(event,(uint16_t)(i<1u?1u:i>KS_MAX_GATE_TICKS?KS_MAX_GATE_TICKS:i));
            *pending=0;
            ks_dirty=1;
            ks_mix_sync_edit();
        }
        plat_unlock(f);
        return;
    }
    if(ks_pat.count>=KS_MAX_EVENTS){plat_unlock(f);return;}
    event=&ks_pat.event[ks_pat.count++];
    /* A recorded note belongs to the pattern, never to this track. */
    event->step=(uint8_t)(ks_visual_step%ks_pat.length);
    event->note=note;event->vel=vel;ks_event_set_ticks(event,3u);event->chance=100;
    *pending=ks_pat.count;
    ks_rec_start[ks_selected_track][note]=tick;
    ks_dirty=1;
    ks_mix_sync_edit();
    ks_masks_refresh();
    plat_unlock(f);
    }
}
void ks_arr_clock_tick(void)
{
    uint32_t i;
    ks_clock++;
    if(!ks_external_player)ks_song_transport_tick(eng.seq_tick);
    for(i=0;i<KS_ACTIVE;++i)if(ks_active[i].ticks){
        if(--ks_active[i].ticks==0){
            ks_arr_track_send(ks_active[i].track,0x80u,ks_active[i].note,0u);
        }
    }
}
void ks_arr_step(uint32_t pos)
{
    uint32_t i,j;
    if(ks_external_player)return;
    if(ks_gp_enabled && ks_mode==KS_MODE_SONG){ks_gp_emit_step(pos);return;}
    if(ks_mode!=KS_MODE_PATTERN){
        const ksm_song_t *song=&ks_songs[ks_song_ix];
        if(ks_mode!=KS_MODE_SONG&&ks_mode!=KS_MODE_ALBUM)return;
        if(!song->flags||ks_album_pending||ks_transport_ended||eng.seq_tick<ks_song_origin)return;
        pos=(eng.seq_tick-ks_song_origin)/KS_GATE_TICKS_PER_STEP;
    }else {
        if(ks_live_enabled)for(i=0;i<KS_TRACKS;i++)ks_arr_live_boundary(i,pos);
        ks_visual_step=(uint8_t)((pos-(ks_live_enabled?ks_live_origin[ks_selected_track]:0u))
                              %ks_arr_play_len(ks_selected_track));
    }
    for(i=0;i<KS_TRACKS;++i){
        const ks_mix_slot_t *slot=ks_live_slot(i);
        if(ks_mode!=KS_MODE_PATTERN && !(ks_songs[ks_song_ix].flags&(1u<<i)))continue;
        uint8_t at=(uint8_t)((pos-(ks_live_enabled&&ks_mode==KS_MODE_PATTERN?ks_live_origin[i]:0u))%slot->length);
        uint32_t n;
        for(n=0;n<slot->count;n++){
            const ks_event_t *e=&slot->event[n];
            if(e->step!=at)continue;
            if(e->chance<100u){
                ks_chance_rng^=ks_chance_rng<<13;
                ks_chance_rng^=ks_chance_rng>>17;
                ks_chance_rng^=ks_chance_rng<<5;
                if(ks_chance_rng%100u>=e->chance)continue;
            }
            for(j=0;j<KS_ACTIVE&&ks_active[j].ticks;j++){}
            if(j==KS_ACTIVE)continue;
            ks_active[j].note=e->note;
            ks_active[j].channel=ks_route_channel[i];
            ks_active[j].track=(uint8_t)i;
            ks_active[j].ticks=ks_event_ticks(e);
            ks_arr_track_send((uint8_t)i,0x90u,e->note,e->vel);
        }
    }
}

/* Shared LOOM-style step editor backend. RAM mutations only. */
int ks_arr_editor_get(uint8_t tr,uint8_t step,void *out_step)
{
    kse_step_t *out=out_step;uint32_t i;
    if(!out||tr>=KS_TRACKS||step>=ks_pat.length)return -1;
    memset(out,0,sizeof(*out));out->gate=100;out->chance=100;out->span=1;
    for(i=0;i<ks_pat.count;++i){
        const ks_event_t *e=&ks_pat.event[i];
        if(e->step==step){
            if(out->count>=KSE_MAX_CHORD)return -1;
            out->note[out->count]=e->note;out->velocity[out->count]=e->vel;
            {
                uint16_t ticks=ks_event_ticks(e);
                uint16_t partial=(uint16_t)((ticks-1u)%KS_GATE_TICKS_PER_STEP+1u);
                out->span=(uint8_t)((ticks-1u)/KS_GATE_TICKS_PER_STEP+1u);
                out->gate=(uint8_t)((partial*100u+3u)/KS_GATE_TICKS_PER_STEP);
            }
            out->chance=e->chance;out->count++;
        }
    }
    return 0;
}
/* KSTEP_LIVE_EDITOR_V051: editor writes are allowed DURING playback.
 * The 1kHz sequencer ISR reads ks_pat; pause that ISR only for the RAM
 * mutation. Do not erase, write flash, reset phase or stop playback here.
 * Live recording still excludes editor edits (its pending[] indexes change). */
int ks_arr_editor_set(uint8_t tr,uint8_t step,const void *in_step)
{
    const kse_step_t *in=in_step;uint32_t i,j,n=0,f;
    if(!in||tr>=KS_TRACKS||step>=ks_pat.length||in->count>KSE_MAX_CHORD||
       ks_recording||in->gate>100u||in->chance>100u||in->span>KS_MAX_STEPS)return -1;
    for(j=0;j<in->count;++j)if(in->note[j]>127u||in->velocity[j]>127u)return -1;
    f=plat_lock(); /* include capacity check: no competing tick or recorder writer */
    for(i=0;i<ks_pat.count;++i)if(ks_pat.event[i].step!=step)n++;
    if(n+in->count>KS_MAX_EVENTS){plat_unlock(f);return -1;}
    n=0;
    for(i=0;i<ks_pat.count;++i)if(ks_pat.event[i].step!=step)
        ks_pat.event[n++]=ks_pat.event[i];
    for(j=0;j<in->count;++j){
        ks_event_t *e=&ks_pat.event[n++];
        uint16_t ticks;
        e->step=step;e->note=in->note[j];
        e->vel=in->velocity[j]?in->velocity[j]:100u;
        ticks=(uint16_t)((uint16_t)((in->span?in->span:1u)-1u)*KS_GATE_TICKS_PER_STEP+
              ((uint32_t)in->gate*KS_GATE_TICKS_PER_STEP+50u)/100u);
        if(!ticks)ticks=1;
        ks_event_set_ticks(e,ticks);
        e->chance=in->chance;
    }
    ks_pat.count=(uint16_t)n;ks_dirty=1;
    ks_mix_sync_edit();ks_masks_refresh();
    plat_unlock(f);
    return 0;
}
