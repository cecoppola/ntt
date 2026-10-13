/* harness.h - shared test scaffolding for ecalc/tests (TESTING.md §5).
 *
 * VERIFY(cond, ...)      counts a check; prints only failures
 * verify_done(name)      prints "VERIFY OK (n checks)" / "VERIFY FAILED (k of n)",
 *                        returns the process exit status
 * now()                  wall seconds
 * rng_t / rng_next       splitmix64
 * gen_limbs(kind)        uniform, all-ones (B-1), zeros, single-bit, sparse limb arrays (decimal limbs < 10^18)
 * mpz_from_limbs / mpz_to_limbs / bi_from_mpz / bi_to_mpz / bi_eq_mpz   (GMP by a product tree, O(M(n) log n); the
 *                        18-digit string forms mpz_from_limbs_str / mpz_to_limbs_str check them; tref.h is the product reference)
 * limbs_mod_m61 / limbs_mod_u64   cheap residues of a limb array (mod 2^61-1, mod any q < 2^63)
 * META / RESULT lines in the bench/ format (harness_meta, harness_result)
 * digits_format          the digit string of a limb array (t_out, t_patch)
 *
 * C and HIP-C++ compatible; needs -lgmp.  Decimal limbs only since archive:PLAN §28.5 (the binary base is archived).
 */
#ifndef EC_HARNESS_H
#define EC_HARNESS_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <gmp.h>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "../bigint.h"
#pragma GCC diagnostic ignored "-Wunused-function"   /* each test uses a subset of these helpers */

static int hv_fails = 0, hv_checks = 0;
__attribute__((constructor)) static void hv_linebuf(void) { setvbuf(stdout, NULL, _IOLBF, 0); }
#define VERIFY(cond, ...) do { hv_checks++; if (!(cond)) { hv_fails++;                      \
    printf("  FAILED %s:%d  %s  ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__);         \
    printf("\n"); } } while (0)
static int verify_done(const char *name)
{
    if (hv_fails) printf("\n%s: VERIFY FAILED (%d of %d checks)\n", name, hv_fails, hv_checks);
    else          printf("\n%s: VERIFY OK (%d checks)\n", name, hv_checks);
    return hv_fails != 0;
}

static double now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

typedef struct { uint64_t s; } rng_t;
static inline uint64_t rng_next(rng_t *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
enum { GEN_UNIFORM, GEN_ONES, GEN_ZEROS, GEN_BIT, GEN_SPARSE, GEN_KINDS };
static const char *gen_name[GEN_KINDS] = {"uniform", "all-ones", "zeros", "single-bit", "sparse"};
/* zeros: top limb nonzero so the length is n */
static void gen_limbs(uint64_t *a, size_t n, int kind, rng_t *r)
{
    size_t i;
    if (!n) return;
    switch (kind) {
    case GEN_UNIFORM: for (i = 0; i < n; i++) a[i] = rng_next(r); break;
    case GEN_ONES:    for (i = 0; i < n; i++) a[i] = ~0ULL; break;
    case GEN_ZEROS:   memset(a, 0, n * sizeof *a); a[n - 1] = 1; break;
    case GEN_BIT:     memset(a, 0, n * sizeof *a); a[n - 1] = 1ULL << (rng_next(r) & 63); break;
    case GEN_SPARSE:  memset(a, 0, n * sizeof *a); a[n - 1] = 1;
                      for (i = 0; i < n / 16 + 1; i++) a[rng_next(r) % n] |= 1ULL << (rng_next(r) & 63);
                      break;
    }
    for (i = 0; i < n; i++) a[i] = kind == GEN_ONES ? BI_B10 - 1 : a[i] % BI_B10;   /* limbs must be < 10^18: ones -> B-1, others reduced */
    if (a[n - 1] == 0) a[n - 1] = 1;
}

static void mpz_from_limbs_str(mpz_t z, const uint64_t *a, size_t n)
{
    if (!n) { mpz_set_ui(z, 0); return; }
    char *s = (char *)malloc(n * 18 + 1);               /* decimal limbs: 18 digits each, via a string */
    for (size_t i = 0; i < n; i++) snprintf(s + 18 * i, 19, "%018llu", (unsigned long long)a[n - 1 - i]);
    mpz_set_str(z, s, 10); free(s);
}
/* the string form (returns limbs written; cap must be large enough) */
static size_t mpz_to_limbs_str(uint64_t *a, size_t cap, const mpz_t z)
{
    size_t n = 0;
    if (mpz_sgn(z) == 0) return 0;
    char *s = mpz_get_str(NULL, 10, z); size_t len = strlen(s); n = (len + 17) / 18;
    if (n > cap) { fprintf(stderr, "mpz_to_limbs: cap %zu < %zu\n", cap, n); abort(); }
    for (size_t i = 0; i < n; i++) {                    /* limb i = digits [len-18(i+1), len-18 i) */
        size_t end = len - 18 * i, beg = end >= 18 ? end - 18 : 0; uint64_t v = 0;
        for (size_t j = beg; j < end; j++) v = v * 10 + (uint64_t)(s[j] - '0');
        a[i] = v;
    }
    free(s); return n;
}
/* GMP <-> decimal limbs by a product tree (TREF, results/TREF.md): value = lo + hi B^h with h = 2^k, the powers B^(2^k) kept;
 * O(M(n) log n) both ways like GMP's own string conversion (mpz_set_str / mpz_get_str, as fast single-threaded), but the halves
 * run as OpenMP tasks.  The 18-digit string forms (mpz_from_limbs_str / mpz_to_limbs_str) stay as their check (t_mul section 0). */
#define HV_TREE_LEAF 16
#define HV_TASK_MIN 8192
static mpz_t hv_pw[48]; static int hv_npw = 0;
static const mpz_t *hv_pow2k(int k)                       /* B^(2^k) */
{
    while (hv_npw <= k) {
        mpz_init(hv_pw[hv_npw]);
        if (!hv_npw) mpz_set_ui(hv_pw[0], BI_B10); else mpz_mul(hv_pw[hv_npw], hv_pw[hv_npw - 1], hv_pw[hv_npw - 1]);
        hv_npw++;
    }
    return (const mpz_t *)&hv_pw[k];
}
static int hv_split(size_t n) { int k = 0; while (((size_t)2 << k) < n) k++; return k; }   /* h = 2^k: n/2 <= h < n */
static void hv_from_rec(mpz_t z, const uint64_t *a, size_t n, int par)
{
    if (n <= HV_TREE_LEAF) {
        mpz_set_ui(z, 0);
        for (size_t i = n; i-- > 0;) { mpz_mul_ui(z, z, BI_B10); mpz_add_ui(z, z, a[i]); }
        return;
    }
    int k = hv_split(n); size_t h = (size_t)1 << k;
    mpz_t t; mpz_init(t);
#pragma omp task shared(t) if (par && n >= HV_TASK_MIN)
    hv_from_rec(t, a, h, par);
    hv_from_rec(z, a + h, n - h, par);
#pragma omp taskwait
    mpz_mul(z, z, *hv_pow2k(k)); mpz_add(z, z, t);
    mpz_clear(t);
}
static void hv_to_rec(uint64_t *a, size_t n, const mpz_t z, int par)   /* exactly n limbs of z mod B^n */
{
    if (n <= HV_TREE_LEAF) {
        mpz_t t; mpz_init_set(t, z);
        for (size_t i = 0; i < n; i++) a[i] = mpz_fdiv_q_ui(t, t, BI_B10);
        mpz_clear(t); return;
    }
    int k = hv_split(n); size_t h = (size_t)1 << k;
    mpz_t q, r; mpz_inits(q, r, NULL);
    mpz_tdiv_qr(q, r, z, *hv_pow2k(k));
#pragma omp task shared(r) if (par && n >= HV_TASK_MIN)
    hv_to_rec(a, h, r, par);
    hv_to_rec(a + h, n - h, q, par);
#pragma omp taskwait
    mpz_clears(q, r, NULL);
}
/* the two directions; above HV_TASK_MIN limbs (outside a parallel region) the halves are OpenMP tasks.  The powers are made first */
static int hv_par(size_t n)
{
    if (n > HV_TREE_LEAF) hv_pow2k(hv_split(n));
#ifdef _OPENMP
    return n >= HV_TASK_MIN && !omp_in_parallel();
#else
    return 0;
#endif
}
static void mpz_from_limbs(mpz_t z, const uint64_t *a, size_t n)
{
    if (hv_par(n)) {
#pragma omp parallel
#pragma omp single
        hv_from_rec(z, a, n, 1);
    } else hv_from_rec(z, a, n, 0);
}
static void hv_to_limbs_n(uint64_t *a, size_t n, const mpz_t z)
{
    if (hv_par(n)) {
#pragma omp parallel
#pragma omp single
        hv_to_rec(a, n, z, 1);
    } else hv_to_rec(a, n, z, 0);
}
/* returns limbs written (normalised); cap must be large enough */
static size_t mpz_to_limbs(uint64_t *a, size_t cap, const mpz_t z)
{
    if (mpz_sgn(z) == 0) return 0;
    size_t n = (mpz_sizeinbase(z, 10) + 17) / 18;          /* (sizeinbase may be one digit high: the top limb is then 0) */
    if (n > cap) { fprintf(stderr, "mpz_to_limbs: cap %zu < %zu\n", cap, n); abort(); }
    hv_to_limbs_n(a, n, z);
    while (n && a[n - 1] == 0) n--;
    return n;
}
static void bi_from_mpz(bigint *a, const mpz_t z)
{
    size_t n = (mpz_sizeinbase(z, 10) + 17) / 18 + 1;
    bi_reserve(a, n ? n : 1);
    a->n = mpz_to_limbs(a->l, a->cap, z);
}
static void bi_to_mpz(mpz_t z, const bigint *a) { mpz_from_limbs(z, a->l, a->n); }
static int bi_eq_mpz(const bigint *a, const mpz_t z)
{
    mpz_t t; int eq;
    mpz_init(t); bi_to_mpz(t, a); eq = mpz_cmp(t, z) == 0; mpz_clear(t);
    return eq;
}
/* the d + 1 significant digits ("2718...") of the decimal-limb number l[0..n) (zeros above n), NUL-terminated */
static void digits_format(char *digits, const uint64_t *l, size_t n, unsigned long d)
{
    size_t nl = (d + 1 + 17) / 18, pad = nl * 18 - (d + 1);
    for (size_t i = 0; i < nl; i++) {
        char buf[19]; uint64_t v = i < n ? l[i] : 0;
        snprintf(buf, sizeof buf, "%018llu", (unsigned long long)v);
        size_t pos = (nl - 1 - i) * 18;
        for (int c = 0; c < 18; c++) if (pos + c >= pad) digits[pos + c - pad] = buf[c];
    }
    digits[d + 1] = 0;
}
static void bi_random(bigint *a, size_t n, int kind, rng_t *r)
{
    bi_reserve(a, n ? n : 1); gen_limbs(a->l, n, kind, r); a->n = n; bi_norm(a);
}

/* META / RESULT lines, same grammar as bench/common_ntt.h */
static const char *hv_bench = "?";
static void harness_meta(const char *bench)
{
    char host[128] = "?"; char date[64];
    time_t t = time(0);
    hv_bench = bench;
    printf("META limb_base=10^18\n");
    gethostname(host, sizeof host);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", localtime(&t));
    printf("META bench=%s host=%s date=%s\n", bench, host, date);
}
static void harness_result(const char *metric, const char *unit, double v)
{
    printf("RESULT %s %s %s %.6g\n", hv_bench, metric, unit, v);
}

/* value of a limb array mod 2^61 - 1: cheap residue check (B = 10^18 reduced once) */
#define HV_M61 0x1FFFFFFFFFFFFFFFULL
static inline uint64_t m61_add(uint64_t a, uint64_t b) { uint64_t s = a + b; return s >= HV_M61 ? s - HV_M61 : s; }
static inline uint64_t m61_mul(uint64_t a, uint64_t b)
{
    unsigned __int128 p = (unsigned __int128)a * b;
    uint64_t lo = (uint64_t)p & HV_M61, hi = (uint64_t)(p >> 61);
    uint64_t s = lo + hi; if (s >= HV_M61) s -= HV_M61; if (s >= HV_M61) s -= HV_M61;
    return s;
}
static uint64_t limbs_mod_m61(const uint64_t *a, size_t n)
{
    uint64_t r = 0, bm = BI_B10 % HV_M61;                    /* B mod M61 */
    for (size_t i = n; i-- > 0;) {
        uint64_t x = (a[i] & HV_M61) + (a[i] >> 61);            /* a[i] mod M61 */
        if (x >= HV_M61) x -= HV_M61;
        r = m61_add(m61_mul(r, bm), x);
    }
    return r;
}
/* value of a limb array mod any q < 2^63 (Horner over B = 10^18 with 128-bit products): a second, independent residue for the
 * sizes where the GMP string bridge is too slow (t_mul above 2^20 limbs, t_newton above 2^22) */
static inline uint64_t hv_mulmod(uint64_t a, uint64_t b, uint64_t q) { return (uint64_t)((unsigned __int128)a * b % q); }
static uint64_t limbs_mod_u64(const uint64_t *a, size_t n, uint64_t q)
{
    uint64_t r = 0, bm = BI_B10 % q;
    for (size_t i = n; i-- > 0;) r = (uint64_t)(((unsigned __int128)r * bm + a[i] % q) % q);
    return r;
}
/* the two 62-bit primes the residue checks use beside M61 (prime: both are T1 / DKM primes of the pipeline's tests) */
#define HV_Q1 1000000000000000003ULL
#define HV_Q2 4611686018427387847ULL
#endif
