/* Host-verified LOOM-8 inspired editing logic, without its synth or flash format. */
#include "ks_step_editor.h"
#include <string.h>

static int clampi(int n, int lo, int hi) { return n<lo?lo:n>hi?hi:n; }
static int position(kse_editor_t *e, unsigned idx, unsigned *p) {
    unsigned length;
    if (!e->opened || idx >= 16 || !e->io.read_length ||
        e->io.read_length(e->user,e->track,e->pattern[e->track],&length) ||
        length < KSE_MIN_STEPS || length > KSE_MAX_STEPS) return -1;
    *p = e->bank*16u + idx;
    return *p < length ? 0 : -1;
}
static int read_step(kse_editor_t *e,unsigned pos,kse_step_t *s) {
    if(!e->io.read_step || e->io.read_step(e->user,e->track,e->pattern[e->track],pos,s))return -1;
    return s->count <= KSE_MAX_CHORD ? 0 : -1;
}
static int write_step(kse_editor_t *e,unsigned pos,const kse_step_t *s) {
    if(!e->io.write_step)return -1;
    kse_step_t prev;
    if(read_step(e,pos,&prev))return -1;
    if(e->io.write_step(e->user,e->track,e->pattern[e->track],pos,s))return -1;
    e->undo_track=e->track;
    e->undo_pattern=e->pattern[e->track];
    e->undo_step=(uint8_t)pos;
    e->undo_before=prev;
    e->undo_valid=1;
    e->dirty=1;
    return 0;
}
void kse_init(kse_editor_t *e,kse_backend_t backend,void *user) {
    memset(e,0,sizeof(*e));e->io=backend;e->user=user;e->held_step=-1;
    e->default_gate=100;e->default_chance=100;
    e->chord_count=1;e->chord[0]=60;e->chord_vel[0]=KSE_FIXED_VELOCITY;
}
void kse_open(kse_editor_t *e,int on) {e->opened=(uint8_t)!!on;e->held_step=-1;}
/* KSTEP_LOOM_EDITOR_FIX_V052: absolute track is authoritative (ALGO/footer). */
void kse_select_track(kse_editor_t *e,unsigned t){
    if(t>=KSE_TRACKS)return;
    if(t!=e->track){e->track=(uint8_t)t;e->bank=0;e->held_step=-1;}
}
void kse_algo(kse_editor_t *e,int delta){
    kse_select_track(e,(unsigned)clampi((int)e->track+delta,0,(int)KSE_TRACKS-1));
}
int kse_pattern(kse_editor_t *e,unsigned pat) {
    if(pat>=KSE_PATTERNS)return -1;
    e->pattern[e->track]=(uint16_t)pat;e->bank=0;e->held_step=-1;
    return 0;
}
int kse_page(kse_editor_t *e,int delta) {
    unsigned len;
    if(!e->opened||!e->io.read_length||e->io.read_length(e->user,e->track,e->pattern[e->track],&len)||
       len < KSE_MIN_STEPS || len > KSE_MAX_STEPS)return -1;
    e->bank=(uint8_t)clampi((int)e->bank+delta,0,((int)len-1)/16);
    e->held_step=-1;
    return 0;
}
int kse_arm_notes(kse_editor_t *e,const uint8_t *notes,const uint8_t *velocity,unsigned count) {
    unsigned i;
    if(!notes||!count||count>KSE_MAX_CHORD)return -1;
    for(i=0;i<count;i++)if(notes[i]>127)return -1;
    for(i=0;i<count;i++){
        e->chord[i]=notes[i];
        e->chord_vel[i]=velocity ? (uint8_t)clampi(velocity[i],1,127) : KSE_FIXED_VELOCITY;
    }
    e->chord_count=(uint8_t)count;return 0;
}
int kse_toggle_white(kse_editor_t *e,unsigned i) {
    kse_step_t s;unsigned pos,j;
    if(position(e,i,&pos)||read_step(e,pos,&s))return -1;
    if(s.count)memset(&s,0,sizeof(s));
    else {
        memset(&s,0,sizeof(s));s.count=e->chord_count;
        for(j=0;j<s.count;j++){s.note[j]=e->chord[j];s.velocity[j]=e->chord_vel[j];}
        s.gate=e->default_gate;s.chance=e->default_chance;s.span=1;
    }
    return write_step(e,pos,&s);
}
int kse_hold_white(kse_editor_t *e,int i) {
    unsigned pos;
    if(i==-1){e->held_step=-1;return 0;}
    if(i<0||position(e,(unsigned)i,&pos))return -1;
    e->held_step=(int16_t)pos;return 0;
}
/* KSTEP_LOOM_EDITOR_FIX_V052: LOOM-like white+white length gesture.
 * The first held step is the note's start; a later white key is the inclusive
 * endpoint. All 16ths up to pattern length are supported. The second key
 * never toggles a step on release; editing is valid during PLAY. */
int kse_extend_held_to(kse_editor_t *e,unsigned white_index){
    unsigned end; kse_step_t s; unsigned i;
    if(e->held_step<0||position(e,white_index,&end)||end<=(unsigned)e->held_step||
       end>=(unsigned)KSE_MAX_STEPS||read_step(e,(unsigned)e->held_step,&s))return -1;
    if(!s.count){
        if(!e->chord_count||e->chord_count>KSE_MAX_CHORD)return -1;
        memset(&s,0,sizeof s);s.count=e->chord_count;
        for(i=0;i<s.count;++i){s.note[i]=e->chord[i];s.velocity[i]=e->chord_vel[i];}
        s.gate=e->default_gate;s.chance=e->default_chance;
    }
    s.span=(uint8_t)(end-(unsigned)e->held_step+1u);
    if(!s.gate)s.gate=100;
    return write_step(e,(unsigned)e->held_step,&s);
}
/* KSTEP_LIVE_EDITOR_V051: a held step can be edited without leaving PLAY.
 * Empty held steps acquire the current note/chord as soon as a knob edits them. */
static int edit_step(kse_editor_t *e, unsigned mode,unsigned v,unsigned idx) {
    kse_step_t s;
    unsigned j;
    if(e->held_step<0||read_step(e,(unsigned)e->held_step,&s))return -1;
    if(!s.count){
        if(!e->chord_count||e->chord_count>KSE_MAX_CHORD)return -1;
        memset(&s,0,sizeof s);
        s.count=e->chord_count;
        for(j=0;j<s.count;++j){s.note[j]=e->chord[j];s.velocity[j]=e->chord_vel[j];}
        s.gate=e->default_gate;s.chance=e->default_chance;s.span=1;
    }
    if(mode==0){if(v<1||v>100)return -1;s.gate=(uint8_t)v;}
    else if(mode==1){if(v>100)return -1;s.chance=(uint8_t)v;}
    else if(mode==2){if(idx>=s.count||v>127)return -1;s.note[idx]=(uint8_t)v;}
    else {if(idx>=s.count||v<1||v>127)return -1;s.velocity[idx]=(uint8_t)v;}
    return write_step(e,(unsigned)e->held_step,&s);
}
int kse_edit_held_gate(kse_editor_t *e,unsigned g){return edit_step(e,0,g,0);}
int kse_edit_held_chance(kse_editor_t *e,unsigned c){return edit_step(e,1,c,0);}
int kse_edit_held_note(kse_editor_t *e,unsigned idx,unsigned note){return edit_step(e,2,note,idx);}
int kse_edit_held_velocity(kse_editor_t *e,unsigned idx,unsigned velocity){return edit_step(e,3,velocity,idx);}
/* Step held + SEQ keyboard notes: replace the chord, retain gate/chance. */
int kse_replace_held_chord(kse_editor_t *e,const uint8_t *notes,unsigned count){
    kse_step_t s;unsigned i;
    if(e->held_step<0||!notes||!count||count>KSE_MAX_CHORD||
       read_step(e,(unsigned)e->held_step,&s))return -1;
    for(i=0;i<count;++i)if(notes[i]>127u)return -1;
    if(!s.count){s.gate=e->default_gate;s.chance=e->default_chance;s.span=1;}
    s.count=(uint8_t)count;
    for(i=0;i<count;++i){s.note[i]=notes[i];s.velocity[i]=KSE_FIXED_VELOCITY;}
    return write_step(e,(unsigned)e->held_step,&s);
}
int kse_copy_held(kse_editor_t *e){
    if(e->held_step<0||read_step(e,(unsigned)e->held_step,&e->clipboard))return -1;
    e->clipboard_valid=1;return 0;
}
int kse_paste_held(kse_editor_t *e){
    return e->clipboard_valid&&e->held_step>=0?write_step(e,(unsigned)e->held_step,&e->clipboard):-1;
}
int kse_undo(kse_editor_t *e){
    if(!e->undo_valid||!e->io.write_step)return -1;
    if(e->io.write_step(e->user,e->undo_track,e->undo_pattern,e->undo_step,&e->undo_before))return -1;
    e->undo_valid=0;e->dirty=1;return 0;
}
int kse_set_length(kse_editor_t *e,unsigned len){
    if(len<KSE_MIN_STEPS||len>KSE_MAX_STEPS||!e->io.write_length)return -1;
    if(e->io.write_length(e->user,e->track,e->pattern[e->track],len))return -1;
    if(e->bank*16u>=len)e->bank=(uint8_t)((len-1)/16);
    e->held_step=-1;e->dirty=1;return 0;
}
