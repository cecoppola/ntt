/* t_arith - the arithmetic of modarith.h and the prime set, one binary (was t_modarith, t_roots, t_primes).
 *   t_arith [all [PAIRS_M]]   roots, primes (parts 1-3, no GPU), modarith (default 100 million pairs per case)
 *   t_arith modarith [PAIRS_M]  modarith.h against __uint128_t and GMP on the devices (default 1000 million pairs)
 *   t_arith roots               the roots of unity above 2^33 (ec_root / ec_root3), host only
 *   t_arith primes [gpu [LOGMAX]]  the 3-prime bound and Garner against GMP (parts 1-3); `gpu`: part 4, end to end on the GPUs (ECALC_NP=3|4)
 * The three sections keep the descriptions of their original files below. */
#include "harness.h"
#include "../modarith.h"
#include "../crt.h"
#include "../mem.h"
#include "../rns_mul.h"
#include "../dbig.h"
#include "tref.h"
#include <hip/hip_runtime.h>
#include <math.h>
#include <omp.h>
#include <fcntl.h>
#include <sys/wait.h>
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

/* ===== modarith ===== (was t_modarith)
 t_modarith - step 1 test: modarith.h against __uint128_t and GMP.
 *
 *  1. roots: w33 = g^((p-1)/2^33) by GMP; ec_root(i, logn) has exact order
 *     2^logn for logn in {1, 2, 10, 20, 31, 32, 33}; ec_root_inv is its inverse
 *  2. device modmul: NPAIRS random pairs per prime for canonical x canonical
 *     and lazy [0,2p) x canonical vs (a*b) % p in 128-bit; two corrections
 *     must be exact; the one-correction failure rate is reported (measured: 0.57 %
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
 * Usage: t_arith modarith [pairs, millions (default 1000)]
 */
/* ===== roots ===== (was t_roots)
 t_roots - the roots of unity above 2^33 (modarith.h ec_root / ec_root3), host only.
 *
 * ec_root(i, logn) = w33^(2^(33 - logn)): a negative shift for logn > 33 (agent L8 measured the "root" = 1
 * for logn 34..41 on p0, p1, p2), reached by the mn tier's planes from 8 nodes on (2^40 at 576).  Checks, for every prime:
 *   1. bit-identity: ec_root(i, logn) == the old formula for logn 0..33, ec_root3(i, logk) == the old one for logk 0..33
 *      (the old code copied here: the values every transform so far has used);
 *   2. the derivation: ec_root(i, logn) == g^((p-1) / 2^logn) by GMP (mpz_powm) for logn 0..v2(p-1), and ec_root3 ==
 *      g^((p-1) / (3 2^logk)) for logk 0..v2(p-1);
 *   3. the order: w^(2^logn) = 1 and w^(2^(logn-1)) = p - 1 (exact order 2^logn), logn 1..v2(p-1); root3: w^(3 2^logk) = 1,
 *      w^(3 2^(logk-1)) = p - 1 (logk >= 1) and w^(2^logk) != 1 (the factor 3);
 *   4. the chain: root(k+1)^2 = root(k) across the table/generator boundary (33 -> 34), root3(k)^3 = root(k), inverses;
 *   5. the refusal: ec_root(i, v2 + 1) and ec_root3(i, v2 + 1) stop the process through ec_fatal (rc 3), in a child;
 *   6. ec_logn_limit(3) = ec_logn_limit(4) = 44 (the WP8 set), and L8's measurement reproduced for the old formula
 *      (logn 34 gives 1 or garbage, never a root of order 2^34) -- only printed.
 * Usage: t_arith roots (host code only, but linked with the rest of t_arith by make) */
/* ===== primes ===== (was t_primes)
 t_primes - three primes instead of four for base-10^18 limbs.
 *
 *  1. the bound, exact (GMP): for every 3-prime subset of c 2^44 + 1, c = 240, 216, 207, 147, the product and the
 *     margin prod p / (n (B-1)^2) at n = 2^20 .. 2^33 terms and at the 3 2^k lengths 3 2^31 .. 3 2^33; binary 2^64 limbs
 *     for contrast; the subset of the pipeline ({0,1,2} = c 240, 216, 207) must be the best and above 1 at every length
 *     the primes allow; ec_np3_max_terms against GMP's floor((p0 p1 p2 - 1) / (B-1)^2)
 *  2. Garner from three residues at the extremes: v = n (B-1)^2 for n = 2^20 .. 2^33, the largest allowed
 *     (max_terms (B-1)^2), p0 p1 p2 - 1, and 10^6 random v < p0 p1 p2: crt_garner3 and ec_words_to_dec3 against GMP
 *  3. crt_carry_par4 with ec_np = 3 (decimal): planes whose every coefficient is one extreme value W (the 2^33 worst case,
 *     the largest allowed, p0 p1 p2 - 1) at 2^16 coefficients and T = 1, 7, 96, 192 against GMP; the exact convolution
 *     profile of two all-(B-1) operands of 2^22 limbs (peak 2^22 (B-1)^2) against the closed form (B^n - 1)^2; and the
 *     three-prime result against the four-prime one on the same coefficients
 *  4. (argument gpu [log2 of the largest dist operand, 30]) end to end on the GPUs at the ec_np of the environment
 *     (ECALC_NP=3 or 4, decimal limbs): all-(B-1) operands (every coefficient at its maximum, the worst case) n x n
 *     through the device path (rns_mul_dist_db) up to n = 2^30 (2^31 points, peak coefficient 2^30 (B-1)^2) at two plane
 *     sizes -- the default (one plane) and, to n = 2^29, half of it (DIST_LOGN_TEST: a 2-piece grid; TREF replaced the removed
 *     host tier rns_mul by it) -- checked against the closed form B^2n - 2 B^n + 1 (GMP-checked itself at n <= 2^20) and
 *     against each other; random operands through both and the batch tier against the test reference (tests/tref.h)
 *
 * Usage: t_arith primes [gpu [LOGMAX (30)]]      (parts 1-3 need no GPU; ECALC_NP is read by part 4 only)
 */

/* ---------- modarith ---------- */


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

static int run_modarith(size_t npairs)
{
    int nd = 0, i, li;
    HIP_CHECK(hipGetDeviceCount(&nd));
    printf("== t_modarith: %zu pairs per case, %d devices ==\n", npairs, nd);

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
    return 0;
}

/* ---------- roots ---------- */

static uint64_t old_root(int i, int logn) { return ec_powmod(ec_W33[i], 1ULL << (EC_LOGN_MAX - logn), ec_P[i]); }
static uint64_t old_root3(int i, int logk) { return ec_W3X33[i] ? ec_powmod(ec_W3X33[i], 1ULL << (EC_LOGN_MAX - logk), ec_P[i]) : 0; }
static uint64_t gmp_pow(uint64_t g, uint64_t e, uint64_t p)
{
    mpz_t b, x, m; mpz_init_set_ui(b, g); mpz_init_set_ui(x, e); mpz_init_set_ui(m, p);
    mpz_powm(b, b, x, m); uint64_t r = mpz_get_ui(b); mpz_clear(b); mpz_clear(x); mpz_clear(m); return r;
}
/* 1 when calling f(i, k) in a child process ends with exit code 3 (ec_fatal) */
static int refuses(uint64_t (*f)(int, int), int i, int k)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { int fd = open("/dev/null", O_WRONLY); if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); } (void)f(i, k); _exit(0); }
    int st = 0; waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == EC_RC_FATAL;
}
static int run_roots(void)
{
    printf("t_roots: EC_LOGN_MAX %d (the table order), ec_logn_limit(3) %d, ec_logn_limit(4) %d\n", EC_LOGN_MAX, ec_logn_limit(3), ec_logn_limit(4));
    VERIFY(ec_logn_limit(3) == 44 && ec_logn_limit(4) == 44, "the WP8 set's 2-adic limit is 44: %d, %d", ec_logn_limit(3), ec_logn_limit(4));
    for (int i = 0; i < EC_NP; i++) {
        uint64_t p = ec_P[i], g = ec_G[i]; int v2 = ec_v2(i), n1 = 0, n2 = 0, n3 = 0;
        VERIFY(((p - 1) >> v2) & 1, "prime %d: v2", i);
        printf("prime %d: p = %llu = %llu * 2^%d + 1, g = %llu\n", i, (unsigned long long)p, (unsigned long long)((p - 1) >> v2), v2, (unsigned long long)g);
        /* 1. bit-identity for logn <= 33 */
        for (int k = 0; k <= EC_LOGN_MAX; k++) {
            VERIFY(ec_root(i, k) == old_root(i, k), "prime %d: root(%d) %llu, old %llu", i, k, (unsigned long long)ec_root(i, k), (unsigned long long)old_root(i, k));
            VERIFY(ec_root_inv(i, k) == ec_inv(old_root(i, k), p), "prime %d: root_inv(%d)", i, k);
            if (ec_has_radix3()) {
                VERIFY(ec_root3(i, k) == old_root3(i, k), "prime %d: root3(%d) %llu, old %llu", i, k, (unsigned long long)ec_root3(i, k), (unsigned long long)old_root3(i, k));
                VERIFY(ec_root3_inv(i, k) == ec_inv(old_root3(i, k), p), "prime %d: root3_inv(%d)", i, k);
            }
            n1++;
        }
        /* 2-4. the derivation, the order and the chain up to v2 */
        for (int k = 0; k <= v2; k++) {
            uint64_t w = ec_root(i, k);
            VERIFY(w == gmp_pow(g, (p - 1) >> k, p), "prime %d: root(%d) == g^((p-1)/2^%d) (GMP)", i, k, k);
            VERIFY(ec_powmod(w, 1ULL << k, p) == 1, "prime %d: root(%d)^(2^%d) = 1", i, k, k);
            if (k >= 1) VERIFY(ec_powmod(w, 1ULL << (k - 1), p) == p - 1, "prime %d: root(%d)^(2^%d) = p - 1 (exact order)", i, k, k - 1);
            if (k >= 1) VERIFY(ec_mulmod_ref(w, w, p) == ec_root(i, k - 1), "prime %d: root(%d)^2 = root(%d)", i, k, k - 1);
            VERIFY(ec_mulmod_ref(w, ec_root_inv(i, k), p) == 1, "prime %d: root(%d) root_inv(%d) = 1", i, k, k);
            n2++;
            if (ec_has_radix3() && ((p - 1) >> k) % 3 == 0) {
                uint64_t w3 = ec_root3(i, k);
                VERIFY(w3 == gmp_pow(g, ((p - 1) >> k) / 3, p), "prime %d: root3(%d) == g^((p-1)/(3 2^%d)) (GMP)", i, k, k);
                VERIFY(ec_powmod(w3, 3ULL << k, p) == 1, "prime %d: root3(%d)^(3 2^%d) = 1", i, k, k);
                if (k >= 1) VERIFY(ec_powmod(w3, 3ULL << (k - 1), p) == p - 1, "prime %d: root3(%d)^(3 2^%d) = p - 1", i, k, k - 1);
                VERIFY(ec_powmod(w3, 1ULL << k, p) != 1, "prime %d: root3(%d)^(2^%d) != 1 (order divisible by 3)", i, k, k);
                VERIFY(ec_powmod(w3, 3, p) == w, "prime %d: root3(%d)^3 = root(%d)", i, k, k);
                if (k >= 1) VERIFY(ec_mulmod_ref(w3, w3, p) == ec_root3(i, k - 1), "prime %d: root3(%d)^2 = root3(%d)", i, k, k - 1);
                VERIFY(ec_mulmod_ref(w3, ec_root3_inv(i, k), p) == 1, "prime %d: root3 inverse", i);
                n3++;
            }
        }
        /* 5. the refusal beyond v2 (and below 0) */
        VERIFY(refuses(ec_root, i, v2 + 1), "prime %d: ec_root(%d) stops (ec_fatal rc 3)", i, v2 + 1);
        VERIFY(refuses(ec_root, i, -1), "prime %d: ec_root(-1) stops", i);
        if (ec_has_radix3()) VERIFY(refuses(ec_root3, i, v2 + 1), "prime %d: ec_root3(%d) stops", i, v2 + 1);
        /* 6. the old formula at 2^34 (L8's finding; volatile: the shift is a runtime one, as in ntt_dist.c) */
        { volatile int k = 34; uint64_t w = ec_powmod(ec_W33[i], 1ULL << (EC_LOGN_MAX - k), p), h = ec_powmod(w, 1ULL << 33, p);
          printf("  old formula at logn 34: w = %llu, w^(2^33) = %llu (%s); new root(34) = %llu\n", (unsigned long long)w, (unsigned long long)h,
                 h == p - 1 ? "a root" : "NOT a root of order 2^34", (unsigned long long)ec_root(i, 34)); }
        printf("  identical to the old roots for logn 0..%d (%d, radix-3 too); order + GMP + chain checked for logn 0..%d (2^k) and %d radix-3 orders\n", EC_LOGN_MAX, n1, v2, n3);
        (void)n2;
    }
    return 0;
}

/* ---------- primes ---------- */

static const uint64_t B = BI_B10;

static void mpz_from_words(mpz_t z, const uint64_t *w, size_t n) { mpz_import(z, n, -1, 8, 0, 0, w); }
static double mpz_log2(const mpz_t z) { long e; double m = mpz_get_d_2exp(&e, z); return e + log2(m); }

/* ---- 1. the bound ---------------------------------------------------------------------------------------------- */
static void part1(void)
{
    printf("-- 1. the three-prime bound: margin = prod p / (n (B-1)^2), B = 10^18 (exact, GMP)\n");
    mpz_t P, D, t, u; mpz_inits(P, D, t, u, NULL);
    mpz_set_ui(D, B - 1); mpz_mul(D, D, D);                                   /* (B-1)^2 */
    static const int cs[4] = { 240, 216, 207, 147 };
    double best = 0; int bestdrop = -1;
    printf("   %-18s %8s", "subset (c)", "log2 P");
    for (int k = 20; k <= 33; k++) if (k >= 26 || k == 20) printf("   2^%d", k);
    printf("  3*2^31  3*2^32  3*2^33\n");
    for (int drop = 3; drop >= -1; drop--) {                                  /* drop = -1: all four (for reference) */
        mpz_set_ui(P, 1); char name[64] = ""; int nn = 0;
        for (int i = 0; i < 4; i++) if (i != drop) { mpz_mul_ui(P, P, ec_P[i]); nn += snprintf(name + nn, sizeof name - nn, "%s%d", nn ? "," : "", cs[i]); }
        VERIFY(drop < 0 || mpz_sizeinbase(P, 2) <= 156, "3-prime product < 2^156");
        printf("   %-18s %8.3f", name, mpz_log2(P));
        for (int k = 20; k <= 33; k++) {
            if (k < 26 && k != 20) continue;
            mpz_mul_2exp(t, D, k); mpz_set(u, P);
            printf(" %6.3g", mpz_get_d(u) / mpz_get_d(t));
        }
        for (int k = 31; k <= 33; k++) { mpz_mul_2exp(t, D, k); mpz_mul_ui(t, t, 3); printf(" %7.3g", mpz_get_d(P) / mpz_get_d(t)); }
        printf("\n");
        if (drop >= 0) {
            mpz_mul_2exp(t, D, 33); double m33 = mpz_get_d(P) / mpz_get_d(t);
            if (m33 > best) { best = m33; bestdrop = drop; }
            if (drop == 3) {                                                  /* the pipeline's subset: primes 0, 1, 2 */
                for (int k = 20; k <= 33; k++) { mpz_mul_2exp(t, D, k); VERIFY(mpz_cmp(t, P) < 0, "2^%d (B-1)^2 < p0 p1 p2", k); }
                for (int k = 20; k <= 33; k++) { mpz_mul_2exp(t, D, k); mpz_mul_ui(t, t, 3); VERIFY(mpz_cmp(t, P) < 0, "3 2^%d (B-1)^2 < p0 p1 p2", k); }
                mpz_sub_ui(u, P, 1); mpz_fdiv_q(u, u, D);
                crt_init();
                VERIFY(mpz_cmp_ui(u, (unsigned long)ec_np3_max_terms) == 0, "ec_np3_max_terms %zu == GMP %lu", ec_np3_max_terms, mpz_get_ui(u));
                printf("   the pipeline's subset {240,216,207}: at most %zu terms (2^%.3f); margin 27x at 2^31 is %.2fx, at 2^33 %.2fx; "
                       "3 2^33 (the longest 3 2^k length) %.3fx\n", ec_np3_max_terms, log2((double)ec_np3_max_terms),
                       mpz_get_d(P) / (mpz_get_d(D) * 0x1p31), mpz_get_d(P) / (mpz_get_d(D) * 0x1p33), mpz_get_d(P) / (mpz_get_d(D) * 3 * 0x1p33));
                harness_result("margin_2e31", "x", mpz_get_d(P) / (mpz_get_d(D) * 0x1p31));
                harness_result("margin_2e33", "x", mpz_get_d(P) / (mpz_get_d(D) * 0x1p33));
            }
        }
    }
    printf("   best subset for the margin: drop c = %d (%.2fx at 2^33); the four primes share the kernels (c 2^44 + 1, 3 | c: 2^k and 3 2^k lengths, p < 2^52 for the FP64 Barrett)\n", cs[bestdrop], best);
    VERIFY(bestdrop == 3, "the pipeline's subset {0,1,2} has the best margin");
    mpz_clears(P, D, t, u, NULL);
}

/* ---- 2. Garner at the extremes ------------------------------------------------------------------------------------ */
static int garner_one(const mpz_t v, const char *what)
{
    uint64_t r[3], w[3] = { 0, 0, 0 }, d[3];
    for (int i = 0; i < 3; i++) r[i] = mpz_fdiv_ui(v, ec_P[i]);
    crt_garner3(r, w); ec_words_to_dec3(w, d);
    mpz_t g, q, rem; mpz_inits(g, q, rem, NULL);
    mpz_from_words(g, w, 3);
    int ok = mpz_cmp(g, v) == 0;
    mpz_set(q, v);
    for (int k = 0; k < 3 && ok; k++) { mpz_t bb; mpz_init_set_ui(bb, B); mpz_fdiv_qr(q, rem, q, bb); ok = mpz_cmp_ui(rem, (unsigned long)d[k]) == 0 && d[k] < B; mpz_clear(bb); }
    ok = ok && mpz_sgn(q) == 0;
    if (!ok) VERIFY(0, "garner3 %s", what);
    else hv_checks++;
    mpz_clears(g, q, rem, NULL);
    return ok;
}
static void part2(void)
{
    printf("-- 2. crt_garner3 + ec_words_to_dec3 at the extremes, against GMP\n");
    mpz_t P, D, v; mpz_inits(P, D, v, NULL);
    mpz_set_ui(P, 1); for (int i = 0; i < 3; i++) mpz_mul_ui(P, P, ec_P[i]);
    mpz_set_ui(D, B - 1); mpz_mul(D, D, D);
    char what[64]; int bad = 0;
    for (int k = 20; k <= 33; k++) { mpz_mul_2exp(v, D, k); snprintf(what, sizeof what, "2^%d (B-1)^2", k); bad += !garner_one(v, what);
                                     mpz_mul_ui(v, v, 3); if (mpz_cmp(v, P) < 0) { snprintf(what, sizeof what, "3 2^%d (B-1)^2", k); bad += !garner_one(v, what); } }
    mpz_mul_ui(v, D, (unsigned long)ec_np3_max_terms); bad += !garner_one(v, "max_terms (B-1)^2");
    mpz_sub_ui(v, P, 1); bad += !garner_one(v, "p0 p1 p2 - 1");
    mpz_set_ui(v, 0); bad += !garner_one(v, "0");
    mpz_set_ui(v, B - 1); bad += !garner_one(v, "B-1");
    mpz_set_ui(v, B); bad += !garner_one(v, "B");
    gmp_randstate_t rs; gmp_randinit_default(rs); gmp_randseed_ui(rs, 0x9E37);
    for (int i = 0; i < 1000000; i++) { mpz_urandomm(v, rs, P); if (!garner_one(v, "random")) bad++; }
    gmp_randclear(rs);
    printf("   %d extreme + 10^6 random values: %s\n", 33 - 20 + 1 + 5, bad ? "MISMATCH" : "all exact");
    mpz_clears(P, D, v, NULL);
}

/* ---- 3. the CPU CRT with carry at ec_np = 3 ----------------------------------------------------------------------- */
static void residues_of(const mpz_t v, uint64_t r[4]) { for (int i = 0; i < 4; i++) r[i] = mpz_fdiv_ui(v, ec_P[i]); }
static void part3(void)
{
    printf("-- 3. crt_carry_par4 at ec_np = 3, decimal limbs\n");
    int save = ec_np; ec_np = 3; crt_init();
    mpz_t P, D, W, v, w, bm; mpz_inits(P, D, W, v, w, bm, NULL);
    mpz_set_ui(P, 1); for (int i = 0; i < 3; i++) mpz_mul_ui(P, P, ec_P[i]);
    mpz_set_ui(D, B - 1); mpz_mul(D, D, D);
    size_t m = (size_t)1 << 16; uint64_t *res[4], *out = (uint64_t *)malloc((m + 4) * 8);
    for (int i = 0; i < 4; i++) res[i] = (uint64_t *)malloc(m * 8);
    static const int Ts[] = { 1, 7, 96, 192 };
    for (int which = 0; which < 3; which++) {
        const char *nm = which == 0 ? "2^33 (B-1)^2" : which == 1 ? "max_terms (B-1)^2" : "p0 p1 p2 - 1";
        if (which == 0) mpz_mul_2exp(W, D, 33); else if (which == 1) mpz_mul_ui(W, D, (unsigned long)ec_np3_max_terms); else mpz_sub_ui(W, P, 1);
        uint64_t r[4]; residues_of(W, r);
        for (int i = 0; i < 4; i++) for (size_t k = 0; k < m; k++) res[i][k] = i < 3 ? r[i] : 0xDEADBEEFDEADBEEFULL;   /* plane 3 must not be read */
        mpz_set_ui(bm, B); mpz_pow_ui(v, bm, m); mpz_sub_ui(v, v, 1); mpz_divexact_ui(v, v, B - 1); mpz_mul(v, v, W);    /* W (B^m - 1) / (B - 1) */
        for (int ti = 0; ti < 4; ti++) {
            for (size_t k = 0; k < m + 4; k++) out[k] = 0xDEADBEEFULL;
            crt_carry_par4(res, m, out, Ts[ti]);
            mpz_from_limbs(w, out, m + 4);
            VERIFY(mpz_cmp(v, w) == 0, "every coefficient %s, n=%zu T=%d", nm, m, Ts[ti]);
        }
    }
    /* the exact convolution profile of two all-(B-1) operands of n limbs: c_k = min(k+1, 2n-1-k) (B-1)^2 */
    {
        size_t n = (size_t)1 << 22, nc = 2 * n; uint64_t *pl[4], *o = (uint64_t *)malloc((nc + 4) * 8), *o4 = (uint64_t *)malloc((nc + 4) * 8);
        for (int i = 0; i < 4; i++) pl[i] = (uint64_t *)malloc(nc * 8);
        unsigned __int128 d2 = (unsigned __int128)(B - 1) * (B - 1);
        double t0 = now();
#pragma omp parallel for schedule(static)
        for (size_t k = 0; k < nc; k++) {
            uint64_t cnt = k + 1 < nc - 1 - k + 1 ? k + 1 : nc - 1 - k; if (k == nc - 1) cnt = 0;
            for (int i = 0; i < 4; i++) { unsigned __int128 x = d2 % ec_P[i]; pl[i][k] = (uint64_t)((x * (cnt % ec_P[i])) % ec_P[i]); }
        }
        crt_carry_par4(pl, nc, o, 96);
        double t1 = now();
        int ok = o[0] == 1; for (size_t k = 1; k < n && ok; k++) ok = o[k] == 0;
        ok = ok && o[n] == B - 2; for (size_t k = n + 1; k < nc && ok; k++) ok = o[k] == B - 1;
        for (size_t k = nc; k < nc + 4 && ok; k++) ok = o[k] == 0;
        VERIFY(ok, "all-(B-1) profile at n = 2^22 (peak 2^22 (B-1)^2) == (B^n - 1)^2");
        ec_np = 4; crt_carry_par4(pl, nc, o4, 96); ec_np = 3;
        VERIFY(memcmp(o, o4, (nc + 4) * 8) == 0, "three-prime CRT == four-prime CRT on the same coefficients (2^23)");
        printf("   profile of (B^n-1)^2 at n = 2^22 (2^23 coefficients): %s, %.2f s\n", ok ? "exact" : "WRONG", t1 - t0);
        for (int i = 0; i < 4; i++) free(pl[i]); free(o); free(o4);
    }
    /* throughput: three against four primes on the same random coefficients < p0 p1 p2 (2^26 coefficients, T = 192) */
    {
        size_t n = (size_t)1 << 26; uint64_t *pl[4], *o = (uint64_t *)malloc((n + 4) * 8);
        for (int i = 0; i < 4; i++) pl[i] = (uint64_t *)malloc(n * 8);
#pragma omp parallel
        { rng_t r = { 0xF00D + (uint64_t)omp_get_thread_num() };
#pragma omp for schedule(static)
          for (size_t k = 0; k < n; k++) { uint64_t a = rng_next(&r) % (B - 1), b = rng_next(&r) % (B - 1), c = rng_next(&r) % 1000000;   /* v = a b c-ish: (a b mod p) scaled */
                                           for (int i = 0; i < 4; i++) pl[i][k] = (uint64_t)(((unsigned __int128)(a % ec_P[i]) * (b % ec_P[i]) % ec_P[i]) * (c % ec_P[i]) % ec_P[i]); } }
        for (int np = 4; np >= 3; np--) {
            ec_np = np; double best = 1e9;
            for (int rep = 0; rep < 3; rep++) { double t0 = now(); crt_carry_par4(pl, n, o, 192); double dt = now() - t0; if (dt < best) best = dt; }
            printf("   CPU CRT (decimal) 2^26 coefficients, T=192: %d primes %.3f s (%.2f Gcoef/s)\n", np, best, n / best / 1e9);
        }
        ec_np = 3;
        for (int i = 0; i < 4; i++) free(pl[i]); free(o);
    }
    for (int i = 0; i < 4; i++) free(res[i]); free(out);
    mpz_clears(P, D, W, v, w, bm, NULL);
    ec_np = save;
}

/* ---- 4. end to end on the GPUs ------------------------------------------------------------------------------------ */
static int allmax_ok(const bigint *C, size_t n)
{
    if (C->n != 2 * n) return 0;
    if (C->l[0] != 1) return 0;
    for (size_t k = 1; k < n; k++) if (C->l[k]) return 0;
    if (C->l[n] != B - 2) return 0;
    for (size_t k = n + 1; k < 2 * n; k++) if (C->l[k] != B - 1) return 0;
    return 1;
}
static void check_rand(const char *what, const bigint *A, const bigint *Bb, const bigint *C) { tref_check(what, A, Bb, C); }
/* C = A B on the device path; cap = the plane cap 2^cap (DIST_LOGN_TEST), 0 = the default */
static void dev_mul(bigint *C, const bigint *A, const bigint *Bb, int cap)
{
    char cb[16]; if (cap) { snprintf(cb, sizeof cb, "%d", cap); setenv("DIST_LOGN_TEST", cb, 1); }
    dbig x, y, z; db_init(&x); db_init(&y); db_init(&z);
    db_from_bi(&x, A); db_from_bi(&y, Bb); rns_mul_dist_db(&z, &x, &y); db_to_bi(C, &z);
    db_free(&x); db_free(&y); db_free(&z);
    if (cap) unsetenv("DIST_LOGN_TEST");
}
static void part4(int logmax)
{
    rns_init(31);
    printf("-- 4. GPU end to end at ec_np = %d (ECALC_NP), decimal limbs, pools 2^31 (plane pool 0: %zu limbs)\n", ec_np, rns_plane_limbs());
    bigint A, Bb, C; bi_init(&A); bi_init(&Bb); bi_init(&C);
    rng_t rng = { 0x3B1ULL };
    /* the closed form itself against GMP */
    { size_t n = (size_t)1 << 20; bi_random(&A, n, GEN_ONES, &rng); bi_random(&Bb, n, GEN_ONES, &rng); dev_mul(&C, &A, &Bb, 0);
      mpz_t a, b, c; mpz_inits(a, b, c, NULL); bi_to_mpz(a, &A); bi_to_mpz(b, &Bb); mpz_mul(c, a, b);
      VERIFY(bi_eq_mpz(&C, c) && allmax_ok(&C, n), "all-(B-1) 2^20 x 2^20: the device product == mpz_mul == closed form"); mpz_clears(a, b, c, NULL); }
    bigint D; bi_init(&D);
    for (int tier = 0; tier < 2; tier++) {                                   /* tier 0: half the plane (a 2-piece grid), tier 1: the default */
        for (int lg = 19; lg <= logmax; lg++) {
            size_t n = (size_t)1 << lg;
            if (tier == 0 && lg > 29) break;                                 /* (as the removed host tier: to 2^29) */
            int cap = tier == 0 ? (lg < 20 ? 20 : lg) : 0;                    /* 2 n = 2^(lg+1) points under a 2^lg cap */
            const char *nm = tier ? "dist" : "grid";
            bi_random(&A, n, GEN_ONES, &rng); bi_random(&Bb, n, GEN_ONES, &rng);
            double t0 = now(); dev_mul(&C, &A, &Bb, cap); double t1 = now();
            VERIFY(allmax_ok(&C, n), "%s all-(B-1) 2^%d x 2^%d (peak coefficient 2^%d (B-1)^2)", nm, lg, lg, lg);
            printf("   %-8s all-(B-1) 2^%d x 2^%d (2^%d points, cap %s): %s  %.2f s\n", nm, lg, lg, lg + 1, cap ? "half" : "default", allmax_ok(&C, n) ? "exact" : "WRONG", t1 - t0);
            if (lg <= 25 || lg == logmax) {                                   /* random operands, the same sizes (uneven): both caps agree */
                bi_random(&A, n + 3, GEN_UNIFORM, &rng); bi_random(&Bb, n - 5, GEN_UNIFORM, &rng);
                dev_mul(&C, &A, &Bb, cap);
                char what[64]; snprintf(what, sizeof what, "%s random 2^%d", nm, lg); check_rand(what, &A, &Bb, &C);
                if (tier == 0) { dev_mul(&D, &A, &Bb, 0); VERIFY(bi_cmp(&C, &D) == 0, "%s: half and default plane differ", what); }
            }
        }
    }
    bi_free(&D);
    /* the batch tier (striped, staged operands: host memory): 16 products each of 2^k x 2^k limbs */
    for (int lg = 10; lg <= 18; lg += 4) {
        size_t n = (size_t)1 << lg, N = 16; rns_prod P[16]; bigint As[16], Bs[16]; uint64_t *cs[16];
        for (size_t i = 0; i < N; i++) { bi_init(&As[i]); bi_init(&Bs[i]); bi_random(&As[i], n, i & 1 ? GEN_ONES : GEN_UNIFORM, &rng); bi_random(&Bs[i], n, i & 1 ? GEN_ONES : GEN_UNIFORM, &rng);
                                         cs[i] = (uint64_t *)calloc(2 * n + 1, 8); P[i].a = As[i].l; P[i].na = As[i].n; P[i].b = Bs[i].l; P[i].nb = Bs[i].n; P[i].c = cs[i]; P[i].x = 0; P[i].nx = 0; P[i].ncn = 0; }
        rns_mul_batch(P, N);
        for (size_t i = 0; i < N; i++) { bigint Cv = { cs[i], 2 * n, 2 * n + 1 }; bi_norm(&Cv); char what[64]; snprintf(what, sizeof what, "batch 2^%d #%zu", lg, i);
                                         if (i & 1) VERIFY(allmax_ok(&Cv, n), "%s all-(B-1)", what); else check_rand(what, &As[i], &Bs[i], &Cv);
                                         free(cs[i]); bi_free(&As[i]); bi_free(&Bs[i]); }
        printf("   batch    16 products of 2^%d x 2^%d: checked\n", lg, lg);
    }
    bi_free(&A); bi_free(&Bb); bi_free(&C);
}


int main(int argc, char **argv)
{
    const char *sec = argc > 1 ? argv[1] : "all";
    harness_meta("t_arith");
    if (!strcmp(sec, "modarith")) { run_modarith((argc > 2 ? strtoull(argv[2], 0, 10) : 1000) * 1000000ULL); }
    else if (!strcmp(sec, "roots")) run_roots();
    else if (!strcmp(sec, "primes")) {
        int gpu = argc > 2 && !strcmp(argv[2], "gpu"), logmax = argc > 3 ? atoi(argv[3]) : 30;
        if (!gpu) { crt_init(); part1(); part2(); part3(); } else part4(logmax);
    } else if (!strcmp(sec, "all")) {
        run_roots(); crt_init(); part1(); part2(); part3();
        run_modarith((argc > 2 ? strtoull(argv[2], 0, 10) : 100) * 1000000ULL);
    } else { fprintf(stderr, "usage: t_arith [all [PAIRS_M] | modarith [PAIRS_M] | roots | primes [gpu [LOGMAX]]]\n"); return 2; }
    return verify_done("t_arith");
}
