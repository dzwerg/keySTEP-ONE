/* v0.5.29 - display-only mirror of MIDI output events.
 * This file is compiled inside app.c's unity build, before ui.c.
 * Multiple notes and all 16 channels; TO_SYNTH is the single owner of
 * a played note (the external+internal calls for S tracks are separate).
 * No flash, no SYSEx, no MIDI routing changes. */
#include <stdint.h>
#include <string.h>
#ifndef PLAT_POOL
#define PLAT_POOL
#endif
static uint8_t ks_out_held[16][128] PLAT_POOL;
static uint8_t ks_out_last_note = 0u;
static void ks_live_notes_clear(void){memset(ks_out_held,0,sizeof ks_out_held);}
static void ks_live_notes_packet(uint32_t packet){
    unsigned st=(packet>>8)&255u, ch=st&15u, kind=st&0xf0u;
    unsigned note=(packet>>16)&127u, vel=(packet>>24)&127u;
    if(kind==0x90u && vel){
        if(ks_out_held[ch][note]<255u)ks_out_held[ch][note]++;
        ks_out_last_note=(uint8_t)note;
    }else if(kind==0x80u || (kind==0x90u && !vel)){
        if(ks_out_held[ch][note])ks_out_held[ch][note]--;
    }else if(kind==0xb0u && (note==120u || note==121u || note==123u)){
        memset(ks_out_held[ch],0,sizeof ks_out_held[ch]);
    }
}
/* Return distinct highlighted keys across all channels and actual voice
 * count. The last active note is the label; otherwise choose a held note. */
static void ks_live_notes_snapshot(uint32_t bits[4],int *voices,int *last){
    unsigned ch,n;int last_active=0;
    bits[0]=bits[1]=bits[2]=bits[3]=0;
    *voices=0;*last=-1;
    for(ch=0;ch<16u;ch++)for(n=0;n<128u;n++)if(ks_out_held[ch][n]){
        bits[n>>5]|=1u<<(n&31u);
        *voices+=(int)ks_out_held[ch][n];
    }
    if(ks_out_last_note<128u &&
       (bits[ks_out_last_note>>5]&(1u<<(ks_out_last_note&31u)))){
        *last=ks_out_last_note;
        last_active=1;
    }
    if(!last_active)for(n=0;n<128u;n++)if(bits[n>>5]&(1u<<(n&31u))){
        *last=(int)n;break;
    }
}
