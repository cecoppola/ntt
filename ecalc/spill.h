/* spill.h - file I/O that leaves nothing in the page cache (the spill of a device
 * number's limb range to disk and back is not provided: no production caller).
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
 *
 * Switches: ECALC_ODIRECT=1 (the checkpoint writer, the top set and the output file use this file's I/O; off = the old
 * buffered code).  ECALC_ODIRECT=auto (see sp_direct_for). */
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
    uint64_t eoff; size_t egot, eneed; /* read: the failed read's file offset, the bytes it got and needed (the short-read message) */
} sp_file;

int  sp_odirect(void);                 /* ECALC_ODIRECT=1 (also 1 for ECALC_ODIRECT=auto: the cache-hygiene paths on, O_DIRECT per file below) */
/* ECALC_ODIRECT=auto -- O_DIRECT chosen per file by the file system under it (statfs f_type): local disks
 * (ext4, xfs, ...) O_DIRECT; NFS O_DIRECT (measured faster, results/OUTPUT_IO.md §IO15; ECALC_ODIRECT_NFS=0: buffered + fsync + DONTNEED); Lustre O_DIRECT (an ASSUMPTION
 * to be measured on the target; ECALC_ODIRECT_LUSTRE=0: buffered); tmpfs buffered (it refuses O_DIRECT; the file is memory).
 * Without auto, sp_direct_for() is sp_odirect(). */
enum { SP_FS_LOCAL = 0, SP_FS_NFS, SP_FS_LUSTRE, SP_FS_TMPFS, SP_FS_OTHER };
int  sp_odirect_auto(void);            /* ECALC_ODIRECT=auto */
int  sp_fs_kind(const char *path);     /* the file system under path (or its directory when path does not exist yet) */
const char *sp_fs_name(int kind);
int  sp_direct_for(const char *path);  /* O_DIRECT for this file? (auto: by sp_fs_kind; else sp_odirect()) */
int  sp_direct_by_fs(const char *path);   /* the file-system rule alone (ECALC_OUT_MODE=auto uses it whatever ECALC_ODIRECT says) */
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
void sp_dev_sync_all(void);            /* hipDeviceSynchronize on every device (the caller's device kept): before a DMA on our own streams reads what the pipeline wrote */

#ifdef __cplusplus
}
#endif
#endif
