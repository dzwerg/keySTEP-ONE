/* analogBEAST MUSIC DISK v16: variable-length, append-only NOR log.
 * No fixed file count or per-type allocation. Only the physical volume limits entries.
 * Whole-file replacement is atomic: payload first, commit marker last.
 * Firmware Stage16 exposes READ-ONLY diagnostics. Host tests exercise writes and GC.
 */
#ifndef AB16_FS_H
#define AB16_FS_H
#include <stdint.h>
#define AB16_BASE 0xA0000u
#define AB16_END  0xDC000u
#define AB16_SIZE (AB16_END-AB16_BASE)
#define AB16_SECTOR 4096u
#define AB16_NSECTORS (AB16_SIZE/AB16_SECTOR)
#define AB16_HDR 32u
#define AB16_MAX_DATA (AB16_SECTOR-AB16_HDR)
#define AB16_SYS 0u
#define AB16_PAT 1u
#define AB16_PATTERN_ID_MAX 512u /* MD16 IDs; legacy MD13 range unchanged */
#define AB16_SONG 2u
#define AB16_ALBUM 3u
#define AB16_PRESET 4u
#define AB16_VL1 5u
#define AB16_MBOX 6u      /* M1-x manifest */
#define AB16_MCHUNK 7u
#define AB16_KSTEP4 8u /* keySTEP four-track polyphonic pattern codec; preserves AB16 types 1-7 */    /* M1-x immutable song data chunks */
#define AB16_OK 0
#define AB16_EIO -1
#define AB16_EINVAL -2
#define AB16_EFOREIGN -3
#define AB16_ENOFS -4
#define AB16_ENOTFOUND -5
#define AB16_ENOSPACE -6
#define AB16_ECRC -7
#define AB16_ESEQ -8
#define AB16_ESTATE -9
#define AB16_EPOWER -10

typedef struct {
    void *ctx;
    int (*read)(void*,uint32_t,void*,uint32_t);
    int (*prog)(void*,uint32_t,const void*,uint32_t);
    int (*erase)(void*,uint32_t);
} ab16_io;
typedef struct {
    ab16_io io;
    uint32_t next_seq;
    uint32_t records;
    uint32_t blocked_sectors;
    uint32_t blank_sectors;
    uint8_t mounted;
} ab16_fs;
typedef struct {
    uint32_t offset; /* record header offset within AB16_SIZE */
    uint32_t seq;
    uint32_t id;
    uint16_t type;
    uint16_t length;
    uint8_t deleted;
} ab16_file;
typedef int (*ab16_fill)(void*,uint32_t,void*,uint32_t);

/* mount accepts only an explicit AB16 filesystem marker, otherwise makes NO writes */
int ab16_mount(ab16_fs *fs,ab16_io io);
/* create is host-only until a separate, explicitly approved device format flow exists.
 * Requires the ENTIRE 240KiB region to be ERASED. Does NOT erase anything. */
int ab16_create_erased(ab16_fs *fs,ab16_io io);
int ab16_lookup(ab16_fs *fs,uint16_t type,uint32_t id,ab16_file *file);
int ab16_read(ab16_fs *fs,uint16_t type,uint32_t id,uint32_t off,void *dst,uint32_t n);
/* Foreground/UI selector: picks the next existing, CRC-valid file (with wrap).
 * direction must be +1 or -1; deleted/missing files are never selectable.
 * For one remaining file, selection stays on that file. When a type has no
 * live files returns AB16_ENOTFOUND (UI should display EMPTY and not PLAY).
 * This scans the NOR log and must NEVER run from an audio interrupt. */
int ab16_select_existing(ab16_fs *fs,uint16_t type,uint32_t current,
                         int direction,uint32_t *selected);
int ab16_put(ab16_fs *fs,uint16_t type,uint32_t id,uint16_t n,ab16_fill fill,void *ctx);
int ab16_remove(ab16_fs *fs,uint16_t type,uint32_t id);
/* One incremental GC pass, with no erasure until every live record is safely copied.
 * On insufficient destination space the old sector is retained unchanged. */
int ab16_gc_one(ab16_fs *fs,uint32_t *freed_sectors);
#endif
