/* keySTEP ONE v0.5 editor core — transport / flash-format independent.
 * NOTE: this is a portable component, NOT an installed FM-1 firmware feature.
 * Host code is free to adapt the callbacks to analogBEAST v1.2.
 * SPDX-License-Identifier: MIT */
#ifndef KS_STEP_EDITOR_H
#define KS_STEP_EDITOR_H
#include <stdint.h>

#define KSE_TRACKS 4u
#define KSE_PATTERNS 512u
#define KSE_MIN_STEPS 8u
#define KSE_MAX_STEPS 128u
#define KSE_PAGE_STEPS 16u
#define KSE_MAX_CHORD 8u
#define KSE_FIXED_VELOCITY 100u

typedef struct {
    uint8_t count;
    uint8_t note[KSE_MAX_CHORD];
    uint8_t velocity[KSE_MAX_CHORD];
    uint8_t gate;    /* 1..100, percentage of FINAL 16th */
    uint8_t span;    /* KSTEP_LOOM_EDITOR_FIX_V052: note length 1..128 16th steps */
    uint8_t chance;  /* 0..100; default 100 */
} kse_step_t;

typedef struct {
    int (*read_step)(void *user, unsigned track, unsigned pattern, unsigned step, kse_step_t *out);
    int (*write_step)(void *user, unsigned track, unsigned pattern, unsigned step, const kse_step_t *data);
    int (*read_length)(void *user, unsigned track, unsigned pattern, unsigned *length);
    int (*write_length)(void *user, unsigned track, unsigned pattern, unsigned length);
    /* Mutations are volatile until the existing analogBEAST SAVE commits. */
} kse_backend_t;

typedef struct {
    kse_backend_t io;
    void *user;
    uint16_t pattern[KSE_TRACKS];
    uint8_t track;
    uint8_t bank;             /* 16-step viewport 0..7 */
    uint8_t opened;
    int16_t held_step;        /* absolute step or -1 */
    uint8_t chord_count;
    uint8_t chord[KSE_MAX_CHORD];
    uint8_t chord_vel[KSE_MAX_CHORD];
    uint8_t default_gate;
    uint8_t default_chance;
    uint8_t dirty;
    /* Single-step undo, no physical flash writes. */
    uint8_t undo_valid;
    uint8_t undo_track;
    uint16_t undo_pattern;
    uint8_t undo_step;
    kse_step_t undo_before;
    uint8_t clipboard_valid;
    kse_step_t clipboard;
} kse_editor_t;

void kse_init(kse_editor_t *e, kse_backend_t backend, void *user);
void kse_open(kse_editor_t *e, int on);
void kse_algo(kse_editor_t *e,int delta);
void kse_select_track(kse_editor_t *e,unsigned track); /* absolute TR1..TR4, no drift */
int kse_pattern(kse_editor_t *e, unsigned pattern); /* 0..511 */
int kse_page(kse_editor_t *e, int delta); /* OCT +/- */
int kse_arm_notes(kse_editor_t *e, const uint8_t *notes, const uint8_t *velocity, unsigned count);
/* Tapping a white key: occupied -> empty, empty -> current chord. */
int kse_toggle_white(kse_editor_t *e, unsigned white_index); /* 0..15 */
int kse_hold_white(kse_editor_t *e,int white_index); /* -1 releases */
int kse_extend_held_to(kse_editor_t *e,unsigned white_index); /* first white held + later white sets note length */
int kse_edit_held_gate(kse_editor_t *e, unsigned gate);
int kse_edit_held_chance(kse_editor_t *e, unsigned chance);
int kse_edit_held_note(kse_editor_t *e, unsigned index, unsigned note);
int kse_edit_held_velocity(kse_editor_t *e,unsigned idx,unsigned velocity);
int kse_replace_held_chord(kse_editor_t *e,const uint8_t *notes,unsigned count);
int kse_copy_held(kse_editor_t *e);
int kse_paste_held(kse_editor_t *e);
int kse_undo(kse_editor_t *e);
int kse_set_length(kse_editor_t *e, unsigned length); /* 8..128 */
#endif
