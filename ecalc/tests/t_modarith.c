/* t_modarith - step 1 test: modarith.h against __uint128_t and GMP.
 *
 *  1. roots: w33 = g^((p-1)/2^33) by GMP; ec_root(i, logn) has exact order
 *     2^logn for logn in {1, 2, 10, 20, 31, 32, 33}; ec_root_inv is its inverse
 *  2. device modmul: NPAIRS random pairs per prime for canonical x canonical
 *     and lazy [0,2p) x canonical vs (a*b) % p in 128-bit; two corrections
 *     must be exact; the one-correction failure rate is reported (paper: 0.57 %
 *     for P[1]); both-lazy is measured: inexact for P0, P1 (> 2^51.7), exact
 *     for P2, P3 (< 2^51.4) -- rule 1 stays binding for every plane
 *  3. edge values, all pairs: 0, 1, 2, p-2, p-1, p, p+1, 2p-2, 2p-1, 2^51,
 *     2^52-1, floor(p/2), floor(p/2)+1
 *  4. host ec_mm (same source, x86 fma) vs reference: 10^7 pairs per prime
 *  5. canon64: NPAIRS random 64-bit limbs per prime vs x % p; the count of
 *     second corrections must be 0; edges k p - 1, k p, k p + 1, 2^64 - 1
 *  6. Shoup lazy modmul: NPAIRS pairs w < p, y < 2^64; lazy < 2p; folded exact
 *  7. rate: 8-chain issue throughput per APU, all devices (bench/15 gave
 *     1 330 Gmodmul/s per APU; a regression here means the library changed)
 *
 * Usage: t_modarith [pairs, millions (default 1000)]
 */
#include "harness.h"
#include "../modarith.h"
#include <hip/hip_runtime.h>

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

/* one-correction variant, only to count how often it fails */
__device__ static inline double mm1(double a, double b, double p, double pinv)
{
    double hi = a * b, lo = fma(a, b, -hi), q = floor(hi * pinv);
    double r = fma(-q, p, hi) + lo;
    r += (r < 0.0 ? p : 0.0);
    r -= (r >= p ? p : 0.0);
    return r;
}
__device__ static inline uint64_t mix(uint64_t s)
{
    s ^= s >> 31; s *= 0xBF58476D1CE4E5B9ULL; s ^= s >> 29; s *= 0x94D049BB133111EBULL; return s ^ (s >> 32);
}

__global__ void k_check(ec_mod m, uint64_t seed, size_t n, int lazya, int lazyb,
                        unsigned long long *bad, unsigned long long *fail1)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    unsigned long long nb = 0, nf = 0;
    for (; i < n; i += stride) {
        uint64_t s = mix(seed ^ (i * 0x9E3779B97F4A7C15ULL));
        uint64_t a = s % (lazya * m.pu);
        uint64_t b = mix(s + 1) % (lazyb * m.pu);
        uint64_t ref = (uint64_t)(((unsigned __int128)a * b) % m.pu);
        nb += (ec_mmu(a, b, m) != ref);
        nf += ((uint64_t)mm1((double)a, (double)b, m.p, m.pinv) != ref);
    }
    if (nb) atomicAdd(bad, nb);
    if (nf) atomicAdd(fail1, nf);
}
__global__ void k_edges(ec_mod m, const uint64_t *vals, int nv, unsigned long long *bad)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= nv * nv) return;
    uint64_t a = vals[i / nv], b = vals[i % nv];
    if (b >= m.pu) return;                                  /* b must be canonical */
    uint64_t ref = (uint64_t)(((unsigned __int128)a * b) % m.pu);
    if (ec_mmu(a, b, m) != ref) atomicAdd(bad, 1ULL);
}
__global__ void k_canon(ec_mod m, uint64_t seed, size_t n, unsigned long long *bad,
                        unsigned long long *corr1, unsigned long long *corr2)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    unsigned long long nb = 0, c1 = 0, c2 = 0;
    for (; i < n; i += stride) {
        uint64_t x = mix(seed ^ (i * 0x9E3779B97F4A7C15ULL));
        if (i < 8) x = m.pu * (i + 1) - 1 + (i & 3);          /* a few k p +- 1 */
        if (i == 8) x = ~0ULL;
        uint64_t q = __umul64hi(x, m.mu) >> 51, r = x - q * m.pu;
        c1 += r >= m.pu; r -= r >= m.pu ? m.pu : 0;
        c2 += r >= m.pu;
        nb += ec_canon64(x, m.pu, m.mu) != x % m.pu;
    }
    if (nb) atomicAdd(bad, nb);
    if (c1) atomicAdd(corr1, c1);
    if (c2) atomicAdd(corr2, c2);
}
__global__ void k_shoup(ec_mod m, uint64_t seed, size_t n, unsigned long long *bad, unsigned long long *notlazy)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    unsigned long long nb = 0, nl = 0;
    for (; i < n; i += stride) {
        uint64_t s = mix(seed ^ (i * 0x9E3779B97F4A7C15ULL));
        uint64_t w = s % m.pu, y = mix(s + 1);
        uint64_t wp = (uint64_t)((((unsigned __int128)w) << 64) / m.pu);
        uint64_t ref = (uint64_t)(((unsigned __int128)w * y) % m.pu);
        uint64_t lz = ec_shoup_lazy(w, wp, y, m.pu);
        nl += lz >= 2 * m.pu;
        nb += ec_fold(lz, m.pu) != ref;
    }
    if (nb) atomicAdd(bad, nb);
    if (nl) atomicAdd(notlazy, nl);
}
#define CH 8
#define THREADS 256
__global__ __launch_bounds__(THREADS)
void k_rate(double p, double pinv, double c, int iters, double *sink)
{
    double x[CH]; int i, j;
    for (j = 0; j < CH; j++) x[j] = (double)((threadIdx.x * 7 + j * 13 + 1) % 1000);
    for (i = 0; i < iters; i++) {
#pragma unroll
        for (j = 0; j < CH; j++) x[j] = ec_mm(x[j], c, p, pinv);
    }
    double s = 0; for (j = 0; j < CH; j++) s += x[j];
    if (s == -1.0) *sink = s;
}

int main(int argc, char **argv)
{
    size_t npairs = (argc > 1 ? strtoull(argv[1], 0, 10) : 1000) * 1000000ULL;
    int nd = 0, i, li;
    HIP_CHECK(hipGetDeviceCount(&nd));
    printf("== t_modarith: %zu pairs per case, %d devices ==\n", npairs, nd);
    harness_meta("t_modarith");

    /* 1. roots via GMP */
    printf("-- roots\n");
    static const int logs[] = {1, 2, 10, 20, 31, 32, 33};
    mpz_t p, g, e, w, t;
    mpz_inits(p, g, e, w, t, NULL);
    for (i = 0; i < EC_NP; i++) {
        mpz_set_ui(p, ec_P[i]); mpz_set_ui(g, ec_G[i]);
        mpz_sub_ui(e, p, 1); mpz_fdiv_q_2exp(e, e, 33);
        mpz_powm(w, g, e, p);
        VERIFY(mpz_cmp_ui(w, ec_W33[i]) == 0, "P%d w33 table %llu, GMP %s", i, (unsigned long long)ec_W33[i], mpz_get_str(NULL, 10, w));
        if (ec_has_radix3()) {                                        /* w3x33 = g^((p-1)/(3 2^33)), exact order 3 2^33 */
            mpz_sub_ui(e, p, 1); mpz_fdiv_q_2exp(e, e, 33); VERIFY(mpz_divisible_ui_p(e, 3), "P%d: 3 | (p-1)/2^33", i); mpz_fdiv_q_ui(e, e, 3);
            mpz_powm(w, g, e, p);
            VERIFY(mpz_cmp_ui(w, ec_W3X33[i]) == 0, "P%d w3x33 table %llu, GMP %s", i, (unsigned long long)ec_W3X33[i], mpz_get_str(NULL, 10, w));
            VERIFY(ec_powmod(ec_W3X33[i], 3, ec_P[i]) == ec_W33[i], "P%d w3x33^3 == w33", i);
            VERIFY(ec_powmod(ec_W3X33[i], 3ULL << 32, ec_P[i]) != 1 && ec_powmod(ec_W3X33[i], 1ULL << 33, ec_P[i]) != 1, "P%d w3x33 has exact order 3 2^33", i);
            VERIFY(ec_powmod(ec_root3(i, 10), 3 << 10, ec_P[i]) == 1 && ec_powmod(ec_root3(i, 10), 1 << 10, ec_P[i]) != 1, "P%d root3(10) order", i);
        }
        for (li = 0; li < (int)(sizeof logs / sizeof *logs); li++) {
            int ln = logs[li];
            uint64_t r = ec_root(i, ln), ri = ec_root_inv(i, ln);
            mpz_set_ui(w, r);
            mpz_set_ui(e, 1); mpz_mul_2exp(e, e, ln); mpz_powm(t, w, e, p);
            VERIFY(mpz_cmp_ui(t, 1) == 0, "P%d root(%d)^(2^%d) != 1", i, ln, ln);
            mpz_set_ui(e, 1); mpz_mul_2exp(e, e, ln - 1); mpz_powm(t, w, e, p);
            VERIFY(mpz_cmp_ui(t, ec_P[i] - 1) == 0, "P%d root(%d) order < 2^%d", i, ln, ln);
            VERIFY(ec_mulmod_ref(r, ri, ec_P[i]) == 1, "P%d root_inv(%d)", i, ln);
        }
        VERIFY(ec_mulmod_ref(ec_root(i, 33), 1, ec_P[i]) == ec_W33[i], "root(33) == w33");
    }
    mpz_clears(p, g, e, w, t, NULL);

    /* device checks on device 0 */
    HIP_CHECK(hipSetDevice(0));
    unsigned long long *cnt; HIP_CHECK(hipMallocManaged(&cnt, 8 * sizeof *cnt));
    int blocks = 228 * 8;
    static const uint64_t edge_base[] = {0, 1, 2, 3, 1ULL << 51, (1ULL << 52) - 1};
    uint64_t *dv; HIP_CHECK(hipMallocManaged(&dv, 32 * sizeof *dv));

    for (i = 0; i < EC_NP; i++) {
        ec_mod m = ec_mod_get(i);
        printf("-- P%d = %llu\n", i, (unsigned long long)m.pu);
        /* 2. random pairs */
        static const int cases[3][2] = {{1, 1}, {2, 1}, {2, 2}};
        static const char *cname[3] = {"canon x canon", "lazy x canon ", "lazy x lazy  "};
        for (int c = 0; c < 3; c++) {
            memset(cnt, 0, 8 * sizeof *cnt);
            double t0 = now();
            k_check<<<blocks, 256>>>(m, 0xC0FFEEULL + i * 17 + c, npairs, cases[c][0], cases[c][1], cnt, cnt + 1);
            HIP_CHECK(hipDeviceSynchronize());
            double dt = now() - t0;
            printf("   %s  2-corr bad %llu   1-corr fail %.4f %%   (%.1f s)\n", cname[c], cnt[0],
                   100.0 * cnt[1] / npairs, dt);
            if (c < 2) VERIFY(cnt[0] == 0, "%s: %llu mismatches", cname[c], cnt[0]);
            else printf("   (lazy x lazy is %s for P%d -- rule 1 is binding for P0, P1 and kept for all)\n",
                        cnt[0] ? "INEXACT" : "exact", i);
            if (c == 1) harness_result(i == 1 ? "one_corr_fail_P1" : "one_corr_fail", "%", 100.0 * cnt[1] / npairs);
        }
        /* 3. edges */
        int nv = 0;
        for (size_t k = 0; k < sizeof edge_base / sizeof *edge_base; k++) dv[nv++] = edge_base[k];
        dv[nv++] = m.pu - 2; dv[nv++] = m.pu - 1; dv[nv++] = m.pu; dv[nv++] = m.pu + 1;
        dv[nv++] = 2 * m.pu - 2; dv[nv++] = 2 * m.pu - 1; dv[nv++] = m.pu / 2; dv[nv++] = m.pu / 2 + 1;
        memset(cnt, 0, 8 * sizeof *cnt);
        k_edges<<<(nv * nv + 255) / 256, 256>>>(m, dv, nv, cnt);
        HIP_CHECK(hipDeviceSynchronize());
        VERIFY(cnt[0] == 0, "edge pairs: %llu mismatches", cnt[0]);
        /* 4. host modmul */
        {
            rng_t r = {0x5EEDULL + i}; unsigned long long bad = 0, n = 10000000;
#pragma omp parallel for reduction(+:bad)
            for (unsigned long long k = 0; k < n; k++) {
                rng_t rk = {r.s + k * 0x1234567ULL};
                uint64_t a = rng_next(&rk) % (2 * m.pu), b = rng_next(&rk) % m.pu;
                bad += ec_mmu(a, b, m) != ec_mulmod_ref(a, b, m.pu);
            }
            VERIFY(bad == 0, "host ec_mm: %llu mismatches of %llu", bad, n);
        }
        /* 5. canon64 */
        memset(cnt, 0, 8 * sizeof *cnt);
        k_canon<<<blocks, 256>>>(m, 0xCA0ULL + i, npairs, cnt, cnt + 1, cnt + 2);
        HIP_CHECK(hipDeviceSynchronize());
        printf("   canon64: bad %llu, first corrections %.4f %%, second %llu\n", cnt[0], 100.0 * cnt[1] / npairs, cnt[2]);
        VERIFY(cnt[0] == 0, "canon64: %llu mismatches", cnt[0]);
        VERIFY(cnt[2] == 0, "canon64 needed a second correction %llu times", cnt[2]);
        /* 6. shoup */
        memset(cnt, 0, 8 * sizeof *cnt);
        k_shoup<<<blocks, 256>>>(m, 0x5A0ULL + i, npairs, cnt, cnt + 1);
        HIP_CHECK(hipDeviceSynchronize());
        VERIFY(cnt[0] == 0, "shoup: %llu mismatches", cnt[0]);
        VERIFY(cnt[1] == 0, "shoup lazy result >= 2p %llu times", cnt[1]);
    }

    /* 7. rate on every device */
    printf("-- rate (8 chains, P0)\n");
    {
        ec_mod m = ec_mod_get(0);
        int iters = 20000;
        double tot = 0;
        for (int d = 0; d < nd; d++) {
            HIP_CHECK(hipSetDevice(d));
            double *sink; HIP_CHECK(hipMalloc(&sink, 8));
            hipEvent_t a, b; HIP_CHECK(hipEventCreate(&a)); HIP_CHECK(hipEventCreate(&b));
            k_rate<<<228 * 8, THREADS>>>(m.p, m.pinv, 12345.0, 100, sink);
            HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipEventRecord(a));
            k_rate<<<228 * 8, THREADS>>>(m.p, m.pinv, 12345.0, iters, sink);
            HIP_CHECK(hipEventRecord(b)); HIP_CHECK(hipEventSynchronize(b));
            float ms; HIP_CHECK(hipEventElapsedTime(&ms, a, b));
            double rate = (double)228 * 8 * THREADS * CH * iters / (ms * 1e-3) / 1e9;
            printf("   APU%d %.0f Gmodmul/s\n", d, rate);
            tot += rate;
            HIP_CHECK(hipFree(sink));
        }
        harness_result("modmul_rate_per_apu", "Gmodmul/s", tot / nd);
        VERIFY(tot / nd > 1000, "modmul rate %.0f < 1000 Gmodmul/s (bench/15: 1330)", tot / nd);
    }
    return verify_done("t_modarith");
}
