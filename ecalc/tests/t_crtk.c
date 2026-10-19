/* t_crtk - K4b: k_crt_batch with the parallel carry (RNS_CRT_SCAN, gconst.scan = 1) against the serial chain (scan = 0).
 *
 *  1. equivalence: every limb and every stripe spill identical, scan 0 vs 1, three and four primes, on
 *     - random residues (any coefficient below the primes' product);
 *     - long carry ripples: every coefficient B - 1 with sparse 2B - 1 generators, plus an added operand of B - 1 limbs;
 *     - all residues p_i - 1 (the largest coefficient: every limb carries, every stripe spills);
 *     shapes: the C form (one product, C stripes of `rows` coefficients), ragged stripes (nc not a multiple of 256,
 *     nc < S), and a B-form batch (M products, S stripes each, products with an added operand, the top carry limb).
 *  2. timing (part 2, argument log2 q > 0): the C form of the 1-node 6.441e10 run, q = 2^logq coefficients in
 *     C = q / 8192 stripes (2^29 -> 65536 x 8192, results/KERNELS.md §S39), scan 0 vs 1, three and four primes, best and median of 7.
 *
 * Usage: t_crtk [log2 q for part 2 (29); 0 skips part 2]
 */
#include "harness.h"
#include "../rns_int.h"
#include "../modarith.h"
#include <hip/hip_runtime.h>

#define HC(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(2); } } while (0)

__device__ static inline uint64_t mix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ULL; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31);
}
/* plane p of n coefficients: mode 0 random residue, 1 the value B - 1 (sparse 2B - 1), 2 p - 1 */
__global__ void k_fill(uint64_t *pl, size_t n, uint64_t p, int mode, uint64_t seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += st) {
        uint64_t h = mix64(seed ^ (i * 0x632BE59BD9B4E019ULL)), v;
        if (mode == 0) { v = h & ((1ULL << 52) - 1); while (v >= p) v -= p; }
        else if (mode == 1) { uint64_t c = (mix64(i ^ 0x5bd1e995ULL ^ seed) % 997 == 0) ? 2 * EC_1E18 - 1 : EC_1E18 - 1; v = c % p; }
        else v = p - 1;
        pl[i] = v;
    }
}
__global__ void k_fillx(uint64_t *x, size_t n, int mode, uint64_t seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += st) x[i] = mode == 1 ? EC_1E18 - 1 : mix64(seed + i) % EC_1E18;
}

static uint64_t *g_pl[4];
static size_t g_cap;
static void planes(size_t n, int mode, uint64_t seed)
{
    if (n > g_cap) { for (int p = 0; p < 4; p++) { if (g_pl[p]) HC(hipFree(g_pl[p])); HC(hipMalloc(&g_pl[p], n * 8)); } g_cap = n; }
    for (int p = 0; p < 4; p++) k_fill<<<1024, 256>>>(g_pl[p], n, ec_P[p], mode, seed + 77 * p);
    HC(hipDeviceSynchronize());
}

/* M products; product i has nc[i] coefficients at plane offset i * Lpts, S stripes, an added operand of nx[i] limbs; run scan 0 and 1, compare */
static size_t run_cmp(int np, size_t M, const uint32_t *nc, const uint32_t *nx, size_t Lpts, int S, int mode, uint64_t seed, const char *tag)
{
    planes(M * Lpts, mode, seed);
    struct bdesc *hd = (struct bdesc *)calloc(M, sizeof *hd), *dd;
    uint64_t *out[2], *sp[2], *x = NULL; size_t tot = 0, xtot = 0, *oo = (size_t *)malloc(M * sizeof *oo), *xo = (size_t *)malloc(M * sizeof *xo);
    for (size_t i = 0; i < M; i++) { oo[i] = tot; tot += nc[i] + 1; xo[i] = xtot; xtot += nx[i]; }
    if (xtot) { HC(hipMalloc(&x, xtot * 8)); k_fillx<<<256, 256>>>(x, xtot, mode, seed); }
    for (int v = 0; v < 2; v++) { HC(hipMalloc(&out[v], tot * 8)); HC(hipMemset(out[v], 0x5A, tot * 8)); HC(hipMalloc(&sp[v], M * S * 32)); HC(hipMemset(sp[v], 0x3C, M * S * 32)); }
    HC(hipMalloc(&dd, M * sizeof *dd));
    struct gconst g = rns_gconst(); g.np = np;
    for (int v = 0; v < 2; v++) {
        for (size_t i = 0; i < M; i++) { hd[i].c = out[v] + oo[i]; hd[i].na = nc[i] - nc[i] / 3; hd[i].nb = nc[i] / 3; hd[i].nx = nx[i]; hd[i].x = nx[i] ? x + xo[i] : NULL; }
        HC(hipMemcpy(dd, hd, M * sizeof *dd, hipMemcpyHostToDevice));
        g.scan = v;
        k_crt_batch<<<(unsigned)(M * S), CRT_THREADS>>>(g_pl[0], g_pl[1], g_pl[2], g_pl[3], dd, 0, S, Lpts, g, sp[v]);
        HC(hipDeviceSynchronize());
    }
    uint64_t *h0 = (uint64_t *)malloc(tot * 8), *h1 = (uint64_t *)malloc(tot * 8), *s0 = (uint64_t *)malloc(M * S * 32), *s1 = (uint64_t *)malloc(M * S * 32);
    HC(hipMemcpy(h0, out[0], tot * 8, hipMemcpyDeviceToHost)); HC(hipMemcpy(h1, out[1], tot * 8, hipMemcpyDeviceToHost));
    HC(hipMemcpy(s0, sp[0], M * S * 32, hipMemcpyDeviceToHost)); HC(hipMemcpy(s1, sp[1], M * S * 32, hipMemcpyDeviceToHost));
    size_t bad = 0, carries = 0;
    for (size_t i = 0; i < tot; i++) bad += h0[i] != h1[i];
    for (size_t i = 0; i < M * (size_t)S * 4; i++) bad += s0[i] != s1[i];
    for (size_t i = 0; i < tot; i++) carries += h0[i] == 0 || h0[i] == EC_1E18 - 1;
    printf("  %-34s np %d M %3zu S %5d mode %d: %s (limbs %zu, B-1/0 limbs %zu)\n", tag, np, M, S, mode, bad ? "DIFFER" : "identical", tot, carries);
    VERIFY(bad == 0, "%s np %d M %zu S %d mode %d: %zu words differ (limbs %zu, B-1/0 limbs %zu)", tag, np, M, S, mode, bad, tot, carries);
    for (int v = 0; v < 2; v++) { HC(hipFree(out[v])); HC(hipFree(sp[v])); }
    if (x) HC(hipFree(x));
    HC(hipFree(dd)); free(hd); free(oo); free(xo); free(h0); free(h1); free(s0); free(s1);
    return bad;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
    int logq = argc > 1 ? atoi(argv[1]) : 29;
    HC(hipSetDevice(0));
    /* part 1: equivalence */
    for (int np = 3; np <= 4; np++)
        for (int mode = 0; mode < 3; mode++) {
            uint64_t seed = 1000 * np + mode;
            { size_t q = 1 << 20; uint32_t nc = (uint32_t)q, nx = 0; run_cmp(np, 1, &nc, &nx, q, (int)(q / 8192), mode, seed, "C form 2^20 / 128"); }
            { size_t q = 3 << 18; uint32_t nc = (uint32_t)q, nx = 0; run_cmp(np, 1, &nc, &nx, q, (int)(q / 8192), mode, seed + 1, "C form 3*2^18 / 96"); }
            { uint32_t nc = 777 * 13 + 5, nx = 0; run_cmp(np, 1, &nc, &nx, nc, 13, mode, seed + 2, "ragged 10106 / 13"); }
            { uint32_t nc = 300001, nx = 0; run_cmp(np, 1, &nc, &nx, nc, 7, mode, seed + 3, "ragged 300001 / 7"); }
            { uint32_t nc = 5, nx = 0; run_cmp(np, 1, &nc, &nx, 8, 9, mode, seed + 4, "nc 5 < S 9"); }
            { uint32_t nc = 1, nx = 1; run_cmp(np, 1, &nc, &nx, 1, 1, mode, seed + 5, "nc 1 + x"); }
            {   /* B-form batch: 24 products of varied lengths, S = 5, every other one with an added operand */
                enum { MB = 24 }; uint32_t nc[MB], nx[MB]; size_t L = 1 << 16;
                for (int i = 0; i < MB; i++) { nc[i] = (uint32_t)(L - 37 * i * i - (i & 3)); nx[i] = (i & 1) ? nc[i] - 11 * i : 0; }
                run_cmp(np, MB, nc, nx, L, 5, mode, seed + 6, "B-form batch 24 x 2^16 / 5 + x");
                run_cmp(np, MB, nc, nx, L, 1, mode, seed + 7, "B-form batch 24 x 2^16 / 1 + x");
            }
        }
    /* part 2: the C-form timing */
    if (logq > 0) {
        size_t q = (size_t)1 << logq; int C = (int)(q / 8192); if (C < 1) C = 1;
        planes(q, 0, 42);
        uint64_t *out, *sp; struct bdesc hd = { 0, 0, 0, 0, (uint32_t)q, 0, 0 }, *dd;
        HC(hipMalloc(&out, (q + 1) * 8)); HC(hipMalloc(&sp, (size_t)C * 32)); HC(hipMalloc(&dd, sizeof hd)); hd.c = out;
        HC(hipMemcpy(dd, &hd, sizeof hd, hipMemcpyHostToDevice));
        hipEvent_t e0, e1; HC(hipEventCreate(&e0)); HC(hipEventCreate(&e1));
        for (int np = 3; np <= 4; np++) {
            double ms[2][7];
            for (int rep = 0; rep < 8; rep++)
                for (int v = 0; v < 2; v++) {
                    struct gconst g = rns_gconst(); g.np = np; g.scan = v;
                    HC(hipEventRecord(e0, 0));
                    k_crt_batch<<<(unsigned)C, CRT_THREADS>>>(g_pl[0], g_pl[1], g_pl[2], g_pl[3], dd, 0, C, q, g, sp);
                    HC(hipEventRecord(e1, 0)); HC(hipEventSynchronize(e1));
                    float t; HC(hipEventElapsedTime(&t, e0, e1)); if (rep) ms[v][rep - 1] = t;   /* rep 0: warm-up */
                }
            for (int v = 0; v < 2; v++) qsort(ms[v], 7, sizeof(double), cmp_d);
            double gb = (double)q * 8 * (np + 1) / 1e9;
            printf("timing C form q 2^%d C %d np %d: scan 0 best %.3f ms median %.3f ms (%.0f GB/s) | scan 1 best %.3f ms median %.3f ms (%.0f GB/s) | ratio %.2f\n",
                   logq, C, np, ms[0][0], ms[0][3], gb / ms[0][3] * 1e3, ms[1][0], ms[1][3], gb / ms[1][3] * 1e3, ms[0][3] / ms[1][3]);
        }
    }
    return verify_done("t_crtk");
}
