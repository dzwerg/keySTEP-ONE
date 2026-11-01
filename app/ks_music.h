/* keySTEP ONE native event-based SONG and ALBUM formats.
 * New AB16 object types; no change to existing pattern (type 8) or legacy AB16.
 * 24 PPQN, 6 ticks per sixteenth. Runtime state never serialized.
 */
#ifndef KS_MUSIC_H
#define KS_MUSIC_H
#include <stdint.h>
#define KSM_MAX_STEPS 2000u
#define KSM_TICKS_STEP 6u
#define KSM_MAX_EVENTS 480u
#define KSM_MAX_ALBUM 32u
#define KSM_SONG_MAGIC 0x3147534bu /* KSG1 little endian */
#define KSM_ALBUM_MAGIC 0x31414c4bu /* KLA1 */
#define KSM_VERSION 1u
#define KSM_AB16_SONG 9u
#define KSM_AB16_ALBUM 10u
/* Exactly 8 bytes per event. Sparse: no bytes for empty steps. */
typedef struct {
    uint16_t at, gate; /* tick absolute from song start, duration in ticks */
    uint8_t track, note, vel, flags;
} ksm_event_t;
typedef struct {
    uint32_t magic;
    uint16_t version, count, length, flags; /* length in sixteenth-note steps */
    ksm_event_t event[KSM_MAX_EVENTS];
} ksm_song_t;
typedef struct {
    uint32_t magic;
    uint16_t version, count;
    uint16_t song[KSM_MAX_ALBUM]; /* song IDs 0..63 */
} ksm_album_t;
typedef struct {
    uint16_t pending[4][128]; /* index+1 of currently recording note */
    uint16_t play_cursor;
    uint8_t recording;
} ksm_runtime_t;
typedef void (*ksm_emit_t)(void*,uint8_t,uint8_t,uint8_t,uint16_t);
void ksm_song_clear(ksm_song_t*);
void ksm_album_clear(ksm_album_t*);
int ksm_song_valid(const ksm_song_t*);
int ksm_album_valid(const ksm_album_t*);
void ksm_runtime_clear(ksm_runtime_t*);
int ksm_record(ksm_song_t*,ksm_runtime_t*,uint32_t tick,uint8_t tr,uint8_t status,uint8_t note,uint8_t vel);
void ksm_record_finish(ksm_song_t*,ksm_runtime_t*,uint32_t tick);
void ksm_prepare_play(ksm_song_t*,ksm_runtime_t*);
unsigned ksm_play_tick(const ksm_song_t*,ksm_runtime_t*,uint32_t tick,ksm_emit_t emit,void*user);
unsigned ksm_song_wire_size(const ksm_song_t*);
unsigned ksm_album_wire_size(const ksm_album_t*);
#endif
