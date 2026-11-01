#include "ks_song_grid_core.h"
#include "ks_arranger.h"
#include <string.h>
static unsigned ksg_offset(const ksg_grid_t *g,unsigned tr){
    unsigned i,p=0;for(i=0;i<tr && i<KSG_TRACKS;i++)p+=g->count[i];return p;
}
unsigned ksg_total(const ksg_grid_t *g){return ksg_offset(g,KSG_TRACKS);}
void ksg_clear(ksg_grid_t *g){memset(g,0,sizeof *g);}
int ksg_valid(const ksg_grid_t *g){
    unsigned i,n=ksg_total(g);
    if(n>KSG_MAX_REFS)return 0;
    for(i=0;i<n;i++)if(g->pat[i]>KSG_SILENT)return 0;
    return 1;
}
uint16_t ksg_get(const ksg_grid_t *g,unsigned tr,unsigned col){
    if(tr>=KSG_TRACKS||col>=g->count[tr])return KSG_EMPTY;
    return g->pat[ksg_offset(g,tr)+col];
}
int ksg_put(ksg_grid_t *g,unsigned tr,unsigned col,uint16_t pat){
    unsigned at,n;
    if(tr>=KSG_TRACKS||pat>KSG_SILENT||col>g->count[tr]||!ksg_valid(g))return -1;
    at=ksg_offset(g,tr)+col;n=ksg_total(g);
    if(col==g->count[tr]){
        if(n>=KSG_MAX_REFS)return -2;
        memmove(g->pat+at+1u,g->pat+at,(n-at)*sizeof(uint16_t));
        g->count[tr]++;
    }
    g->pat[at]=pat;
    return 0;
}
int ksg_del(ksg_grid_t *g,unsigned tr,unsigned col){
    unsigned at,n;
    if(tr>=KSG_TRACKS||col>=g->count[tr]||!ksg_valid(g))return -1;
    at=ksg_offset(g,tr)+col;n=ksg_total(g);
    memmove(g->pat+at,g->pat+at+1u,(n-at-1u)*sizeof(uint16_t));
    g->count[tr]--;
    return 0;
}
int ksg_move(ksg_grid_t *g,unsigned tr,unsigned from,unsigned to){
    unsigned off;uint16_t p;
    if(tr>=KSG_TRACKS||from>=g->count[tr]||to>=g->count[tr]||!ksg_valid(g))return -1;
    off=ksg_offset(g,tr);p=g->pat[off+from];
    if(to>from)memmove(g->pat+off+from,g->pat+off+from+1u,(to-from)*sizeof(uint16_t));
    else if(to<from)memmove(g->pat+off+to+1u,g->pat+off+to,(from-to)*sizeof(uint16_t));
    g->pat[off+to]=p;return 0;
}
