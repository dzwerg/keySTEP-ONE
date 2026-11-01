/* keySTEP ONE v0.5.15 – transient, independent, four-track pattern play source.
 * Does not write, reformat or change AB16 on-disk structures. */
#ifndef KS_PATTERN_MIX_H
#define KS_PATTERN_MIX_H
#include "ks_arranger.h"
typedef struct {
    uint16_t pattern, count;
    uint8_t length;
    uint8_t reserved;
    uint16_t mask[8];
    ks_event_t event[KS_MAX_EVENTS];
} ks_mix_slot_t;
typedef struct {
    ks_mix_slot_t slot[KS_TRACKS];
} ks_mix_state_t;
int ks_mix_assign(ks_mix_state_t *mix,unsigned tr,uint16_t pattern,const ks_pattern_t *src);
uint16_t ks_mix_mask(const ks_mix_state_t *mix,unsigned tr,unsigned block);
#endif
