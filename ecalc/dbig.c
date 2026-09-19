/* dbig.c - see dbig.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <pthread.h>
#include <hip/hip_runtime.h>
#include "dbig.h"
#include "mem.h"
#include <time.h>
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define CH 4096                                        /* limbs per carry chunk (256 threads x 16) */
static const uint64_t B10 = 1000000000000000000ULL;
struct db_stats db_st;
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int g_par = -1;                                 /* DBIG_SERIAL=1: drive the four quarters from one thread (debug) */
static void par_init(void) { if (g_par < 0) g_par = !(getenv("DBIG_SERIAL") && atoi(getenv("DBIG_SERIAL"))); }

struct dv { const uint64_t *q[DB_NQ]; size_t qc, off, shift; int lq, m3; };     /* shift: the operand as a << shift limbs; qc = (m3 ? 3 : 1) << lq */
__device__ static inline size_t dq(size_t g, int lq, int m3) { return m3 ? (g >> lq) / 3 : g >> lq; }
__device__ static inline uint64_t dget(const struct dv v, size_t i) { if (i < v.shift) return 0; size_t g = v.off + i - v.shift, d = dq(g, v.lq, v.m3); return v.q[d][g - d * v.qc]; }
static struct dv view_of(const dbig *a) { struct dv v; for (int d = 0; d < DB_NQ; d++) v.q[d] = a->q[d]; v.qc = a->qc; v.lq = a->lq; v.m3 = a->m3; v.off = a->off; v.shift = 0; return v; }
static inline size_t hq(const dbig *a, size_t g) { return a->m3 ? (g >> a->lq) / 3 : g >> a->lq; }   /* host: quarter of global limb g */
static void need_owner(const dbig *r, const char *what) { if (r->off || (!r->cap && r->n)) { fprintf(stderr, "dbig: %s into a view\n", what); abort(); } }

__global__ void k_touch(uint64_t *p, size_t n) { size_t step = (1 << 21) / 8; for (size_t i = (size_t)threadIdx.x * step; i < n; i += step * blockDim.x) { uint64_t v = p[i]; if (v == 0x123456789ULL) p[i] = v; } }
/* quarter blocks come from per-device free lists by size class (hipMalloc costs ~0.06 s/GB and the Newton
 * loop allocates and frees temporaries every iteration); db_release_pools gives everything back */
/* Block pool per device: address-ordered free extents with coalescing (best fit, carve from the front,
 * the remainder stays an extent; neighbours merge on free).  Sizes are 2^l or 3 2^l limbs but nothing
 * depends on it, so the bs phase's many sizes and the dm phase's few large ones share the pool without
 * fragmentation (RESULTS.md 64).  Extents come from donated regions (db_donate) or from hipMalloc when
 * nothing fits (0.057 s/GB); regions are freed whole by db_release_pools. */
struct ext { char *p; size_t bytes; int reg; };                   /* reg: the whole region (g_donated) it lies in; extents never merge across regions, since a block must stay inside one hipMalloc allocation */
static struct { struct ext e[8192]; int n; } g_ext[DB_NQ];
static size_t g_pool_bytes;
static struct { uint64_t *p; int dev; size_t bytes; int reg; } g_live[8192]; static int g_nlive;
static pthread_mutex_t g_pool_mx = PTHREAD_MUTEX_INITIALIZER;   /* Phase 8: a background thread may free blocks */
static struct { void *p; int dev; size_t bytes; } g_donated[256]; static int g_ndonated;   /* whole hipMalloc'd or donated regions */
static size_t fsize(int fam, int l) { return (size_t)(fam ? 3 : 1) * 8 << l; }
static void live_add(uint64_t *p, int d, size_t bytes, int reg) { if (g_nlive < 8192) { g_live[g_nlive].p = p; g_live[g_nlive].dev = d; g_live[g_nlive].bytes = bytes; g_live[g_nlive].reg = reg; g_nlive++; } else { fprintf(stderr, "dbig: live table full\n"); abort(); } }
static size_t live_take(uint64_t *p, int *reg) { for (int i = 0; i < g_nlive; i++) if (g_live[i].p == p) { size_t b = g_live[i].bytes; *reg = g_live[i].reg; g_live[i] = g_live[--g_nlive]; return b; } return 0; }
static void ext_insert(int d, char *p, size_t bytes, int reg)
{
    struct ext *e = g_ext[d].e; int n = g_ext[d].n, i = 0;
    while (i < n && e[i].p < p) i++;
    int mprev = i > 0 && e[i - 1].reg == reg && e[i - 1].p + e[i - 1].bytes == p, mnext = i < n && e[i].reg == reg && p + bytes == e[i].p;
    if (mprev && mnext) { e[i - 1].bytes += bytes + e[i].bytes; memmove(&e[i], &e[i + 1], (n - i - 1) * sizeof *e); g_ext[d].n--; }
    else if (mprev) e[i - 1].bytes += bytes;
    else if (mnext) { e[i].p = p; e[i].bytes += bytes; }
    else { if (n >= 8192) { fprintf(stderr, "dbig: extent table full\n"); abort(); } memmove(&e[i + 1], &e[i], (n - i) * sizeof *e); e[i].p = p; e[i].bytes = bytes; e[i].reg = reg; g_ext[d].n++; }
}
static char *ext_take(int d, size_t need, int *reg)        /* best fit, carved from the front */
{
    struct ext *e = g_ext[d].e; int n = g_ext[d].n, best = -1;
    for (int i = 0; i < n; i++) if (e[i].bytes >= need && (best < 0 || e[i].bytes < e[best].bytes)) best = i;
    if (best < 0) return 0;
    char *p = e[best].p; *reg = e[best].reg; e[best].p += need; e[best].bytes -= need;
    if (!e[best].bytes) { memmove(&e[best], &e[best + 1], (n - best - 1) * sizeof *e); g_ext[d].n--; }
    return p;
}
static uint64_t *q_alloc_locked(int d, int fam, int l);
static uint64_t *q_alloc(int d, int fam, int l) { pthread_mutex_lock(&g_pool_mx); uint64_t *p = q_alloc_locked(d, fam, l); pthread_mutex_unlock(&g_pool_mx); return p; }
static uint64_t *q_alloc_locked(int d, int fam, int l)
{
    size_t need = fsize(fam, l); int reg;
    char *p = ext_take(d, need, &reg);
    if (!p) {
        g_pool_bytes += need;
        int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d));
        void *m; HIP_CHECK(hipMalloc(&m, need)); HIP_CHECK(hipMemset(m, 0, need)); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipSetDevice(cur));
        if (g_ndonated < 256) { reg = g_ndonated; g_donated[g_ndonated].p = m; g_donated[g_ndonated].dev = d; g_donated[g_ndonated].bytes = need; g_ndonated++; } else { fprintf(stderr, "dbig: region table full\n"); abort(); }
        p = (char *)m;
    }
    live_add((uint64_t *)p, d, need, reg); return (uint64_t *)p;
}
static void q_release(int d, uint64_t *p) { int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipFree(p)); HIP_CHECK(hipSetDevice(cur)); }
static void q_free(int d, uint64_t *p)
{
    pthread_mutex_lock(&g_pool_mx);
    int reg; size_t bytes = live_take(p, &reg); if (!bytes) { fprintf(stderr, "dbig: freeing an unknown block\n"); abort(); }
    ext_insert(d, (char *)p, bytes, reg);
    pthread_mutex_unlock(&g_pool_mx);
}
void db_donate(int dev, void *p, size_t bytes)
{
    pthread_mutex_lock(&g_pool_mx);
    if (g_ndonated >= 256) { fprintf(stderr, "db_donate: too many regions\n"); abort(); }
    g_donated[g_ndonated].p = p; g_donated[g_ndonated].dev = dev; g_donated[g_ndonated].bytes = bytes; g_ndonated++;
    ext_insert(dev, (char *)p, bytes, g_ndonated - 1);
    pthread_mutex_unlock(&g_pool_mx);
}
size_t db_pool_free_bytes(int d) { size_t s = 0; for (int i = 0; i < g_ext[d].n; i++) s += g_ext[d].e[i].bytes; return s; }
int db_pool_extents(int d) { return g_ext[d].n; }
void db_release_pools(void)
{
    for (int d = 0; d < DB_NQ; d++) g_ext[d].n = 0;              /* every extent is a piece of a whole region */
    for (int i = 0; i < g_ndonated; i++) q_release(g_donated[i].dev, (uint64_t *)g_donated[i].p);
    g_ndonated = 0; g_nlive = 0; g_pool_bytes = 0;
}
size_t db_pool_bytes(void) { return g_pool_bytes; }
void db_init(dbig *x) { par_init(); memset(x, 0, sizeof *x); }
void db_free(dbig *x) { if (x->cap) for (int d = 0; d < DB_NQ; d++) if (x->q[d]) q_free(d, x->q[d]); memset(x, 0, sizeof *x); }
void db_reserve(dbig *x, size_t limbs)
{
    if (limbs <= x->cap) return;
    double t0 = tnow(); db_st.n_reserve++;
    if (x->off) { fprintf(stderr, "db_reserve: a view\n"); abort(); }
    /* the smallest quarter of the form 2^l or 3 2^l that holds limbs/4 */
    size_t need = (limbs + DB_NQ - 1) / DB_NQ, qc = (size_t)1 << 10; int lq = 10, m3 = 0;
    while (qc < need) { qc <<= 1; lq++; }
    if (lq >= 12 && ((size_t)3 << (lq - 2)) >= need) { m3 = 1; lq -= 2; qc = (size_t)3 << lq; }
    dbig y; db_init(&y); y.cap = qc * DB_NQ; y.qc = qc; y.lq = lq; y.m3 = m3;
    for (int d = 0; d < DB_NQ; d++) y.q[d] = q_alloc(d, m3, lq);
    if (getenv("DBIG_WARM")) {                             /* touch every 2 MiB page of each quarter from every other device */
        for (int d = 0; d < DB_NQ; d++) for (int c = 0; c < DB_NQ; c++) if (c != d) {
            HIP_CHECK(hipSetDevice(c));
            k_touch<<<1, 256>>>(y.q[d], qc);
            HIP_CHECK(hipStreamSynchronize(0));
        }
        HIP_CHECK(hipSetDevice(0));
    }
    if (x->n) {                                           /* keep the contents: quarter-wise DMA through the limb map */
        size_t n = x->n;
        for (int d = 0; d < DB_NQ; d++) {
            size_t lo = (size_t)d * y.qc, hi = lo + y.qc; if (hi > n) hi = n;
            for (size_t i = lo; i < hi;) {                 /* the source run containing i */
                size_t sd = hq(x, i), so = i - sd * x->qc, run = x->qc - so; if (i + run > hi) run = hi - i;
                mem_dev_copy_on(d, y.q[d] + (i - lo), x->q[sd] + so, run * 8); i += run;
            }
        }
        y.n = n;
    }
    for (int d = 0; d < DB_NQ; d++) if (x->q[d]) q_free(d, x->q[d]);
    *x = y;
    db_st.t_reserve += tnow() - t0;
}
/* (the kernels above synchronise the null stream only, not the device: a background copy on the non-blocking
 * stream below must not be waited for by every kernel of the Newton loop -- Phase 8, RESULTS.md 68) */
/* host <-> device copies through a pinned bounce buffer per device (1 GiB): hipMemcpy with pageable host
 * memory runs at ~3 GB/s, pinned at ~50 GB/s; the host side is a parallel memcpy.  Quarters one after
 * another (concurrent hipMemcpy with pageable memory faults, RESULTS.md 59), chunks pipelined by two. */
#define BOUNCE ((size_t)1 << 27)                              /* limbs: 1 GiB */
static uint64_t *g_bounce[DB_NQ][2]; static hipStream_t g_bs[DB_NQ];
static pthread_mutex_t g_bounce_mx = PTHREAD_MUTEX_INITIALIZER;   /* Phase 8: copies may be issued from a background thread; one at a time per process */
static void bounce_init(int d)
{
    if (g_bounce[d][0]) return;
    HIP_CHECK(hipSetDevice(d));
    HIP_CHECK(hipHostMalloc((void **)&g_bounce[d][0], BOUNCE * 8, 0)); HIP_CHECK(hipHostMalloc((void **)&g_bounce[d][1], BOUNCE * 8, 0));
    HIP_CHECK(hipStreamCreateWithFlags(&g_bs[d], hipStreamNonBlocking));   /* Phase 8: copies from a background thread must not serialise with the kernels on the null stream */
}
static void par_memcpy(uint64_t *dst, const uint64_t *src, size_t n)
{
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < n; i += 1 << 18) { size_t m = n - i < (1 << 18) ? n - i : (1 << 18); memcpy(dst + i, src + i, m * 8); }
}
void db_from_bi(dbig *x, const bigint *a)
{
    pthread_mutex_lock(&g_bounce_mx);
    db_reserve(x, a->n ? a->n : 1); x->n = a->n;
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo = (size_t)d * x->qc; if (lo >= a->n) break;
        size_t len = a->n - lo < x->qc ? a->n - lo : x->qc;
        bounce_init(d); HIP_CHECK(hipSetDevice(d)); int b = 0;
        HIP_CHECK(hipStreamSynchronize(0));                         /* nothing of ours still runs on the target */
        for (size_t i = 0; i < len; i += BOUNCE, b ^= 1) {
            size_t m = len - i < BOUNCE ? len - i : BOUNCE;
            HIP_CHECK(hipStreamSynchronize(g_bs[d]));               /* the buffer's previous upload is done */
            par_memcpy(g_bounce[d][b], a->l + lo + i, m);
            HIP_CHECK(hipMemcpyAsync(x->q[d] + i, g_bounce[d][b], m * 8, hipMemcpyHostToDevice, g_bs[d]));
        }
        HIP_CHECK(hipStreamSynchronize(g_bs[d]));
    }
    pthread_mutex_unlock(&g_bounce_mx);
}
void db_to_bi(bigint *r, const dbig *x)
{
    pthread_mutex_lock(&g_bounce_mx);
    bi_reserve(r, x->n ? x->n : 1); r->n = x->n;
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo = (size_t)d * x->qc; if (lo >= x->n) break;
        size_t len = x->n - lo < x->qc ? x->n - lo : x->qc;
        bounce_init(d); HIP_CHECK(hipSetDevice(d)); int b = 0; size_t prev = 0, prevm = 0; int have = 0;
        HIP_CHECK(hipStreamSynchronize(0));                         /* the source is complete (its kernels ran on the null stream) */
        for (size_t i = 0; i < len; i += BOUNCE, b ^= 1) {
            size_t m = len - i < BOUNCE ? len - i : BOUNCE;
            HIP_CHECK(hipMemcpyAsync(g_bounce[d][b], x->q[d] + i, m * 8, hipMemcpyDeviceToHost, g_bs[d]));
            if (have) par_memcpy(r->l + lo + prev, g_bounce[d][b ^ 1], prevm);   /* the previous chunk, already landed */
            HIP_CHECK(hipStreamSynchronize(g_bs[d]));
            prev = i; prevm = m; have = 1;
        }
        if (have) par_memcpy(r->l + lo + prev, g_bounce[d][b ^ 1], prevm);
    }
    pthread_mutex_unlock(&g_bounce_mx);
}

/* ---- residue mod q of a device number (Phase 8 I3: the T1 residues of P, Q, R without host copies) ----
 * one thread per chunk of MODQ_CH limbs computes v_c = sum x[i] B^(i - lo) mod q (Horner from the top of the
 * chunk); the host combines the chunks: r = sum v_c B^(lo_c) mod q, Horner over the chunks from the top */
#define MODQ_CH 4096
__device__ static uint64_t mulmod_dev(uint64_t a, uint64_t b, uint64_t q) { return (uint64_t)((unsigned __int128)a * b % q); }
__global__ void k_modq(const uint64_t *x, size_t n, uint64_t q, uint64_t Bq, uint64_t *out)
{
    size_t c = (size_t)blockIdx.x * blockDim.x + threadIdx.x, lo = c * MODQ_CH; if (lo >= n) return;
    size_t hi = lo + MODQ_CH < n ? lo + MODQ_CH : n;
    uint64_t v = 0;
    for (size_t k = hi; k-- > lo;) v = (uint64_t)(((unsigned __int128)v * Bq + x[k]) % q);
    out[c] = v;
}
static uint64_t powmod_h(uint64_t b, uint64_t e, uint64_t q) { uint64_t r = 1; b %= q; while (e) { if (e & 1) r = (uint64_t)((unsigned __int128)r * b % q); b = (uint64_t)((unsigned __int128)b * b % q); e >>= 1; } return r; }
static void qrange(const dbig *x, int d, size_t n, size_t *lo, size_t *hi);
uint64_t db_mod_q(const dbig *x, uint64_t q)
{
    if (!x->n) return 0;
    uint64_t Bq = bi_decimal ? BI_B10 % q : (uint64_t)(((unsigned __int128)1 << 64) % q), Bch = powmod_h(Bq, MODQ_CH, q);
    uint64_t r = 0; size_t maxc = x->qc / MODQ_CH + 2;
    uint64_t *hv = (uint64_t *)malloc(maxc * 8);
    for (int d = DB_NQ; d-- > 0;) {                                 /* from the top quarter down: r = r B^len + v_quarter */
        size_t lo, hi; qrange(x, d, x->n, &lo, &hi); if (lo >= hi) continue;
        size_t first = x->off + lo - (size_t)d * x->qc, len = hi - lo, nc = (len + MODQ_CH - 1) / MODQ_CH;
        uint64_t *dv; HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&dv, nc * 8));
        k_modq<<<(unsigned)((nc + 255) / 256), 256>>>(x->q[d] + first, len, q, Bq, dv);
        HIP_CHECK(hipStreamSynchronize(0));
        HIP_CHECK(hipMemcpy(hv, dv, nc * 8, hipMemcpyDeviceToHost)); HIP_CHECK(hipFree(dv));
        uint64_t vq = 0; for (size_t c = nc; c-- > 0;) vq = (uint64_t)(((unsigned __int128)vq * Bch + hv[c]) % q);   /* chunks below the top one are full */
        r = (uint64_t)(((unsigned __int128)r * powmod_h(Bq, len, q) + vq) % q);
    }
    free(hv);
    return r;
}
/* ---- kernels: one per quarter, over the result's limbs [lo, hi) of that quarter ---- */
__global__ void k_gather_shift(uint64_t *out, size_t lo, size_t hi, struct dv a, size_t an, long shift)   /* out[i] = a[i + shift] or 0 */
{
    size_t i = lo + (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < hi; i += stride) { long s = (long)i + shift; out[i - lo] = (s >= 0 && (size_t)s < an) ? dget(a, (size_t)s) : 0; }
}
/* add/sub of one chunk (CH = 256 threads x SEG limbs) with a block-level carry scan: thread t sums its
 * SEG limbs with carry-in 0 (generate g, propagate p over the segment), a scan over the 256 (g, p) gives
 * every segment's carry-in, the segment is redone with it.  r may be a or b (same layout: a thread only
 * touches its own limbs).  cout/prop per chunk for the host scan across chunks and quarters.
 * The b operand is either a dbig or a sparse set of 4-limb spills (sp != 0): spill j sits at limb
 * R j + row0 + rows for row0 in {0, rows, 2 rows, 3 rows} (four ranks' spill arrays). */
struct sparse { const uint64_t *sp[4]; size_t R, rows, C; int single; size_t pos; uint64_t val; };   /* single: one limb val at pos */
__device__ static inline uint64_t sparse_get(const struct sparse s, size_t i)
{
    if (s.single) return i == s.pos ? s.val : 0;
    /* i = R j + (r+1) rows + t, t < 4: j = i / R, rem = i - R j; r+1 = rem / rows if rem % rows < 4 */
    size_t j = i / s.R, rem = i - j * s.R, q = rem / s.rows, t = rem - q * s.rows;
    if (t >= 4) return 0;
    if (q == 0) return (j >= 1 && j - 1 < s.C) ? s.sp[3][(j - 1) * 4 + t] : 0;   /* rank 3's spill of column j-1 lands at R j */
    return j < s.C ? s.sp[q - 1][j * 4 + t] : 0;
}
#define SEG 16
__global__ void k_addsub(uint64_t *out, size_t lo, size_t hi, struct dv a, size_t an, struct dv b, size_t bn, struct sparse sp, int has_sp, int sub, int dec, uint8_t *cout, uint8_t *prop)
{
    __shared__ uint8_t G[256], P[256];
    size_t c0 = lo + (size_t)blockIdx.x * CH; if (c0 >= hi) return;
    size_t c1 = c0 + CH < hi ? c0 + CH : hi, s0 = c0 + (size_t)threadIdx.x * SEG, s1 = s0 + SEG < c1 ? s0 + SEG : c1;
    uint64_t x[SEG], y[SEG]; int g = 0, p = 1;
    for (int k = 0; k < SEG; k++) {
        size_t i = s0 + k;
        x[k] = (i < s1 && i < an) ? dget(a, i) : 0;
        y[k] = (i < s1) ? (has_sp ? (i < bn ? sparse_get(sp, i) : 0) : (i < bn ? dget(b, i) : 0)) : 0;
    }
    {   /* pass 1: carry-in 0 -> generate / propagate of the segment */
        uint64_t cy = 0; int pr = 1;
        for (int k = 0; k < SEG; k++) { if (s0 + k >= s1) break;
            uint64_t xx = x[k], yy = y[k], s;
            if (dec) { if (sub) { s = xx + B10 - yy - cy; cy = s < B10; pr &= (xx == yy); } else { s = xx + yy + cy; cy = s >= B10; pr &= (xx + yy == B10 - 1); } }
            else { if (sub) { s = xx - yy - cy; cy = (xx < yy) || (xx == yy && cy); pr &= (xx == yy); } else { s = xx + yy + cy; cy = (s < xx) || (cy && s == xx); pr &= (xx + yy == ~0ULL); } }
        }
        g = (int)cy; p = (s0 < s1) ? pr : 1;
    }
    G[threadIdx.x] = (uint8_t)g; P[threadIdx.x] = (uint8_t)p;
    __syncthreads();
    if (threadIdx.x == 0) {                                   /* serial scan over 256 segments (cheap) */
        uint8_t cy = 0;
        for (int t = 0; t < 256; t++) { uint8_t gg = G[t], pp = P[t]; G[t] = cy; cy = gg | (pp & cy); }
        cout[blockIdx.x] = cy; prop[blockIdx.x] = 1;
        for (int t = 0; t < 256; t++) if (!P[t]) { prop[blockIdx.x] = 0; break; }
    }
    __syncthreads();
    {   /* pass 2: with the segment's carry-in */
        uint64_t cy = G[threadIdx.x];
        for (int k = 0; k < SEG; k++) { size_t i = s0 + k; if (i >= s1) break;
            uint64_t xx = x[k], yy = y[k], s;
            if (dec) { if (sub) { s = xx + B10 - yy - cy; cy = s < B10; s = cy ? s : s - B10; } else { s = xx + yy + cy; cy = s >= B10; s = cy ? s - B10 : s; } }
            else { if (sub) { s = xx - yy - cy; cy = (xx < yy) || (xx == yy && cy); } else { s = xx + yy + cy; cy = (s < xx) || (cy && s == xx); } }
            out[i - lo] = s;
        }
    }
}
/* apply a carry-in of 1 (or borrow) to a chunk, rippling until absorbed */
__global__ void k_carry(uint64_t *out, size_t lo, size_t hi, const uint8_t *cin, int sub, int dec)
{
    size_t c0 = lo + (size_t)blockIdx.x * CH; if (c0 >= hi || !cin[blockIdx.x]) return;
    size_t c1 = c0 + CH < hi ? c0 + CH : hi;
    if (threadIdx.x) return;
    uint64_t *o = out + (c0 - lo);
    for (size_t i = c0; i < c1; i++) {
        uint64_t v = o[i - c0];
        if (dec) { if (sub) { if (v) { o[i - c0] = v - 1; return; } o[i - c0] = B10 - 1; } else { if (v + 1 < B10) { o[i - c0] = v + 1; return; } o[i - c0] = 0; } }
        else { if (sub) { o[i - c0] = v - 1; if (v) return; } else { o[i - c0] = v + 1; if (v != ~0ULL) return; } }
    }
}
__global__ void k_maxidx(struct dv a, struct dv b, int hasb, size_t lo, size_t hi, size_t *res)   /* 1 + highest i in [lo,hi) with a[i] != b[i] (or != 0), per block */
{
    __shared__ size_t sm[256];
    size_t best = 0; int found = 0;
    for (size_t i = lo + (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < hi; i += (size_t)gridDim.x * blockDim.x)
        if (hasb ? dget(a, i) != dget(b, i) : dget(a, i) != 0) { if (!found || i > best) best = i; found = 1; }
    sm[threadIdx.x] = found ? best + 1 : 0;
    __syncthreads();
    for (int s = 128; s > 0; s >>= 1) { if (threadIdx.x < s && sm[threadIdx.x + s] > sm[threadIdx.x]) sm[threadIdx.x] = sm[threadIdx.x + s]; __syncthreads(); }
    if (threadIdx.x == 0) res[blockIdx.x] = sm[0];
}

static unsigned nblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 8 ? 228 * 8 : b); }
static uint8_t *g_flags[DB_NQ][2]; static size_t g_flags_cap[DB_NQ]; static size_t *g_red[DB_NQ]; static size_t *g_hred;
static void flags_reserve(int d, size_t chunks)
{
    if (g_flags_cap[d] >= chunks) return;
    HIP_CHECK(hipSetDevice(d));
    if (g_flags[d][0]) { HIP_CHECK(hipHostFree(g_flags[d][0])); HIP_CHECK(hipHostFree(g_flags[d][1])); }
    HIP_CHECK(hipHostMalloc((void **)&g_flags[d][0], chunks + 16, 0)); HIP_CHECK(hipHostMalloc((void **)&g_flags[d][1], chunks + 16, 0));   /* pinned host: written by the kernel, scanned by the host */
    g_flags_cap[d] = chunks;
    if (!g_red[d]) { HIP_CHECK(hipMalloc(&g_red[d], 228 * 8 * 8)); }
    if (!g_hred) g_hred = (size_t *)malloc(228 * 8 * 8 * DB_NQ);
}
/* the limbs [lo, hi) of x (n limbs, possibly a view at x->off) whose storage is in quarter d */
static void qrange(const dbig *x, int d, size_t n, size_t *lo, size_t *hi)
{
    size_t g0 = (size_t)d * x->qc, g1 = g0 + x->qc;              /* global limb range of quarter d */
    *lo = g0 > x->off ? g0 - x->off : 0; *hi = g1 > x->off ? g1 - x->off : 0;
    if (*hi > n) *hi = n; if (*lo > n) *lo = n;
}

static void shift_into(dbig *r, const dbig *a, long shift, size_t n)      /* r[i] = a[i + shift], n limbs */
{
    double t0 = tnow(); db_st.n_shift++;
    if (r == a) { fprintf(stderr, "db shift: in place\n"); abort(); }
    need_owner(r, "shift"); db_reserve(r, n ? n : 1);
    struct dv v = view_of(a);
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(r, d, n, &lo, &hi); if (lo >= hi) continue;
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_gather_shift<<<nblk(hi - lo), 256>>>(r->q[d], lo, hi, v, a->n, shift);
        HIP_CHECK(hipStreamSynchronize(0));
    }
    r->n = n; db_norm(r);
    db_st.t_shift += tnow() - t0;
}
void db_shr_limbs(dbig *r, const dbig *a, size_t k) { shift_into(r, a, (long)k, a->n > k ? a->n - k : 0); }
void db_shl_limbs(dbig *r, const dbig *a, size_t k) { shift_into(r, a, -(long)k, a->n ? a->n + k : 0); }
void db_copy(dbig *r, const dbig *a) { if (r == a) return; shift_into(r, a, 0, a->n); }

static void addsub_core(dbig *r, const dbig *a, size_t ashift, const dbig *b, const struct sparse *spx, size_t bn, int sub)
{
    double t0 = tnow(); db_st.n_addsub++;
    size_t an = a->n ? a->n + ashift : 0, n = an > bn ? an : bn; if (!sub) n++;
    /* in place is fine when the layouts match (a thread only touches its own limbs); otherwise a temporary */
    dbig tmp; int inplace = (r == a || r == b), same = (r == a && !r->off && !ashift) || (b && r == b && !r->off);   /* r == b is safe even with a shifted: a thread reads only its own limbs of r */
    dbig *out = r;
    if (inplace && !same) { db_init(&tmp); out = &tmp; }
    need_owner(out, "add/sub"); db_reserve(out, n ? n : 1);
    struct dv va = view_of(a), vb = b ? view_of(b) : va; va.shift = ashift; struct sparse sp; memset(&sp, 0, sizeof sp); if (spx) sp = *spx;
    size_t chunks[DB_NQ], lo[DB_NQ], hi[DB_NQ];
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
    for (int d = 0; d < DB_NQ; d++) {
        qrange(out, d, n, &lo[d], &hi[d]); chunks[d] = (hi[d] - lo[d] + CH - 1) / CH;
        if (!chunks[d]) continue;
        flags_reserve(d, chunks[d]);
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_addsub<<<(unsigned)chunks[d], 256>>>(out->q[d], lo[d], hi[d], va, an, vb, bn, sp, spx != 0, sub, bi_decimal, g_flags[d][0], g_flags[d][1]);
        HIP_CHECK(hipStreamSynchronize(0));
    }
    /* scan the chunk flags in order: carry-in of chunk = carry-out of the previous, or its carry-in if it propagates */
    uint8_t cy = 0;
    for (int d = 0; d < DB_NQ; d++) for (size_t c = 0; c < chunks[d]; c++) {
        uint8_t co = g_flags[d][0][c], pr = g_flags[d][1][c];
        g_flags[d][0][c] = cy;                          /* reuse as carry-in */
        cy = co | (pr & cy);
    }
    if (cy && !sub) { fprintf(stderr, "db_add: carry out of the top (n undersized)\n"); abort(); }
    if (cy && sub) { fprintf(stderr, "db_sub: a < b\n"); abort(); }
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
    for (int d = 0; d < DB_NQ; d++) {
        if (!chunks[d]) continue;
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_carry<<<(unsigned)chunks[d], 1>>>(out->q[d], lo[d], hi[d], g_flags[d][0], sub, bi_decimal);
        HIP_CHECK(hipStreamSynchronize(0));
    }
    out->n = n; db_norm(out);
    if (inplace && !same) { dbig sw = *r; *r = tmp; tmp = sw; db_free(&tmp); }
    db_st.t_addsub += tnow() - t0;
}
void db_add(dbig *r, const dbig *a, const dbig *b) { addsub_core(r, a, 0, b, 0, b->n, 0); }
void db_sub(dbig *r, const dbig *a, const dbig *b) { addsub_core(r, a, 0, b, 0, b->n, 1); }
void db_add_shifted(dbig *r, const dbig *a, size_t k, const dbig *b) { addsub_core(r, a, k, b, 0, b->n, 0); }   /* r = (a << k) + b */
void db_sub_shifted(dbig *r, const dbig *a, size_t k, const dbig *b) { addsub_core(r, a, k, b, 0, b->n, 1); }   /* r = (a << k) - b */
/* r = a + the sparse spill set (4 limbs at R j + (q+1) rows for q = 0..3, j < C), n limbs of result */
void db_add_spills(dbig *r, const dbig *a, const uint64_t *const sp[4], size_t R, size_t rows, size_t C, size_t n)
{
    struct sparse s; memset(&s, 0, sizeof s); for (int q = 0; q < 4; q++) s.sp[q] = sp[q]; s.R = R; s.rows = rows; s.C = C;
    addsub_core(r, a, 0, 0, &s, n, 0);
}
/* r = a - B^e  (a >= B^e) */
void db_sub_pow(dbig *r, const dbig *a, size_t e)
{
    struct sparse s; memset(&s, 0, sizeof s); s.single = 1; s.pos = e; s.val = 1;
    addsub_core(r, a, 0, 0, &s, e + 1, 1);
}
/* r = B^e - a  (0 < a < B^e): the limb-wise complement (B-1-a[i], i < e) plus one */
__global__ void k_complement(uint64_t *out, size_t lo, size_t hi, struct dv a, size_t an, uint64_t top)
{
    size_t i = lo + (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < hi; i += stride) out[i - lo] = top - (i < an ? dget(a, i) : 0);
}
void db_pow_sub(dbig *r, size_t e, const dbig *a)
{
    if (r == a) { fprintf(stderr, "db_pow_sub: in place\n"); abort(); }
    need_owner(r, "pow_sub"); db_reserve(r, e + 1);
    struct dv va = view_of(a); uint64_t top = bi_decimal ? B10 - 1 : ~0ULL;
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(r, d, e, &lo, &hi); if (lo >= hi) continue;
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_complement<<<nblk(hi - lo), 256>>>(r->q[d], lo, hi, va, a->n, top);
        HIP_CHECK(hipStreamSynchronize(0));
    }
    r->n = e; db_norm(r);
    struct sparse s; memset(&s, 0, sizeof s); s.single = 1; s.pos = 0; s.val = 1;
    addsub_core(r, r, 0, 0, &s, 1, 0);
}

static size_t maxidx(const dbig *a, const dbig *b, size_t n)      /* 1 + highest index i < n with a[i] != b[i] (b null: != 0), or 0 */
{
    double t0 = tnow(); db_st.n_maxidx++;
    size_t best = 0; struct dv va = view_of(a), vb = b ? view_of(b) : va;
#pragma omp parallel for num_threads(DB_NQ) reduction(max:best) if(g_par)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(a, d, n, &lo, &hi); if (lo >= hi) continue;
        flags_reserve(d, 1);
        HIP_CHECK(hipSetDevice(d));
        unsigned blocks = nblk(hi - lo);
#pragma omp critical
        k_maxidx<<<blocks, 256>>>(va, vb, b != 0, lo, hi, g_red[d]);
        HIP_CHECK(hipMemcpy(g_hred + d * 228 * 8, g_red[d], blocks * 8, hipMemcpyDeviceToHost));
        size_t m = 0; for (unsigned i = 0; i < blocks; i++) if (g_hred[d * 228 * 8 + i] > m) m = g_hred[d * 228 * 8 + i];
        if (m > best) best = m;
    }
    db_st.t_maxidx += tnow() - t0;
    return best;
}
void db_norm(dbig *r) { r->n = maxidx(r, 0, r->n); }
uint64_t db_limb(const dbig *a, size_t i) { uint64_t v; size_t g = a->off + i, d = hq(a, g); mem_dev_copy_on((int)d, &v, a->q[d] + (g - d * a->qc), 8); return v; }
uint64_t db_top(const dbig *a) { return a->n ? db_limb(a, a->n - 1) : 0; }
int db_cmp(const dbig *a, const dbig *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    size_t m = maxidx(a, b, a->n); if (!m) return 0;
    uint64_t x = db_limb(a, m - 1), y = db_limb(b, m - 1);
    return x < y ? -1 : 1;
}
void db_set_zero(dbig *r) { r->n = 0; }
void db_set_u64(dbig *r, uint64_t v) { db_reserve(r, 1); mem_dev_copy_on(0, r->q[0], &v, 8); r->n = v ? 1 : 0; }
void db_set_base_pow(dbig *r, size_t k)
{
    db_reserve(r, k + 1);
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
    for (int d = 0; d < DB_NQ; d++) { size_t lo, hi; qrange(r, d, k + 1, &lo, &hi); if (lo < hi) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemset(r->q[d], 0, (hi - lo) * 8)); HIP_CHECK(hipDeviceSynchronize()); } }
    uint64_t one = 1; size_t d = hq(r, k); mem_dev_copy_on((int)d, r->q[d] + (k - d * r->qc), &one, 8);
    r->n = k + 1;
}
dbig db_view(const dbig *a, size_t lo, size_t len)
{
    dbig v = *a; v.off = a->off + lo; v.n = len; v.cap = 0; return v;   /* not owning: never db_free it */
}
