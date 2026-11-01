#ifndef KS_ALBUM_GRID_H
#define KS_ALBUM_GRID_H
#include <stdint.h>
#define KSAL_AB16_TYPE 15u /* distinct from legacy album 10, M2 13/14 */
#define KSAL_MAX_ENTRIES 256u /* explicitly requested 256-item arranger capacity */
enum {KSAL_S=1,KSAL_M=2,KSAL_BPM=3,KSAL_INST=4};
typedef struct {uint8_t kind,unused;uint16_t value;} ksal_entry_t;
typedef struct {uint16_t count,album_id;uint8_t dirty;ksal_entry_t e[KSAL_MAX_ENTRIES];} ksal_t;
extern ksal_t ksal;
int ksal_load(uint16_t id);
int ksal_save(void);
int ksal_exists(uint16_t id);
int ksal_next(uint16_t from,int dir,uint16_t *out);
int ksal_insert(uint16_t position,uint8_t kind,uint16_t value);
int ksal_delete(uint16_t position);
int ksal_move(uint16_t from,uint16_t to);
int ksal_valid_entry(uint8_t kind,uint16_t value);
void ksal_program_observe(uint32_t pkt);
uint8_t ksal_program_current(uint8_t channel);
#endif
