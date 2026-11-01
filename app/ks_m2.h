/* keySTEP ONE M2: polyphonic, transactional SysEx. Not analogBEAST M1. */
#ifndef KS_M2_H
#define KS_M2_H
#include <stdint.h>
#include "onekey_play.h"
#define KSM2_MANIFEST_TYPE 13u
#define KSM2_CHUNK_TYPE 14u
#define KSM2_CHUNK_SIZE 196u
void ksm2_init(void);
int ksm2_rx_packet(uint32_t pkt,int port); /* USB CIN4/5/6/7, consumes only own SysEx */
int ksm2_rx_sysex(const uint8_t *data, uint32_t len);
int ksm2_exists(uint16_t id);
int ksm2_select(uint16_t current,int direction,uint16_t *result);
int ksm2_name(uint16_t id,char *name,unsigned capacity);
int ksm2_prepare(uint16_t id);
void ksm2_stop(void);
void ksm2_override_tempo(uint8_t bpm);
void ksm2_override_program(uint8_t program);
int ksm2_active(void);
int ksm2_poll(void); /* main loop only: no flash in ISR */
int ksm2_finished(void);
/* ONE TOUCH paused M2 playback; original channel retained, read-only seek. */
uint16_t ksm2_playing_id(void);
int ksm2_onetouch_seek(uint32_t cursor,int valid,int direction,
                       uint32_t *next_index,okp_note_t notes[OKP_MAX_CHORD]);
#endif
