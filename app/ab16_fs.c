/* SPDX-License-Identifier: GPL-3.0-only
 * MUSIC DISK v16 - no heap, no table in RAM, sector-local streaming records.
 * This module does no automatic formatting and never touches a device except via ab16_io.
 */
#include "ab16_fs.h"
#define AB16_MAGIC 0x36314241u   /* AB16 little-endian */
#define AB16_COMMIT 0xB16B00B5u
#define AB16_DEL 1u
#define AB16_GC_SYS_ID 0u
#define AB16_SYS_BYTES 8u
static const uint8_t ab16_super[8]={0x41,0x42,0x31,0x36,0x4d,0x44,0x01,0x00};
static uint16_t ab16_ld16(const uint8_t*b){return (uint16_t)(b[0]|(uint16_t)b[1]<<8);}
static uint32_t ab16_ld32(const uint8_t*b){return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
static void ab16_st16(uint8_t*b,uint16_t v){b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);}
static void ab16_st32(uint8_t*b,uint32_t v){b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);b[2]=(uint8_t)(v>>16);b[3]=(uint8_t)(v>>24);}
static uint32_t ab16_crc_add(uint32_t c,const uint8_t*b,uint32_t n){
    uint32_t i; for(i=0;i<n;i++){unsigned k;c^=b[i];for(k=0;k<8u;k++)c=(c>>1)^(0xEDB88320u & (0u-(c&1u)));}return c;
}
static uint32_t ab16_crc32(const uint8_t*b,uint32_t n){return ~ab16_crc_add(0xFFFFFFFFu,b,n);}
static uint32_t ab16_total(uint32_t n){return (AB16_HDR+n+3u)&~3u;}
static int ab16_rd(ab16_fs*f,uint32_t off,void *buf,uint32_t n){
    if(off>AB16_SIZE||n>AB16_SIZE-off||!f->io.read)return AB16_EINVAL;
    return f->io.read(f->io.ctx,off,buf,n)?AB16_EIO:0;
}
static int ab16_pg(ab16_fs*f,uint32_t off,const void *buf,uint32_t n){
    if(off>AB16_SIZE||n>AB16_SIZE-off||!f->io.prog)return AB16_EINVAL;
    return f->io.prog(f->io.ctx,off,buf,n)?AB16_EIO:0;
}
static int ab16_er(ab16_fs*f,uint32_t off){
    if(off%AB16_SECTOR||off>AB16_SIZE-AB16_SECTOR||!f->io.erase)return AB16_EINVAL;
    return f->io.erase(f->io.ctx,off)?AB16_EIO:0;
}
static int ab16_allff(ab16_fs*f,uint32_t off,uint32_t n){
    uint8_t b[128];uint32_t i,k;int rc;
    while(n){k=n>sizeof b?(uint32_t)sizeof b:n;rc=ab16_rd(f,off,b,k);if(rc)return rc;
        for(i=0;i<k;i++)if(b[i]!=0xFFu)return 0;
        off+=k;n-=k;
    }return 1;
}
typedef struct {uint32_t seq,id,crc;uint16_t type,length,flags;} ab16_ahdr;
static int ab16_decode(const uint8_t raw[AB16_HDR],ab16_ahdr *h){
    if(ab16_ld32(raw)!=AB16_MAGIC||ab16_ld32(raw+28)!=AB16_COMMIT)return 0;
    if(ab16_ld32(raw+24)!=ab16_crc32(raw,24u))return 0;
    h->seq=ab16_ld32(raw+4);h->type=ab16_ld16(raw+8);h->flags=ab16_ld16(raw+10);
    h->id=ab16_ld32(raw+12);h->length=ab16_ld16(raw+16);h->crc=ab16_ld32(raw+20);
    if(ab16_ld16(raw+18)!=0u||!(h->seq)||h->length>AB16_MAX_DATA||
       (h->flags&~AB16_DEL)||((h->flags&AB16_DEL)&&h->length))return 0;
    return 1;
}
/* Iterator: unknown or torn sector tail is NOT appendable. Never assume FF
 * from the first four bytes without checking the rest. */
typedef int (*ab16_record_fn)(ab16_fs*,uint32_t,const ab16_ahdr*,void*);
static int ab16_walk(ab16_fs*f,ab16_record_fn fn,void*arg,uint32_t *tails,uint8_t *state){
    uint32_t si,off,base,stop;uint8_t b[AB16_HDR];ab16_ahdr h;int rc;
    for(si=0;si<AB16_NSECTORS;si++){
        base=si*AB16_SECTOR;stop=base+AB16_SECTOR;off=base;
        if(state)state[si]=0u; /* 0 blank, 1 log, 2 blocked */
        for(;;){
            if(stop-off<AB16_HDR){
                rc=ab16_allff(f,off,stop-off);if(rc<0)return rc;
                if(rc==0&&state)state[si]=2u;
                break;
            }
            rc=ab16_rd(f,off,b,sizeof b);if(rc)return rc;
            if(ab16_ld32(b)==0xFFFFFFFFu){
                rc=ab16_allff(f,off,stop-off);if(rc<0)return rc;
                if(!rc){if(state)state[si]=2u;off=stop;}
                break;
            }
            if(!ab16_decode(b,&h)||ab16_total(h.length)>stop-off){
                if(state)state[si]=2u;
                off=stop;break;
            }
            if(state&&state[si]==0u)state[si]=1u;
            if(fn){rc=fn(f,off,&h,arg);if(rc)return rc;}
            off+=ab16_total(h.length);
        }
        if(tails)tails[si]=off;
    }
    return 0;
}
typedef struct {uint16_t kind;uint32_t id;ab16_file f;uint8_t got;} ab16_lookarg;
static int ab16_lookfn(ab16_fs*f,uint32_t off,const ab16_ahdr*h,void*ctx){
    ab16_lookarg*a=(ab16_lookarg*)ctx;(void)f;
    if(h->type==a->kind&&h->id==a->id&&(!a->got||h->seq>a->f.seq)){
        a->got=1u;a->f.offset=off;a->f.seq=h->seq;a->f.type=h->type;a->f.id=h->id;
        a->f.deleted=(uint8_t)(h->flags&AB16_DEL);a->f.length=h->length;
    }return 0;
}
static int ab16_rawlookup(ab16_fs*f,uint16_t kind,uint32_t id,ab16_file*out){
    ab16_lookarg a;int rc; a.kind=kind;a.id=id;a.got=0u;
    rc=ab16_walk(f,ab16_lookfn,&a,0,0);if(rc)return rc;
    if(!a.got)return AB16_ENOTFOUND;
    *out=a.f;return 0;
}
static int ab16_rawread(ab16_fs*f,const ab16_file*file,uint32_t off,void*out,uint32_t n){
    uint8_t b[128],head[AB16_HDR];uint32_t crc=0xFFFFFFFFu,pos=0,k;int rc;
    if(off>file->length||n>file->length-off)return AB16_EINVAL;
    rc=ab16_rd(f,file->offset,head,sizeof head);if(rc)return rc;
    if(ab16_ld32(head)!=AB16_MAGIC||ab16_ld32(head+28)!=AB16_COMMIT)return AB16_ECRC;
    while(pos<file->length){
        k=file->length-pos;if(k>sizeof b)k=sizeof b;
        rc=ab16_rd(f,file->offset+AB16_HDR+pos,b,k);if(rc)return rc;
        crc=ab16_crc_add(crc,b,k);
        if(out && off<pos+k && pos<off+n){
            uint32_t lo=off>pos?off:pos,hi=(off+n)<(pos+k)?(off+n):(pos+k),i;
            for(i=lo;i<hi;i++)((uint8_t*)out)[i-off]=b[i-pos];
        }
        pos+=k;
    }
    return (~crc)==ab16_ld32(head+20)?0:AB16_ECRC;
}
int ab16_lookup(ab16_fs*f,uint16_t type,uint32_t id,ab16_file*out){
    int rc;if(!f||!f->mounted||!out||!type)return AB16_ESTATE;
    rc=ab16_rawlookup(f,type,id,out);if(rc)return rc;
    if(out->deleted)return AB16_ENOTFOUND;
    return ab16_rawread(f,out,0,0,0);
}
int ab16_read(ab16_fs*f,uint16_t type,uint32_t id,uint32_t off,void*dst,uint32_t n){
    ab16_file e;int rc;if(n&&!dst)return AB16_EINVAL;
    rc=ab16_lookup(f,type,id,&e);if(rc)return rc;
    return ab16_rawread(f,&e,off,dst,n);
}
/* A menu selection must follow actual committed files, not presumed ID ranges.
 * No file-ID table is allocated in RAM. Each foreground request finds the
 * closest candidate in the appropriate direction, then validates its latest
 * version (including tombstones, payload CRC) before exposing it to PLAY.
 * Repeating this on every encoder detent is safe, but not an audio-ISR task. */
typedef struct {
    uint16_t kind;
    uint32_t current,last,best;
    uint8_t phase,has_last,got;
    int direction;
} ab16_pickarg;
static int ab16_pickfn(ab16_fs *f,uint32_t off,const ab16_ahdr *h,void *ctx){
    ab16_pickarg *a=(ab16_pickarg*)ctx;
    uint32_t id=h->id;
    (void)f;(void)off;
    if(h->type!=a->kind||id==0u)return 0;
    if(a->direction>0){
        if(a->phase==0u ? id<=a->current : id>a->current)return 0;
        if(a->has_last&&id<=a->last)return 0;
        if(!a->got||id<a->best){a->got=1u;a->best=id;}
    }else{
        if(a->phase==0u ? id>=a->current : id<a->current)return 0;
        if(a->has_last&&id>=a->last)return 0;
        if(!a->got||id>a->best){a->got=1u;a->best=id;}
    }
    return 0;
}
int ab16_select_existing(ab16_fs *f,uint16_t type,uint32_t current,
                         int direction,uint32_t *selected){
    ab16_pickarg a;ab16_file file;int rc;unsigned phase;
    if(!f||!f->mounted||!selected||type==AB16_SYS||
       (direction!=1&&direction!=-1))return AB16_EINVAL;
    a.kind=type;a.current=current;a.direction=direction;
    for(phase=0u;phase<2u;phase++){
        a.phase=(uint8_t)phase;a.has_last=0u;
        for(;;){
            a.got=0u;
            rc=ab16_walk(f,ab16_pickfn,&a,0,0);
            if(rc)return rc;
            if(!a.got)break;
            rc=ab16_lookup(f,type,a.best,&file);
            if(rc==AB16_OK){*selected=a.best;return AB16_OK;}
            if(rc!=AB16_ENOTFOUND&&rc!=AB16_ECRC)return rc;
            a.has_last=1u;a.last=a.best;
        }
    }
    return AB16_ENOTFOUND;
}
typedef struct {uint32_t seq,count;} ab16_mountarg;
static int ab16_mountfn(ab16_fs*f,uint32_t off,const ab16_ahdr*h,void*ctx){
    ab16_mountarg*m=(ab16_mountarg*)ctx;(void)f;(void)off;
    m->count++;if(h->seq>m->seq)m->seq=h->seq;
    return 0;
}
int ab16_mount(ab16_fs*f,ab16_io io){
    uint32_t i;uint8_t state[AB16_NSECTORS];ab16_mountarg m={0,0};ab16_file sb;
    int rc;if(!f||!io.read)return AB16_EINVAL;
    f->io=io;f->mounted=0u;f->next_seq=0u;f->records=0u;f->blank_sectors=0u;f->blocked_sectors=0u;
    rc=ab16_walk(f,ab16_mountfn,&m,0,state);if(rc)return rc;
    for(i=0;i<AB16_NSECTORS;i++){
        if(state[i]==0u)f->blank_sectors++;
        if(state[i]==2u)f->blocked_sectors++;
    }
    rc=ab16_rawlookup(f,AB16_SYS,AB16_GC_SYS_ID,&sb);
    if(rc)return m.count?AB16_EFOREIGN:AB16_ENOFS;
    if(sb.deleted||sb.length!=AB16_SYS_BYTES)return AB16_EFOREIGN;
    {uint8_t marker[AB16_SYS_BYTES];
        rc=ab16_rawread(f,&sb,0,marker,sizeof marker);if(rc)return rc;
        for(i=0;i<sizeof marker;i++)if(marker[i]!=ab16_super[i])return AB16_EFOREIGN;
    }
    if(m.seq==0xFFFFFFFFu)return AB16_ESEQ;
    f->records=m.count;f->next_seq=m.seq+1u;f->mounted=1u;
    return 0;
}
static int ab16_findtail(ab16_fs*f,uint32_t bytes,uint32_t avoid,uint32_t*out){
    uint32_t tails[AB16_NSECTORS],i;uint8_t state[AB16_NSECTORS];int rc;
    rc=ab16_walk(f,0,0,tails,state);if(rc)return rc;
    /* prefer using old sector tails, saving completely blank sectors for GC */
    for(i=0;i<AB16_NSECTORS;i++)if(i!=avoid&&state[i]==1u&&
         tails[i]<=(i+1u)*AB16_SECTOR&&bytes<=(i+1u)*AB16_SECTOR-tails[i]){
        *out=tails[i];return 0;
    }
    for(i=0;i<AB16_NSECTORS;i++)if(i!=avoid&&state[i]==0u&&bytes<=AB16_SECTOR){
        *out=i*AB16_SECTOR;return 0;
    }
    return AB16_ENOSPACE;
}
static int ab16_arrfill(void*ctx,uint32_t off,void*dst,uint32_t n){
    const uint8_t*s=(const uint8_t*)ctx;uint8_t*d=(uint8_t*)dst;
    uint32_t i;for(i=0;i<n;i++)d[i]=s[off+i];return 0;
}
static int ab16_append(ab16_fs*f,uint16_t type,uint32_t id,uint16_t n,uint16_t flags,
                  ab16_fill fill,void*ctx,uint32_t avoid){
    uint8_t hdr[AB16_HDR],buf[128],mark[4];uint32_t off,seq,c=0xFFFFFFFFu,pos,k,sz=ab16_total(n);int rc;
    if(!f->mounted)return AB16_ESTATE;
    if(!type&&id!=AB16_GC_SYS_ID)return AB16_EINVAL;
    if(n>AB16_MAX_DATA||(flags&AB16_DEL&&n)||(!fill&&n)||f->next_seq==0xFFFFFFFFu)return AB16_EINVAL;
    rc=ab16_findtail(f,sz,avoid,&off);if(rc)return rc;
    for(pos=0;pos<n;pos+=k){
        k=n-pos;if(k>sizeof buf)k=sizeof buf;
        if(fill(ctx,pos,buf,k))return AB16_EIO;
        c=ab16_crc_add(c,buf,k);
    }
    for(pos=0;pos<AB16_HDR;pos++)hdr[pos]=0xFFu;
    seq=f->next_seq;
    ab16_st32(hdr,AB16_MAGIC);ab16_st32(hdr+4,seq);ab16_st16(hdr+8,type);ab16_st16(hdr+10,flags);
    ab16_st32(hdr+12,id);ab16_st16(hdr+16,n);ab16_st16(hdr+18,0u);
    ab16_st32(hdr+20,~c);ab16_st32(hdr+24,ab16_crc32(hdr,24));
    rc=ab16_pg(f,off,hdr,28u);if(rc)return rc;
    for(pos=0;pos<n;pos+=k){
        k=n-pos;if(k>sizeof buf)k=sizeof buf;
        if(fill(ctx,pos,buf,k))return AB16_EIO;
        rc=ab16_pg(f,off+AB16_HDR+pos,buf,k);if(rc)return rc;
    }
    ab16_st32(mark,AB16_COMMIT);
    rc=ab16_pg(f,off+28u,mark,4u);if(rc)return rc;
    /* Read-back verification after commit. On error, old version stays present.
     * Caller must re-mount before retrying a failed write. */
    {ab16_file file;file.offset=off;file.length=n;
        rc=ab16_rawread(f,&file,0,0,0);if(rc)return rc;
    }
    f->next_seq=seq+1u;f->records++;
    return 0;
}
int ab16_create_erased(ab16_fs*f,ab16_io io){
    uint32_t i;int rc;
    if(!f||!io.read||!io.prog)return AB16_EINVAL;
    f->io=io;f->mounted=0;
    for(i=0;i<AB16_SIZE;i+=256u){rc=ab16_allff(f,i,256u);if(rc<0)return rc;if(!rc)return AB16_EFOREIGN;}
    f->mounted=1;f->next_seq=1;f->records=0;
    rc=ab16_append(f,AB16_SYS,0,AB16_SYS_BYTES,0u,ab16_arrfill,(void*)ab16_super,AB16_NSECTORS);
    f->mounted=0;
    if(rc)return rc;
    return ab16_mount(f,io);
}
int ab16_put(ab16_fs*f,uint16_t type,uint32_t id,uint16_t n,ab16_fill fill,void*ctx){
    if(!f||!f->mounted||!type||n>AB16_MAX_DATA)return AB16_EINVAL;
    return ab16_append(f,type,id,n,0u,fill,ctx,AB16_NSECTORS);
}
int ab16_remove(ab16_fs*f,uint16_t type,uint32_t id){
    ab16_file e;int rc;if(!f||!f->mounted||!type)return AB16_EINVAL;
    rc=ab16_lookup(f,type,id,&e);if(rc)return rc;
    return ab16_append(f,type,id,0u,AB16_DEL,0,0,AB16_NSECTORS);
}
/* Streaming copier: source record's payload is read, then re-appended with a
 * new sequence. Before erasing source, verify NO source record is still latest. */
typedef struct {ab16_fs*f;uint32_t offset;} ab16_srcread;
static int ab16_sourcefill(void*ctx,uint32_t off,void*dst,uint32_t n){
    ab16_srcread*s=(ab16_srcread*)ctx;return ab16_rd(s->f,s->offset+AB16_HDR+off,dst,n);
}
static int ab16_sector_live(ab16_fs*f,uint32_t sector,uint32_t*live,uint32_t*stale){
    uint32_t off=sector*AB16_SECTOR,stop=off+AB16_SECTOR,phys;uint8_t b[AB16_HDR];
    ab16_ahdr h;ab16_file newest;int rc;*live=*stale=0u;
    for(;stop-off>=AB16_HDR;off+=ab16_total(h.length)){
        rc=ab16_rd(f,off,b,sizeof b);if(rc)return rc;
        if(!ab16_decode(b,&h)||ab16_total(h.length)>stop-off)break;
        phys=ab16_total(h.length);
        rc=ab16_rawlookup(f,h.type,h.id,&newest);if(rc)return rc;
        if(newest.offset==off)*live+=phys;
        else *stale+=phys;
    }
    return 0;
}
int ab16_gc_one(ab16_fs*f,uint32_t*freed){
    uint32_t sector,best=AB16_NSECTORS,best_stale=0u,best_live=0u,li,st;
    uint32_t off,stop;uint8_t b[AB16_HDR],states[AB16_NSECTORS];ab16_ahdr h;
    int rc;
    if(freed)*freed=0u;
    if(!f||!f->mounted||!f->io.erase||!f->io.prog)return AB16_ESTATE;
    rc=ab16_walk(f,0,0,0,states);if(rc)return rc;
    /* All-blank and FOREIGN sectors are excluded from GC. Torn sectors are
     * quarantined until a separate recovery tool proves they are ours. */
    for(sector=0;sector<AB16_NSECTORS;sector++){
        if(states[sector]!=1u)continue;
        rc=ab16_sector_live(f,sector,&li,&st);if(rc)return rc;
        if(st && (best==AB16_NSECTORS || (best_live>0u&&li==0u) ||
                   (best_live>0u&&li<best_live) || (li==best_live&&st>best_stale))){
            best=sector;best_live=li;best_stale=st;
        }
    }
    if(best==AB16_NSECTORS)return 0;
    off=best*AB16_SECTOR;stop=off+AB16_SECTOR;
    while(stop-off>=AB16_HDR){
        ab16_file newest;ab16_srcread src;
        rc=ab16_rd(f,off,b,sizeof b);if(rc)return rc;
        if(!ab16_decode(b,&h)||ab16_total(h.length)>stop-off)break;
        rc=ab16_rawlookup(f,h.type,h.id,&newest);if(rc)return rc;
        if(newest.offset==off){
            if(ab16_rawread(f,&newest,0,0,0))return AB16_ECRC;
            src.f=f;src.offset=off;
            rc=ab16_append(f,h.type,h.id,h.length,h.flags,ab16_sourcefill,&src,best);
            if(rc)return rc; /* leave source intact */
        }
        off+=ab16_total(h.length);
    }
    rc=ab16_sector_live(f,best,&li,&st);if(rc)return rc;
    if(li)return AB16_ESTATE; /* abort BEFORE erase if any source record still current */
    rc=ab16_er(f,best*AB16_SECTOR);if(rc)return rc;
    rc=ab16_allff(f,best*AB16_SECTOR,AB16_SECTOR);if(rc!=1)return AB16_EIO;
    if(freed)*freed=1u;
    return ab16_mount(f,f->io);
}

