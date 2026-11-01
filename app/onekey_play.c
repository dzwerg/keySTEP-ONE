/* SPDX-License-Identifier: MIT */
#include "onekey_play.h"
#include <string.h>

static int okp_key_slot(int n)
{
    return n >= 1 && n <= 16 ? n - 1 : -1;
}
static int okp_reverse(int n)
{
    return (n >= 1 && n <= 4) || (n >= 9 && n <= 12);
}
static int okp_same_note(const okp_note_t *a, const okp_note_t *b)
{
    return a->track == b->track && a->channel == b->channel && a->pitch == b->pitch;
}
static int okp_other_holds(const okp_t *p, unsigned except, const okp_note_t *n)
{
    unsigned k, i;
    for (k = 0; k < OKP_KEYS; ++k) {
        if (k == except) continue;
        for (i = 0; i < p->voice_count[k]; ++i)
            if (okp_same_note(&p->voices[k][i], n)) return 1;
    }
    return 0;
}
static int okp_already_in_slot(const okp_t *p, unsigned slot, const okp_note_t *n)
{
    unsigned i;
    for (i = 0; i < p->voice_count[slot]; i++)
        if (okp_same_note(&p->voices[slot][i], n)) return 1;
    return 0;
}
static void okp_release_slot(okp_t *p, unsigned slot)
{
    unsigned i;
    /* Clear this slot only AFTER determining whether other keys still own each note. */
    for (i = 0; i < p->voice_count[slot]; ++i) {
        const okp_note_t *n = &p->voices[slot][i];
        if (p->io.emit_note && !okp_other_holds(p, slot, n))
            p->io.emit_note(p->io.user, n, 0);
    }
    p->voice_count[slot] = 0;
    p->held[slot] = 0;
}
void okp_release_all(okp_t *p)
{
    unsigned i;
    for (i = 0; i < OKP_KEYS; i++) okp_release_slot(p, i);
}
void okp_init(okp_t *p, const okp_io_t *io)
{
    memset(p, 0, sizeof *p);
    if (io) p->io = *io;
    p->cursor = OKP_NOTE_NONE;
}
void okp_set_source(okp_t *p, uint32_t event_count, int loop)
{
    okp_release_all(p);
    p->seek_mode=0u;
    p->count = event_count;
    p->wrap = loop ? 1u : 0u;
    p->cursor = OKP_NOTE_NONE;
}
void okp_set_seek_source(okp_t *p)
{
    okp_release_all(p);
    p->seek_mode=1u;
    p->count=0u;
    p->wrap=0u; /* S/M songs are finite; their PLAYER controls looping. */
    p->cursor=OKP_NOTE_NONE;
}
void okp_set_seek_loop(okp_t *p, int loop)
{
    /* Called when K4 changes or a SONG source becomes active: no note-offs,
     * cursor resets, or mutation of source data. PATTERN always keeps wrap. */
    if(p->seek_mode)p->wrap=loop?1u:0u;
}
void okp_set_paused(okp_t *p, int paused)
{
    if (p->paused != (paused ? 1u : 0u)) okp_release_all(p);
    p->paused = paused ? 1u : 0u;
}
int okp_key(okp_t *p, int key_number, int down)
{
    int slot, reverse;
    int32_t idx;
    okp_note_t notes[OKP_MAX_CHORD];
    int n, j;
    if (!p->paused) return 0;
    slot = okp_key_slot(key_number);
    if (slot < 0) return 0;              /* UI silences all other keys */
    if (!down) {
        okp_release_slot(p, (unsigned)slot);
        return 1;
    }
    if (p->held[slot]) return 1;         /* key repeat: no repeated note-on */
    p->held[slot] = 1;
    reverse = okp_reverse(key_number);
    if (p->seek_mode) {
        uint32_t next=OKP_NOTE_NONE;
        if (!p->io.seek_event) return 1;
        n=p->io.seek_event(p->io.user,p->cursor,p->cursor!=OKP_NOTE_NONE,
                           reverse?-1:1,&next,notes);
        /* End of finite S/M song: only LOOP wraps. ONCE leaves cursor at
         * the boundary. Never wrap on a negative read/flash error. */
        if(n==0 && p->wrap && p->cursor!=OKP_NOTE_NONE){
            next=OKP_NOTE_NONE;
            n=p->io.seek_event(p->io.user,OKP_NOTE_NONE,0,
                               reverse?-1:1,&next,notes);
        }
        if (n<1 || n>(int)OKP_MAX_CHORD || next==OKP_NOTE_NONE)return 1;
        p->cursor=next;
    } else {
        if (!p->count || !p->io.read_event) return 1;
        if (p->cursor == OKP_NOTE_NONE)
            idx = reverse ? (int32_t)(p->count-1u) : 0;
        else
            idx = (int32_t)p->cursor + (reverse ? -1 : 1);
        if (idx < 0 || (uint32_t)idx >= p->count) {
            if (!p->wrap) return 1;
            idx = idx < 0 ? (int32_t)(p->count-1u) : 0;
        }
        n = p->io.read_event(p->io.user, (uint32_t)idx, notes);
        if (n < 1 || n > (int)OKP_MAX_CHORD) return 1;
        p->cursor = (uint32_t)idx;
    }

    for (j = 0; j < n; ++j) {
        if (notes[j].track >= 4 || notes[j].channel >= 16 || notes[j].pitch >= 128 ||
            notes[j].velocity < 1 || notes[j].velocity > 127 ||
            okp_already_in_slot(p, (unsigned)slot, &notes[j]))
            continue;
        /* Don't send an extra NoteOn when another physical key holds identical
         * notes: that can cause hanging notes on receivers with voice stacking. */
        if (!okp_other_holds(p, (unsigned)slot, &notes[j]) && p->io.emit_note)
            p->io.emit_note(p->io.user, &notes[j], 1);
        p->voices[slot][p->voice_count[slot]++] = notes[j];
    }
    return 1;
}
