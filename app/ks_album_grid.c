/* v0.5.32: separate, versioned, non-destructive album grid.
 * C unity build: ks_arranger.c and ks_m2.c are included before this module. */
#include "ks_album_grid.h"
#include <string.h>
#define KSAL_MAGIC 0x324C414Bu /* KAL2 */
#define KSAL_VERSION 1u
#define KSAL_HDR 8u
ksal_t ksal PLAT_POOL;
static uint8_t ksal_programs[16]; /* last actually emitted MIDI program per channel */
void ksal_program_observe(uint32_t pkt){
    uint8_t st=(uint8_t)((pkt>>8)&255u);
    if((st&0xf0u)==0xc0u)ksal_programs[st&15u]=(uint8_t)((pkt>>16)&127u);
}
uint8_t ksal_program_current(uint8_t ch){return ch<16u?ksal_programs[ch]:0u;}
static uint32_t ksal_crc(const uint8_t *b,unsigned n){
    uint32_t c=0xffffffffu; unsigned i,j;
    for(i=0;i<n;i++){c^=b[i];for(j=0;j<8;j++)c=(c>>1)^((c&1u)?0xedb88320u:0u);}
    return c^0xffffffffu;
}
static void ksal_u16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static uint16_t ksal_r16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static void ksal_u32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint32_t ksal_r32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int ksal_valid_entry(uint8_t kind,uint16_t val){
    return (kind==KSAL_S && val<64u)||(kind==KSAL_M && val>0u)||
           (kind==KSAL_BPM && val>=40u && val<=240u)||
           (kind==KSAL_INST && val<128u);
}
int ksal_exists(uint16_t id){ab16_file f;return id<32u&&ks_fs.mounted&&
    ab16_lookup(&ks_fs,KSAL_AB16_TYPE,(uint32_t)id+1u,&f)==0;}
int ksal_next(uint16_t from,int dir,uint16_t *out){
    unsigned i;int n=(int)from;
    if(!out||(dir!=1&&dir!=-1))return -1;
    for(i=0;i<32u;i++){
        n=(n+dir+32)%32;
        if(ksal_exists((uint16_t)n)){*out=(uint16_t)n;return 0;}
    }
    return -1;
}
int ksal_load(uint16_t id){
    ab16_file f;uint16_t n;uint32_t expect;unsigned i;
    if(id>=32u||eng_running()||ks_recording||ksal.dirty||!ks_fs.mounted)return -1;
    if(ab16_lookup(&ks_fs,KSAL_AB16_TYPE,(uint32_t)id+1u,&f)!=0){
        memset(&ksal,0,sizeof ksal);ksal.album_id=id;return 0;
    }
    if(f.length<KSAL_HDR+4u||f.length>KSAL_HDR+4u*KSAL_MAX_ENTRIES+4u)return -2;
    if(ab16_read(&ks_fs,KSAL_AB16_TYPE,(uint32_t)id+1u,0u,ks_wire,f.length)!=0)return -2;
    if(ksal_r32(ks_wire)!=KSAL_MAGIC||ksal_r16(ks_wire+4)!=KSAL_VERSION)return -2;
    n=ksal_r16(ks_wire+6);
    if(n>KSAL_MAX_ENTRIES||f.length!=KSAL_HDR+4u*n+4u)return -2;
    expect=ksal_r32(ks_wire+KSAL_HDR+4u*n);
    if(ksal_crc(ks_wire,KSAL_HDR+4u*n)!=expect)return -2;
    for(i=0;i<n;i++)if(!ksal_valid_entry(ks_wire[KSAL_HDR+i*4u],ksal_r16(ks_wire+KSAL_HDR+i*4u+2u)))return -2;
    memset(&ksal,0,sizeof ksal);ksal.album_id=id;ksal.count=n;
    for(i=0;i<n;i++){
        const uint8_t *p=ks_wire+KSAL_HDR+i*4u;
        ksal.e[i].kind=p[0];ksal.e[i].value=ksal_r16(p+2u);
    }
    return 0;
}
int ksal_save(void){
    unsigned i,n;ab16_file f;
    if(!ks_fs.mounted||eng_running()||ks_recording||ksal.album_id>=32u)return -1;
    n=KSAL_HDR+4u*ksal.count+4u;
    if(n>AB16_MAX_DATA)return -2;
    memset(ks_wire,0,n);
    ksal_u32(ks_wire,KSAL_MAGIC);ksal_u16(ks_wire+4,KSAL_VERSION);ksal_u16(ks_wire+6,ksal.count);
    for(i=0;i<ksal.count;i++){
        uint8_t *p=ks_wire+KSAL_HDR+i*4u;
        p[0]=ksal.e[i].kind;ksal_u16(p+2,ksal.e[i].value);
    }
    ksal_u32(ks_wire+n-4u,ksal_crc(ks_wire,n-4u));
    if(ab16_put(&ks_fs,KSAL_AB16_TYPE,(uint32_t)ksal.album_id+1u,(uint16_t)n,ks_fill_wire,0))return -3;
    if(ab16_lookup(&ks_fs,KSAL_AB16_TYPE,(uint32_t)ksal.album_id+1u,&f)||f.length!=n)return -3;
    ksal.dirty=0u;return 0;
}
int ksal_insert(uint16_t pos,uint8_t kind,uint16_t value){
    unsigned i;
    if(eng_running()||ks_recording||pos>ksal.count||ksal.count>=KSAL_MAX_ENTRIES||!ksal_valid_entry(kind,value))return -1;
    if(kind==KSAL_S && !ks_arr_song_exists(value))return -2;
    if(kind==KSAL_M && !ksm2_exists(value))return -2;
    for(i=ksal.count;i>pos;i--)ksal.e[i]=ksal.e[i-1u];
    ksal.e[pos].kind=kind;ksal.e[pos].unused=0;ksal.e[pos].value=value;
    ksal.count++;ksal.dirty=1u;return 0;
}
int ksal_delete(uint16_t pos){
    unsigned i;if(eng_running()||ks_recording||pos>=ksal.count)return -1;
    for(i=pos;i+1u<ksal.count;i++)ksal.e[i]=ksal.e[i+1u];
    ksal.count--;ksal.dirty=1u;return 0;
}
int ksal_move(uint16_t from,uint16_t to){
    ksal_entry_t val;unsigned i;
    if(eng_running()||ks_recording||from>=ksal.count||to>=ksal.count)return -1;
    if(from==to)return 0;
    val=ksal.e[from];
    if(from<to)for(i=from;i<to;i++)ksal.e[i]=ksal.e[i+1u];
    else for(i=from;i>to;i--)ksal.e[i]=ksal.e[i-1u];
    ksal.e[to]=val;ksal.dirty=1u;return 0;
}
