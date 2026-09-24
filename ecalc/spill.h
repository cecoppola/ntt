/* spill.h - Phase 14 S1 (APUMULT_STUDY E3, E4): file I/O that leaves nothing in the page cache, and the spill of a
 * device number's limb range to disk and back.
 *
 * On the APU the page cache is HBM (results/apumult/apumult_M.md: a 64 GiB buffered write took 64 GiB of MemFree and
 * kept it after fsync).  Two remedies, both measured there: O_DIRECT (never enters the cache; 2.0 GB/s write, 5.9 GB/s
 * read on aac6's NVMe) and fsync + posix_fadvise(DONTNEED) (gives the cache back after the write).  O_DIRECT needs
 * 4 KiB-aligned buffers, lengths and offsets, and cannot DMA from hipMalloc memory (EFAULT): device data goes through a
 * pinned host bounce.
 *
 *  sp_file    a sequential file written or read through aligned buffers: unaligned lengths are carried (< 4 KiB kept
 *             between calls, the last block written padded and the file truncated to its exact size); O_DIRECT when
 *             asked and the file system takes it, else buffered + fsync + DONTNEED at close.  The data moves by
 *             spf_write_dev / spf_read_dev from / to device memory (dev >= 0, hipMemcpyAsync through the caller's bounce
 *             buffers, double-buffered when two are given) or host memory (dev < 0, memcpy).
 *  spill_*    a dbig's limbs [lo, hi) -> ECALC_SPILL_DIR (one file per quarter, four threads, each on its APU with a
 *             bounce on its NUMA node), in the background; spill_wait = completion (and, with SPILL_FREE, the blocks
 *             returned to the pool); spill_restore = db_reserve + read back.
 *
 * Switches: ECALC_ODIRECT=1 (the checkpoint writer, the top set and the output file use this file's I/O; off = the old
 * buffered code), ECALC_SPILL_DIR=<dir> (unset: spilling disabled, spill_start returns 0), SPILL_CHUNK_MB (the bounce
 * buffers, 2 per APU, default 256), SPILL_BUFFERED=1 (test: no O_DIRECT, fsync + DONTNEED instead). */
#ifndef EC_SPILL_H
#define EC_SPILL_H
#include <stddef.h>
#include <stdint.h>
#include "dbig.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SP_ALIGN 4096
typedef struct sp_file {
    int fd, write, direct;             /* direct: the fd is O_DIRECT */
    uint64_t pos;                      /* file offset of the next byte (write: including the carry) */
    size_t ncarry;                     /* write: bytes held in carry, not yet on disk (< SP_ALIGN) */
    unsigned char *carry;              /* SP_ALIGN bytes, aligned */
    void *st[8];                       /* hipStream_t per device for the DMA (created on first use) */
    int err;                           /* errno of the first failure */
    double t_io, t_dma;                /* seconds in pwrite/pread, in waiting for the DMA */
    uint64_t bytes;                    /* payload bytes moved */
} sp_file;

int  sp_odirect(void);                 /* ECALC_ODIRECT=1 */
int  spf_open(sp_file *f, const char *path, int write, int direct);   /* 1 ok; direct falls back to buffered if the fs refuses O_DIRECT */
/* the next `bytes` of the file <- src (write) / -> dst (read).  dev >= 0: device memory on APU dev, moved through buf[0]
 * (and buf[1] if non-null: double-buffered) of bcap bytes each (aligned, a multiple of SP_ALIGN, >= 4 SP_ALIGN; pinned
 * for full speed).  dev < 0: host memory (memcpy through buf[0]).  guard (may be null): {active, cancel} -- *active is
 * held > 0 while a device read of src is in flight and *cancel is checked before each (the background writer's
 * contract, binsplit.c); returns 0 on failure or cancel. */
int  spf_write_dev(sp_file *f, int dev, const void *src, size_t bytes, void *const buf[2], size_t bcap, volatile int *guard[2]);
int  spf_read_dev(sp_file *f, int dev, void *dst, size_t bytes, void *const buf[2], size_t bcap);
int  spf_close(sp_file *f, int ok);    /* write: the carry (padded, then ftruncate), fsync, DONTNEED; returns ok && no error */
void sp_drop_cache(int fd);            /* fsync + posix_fadvise(DONTNEED) on a buffered file (the output writer, headers) */
void sp_drop_cache_path(const char *path);

/* ---- the spill primitive ---- */
enum { SPILL_FREE = 1 };               /* spill_start takes the dbig (the caller's descriptor is zeroed) and frees its blocks when the data is on disk */
typedef struct spill spill;
spill *spill_start(dbig *x, size_t lo, size_t hi, const char *name, int flags);   /* background; 0 when ECALC_SPILL_DIR is unset or on failure to start */
spill *spill_start_dir(dbig *x, size_t lo, size_t hi, const char *dir, const char *name, int flags);
int    spill_done(spill *s);           /* 1 when the data is on disk (non-blocking) */
int    spill_wait(spill *s);           /* waits; 1 ok.  With SPILL_FREE the blocks are free after it */
int    spill_restore(spill *s, dbig *x);   /* x (empty or of cap >= the spilled number's) <- limbs [lo, hi), x->n = the spilled n; 1 ok */
int    spill_restore_start(spill *s, dbig *x);   /* the same in the background: x's blocks reserved now, the reads started */
int    spill_restore_wait(spill *s);   /* the restore's completion; 1 ok (x->n set here) */
void   spill_drop(spill *s);           /* remove the files, free the handle */
struct spill_stats { size_t bytes; double t_write, t_read, rate_w, rate_r; };
void   spill_get_stats(const spill *s, struct spill_stats *st);
int    spill_enabled(void);            /* ECALC_SPILL_DIR set */
void   spill_prealloc(void);           /* the bounce buffers of the four APUs now (2 x SPILL_CHUNK_MB each, pinned, NUMA-local: 2 GiB at
                                        * the default; from the main thread at a quiet point -- mem.c's registry is read unlocked); else
                                        * they are allocated at the first spill */
void   spill_fini(void);               /* release the bounce buffers */

#ifdef __cplusplus
}
#endif
#endif
