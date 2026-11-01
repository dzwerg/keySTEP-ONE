#include "ks_music.h"
#include <string.h>
#include <stddef.h>
_Static_assert(sizeof(ksm_event_t)==8u,"song event must remain 8 bytes");
_Static_assert(offsetof(ksm_song_t,event)+KSM_MAX_EVENTS*8u<=4064u,"AB16 record overflow");
void ksm_song_clear(ksm_song_t*s){memset(s,0,sizeof *s);s->magic=KSM_SONG_MAGIC;s->version=KSM_VERSION;s->length=16u;}
void ksm_album_clear(ksm_album_t*a){memset(a,0,sizeof *a);a->magic=KSM_ALBUM_MAGIC;a->version=KSM_VERSION;}
void ksm_runtime_clear(ksm_runtime_t*r){memset(r,0,sizeof *r);}
unsigned ksm_song_wire_size(const ksm_song_t*s){return (unsigned)offsetof(ksm_song_t,event)+(unsigned)s->count*sizeof(ksm_event_t);}
unsigned ksm_album_wire_size(const ksm_album_t*a){return (unsigned)offsetof(ksm_album_t,song)+(unsigned)a->count*sizeof(uint16_t);}
int ksm_song_valid(const ksm_song_t*s){
    unsigned i;
    if(!s||s->magic!=KSM_SONG_MAGIC||s->version!=KSM_VERSION||s->count>KSM_MAX_EVENTS||s->length<1u||s->length>KSM_MAX_STEPS||(s->flags&~15u))return 0;
    for(i=0;i<s->count;i++){const ksm_event_t*e=&s->event[i];
        if(i && e->at<s->event[i-1u].at)return 0;
        if(e->at>=s->length*KSM_TICKS_STEP||e->track>=4u||e->note>127u||e->vel==0u||e->vel>127u||e->gate==0u||e->flags!=0u)return 0;
    }return 1;
}
int ksm_album_valid(const ksm_album_t*a){unsigned i;if(!a||a->magic!=KSM_ALBUM_MAGIC||a->version!=KSM_VERSION||a->count>KSM_MAX_ALBUM)return 0;
    for(i=0;i<a->count;i++)if(a->song[i]>=64u)return 0;
    return 1;
}
/* Never generate a fake note-off; a repeated note-on closes its predecessor. */
static void close_pending(ksm_song_t*s,ksm_runtime_t*r,uint32_t tick,uint8_t tr,uint8_t note){
    uint16_t n=r->pending[tr][note];
    if(n&&n<=s->count){ksm_event_t*e=&s->event[n-1u];
        uint32_t dur=tick>e->at?tick-e->at:1u;
        if(dur>65535u)dur=65535u;
        e->gate=(uint16_t)dur;
    }r->pending[tr][note]=0;
}
int ksm_record(ksm_song_t*s,ksm_runtime_t*r,uint32_t tick,uint8_t tr,uint8_t status,uint8_t note,uint8_t vel){
    ksm_event_t*e;uint32_t limit=KSM_MAX_STEPS*KSM_TICKS_STEP;
    if(!s||!r||tr>=4u||note>127u||tick>=limit)return -1;
    status&=0xf0u;
    if(status==0x80u||(status==0x90u&&!vel)){
        close_pending(s,r,tick,tr,note);return 0;
    }
    if(status!=0x90u)return -1;
    close_pending(s,r,tick,tr,note);
    if(s->count==KSM_MAX_EVENTS)return -2;
    e=&s->event[s->count++];e->at=(uint16_t)tick;e->gate=1u;e->track=tr;e->note=note;e->vel=vel;e->flags=0;
    r->pending[tr][note]=s->count;
    if((uint32_t)s->length*KSM_TICKS_STEP<=tick)s->length=(uint16_t)(tick/KSM_TICKS_STEP+1u);
    return 0;
}
void ksm_record_finish(ksm_song_t*s,ksm_runtime_t*r,uint32_t tick){
    unsigned tr,n,i;uint32_t end=0;
    for(tr=0;tr<4u;tr++)for(n=0;n<128u;n++)if(r->pending[tr][n])close_pending(s,r,tick,(uint8_t)tr,(uint8_t)n);
    for(i=0;i<s->count;i++){const ksm_event_t*e=&s->event[i];uint32_t eend=(uint32_t)e->at+e->gate;if(eend>end)end=eend;}
    /* Trim to the last note, round up to a 16-step block. */
    end=(end+KSM_TICKS_STEP-1u)/KSM_TICKS_STEP;
    end=(end+15u)&~15u;
    if(end<16u)end=16u;
    if(end>KSM_MAX_STEPS)end=KSM_MAX_STEPS;
    s->length=(uint16_t)end;r->recording=0;
}
void ksm_prepare_play(ksm_song_t*s,ksm_runtime_t*r){
    /* Persisted/recorded events are monotonic. O(1) ISR restart. */
    (void)s;r->play_cursor=0;
}
unsigned ksm_play_tick(const ksm_song_t*s,ksm_runtime_t*r,uint32_t tick,ksm_emit_t emit,void*user){
    unsigned n=0;
    while(r->play_cursor<s->count&&s->event[r->play_cursor].at<tick)r->play_cursor++;
    while(r->play_cursor<s->count&&s->event[r->play_cursor].at==tick){const ksm_event_t*e=&s->event[r->play_cursor++];
        if(emit)emit(user,e->track,e->note,e->vel,e->gate);
        n++;
    }
    return n;
}
