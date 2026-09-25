/* ntt3.c - transforms of length 3 * 2^logk on top of the 2^k engine (WP8).
 *
 * Forward (DIF): one radix-3 stage over the thirds, x[j], x[j+m], x[j+2m]
 * (m = 2^logk) -> y_r[j] = (x[j] + w3^r x[j+m] + w3^(2r) x[j+2m]) w_n^(r j),
 * stored back in the same three places, then three length-m transforms on the
 * contiguous thirds through ntt_fwd (batch 3).  Output: third r, position t
 * (bit-reversed within the third) holds X[3 t' + r]; the pointwise product
 * only needs both operands in the same order.  Inverse: ntt_inv on the thirds
 * (each scaled by m^-1), then the mirrored stage with w_n^(-r j), w3^(-r i)
 * and 3^-1.  Twiddles w_n^j, j < m, from two tables of <= 2^16 entries
 * (w_n^(j >> 16 << 16) and w_n^(j & 0xffff)), one extra modmul per point.
 * Needs the EC_PRIMES=1 prime set (3 * 2^44 | p - 1). */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include "ntt.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)

struct r3tw { uint64_t *t1, *t2, *t1i, *t2i; };
static struct r3tw g_tw[8][EC_NP][NTT_LOGN_MAX + 1];      /* [device][prime][logk] */

static uint64_t *upload(const uint64_t *h, size_t n)
{
    uint64_t *d; HIP_CHECK(hipMalloc(&d, n * 8)); HIP_CHECK(hipMemcpy(d, h, n * 8, hipMemcpyHostToDevice)); return d;
}
static void make_tables(uint64_t **t1, uint64_t **t2, int prime, uint64_t w, int logk)
{
    uint64_t p = ec_P[prime];
    size_t n2 = logk >= 16 ? (size_t)1 << 16 : (size_t)1 << logk, n1 = logk >= 16 ? ((size_t)1 << (logk - 16)) : 1;
    uint64_t *h2 = (uint64_t *)malloc(n2 * 8), *h1 = (uint64_t *)malloc(n1 * 8), a = 1;
    for (size_t i = 0; i < n2; i++) { h2[i] = a; a = ec_mulmod_ref(a, w, p); }
    uint64_t w16 = ec_powmod(w, (uint64_t)1 << 16, p); a = 1;
    for (size_t i = 0; i < n1; i++) { h1[i] = a; a = ec_mulmod_ref(a, w16, p); }
    *t1 = upload(h1, n1); *t2 = upload(h2, n2); free(h1); free(h2);
}
static const struct r3tw *tables(int prime, int logk)
{
    int dev; HIP_CHECK(hipGetDevice(&dev));
    struct r3tw *t = &g_tw[dev][prime][logk];
    if (!t->t1) {
        if (!ec_has_radix3()) { ec_fatal(EC_RC_FATAL, "ntt3: the prime set has no 3 2^k roots (build with EC_PRIMES=1)\n"); }
        make_tables(&t->t1, &t->t2, prime, ec_root3(prime, logk), logk);
        make_tables(&t->t1i, &t->t2i, prime, ec_root3_inv(prime, logk), logk);
    }
    return t;
}

__device__ static inline uint64_t tw_at(const uint64_t *t1, const uint64_t *t2, size_t j, ec_mod m)
{
    return ec_mmu(t2[j & 0xffff], t1[j >> 16], m);
}
__device__ static inline uint64_t add3(uint64_t a, uint64_t b, uint64_t c, uint64_t p)   /* a, b, c < p -> < p */
{
    uint64_t s = ec_fold(a + b, p); return ec_fold(s + c, p);
}
/* x: batch transforms of 3m points each; one thread per (transform, j) */
__global__ void k_r3_fwd(uint64_t *x, size_t m, int logk, size_t batch, const uint64_t *t1, const uint64_t *t2, uint64_t w3, uint64_t w3s, ec_mod md)
{
    size_t total = batch * m, i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    const uint64_t p = md.pu;
    for (; i < total; i += stride) {
        size_t b = i >> logk, j = i & (m - 1); uint64_t *v = x + b * 3 * m;
        uint64_t a0 = v[j], a1 = v[j + m], a2 = v[j + 2 * m];
        uint64_t w3a1 = ec_mmu(a1, w3, md), w3sa1 = ec_mmu(a1, w3s, md), w3a2 = ec_mmu(a2, w3, md), w3sa2 = ec_mmu(a2, w3s, md);
        uint64_t y0 = add3(a0, a1, a2, p), y1 = add3(a0, w3a1, w3sa2, p), y2 = add3(a0, w3sa1, w3a2, p);
        uint64_t w = tw_at(t1, t2, j, md), w2 = ec_mmu(w, w, md);
        v[j] = y0; v[j + m] = ec_mmu(y1, w, md); v[j + 2 * m] = ec_mmu(y2, w2, md);
    }
}
__global__ void k_r3_inv(uint64_t *x, size_t m, int logk, size_t batch, const uint64_t *t1i, const uint64_t *t2i, uint64_t w3, uint64_t w3s, uint64_t inv3, ec_mod md)
{
    size_t total = batch * m, i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    const uint64_t p = md.pu;
    for (; i < total; i += stride) {
        size_t b = i >> logk, j = i & (m - 1); uint64_t *v = x + b * 3 * m;
        uint64_t wi = tw_at(t1i, t2i, j, md), wi2 = ec_mmu(wi, wi, md);
        uint64_t a0 = ec_fold(v[j], p), a1 = ec_mmu(ec_fold(v[j + m], p), wi, md), a2 = ec_mmu(ec_fold(v[j + 2 * m], p), wi2, md);
        uint64_t w3a1 = ec_mmu(a1, w3, md), w3sa1 = ec_mmu(a1, w3s, md), w3a2 = ec_mmu(a2, w3, md), w3sa2 = ec_mmu(a2, w3s, md);
        uint64_t x0 = add3(a0, a1, a2, p), x1 = add3(a0, w3sa1, w3a2, p), x2 = add3(a0, w3a1, w3sa2, p);
        v[j] = ec_mmu(x0, inv3, md); v[j + m] = ec_mmu(x1, inv3, md); v[j + 2 * m] = ec_mmu(x2, inv3, md);
    }
}
static unsigned nblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 16 ? 228 * 16 : b); }

void ntt_fwd3(ntt_ctx *c, uint64_t *x, int logk, size_t batch, hipStream_t s)
{
    int prime = ntt_ctx_prime(c); size_t m = (size_t)1 << logk; ec_mod md = ec_mod_get(prime);
    const struct r3tw *t = tables(prime, logk);
    uint64_t w3 = ec_powmod(ec_root3(prime, logk), m, ec_P[prime]), w3s = ec_mulmod_ref(w3, w3, ec_P[prime]);
    k_r3_fwd<<<nblk(batch * m), 256, 0, s>>>(x, m, logk, batch, t->t1, t->t2, w3, w3s, md);
    ntt_fwd(c, x, logk, 3 * batch, s);
}
void ntt_inv3(ntt_ctx *c, uint64_t *x, int logk, size_t batch, hipStream_t s)
{
    int prime = ntt_ctx_prime(c); size_t m = (size_t)1 << logk; ec_mod md = ec_mod_get(prime);
    const struct r3tw *t = tables(prime, logk);
    uint64_t w3 = ec_powmod(ec_root3(prime, logk), m, ec_P[prime]), w3s = ec_mulmod_ref(w3, w3, ec_P[prime]), inv3 = ec_inv(3, ec_P[prime]);
    ntt_inv(c, x, logk, 3 * batch, s);
    k_r3_inv<<<nblk(batch * m), 256, 0, s>>>(x, m, logk, batch, t->t1i, t->t2i, w3, w3s, inv3, md);
}
/* Phase 9 B1: the pointwise product in any y layout, fused into the first inverse pass (the 2^logk engine's
 * b1 pass over the thirds) from logk + 1 >= PW_FUSE on; the same modmul either way, so bit-identical */
void ntt_inv3_pw_y(ntt_ctx *c, uint64_t *x, const uint64_t *y, int ymode, int logk, size_t batch, hipStream_t s)
{
    int prime = ntt_ctx_prime(c); size_t m = (size_t)1 << logk; ec_mod md = ec_mod_get(prime);
    const struct r3tw *t = tables(prime, logk);
    uint64_t w3 = ec_powmod(ec_root3(prime, logk), m, ec_P[prime]), w3s = ec_mulmod_ref(w3, w3, ec_P[prime]), inv3 = ec_inv(3, ec_P[prime]);
    if (logk + 1 >= ntt_pw_fuse) ntt_inv3_core_pw(c, x, y, ymode, logk, batch, s);
    else { ntt_pw_y(c, x, y, ymode, 1, logk, batch, s); ntt_inv(c, x, logk, 3 * batch, s); }
    k_r3_inv<<<nblk(batch * m), 256, 0, s>>>(x, m, logk, batch, t->t1i, t->t2i, w3, w3s, inv3, md);
}
void ntt_inv3_pw(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logk, size_t batch, hipStream_t s) { ntt_inv3_pw_y(c, x, y, NTT_Y_FULL, logk, batch, s); }
void ntt_inv3_pw_bcast(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logk, size_t batch, hipStream_t s) { ntt_inv3_pw_y(c, x, y, NTT_Y_BCAST, logk, batch, s); }
