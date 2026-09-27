/* mn_out.c - per-node output and verification: see mn_out.h (Phase 9 A-out, PLAN.md 19 M5 + C1) */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <omp.h>
#include <hip/hip_runtime.h>
#include "mn_out.h"
#include "mem.h"
#include "spill.h"                                    /* Phase 14 S1 (E3): ECALC_ODIRECT */
#include "packed_fmt.h"                               /* Phase 15 IO (W2): ECALC_OUT_PACKED */
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)

/* ---- small collectives on host vectors, through comm_allgather (device buffers on APU 0) ---- */
void mn_out_allgather_u64(comm *c, const uint64_t *v, int k, uint64_t *out)
{
    if (!c || comm_size(c) == 1) { memcpy(out, v, (size_t)k * 8); return; }
    int n = comm_size(c), dev0; HIP_CHECK(hipGetDevice(&dev0)); HIP_CHECK(hipSetDevice(0));
    uint64_t *ds, *dr; HIP_CHECK(hipMalloc(&ds, (size_t)k * 8)); HIP_CHECK(hipMalloc(&dr, (size_t)n * k * 8));
    HIP_CHECK(hipMemcpy(ds, v, (size_t)k * 8, hipMemcpyHostToDevice));
    comm_allgather(c, ds, dr, (size_t)k * 8);
    HIP_CHECK(hipMemcpy(out, dr, (size_t)n * k * 8, hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(ds)); HIP_CHECK(hipFree(dr)); HIP_CHECK(hipSetDevice(dev0));
}
void mn_out_bcast_u64(comm *c, uint64_t *v, int k, int root)
{
    if (!c || comm_size(c) == 1) return;
    uint64_t *all = (uint64_t *)malloc((size_t)comm_size(c) * k * 8);
    mn_out_allgather_u64(c, v, k, all); memcpy(v, all + (size_t)root * k, (size_t)k * 8); free(all);
}
int mn_out_allreduce_or(comm *c, int v)
{
    if (!c || comm_size(c) == 1) return v;
    int n = comm_size(c); uint64_t x = (uint64_t)v, *all = (uint64_t *)malloc((size_t)n * 8);
    mn_out_allgather_u64(c, &x, 1, all); int r = 0; for (int i = 0; i < n; i++) r |= (int)all[i]; free(all); return r;
}
/* X mod q = sum over the nodes of res_r B^lo_r */
void mn_out_res_combine(comm *c, const uint64_t *res, size_t lo, uint64_t *out)
{
    uint64_t v[T1_NQ + 1]; for (int i = 0; i < T1_NQ; i++) v[i] = res[i]; v[T1_NQ] = lo;
    if (!c || comm_size(c) == 1) { for (int i = 0; i < T1_NQ; i++) out[i] = vf_shift_res(res[i], lo, t1_q[i]); return; }
    int n = comm_size(c); uint64_t *all = (uint64_t *)malloc((size_t)n * (T1_NQ + 1) * 8);
    mn_out_allgather_u64(c, v, T1_NQ + 1, all);
    for (int i = 0; i < T1_NQ; i++) { uint64_t s = 0; for (int r = 0; r < n; r++) s = vf_add_mod(s, vf_shift_res(all[(size_t)r * (T1_NQ + 1) + i], all[(size_t)r * (T1_NQ + 1) + T1_NQ], t1_q[i]), t1_q[i]); out[i] = s; }
    free(all);
}
/* the term ranges of the nodes in order (node 0 = the lowest terms): P = P_A Q_B + P_B, Q = Q_A Q_B */
void mn_out_pq_combine(comm *c, const uint64_t *p, const uint64_t *qq, uint64_t *P, uint64_t *Q)
{
    if (!c || comm_size(c) == 1) { memcpy(P, p, T1_NQ * 8); memcpy(Q, qq, T1_NQ * 8); return; }
    int n = comm_size(c); uint64_t v[2 * T1_NQ], *all = (uint64_t *)malloc((size_t)n * 2 * T1_NQ * 8);
    memcpy(v, p, T1_NQ * 8); memcpy(v + T1_NQ, qq, T1_NQ * 8);
    mn_out_allgather_u64(c, v, 2 * T1_NQ, all);
    for (int i = 0; i < T1_NQ; i++) {
        P[i] = all[i]; Q[i] = all[T1_NQ + i];
        for (int r = 1; r < n; r++) vf_pq_join(&P[i], &Q[i], all[(size_t)r * 2 * T1_NQ + i], all[(size_t)r * 2 * T1_NQ + T1_NQ + i], t1_q[i]);
    }
    free(all);
}
void mn_out_res_share(const mn_out_src *src, uint64_t *res)
{
    if (src->dev) { dbig v = *src->dev; v.n = src->cnt; db_mod_qs(&v, t1_q, T1_NQ, res); }
    else for (int i = 0; i < T1_NQ; i++) res[i] = vf_limbs_mod(src->host, src->cnt, t1_q[i]);
}
/* the stand-in for A-div's sharded X: node 0 scatters its host X (basis N = xn) over mesh 0 */
void mn_out_scatter_standin(comm *c, const uint64_t *X, size_t xn, dbig *share, size_t *lo, size_t *cnt, size_t *n_out)
{
    int n = c ? comm_size(c) : 1, me = c ? comm_rank(c) : 0;
    uint64_t v = xn;
    if (n > 1) { if (me == 0) for (int r = 1; r < n; r++) comm_send(c, r, &v, 8); else comm_recv(c, 0, &v, 8); }
    xn = v; *n_out = xn;
    size_t l, h; comm_shard(xn, me, n, &l, &h); *lo = l; *cnt = h - l;
    uint64_t *buf = 0;
    if (me == 0) {
        for (int r = 1; r < n; r++) { size_t rl, rh; comm_shard(xn, r, n, &rl, &rh); if (rh > rl) comm_send(c, r, X + rl, (rh - rl) * 8); }
        buf = (uint64_t *)X + l;
    } else { buf = (uint64_t *)malloc((h - l + 1) * 8); if (h > l) comm_recv(c, 0, buf, (h - l) * 8); }
    bigint view = { buf, h - l, 0 };
    db_init(share); db_from_bi(share, &view); share->n = h - l;
    if (me) free(buf);
}

/* ---- the digits ---- */
static inline void fmt18(char *o, uint64_t v)              /* 18 digits, zero-padded */
{
    uint64_t hi = v / 1000000000ULL, lo = v % 1000000000ULL;
    for (int i = 17; i >= 9; i--) { o[i] = (char)('0' + lo % 10); lo /= 10; }
    for (int i = 8; i >= 0; i--) { o[i] = (char)('0' + hi % 10); hi /= 10; }
}
/* limbs l[0..cnt) (l[cnt-1] the top) -> 18 cnt chars, the top limb first */
static void fmt_limbs(char *dst, const uint64_t *l, size_t cnt)
{
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < cnt; i++) fmt18(dst + (cnt - 1 - i) * 18, l[i]);
}
/* limbs [a, a+cnt) of a device number (a relative to it; views honoured) into a pinned host buffer.  Phase 10 H (B1): the
 * writer reads X on the device while the low product runs on the GPUs -- the copies go on a non-blocking stream per APU
 * (a null-stream hipMemcpy would serialise with every kernel of the product) */
static hipStream_t g_fs[DB_NQ]; static pthread_once_t g_fs_once = PTHREAD_ONCE_INIT;
static void fs_init(void) { int dev0; HIP_CHECK(hipGetDevice(&dev0)); for (int d = 0; d < DB_NQ; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipStreamCreateWithFlags(&g_fs[d], hipStreamNonBlocking)); } HIP_CHECK(hipSetDevice(dev0)); }
static void dev_fetch(const dbig *x, size_t a, size_t cnt, uint64_t *host)
{
    size_t g = x->off + a, ge = g + cnt; int dev0; HIP_CHECK(hipGetDevice(&dev0));
    pthread_once(&g_fs_once, fs_init);
    for (int d = 0; d < DB_NQ; d++) {
        size_t q0 = (size_t)d * x->qc, q1 = q0 + x->qc, s = g > q0 ? g : q0, e = ge < q1 ? ge : q1;
        if (s >= e) continue;
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMemcpyAsync(host + (s - g), x->q[d] + (s - q0), (e - s) * 8, hipMemcpyDeviceToHost, g_fs[d]));
        HIP_CHECK(hipStreamSynchronize(g_fs[d]));
    }
    HIP_CHECK(hipSetDevice(dev0));
}
/* the node's limb range [lo, hi) and digit range [k0, k1) */
static void ranges(const mn_out *o, const mn_out_src *src, size_t *nl, size_t *pad, size_t *lo, size_t *hi, size_t *k0, size_t *k1)
{
    *nl = (o->d + 1 + 17) / 18; *pad = *nl * 18 - (o->d + 1);
    *lo = src->lo; *hi = src->lo + src->cnt; if (*hi > *nl) *hi = *nl; if (*lo > *hi) *lo = *hi;
    if (o->rank == o->size - 1 && *hi < *nl) *hi = *nl;              /* the top node covers the leading limbs even if X is shorter (zeros) */
    if (*lo >= *hi) { *k0 = *k1 = 0; return; }
    size_t p0 = (*nl - *hi) * 18; *k0 = p0 > *pad ? p0 - *pad : 0; *k1 = (*nl - *lo) * 18 - *pad;
}
/* the T2 tails: every node's last min(49, its range) digits, all-gathered; this node's head = the tails of the
 * nodes above it (nearest first) cut to 49 */
void mn_out_boundaries(mn_out *o, const mn_out_src *src, comm *c)
{
    o->nhead = 0; o->c = c;                                       /* Phase 15 IO (W5b): the waves' barrier */
    if (!c || comm_size(c) == 1) return;
    size_t nl, pad, lo, hi, k0, k1; ranges(o, src, &nl, &pad, &lo, &hi, &k0, &k1);
    char tail[64]; size_t nt = 0;
    if (hi > lo) {
        size_t b = lo + 3 < hi ? lo + 3 : hi, cnt = b - lo; uint64_t l[3]; char s[54];
        size_t avail = src->cnt < cnt ? src->cnt : cnt; memset(l, 0, sizeof l);
        if (src->dev) { uint64_t *p; HIP_CHECK(hipHostMalloc((void **)&p, cnt * 8, 0)); if (avail) dev_fetch(src->dev, 0, avail, p); memcpy(l, p, avail * 8); HIP_CHECK(hipHostFree(p)); }
        else memcpy(l, src->host, avail * 8);
        for (size_t i = 0; i < cnt; i++) fmt18(s + (cnt - 1 - i) * 18, l[i]);
        size_t have = k1 - k0; nt = have < 49 ? have : 49;      /* the last nt chars of the range = the last nt of s */
        memcpy(tail, s + cnt * 18 - nt, nt);
    }
    uint64_t v[8]; memset(v, 0, sizeof v); v[0] = nt; memcpy(v + 1, tail, nt);
    int n = comm_size(c), me = comm_rank(c); uint64_t *all = (uint64_t *)malloc((size_t)n * 8 * 8);
    mn_out_allgather_u64(c, v, 8, all);
    char head[64 * 2]; size_t nh = 0;                             /* built from the nearest node above outwards */
    for (int r = me + 1; r < n && nh < 49; r++) {
        size_t t = (size_t)all[(size_t)r * 8]; if (!t) continue;
        memmove(head + t, head, nh); memcpy(head, (const char *)(all + (size_t)r * 8 + 1), t); nh += t;
    }
    if (nh > 49) { memmove(head, head + nh - 49, 49); nh = 49; }
    memcpy(o->head, head, nh); o->nhead = nh;
    free(all);
}

/* ---- the writer thread: one job at a time (a chunk of the part file), the formatter blocks only when both
 * buffers are in flight ----
 * Phase 15 IO (W1): the write mode (ECALC_OUT_MODE: direct / buffered / sync / drop / auto; unset = the Phase 14 rule) and
 * the number of pwrite threads per chunk (MN_OUT_THREADS, 8).  W2: the packed form (ECALC_OUT_PACKED=1, packed_fmt.h).
 * W5: the stripe layout (MN_OUT_STRIPE) and the waves (MN_OUT_WAVES).  Functions: out_mode, out_stripe, out_open,
 * direct_job, writer_run, writer_drain, out_packed, packed_mods, packed_digits, packed_hdr_*, out_run_one, mn_out_run
 * (the waves), mn_out_finish. */
enum { OUT_BUFFERED = 0, OUT_DIRECT, OUT_SYNC, OUT_DROP };
static const char *out_mode_name[] = { "buffered", "O_DIRECT", "buffered + fsync + DONTNEED at close", "buffered + fdatasync + DONTNEED per chunk" };
struct wjob { const char *data; size_t len; size_t off; int prefix, newline, buf; char first; };   /* prefix: "<first>." before the data (digit 0 and the point) */
struct writer {
    int fd; pthread_t th; sem_t job_ready, job_taken, buf_free[2]; struct wjob job; int stop; double t_write; size_t bytes; int err;
    char *buf[2]; uint64_t *lbuf; size_t bufsz; int dev_src;
    int direct; char *abuf; size_t ncarry; uint64_t apos;   /* Phase 14 S1 (E3): the O_DIRECT form -- an aligned staging copy, the < 4 KiB carry, the file offset of its first byte */
    int mode, nt, packed; char *hdr;                         /* Phase 15 IO: OUT_*, pwrite threads, the packed form and its header (aligned, ECP_HDR_BYTES) */
};
static void pwrite_all(int fd, const char *p, size_t n, size_t off, int *err)
{
    while (n) { ssize_t w = pwrite(fd, p, n, (off_t)off); if (w < 0) { if (errno == EINTR) continue; *err = errno; return; } p += w; n -= (size_t)w; off += (size_t)w; }
}
/* Phase 15 IO (W1, W7): the write mode of a part file.  ECALC_OUT_MODE=direct|buffered|sync|drop sets it; =auto takes the file
 * system's rule (spill.c sp_direct_by_fs: O_DIRECT on local disks and Lustre [assumed], else sync); unset: the Phase 14 rule
 * (O_DIRECT when ECALC_ODIRECT is on [ECALC_ODIRECT=auto: by the file system] and ECALC_OUT_ODIRECT is not 0, else buffered;
 * with ECALC_ODIRECT=auto the buffered form is sync) */
static int out_mode(const char *name)
{
    const char *m = getenv("ECALC_OUT_MODE");
    if (m && *m) {
        if (!strcmp(m, "direct")) return OUT_DIRECT;
        if (!strcmp(m, "buffered")) return OUT_BUFFERED;
        if (!strcmp(m, "sync")) return OUT_SYNC;
        if (!strcmp(m, "drop")) return OUT_DROP;
        if (!strcmp(m, "auto")) return sp_direct_by_fs(name) ? OUT_DIRECT : OUT_SYNC;
        fprintf(stderr, "mn_out: ECALC_OUT_MODE=%s: one of direct, buffered, sync, drop, auto (the default rule is used)\n", m);
    }
    if (sp_direct_for(name) && (!getenv("ECALC_OUT_ODIRECT") || atoi(getenv("ECALC_OUT_ODIRECT")))) return OUT_DIRECT;
    return sp_odirect_auto() ? OUT_SYNC : OUT_BUFFERED;
}
/* Phase 15 IO (W5a): MN_OUT_STRIPE=<count>[:<stripe MB>[:<OSTs>]] -- the part file created with that Lustre layout before it is
 * opened (a new file takes its directory's layout otherwise).  With <OSTs> (the file system's OST count) the first OST is
 * (part * count) mod OSTs, so the nodes' files start on distinct targets; without it Lustre's allocator places them.  Built
 * with -DMN_OUT_LLAPI (and -llustreapi) the layout is set by llapi_file_create, else by running `lfs setstripe`.  Only on a
 * Lustre file system (MN_OUT_STRIPE_FORCE=1: always -- the test of the command path off Lustre). */
#ifdef MN_OUT_LLAPI
#include <lustre/lustreapi.h>
#endif
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
static void out_stripe(const char *name, int part, int verbose)
{
    const char *e = getenv("MN_OUT_STRIPE"); if (!e || !*e) return;
    int force = getenv("MN_OUT_STRIPE_FORCE") && atoi(getenv("MN_OUT_STRIPE_FORCE"));
    int fs = sp_fs_kind(name);
    if (fs != SP_FS_LUSTRE && !force) { if (verbose || part == 0) printf("mn_out: MN_OUT_STRIPE=%s ignored: %s is on %s, not Lustre\n", e, name, sp_fs_name(fs)); return; }
    long cnt = atol(e), smb = 0, nost = 0; const char *p = strchr(e, ':');
    if (p) { smb = atol(p + 1); p = strchr(p + 1, ':'); if (p) nost = atol(p + 1); }
    if (cnt == 0) cnt = 1;
    long idx = nost > 0 ? ((long)part * (cnt > 0 ? cnt : 1)) % nost : -1;
    unlink(name);                                        /* a layout is set on a new file only */
    int rc;
#ifdef MN_OUT_LLAPI
    rc = llapi_file_create(name, (unsigned long long)smb << 20, (int)idx, (int)cnt, 0);
#else
    char c_s[32], s_s[32], i_s[32]; snprintf(c_s, sizeof c_s, "%ld", cnt); snprintf(s_s, sizeof s_s, "%ldM", smb); snprintf(i_s, sizeof i_s, "%ld", idx);
    char *argv[12]; int a = 0; argv[a++] = (char *)"lfs"; argv[a++] = (char *)"setstripe"; argv[a++] = (char *)"-c"; argv[a++] = c_s;
    if (smb > 0) { argv[a++] = (char *)"-S"; argv[a++] = s_s; }
    if (idx >= 0) { argv[a++] = (char *)"-i"; argv[a++] = i_s; }
    argv[a++] = (char *)name; argv[a] = 0;
    pid_t pid; rc = posix_spawnp(&pid, "lfs", 0, 0, argv, environ);
    if (rc == 0) { int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {} rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
#endif
    if (verbose || part == 0 || rc) printf("mn_out: %s: stripe count %ld, size %ld MB, first OST %ld (%s)\n", name, cnt, smb, idx, rc ? "FAILED: the file takes its directory's layout" : "set");
}
/* Phase 14 S1 (E3): one job in the O_DIRECT form.  The jobs arrive in file order (each chunk starts where the last ended),
 * so the bytes are appended to the aligned staging copy after the carry, the whole 4 KiB blocks written (8 threads, as
 * the buffered form), the rest carried; mn_out_finish writes the last block padded and truncates the file to size */
static void direct_job(struct writer *w, const struct wjob *j)
{
    if (w->err) return;
    if (j->off != w->apos + w->ncarry) { w->err = EINVAL; fprintf(stderr, "mn_out: O_DIRECT writer: a chunk at %zu, expected %llu\n", j->off, (unsigned long long)(w->apos + w->ncarry)); return; }
    char *p = w->abuf + w->ncarry;
    if (j->prefix) { p[0] = j->first; p[1] = '.'; p += 2; w->bytes += 2; }
    if (j->len) {
        const int NT = w->nt; size_t seg = (j->len + NT - 1) / NT;
#pragma omp parallel for num_threads(NT) schedule(static)
        for (int t = 0; t < NT; t++) { size_t s = (size_t)t * seg; if (s < j->len) memcpy(p + s, j->data + s, j->len - s < seg ? j->len - s : seg); }
        p += j->len; w->bytes += j->len;
    }
    if (j->newline) { *p++ = '\n'; w->bytes++; }
    size_t tot = (size_t)(p - w->abuf), aw = tot & ~(size_t)(SP_ALIGN - 1);
    if (aw) {
        const int NT = w->nt; size_t seg = ((aw / NT) + SP_ALIGN - 1) & ~(size_t)(SP_ALIGN - 1);
#pragma omp parallel for num_threads(NT) schedule(static)
        for (int t = 0; t < NT; t++) { size_t s = (size_t)t * seg; if (s < aw) { size_t n = aw - s < seg ? aw - s : seg; pwrite_all(w->fd, w->abuf + s, n, w->apos + s, &w->err); } }
    }
    memmove(w->abuf, w->abuf + aw, tot - aw); w->ncarry = tot - aw; w->apos += aw;
}
static void *writer_run(void *a)
{
    struct writer *w = (struct writer *)a;
    for (;;) {
        sem_wait(&w->job_ready);
        if (w->stop) break;
        struct wjob j = w->job; sem_post(&w->job_taken);
        if (w->direct) { double t0 = mem_now(); direct_job(w, &j); w->t_write += mem_now() - t0; sem_post(&w->buf_free[j.buf]); continue; }
        double t0 = mem_now(); size_t off = j.off, off0 = off;
        if (j.prefix) { char pf[2] = { j.first, '.' }; pwrite_all(w->fd, pf, 2, off, &w->err); off += 2; w->bytes += 2; }
        if (j.len) {
            const int NT = w->nt; size_t seg = (j.len + NT - 1) / NT; seg = (seg + 4095) & ~(size_t)4095;
#pragma omp parallel for num_threads(NT) schedule(static)
            for (int t = 0; t < NT; t++) { size_t s = (size_t)t * seg; if (s < j.len) { size_t n = j.len - s < seg ? j.len - s : seg; pwrite_all(w->fd, j.data + s, n, off + s, &w->err); } }
            off += j.len; w->bytes += j.len;
        }
        if (j.newline) { pwrite_all(w->fd, "\n", 1, off, &w->err); w->bytes++; off++; }
        if (w->mode == OUT_DROP && !w->err) {            /* Phase 15 IO (W1): the chunk on the server, then out of the page cache (HBM on the APU) */
            if (fdatasync(w->fd) != 0) w->err = errno;
            posix_fadvise(w->fd, (off_t)off0, (off_t)(off - off0), POSIX_FADV_DONTNEED);
        }
        w->t_write += mem_now() - t0;
        sem_post(&w->buf_free[j.buf]);
    }
    return 0;
}
/* Phase 15 IO (W5b): every job handed to the writer is on disk (the carry of the O_DIRECT form excepted: < 4 KiB, written at
 * finish); the buffered forms also fdatasync, so a wave's data has left the node before the next wave starts */
static void writer_drain(struct writer *w)
{
    for (int b = 0; b < 2; b++) sem_wait(&w->buf_free[b]);
    if (!w->direct && w->fd >= 0 && fdatasync(w->fd) != 0 && !w->err) w->err = errno;
    for (int b = 0; b < 2; b++) sem_post(&w->buf_free[b]);
}

/* ---- Phase 15 IO (W2): the packed form (packed_fmt.h) ---- */
static int out_packed(void) { static int v = -1; if (v < 0) { const char *e = getenv("ECALC_OUT_PACKED"); v = e && atoi(e); } return v; }
static uint64_t mulmod_u(uint64_t a, uint64_t b, uint64_t q) { return (uint64_t)(((unsigned __int128)a * b) % q); }
static uint64_t powmod_u(uint64_t b, uint64_t e, uint64_t q) { uint64_t r = 1 % q; b %= q; while (e) { if (e & 1) r = mulmod_u(r, b, q); b = mulmod_u(b, b, q); e >>= 1; } return r; }
/* the value of n limbs stored most significant first (base 10^18) mod the T1 primes, from the bytes the writer is handed */
static void packed_mods(const uint64_t *l, size_t n, uint64_t *out)
{
    const uint64_t B = 1000000000000000000ULL;
    for (int j = 0; j < T1_NQ; j++) out[j] = 0;
    if (!n) return;
    int T = omp_get_max_threads(); if ((size_t)T > n / 1024 + 1) T = (int)(n / 1024 + 1);
    uint64_t *cv = (uint64_t *)malloc((size_t)T * T1_NQ * 8);
#pragma omp parallel for num_threads(T) schedule(static)
    for (int t = 0; t < T; t++) {
        size_t a = n * t / T, e = n * (t + 1) / T; uint64_t v[T1_NQ], b[T1_NQ];
        for (int j = 0; j < T1_NQ; j++) { v[j] = 0; b[j] = B % t1_q[j]; }
        for (size_t i = a; i < e; i++) for (int j = 0; j < T1_NQ; j++) v[j] = (uint64_t)(((unsigned __int128)v[j] * b[j] + l[i]) % t1_q[j]);
        for (int j = 0; j < T1_NQ; j++) cv[(size_t)t * T1_NQ + j] = v[j];
    }
    for (int j = 0; j < T1_NQ; j++) {
        uint64_t r = 0, bq = B % t1_q[j];
        for (int t = 0; t < T; t++) { size_t a = n * t / T, e = n * (t + 1) / T; r = (mulmod_u(r, powmod_u(bq, e - a, t1_q[j]), t1_q[j]) + cv[(size_t)t * T1_NQ + j]) % t1_q[j]; }
        out[j] = r;
    }
    free(cv);
}
/* the digits [x, y) of a chunk formatted from its limbs alone (l[0] = global limb a; digit k is char k + pad of the string,
 * in global limb nl-1-(k+pad)/18): T2, the head, the first / last digits -- no ASCII copy of the chunk */
static void packed_digits(const uint64_t *l, size_t a, size_t nl, size_t pad, size_t x, size_t y, char *out)
{
    char t[18]; size_t cur = (size_t)-1;
    for (size_t k = x; k < y; k++) {
        size_t c = k + pad, i = nl - 1 - c / 18;
        if (i != cur) { fmt18(t, l[i - a]); cur = i; }
        out[k - x] = t[c % 18];
    }
}
static void packed_hdr_write(struct writer *w, const mn_out *o, size_t nl, size_t pad, size_t lo, size_t hi)
{
    char *h = w->hdr; memset(h, 0, ECP_HDR_BYTES);
    ecp_hdr *p = (ecp_hdr *)h;
    memcpy(p->magic, ECP_MAGIC, 8); p->version = ECP_VERSION; p->hdr_bytes = ECP_HDR_BYTES; p->endian = ECP_ENDIAN;
    p->limb_bytes = 8; p->limb_digits = 18; p->order = 1; p->part = (uint32_t)(o->size > 1 ? o->size - 1 - o->rank : 0); p->nparts = (uint32_t)(o->size > 1 ? o->size : 1);
    p->d = o->d; p->d_out = o->d_out; p->nl = nl; p->pad = pad; p->lo = lo; p->hi = hi; p->k0 = o->k0; p->k1 = o->k1; p->complete = 0;
    p->nq = T1_NQ; for (int i = 0; i < T1_NQ; i++) p->q[i] = t1_q[i];
    snprintf(h + ECP_TEXT_OFF, ECP_HDR_BYTES - ECP_TEXT_OFF,
             "ecalc packed digits of e, format v%u: after this %u-byte header, the base-10^18 limbs [%zu, %zu) of X (of %zu),\n"
             "8 bytes each, little-endian, the most significant limb first; part %u of %u; the digits [%zu, %zu) of %lu computed;\n"
             "the ASCII file is \"2.\" + digits 1..%lu + a newline: convert with tools/unpack_digits.\n",
             ECP_VERSION, ECP_HDR_BYTES, lo, hi, nl, p->part, p->nparts, o->k0, o->k1, o->d, o->d_out);
    if (w->fd >= 0) pwrite_all(w->fd, h, ECP_HDR_BYTES, 0, &w->err);
}

/* the file of this node's range: the name, the stripe layout, the mode (W1/W7) */
static void out_open(struct writer *w, const mn_out *o)
{
    char name[4096];
    if (o->size > 1) snprintf(name, sizeof name, "%s.part%04d", o->outfile, o->size - 1 - o->rank); else snprintf(name, sizeof name, "%s", o->outfile);
    out_stripe(name, o->size > 1 ? o->size - 1 - o->rank : 0, o->verbose);
    w->mode = out_mode(name);
    if (w->mode == OUT_DIRECT) {                         /* Phase 14 S1 (E3) */
        w->fd = open(name, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
        if (w->fd >= 0) w->direct = 1; else w->mode = OUT_BUFFERED;
    }
    if (w->fd < 0) w->fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (w->fd < 0) { printf("mn_out: cannot open %s: %s\n", name, strerror(errno)); return; }
    if ((o->verbose || getenv("ECALC_OUT_MODE") || getenv("MN_OUT_THREADS") || w->packed || sp_odirect_auto()) && (o->rank == 0 || o->verbose))
        printf("mn_out: %s: %s, %s form, %d write threads, file system %s\n", name, out_mode_name[w->mode], w->packed ? "packed" : "ASCII", w->nt, sp_fs_name(sp_fs_kind(name)));
}
static int out_run_one(mn_out *o, const mn_out_src *src)
{
    size_t nl, pad, lo, hi, k0, k1; ranges(o, src, &nl, &pad, &lo, &hi, &k0, &k1);
    o->k0 = k0; o->k1 = k1; o->ndig = 0; o->bad2 = o->nwin = 0; o->bytes = 0; o->nchunks = 0; o->t_fmt = o->t_res = o->t_t2 = o->t_write = o->t_fetch = o->t_wait = 0;
    o->first[0] = o->last[0] = 0; o->ntail = 0;
    for (int i = 0; i < T1_NQ; i++) o->dres[i] = 0;
    size_t L = o->chunk_limbs; if (!L) { size_t mb = getenv("MN_OUT_CHUNK_MB") ? (size_t)atoi(getenv("MN_OUT_CHUNK_MB")) : 256; L = (mb << 20) / 18; if (L < 64) L = 64; }
    if (L > hi - lo && hi > lo) L = hi - lo;
    struct writer *w = (struct writer *)calloc(1, sizeof *w); o->priv = w; w->fd = -1; w->dev_src = src->dev != 0;
    w->nt = getenv("MN_OUT_THREADS") ? atoi(getenv("MN_OUT_THREADS")) : 8; if (w->nt < 1) w->nt = 1; if (w->nt > 64) w->nt = 64;   /* Phase 15 IO (W1) */
    w->packed = out_packed(); o->packed = w->packed;
    if (o->outfile) out_open(w, o);
    sem_init(&w->job_ready, 0, 0); sem_init(&w->job_taken, 0, 0); sem_init(&w->buf_free[0], 0, 1); sem_init(&w->buf_free[1], 0, 1);
    w->bufsz = L * 18 + 64;
    if (w->direct && posix_memalign((void **)&w->abuf, (size_t)2 << 20, (w->bufsz + 3 * SP_ALIGN) & ~(size_t)(SP_ALIGN - 1))) { ec_fatal(EC_RC_OOM, "mn_out: %zu bytes\n", w->bufsz); }
    for (int b = 0; b < 2; b++) if (posix_memalign((void **)&w->buf[b], 2u << 20, w->bufsz)) { ec_fatal(EC_RC_OOM, "mn_out: %zu bytes\n", w->bufsz); }
    if (src->dev) HIP_CHECK(hipHostMalloc((void **)&w->lbuf, L * 8, 0)); else w->lbuf = (uint64_t *)malloc(L * 8);
    if (w->packed) {                                   /* Phase 15 IO (W2): the header now (complete = 0), completed by mn_out_finish */
        if (posix_memalign((void **)&w->hdr, SP_ALIGN, ECP_HDR_BYTES)) ec_fatal(EC_RC_OOM, "mn_out: header\n");
        packed_hdr_write(w, o, nl, pad, lo, hi); w->apos = ECP_HDR_BYTES;
        w->bytes = w->fd >= 0 ? ECP_HDR_BYTES : 0;
    }
    pthread_create(&w->th, 0, writer_run, w);
    size_t fbase = k0 == 0 ? 0 : k0 + 1, kw_end = o->d_out + 1;      /* the part's first byte in the file; digits < kw_end are written */
    char head[64]; size_t nhead = o->nhead; memcpy(head, o->head, nhead);
    unsigned long woff[256]; int nwo = 0;
    if (w->packed) { nwo = tier2_window_offsets(woff, 256); if (nwo > 256) nwo = 256; }
    int b = 0;
    for (size_t bb = hi; bb > lo; b ^= 1) {
        size_t a = bb - lo > L ? bb - L : lo, cnt = bb - a;
        double t0 = mem_now(); sem_wait(&w->buf_free[b]); double t1 = mem_now(); o->t_wait += t1 - t0;
        const uint64_t *l; size_t avail = src->lo + src->cnt > a ? src->lo + src->cnt - a : 0; if (avail > cnt) avail = cnt;   /* limbs beyond the source are zero */
        if (src->dev || avail < cnt) { if (avail) { if (src->dev) dev_fetch(src->dev, a - src->lo, avail, w->lbuf); else memcpy(w->lbuf, src->host + (a - src->lo), avail * 8); } memset(w->lbuf + avail, 0, (cnt - avail) * 8); l = w->lbuf; }
        else l = src->host + (a - src->lo);
        double t2 = mem_now(); o->t_fetch += t2 - t1;
        size_t p0 = (nl - bb) * 18, ck0 = p0 > pad ? p0 - pad : 0, ck1 = (nl - a) * 18 - pad;   /* the chunk's digits */
        size_t len = ck1 - ck0;
        if (w->packed) {                               /* Phase 15 IO (W2): the limbs, most significant first; the checks from them */
            uint64_t *pk = (uint64_t *)w->buf[b];
#pragma omp parallel for schedule(static)
            for (size_t i = 0; i < cnt; i++) pk[i] = l[cnt - 1 - i];
            double t3 = mem_now(); o->t_fmt += t3 - t2;
            uint64_t v[T1_NQ]; packed_mods(pk, cnt, v);   /* the digits' value = the limbs' (the top limb's pad zeros lead) */
            for (int i = 0; i < T1_NQ; i++) o->dres[i] = vf_digits_join(o->dres[i], len, v[i], t1_q[i]);
            o->ndig += len;
            double t4 = mem_now(); o->t_res += t4 - t3;
            /* T2: the windows that end in this chunk (as tier2_range picks them), from the digits they cover and the head */
            size_t ndig = o->d_out + 1, x = (size_t)-1, y = 0;
            for (int i = 0; i < nwo; i++) { size_t e = woff[i] + 50; if (e > ndig || e <= ck0 || e > ck1 || woff[i] + nhead < ck0) continue; if (woff[i] < x) x = woff[i]; if (e > y) y = e; }
            if (y) {
                size_t x0 = x > ck0 + 49 ? x - 49 : ck0;   /* 49 digits before the first window (tier2_range's head), inside the chunk when they are */
                char *sub = (char *)malloc(y - x0 + 1); packed_digits(l, a, nl, pad, x0, y, sub);
                o->bad2 += tier2_range(sub, x0, y, head, x0 == ck0 ? nhead : 0, ndig, o->verbose, &o->nwin);
                free(sub);
            }
            { size_t t = len < 49 ? len : 49; char tl[49]; packed_digits(l, a, nl, pad, ck1 - t, ck1, tl);
              if (t < 49 && nhead) { size_t keep = 49 - t < nhead ? 49 - t : nhead; memmove(head, head + nhead - keep, keep); memcpy(head + keep, tl, t); nhead = keep + t; } else { memcpy(head, tl, t); nhead = t; } }
            o->t_t2 += mem_now() - t4;
            if (bb == hi) { size_t f = len < 62 ? len : 62; packed_digits(l, a, nl, pad, ck0, ck0 + f, o->first); o->first[f] = 0; }
            if (ck0 <= o->d_out && o->d_out < ck1) { size_t e = o->d_out + 1 - ck0, t = e < 20 ? e : 20; packed_digits(l, a, nl, pad, o->d_out + 1 - t, o->d_out + 1, o->last); o->last[t] = 0; }
            if (o->d > o->d_out && ck0 <= o->d_out + 1 && ck1 == o->d + 1 && o->d - o->d_out < sizeof o->tail) { o->ntail = o->d - o->d_out; packed_digits(l, a, nl, pad, o->d_out + 1, o->d + 1, o->tail); }
            if (w->fd >= 0) {
                struct wjob j; memset(&j, 0, sizeof j); j.buf = b; j.data = (const char *)pk; j.len = cnt * 8; j.off = ECP_HDR_BYTES + (hi - bb) * 8;
                double tw = mem_now(); w->job = j; sem_post(&w->job_ready); sem_wait(&w->job_taken); o->t_wait += mem_now() - tw;
            } else sem_post(&w->buf_free[b]);
            o->nchunks++;
            bb = a;
            continue;
        }
        char *buf = w->buf[b]; fmt_limbs(buf, l, cnt);
        const char *s = buf + (p0 > pad ? 0 : pad - p0);
        double t3 = mem_now(); o->t_fmt += t3 - t2;
        uint64_t v[T1_NQ]; vf_digits_mods(s, len, t1_q, T1_NQ, v);
        for (int i = 0; i < T1_NQ; i++) o->dres[i] = vf_digits_join(o->dres[i], len, v[i], t1_q[i]);
        o->ndig += len;
        double t4 = mem_now(); o->t_res += t4 - t3;
        o->bad2 += tier2_range(s, ck0, ck1, head, nhead, o->d_out + 1, o->verbose, &o->nwin);
        { size_t t = len < 49 ? len : 49; if (t < 49 && nhead) { size_t keep = 49 - t < nhead ? 49 - t : nhead; memmove(head, head + nhead - keep, keep); memcpy(head + keep, s, t); nhead = keep + t; } else { memcpy(head, s + len - t, t); nhead = t; } }
        o->t_t2 += mem_now() - t4;
        if (bb == hi) { size_t f = len < 62 ? len : 62; memcpy(o->first, s, f); o->first[f] = 0; }
        if (ck0 <= o->d_out && o->d_out < ck1) { size_t e = o->d_out + 1 - ck0, t = e < 20 ? e : 20; memcpy(o->last, s + e - t, t); o->last[t] = 0; }   /* the 20 digits ending at d_out */
        if (o->d > o->d_out && ck0 <= o->d_out + 1 && ck1 == o->d + 1 && o->d - o->d_out < sizeof o->tail) { o->ntail = o->d - o->d_out; memcpy(o->tail, s + (o->d_out + 1 - ck0), o->ntail); }   /* V: the computed digits after d_out (inside the last limb) */
        /* the write: digits [ck0, min(ck1, kw_end)); "2." before digit 0, the newline after digit d_out */
        size_t we = ck1 < kw_end ? ck1 : kw_end;
        if (w->fd >= 0 && ck0 < we) {
            struct wjob j; j.buf = b; j.prefix = ck0 == 0; j.first = s[0]; j.data = s + j.prefix; j.len = we - ck0 - j.prefix; j.off = (j.prefix ? 0 : ck0 + 1) - fbase; j.newline = ck0 <= o->d_out && o->d_out < ck1;
            double tw = mem_now(); w->job = j; sem_post(&w->job_ready); sem_wait(&w->job_taken); o->t_wait += mem_now() - tw;   /* the writer still busy with the previous chunk */
        } else sem_post(&w->buf_free[b]);
        o->nchunks++;
        bb = a;
    }
    return 0;
}
/* Phase 15 IO (W5b): MN_OUT_WAVES=<n> at size > 1 (mn_out_boundaries gave the communicator) -- the ranks in waves of
 * ceil(size/n) in rank order: a wave formats, checks and writes its ranges completely (writer_drain) while the others wait
 * at the barrier (an all-gather) that ends the wave; the same number of barriers on every rank.  Unset or 1: every node at
 * once, as before. */
int mn_out_run(mn_out *o, const mn_out_src *src)
{
    int W = getenv("MN_OUT_WAVES") ? atoi(getenv("MN_OUT_WAVES")) : 1;
    o->t_wave = o->t_wave_own = 0; o->wave = 0; o->nwaves = 1;
    if (W <= 1 || o->size <= 1 || !o->c) return out_run_one(o, src);
    int per = (o->size + W - 1) / W, nw = (o->size + per - 1) / per, my = o->rank / per, rc = 0;
    o->wave = my; o->nwaves = nw;
    for (int wv = 0; wv < nw; wv++) {
        double t0 = mem_now();
        if (wv == my) { rc = out_run_one(o, src); writer_drain((struct writer *)o->priv); o->t_wave_own = mem_now() - t0; }
        double tb = mem_now(); mn_out_allreduce_or(o->c, 0);
        o->t_wave += wv == my ? mem_now() - tb : mem_now() - t0;
    }
    printf("mn: node %d: MN_OUT_WAVES=%d: wave %d of %d (%d ranks each): wrote in %.2f s, waited %.2f s\n", o->rank, W, my, nw, per, o->t_wave_own, o->t_wave);
    return rc;
}
void mn_out_finish(mn_out *o)
{
    struct writer *w = (struct writer *)o->priv; if (!w) return;
    w->stop = 1; sem_post(&w->job_ready); pthread_join(w->th, 0);
    if (w->direct && w->fd >= 0) {                     /* Phase 14 S1 (E3): the last block, padded, then the file cut to size */
        double t0 = mem_now();
        if (w->ncarry && !w->err) {
            size_t pad = (w->ncarry + SP_ALIGN - 1) & ~(size_t)(SP_ALIGN - 1); memset(w->abuf + w->ncarry, 0, pad - w->ncarry);
            pwrite_all(w->fd, w->abuf, pad, w->apos, &w->err);
            if (!w->err && ftruncate(w->fd, (off_t)(w->apos + w->ncarry)) != 0) w->err = errno;
        }
        w->t_write += mem_now() - t0;
    }
    if (w->packed && w->fd >= 0 && !w->err) {          /* Phase 15 IO (W2): the header with the residues, complete = 1 */
        ecp_hdr *h = (ecp_hdr *)w->hdr; h->complete = 1; for (int i = 0; i < T1_NQ; i++) h->dres[i] = o->dres[i];
        pwrite_all(w->fd, w->hdr, ECP_HDR_BYTES, 0, &w->err);
    }
    if (w->fd >= 0 && (w->direct || w->mode == OUT_SYNC || w->mode == OUT_DROP)) {   /* on the server; the buffered forms: out of the page cache */
        double t0 = mem_now();
        if (!w->err && fsync(w->fd) != 0) w->err = errno;
        if (!w->direct) posix_fadvise(w->fd, 0, 0, POSIX_FADV_DONTNEED);
        w->t_write += mem_now() - t0;
    }
    if (w->fd >= 0) close(w->fd);
    free(w->abuf); free(w->hdr);
    if (w->err) printf("mn_out: write error: %s\n", strerror(w->err));
    o->t_write = w->t_write; o->bytes = w->bytes;
    for (int b = 0; b < 2; b++) free(w->buf[b]);
    if (w->lbuf) { if (w->dev_src) HIP_CHECK(hipHostFree(w->lbuf)); else free(w->lbuf); }
    sem_destroy(&w->job_ready); sem_destroy(&w->job_taken); sem_destroy(&w->buf_free[0]); sem_destroy(&w->buf_free[1]);
    free(w); o->priv = 0;
}
/* the whole string's residue from the nodes' pieces, top node first: D = D 10^ndig_r + dres_r */
void mn_out_digit_res(const mn_out *o, comm *c, uint64_t *Dres)
{
    if (!c || comm_size(c) == 1) { memcpy(Dres, o->dres, T1_NQ * 8); return; }
    int n = comm_size(c); uint64_t v[T1_NQ + 1], *all = (uint64_t *)malloc((size_t)n * (T1_NQ + 1) * 8);
    memcpy(v, o->dres, T1_NQ * 8); v[T1_NQ] = o->ndig;
    mn_out_allgather_u64(c, v, T1_NQ + 1, all);
    for (int i = 0; i < T1_NQ; i++) { uint64_t D = 0; for (int r = n; r-- > 0;) D = vf_digits_join(D, (size_t)all[(size_t)r * (T1_NQ + 1) + T1_NQ], all[(size_t)r * (T1_NQ + 1) + i], t1_q[i]); Dres[i] = D; }
    free(all);
}

/* ---- Phase 11 V: the sidecar and the recheck mode (mn_out.h) ---- */
#include "binsplit.h"
#include "mdb.h"
#include <sys/stat.h>
const char *mn_out_ckpt_default(const char *outfile)
{
    if (!outfile) return 0;
    size_t n = strlen(outfile); char *d = (char *)malloc(n + 5); memcpy(d, outfile, n); memcpy(d + n, ".top", 5); return d;
}
void mn_out_sidecar_write(const char *outfile, unsigned long N, unsigned long d, unsigned long d_out, int size, const uint64_t *Xres, const uint64_t *Rres, const uint64_t *Pres, const uint64_t *Qres, const char *tail, size_t ntail)
{
    char name[4096]; snprintf(name, sizeof name, "%s.t1", outfile);
    FILE *f = fopen(name, "w"); if (!f) { printf("mn_out: cannot write %s: %s\n", name, strerror(errno)); return; }
    fprintf(f, "ecalc-t1 v1\nN %lu d %lu d_out %lu size %d base %s\nq", N, d, d_out, size, bi_decimal ? "10^18" : "2^64");
    for (int i = 0; i < T1_NQ; i++) fprintf(f, " %llu", (unsigned long long)t1_q[i]);
    const char *nm[4] = { "X", "R", "P", "Q" }; const uint64_t *rs[4] = { Xres, Rres, Pres, Qres };
    for (int k = 0; k < 4; k++) { fprintf(f, "\n%s", nm[k]); for (int i = 0; i < T1_NQ; i++) fprintf(f, " %llu", (unsigned long long)rs[k][i]); }
    fprintf(f, "\ntail %zu %.*s\n", ntail, (int)ntail, tail);
    fclose(f);
}
/* Phase 12 W: the digit file's count and last 49 chars without a pass over it (stat + one read at the end): the counts and
 * tails are all-gathered before the pass, so one read of the file serves both the residues and the windows */
/* Phase 15 IO (W2): a packed part (packed_fmt.h) -- its header, 1 when the file is one (and complete and of this build's primes) */
static int recheck_packed_hdr(const char *name, ecp_hdr *h)
{
    FILE *f = fopen(name, "rb"); if (!f) return 0;
    int ok = fread(h, sizeof *h, 1, f) == 1 && !memcmp(h->magic, ECP_MAGIC, 8); fclose(f);
    if (!ok) return 0;
    if (h->version != ECP_VERSION || h->endian != ECP_ENDIAN || h->limb_bytes != 8 || h->order != 1) { printf("recheck: %s: a packed file of another version or byte order\n", name); return -1; }
    if (!h->complete) { printf("recheck: %s: the packed file was not closed by its run (complete = 0)\n", name); return -1; }
    return 1;
}
/* the digits [x, y) of a packed part (a few limbs read at their offsets) */
static int recheck_packed_digits(const char *name, const ecp_hdr *h, size_t x, size_t y, char *out)
{
    if (x >= y) return 1;
    size_t ia = h->nl - 1 - (x + h->pad) / 18, ib = h->nl - 1 - (y - 1 + h->pad) / 18;   /* the limbs, ia >= ib */
    size_t n = ia - ib + 1; uint64_t *l = (uint64_t *)malloc(n * 8);
    FILE *f = fopen(name, "rb"); int ok = f && fseeko(f, (off_t)(ECP_HDR_BYTES + (h->hi - 1 - ia) * 8), SEEK_SET) == 0 && fread(l, 8, n, f) == n;
    if (f) fclose(f);
    if (ok) for (size_t k = x; k < y; k++) { size_t c = k + h->pad, i = h->nl - 1 - c / 18; char t[18]; fmt18(t, l[ia - i]); out[k - x] = t[c % 18]; }
    free(l);
    return ok;
}
static int recheck_stat(const char *name, int first_part, int last_part, size_t *ndig, char *tail49, size_t *ntail49)
{
    { ecp_hdr h; int pk = recheck_packed_hdr(name, &h);   /* Phase 15 IO (W2) */
      if (pk < 0) return 0;
      if (pk) {
          size_t e = h.k1 < h.d_out + 1 ? h.k1 : h.d_out + 1, n = e > h.k0 ? e - h.k0 : 0, t = n < 49 ? n : 49;
          *ndig = n; *ntail49 = t;
          if (!recheck_packed_digits(name, &h, e - t, e, tail49)) { printf("recheck: cannot read %s\n", name); return 0; }
          return 1;
      } }
    struct stat st; if (stat(name, &st) != 0) { printf("recheck: cannot stat %s: %s\n", name, strerror(errno)); return 0; }
    FILE *f = fopen(name, "rb"); if (!f) { printf("recheck: cannot open %s: %s\n", name, strerror(errno)); return 0; }
    size_t sz = (size_t)st.st_size, n = sz, nl = 0;
    if (last_part) { char e[4]; size_t t = sz < 4 ? sz : 4; fseeko(f, (off_t)(sz - t), SEEK_SET); if (fread(e, 1, t, f) != t) { fclose(f); return 0; } while (nl < t && (e[t - 1 - nl] == '\n' || e[t - 1 - nl] == '\r')) nl++; n -= nl; }
    if (first_part && n >= 2) n--;                               /* the '.' after the leading digit */
    size_t want = 49 + nl + 1, t = want < sz ? want : sz; char b[64]; fseeko(f, (off_t)(sz - t), SEEK_SET);
    if (fread(b, 1, t, f) != t) { fclose(f); return 0; } fclose(f);
    size_t e = t - nl; for (size_t q = 0; q < e; q++) if (b[q] == '.') { memmove(b + q, b + q + 1, e - q - 1); e--; break; }   /* (a tiny first part: the '.' is not a digit) */
    size_t k = e < 49 ? e : 49; memcpy(tail49, b + e - k, k); *ntail49 = k; *ndig = n;
    return 1;
}
/* one pass over this node's digit file in chunks: the residues (Horner), the T2 windows (the head carried: the node above's
 * tail first), the count of digit chars (the '.' and the newline are not digits); returns 0 when the file cannot be read.
 * A reader thread fills the other of two 256 MB buffers while a chunk is processed (the file is the recheck's time: 40 GB
 * at 4e10), and the non-digit scan runs over the OpenMP team. */
struct recheck_rd { FILE *f; char *buf[2]; size_t have[2]; int last[2]; sem_t full[2], empty[2]; size_t CH; volatile int stop;
                    int packed; ecp_hdr h; uint64_t *lb; size_t pos; };   /* Phase 15 IO (W2): a packed part -- its header, a limb buffer, the next limb (file order) */
/* Phase 15 IO (W2): the next chunk of a packed part as ASCII digits (those in [k0, min(k1, d_out + 1)): no pad, no tail) */
static void *recheck_rd_packed(struct recheck_rd *r)
{
    const ecp_hdr *h = &r->h; size_t nlimbs = h->hi - h->lo, CHL = r->CH / 18, e = h->k1 < h->d_out + 1 ? h->k1 : h->d_out + 1;
    if (fseeko(r->f, ECP_HDR_BYTES, SEEK_SET) != 0) nlimbs = 0;
    for (int k = 0;; k ^= 1) {
        sem_wait(&r->empty[k]); if (r->stop) return 0;
        size_t m = nlimbs - r->pos < CHL ? nlimbs - r->pos : CHL, got = m ? fread(r->lb, 8, m, r->f) : 0, n = 0;
        if (got == m && m) {
            char *b = r->buf[k];
#pragma omp parallel for schedule(static)
            for (size_t j = 0; j < m; j++) fmt18(b + 18 * j, r->lb[j]);
            size_t c0 = 18 * (h->nl - h->hi + r->pos), c1 = c0 + 18 * m;          /* the chunk's chars; digit = char - pad */
            size_t x = c0 > h->pad ? c0 - h->pad : 0, y = c1 - h->pad; if (x < h->k0) x = h->k0; if (y > e) y = e;
            if (y > x) { n = y - x; memmove(b, b + (x + h->pad - c0), n); }
        }
        r->pos += m;
        int last = got != m || r->pos >= nlimbs;
        r->have[k] = n; r->last[k] = last; sem_post(&r->full[k]);
        if (last) return 0;
    }
}
static void *recheck_rd_run(void *a)
{
    struct recheck_rd *r = (struct recheck_rd *)a;
    if (r->packed) return recheck_rd_packed(r);
    for (int k = 0;; k ^= 1) {
        sem_wait(&r->empty[k]); if (r->stop) return 0;
        size_t n = fread(r->buf[k], 1, r->CH, r->f); int ch = fgetc(r->f), last = ch == EOF; if (!last) ungetc(ch, r->f);
        r->have[k] = n; r->last[k] = last; sem_post(&r->full[k]);
        if (last || !n) return 0;
    }
}
static int recheck_pass(const char *name, int first_part, uint64_t *dres, size_t *ndig, size_t k0, const char *head0, size_t nhead0, size_t ndig_all, int verbose, int *nwin, int *bad2)
{
    struct recheck_rd r; memset(&r, 0, sizeof r);
    r.f = fopen(name, "rb"); if (!r.f) { printf("recheck: cannot open %s: %s\n", name, strerror(errno)); return 0; }
    r.CH = (size_t)256 << 20; for (int k = 0; k < 2; k++) { r.buf[k] = (char *)malloc(r.CH + 64); sem_init(&r.full[k], 0, 0); sem_init(&r.empty[k], 0, 1); }
    if (recheck_packed_hdr(name, &r.h) == 1) { r.packed = 1; r.lb = (uint64_t *)malloc(r.CH / 18 * 8); first_part = 0; }   /* Phase 15 IO (W2): no '.' in a packed part */
    pthread_t th; pthread_create(&th, 0, recheck_rd_run, &r);
    size_t n = 0, k = k0; char head[64]; size_t nhead = nhead0; memcpy(head, head0, nhead0); int first = first_part, ok = 1;
    for (int i = 0; i < T1_NQ; i++) dres[i] = 0;
    *bad2 = 0;
    for (int b = 0;; b ^= 1) {
        sem_wait(&r.full[b]); size_t len = r.have[b]; int last = r.last[b]; char *sp = r.buf[b];
        if (!len) break;
        if (first) { if (len >= 2 && sp[1] == '.') { memmove(sp + 1, sp + 2, len - 2); len--; } first = 0; }   /* the '.' after the first digit of the first part */
        if (last) while (len && (sp[len - 1] == '\n' || sp[len - 1] == '\r')) len--;                          /* a trailing newline of the last chunk */
        size_t badpos = (size_t)-1;
#pragma omp parallel for reduction(min:badpos) schedule(static)
        for (size_t q = 0; q < len; q++) if ((unsigned)(sp[q] - '0') > 9u && q < badpos) badpos = q;
        if (badpos != (size_t)-1) { printf("recheck: %s: a non-digit at char %zu\n", name, n + badpos); ok = 0; break; }
        uint64_t v[T1_NQ]; vf_digits_mods(sp, len, t1_q, T1_NQ, v);
        for (int i = 0; i < T1_NQ; i++) dres[i] = vf_digits_join(dres[i], len, v[i], t1_q[i]);
        *bad2 += tier2_range(sp, k, k + len, head, nhead, ndig_all, verbose, nwin);
        { size_t t = len < 49 ? len : 49; if (t < 49 && nhead) { size_t keep = 49 - t < nhead ? 49 - t : nhead; memmove(head, head + nhead - keep, keep); memcpy(head + keep, sp, t); nhead = keep + t; } else { memcpy(head, sp + len - t, t); nhead = t; } }
        n += len; k += len;
        sem_post(&r.empty[b]);
        if (last) break;
    }
    if (!ok) { r.stop = 1; sem_post(&r.empty[0]); sem_post(&r.empty[1]); }   /* the reader may be waiting for a buffer */
    pthread_join(th, 0);
    for (int b = 0; b < 2; b++) { free(r.buf[b]); sem_destroy(&r.full[b]); sem_destroy(&r.empty[b]); }
    free(r.lb);
    if (sp_odirect()) posix_fadvise(fileno(r.f), 0, 0, POSIX_FADV_DONTNEED);   /* Phase 14 S1 (E3): the digits read by the recheck leave the page cache (clean pages) */
    fclose(r.f);
    *ndig = n;
    return ok;
}
int mn_out_recheck(unsigned long N, unsigned long d, unsigned long d_out, const char *outfile, comm *c, int rank, int size, unsigned long a0, unsigned long b1, int verbose)
{
    int multi = size > 1, fail = 0; double t0 = mem_now();
    if (!outfile) { printf("recheck: no digit file\n"); return 1; }
    /* the sidecar: node 0 reads it, every node gets the values */
    uint64_t sc[4 * T1_NQ + 8]; memset(sc, 0, sizeof sc); char tail[24] = { 0 }; size_t ntail = 0;
    if (rank == 0) {
        char name[4096]; snprintf(name, sizeof name, "%s.t1", outfile); FILE *f = fopen(name, "r");
        if (!f) { printf("recheck: cannot open %s: %s\n", name, strerror(errno)); sc[4 * T1_NQ] = 1; }
        else {
            char line[1024]; unsigned long sN = 0, sd = 0, sdo = 0; int ss = 0, ok = 1;
            if (!fgets(line, sizeof line, f) || strncmp(line, "ecalc-t1 v1", 11)) ok = 0;
            if (ok && (!fgets(line, sizeof line, f) || sscanf(line, "N %lu d %lu d_out %lu size %d", &sN, &sd, &sdo, &ss) != 4)) ok = 0;
            if (ok && (sN != N || sd != d || sdo != d_out)) { printf("recheck: %s is for N %lu, d %lu, d_out %lu (this run: %lu, %lu, %lu)\n", name, sN, sd, sdo, N, d, d_out); ok = 0; }
            if (ok && fgets(line, sizeof line, f)) { uint64_t q[T1_NQ]; char *p = line + 1; for (int i = 0; i < T1_NQ; i++) q[i] = strtoull(p, &p, 10); for (int i = 0; i < T1_NQ; i++) if (q[i] != t1_q[i]) { printf("recheck: %s used other T1 moduli\n", name); ok = 0; break; } }
            for (int k = 0; ok && k < 4; k++) { if (!fgets(line, sizeof line, f)) { ok = 0; break; } char *p = line + 1; for (int i = 0; i < T1_NQ; i++) sc[k * T1_NQ + i] = strtoull(p, &p, 10); }
            if (ok && fgets(line, sizeof line, f)) { unsigned long nt = 0; char tb[64] = { 0 }; if (sscanf(line, "tail %lu %63s", &nt, tb) >= 1 && nt < sizeof tail) { ntail = nt; memcpy(tail, tb, nt); } }
            if (!ok) sc[4 * T1_NQ] = 1;
            fclose(f);
            if (ok) { sc[4 * T1_NQ + 1] = ntail; for (size_t i = 0; i < ntail; i++) sc[4 * T1_NQ + 2 + i / 8] |= (uint64_t)(unsigned char)tail[i] << (8 * (i % 8)); if (ss != size) printf("recheck: the run had %d nodes, this recheck %d (the residues do not depend on it)\n", ss, size); }
        }
    }
    mn_out_bcast_u64(c, sc, 4 * T1_NQ + 8, 0);
    if (sc[4 * T1_NQ]) { printf("recheck: node %d: no usable sidecar\n", rank); return 1; }
    ntail = (size_t)sc[4 * T1_NQ + 1]; for (size_t i = 0; i < ntail; i++) tail[i] = (char)(sc[4 * T1_NQ + 2 + i / 8] >> (8 * (i % 8)));
    const uint64_t *sX = sc, *sR = sc + T1_NQ, *sP = sc + 2 * T1_NQ, *sQ = sc + 3 * T1_NQ;
    /* 1. the digits: this node's file (part size-1-rank, the top node's part first in the file) */
    char name[4096]; if (multi) snprintf(name, sizeof name, "%s.part%04d", outfile, size - 1 - rank); else snprintf(name, sizeof name, "%s", outfile);
    int first_part = rank == size - 1;
    mn_out o; memset(&o, 0, sizeof o); o.d = d; o.d_out = d_out; o.rank = rank; o.size = size;
    char t49[64]; size_t nt49 = 0;
    if (!recheck_stat(name, first_part, rank == 0, &o.ndig, t49, &nt49)) return 1;                /* the count and the tail from the file's size and end (no pass) */
    /* the digit counts and the tails over the nodes: the global index of this node's first digit, the T2 head */
    uint64_t v[8]; memset(v, 0, sizeof v); v[0] = o.ndig; v[1] = nt49; memcpy(v + 2, t49, nt49);
    uint64_t *all = (uint64_t *)malloc((size_t)size * 8 * 8); mn_out_allgather_u64(c, v, 8, all);
    size_t ndig_all = 0, k0 = 0; for (int r = 0; r < size; r++) { ndig_all += all[(size_t)r * 8]; if (r > rank) k0 += all[(size_t)r * 8]; }
    char head[128]; size_t nhead = 0;
    for (int r = rank + 1; r < size && nhead < 49; r++) { size_t t = all[(size_t)r * 8 + 1]; if (!t) continue; memmove(head + t, head, nhead); memcpy(head, (const char *)(all + (size_t)r * 8 + 2), t); nhead += t; }
    if (nhead > 49) { memmove(head, head + nhead - 49, 49); nhead = 49; }
    free(all);
    if (ndig_all != d_out + 1) { printf("recheck: node %d: the file holds %zu digits, d_out + 1 = %lu expected\n", rank, ndig_all, d_out + 1); fail = 1; }
    /* 1 + 2. one pass over the file: the residues and the T2 windows (Phase 12 W: one read, not two -- 40 GB at 4e10) */
    int nwin = 0, bad2 = 0; size_t nread = 0;
    if (!recheck_pass(name, first_part, o.dres, &nread, k0, head, nhead, ndig_all, verbose >= 2, &nwin, &bad2)) return 1;
    if (nread != o.ndig) { printf("recheck: node %d: %s holds %zu digit chars, %zu by its size\n", rank, name, nread, o.ndig); fail = 1; }
    double t1 = mem_now();
    uint64_t Dres[T1_NQ]; mn_out_digit_res(&o, c, Dres);                                           /* the file's string "2" + fraction, joined top node first */
    /* X mod q from the file: X = (the written digits) 10^(d - d_out) + the tail */
    uint64_t Xf[T1_NQ]; { uint64_t tv[T1_NQ]; vf_digits_mods(tail, ntail, t1_q, T1_NQ, tv); for (int i = 0; i < T1_NQ; i++) Xf[i] = vf_digits_join(Dres[i], ntail, tv[i], t1_q[i]); }
    if (ntail != d - d_out) { printf("recheck: node %d: the sidecar holds %zu tail digits, d - d_out = %lu\n", rank, ntail, d - d_out); fail = 1; }
    double t2 = t1;
    /* 3. P, Q from the checkpointed top-level shares */
    uint64_t Pc[T1_NQ], Qc[T1_NQ]; int have_pq = 0;
    if (!bs_ckpt_dir) { const char *dd = mn_out_ckpt_default(outfile); struct stat st; if (dd && stat(dd, &st) == 0 && S_ISDIR(st.st_mode)) bs_ckpt_dir = dd; }   /* Phase 12 W: the run's default <outfile>.top */
    if (bs_ckpt_dir) {
        int L = size > 1 ? bs_ckpt_tree_find(N) : 0;   /* Phase 13 N (1.7, minimal): the top set's level is the schedule's (MN_GROUPS, a size 3 or 6 ...), not log2 size -- the highest complete set */
        if (!L) while ((1 << L) < size) L++;
        uint64_t desc[10]; dbig ps, qs;
        if (bs_ckpt_tree_read(L, N, desc, &ps, &qs)) {
            mdb P, Q; memset(&P, 0, sizeof P); memset(&Q, 0, sizeof Q);
            P.sh = ps; P.n = desc[0]; P.N = desc[1]; P.g0 = (int)desc[2]; P.g = (int)desc[3]; Q.sh = qs; Q.n = desc[5]; Q.N = desc[6]; Q.g0 = (int)desc[7]; Q.g = (int)desc[8];
            uint64_t vp[T1_NQ], vq[T1_NQ]; size_t lo, hi;
            mdb_share(&P, rank, &lo, &hi); if (hi > P.n) hi = P.n; { dbig sv = P.sh; sv.n = hi > lo ? hi - lo : 0; if (sv.n > P.sh.n) sv.n = P.sh.n; if (sv.n) db_mod_qs(&sv, t1_q, T1_NQ, vp); else memset(vp, 0, sizeof vp); mn_out_res_combine(c, vp, lo, Pc); }
            mdb_share(&Q, rank, &lo, &hi); if (hi > Q.n) hi = Q.n; { dbig sv = Q.sh; sv.n = hi > lo ? hi - lo : 0; if (sv.n > Q.sh.n) sv.n = Q.sh.n; if (sv.n) db_mod_qs(&sv, t1_q, T1_NQ, vq); else memset(vq, 0, sizeof vq); mn_out_res_combine(c, vq, lo, Qc); }
            db_free(&ps); db_free(&qs); have_pq = 1;
            if (multi) printf("mn: node %d: ", rank); printf("recheck: P (%zu limbs), Q (%zu limbs) from the tree level %d set in %s\n", P.n, Q.n, L, bs_ckpt_dir);
        } else printf("recheck: node %d: no tree level %d set for this run in %s (P, Q taken from the sidecar)\n", rank, L, bs_ckpt_dir);
    } else printf("recheck: node %d: no BS_CKPT_DIR and no %s.top (P, Q taken from the sidecar)\n", rank, outfile);
    if (!have_pq) { memcpy(Pc, sP, sizeof Pc); memcpy(Qc, sQ, sizeof Qc); }
    double t3 = mem_now();
    /* 4. the recurrence over this node's terms, joined */
    uint64_t pr[T1_NQ], qr[T1_NQ], Pg[T1_NQ], Qg[T1_NQ];
    for (int i = 0; i < T1_NQ; i++) vf_pq_range_mod(a0, b1, t1_q[i], &pr[i], &qr[i]);
    mn_out_pq_combine(c, pr, qr, Pg, Qg);
    double t4 = mem_now();
    /* 5. the checks */
    bigint none; bi_init(&none);
    int bad1 = tier1_res_pq(N, d, Pc, Qc, &none, &none, Pg, Qg, Xf, sR, verbose >= 2);
    int bad3 = tier1_digits_cmp(Xf, sX, verbose >= 2);
    int badPQ = 0; for (int i = 0; i < T1_NQ; i++) if (Pc[i] != sP[i] || Qc[i] != sQ[i]) badPQ++;
    if (multi) printf("mn: node %d: ", rank);
    printf("recheck: %zu digits read from %s in %.1f s (one pass: residues and windows); windows %s (%d checked); digits -> X mod q %s the run's X residues; P, Q %s: %s the recurrence (%.1f s), %s the run's; T1 identity with the run's R residues %s\n",
           o.ndig, name, t1 - t0, bad2 ? "FAILED" : "ok", nwin, bad3 ? "DIFFER from" : "==", have_pq ? "from the checkpoint" : "from the sidecar", bad1 ? "BAD at some prime vs" : "==", t4 - t3, badPQ ? "DIFFER from" : "==", bad1 ? "FAILED" : "ok");
    if (verbose >= 2) { printf("      recheck: the file pass %.1f s, checkpoint residues %.1f s\n", t1 - t0, t3 - t2); }
    fail = fail || bad1 || bad2 || bad3 || badPQ;
    if (multi) printf("mn: node %d: ", rank); printf("%s\n", fail ? "RECHECK FAILED" : "RECHECK OK");
    if (multi) { int any = mn_out_allreduce_or(c, fail); if (rank == 0) printf("mn: all %d nodes: %s\n", size, any ? "RECHECK FAILED" : "RECHECK OK"); fail = any; }
    return fail;
}
