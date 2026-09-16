/* harness.h - shared test scaffolding for ecalc/tests (PLAN.md 8, step 0).
 *
 * VERIFY(cond, ...)      counts a check; prints only failures
 * verify_done(name)      prints "VERIFY OK (n checks)" / "VERIFY FAILED (k of n)",
 *                        returns the process exit status
 * now()                  wall seconds
 * rng_t / rng_next       splitmix64
 * gen_limbs(kind)        uniform, all-ones, zeros, single-bit, sparse limb arrays
 * mpz_from_limbs / mpz_to_limbs / bi_from_mpz / bi_to_mpz / bi_eq_mpz
 * META / RESULT lines in the bench/ format (harness_meta, harness_result)
 *
 * C and HIP-C++ compatible; needs -lgmp.
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
#include "../bigint.h"

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
}

static void mpz_from_limbs(mpz_t z, const uint64_t *a, size_t n)
{
    if (!n) { mpz_set_ui(z, 0); return; }
    mpz_import(z, n, -1, sizeof *a, 0, 0, a);
}
/* returns limbs written; cap must be >= mpz_size */
static size_t mpz_to_limbs(uint64_t *a, size_t cap, const mpz_t z)
{
    size_t n = 0;
    if (mpz_sgn(z) == 0) return 0;
    if (mpz_size(z) > cap) { fprintf(stderr, "mpz_to_limbs: cap %zu < %zu\n", cap, mpz_size(z)); abort(); }
    mpz_export(a, &n, -1, sizeof *a, 0, 0, z);
    return n;
}
static void bi_from_mpz(bigint *a, const mpz_t z)
{
    size_t n = mpz_size(z);
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
    gethostname(host, sizeof host);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", localtime(&t));
    printf("META bench=%s host=%s date=%s\n", bench, host, date);
}
static void harness_result(const char *metric, const char *unit, double v)
{
    printf("RESULT %s %s %s %.6g\n", hv_bench, metric, unit, v);
}
#endif

/* value of a limb array mod 2^61 - 1 (2^64 = 8 mod M61): cheap residue check */
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
    uint64_t r = 0;
    for (size_t i = n; i-- > 0;) {
        uint64_t x = (a[i] & HV_M61) + (a[i] >> 61);            /* a[i] mod M61 */
        if (x >= HV_M61) x -= HV_M61;
        r = m61_add(m61_mul(r, 8), x);
    }
    return r;
}
