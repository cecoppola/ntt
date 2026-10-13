/* t_crt24k - K4c: k_crt24 (p24_crt.c, the P24 CRT) with the parallel carry (P24_CRT_SCAN=1) against the serial chain (0).
 *
 *  1. equivalence: every out-run limb, every column spill and the device flag identical, scan 0 vs 1, on coefficients
 *     (one per point x, residues made on the host)
 *       0 random below 10^60;  1 long carry ripples: 10^24 - 1 everywhere (all limbs B - 1) with sparse 2 10^24 - 1
 *       generators (about 1 in 997);  2 one generator at each column's first point (the ripple crosses every chunk into
 *       the spill);  3 the bound 10^60 - 1 everywhere;  4 all residues p_i - 1 (past the bound: the flag is raised, the
 *       limbs must still agree);  5 a mix of 0, 10^24 - 1, 10^48 - 1, 10^60 - 1 and random;
 *     shapes: rows < 256, rows not a multiple of 256 (uneven stripes: nr ranks of R), every a mod 3, rows 1 and 3, and the
 *     production column (rows 8192).
 *  2. timing (argument log2 of the points per rank > 0): k_crt24 at rows 4096 / 8192 / 16384 (2 nodes = 8 ranks: rows = R / 8),
 *     C = points / rows, random coefficients below 10^60 (a 2^20-point pool tiled), scan 0 vs 1, best and median of 7.
 *
 * Usage: t_crt24k [log2 points per rank for part 2 (30); 0 skips part 2]
 */
#include "harness.h"
#include "../rns_int.h"
#include "../modarith.h"
#include "../p24.h"
#include <hip/hip_runtime.h>

#define HC(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(2); } } while (0)

static const uint64_t B = EC_1E18;
static uint64_t mix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ULL; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31);
}
/* the coefficient of point x as four base-B digits (d[3] < 10^6 keeps it below 10^60); returns 0, or 1 for "residues p - 1" */
static int coef(int mode, size_t x, size_t xfirst, uint64_t seed, uint64_t d[4])
{
    uint64_t h = mix64(seed ^ (x * 0x632BE59BD9B4E019ULL));
    d[0] = d[1] = d[2] = d[3] = 0;
    switch (mode) {
    case 0: d[0] = mix64(h + 1) % B; d[1] = mix64(h + 2) % B; d[2] = mix64(h + 3) % B; d[3] = mix64(h + 4) % 1000000; break;
    case 1: d[0] = B - 1; d[1] = (h % 997 == 0) ? 1999999 : 999999; break;                 /* 10^24 - 1, or 2 10^24 - 1 */
    case 2: d[0] = B - 1; d[1] = x == xfirst ? 1999999 : 999999; break;
    case 3: d[0] = d[1] = d[2] = B - 1; d[3] = 999999; break;                                /* 10^60 - 1 */
    case 4: return 1;
    default: switch (h % 5) {
        case 0: break;
        case 1: d[0] = B - 1; d[1] = 999999; break;
        case 2: d[0] = d[1] = B - 1; d[2] = 999999999999ULL; break;                          /* 10^48 - 1 */
        case 3: d[0] = d[1] = d[2] = B - 1; d[3] = 999999; break;
        default: d[0] = mix64(h + 1) % B; d[1] = mix64(h + 2) % B; d[2] = mix64(h + 3) % B; d[3] = mix64(h + 4) % 1000000; }
    }
    return 0;
}
static uint64_t resid(const uint64_t d[4], uint64_t p)
{
    unsigned __int128 r = 0;
    for (int k = 3; k >= 0; k--) r = (r * B + d[k]) % p;
    return (uint64_t)r;
}
static void setscan(int v) { setenv("P24_CRT_SCAN", v ? "1" : "0", 1); }

/* rank rho of nr over R, C columns: scan 0 and 1 on the same planes; compare */
static void run_cmp(size_t R, size_t C, int nr, int rho, int mode, uint64_t seed)
{
    size_t a = R * (size_t)rho / (size_t)nr, rows = R * (size_t)(rho + 1) / (size_t)nr - a, q = rows * C;
    struct p24_run m = p24_run_make(R, C, a, rows, 0); size_t tot = p24_run_total(&m) + 8;
    uint64_t *h[4], *dp[4];
    for (int p = 0; p < 4; p++) h[p] = (uint64_t *)malloc(q * 8);
    for (size_t j = 0; j < C; j++)
        for (size_t k = 0; k < rows; k++) {
            uint64_t d[4]; size_t x = R * j + a + k;
            int pm1 = coef(mode, x, R * j + a, seed, d);
            for (int p = 0; p < 4; p++) h[p][j * rows + k] = pm1 ? ec_P[p] - 1 : resid(d, ec_P[p]);
        }
    for (int p = 0; p < 4; p++) { HC(hipMalloc(&dp[p], q * 8)); HC(hipMemcpy(dp[p], h[p], q * 8, hipMemcpyHostToDevice)); }
    uint64_t *out[2], *sp[2], *ho[2], *hs[2]; unsigned fl[2];
    struct gconst gc = rns_gconst(); gc.np = 4;
    for (int v = 0; v < 2; v++) {
        HC(hipMalloc(&out[v], tot * 8)); HC(hipMemset(out[v], 0xA5, tot * 8)); HC(hipMalloc(&sp[v], C * 32)); HC(hipMemset(sp[v], 0x3C, C * 32));
        setscan(v);
        p24_crt_launch(dp[0], dp[1], dp[2], dp[3], &gc, R, C, a, rows, out[v], sp[v], 0);
        HC(hipDeviceSynchronize()); fl[v] = p24_crt_err(0);
        ho[v] = (uint64_t *)malloc(tot * 8); hs[v] = (uint64_t *)malloc(C * 32);
        HC(hipMemcpy(ho[v], out[v], tot * 8, hipMemcpyDeviceToHost)); HC(hipMemcpy(hs[v], sp[v], C * 32, hipMemcpyDeviceToHost));
    }
    size_t bad = 0, nines = 0;
    for (size_t i = 0; i < tot; i++) { bad += ho[0][i] != ho[1][i]; nines += ho[0][i] == B - 1; }
    for (size_t i = 0; i < C * 4; i++) bad += hs[0][i] != hs[1][i];
    printf("  R %6zu C %3zu nr %d rho %d (rows %5zu, a mod 3 = %zu) mode %d: %s (limbs %zu, B-1 limbs %zu, flag %u/%u)\n",
           R, C, nr, rho, rows, a % 3, mode, bad || fl[0] != fl[1] ? "DIFFER" : "identical", tot - 8, nines, fl[0], fl[1]);
    VERIFY(bad == 0 && fl[0] == fl[1], "R %zu C %zu nr %d rho %d mode %d: %zu words differ, flags %u / %u", R, C, nr, rho, mode, bad, fl[0], fl[1]);
    VERIFY(mode == 4 || fl[0] == 0, "mode %d: flag %u below the bound", mode, fl[0]);
    for (int v = 0; v < 2; v++) { HC(hipFree(out[v])); HC(hipFree(sp[v])); free(ho[v]); free(hs[v]); }
    for (int p = 0; p < 4; p++) { HC(hipFree(dp[p])); free(h[p]); }
}

static int cmp_d(const void *x, const void *y) { double u = *(const double *)x, w = *(const double *)y; return u < w ? -1 : u > w; }

int main(int argc, char **argv)
{
    int logq = argc > 1 ? atoi(argv[1]) : 30;
    HC(hipSetDevice(0));
    printf("t_crt24k part 1: k_crt24 scan 0 vs scan 1\n");
    struct { size_t R, C; int nr; } sh[] = { {1024, 16, 8}, {1000, 9, 3}, {4096, 6, 8}, {2048, 5, 1}, {777, 7, 1}, {65536, 12, 8}, {12, 5, 4}, {8, 3, 8}, {3000, 4, 7} };
    for (size_t i = 0; i < sizeof sh / sizeof sh[0]; i++)
        for (int mode = 0; mode < 6; mode++) {
            int rh[3] = { 0, sh[i].nr / 2, sh[i].nr - 1 };
            for (int t = 0; t < 3; t++) { if (t && rh[t] == rh[t - 1]) continue; run_cmp(sh[i].R, sh[i].C, sh[i].nr, rh[t], mode, 1000 * i + 10 * mode + t); }
        }
    if (logq > 0) {
        size_t q = (size_t)1 << logq, pool = (size_t)1 << 20;
        uint64_t *pl[4], *hp = (uint64_t *)malloc(pool * 8);
        for (int p = 0; p < 4; p++) {
            for (size_t i = 0; i < pool; i++) { uint64_t d[4]; coef(0, i, 0, 4242, d); hp[i] = resid(d, ec_P[p]); }
            HC(hipMalloc(&pl[p], q * 8));
            for (size_t o = 0; o < q; o += pool) HC(hipMemcpy(pl[p] + o, hp, (q - o < pool ? q - o : pool) * 8, hipMemcpyHostToDevice));
        }
        hipEvent_t e0, e1; HC(hipEventCreate(&e0)); HC(hipEventCreate(&e1));
        struct gconst gc = rns_gconst(); gc.np = 4;
        size_t rws[3] = { 4096, 8192, 16384 };
        for (int ri = 0; ri < 3; ri++) {
            size_t rows = rws[ri], C = q / rows, R = 8 * rows;                               /* rank 0 of 8: a = 0 */
            struct p24_run m = p24_run_make(R, C, 0, rows, 0); size_t tot = p24_run_total(&m) + 8;
            uint64_t *out, *sp; HC(hipMalloc(&out, tot * 8)); HC(hipMalloc(&sp, C * 32));
            double ms[2][7];
            for (int rep = 0; rep < 8; rep++)
                for (int v = 0; v < 2; v++) {
                    setscan(v);
                    HC(hipEventRecord(e0, 0));
                    p24_crt_launch(pl[0], pl[1], pl[2], pl[3], &gc, R, C, 0, rows, out, sp, 0);
                    HC(hipEventRecord(e1, 0)); HC(hipEventSynchronize(e1));
                    float t; HC(hipEventElapsedTime(&t, e0, e1)); if (rep) ms[v][rep - 1] = t;
                }
            VERIFY(p24_crt_err(0) == 0, "timing rows %zu: flag", rows);
            for (int v = 0; v < 2; v++) qsort(ms[v], 7, sizeof(double), cmp_d);
            double gb = (double)q * 8 * 4 / 1e9 + (double)tot * 8 / 1e9;
            printf("timing k_crt24 2^%d points rows %zu C %zu: scan 0 best %.3f ms median %.3f ms (%.0f GB/s) | scan 1 best %.3f ms median %.3f ms (%.0f GB/s) | ratio %.2f\n",
                   logq, rows, C, ms[0][0], ms[0][3], gb / ms[0][3] * 1e3, ms[1][0], ms[1][3], gb / ms[1][3] * 1e3, ms[0][3] / ms[1][3]);
            HC(hipFree(out)); HC(hipFree(sp));
        }
        for (int p = 0; p < 4; p++) HC(hipFree(pl[p]));
        free(hp);
    }
    return verify_done("t_crt24k");
}
