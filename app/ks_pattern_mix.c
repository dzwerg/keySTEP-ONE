#include "ks_pattern_mix.h"
#include <string.h>
/* Copy the ENTIRE track-independent pattern to one performance track.
 * Notes, timings and gates belong to the source pattern. Track routing is
 * held separately by the player and is NEVER serialized into a pattern. */
int ks_mix_assign(ks_mix_state_t *mix,unsigned tr,uint16_t pattern,const ks_pattern_t *src)
{
    ks_mix_slot_t *dst;
    unsigned i,n=0;
    if(!mix||!src||tr>=KS_TRACKS||pattern>=KS_PATTERNS||
       src->length<8u||src->length>KS_MAX_STEPS||src->count>KS_MAX_EVENTS)return -1;
    dst=&mix->slot[tr];
    /* A full-lane copy is atomic under the caller's plat_lock when playback may run. */
    dst->pattern=pattern;
    dst->length=src->length;
    memset(dst->mask,0,sizeof dst->mask);
    for(i=0;i<src->count;i++){
        const ks_event_t *e=&src->event[i];
        if(e->step>=src->length||e->note>127u||e->vel==0u)return -1;
        dst->event[n++]=*e;
        dst->mask[e->step/16u]|=(uint16_t)(1u<<(e->step%16u));
    }
    dst->count=(uint16_t)n;
    return 0;
}
uint16_t ks_mix_mask(const ks_mix_state_t *mix,unsigned tr,unsigned block)
{
    if(!mix||tr>=KS_TRACKS||block>=8u||block>=(mix->slot[tr].length+15u)/16u)return 0u;
    return mix->slot[tr].mask[block];
}
