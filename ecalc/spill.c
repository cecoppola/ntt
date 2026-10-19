/* spill.c - see spill.h  */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <hip/hip_runtime.h>
#include "fatal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include "spill.h"
#include "mem.h"

#define SP_HIP(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)

int sp_odirect(void)
{
    static int v = -1;
    if (v < 0) v = getenv("ECALC_ODIRECT") ? (sp_odirect_auto() ? 1 : atoi(getenv("ECALC_ODIRECT"))) : 1;   /* default 1; a file system that refuses O_DIRECT falls back to buffered + dropped cache */
    return v;
}
/* ---- O_DIRECT by file-system type ---- */
int sp_odirect_auto(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("ECALC_ODIRECT"); v = !e || !strcmp(e, "auto"); }   /* default auto */
    return v;
}
int sp_fs_kind(const char *path)
{
    struct statfs s; char dir[4096];
    if (!path || !*path) path = ".";
    if (statfs(path, &s) != 0) {                                    /* not created yet: its directory */
        snprintf(dir, sizeof dir, "%s", path); char *sl = strrchr(dir, '/');
        if (sl) { if (sl == dir) sl[1] = 0; else *sl = 0; } else snprintf(dir, sizeof dir, ".");
        if (statfs(dir, &s) != 0) return SP_FS_OTHER;
    }
    unsigned long t = (unsigned long)s.f_type;
    if (t == 0x6969UL || t == 0x6E667364UL /* nfsd */ || t == 0xFE534D42UL /* smb2 */ || t == 0xFF534D42UL /* cifs */) return SP_FS_NFS;
    if (t == 0x0BD00BD0UL) return SP_FS_LUSTRE;
    if (t == 0x01021994UL || t == 0x858458F6UL /* ramfs */) return SP_FS_TMPFS;
    if (t == 0xEF53UL /* ext2/3/4 */ || t == 0x58465342UL /* xfs */ || t == 0x9123683EUL /* btrfs */ || t == 0x2FC12FC1UL /* zfs */) return SP_FS_LOCAL;
    return SP_FS_OTHER;
}
const char *sp_fs_name(int k) { static const char *n[] = { "local", "NFS", "Lustre", "tmpfs", "other" }; return k >= 0 && k <= SP_FS_OTHER ? n[k] : "?"; }
int sp_direct_for(const char *path) { return sp_odirect_auto() ? sp_direct_by_fs(path) : sp_odirect(); }
int sp_direct_by_fs(const char *path)
{
    const char *e;
    switch (sp_fs_kind(path)) {
    case SP_FS_NFS:    e = getenv("ECALC_ODIRECT_NFS");    return e ? atoi(e) : 1;   /* W1 (results/OUTPUT_IO.md §IO15 §3): on aac6's NFS O_DIRECT 0.64-0.96 GB/s vs buffered + fsync 0.36-0.48 (10 GbE), 0.113-0.116 vs 0.098-0.104 (1 GbE) */
    case SP_FS_LUSTRE: e = getenv("ECALC_ODIRECT_LUSTRE"); return e ? atoi(e) : 1;   /* ASSUMED: large aligned direct writes are Lustre's fast path; measure on the target (TARGET_TASKS T1) */
    case SP_FS_TMPFS:  return 0;
    case SP_FS_OTHER:  e = getenv("ECALC_ODIRECT_OTHER");  return e ? atoi(e) : 1;
    default:           return 1;
    }
}
void sp_drop_cache(int fd)
{
    if (fd < 0) return;
    fsync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);     /* only clean pages go: hence the fsync first (apumult_M.md 2) */
}
/* every device's queued work complete (the DMA of this file runs on its own non-blocking streams, which are not ordered
 * after the pipeline's null-stream work the way a hipMemcpy is); the calling thread's device kept */
void sp_dev_sync_all(void)
{
    int cur, n = 0; SP_HIP(hipGetDevice(&cur)); SP_HIP(hipGetDeviceCount(&n)); if (n > 8) n = 8;
    for (int d = 0; d < n; d++) { SP_HIP(hipSetDevice(d)); SP_HIP(hipDeviceSynchronize()); }
    SP_HIP(hipSetDevice(cur));
}
void sp_drop_cache_path(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { sp_drop_cache(fd); close(fd); }
}

/* ---- sp_file ---- */
int spf_open(sp_file *f, const char *path, int write, int direct)
{
    memset(f, 0, sizeof *f); f->fd = -1; f->write = write;
    int fl = write ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY;
    if (direct && sp_odirect_auto()) direct = sp_direct_for(path);   /* Phase 15 IO (W7) */
    if (direct) {
        f->fd = open(path, fl | O_DIRECT, 0644);
        if (f->fd >= 0) f->direct = 1;
        else if (errno != EINVAL) { fprintf(stderr, "spill: cannot open %s: %s\n", path, strerror(errno)); return 0; }
    }
    if (f->fd < 0) f->fd = open(path, fl, 0644);      /* the file system refused O_DIRECT (tmpfs): buffered, dropped from the cache at close */
    if (f->fd < 0) { fprintf(stderr, "spill: cannot open %s: %s\n", path, strerror(errno)); return 0; }
    if (posix_memalign((void **)&f->carry, SP_ALIGN, SP_ALIGN)) { close(f->fd); f->fd = -1; return 0; }
    return 1;
}
static hipStream_t sp_stream(sp_file *f, int dev)
{
    if (!f->st[dev]) { hipStream_t s; SP_HIP(hipSetDevice(dev)); SP_HIP(hipStreamCreateWithFlags(&s, hipStreamNonBlocking)); f->st[dev] = (void *)s; }
    return (hipStream_t)f->st[dev];
}
static int pwrite_full(sp_file *f, const void *p, size_t n, uint64_t off)
{
    double t = mem_now();
    const char *c = (const char *)p; int tries = 0;
    while (n) {
        ssize_t w = pwrite(f->fd, c, n, (off_t)off);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EIO || errno == ETIMEDOUT || errno == EAGAIN || errno == ENOMEM) && tries < 3) {   /* as pread_need: the same bytes to the same offset again */
            int e = errno; tries++;
            fprintf(stderr, "spill: write at offset %llu: %s -- again in %u s (%d of 3)\n", (unsigned long long)off, strerror(e), 1u << (2 * (tries - 1)), tries);
            sleep(1u << (2 * (tries - 1))); continue;
        }
        if (w < 0) { if (!f->err) f->err = errno; return 0; }
        if (w == 0) { if (!f->err) f->err = EIO; return 0; }
        c += w; n -= (size_t)w; off += (uint64_t)w; tries = 0;
    }
    f->t_io += mem_now() - t;
    return 1;
}
/* reads until `need` bytes are in (the rest of `n` may be past the end of the file).  CKPT (results/CHECKPOINT.md §CKPT): the home
 * file system of the test cluster is NFS mounted `soft` on the compute nodes, so a read whose RPCs wait longer than the
 * mount's timeouts fails with EIO; four 512 MiB O_DIRECT reads per node on 6 nodes queued enough to fail 16 of 24 top-set
 * files.  So: (1) one pread asks for at most SP_READ_MAX bytes (64 MiB: 1.5 GiB in flight over 6 nodes, seconds of queue
 * at the server's ~0.3 GB/s against its 60 s timeout; 16 MiB read the set 1.7x slower at 2 nodes); (2) EIO / ETIMEDOUT /
 * EAGAIN / ENOMEM: the same read again after 1, 4, 16 s (a read is idempotent; said on stderr), 3 failures in a row fail
 * it; (3) O_DIRECT: a read that comes back short at a byte count that is not a whole block before `need` is read again
 * from that block (p and off are block-aligned, so p + got and off + got stay aligned); the same partial count twice in
 * a row is the end of the file. */
#define SP_READ_MAX ((size_t)64 << 20)
static int pread_need(sp_file *f, void *p, size_t n, uint64_t off, size_t need)
{
    double t = mem_now();
    char *c = (char *)p; size_t got = 0, part = 0; int tries = 0;
    while (got < need) {
        if (f->direct && (got & (SP_ALIGN - 1))) {      /* a partial block below `need`: again from its start */
            if (got == part) break;                     /* (the end of the file) */
            part = got; got &= ~(size_t)(SP_ALIGN - 1);
        }
        size_t want = n - got < SP_READ_MAX ? n - got : SP_READ_MAX;
        ssize_t r = pread(f->fd, c + got, want, (off_t)(off + got));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EIO || errno == ETIMEDOUT || errno == EAGAIN || errno == ENOMEM) && tries < 3) {
            int e = errno; tries++;
            fprintf(stderr, "spill: read at offset %llu: %s -- again in %u s (%d of 3)\n", (unsigned long long)(off + got), strerror(e), 1u << (2 * (tries - 1)), tries);
            sleep(1u << (2 * (tries - 1))); continue;
        }
        if (r < 0) { if (!f->err) { f->err = errno; f->eoff = off + got; f->egot = got; f->eneed = need; } return 0; }
        if (r == 0) { if (!f->err) { f->err = EIO; f->eoff = off + got; f->egot = got; f->eneed = need; } return 0; }   /* short file */
        got += (size_t)r; tries = 0;
    }
    f->t_io += mem_now() - t;
    if (got < need) { if (!f->err) { f->err = EIO; f->eoff = off + got; f->egot = got; f->eneed = need; } return 0; }
    return 1;
}
/* device -> buffer (async on the file's stream of dev), with the guard; host: memcpy */
static int stage_in(sp_file *f, int dev, void *dst, const void *src, size_t n, volatile int *guard[2])
{
    if (dev < 0) { memcpy(dst, src, n); return 1; }
    if (guard) {
        __atomic_add_fetch(guard[0], 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(guard[1], __ATOMIC_SEQ_CST)) { __atomic_sub_fetch(guard[0], 1, __ATOMIC_SEQ_CST); return 0; }
    }
    hipStream_t s = sp_stream(f, dev);
    SP_HIP(hipMemcpyAsync(dst, src, n, hipMemcpyDefault, s));
    return 1;
}
static void stage_wait(sp_file *f, int dev, volatile int *guard[2])
{
    if (dev < 0) return;
    double t = mem_now();
    SP_HIP(hipStreamSynchronize(sp_stream(f, dev)));
    f->t_dma += mem_now() - t;
    if (guard) __atomic_sub_fetch(guard[0], 1, __ATOMIC_SEQ_CST);
}
static int spf_write_dev_(sp_file *f, int dev, const void *src, size_t bytes, void *const buf[2], size_t bcap, volatile int *guard[2])
{
    if (!bytes) return 1;
    if (f->fd < 0 || !f->write || bcap < 4 * SP_ALIGN || (bcap & (SP_ALIGN - 1))) { fprintf(stderr, "spill: bad write call (bcap %zu)\n", bcap); return 0; }
    if (dev >= 0) SP_HIP(hipSetDevice(dev));
    const char *s = (const char *)src;
    int two = buf[1] != 0 && dev >= 0;
    size_t step = bcap - SP_ALIGN, done = 0, n = bytes < step ? bytes : step;
    int b = 0;
    unsigned char *cb = (unsigned char *)buf[0];
    memcpy(cb, f->carry, f->ncarry);
    if (!stage_in(f, dev, cb + f->ncarry, s, n, guard)) return 0;
    stage_wait(f, dev, guard);
    size_t total = f->ncarry + n; done = n;
    for (;;) {
        cb = (unsigned char *)buf[b];
        size_t aw = total & ~(size_t)(SP_ALIGN - 1), rem = total - aw;
        memcpy(f->carry, cb + aw, rem); f->ncarry = rem;
        int more = done < bytes, nb = two ? b ^ 1 : b;
        size_t nn = more ? (bytes - done < step ? bytes - done : step) : 0;
        if (more && two) {                              /* the next DMA runs under this pwrite */
            memcpy(buf[nb], f->carry, rem);
            if (!stage_in(f, dev, (unsigned char *)buf[nb] + rem, s + done, nn, guard)) return 0;
        }
        int ok = !aw || pwrite_full(f, cb, aw, f->pos);
        if (ok) f->pos += aw;
        if (!more) { if (ok) f->bytes += bytes; return ok; }
        if (!two) {
            if (!ok) return 0;
            memcpy(cb, f->carry, rem);
            if (!stage_in(f, dev, cb + rem, s + done, nn, guard)) return 0;
        }
        stage_wait(f, dev, guard);
        if (!ok) return 0;
        total = rem + nn; done += nn; b = nb;
    }
}
static int spf_read_dev_(sp_file *f, int dev, void *dst, size_t bytes, void *const buf[2], size_t bcap)
{
    if (!bytes) return 1;
    if (f->fd < 0 || f->write || bcap < 4 * SP_ALIGN || (bcap & (SP_ALIGN - 1))) { fprintf(stderr, "spill: bad read call (bcap %zu)\n", bcap); return 0; }
    if (dev >= 0) SP_HIP(hipSetDevice(dev));
    char *d = (char *)dst;
    int two = buf[1] != 0 && dev >= 0;
    size_t step = bcap - 2 * SP_ALIGN, done = 0, off[2] = { 0, 0 }, cnt[2] = { 0, 0 };
    int b = 0;
    /* piece into buffer i: the aligned blocks covering [pos, pos + n) */
#define SP_PIECE(i) do { uint64_t al_ = f->pos & ~(uint64_t)(SP_ALIGN - 1); off[i] = (size_t)(f->pos - al_); \
        cnt[i] = bytes - done < step ? bytes - done : step; size_t len_ = (off[i] + cnt[i] + SP_ALIGN - 1) & ~(size_t)(SP_ALIGN - 1); \
        if (!pread_need(f, buf[i], len_, al_, off[i] + cnt[i])) return 0; f->pos += cnt[i]; } while (0)
    SP_PIECE(0);
    for (;;) {
        char *cb = (char *)buf[b];
        if (dev < 0) memcpy(d + done, cb + off[b], cnt[b]);
        else SP_HIP(hipMemcpyAsync(d + done, cb + off[b], cnt[b], hipMemcpyDefault, sp_stream(f, dev)));
        done += cnt[b];
        int nb = two ? b ^ 1 : b;
        if (done < bytes && two) SP_PIECE(nb);          /* the next read runs under this DMA */
        stage_wait(f, dev, 0);
        if (done >= bytes) break;
        if (!two) SP_PIECE(nb);
        b = nb;
    }
#undef SP_PIECE
    f->bytes += bytes;
    return 1;
}
/* the public calls keep the calling thread's current device (the pipeline's threads assume theirs) */
int spf_write_dev(sp_file *f, int dev, const void *src, size_t bytes, void *const buf[2], size_t bcap, volatile int *guard[2])
{
    int cur = 0; if (dev >= 0) SP_HIP(hipGetDevice(&cur));
    int ok = spf_write_dev_(f, dev, src, bytes, buf, bcap, guard);
    if (dev >= 0) SP_HIP(hipSetDevice(cur));
    return ok;
}
int spf_read_dev(sp_file *f, int dev, void *dst, size_t bytes, void *const buf[2], size_t bcap)
{
    int cur = 0; if (dev >= 0) SP_HIP(hipGetDevice(&cur));
    int ok = spf_read_dev_(f, dev, dst, bytes, buf, bcap);
    if (dev >= 0) SP_HIP(hipSetDevice(cur));
    return ok;
}
int spf_close(sp_file *f, int ok)
{
    if (f->fd < 0) return 0;
    if (f->write) {
        if (ok && f->ncarry) {                          /* the last partial block: padded to a whole one, then the file cut to size */
            memset(f->carry + f->ncarry, 0, SP_ALIGN - f->ncarry);
            ok = pwrite_full(f, f->carry, SP_ALIGN, f->pos);
            if (ok && ftruncate(f->fd, (off_t)(f->pos + f->ncarry)) != 0) { if (!f->err) f->err = errno; ok = 0; }
            if (ok) f->pos += f->ncarry;
            f->ncarry = 0;
        }
        if (ok && fsync(f->fd) != 0) { if (!f->err) f->err = errno; ok = 0; }
    }
    if (!f->direct) posix_fadvise(f->fd, 0, 0, POSIX_FADV_DONTNEED);   /* buffered fallback: nothing stays in the cache */
    close(f->fd); f->fd = -1;
    int cur = -1;
    for (int d = 0; d < 8; d++) if (f->st[d]) { if (cur < 0) SP_HIP(hipGetDevice(&cur)); SP_HIP(hipSetDevice(d)); SP_HIP(hipStreamDestroy((hipStream_t)f->st[d])); f->st[d] = 0; }
    if (cur >= 0) SP_HIP(hipSetDevice(cur));
    free(f->carry); f->carry = 0;
    return ok && !f->err;
}
