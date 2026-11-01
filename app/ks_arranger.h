/* keySTEP ONE: isolated arranger. No change to CTL project format or CH10 drums. */
#ifndef KS_ARRANGER_H
#define KS_ARRANGER_H
#include <stdint.h>
#include "onekey_play.h"
#define KS_TRACKS 4u
#define KS_PATTERNS 512u
#define KS_FLASH_SECTORS 60u
#define KS_MAX_STEPS 128u
#define KS_MAX_EVENTS 640u
#define KS_PAT_MAGIC 0x4B53314Cu
#define KS_PAT_VERSION 3u
/* v0.4.3: the ENTIRE THREE user sample slots are the MUSIC DISK.
 * 0xA0000..0xDBFFF inclusive = 60 x 4 KiB = 240 KiB; NO other flash.
 * Existing samples must be explicitly erased via a double-confirmed hidden
 * panel gesture. NEVER erase/format during boot or an ordinary SAVE.
 * This erases ALL old sample content (not Sonivox ROM sound fonts). */
#define KS_SAMPLE_SLOT_BASE 0xA0000u
#define KS_SAMPLE_SLOT_END 0xB4000u
#define KS_FLASH_BASE KS_SAMPLE_SLOT_BASE
#define KS_SAMPLE_SLOT_BYTES 0x14000u
#define KS_SAMPLE_SLOT_COUNT 3u
#define KS_SAMPLE_ALL_END 0xDC000u
#define KS_FLASH_SECTOR 4096u
_Static_assert(KS_SAMPLE_SLOT_BASE + KS_SAMPLE_SLOT_BYTES*KS_SAMPLE_SLOT_COUNT == KS_SAMPLE_ALL_END,
               "sample window bounds changed");
_Static_assert(KS_SAMPLE_ALL_END - KS_FLASH_BASE == KS_FLASH_SECTORS*KS_FLASH_SECTOR,
               "the AB16 data window must exactly fill 3 sample slots");

/* V3: A pattern is ONE independent musical phrase. No MIDI channel, no
 * instrument and NO track number anywhere in its persistent layout.
 * MIDI tick duration is represented with gate_lo/hi (6 ticks = 1 step).
 * All four performance tracks can reference the same saved pattern ID. */
typedef struct {
    uint8_t step, note, vel, gate_lo, gate_hi, chance;
} ks_event_t;
typedef struct {
    uint32_t magic;
    uint16_t version, count;
    uint8_t length, flags;
    ks_event_t event[KS_MAX_EVENTS];
    uint32_t crc;
} ks_pattern_t;

extern uint16_t ks_pattern_num;
extern uint8_t ks_selected_track, ks_recording, ks_dirty;
extern volatile uint8_t ks_visual_step;
extern uint8_t ks_disk_status; /* 0 empty, 1 keySTEP owned, 2 protected, 3 flash error */
extern uint8_t ks_disk_slot;   /* legacy compatibility: 0 when ready, 255 if unavailable */
void ks_arr_init(void);
int ks_arr_load(uint16_t pattern);
int ks_arr_save(void); /* config / SONG / ALBUM only, never auto-save TEMP PATTERN */
/* V0520 explicit pattern menu APIs: NO track/channel stored in pattern. */
int ks_arr_work_clear(uint8_t track,uint8_t length);
int ks_arr_work_load(uint8_t track,uint16_t pattern);
int ks_arr_work_save(uint8_t track,uint16_t pattern,int overwrite);
int ks_arr_work_dirty(uint8_t track);
int ks_arr_work_unsaved(uint8_t track); /* includes empty, unassigned boot slots; display only */
uint8_t ks_arr_work_length(uint8_t track);
int ks_arr_pattern_exists(uint16_t pattern);
int ks_arr_pattern_next(uint16_t current,int direction,uint16_t *found);
int ks_arr_erase_all_samples(void); /* DANGEROUS: 60-sector explicit user-confirmed wipe */
void ks_arr_set_track(uint8_t track);
/* KSTEP_TRACK_LOCAL_SOUND_V053: separate external MIDI channel and internal GM channel. */
void ks_arr_track_send(uint8_t track,uint32_t status,uint32_t note,uint32_t velocity);
void ks_arr_set_length(uint8_t length);
void ks_arr_set_channel(uint8_t channel); /* 0..15 channels, 16=ALL broadcast */
/* KSTEP_HOME_TRACK_KNOBS_V054: STOP notes on tracks newly hidden by SOLO/MUTE. */
void ks_arr_silence_inaudible(void);
int ks_arr_rec_toggle(void);
void ks_arr_record_packet(uint32_t packet);
void ks_arr_clock_tick(void);
void ks_arr_step(uint32_t step);
void ks_arr_offs(void);
/* Read-only ONE TOUCH navigation of currently loaded native S or song grid.
 * Main/UI loop only, never the clock/audio interrupt. Cursor is tick absolute.
 * No TEMP mutation and no MUSIC DISK writes. */
int ks_arr_onetouch_seek(uint32_t cursor,int valid,int direction,
                         uint32_t *next_tick,okp_note_t notes[OKP_MAX_CHORD]);
uint8_t ks_arr_block(void);
uint8_t ks_arr_blocks(void);
uint8_t ks_arr_len(void);
uint8_t ks_arr_channel(void);
uint16_t ks_arr_mask(uint8_t track, uint8_t block);
/* V0515: player source per track; PATTERN EDIT remains the editable AB16 RAM buffer. */
int ks_arr_assign(uint8_t track,uint16_t pattern);
uint16_t ks_arr_assigned(uint8_t track);
/* LFO LIVE PLAY: prefetch outside ISR, queue for per-track next boundary. */
void ks_arr_live_enable(void);
void ks_arr_live_transport_reset(void);
void ks_arr_live_disable(void);
int ks_arr_live_queue(uint8_t track,uint16_t pattern);
int ks_arr_live_queued(uint8_t track,uint16_t *pattern);
int ks_arr_live_current(uint8_t track,uint16_t *pattern);
/* LIVE four-fader mix: volatile (never stored on MUSIC DISK), 0..100 percent.
 * Internal GM: channel-local CC7 adjusts already-sounding notes.
 * External USB/BLE: scale only new Note-On velocity per track; no global CC7. */
uint8_t ks_arr_live_volume_get(uint8_t track);
void ks_arr_live_volume_set(uint8_t track,uint8_t percent);
uint8_t ks_arr_play_len(uint8_t track);
uint16_t ks_arr_edit_mask(uint8_t track,uint8_t block);
int ks_arr_play_get(uint8_t track,uint8_t step,void *out_step);
uint16_t ks_arr_event_count(void);
/* Editor functions edit RAM only; SAVE commits through analogBEAST AB16 log. */
int ks_arr_editor_get(uint8_t track,uint8_t step,void *out_step);
int ks_arr_editor_set(uint8_t track,uint8_t step,const void *in_step);
/* Native event-song and album engine (AB16 types 9/10). */
uint8_t ks_arr_mode(void); /* 0 PATTERN, 1 SONG, 2 ALBUM */
int ks_arr_any_dirty(void);
int ks_arr_songdata_dirty(void); /* ignores unsaved pattern workspaces */
int ks_arr_set_mode(uint8_t mode);
int ks_arr_load_song(uint16_t id); /* 0..63 */
int ks_arr_load_album(uint16_t id); /* 0..31 */
uint16_t ks_arr_song_id(void),ks_arr_album_id(void);
uint16_t ks_arr_song_length(void),ks_arr_song_events(void);
int ks_arr_song_full(void);
uint8_t ks_arr_album_count(void),ks_arr_album_get(uint8_t slot);
int ks_arr_album_set(uint8_t slot,uint8_t song);
int ks_arr_album_length(uint8_t count);
/* SONG EDIT: four ordered pattern-reference rows, independent of note-event SONG. */
uint16_t ks_arr_grid_count(uint8_t tr);
uint16_t ks_arr_grid_get(uint8_t tr,uint16_t col); /* 0xffff = end (+) */
int ks_arr_grid_put(uint8_t tr,uint16_t col,uint16_t pat);
int ks_arr_grid_del(uint8_t tr,uint16_t col);
int ks_arr_grid_move(uint8_t tr,uint16_t from,uint16_t to);
int ks_arr_grid_dirty(void);
/* SONG arranger loop: preload flash patterns in main loop, never in clock ISR. */
int ks_arr_grid_play_prepare(void);
void ks_arr_grid_play_stop(void);
void ks_arr_grid_set_loop(int on);
void ks_arr_external_player_set(int on);
int ks_arr_song_exists(uint16_t id);
int ks_arr_album_exists(uint16_t id);
int ks_arr_song_next(uint16_t cur,int dir,uint16_t *out);
int ks_arr_album_next(uint16_t cur,int dir,uint16_t *out);
int ks_arr_grid_play_active(void);
int ks_arr_grid_play_underrun(void);
int ks_arr_song_overlay_get(uint8_t track);
int ks_arr_song_overlay_set(uint8_t track,int enabled);
void ks_arr_transport_begin(void);
int ks_arr_poll(void); /* main loop only: 1 = finished, safe to call STOP */
#endif
