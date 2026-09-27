/* L8 (Phase 15, throwaway): what a radix-r pass costs (r = 3, 5, 7, 15) in the style of ntt3.c's k_r3_fwd -- the forward DIF
 * stage over the r parts of a length r 2^k transform: y_s[j] = (sum_i x[j + i m] W_r^(s i)) w_n^(s j), m = 2^k, stored back in
 * place, one thread per (transform, j), the canonical FP64-Barrett ec_mmu (modarith.h) as in ntt3.c.  Radix 15 two ways: the
 * naive 15-point DFT and Cooley-Tukey 3 x 5 (5 DFT-3, 8 twiddles, 3 DFT-5; natural order).  A copy kernel (read r, write r)
 * gives the memory ceiling.  Checked against the direct formula on the host (small m, batch 2) before timing.
 * Primes: p0 = 15 2^48 + 1 (today's c = 240: 3, 5, 15) and 3465 2^40 + 1 (105 | p - 1: radix 7).
 * Build on aac6:  module load rocm; hipcc -O3 -I ecalc tests/l8_rbench.c -o tests/l8_rbench
 * Run:  ./l8_rbench  (APU0; median of 7; points per call about 2^30 for every radix) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <hip/hip_runtime.h>
#include "modarith.h"
#define HC(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

struct rc { uint64_t W[15]; };                  /* W_r^e, e < r (canonical) */
__device__ __host__ static inline uint64_t tw_at(const uint64_t *t1, const uint64_t *t2, size_t j, ec_mod m) { return ec_mmu(t2[j & 0xffff], t1[j >> 16], m); }
__device__ __host__ static inline uint64_t addm(uint64_t a, uint64_t b, uint64_t p) { return ec_fold(a + b, p); }

template <int R> __device__ __host__ static inline void dft(uint64_t *a, const uint64_t *W, int wstep, int wmod, ec_mod md)
{                                               /* naive R-point DFT in place, W = the powers of an order-wmod root, used at stride wstep */
    uint64_t y[R];
    for (int s = 0; s < R; s++) {
        uint64_t acc = a[0];
        for (int i = 1; i < R; i++) { int e = (s * i) % R; acc = addm(acc, e ? ec_mmu(a[i], W[(e * wstep) % wmod], md) : a[i], md.pu); }
        y[s] = acc;
    }
    for (int s = 0; s < R; s++) a[s] = y[s];
}
__device__ __host__ static inline void dft15ct(uint64_t *x, const uint64_t *W, ec_mod md)
{                                               /* N = 3 x 5: n = 5 n1 + n2, k = k1 + 3 k2 (natural order out) */
    uint64_t Y[5][3], X[15];
    for (int n2 = 0; n2 < 5; n2++) {
        uint64_t a[3] = { x[n2], x[5 + n2], x[10 + n2] };
        dft<3>(a, W, 5, 15, md);                 /* W_3 = W_15^5 */
        for (int k1 = 0; k1 < 3; k1++) Y[n2][k1] = (n2 && k1) ? ec_mmu(a[k1], W[n2 * k1], md) : a[k1];
    }
    for (int k1 = 0; k1 < 3; k1++) {
        uint64_t b[5] = { Y[0][k1], Y[1][k1], Y[2][k1], Y[3][k1], Y[4][k1] };
        dft<5>(b, W, 3, 15, md);                 /* W_5 = W_15^3 */
        for (int k2 = 0; k2 < 5; k2++) X[k1 + 3 * k2] = b[k2];
    }
    for (int s = 0; s < 15; s++) x[s] = X[s];
}
template <int R, int CT> __global__ void k_rpass(uint64_t *x, size_t m, int logk, size_t batch, const uint64_t *t1, const uint64_t *t2, struct rc c, ec_mod md)
{
    size_t total = batch * m, i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < total; i += stride) {
        size_t b = i >> logk, j = i & (m - 1); uint64_t *v = x + b * R * m, a[R];
        for (int r = 0; r < R; r++) a[r] = v[j + r * m];
        if (CT) dft15ct(a, c.W, md); else dft<R>(a, c.W, 1, R, md);
        uint64_t w = tw_at(t1, t2, j, md), ws = w;
        v[j] = a[0];
        for (int s = 1; s < R; s++) { v[j + s * m] = ec_mmu(a[s], ws, md); if (s + 1 < R) ws = ec_mmu(ws, w, md); }
    }
}
template <int R> __global__ void k_copy(uint64_t *x, size_t m, int logk, size_t batch)
{
    size_t total = batch * m, i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < total; i += stride) {
        size_t b = i >> logk, j = i & (m - 1); uint64_t *v = x + b * R * m, a[R];
        for (int r = 0; r < R; r++) a[r] = v[j + r * m];
        for (int r = 0; r < R; r++) v[j + r * m] = a[(r + 1) % R];
    }
}
__global__ void k_fill(uint64_t *x, size_t n, uint64_t p) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; for (; i < n; i += (size_t)gridDim.x * blockDim.x) x[i] = (i * 0x9E3779B97F4A7C15ull >> 7) % p; }

static ec_mod mod_of(uint64_t p) { ec_mod m; m.pu = p; m.mu = 0; m.idx = 0; m.p = (double)p; m.pinv = 1.0 / m.p; return m; }
static uint64_t gen_of(uint64_t p) { return p == ec_P[0] ? 19 : 19; }   /* 19 is a primitive root of both (tests/l8_primes.py) */
static unsigned nblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 16 ? 228 * 16 : b); }

struct tabs { uint64_t *t1, *t2; uint64_t *h1, *h2; };
static struct tabs make(uint64_t p, uint64_t w, int logk)
{
    struct tabs t; size_t n2 = logk >= 16 ? (size_t)1 << 16 : (size_t)1 << logk, n1 = logk >= 16 ? ((size_t)1 << (logk - 16)) : 1;
    t.h2 = (uint64_t *)malloc(n2 * 8); t.h1 = (uint64_t *)malloc(n1 * 8); uint64_t a = 1;
    for (size_t i = 0; i < n2; i++) { t.h2[i] = a; a = ec_mulmod_ref(a, w, p); }
    uint64_t w16 = ec_powmod(w, (uint64_t)1 << 16, p); a = 1;
    for (size_t i = 0; i < n1; i++) { t.h1[i] = a; a = ec_mulmod_ref(a, w16, p); }
    HC(hipMalloc(&t.t1, n1 * 8)); HC(hipMalloc(&t.t2, n2 * 8));
    HC(hipMemcpy(t.t1, t.h1, n1 * 8, hipMemcpyHostToDevice)); HC(hipMemcpy(t.t2, t.h2, n2 * 8, hipMemcpyHostToDevice));
    return t;
}
template <int R, int CT> static void launch(uint64_t *x, size_t m, int logk, size_t batch, struct tabs t, struct rc c, ec_mod md)
{ k_rpass<R, CT><<<nblk(batch * m), 256>>>(x, m, logk, batch, t.t1, t.t2, c, md); }
static void run(int R, int CT, uint64_t *x, size_t m, int logk, size_t batch, struct tabs t, struct rc c, ec_mod md)
{
    if (R == 3) launch<3, 0>(x, m, logk, batch, t, c, md);
    else if (R == 5) launch<5, 0>(x, m, logk, batch, t, c, md);
    else if (R == 7) launch<7, 0>(x, m, logk, batch, t, c, md);
    else if (CT) launch<15, 1>(x, m, logk, batch, t, c, md);
    else launch<15, 0>(x, m, logk, batch, t, c, md);
}
static void copy(int R, uint64_t *x, size_t m, int logk, size_t batch)
{
    unsigned g = nblk(batch * m);
    if (R == 3) k_copy<3><<<g, 256>>>(x, m, logk, batch); else if (R == 5) k_copy<5><<<g, 256>>>(x, m, logk, batch);
    else if (R == 7) k_copy<7><<<g, 256>>>(x, m, logk, batch); else k_copy<15><<<g, 256>>>(x, m, logk, batch);
}
static void setup(int R, uint64_t p, int logk, struct rc *c, struct tabs *t)
{
    uint64_t g = gen_of(p), wn = ec_powmod(g, (p - 1) / ((uint64_t)R << logk), p), WR = ec_powmod(wn, (uint64_t)1 << logk, p), a = 1;
    int nW = R == 15 ? 15 : R;
    for (int e = 0; e < nW; e++) { c->W[e] = a; a = ec_mulmod_ref(a, WR, p); }
    *t = make(p, wn, logk);
}
static int check(int R, int CT, uint64_t p)
{                                               /* m = 8, batch 2 against the direct formula */
    int logk = 3; size_t m = 8, batch = 2, n = batch * R * m; struct rc c; struct tabs t; setup(R, p, logk, &c, &t); ec_mod md = mod_of(p);
    uint64_t *h = (uint64_t *)malloc(n * 8), *o = (uint64_t *)malloc(n * 8), *d; for (size_t i = 0; i < n; i++) h[i] = (i * 0x9E3779B97F4A7C15ull >> 5) % p;
    HC(hipMalloc(&d, n * 8)); HC(hipMemcpy(d, h, n * 8, hipMemcpyHostToDevice)); run(R, CT, d, m, logk, batch, t, c, md); HC(hipDeviceSynchronize());
    HC(hipMemcpy(o, d, n * 8, hipMemcpyDeviceToHost));
    uint64_t wn = ec_powmod(gen_of(p), (p - 1) / ((uint64_t)R << logk), p), WR = ec_powmod(wn, m, p); int bad = 0;
    for (size_t b = 0; b < batch; b++) for (size_t j = 0; j < m; j++) for (int s = 0; s < R; s++) {
        uint64_t acc = 0; for (int i = 0; i < R; i++) acc = (acc + ec_mulmod_ref(h[b * R * m + j + i * m], ec_powmod(WR, (uint64_t)(s * i), p), p)) % p;
        acc = ec_mulmod_ref(acc, ec_powmod(wn, (uint64_t)s * j, p), p);
        if (acc != o[b * R * m + j + s * m] % p) bad++;
    }
    HC(hipFree(d)); free(h); free(o); return bad;
}
int main(void)
{
    HC(hipSetDevice(0));
    uint64_t p7 = 3809807790243841ULL;
    struct { int R, CT; uint64_t p; const char *name; } K[] = { {3, 0, ec_P[0], "r3"}, {5, 0, ec_P[0], "r5"}, {7, 0, p7, "r7"}, {15, 1, ec_P[0], "r15 CT 3x5"}, {15, 0, ec_P[0], "r15 naive"} };
    for (int i = 0; i < 5; i++) printf("check %-11s: %d mismatches (m 8, batch 2)\n", K[i].name, check(K[i].R, K[i].CT, K[i].p));
    size_t cap = (size_t)15 << 27; uint64_t *x; HC(hipMalloc(&x, cap * 8)); k_fill<<<4096, 256>>>(x, cap, ec_P[0]); HC(hipDeviceSynchronize());
    int ks[] = {10, 17, 22, 26};
    printf("pass | 2^k | points | pass ms | ps/point | GB/s (16 B/point) | copy ms | copy GB/s | pass / copy\n");
    for (int i = 0; i < 5; i++) for (int q = 0; q < 4; q++) {
        int R = K[i].R, logk = ks[q]; size_t m = (size_t)1 << logk, batch = ((size_t)1 << 30) / ((size_t)R * m);
        if (!batch) batch = 1;
        size_t pts = batch * R * m; if (pts > cap) continue;
        struct rc c; struct tabs t; setup(R, K[i].p, logk, &c, &t); ec_mod md = mod_of(K[i].p);
        double a[7], b[7];
        for (int r = 0; r < 7; r++) {
            double t0 = now(); run(R, K[i].CT, x, m, logk, batch, t, c, md); HC(hipDeviceSynchronize()); a[r] = now() - t0;
            t0 = now(); copy(R, x, m, logk, batch); HC(hipDeviceSynchronize()); b[r] = now() - t0;
        }
        qsort(a, 7, 8, cmpd); qsort(b, 7, 8, cmpd);
        printf("%-11s | %2d | %10zu | %7.3f | %6.2f | %6.0f | %7.3f | %6.0f | %.2f\n", K[i].name, logk, pts, 1e3 * a[3], 1e12 * a[3] / pts, 16.0 * pts / a[3] / 1e9,
               1e3 * b[3], 16.0 * pts / b[3] / 1e9, a[3] / b[3]);
        HC(hipFree(t.t1)); HC(hipFree(t.t2)); free(t.h1); free(t.h2);
    }
    return 0;
}
