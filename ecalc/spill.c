/* spill.c - see spill.h (Phase 14 S1: APUMULT_STUDY E3, E4) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <hip/hip_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "spill.h"
#include "mem.h"

#define SP_HIP(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

int sp_odirect(void)
{
    static int v = -1;
    if (v < 0) v = getenv("ECALC_ODIRECT") ? atoi(getenv("ECALC_ODIRECT")) : 0;
    return v;
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
    const char *c = (const char *)p;
    while (n) {
        ssize_t w = pwrite(f->fd, c, n, (off_t)off);
        if (w < 0) { if (errno == EINTR) continue; if (!f->err) f->err = errno; return 0; }
        if (w == 0) { if (!f->err) f->err = EIO; return 0; }
        c += w; n -= (size_t)w; off += (uint64_t)w;
    }
    f->t_io += mem_now() - t;
    return 1;
}
/* reads until `need` bytes are in (the rest of `n` may be past the end of the file) */
static int pread_need(sp_file *f, void *p, size_t n, uint64_t off, size_t need)
{
    double t = mem_now();
    char *c = (char *)p; size_t got = 0;
    while (got < need) {
        ssize_t r = pread(f->fd, c + got, n - got, (off_t)(off + got));
        if (r < 0) { if (errno == EINTR) continue; if (!f->err) f->err = errno; return 0; }
        if (r == 0) { if (!f->err) f->err = EIO; return 0; }   /* short file */
        got += (size_t)r;
        if (f->direct && (got & (SP_ALIGN - 1))) break;     /* O_DIRECT: a partial block is the end of the file */
    }
    f->t_io += mem_now() - t;
    if (got < need) { if (!f->err) f->err = EIO; return 0; }
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

/* ---- the spill primitive ---- */
static size_t g_bcap;                                   /* bytes per bounce buffer */
static void *g_bounce[DB_NQ][2];
static pthread_mutex_t g_bmx[DB_NQ] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };
static pthread_mutex_t g_init_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_seq;

int spill_enabled(void) { const char *e = getenv("ECALC_SPILL_DIR"); return e && *e; }
/* the two bounce buffers of APU d, on its NUMA node, pinned (the caller holds g_bmx[d]) */
static void bounce_get(int d, void *b[2])
{
    pthread_mutex_lock(&g_init_mx);
    if (!g_bcap) { const char *e = getenv("SPILL_CHUNK_MB"); size_t mb = e ? (size_t)atoi(e) : 256; if (mb < 1) mb = 1; g_bcap = mb << 20; }
    pthread_mutex_unlock(&g_init_mx);
    for (int i = 0; i < 2; i++) if (!g_bounce[d][i]) g_bounce[d][i] = mem_hstage_alloc(d, g_bcap, 0, 0);
    b[0] = g_bounce[d][0]; b[1] = g_bounce[d][1];
}
void spill_fini(void)
{
    for (int d = 0; d < DB_NQ; d++) { pthread_mutex_lock(&g_bmx[d]); for (int i = 0; i < 2; i++) { mem_hstage_free(g_bounce[d][i]); g_bounce[d][i] = 0; } pthread_mutex_unlock(&g_bmx[d]); }
}
void spill_prealloc(void)
{
    int nd = mem_device_count(); if (nd > DB_NQ || nd < 1) nd = DB_NQ;
    for (int d = 0; d < nd; d++) { void *b[2]; pthread_mutex_lock(&g_bmx[d]); bounce_get(d, b); pthread_mutex_unlock(&g_bmx[d]); }
}

struct restore_job { struct spill *s; int d; int ok; };
struct spill {
    dbig x; int flags, owned;                          /* x: the descriptor (a copy; with SPILL_FREE the only one) */
    size_t lo, hi, n, cap, qc;
    char path[DB_NQ][4096]; size_t flo[DB_NQ], fhi[DB_NQ];   /* file d: limbs [flo, fhi) */
    pthread_t th[DB_NQ]; int running[DB_NQ], ok[DB_NQ]; volatile int ndone; int nwork, joined, status;
    double t0, t1, t_read; size_t bytes;
    dbig *rx;                                          /* restore target */
    struct restore_job rj[DB_NQ]; pthread_t rth[DB_NQ]; int rth_on[DB_NQ], r_started, r_direct; double r_t0;   /* the restore in the background */
};
struct spill_job { spill *s; int d; };
static void *spill_writer(void *a)
{
    struct spill_job *j = (struct spill_job *)a; spill *s = j->s; int d = j->d; free(j);
    sp_file f; int ok = 0;
    pthread_mutex_lock(&g_bmx[d]);
    void *b[2]; bounce_get(d, b);
    if (spf_open(&f, s->path[d], 1, !getenv("SPILL_BUFFERED"))) {
        size_t so = s->flo[d] - (size_t)d * s->qc;
        ok = spf_write_dev(&f, d, s->x.q[d] + so, (s->fhi[d] - s->flo[d]) * 8, b, g_bcap, 0);
        ok = spf_close(&f, ok);
        if (!ok) fprintf(stderr, "spill: write of %s failed: %s\n", s->path[d], strerror(f.err ? f.err : EIO));
    }
    pthread_mutex_unlock(&g_bmx[d]);
    s->ok[d] = ok;
    __atomic_add_fetch(&s->ndone, 1, __ATOMIC_ACQ_REL);
    return 0;
}
spill *spill_start_dir(dbig *x, size_t lo, size_t hi, const char *dir, const char *name, int flags)
{
    if (!dir || !*dir) return 0;
    if (x->off || !x->cap) { fprintf(stderr, "spill: %s is a view or empty\n", name); return 0; }
    if (hi > x->cap) hi = x->cap;
    if (lo > hi) lo = hi;
    spill *s = (spill *)calloc(1, sizeof *s);
    s->x = *x; s->flags = flags; s->lo = lo; s->hi = hi; s->n = x->n; s->cap = x->cap; s->qc = x->qc;
    if (flags & SPILL_FREE) { memset(x, 0, sizeof *x); s->owned = 1; }   /* the caller's descriptor no longer holds the blocks */
    mkdir(dir, 0777);
    sp_dev_sync_all();                                 /* the writers' DMA runs on their own non-blocking streams, not ordered after the caller's kernels */
    int seq =__atomic_add_fetch(&g_seq, 1, __ATOMIC_RELAXED);
    s->t0 = mem_now();
    for (int d = 0; d < DB_NQ; d++) {
        size_t a = (size_t)d * s->qc, e = a + s->qc;
        s->flo[d] = lo > a ? lo : a; s->fhi[d] = hi < e ? hi : e;
        if (s->fhi[d] < s->flo[d]) s->fhi[d] = s->flo[d];
        snprintf(s->path[d], sizeof s->path[d], "%s/%s.%d.%d.q%d", dir, name, (int)getpid(), seq, d);
        s->bytes += (s->fhi[d] - s->flo[d]) * 8;
    }
    for (int d = 0; d < DB_NQ; d++) {
        struct spill_job *j = (struct spill_job *)malloc(sizeof *j); j->s = s; j->d = d;
        if (pthread_create(&s->th[d], 0, spill_writer, j)) { fprintf(stderr, "spill: cannot start a writer\n"); free(j); s->ok[d] = 0; __atomic_add_fetch(&s->ndone, 1, __ATOMIC_ACQ_REL); }
        else s->running[d] = 1;
        s->nwork++;
    }
    return s;
}
spill *spill_start(dbig *x, size_t lo, size_t hi, const char *name, int flags)
{
    if (!spill_enabled()) return 0;
    return spill_start_dir(x, lo, hi, getenv("ECALC_SPILL_DIR"), name, flags);
}
int spill_done(spill *s) { return !s || __atomic_load_n(&s->ndone, __ATOMIC_ACQUIRE) >= s->nwork; }
int spill_wait(spill *s)
{
    if (!s) return 0;
    if (!s->joined) {
        for (int d = 0; d < DB_NQ; d++) if (s->running[d]) pthread_join(s->th[d], 0);
        s->t1 = mem_now(); s->joined = 1;
        s->status = 1; for (int d = 0; d < DB_NQ; d++) s->status = s->status && s->ok[d];
        if (s->owned) {
            if (s->status) { db_free(&s->x); s->owned = 2; }   /* 2: freed, the data only on disk */
            else fprintf(stderr, "spill: the write failed: the blocks are kept (spill_restore returns them)\n");
        }
    }
    return s->status;
}
static void *spill_reader(void *a)
{
    struct restore_job *j = (struct restore_job *)a; spill *s = j->s; int d = j->d; dbig *x = s->rx;
    j->ok = 1;
    if (s->fhi[d] == s->flo[d]) return 0;
    sp_file f; int ok = 0;
    pthread_mutex_lock(&g_bmx[d]);
    void *b[2]; bounce_get(d, b);
    if (spf_open(&f, s->path[d], 0, !getenv("SPILL_BUFFERED"))) {
        ok = 1;
        for (size_t i = s->flo[d]; i < s->fhi[d] && ok;) {   /* the target's quarters (its qc may differ from the spilled one's) */
            size_t q = i / x->qc; if (q >= DB_NQ) q = DB_NQ - 1;
            size_t so = i - q * x->qc, run = x->qc - so; if (i + run > s->fhi[d]) run = s->fhi[d] - i;
            ok = spf_read_dev(&f, (int)q, x->q[q] + so, run * 8, b, g_bcap);
            i += run;
        }
        int fe = f.err; ok = spf_close(&f, ok) && ok;
        if (!ok) fprintf(stderr, "spill: read of %s failed: %s\n", s->path[d], strerror(fe ? fe : EIO));
    }
    pthread_mutex_unlock(&g_bmx[d]);
    j->ok = ok;
    return 0;
}
/* the restore in the background: the blocks reserved here (the caller's thread: the pool's layout stays deterministic),
 * the four readers started; spill_restore_wait joins them */
int spill_restore_start(spill *s, dbig *x)
{
    if (!s) return 0;
    s->r_direct = 0; s->r_started = 0;
    if (!spill_wait(s) || s->owned == 1) {             /* the write failed (or the blocks were never freed): hand them back as they are */
        if (s->owned == 1) { *x = s->x; s->owned = 0; s->r_direct = 1; return 1; }
        return 0;
    }
    if (x->off) { fprintf(stderr, "spill_restore: a view\n"); return 0; }
    s->r_t0 = mem_now();
    if (x->cap < s->cap) db_reserve(x, s->cap);
    s->rx = x;
    for (int d = 0; d < DB_NQ; d++) {
        s->rj[d].s = s; s->rj[d].d = d; s->rj[d].ok = 0; s->rth_on[d] = 0;
        if (pthread_create(&s->rth[d], 0, spill_reader, &s->rj[d]) == 0) s->rth_on[d] = 1; else spill_reader(&s->rj[d]);
    }
    s->r_started = 1;
    return 1;
}
int spill_restore_wait(spill *s)
{
    if (!s) return 0;
    if (s->r_direct) { s->r_direct = 0; return 1; }
    if (!s->r_started) return 0;
    int ok = 1;
    for (int d = 0; d < DB_NQ; d++) { if (s->rth_on[d]) pthread_join(s->rth[d], 0); s->rth_on[d] = 0; ok = ok && s->rj[d].ok; }
    s->rx->n = s->n;
    s->t_read = mem_now() - s->r_t0; s->r_started = 0;
    return ok;
}
int spill_restore(spill *s, dbig *x) { return spill_restore_start(s, x) && spill_restore_wait(s); }
void spill_drop(spill *s)
{
    if (!s) return;
    if (s->r_started) spill_restore_wait(s);
    spill_wait(s);
    if (s->owned == 1) db_free(&s->x);                 /* never restored: its blocks go back */
    for (int d = 0; d < DB_NQ; d++) unlink(s->path[d]);
    free(s);
}
void spill_get_stats(const spill *s, struct spill_stats *st)
{
    memset(st, 0, sizeof *st); if (!s) return;
    st->bytes = s->bytes; st->t_write = s->joined ? s->t1 - s->t0 : mem_now() - s->t0; st->t_read = s->t_read;
    st->rate_w = st->t_write > 0 ? s->bytes / st->t_write : 0; st->rate_r = st->t_read > 0 ? s->bytes / st->t_read : 0;
}
