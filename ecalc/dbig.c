/* dbig.c - see dbig.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <hip/hip_runtime.h>
#include "dbig.h"
#include "mem.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define CH 1024                                        /* limbs per carry chunk */
static const uint64_t B10 = 1000000000000000000ULL;

struct dv { const uint64_t *q[DB_NQ]; size_t qc; int lq; };
__device__ static inline uint64_t dget(const struct dv v, size_t i) { return v.q[i >> v.lq][i & (v.qc - 1)]; }
static struct dv view_of(const dbig *a) { struct dv v; for (int d = 0; d < DB_NQ; d++) v.q[d] = a->q[d]; v.qc = a->qc; v.lq = a->lq; return v; }

void db_init(dbig *x) { memset(x, 0, sizeof *x); }
void db_free(dbig *x) { for (int d = 0; d < DB_NQ; d++) if (x->q[d]) mem_dev_free(x->q[d]); memset(x, 0, sizeof *x); }
void db_reserve(dbig *x, size_t limbs)
{
    if (limbs <= x->cap) return;
    size_t qc = 1 << 10; int lq = 10;
    while (qc * DB_NQ < limbs) { qc <<= 1; lq++; }
    dbig y; db_init(&y); y.cap = qc * DB_NQ; y.qc = qc; y.lq = lq;
    for (int d = 0; d < DB_NQ; d++) y.q[d] = (uint64_t *)mem_dev_alloc(d, qc * 8);
    if (x->n) {                                           /* keep the contents: quarter-wise DMA through the limb map */
        size_t n = x->n;
#pragma omp parallel for num_threads(DB_NQ)
        for (int d = 0; d < DB_NQ; d++) {
            size_t lo = (size_t)d * y.qc, hi = lo + y.qc; if (hi > n) hi = n;
            for (size_t i = lo; i < hi;) {                 /* the source run containing i */
                size_t sd = i >> x->lq, so = i & (x->qc - 1), run = x->qc - so; if (i + run > hi) run = hi - i;
                mem_dev_copy_on(d, y.q[d] + (i - lo), x->q[sd] + so, run * 8); i += run;
            }
        }
        y.n = n;
    }
    for (int d = 0; d < DB_NQ; d++) if (x->q[d]) mem_dev_free(x->q[d]);
    *x = y;
}
void db_from_bi(dbig *x, const bigint *a)
{
    db_reserve(x, a->n ? a->n : 1); x->n = a->n;
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) { size_t lo = (size_t)d * x->qc; if (lo < a->n) { size_t len = a->n - lo < x->qc ? a->n - lo : x->qc; mem_dev_copy_on(d, x->q[d], a->l + lo, len * 8); } }
}
void db_to_bi(bigint *r, const dbig *x)
{
    bi_reserve(r, x->n ? x->n : 1); r->n = x->n;
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) { size_t lo = (size_t)d * x->qc; if (lo < x->n) { size_t len = x->n - lo < x->qc ? x->n - lo : x->qc; mem_dev_copy_on(d, r->l + lo, x->q[d], len * 8); } }
}
/* ---- kernels: one per quarter, over the result's limbs [lo, hi) of that quarter ---- */
__global__ void k_gather_shift(uint64_t *out, size_t lo, size_t hi, struct dv a, size_t an, long shift)   /* out[i] = a[i + shift] or 0 */
{
    size_t i = lo + (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < hi; i += stride) { long s = (long)i + shift; out[i - lo] = (s >= 0 && (size_t)s < an) ? dget(a, (size_t)s) : 0; }
}
/* add/sub over one chunk per thread (blockDim 1 is deliberate: the chain is serial, the chunks are many):
 * r[i] = a[i] +/- b[i] with carry-in 0; cout = the chunk's carry-out, prop = the chunk passes a carry-in
 * through (add: every x+y == B-1; sub: every x == y) */
__global__ void k_addsub(uint64_t *out, size_t lo, size_t hi, struct dv a, size_t an, struct dv b, size_t bn, int sub, int dec, uint8_t *cout, uint8_t *prop)
{
    size_t c0 = lo + (size_t)blockIdx.x * CH; if (c0 >= hi) return;
    size_t c1 = c0 + CH < hi ? c0 + CH : hi;
    uint64_t *o = out + (c0 - lo); uint64_t cy = 0; int pr = 1;
    for (size_t i = c0; i < c1; i++) {
        uint64_t x = i < an ? dget(a, i) : 0, y = i < bn ? dget(b, i) : 0, s;
        if (dec) { if (sub) { s = x + B10 - y - cy; cy = s < B10; s = cy ? s : s - B10; pr &= (x == y); }
                   else { s = x + y + cy; cy = s >= B10; s = cy ? s - B10 : s; pr &= (x + y == B10 - 1); } }
        else { if (sub) { s = x - y - cy; cy = (x < y) || (x == y && cy); pr &= (x == y); }
               else { s = x + y + cy; cy = (s < x) || (cy && s == x); pr &= (x + y == ~0ULL); } }
        o[i - c0] = s;
    }
    cout[blockIdx.x] = (uint8_t)cy; prop[blockIdx.x] = (uint8_t)pr;
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
__global__ void k_maxidx(const uint64_t *a, const uint64_t *b, size_t len, size_t *res)   /* highest index with a != b (b may be null: a != 0); res per block */
{
    __shared__ size_t sm[256];
    size_t best = 0; int found = 0;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < len; i += (size_t)gridDim.x * blockDim.x)
        if (b ? a[i] != b[i] : a[i] != 0) { if (!found || i > best) best = i; found = 1; }
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
    if (g_flags[d][0]) { HIP_CHECK(hipFree(g_flags[d][0])); HIP_CHECK(hipFree(g_flags[d][1])); }
    HIP_CHECK(hipMallocManaged((void **)&g_flags[d][0], chunks + 16)); HIP_CHECK(hipMallocManaged((void **)&g_flags[d][1], chunks + 16));
    g_flags_cap[d] = chunks;
    if (!g_red[d]) { HIP_CHECK(hipMalloc(&g_red[d], 228 * 8 * 8)); }
    if (!g_hred) g_hred = (size_t *)malloc(228 * 8 * 8 * DB_NQ);
}
/* quarter ranges of a result with n limbs */
static void qrange(const dbig *r, int d, size_t n, size_t *lo, size_t *hi) { *lo = (size_t)d * r->qc; *hi = *lo + r->qc; if (*hi > n) *hi = n; if (*lo > n) *lo = n; }

static void shift_into(dbig *r, const dbig *a, long shift, size_t n)      /* r[i] = a[i + shift], n limbs */
{
    if (r == a) { fprintf(stderr, "db shift: in place\n"); abort(); }
    db_reserve(r, n ? n : 1);
    struct dv v = view_of(a);
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(r, d, n, &lo, &hi); if (lo >= hi) continue;
        HIP_CHECK(hipSetDevice(d));
        k_gather_shift<<<nblk(hi - lo), 256>>>(r->q[d], lo, hi, v, a->n, shift);
        HIP_CHECK(hipDeviceSynchronize());
    }
    r->n = n; db_norm(r);
}
void db_shr_limbs(dbig *r, const dbig *a, size_t k) { shift_into(r, a, (long)k, a->n > k ? a->n - k : 0); }
void db_shl_limbs(dbig *r, const dbig *a, size_t k) { shift_into(r, a, -(long)k, a->n ? a->n + k : 0); }
void db_copy(dbig *r, const dbig *a) { if (r == a) return; shift_into(r, a, 0, a->n); }

static void addsub(dbig *r, const dbig *a, const dbig *b, int sub)
{
    size_t n = a->n > b->n ? a->n : b->n; if (!sub) n++;
    dbig tmp; int inplace = (r == a || r == b); dbig *out = r;
    if (inplace) { db_init(&tmp); out = &tmp; }
    db_reserve(out, n ? n : 1);
    struct dv va = view_of(a), vb = view_of(b);
    size_t chunks[DB_NQ], lo[DB_NQ], hi[DB_NQ];
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) {
        qrange(out, d, n, &lo[d], &hi[d]); chunks[d] = (hi[d] - lo[d] + CH - 1) / CH;
        if (!chunks[d]) continue;
        flags_reserve(d, chunks[d]);
        HIP_CHECK(hipSetDevice(d));
        k_addsub<<<(unsigned)chunks[d], 1>>>(out->q[d], lo[d], hi[d], va, a->n, vb, b->n, sub, bi_decimal, g_flags[d][0], g_flags[d][1]);
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
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) {
        if (!chunks[d]) continue;
        HIP_CHECK(hipSetDevice(d));
        k_carry<<<(unsigned)chunks[d], 1>>>(out->q[d], lo[d], hi[d], g_flags[d][0], sub, bi_decimal);
        HIP_CHECK(hipDeviceSynchronize());
    }
    out->n = n; db_norm(out);
    if (inplace) { dbig sw = *r; *r = tmp; tmp = sw; db_free(&tmp); }
}
void db_add(dbig *r, const dbig *a, const dbig *b) { addsub(r, a, b, 0); }
void db_sub(dbig *r, const dbig *a, const dbig *b) { addsub(r, a, b, 1); }

static size_t maxidx(const dbig *a, const dbig *b, size_t n)      /* 1 + highest index i < n with a[i] != b[i] (b null: != 0), or 0 */
{
    size_t best = 0;
#pragma omp parallel for num_threads(DB_NQ) reduction(max:best)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(a, d, n, &lo, &hi); if (lo >= hi) continue;
        flags_reserve(d, 1);
        HIP_CHECK(hipSetDevice(d));
        unsigned blocks = nblk(hi - lo);
        k_maxidx<<<blocks, 256>>>(a->q[d], b ? b->q[d] : 0, hi - lo, g_red[d]);
        HIP_CHECK(hipMemcpy(g_hred + d * 228 * 8, g_red[d], blocks * 8, hipMemcpyDeviceToHost));
        size_t m = 0; for (unsigned i = 0; i < blocks; i++) if (g_hred[d * 228 * 8 + i] > m) m = g_hred[d * 228 * 8 + i];
        if (m) m += lo;
        if (m > best) best = m;
    }
    return best;
}
void db_norm(dbig *r) { r->n = maxidx(r, 0, r->n); }
uint64_t db_top(const dbig *a) { if (!a->n) return 0; uint64_t v; size_t i = a->n - 1; mem_dev_copy(&v, a->q[i >> a->lq] + (i & (a->qc - 1)), 8); return v; }
int db_cmp(const dbig *a, const dbig *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    if (a->qc != b->qc) { fprintf(stderr, "db_cmp: layouts differ (n %zu): not supported\n", a->n); abort(); }
    size_t m = maxidx(a, b, a->n); if (!m) return 0;
    uint64_t x, y; size_t i = m - 1;
    mem_dev_copy(&x, a->q[i >> a->lq] + (i & (a->qc - 1)), 8); mem_dev_copy(&y, b->q[i >> b->lq] + (i & (b->qc - 1)), 8);
    return x < y ? -1 : 1;
}
void db_set_zero(dbig *r) { r->n = 0; }
void db_set_u64(dbig *r, uint64_t v) { db_reserve(r, 1); mem_dev_copy(r->q[0], &v, 8); r->n = v ? 1 : 0; }
void db_set_base_pow(dbig *r, size_t k)
{
    db_reserve(r, k + 1);
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) { size_t lo, hi; qrange(r, d, k + 1, &lo, &hi); if (lo < hi) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemset(r->q[d], 0, (hi - lo) * 8)); HIP_CHECK(hipDeviceSynchronize()); } }
    uint64_t one = 1; mem_dev_copy(r->q[k >> r->lq] + (k & (r->qc - 1)), &one, 8);
    r->n = k + 1;
}
dview db_view(const dbig *a, size_t lo, size_t len)
{
    dview v; for (int d = 0; d < DB_NQ; d++) v.q[d] = a->q[d]; v.qc = a->qc; v.lq = a->lq; v.lo = lo; v.len = len; return v;
}
