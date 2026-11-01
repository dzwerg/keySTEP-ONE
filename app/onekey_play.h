/* SPDX-License-Identifier: MIT
 * keySTEP ONE: ONE KEY PLAY transport (standalone implementation).
 * Hardware-tested on v0.5.7, expanded to sixteen triggers in v0.5.30.
 * No allocation, no flash writes.
 */
#ifndef KSTEP_ONEKEY_PLAY_H
#define KSTEP_ONEKEY_PLAY_H
#include <stdint.h>

#define OKP_MAX_CHORD 64u
#define OKP_KEYS 16u
#define OKP_NOTE_NONE 0xffffffffu

typedef struct {
    uint8_t track;      /* 0..3, source track. Preserve internal instrument. */
    uint8_t channel;    /* 0..15: external MIDI channel */
    uint8_t pitch;      /* 0..127 */
    uint8_t velocity;   /* 1..127; from the pattern/song */
} okp_note_t;

typedef struct {
    /* event n = all the simultaneous notes at a tick; empty steps have no event.
     * Return number of notes 1..OKP_MAX_CHORD, or <= 0 on error/empty event. */
    int (*read_event)(void *user, uint32_t index, okp_note_t notes[OKP_MAX_CHORD]);
    /* Route the note through the existing per-track local synth and MIDI paths.
     * The callback must send note-off for the same track/channel/pitch as note-on. */
    void (*emit_note)(void *user, const okp_note_t *note, int is_on);
    void *user;
    /* Optional random-access musical-onset seek for S/M songs. Cursor is
     * source-defined (tick for S, first event index for M); no Flash in ISR.
     * Returns 1..OKP_MAX_CHORD, <=0 if end/error. */
    int (*seek_event)(void *user, uint32_t cursor, int valid, int direction,
                      uint32_t *next_cursor, okp_note_t notes[OKP_MAX_CHORD]);
} okp_io_t;

typedef struct {
    okp_io_t io;
    uint32_t count;       /* number of note events, NOT number of steps */
    uint32_t cursor;       /* last triggered event; -1 = not started */
    uint8_t paused;       /* 1 ONLY for actual PAUSE, not STOP */
    uint8_t seek_mode;
    uint8_t wrap;         /* PATTERN loop: 1; finite SONG: 0 */
    uint8_t held[OKP_KEYS];
    uint8_t voice_count[OKP_KEYS];
    okp_note_t voices[OKP_KEYS][OKP_MAX_CHORD];
} okp_t;

/* 16 independent triggers: reverse T1-4 and T9-12; forward T5-8 and T13-16.
 * Do not alias several physical keys to a single held-note slot. */
enum { OKP_T1=1, OKP_T2, OKP_T3, OKP_T4, OKP_T5, OKP_T6, OKP_T7, OKP_T8,
       OKP_T9, OKP_T10, OKP_T11, OKP_T12, OKP_T13, OKP_T14, OKP_T15, OKP_T16 };
void okp_init(okp_t *p, const okp_io_t *io);
void okp_set_source(okp_t *p, uint32_t event_count, int loop);
void okp_set_seek_source(okp_t *p);
/* SONG ONE TOUCH uses PLAYER K4 ONCE/LOOP; changing it never resets cursor. */
void okp_set_seek_loop(okp_t *p, int loop);
void okp_set_paused(okp_t *p, int paused);
void okp_release_all(okp_t *p);
/* Every white step key 1..16 is active in PAUSE. Black keys remain silent. */
int okp_key(okp_t *p, int key_number, int down);
#endif
