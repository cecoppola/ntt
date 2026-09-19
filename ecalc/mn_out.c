/* mn_out.c - per-node output and verification: see mn_out.h (Phase 9 A-out, PLAN.md 19 M5 + C1) */
#include <stdio.h>
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
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

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
/* limbs [a, a+cnt) of a device number (a relative to it; views honoured) into a pinned host buffer */
static void dev_fetch(const dbig *x, size_t a, size_t cnt, uint64_t *host)
{
    size_t g = x->off + a, ge = g + cnt; int dev0; HIP_CHECK(hipGetDevice(&dev0));
    for (int d = 0; d < DB_NQ; d++) {
        size_t q0 = (size_t)d * x->qc, q1 = q0 + x->qc, s = g > q0 ? g : q0, e = ge < q1 ? ge : q1;
        if (s >= e) continue;
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMemcpy(host + (s - g), x->q[d] + (s - q0), (e - s) * 8, hipMemcpyDeviceToHost));
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
    o->nhead = 0;
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
 * buffers are in flight ---- */
struct wjob { const char *data; size_t len; size_t off; int prefix, newline, buf; char first; };   /* prefix: "<first>." before the data (digit 0 and the point) */
struct writer {
    int fd; pthread_t th; sem_t job_ready, job_taken, buf_free[2]; struct wjob job; int stop; double t_write; size_t bytes; int err;
    char *buf[2]; uint64_t *lbuf; size_t bufsz; int dev_src;
};
static void pwrite_all(int fd, const char *p, size_t n, size_t off, int *err)
{
    while (n) { ssize_t w = pwrite(fd, p, n, (off_t)off); if (w < 0) { if (errno == EINTR) continue; *err = errno; return; } p += w; n -= (size_t)w; off += (size_t)w; }
}
static void *writer_run(void *a)
{
    struct writer *w = (struct writer *)a;
    for (;;) {
        sem_wait(&w->job_ready);
        if (w->stop) break;
        struct wjob j = w->job; sem_post(&w->job_taken);
        double t0 = mem_now(); size_t off = j.off;
        if (j.prefix) { char pf[2] = { j.first, '.' }; pwrite_all(w->fd, pf, 2, off, &w->err); off += 2; w->bytes += 2; }
        if (j.len) {
            const int NT = 8; size_t seg = (j.len + NT - 1) / NT; seg = (seg + 4095) & ~(size_t)4095;
#pragma omp parallel for num_threads(NT) schedule(static)
            for (int t = 0; t < NT; t++) { size_t s = (size_t)t * seg; if (s < j.len) { size_t n = j.len - s < seg ? j.len - s : seg; pwrite_all(w->fd, j.data + s, n, off + s, &w->err); } }
            off += j.len; w->bytes += j.len;
        }
        if (j.newline) { pwrite_all(w->fd, "\n", 1, off, &w->err); w->bytes++; }
        w->t_write += mem_now() - t0;
        sem_post(&w->buf_free[j.buf]);
    }
    return 0;
}
int mn_out_run(mn_out *o, const mn_out_src *src)
{
    size_t nl, pad, lo, hi, k0, k1; ranges(o, src, &nl, &pad, &lo, &hi, &k0, &k1);
    o->k0 = k0; o->k1 = k1; o->ndig = 0; o->bad2 = o->nwin = 0; o->bytes = 0; o->nchunks = 0; o->t_fmt = o->t_res = o->t_t2 = o->t_write = o->t_fetch = o->t_wait = 0;
    o->first[0] = o->last[0] = 0;
    for (int i = 0; i < T1_NQ; i++) o->dres[i] = 0;
    size_t L = o->chunk_limbs; if (!L) { size_t mb = getenv("MN_OUT_CHUNK_MB") ? (size_t)atoi(getenv("MN_OUT_CHUNK_MB")) : 256; L = (mb << 20) / 18; if (L < 64) L = 64; }
    if (L > hi - lo && hi > lo) L = hi - lo;
    struct writer *w = (struct writer *)calloc(1, sizeof *w); o->priv = w; w->fd = -1; w->dev_src = src->dev != 0;
    if (o->outfile) {
        char name[4096];
        if (o->size > 1) snprintf(name, sizeof name, "%s.part%04d", o->outfile, o->size - 1 - o->rank); else snprintf(name, sizeof name, "%s", o->outfile);
        w->fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (w->fd < 0) { printf("mn_out: cannot open %s: %s\n", name, strerror(errno)); }
    }
    sem_init(&w->job_ready, 0, 0); sem_init(&w->job_taken, 0, 0); sem_init(&w->buf_free[0], 0, 1); sem_init(&w->buf_free[1], 0, 1);
    w->bufsz = L * 18 + 64;
    for (int b = 0; b < 2; b++) if (posix_memalign((void **)&w->buf[b], 2u << 20, w->bufsz)) { fprintf(stderr, "mn_out: %zu bytes\n", w->bufsz); exit(1); }
    if (src->dev) HIP_CHECK(hipHostMalloc((void **)&w->lbuf, L * 8, 0)); else w->lbuf = (uint64_t *)malloc(L * 8);
    pthread_create(&w->th, 0, writer_run, w);
    size_t fbase = k0 == 0 ? 0 : k0 + 1, kw_end = o->d_out + 1;      /* the part's first byte in the file; digits < kw_end are written */
    char head[64]; size_t nhead = o->nhead; memcpy(head, o->head, nhead);
    int b = 0;
    for (size_t bb = hi; bb > lo; b ^= 1) {
        size_t a = bb - lo > L ? bb - L : lo, cnt = bb - a;
        double t0 = mem_now(); sem_wait(&w->buf_free[b]); double t1 = mem_now(); o->t_wait += t1 - t0;
        const uint64_t *l; size_t avail = src->lo + src->cnt > a ? src->lo + src->cnt - a : 0; if (avail > cnt) avail = cnt;   /* limbs beyond the source are zero */
        if (src->dev || avail < cnt) { if (avail) { if (src->dev) dev_fetch(src->dev, a - src->lo, avail, w->lbuf); else memcpy(w->lbuf, src->host + (a - src->lo), avail * 8); } memset(w->lbuf + avail, 0, (cnt - avail) * 8); l = w->lbuf; }
        else l = src->host + (a - src->lo);
        double t2 = mem_now(); o->t_fetch += t2 - t1;
        char *buf = w->buf[b]; fmt_limbs(buf, l, cnt);
        size_t p0 = (nl - bb) * 18, ck0 = p0 > pad ? p0 - pad : 0, ck1 = (nl - a) * 18 - pad;   /* the chunk's digits */
        const char *s = buf + (p0 > pad ? 0 : pad - p0); size_t len = ck1 - ck0;
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
        /* the write: digits [ck0, min(ck1, kw_end)); "2." before digit 0, the newline after digit d_out */
        size_t we = ck1 < kw_end ? ck1 : kw_end;
        if (w->fd >= 0 && ck0 < we) {
            struct wjob j; j.buf = b; j.prefix = ck0 == 0; j.first = s[0]; j.data = s + j.prefix; j.len = we - ck0 - j.prefix; j.off = (j.prefix ? 0 : ck0 + 1) - fbase; j.newline = ck0 <= o->d_out && o->d_out < ck1;
            w->job = j; sem_post(&w->job_ready); sem_wait(&w->job_taken);
        } else sem_post(&w->buf_free[b]);
        o->nchunks++;
        bb = a;
    }
    return 0;
}
void mn_out_finish(mn_out *o)
{
    struct writer *w = (struct writer *)o->priv; if (!w) return;
    w->stop = 1; sem_post(&w->job_ready); pthread_join(w->th, 0);
    if (w->fd >= 0) close(w->fd);
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
