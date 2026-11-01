/* keySTEP ONE M2 polyphonic song importer/player.
 * This unity-build module follows ks_arranger.c and precedes ui.c.
 * Uses the existing AB16 240 KiB without autoformatting or legacy M1 writes.
 */
#include "ks_m2.h"
#include <string.h>
#define M2_MAGIC 0x324D534Bu /* KSM2 */
#define M2_VERSION 1u
#define M2_FMAX 240u
#define M2_EVENT_BYTES 8u
#define M2_ACK_KIND 0x7Eu
#define M2_BAD 1u
#define M2_BUSY 2u
#define M2_DISK 3u
#define M2_PROTOCOL 4u
#define M2_FINISH 5u
/* Versioned manifest is committed LAST; an incomplete upload is not selectable. */
typedef struct {
    uint32_t magic;
    uint16_t version,id,ppq;
    uint32_t events,crc,first_chunk;
    uint16_t chunks;
    uint8_t name_len;
    char name[64];
} m2_manifest_t;
static m2_manifest_t m2_manifest PLAT_POOL;
static struct {
    uint8_t active, name_len, remainder_len, error, last_committed;
    uint16_t song_id,ppq,next_chunk;
    uint32_t count,expected_crc,crc,bytes,last_tick,first_chunk,last_chunk_crc;
    uint16_t last_chunk_len;
    uint8_t carry[8];
    char name[64];
} m2_rx PLAT_POOL;
static struct {
    uint8_t frame[M2_FMAX], collecting;
    uint16_t n;
} m2_wire PLAT_POOL;
static struct {
    uint8_t active,finished,loop,saved_tempo,tempo_locked,program_locked;
    uint16_t ppq,prepared_id;
    uint32_t event_index,clock_done,now,frac;
    uint8_t next[8],have;
    uint8_t chunk_cache[KSM2_CHUNK_SIZE];
    uint32_t cache_id;
    uint8_t cache_valid;
} m2_play PLAT_POOL;
static uint16_t m2_le16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t m2_le32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void m2_put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void m2_put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint32_t m2_crc_update(uint32_t crc,const uint8_t *b,unsigned n){
    unsigned i,k;for(i=0;i<n;i++){crc^=b[i];for(k=0;k<8;k++)crc=(crc>>1)^((crc&1u)?0xedb88320u:0u);}return crc;
}
static int m2_fs_ready(void){return ks_fs.mounted && ks_disk_status==1u;}
static int m2_manifest_read(uint16_t id,m2_manifest_t *out){
    ab16_file f;uint8_t v[96];int r;unsigned n;
    if(!id||!m2_fs_ready())return -1;
    r=ab16_lookup(&ks_fs,KSM2_MANIFEST_TYPE,id,&f);
    if(r || f.length<25u || f.length>88u)return -1;
    if(ab16_read(&ks_fs,KSM2_MANIFEST_TYPE,id,0,v,f.length))return -1;
    if(m2_le32(v)!=M2_MAGIC||m2_le16(v+4)!=M2_VERSION||m2_le16(v+6)!=id)return -1;
    out->magic=M2_MAGIC;out->version=M2_VERSION;out->id=id;
    out->ppq=m2_le16(v+8);out->events=m2_le32(v+10);out->crc=m2_le32(v+14);
    out->first_chunk=m2_le32(v+18);out->chunks=m2_le16(v+22);out->name_len=v[24];
    n=out->name_len;
    if(!n||n>63||25u+n!=f.length||!out->ppq||out->ppq>=32768u)return -1;
    memcpy(out->name,v+25,n);out->name[n]=0;
    if((uint32_t)out->chunks*KSM2_CHUNK_SIZE < out->events*M2_EVENT_BYTES)return -1;
    return 0;
}
int ksm2_exists(uint16_t id){return m2_manifest_read(id,&m2_manifest)==0;}
int ksm2_name(uint16_t id,char *name,unsigned cap){
    unsigned n;if(!cap||m2_manifest_read(id,&m2_manifest))return -1;
    n=m2_manifest.name_len;if(n>=cap)n=cap-1u;
    memcpy(name,m2_manifest.name,n);name[n]=0;return 0;
}
int ksm2_select(uint16_t current,int dir,uint16_t *found){
    uint32_t f;uint16_t at=current;unsigned tries=0;
    if(!found||!m2_fs_ready()||(dir!=1&&dir!=-1))return -1;
    do {
        if(ab16_select_existing(&ks_fs,KSM2_MANIFEST_TYPE,at,dir,&f))return -1;
        if(f>65535u)return -1;
        at=(uint16_t)f;if(ksm2_exists(at)){*found=at;return 0;}
        if(++tries>ks_fs.records+1u)return -1;
    }while(at!=current);
    return -1;
}
/* ACK frame: F0 7D 'K' 'S' '2' 01 7E <kind><seq 3x7><status> F7. */
static void m2_ack(uint8_t kind,uint16_t seq,uint8_t status){
    const uint8_t b[13]={0xf0,0x7d,0x4b,0x53,0x32,1,M2_ACK_KIND,kind,(uint8_t)(seq&127u),
      (uint8_t)((seq>>7)&127u),(uint8_t)((seq>>14)&127u),status,0xf7};
    /* Host-requested SysEx replies use the platform's dedicated USB SysEx
       sender; it is designed for full F0...F7 frames, on the main loop. */
    plat_sysex_send(b,sizeof b);
}
static int m2_check_event(const uint8_t *v){
    uint8_t st=v[4],kind=st&0xf0u;
    uint32_t tick=m2_le32(v);
    if(tick<m2_rx.last_tick)return -1;
    if(st==0xffu){if(!(v[5]||v[6]||v[7]))return -1;}
    else if(kind<0x80u||kind>=0xf0u||v[5]>=128u||v[6]>=128u||v[7]!=0u)return -1;
    m2_rx.last_tick=tick;return 0;
}
static void m2_begin(const uint8_t *p,unsigned n){
    uint8_t name_len;
    m2_rx.active=0;
    if(n<13u || n>76u)return;
    name_len=p[12];
    if(name_len==0u||name_len>63u||n!=13u+name_len)return;
    if(!m2_fs_ready()||eng_running()||ks_recording||ks_arr_any_dirty())return;
    memset(&m2_rx,0,sizeof m2_rx);
    m2_rx.song_id=m2_le16(p);m2_rx.ppq=m2_le16(p+2);
    m2_rx.count=m2_le32(p+4);m2_rx.expected_crc=m2_le32(p+8);
    if(!m2_rx.song_id||!m2_rx.ppq||m2_rx.ppq>=32768u||m2_rx.count>0x1fffffffu)return;
    m2_rx.name_len=name_len;memcpy(m2_rx.name,p+13,name_len);m2_rx.name[name_len]=0;
    m2_rx.crc=0xffffffffu;m2_rx.first_chunk=ks_fs.next_seq;
    /* A clean, monotonic AB16 sequence number is a unique staging chunk ID. */
    if(m2_rx.first_chunk==0u || m2_rx.first_chunk>0xffff0000u)return;
    m2_rx.active=1;
}
static int m2_data(const uint8_t *p,unsigned n){
    uint16_t seq,len;unsigned i;
    if(!m2_rx.active||n<4u)return -1;
    seq=m2_le16(p);len=m2_le16(p+2);
    if(!len||len>KSM2_CHUNK_SIZE||n!=len+4u)return -1;
    /* A lost ACK must not make retransmission destroy a valid import. */
    if(m2_rx.next_chunk && seq==(uint16_t)(m2_rx.next_chunk-1u)){
        uint32_t check=~m2_crc_update(0xffffffffu,p+4,len);
        return (len==m2_rx.last_chunk_len && check==m2_rx.last_chunk_crc)?0:-1;
    }
    if(seq!=m2_rx.next_chunk || m2_rx.bytes>m2_rx.count*8u ||
       len>m2_rx.count*8u-m2_rx.bytes)return -1;
    if(ks_fs.next_seq!=m2_rx.first_chunk+(uint32_t)seq)return -1;
    /* Check ordered MIDI events without buffering the whole polyphonic song. */
    for(i=0;i<len;i++){
        m2_rx.carry[m2_rx.remainder_len++]=p[4u+i];
        if(m2_rx.remainder_len==8u){
            if(m2_check_event(m2_rx.carry))return -1;
            m2_rx.remainder_len=0;
        }
    }
    m2_rx.crc=m2_crc_update(m2_rx.crc,p+4,len);
    memcpy(ks_wire,p+4,len);
    if(ab16_put(&ks_fs,KSM2_CHUNK_TYPE,m2_rx.first_chunk+seq,len,ks_fill_wire,0))return -2;
    m2_rx.last_chunk_len=len;
    m2_rx.last_chunk_crc=~m2_crc_update(0xffffffffu,p+4,len);
    m2_rx.next_chunk++;m2_rx.bytes+=len;
    return 0;
}
static int m2_commit(const uint8_t *p,unsigned n){
    uint32_t done_crc;unsigned namelen=m2_rx.name_len;ab16_file f;unsigned size;
    if(!m2_rx.active){
        return m2_rx.last_committed && n==6u &&
               m2_le16(p)==m2_rx.next_chunk &&
               m2_le32(p+2)==m2_rx.expected_crc ? 0 : -1;
    }
    if(n!=6u||m2_le16(p)!=m2_rx.next_chunk)return -1;
    done_crc=~m2_rx.crc;
    if(m2_le32(p+2)!=m2_rx.expected_crc||done_crc!=m2_rx.expected_crc||
       m2_rx.remainder_len||m2_rx.bytes!=m2_rx.count*8u)return -1;
    m2_put32(ks_wire,M2_MAGIC);m2_put16(ks_wire+4,M2_VERSION);
    m2_put16(ks_wire+6,m2_rx.song_id);m2_put16(ks_wire+8,m2_rx.ppq);
    m2_put32(ks_wire+10,m2_rx.count);m2_put32(ks_wire+14,done_crc);
    m2_put32(ks_wire+18,m2_rx.first_chunk);m2_put16(ks_wire+22,m2_rx.next_chunk);
    ks_wire[24]=(uint8_t)namelen;memcpy(ks_wire+25,m2_rx.name,namelen);size=25u+namelen;
    if(ab16_put(&ks_fs,KSM2_MANIFEST_TYPE,m2_rx.song_id,(uint16_t)size,ks_fill_wire,0))return -2;
    if(ab16_lookup(&ks_fs,KSM2_MANIFEST_TYPE,m2_rx.song_id,&f)||f.length!=size)return -2;
    m2_rx.last_committed=1u;
    m2_rx.active=0;return 0;
}
/* PIMPORT V1: chunks form exactly one native V3 pattern record in RAM.
 * No flash writes before validated COMMIT. Existing AB16 type8 id overwritten
 * deliberately; ALL OTHER id/type records remain unchanged. */
#define PIMPORT_BEGIN 5u
#define PIMPORT_DATA 6u
#define PIMPORT_COMMIT 7u
#define PIMPORT_TOKEN 0xa55au
static struct {
    uint8_t active,committed;
    uint16_t id,bytes,sequence,last_chunk_len;
    uint32_t expected_crc,crc,last_chunk_crc;
    uint8_t data[AB16_MAX_DATA];
} pimport PLAT_POOL;
static int pimport_safe(void){
    unsigned i;
    if(!m2_fs_ready()||eng_running()||ks_recording||m2_rx.active||ks_arr_any_dirty())return 0;
    for(i=0;i<KS_TRACKS;i++)if(ks_arr_work_dirty((uint8_t)i))return 0;
    return 1;
}
static int pimport_begin(const uint8_t *p,unsigned n){
    uint16_t id,bytes;
    if(n!=10u||!pimport_safe())return -1;
    id=m2_le16(p);bytes=m2_le16(p+2);
    if(id<1u||id>KS_PATTERNS||bytes<14u||bytes>AB16_MAX_DATA||
       m2_le16(p+8)!=PIMPORT_TOKEN)return -1;
    memset(&pimport,0,sizeof pimport);
    pimport.id=id;pimport.bytes=bytes;pimport.expected_crc=m2_le32(p+4);
    pimport.crc=0xffffffffu;pimport.active=1u;
    return 0;
}
static int pimport_data(const uint8_t *p,unsigned n){
    uint16_t seq,len;uint32_t position;
    if(!pimport.active||n<4u)return -1;
    seq=m2_le16(p);len=m2_le16(p+2);
    if(!len||len>KSM2_CHUNK_SIZE||len+4u!=n)return -1;
    if(pimport.sequence && seq==(uint16_t)(pimport.sequence-1u)){
        uint32_t got=~m2_crc_update(0xffffffffu,p+4,len);
        return (len==pimport.last_chunk_len && got==pimport.last_chunk_crc)?0:-1;
    }
    position=(uint32_t)pimport.sequence*KSM2_CHUNK_SIZE;
    if(seq!=pimport.sequence||position>pimport.bytes||len>pimport.bytes-position)return -1;
    if(!pimport_safe())return -1;
    memcpy(pimport.data+position,p+4,len);
    pimport.crc=m2_crc_update(pimport.crc,p+4,len);
    pimport.last_chunk_len=len;
    pimport.last_chunk_crc=~m2_crc_update(0xffffffffu,p+4,len);
    pimport.sequence++;
    return 0;
}
static int pimport_commit(const uint8_t *p,unsigned n){
    uint32_t len,crc;ab16_file f;int rc;
    if(!pimport.active)
        return pimport.committed && n==6u &&
               m2_le16(p)==pimport.sequence && m2_le32(p+2)==pimport.expected_crc?0:-1;
    if(n!=6u||m2_le16(p)!=pimport.sequence||!pimport_safe())return -1;
    len=(uint32_t)pimport.sequence*KSM2_CHUNK_SIZE;
    if(pimport.sequence && pimport.last_chunk_len<KSM2_CHUNK_SIZE)
       len-=KSM2_CHUNK_SIZE-pimport.last_chunk_len;
    crc=~pimport.crc;
    if(len!=pimport.bytes || crc!=pimport.expected_crc || m2_le32(p+2)!=crc)return -1;
    if(pimport.bytes<14u)return -1;
    memset(&ks_tmp,0,sizeof ks_tmp);
    memcpy(&ks_tmp,pimport.data,offsetof(ks_pattern_t,event));
    if(ks_tmp.count>KS_MAX_EVENTS ||
       offsetof(ks_pattern_t,event)+(uint32_t)ks_tmp.count*sizeof(ks_event_t)+4u!=len)return -1;
    memcpy(ks_tmp.event,pimport.data+offsetof(ks_pattern_t,event),
           (uint32_t)ks_tmp.count*sizeof(ks_event_t));
    memcpy(&ks_tmp.crc,pimport.data+len-4u,4u);
    if(!ks_valid(&ks_tmp))return -1;
    memcpy(ks_wire,pimport.data,len);
    rc=ab16_put(&ks_fs,AB16_KSTEP4,pimport.id,(uint16_t)len,ks_fill_wire,0);
    if(rc)return -2;
    if(ab16_lookup(&ks_fs,AB16_KSTEP4,pimport.id,&f)||f.length!=len)return -2;
    pimport.active=0u;pimport.committed=1u;
    return 0;
}

static int m2_process_frame(void){
    uint8_t unpack[204],kind;const uint8_t *a=m2_wire.frame;unsigned n=0,p=7u;
    uint16_t seq=m2_rx.next_chunk;int result=-1;uint8_t status=M2_PROTOCOL;
    if(m2_wire.n<8u||a[0]!=0xf0||a[1]!=0x7d||a[2]!=0x4b||a[3]!=0x53||
       a[4]!=0x32||a[5]!=1u||a[m2_wire.n-1u]!=0xf7)return -1;
    kind=a[6];
    while(p<m2_wire.n-1u){
        unsigned j,part=m2_wire.n-1u-p-1u;uint8_t flags=a[p++];
        if(part>7u)part=7u;
        if(!part || n+part>sizeof unpack || flags>>part)return -1;
        for(j=0;j<part;j++)unpack[n++]=(uint8_t)(a[p++]|(((flags>>j)&1u)<<7));
    }
    /* ACK the actual incoming sequence, even for an ACK-loss retry. */
    if((kind==2u || kind==3u || kind==PIMPORT_DATA || kind==PIMPORT_COMMIT) && n>=2u)
        seq=m2_le16(unpack);
    if(kind==1u){m2_begin(unpack,n);result=m2_rx.active?0:-1;seq=0u;}
    else if(kind==2u){result=m2_data(unpack,n);}
    else if(kind==3u){result=m2_commit(unpack,n);}
    else if(kind==4u){m2_rx.active=0;m2_rx.last_committed=0;result=0;}
    else if(kind==8u){result=0;seq=0u;} /* read-only PING: no flash access */
    else if(kind==PIMPORT_BEGIN){result=pimport_begin(unpack,n);seq=0u;}
    else if(kind==PIMPORT_DATA){result=pimport_data(unpack,n);}
    else if(kind==PIMPORT_COMMIT){result=pimport_commit(unpack,n);}
    if(!result)status=0;
    else status=result==-2?M2_DISK:(!m2_fs_ready()?M2_DISK:(eng_running()?M2_BUSY:M2_PROTOCOL));
    m2_ack(kind,seq,status);
    if(result && kind!=1u && kind!=PIMPORT_BEGIN &&
       kind!=PIMPORT_DATA && kind!=PIMPORT_COMMIT)m2_rx.active=0;
    if(result && (kind==PIMPORT_DATA || kind==PIMPORT_COMMIT))pimport.active=0;
    return result;
}
/* fm1-base/felucca-bsp USB SysEx hook: p excludes F0 and F7.
 * The ordinary MIDI queue does not guarantee delivery of SysEx frames.
 * Only our KS2 protocol is consumed. Remains outside the clock ISR. */
int ksm2_rx_sysex(const uint8_t *p,uint32_t n){
    if(!p || n<6u || n+2u>M2_FMAX ||
       p[0]!=0x7du || p[1]!=0x4bu || p[2]!=0x53u ||
       p[3]!=0x32u || p[4]!=1u)return 0;
    m2_wire.collecting=0u;
    m2_wire.frame[0]=0xf0u;
    memcpy(m2_wire.frame+1u,p,n);
    m2_wire.frame[n+1u]=0xf7u;
    m2_wire.n=(uint16_t)(n+2u);
    (void)m2_process_frame();
    return 1;
}
/* CIN 4,5,6,7; only the dedicated KS2 SysEx beginning is captured. */
int ksm2_rx_packet(uint32_t pkt,int port){
    unsigned cin=pkt&15u,count,i;uint8_t b[3];
    if(port!=PORT_USB||cin<4u||cin>7u)return 0;
    b[0]=(uint8_t)(pkt>>8);b[1]=(uint8_t)(pkt>>16);b[2]=(uint8_t)(pkt>>24);
    if(!m2_wire.collecting){
        if(cin!=4u||b[0]!=0xf0||b[1]!=0x7d||b[2]!=0x4b)return 0;
        m2_wire.collecting=1u;m2_wire.n=0u;
    }
    count=cin==5u?1u:cin==6u?2u:3u;
    for(i=0;i<count;i++){
        if(m2_wire.n>=M2_FMAX){m2_wire.collecting=0u;m2_rx.active=0;return 1;}
        m2_wire.frame[m2_wire.n++]=b[i];
    }
    if(cin>=5u){(void)m2_process_frame();m2_wire.collecting=0u;}
    return 1;
}
void ksm2_init(void){memset(&pimport,0,sizeof pimport);memset(&m2_rx,0,sizeof m2_rx);memset(&m2_wire,0,sizeof m2_wire);memset(&m2_play,0,sizeof m2_play);}
/* Stream event bytes via a 196-byte main-loop cache; never read flash in ISR. */
static int m2_event(uint32_t index,uint8_t e[8]){
    unsigned k;uint32_t off=index*8u;
    for(k=0;k<8u;k++){
        uint32_t ch=(off+k)/KSM2_CHUNK_SIZE,pos=(off+k)%KSM2_CHUNK_SIZE;
        unsigned bytes=(unsigned)((m2_manifest.events*8u-ch*KSM2_CHUNK_SIZE)>KSM2_CHUNK_SIZE?
            KSM2_CHUNK_SIZE:(m2_manifest.events*8u-ch*KSM2_CHUNK_SIZE));
        if(ch>=m2_manifest.chunks||!bytes)return -1;
        if(!m2_play.cache_valid || m2_play.cache_id!=ch){
            if(ab16_read(&ks_fs,KSM2_CHUNK_TYPE,m2_manifest.first_chunk+ch,0,m2_play.chunk_cache,bytes))return -1;
            m2_play.cache_id=ch;m2_play.cache_valid=1u;
        }
        e[k]=m2_play.chunk_cache[pos];
    }
    return 0;
}
/* v0.5.30: seek note-on groups in the already prepared, paused M song.
 * Current cursor is the FIRST event index at the last triggered tick, not a
 * RAM snapshot of the whole polyphonic song. One key-press = one onset tick.
 * No filesystem mutation and NO Flash read from engine/clock interrupts. */
int ksm2_onetouch_seek(uint32_t cursor,int valid,int direction,
                       uint32_t *next_index,okp_note_t notes[OKP_MAX_CHORD])
{
    uint8_t e[8],b[8];uint32_t i,first,tick,old_tick=0u;
    unsigned count=0u;
    if(!m2_play.active||!next_index||!notes||(direction!=1&&direction!=-1)||
       eng_running()||!m2_manifest.events)return -1;
    if(valid){
        if(cursor>=m2_manifest.events||m2_event(cursor,b))return -1;
        old_tick=m2_le32(b);
    }
    if(!valid)i=direction>0?0u:m2_manifest.events-1u;
    else if(direction>0){
        if(cursor+1u>=m2_manifest.events)return 0;
        i=cursor+1u;
    }else{
        if(!cursor)return 0;
        i=cursor-1u;
    }
    for(;;){
        uint8_t st;
        if(m2_event(i,e))return -1;
        tick=m2_le32(e);st=e[4];
        if((!valid||tick!=old_tick) && (st&0xf0u)==0x90u && e[6])break;
        if(direction>0){if(i+1u>=m2_manifest.events)return 0;i++;}
        else{if(i==0u)return 0;i--;}
    }
    /* Seek to the first event at that tick, including channel/controller events.
     * The cursor after reading will always point to the group's beginning. */
    first=i;
    while(first>0u){
        if(m2_event(first-1u,b))return -1;
        if(m2_le32(b)!=tick)break;
        first--;
    }
    for(i=first;i<m2_manifest.events;i++){
        if(m2_event(i,e))return -1;
        if(m2_le32(e)!=tick)break;
        if((e[4]&0xf0u)!=0x90u || !e[6])continue;
        if(count<OKP_MAX_CHORD){
            notes[count].track=0u; /* M2 owns its ORIGINAL 16 MIDI channels */
            notes[count].channel=(uint8_t)(e[4]&15u);
            notes[count].pitch=e[5];notes[count].velocity=e[6];
            count++;
        }
    }
    if(!count)return 0;
    *next_index=first;return (int)count;
}
int ksm2_prepare(uint16_t id){
    if(eng_running()||ks_recording||ks_arr_songdata_dirty()||m2_manifest_read(id,&m2_manifest))return -1;
    memset(&m2_play,0,sizeof m2_play);
    m2_play.ppq=m2_manifest.ppq;
    m2_play.prepared_id=id;
    m2_play.saved_tempo=proj.tempo;
    if(m2_manifest.events){
        if(m2_event(0,m2_play.next))return -2;
        m2_play.have=1;
    }
    ks_arr_external_player_set(1);
    m2_play.active=1;return 0;
}
void ksm2_stop(void){
    unsigned ch; if(!m2_play.active)return;
    m2_play.active=0;m2_play.finished=0;
    proj.tempo=m2_play.saved_tempo;
    ks_arr_external_player_set(0);
    for(ch=0;ch<16u;ch++){
        uint32_t cc=((0xB0u+ch)>>4)|((uint32_t)(0xB0u+ch)<<8)|(123u<<16);
        ctl_send(cc,TO_USB|TO_BLE|TO_SYNTH);
    }
}
/* Lock tempo/program for this playback only. Never edit the stored M2. */
void ksm2_override_tempo(uint8_t bpm){
    if(!m2_play.active || bpm<40u || bpm>240u)return;
    m2_play.tempo_locked=1u;
    if(proj.clock!=CLK_EXT)proj.tempo=bpm;
}
void ksm2_override_program(uint8_t prog){
    unsigned ch;
    if(!m2_play.active || prog>=128u)return;
    m2_play.program_locked=1u;
    for(ch=0;ch<16u;ch++){
        uint32_t pkt=((uint32_t)0xc0u>>4)|((uint32_t)(0xc0u+ch)<<8)|((uint32_t)prog<<16);
        ctl_send(pkt,TO_USB|TO_BLE|TO_SYNTH);
    }
}
int ksm2_active(void){return m2_play.active;}
uint16_t ksm2_playing_id(void){return m2_play.active?m2_play.prepared_id:0xffffu;}
int ksm2_finished(void){return m2_play.finished;}
static void m2_emit(const uint8_t e[8]){
    uint8_t status=e[4];uint32_t pkt;
    if(status==0xffu){
        /* SMF tempo events are microseconds per quarter. MIDI clock = 24 PPQN. */
        uint32_t us=(uint32_t)e[5]|((uint32_t)e[6]<<8)|((uint32_t)e[7]<<16);
        uint32_t bpm=us?60000000u/us:0u;
        if(!m2_play.tempo_locked && proj.clock!=CLK_EXT && bpm>=40u && bpm<=240u)proj.tempo=(uint8_t)bpm;
        return;
    }
    if(m2_play.program_locked && (status&0xf0u)==0xc0u)return;
    pkt=(status>>4)|((uint32_t)status<<8)|((uint32_t)e[5]<<16)|((uint32_t)e[6]<<24);
    ctl_send(pkt,TO_USB|TO_BLE|TO_SYNTH);
}
int ksm2_poll(void){
    uint32_t ticks;unsigned budget=256;
    if(!m2_play.active||!eng_running())return 0;
    ticks=eng.seq_tick;
    /* Convert the 24 PPQN engine clock into imported PPQ without multiplication. */
    while(m2_play.clock_done<ticks){
        m2_play.now+=(m2_play.ppq/24u);
        m2_play.frac+=(m2_play.ppq%24u);
        if(m2_play.frac>=24u){m2_play.frac-=24u;m2_play.now++;}
        m2_play.clock_done++;
        if(ticks-m2_play.clock_done>1000000u)return -1;
    }
    while(m2_play.have && m2_le32(m2_play.next)<=m2_play.now && budget--){
        m2_emit(m2_play.next);
        m2_play.event_index++;
        if(m2_play.event_index>=m2_manifest.events){m2_play.have=0;break;}
        if(m2_event(m2_play.event_index,m2_play.next))return -1;
    }
    /* Events remaining at the same tick continue on the next UI iteration. */
    if(!m2_play.have){m2_play.finished=1;return 1;}
    return 0;
}
