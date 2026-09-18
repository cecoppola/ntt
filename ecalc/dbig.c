/* dbig.c - see dbig.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
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

struct dv { const uint64_t *q[DB_NQ]; size_t qc, off, shift; int lq; };            /* shift: the operand as a << shift limbs */
__device__ static inline uint64_t dget(const struct dv v, size_t i) { if (i < v.shift) return 0; size_t g = v.off + i - v.shift; return v.q[g >> v.lq][g & (v.qc - 1)]; }
static struct dv view_of(const dbig *a) { struct dv v; for (int d = 0; d < DB_NQ; d++) v.q[d] = a->q[d]; v.qc = a->qc; v.lq = a->lq; v.off = a->off; v.shift = 0; return v; }
static void need_owner(const dbig *r, const char *what) { if (r->off || (!r->cap && r->n)) { fprintf(stderr, "dbig: %s into a view\n", what); abort(); } }

__global__ void k_touch(uint64_t *p, size_t n) { size_t step = (1 << 21) / 8; for (size_t i = (size_t)threadIdx.x * step; i < n; i += step * blockDim.x) { uint64_t v = p[i]; if (v == 0x123456789ULL) p[i] = v; } }
/* quarter blocks come from per-device free lists by size class (hipMalloc costs ~0.06 s/GB and the Newton
 * loop allocates and frees temporaries every iteration); db_release_pools gives everything back */
static struct { uint64_t *p[64]; int n; } g_free[DB_NQ][40];
static size_t g_pool_bytes;
/* quarter blocks bypass mem's pointer registry (every dbig op addresses its quarters explicitly) */
static uint64_t *q_alloc(int d, int lq)
{
    if (g_free[d][lq].n) return g_free[d][lq].p[--g_free[d][lq].n];
    g_pool_bytes += ((size_t)8 << lq);
    int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d));
    void *p; HIP_CHECK(hipMalloc(&p, (size_t)8 << lq)); HIP_CHECK(hipMemset(p, 0, (size_t)8 << lq)); HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipSetDevice(cur));
    return (uint64_t *)p;
}
static void q_release(int d, uint64_t *p) { int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipFree(p)); HIP_CHECK(hipSetDevice(cur)); }
static void q_free(int d, int lq, uint64_t *p)
{
    if (g_free[d][lq].n < 64) g_free[d][lq].p[g_free[d][lq].n++] = p; else { q_release(d, p); g_pool_bytes -= ((size_t)8 << lq); }
}
void db_release_pools(void)
{
    for (int d = 0; d < DB_NQ; d++) for (int l = 0; l < 40; l++) { while (g_free[d][l].n) { q_release(d, g_free[d][l].p[--g_free[d][l].n]); g_pool_bytes -= ((size_t)8 << l); } }
}
size_t db_pool_bytes(void) { return g_pool_bytes; }
void db_init(dbig *x) { par_init(); memset(x, 0, sizeof *x); }
void db_free(dbig *x) { if (x->cap) for (int d = 0; d < DB_NQ; d++) if (x->q[d]) q_free(d, x->lq, x->q[d]); memset(x, 0, sizeof *x); }
void db_reserve(dbig *x, size_t limbs)
{
    if (limbs <= x->cap) return;
    double t0 = tnow(); db_st.n_reserve++;
    if (x->off) { fprintf(stderr, "db_reserve: a view\n"); abort(); }
    size_t qc = 1 << 10; int lq = 10;
    while (qc * DB_NQ < limbs) { qc <<= 1; lq++; }
    dbig y; db_init(&y); y.cap = qc * DB_NQ; y.qc = qc; y.lq = lq;
    for (int d = 0; d < DB_NQ; d++) y.q[d] = q_alloc(d, lq);
    if (getenv("DBIG_WARM")) {                             /* touch every 2 MiB page of each quarter from every other device */
        for (int d = 0; d < DB_NQ; d++) for (int c = 0; c < DB_NQ; c++) if (c != d) {
            HIP_CHECK(hipSetDevice(c));
            k_touch<<<1, 256>>>(y.q[d], qc);
            HIP_CHECK(hipDeviceSynchronize());
        }
        HIP_CHECK(hipSetDevice(0));
    }
    if (x->n) {                                           /* keep the contents: quarter-wise DMA through the limb map */
        size_t n = x->n;
        for (int d = 0; d < DB_NQ; d++) {
            size_t lo = (size_t)d * y.qc, hi = lo + y.qc; if (hi > n) hi = n;
            for (size_t i = lo; i < hi;) {                 /* the source run containing i */
                size_t sd = i >> x->lq, so = i & (x->qc - 1), run = x->qc - so; if (i + run > hi) run = hi - i;
                mem_dev_copy_on(d, y.q[d] + (i - lo), x->q[sd] + so, run * 8); i += run;
            }
        }
        y.n = n;
    }
    for (int d = 0; d < DB_NQ; d++) if (x->q[d]) q_free(d, x->lq, x->q[d]);
    *x = y;
    db_st.t_reserve += tnow() - t0;
}
void db_from_bi(dbig *x, const bigint *a)
{
    db_reserve(x, a->n ? a->n : 1); x->n = a->n;
    for (int d = 0; d < DB_NQ; d++)                       /* serial: concurrent hipMemcpy with pageable host memory faults (RESULTS.md 59) */ { size_t lo = (size_t)d * x->qc; if (lo < a->n) { size_t len = a->n - lo < x->qc ? a->n - lo : x->qc; mem_dev_copy_on(d, x->q[d], a->l + lo, len * 8); } }
}
void db_to_bi(bigint *r, const dbig *x)
{
    bi_reserve(r, x->n ? x->n : 1); r->n = x->n;
    for (int d = 0; d < DB_NQ; d++) { size_t lo = (size_t)d * x->qc; if (lo < x->n) { size_t len = x->n - lo < x->qc ? x->n - lo : x->qc; mem_dev_copy_on(d, r->l + lo, x->q[d], len * 8); } }
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
        HIP_CHECK(hipDeviceSynchronize());
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
    dbig tmp; int inplace = (r == a || r == b), same = (r == a && !r->off && !ashift) || (b && r == b && !r->off && a->qc == b->qc && !ashift);
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
        HIP_CHECK(hipDeviceSynchronize());
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
        HIP_CHECK(hipDeviceSynchronize());
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
        HIP_CHECK(hipDeviceSynchronize());
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
uint64_t db_limb(const dbig *a, size_t i) { uint64_t v; size_t g = a->off + i; mem_dev_copy_on((int)(g >> a->lq), &v, a->q[g >> a->lq] + (g & (a->qc - 1)), 8); return v; }
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
    uint64_t one = 1; mem_dev_copy_on((int)(k >> r->lq), r->q[k >> r->lq] + (k & (r->qc - 1)), &one, 8);
    r->n = k + 1;
}
dbig db_view(const dbig *a, size_t lo, size_t len)
{
    dbig v = *a; v.off = a->off + lo; v.n = len; v.cap = 0; return v;   /* not owning: never db_free it */
}
