/* keySTEP ONE SONG GRID v0.5.23: pattern references, NOT pattern data. */
#ifndef KS_SONG_GRID_CORE_H
#define KS_SONG_GRID_CORE_H
#include <stdint.h>
#include "ab16_fs.h"
#define KSG_TRACKS 4u
#define KSG_EMPTY 0xffffu
#define KSG_SILENT 512u /* on-disk sentinel, distinct from pattern IDs 0..511 */
#define KSG_SILENT_STEPS 16u
/* Capacity follows the AB16 single-record payload, not an arbitrary event limit.
 * A future multirecord format can extend it without changing the editor UI. */
#define KSG_MAX_REFS ((AB16_MAX_DATA-18u)/2u)
typedef struct {
    uint16_t count[KSG_TRACKS];
    uint16_t pat[KSG_MAX_REFS];
} ksg_grid_t;
void ksg_clear(ksg_grid_t *g);
unsigned ksg_total(const ksg_grid_t *g);
uint16_t ksg_get(const ksg_grid_t *g,unsigned tr,unsigned col);
int ksg_put(ksg_grid_t *g,unsigned tr,unsigned col,uint16_t pat);
int ksg_del(ksg_grid_t *g,unsigned tr,unsigned col);
int ksg_move(ksg_grid_t *g,unsigned tr,unsigned from,unsigned to);
int ksg_valid(const ksg_grid_t *g);
#endif
