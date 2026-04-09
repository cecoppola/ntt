/*
 * ntt_mont.c — NTT with Montgomery multiplication butterfly
 *
 * Purpose:   Drop-in replacement for ntt_cpu.c using Montgomery multiplication
 *            in the butterfly. Benchmarks side-by-side against the lazy
 *            reference to quantify the speedup from eliminating __uint128_t.
 * Algorithm: Cooley-Tukey DIT, radix-2. Per-stage reduction (values reduced
 *            to [0, q) after each stage) keeps montgomery inputs in range.
 * Montgomery: R = 2^32. Inputs a, b in [0, q) ⊂ [0, 2^32). All products
 *            a*b < 2^64 — no __uint128_t needed in the hot path.
 * Ref:       Montgomery, "Modular Multiplication Without Trial Division,"
 *            Math. Comp. 44 (1985), §3.
 *            Longa & Naehrig, CANS 2016, Alg. 1 (butterfly structure).
 * Compiler:  gcc or clang, C99+.
 *   Build:   cc -O2 -Wall -Wextra -o ntt_mont ntt_mont.c
 */

#include "ntt.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── ANSI ────────────────────────────────────────────────────────────────── */
#define WHT "\033[1;37m"
#define CYN "\033[1;36m"
#define GRN "\033[1;32m"
#define YLW "\033[1;33m"
#define RST "\033[0m"

/* ── Hardware info ───────────────────────────────────────────────────────── */
typedef struct {
    long     n_cpus;
    long     page_sz;
    long     phys_pages;
    uint64_t mem_bytes;
} hw_info_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * MONTGOMERY PARAMETERS
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * mont_ctx_t: all Montgomery parameters for a given modulus q.
 * R = 2^32 throughout. Computed once in mont_ctx_init(); treated as read-only.
 */
typedef struct {
    uint64_t q;          /* modulus, q < 2^32 and q odd (prime)               */
    uint64_t r2;         /* R^2 mod q = 2^64 mod q; used to enter Mont. form  */
    uint32_t q_prime;    /* -q^{-1} mod R; satisfies q * q_prime ≡ -1 (mod R) */
} mont_ctx_t;

/*
 * mont_q_prime_compute: compute -q^{-1} mod 2^32 via Hensel lifting.
 * Purpose:  find q' such that q * q' ≡ -1 (mod 2^32).
 * Algorithm: 5 squarings from seed x=1 (q*1 ≡ 1 mod 2 since q odd), each
 *            iteration doubles the number of correct bits via Newton's method:
 *            x ← x * (2 - q * x).  After 5 iterations: 2^(2^5) = 2^32 bits.
 * Ref:       Dumas, "Simultaneous Modular Inversion and Montgomery Inverse,"
 *            Finite Fields Appl. 37 (2016), Lemma 1.
 */
static uint32_t mont_q_prime_compute(uint64_t q)
{
    uint32_t x = 1;                        /* q * 1 ≡ 1 (mod 2) — seed      */
    x *= 2 - (uint32_t)q * x;             /* correct mod 2^2               */
    x *= 2 - (uint32_t)q * x;             /* correct mod 2^4               */
    x *= 2 - (uint32_t)q * x;             /* correct mod 2^8               */
    x *= 2 - (uint32_t)q * x;             /* correct mod 2^16              */
    x *= 2 - (uint32_t)q * x;             /* correct mod 2^32  (x = q^{-1} mod 2^32) */
    return (uint32_t)(-(uint64_t)x);      /* negate: -q^{-1} mod 2^32      */
}

/*
 * mont_ctx_init: populate a mont_ctx_t for modulus q.
 * Purpose:  one-time setup before any Montgomery multiplication.
 * Inputs:   q — NTT prime modulus, must be odd and < 2^32.
 * Output:   ctx filled with q, r2 = 2^64 mod q, q_prime = -q^{-1} mod 2^32.
 */
static void mont_ctx_init(mont_ctx_t *ctx, uint64_t q)
{
    ctx->q       = q;
    ctx->r2      = (uint64_t)(((__uint128_t)1 << 64) % q); /* R^2 mod q, R=2^32 */
    ctx->q_prime = mont_q_prime_compute(q);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MONTGOMERY ARITHMETIC
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * mont_mul: compute a * b * R^{-1} mod q (REDC algorithm).
 * Purpose:  Montgomery butterfly multiply — the NTT hot path.
 * Inputs:   a, b in [0, q); q < 2^32; so a*b < 2^64 (no 128-bit needed).
 * Output:   a * b * R^{-1} mod q, in [0, q).
 * Algorithm: REDC with R = 2^32.
 *   t   = a * b                        (64-bit; a,b < q < 2^32 → t < 2^64)
 *   mp  = (t mod R) * q' mod R         (32-bit; selects correction factor)
 *   sum = t + mp * q                   (≤ 2^65; use carry arithmetic)
 *   u   = sum / R                      (exact; bottom 32 bits of sum are 0)
 *   return u >= q ? u - q : u
 * Invariant: q * q_prime ≡ -1 (mod R) ensures t + mp*q ≡ 0 (mod R).
 * Ref:       Montgomery 1985, §3; Barrett 1987, §4 (alternative framing).
 */
static inline uint64_t mont_mul(uint64_t a, uint64_t b, const mont_ctx_t *ctx)
{
    uint64_t t    = a * b;                            /* < 2^64 since a,b < 2^32  */
    uint32_t mp   = (uint32_t)t * ctx->q_prime;      /* (t mod R) * q' mod R     */
    uint64_t corr = (uint64_t)mp * ctx->q;           /* correction; < 2^64       */
    uint64_t lo   = t + corr;                        /* may overflow; track carry */
    uint64_t carry = (lo < t) ? 1ULL : 0ULL;         /* 1 if wrapped             */
    uint64_t u    = (lo >> 32) | (carry << 32);      /* (t + mp*q) / R           */
    return u >= ctx->q ? u - ctx->q : u;             /* final conditional reduce  */
}

/*
 * mont_enter: convert a from standard form to Montgomery form.
 * Purpose:   a → a * R mod q = a * 2^32 mod q.
 * Method:    mont_mul(a, R^2 mod q) = a * R^2 * R^{-1} mod q = a * R mod q.
 */
static inline uint64_t mont_enter(uint64_t a, const mont_ctx_t *ctx)
{
    return mont_mul(a, ctx->r2, ctx);
}

/*
 * mont_exit: convert a from Montgomery form back to standard form.
 * Purpose:   a * R mod q → a mod q.
 * Method:    mont_mul(a, 1) = a * 1 * R^{-1} mod q = a * R^{-1} mod q.
 *            Since a was a*R, the result is a*R * R^{-1} = a.
 */
static inline uint64_t mont_exit(uint64_t a, const mont_ctx_t *ctx)
{
    return mont_mul(a, 1, ctx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SHARED HELPERS (mod_pow, ntt_params_init, bit_reverse_perm)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t mod_pow(uint64_t base, uint64_t exp, uint64_t m)
{
    uint64_t r = 1;
    base %= m;
    while (exp > 0) {
        if (exp & 1) r = (uint64_t)((__uint128_t)r * base % m);
        base = (uint64_t)((__uint128_t)base * base % m);
        exp >>= 1;
    }
    return r;
}

/* ntt_params_init: implements the declaration in ntt.h.
 * Defined here so ntt_mont.c is self-contained (no link dep on ntt_cpu.c). */
int ntt_params_init(ntt_params_t *p)
{
    if (!p->n || (p->n & (p->n - 1))) return -1;
    p->omega_inv = mod_pow(p->omega, p->q - 2, p->q);
    p->n_inv     = mod_pow(p->n,     p->q - 2, p->q);
    uint64_t t   = p->n; p->log2_n = 0;
    while (t > 1) { t >>= 1; p->log2_n++; }
    const ntt_modulus_info_t *mi = ntt_modulus_find(p->q);
    p->reduce = mi ? mi->reduce : reduce_generic;
    return 0;
}

static void bit_reverse_perm(uint64_t *a, uint64_t n, uint32_t log2_n)
{
    for (uint64_t i = 0; i < n; i++) {
        uint64_t rev = 0, x = i;
        for (uint32_t b = 0; b < log2_n; b++) { rev = (rev << 1) | (x & 1); x >>= 1; }
        if (i < rev) { uint64_t tmp = a[i]; a[i] = a[rev]; a[rev] = tmp; }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TWIDDLE ALLOCATION (ntt.h declarations implemented here)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ntt_alloc_twiddles and ntt_alloc_twiddles_inv: standard (lazy) twiddles.
 * Duplicated from ntt_cpu.c so ntt_mont.c links standalone. */
uint64_t *ntt_alloc_twiddles(const ntt_params_t *p)
{
    uint64_t half = p->n >> 1;
    uint64_t *tw  = malloc(half * sizeof *tw);
    if (!tw) return NULL;
    tw[0] = 1;
    for (uint64_t k = 1; k < half; k++)
        tw[k] = (uint64_t)((__uint128_t)tw[k-1] * p->omega % p->q);
    return tw;
}

uint64_t *ntt_alloc_twiddles_inv(const ntt_params_t *p)
{
    uint64_t half = p->n >> 1;
    uint64_t *tw  = malloc(half * sizeof *tw);
    if (!tw) return NULL;
    tw[0] = 1;
    for (uint64_t k = 1; k < half; k++)
        tw[k] = (uint64_t)((__uint128_t)tw[k-1] * p->omega_inv % p->q);
    return tw;
}

/*
 * ntt_alloc_twiddles_mont: twiddles pre-converted to Montgomery form.
 * tw_mont[k] = omega^k * R mod q.  Used exclusively with ntt_forward_mont.
 */
uint64_t *ntt_alloc_twiddles_mont(const ntt_params_t *p)
{
    mont_ctx_t ctx;
    mont_ctx_init(&ctx, p->q);
    uint64_t half = p->n >> 1;
    uint64_t *tw  = malloc(half * sizeof *tw);
    if (!tw) return NULL;
    uint64_t omega_mont = mont_enter(p->omega, &ctx);
    tw[0] = mont_enter(1, &ctx);
    for (uint64_t k = 1; k < half; k++)
        tw[k] = mont_mul(tw[k-1], omega_mont, &ctx);
    return tw;
}

uint64_t *ntt_alloc_twiddles_inv_mont(const ntt_params_t *p)
{
    mont_ctx_t ctx;
    mont_ctx_init(&ctx, p->q);
    uint64_t half = p->n >> 1;
    uint64_t *tw  = malloc(half * sizeof *tw);
    if (!tw) return NULL;
    uint64_t omega_inv_mont = mont_enter(p->omega_inv, &ctx);
    tw[0] = mont_enter(1, &ctx);
    for (uint64_t k = 1; k < half; k++)
        tw[k] = mont_mul(tw[k-1], omega_inv_mont, &ctx);
    return tw;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MONTGOMERY NTT
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * ntt_forward_mont: in-place CT-DIT NTT with Montgomery butterfly.
 * Purpose:  same external interface as ntt_forward; faster on CPU because
 *           montgomery multiply avoids __uint128_t in the hot inner loop.
 * Inputs:   a[0..n-1] in standard form [0, q).
 *           twiddles: from ntt_alloc_twiddles_mont(p)  (Montgomery form).
 * Output:   NTT(a) in standard form [0, q).
 * Algorithm: CT-DIT. Each butterfly does conditional subtraction on both
 *            outputs, keeping all values in [0, q). This ensures mont_mul
 *            inputs are always in [0, q) without a separate reduction pass.
 *   Stage loop: bit-reverse → for each stage:
 *     butterfly: u = a[k+j]; t = mont_mul(tw[j·stride], a[k+j+half])
 *                a[k+j]       = (u+t >= q) ? u+t-q : u+t
 *                a[k+j+half]  = (u+q-t >= q) ? u-t : u+q-t
 *   Conversion: enter Montgomery on input; exit on output.
 * Ref:       Montgomery 1985; Longa & Naehrig CANS 2016, Alg. 1.
 * Invariant: twiddles must be in Montgomery form (from ntt_alloc_twiddles_mont).
 */
void ntt_forward_mont(uint64_t *a, const uint64_t *twiddles, const ntt_params_t *p)
{
    uint64_t   n   = p->n;
    uint64_t   q   = p->q;
    mont_ctx_t ctx;
    mont_ctx_init(&ctx, q);

    /* Convert input to Montgomery form: a[i] ← a[i] * R mod q */
    for (uint64_t i = 0; i < n; i++)
        a[i] = mont_enter(a[i], &ctx);

    bit_reverse_perm(a, n, p->log2_n);

    for (uint64_t len = 2; len <= n; len <<= 1) {
        uint64_t half   = len >> 1;
        uint64_t stride = n / len;
        for (uint64_t k = 0; k < n; k += len) {
            for (uint64_t j = 0; j < half; j++) {
                uint64_t u = a[k + j];
                uint64_t t = mont_mul(twiddles[j * stride], a[k + j + half], &ctx);
                uint64_t s = u + t;
                uint64_t d = u + q - t;
                a[k + j]        = s >= q ? s - q : s;
                a[k + j + half] = d >= q ? d - q : d;
            }
        }
    }

    /* Convert output back to standard form: a[i] ← a[i] * R^{-1} mod q */
    for (uint64_t i = 0; i < n; i++)
        a[i] = mont_exit(a[i], &ctx);
}

/*
 * ntt_inverse_mont: in-place INTT with Montgomery butterfly.
 * Applies ntt_forward_mont with inverse twiddles, then scales by n^{-1}.
 * twiddles_inv: from ntt_alloc_twiddles_inv_mont(p).
 */
void ntt_inverse_mont(uint64_t *a, const uint64_t *twiddles_inv, const ntt_params_t *p)
{
    uint64_t   n   = p->n;
    uint64_t   q   = p->q;
    mont_ctx_t ctx;
    mont_ctx_init(&ctx, q);

    for (uint64_t i = 0; i < n; i++)
        a[i] = mont_enter(a[i], &ctx);

    bit_reverse_perm(a, n, p->log2_n);

    for (uint64_t len = 2; len <= n; len <<= 1) {
        uint64_t half   = len >> 1;
        uint64_t stride = n / len;
        for (uint64_t k = 0; k < n; k += len) {
            for (uint64_t j = 0; j < half; j++) {
                uint64_t u = a[k + j];
                uint64_t t = mont_mul(twiddles_inv[j * stride], a[k + j + half], &ctx);
                uint64_t s = u + t;
                uint64_t d = u + q - t;
                a[k + j]        = s >= q ? s - q : s;
                a[k + j + half] = d >= q ? d - q : d;
            }
        }
    }

    /* Scale by n^{-1} mod q and exit Montgomery form */
    uint64_t n_inv_mont = mont_enter(p->n_inv, &ctx);
    for (uint64_t i = 0; i < n; i++)
        a[i] = mont_exit(mont_mul(a[i], n_inv_mont, &ctx), &ctx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LAZY REFERENCE (static inline — for selftest comparison only)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ref_ntt_{forward,inverse}: lazy CT-DIT reference for selftest only. */
static void ref_ntt_forward(uint64_t *a, const uint64_t *tw, const ntt_params_t *p)
{
    uint64_t n = p->n, q = p->q;
    bit_reverse_perm(a, n, p->log2_n);
    for (uint64_t len = 2; len <= n; len <<= 1) {
        uint64_t half = len >> 1, stride = n / len;
        for (uint64_t k = 0; k < n; k += len)
            for (uint64_t j = 0; j < half; j++) {
                uint64_t u = a[k+j];
                uint64_t t = (uint64_t)((__uint128_t)tw[j*stride] * a[k+j+half] % q);
                a[k+j]      = u + t;
                a[k+j+half] = u + q - t;
            }
    }
    for (uint64_t i = 0; i < n; i++) a[i] %= q;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SELFTEST AND BENCHMARK
 * ═══════════════════════════════════════════════════════════════════════════ */

static int selftest(const ntt_params_t *p)
{
    uint64_t n   = p->n;
    uint64_t *tw  = ntt_alloc_twiddles(p);
    uint64_t *twi = ntt_alloc_twiddles_inv(p);
    uint64_t *twm = ntt_alloc_twiddles_mont(p);
    uint64_t *twmi = ntt_alloc_twiddles_inv_mont(p);
    uint64_t *ref  = malloc(n * sizeof *ref);
    uint64_t *mont = malloc(n * sizeof *mont);
    int ok = 0;

    if (!tw || !twi || !twm || !twmi || !ref || !mont) { ok = -1; goto done; }

    for (uint64_t i = 0; i < n; i++)
        ref[i] = mont[i] = (i * 1234567891ULL + 42) % p->q;

    ref_ntt_forward(ref, tw, p);
    ntt_forward_mont(mont, twm, p);

    for (uint64_t i = 0; i < n; i++) {
        if (ref[i] != mont[i]) {
            printf("  \033[1;31mFAIL\033[0m forward mismatch at %lu: lazy=%lu mont=%lu\n",
                   i, ref[i], mont[i]);
            ok = -1; goto done;
        }
    }

    /* Round-trip via Montgomery */
    ntt_inverse_mont(mont, twmi, p);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t orig = (i * 1234567891ULL + 42) % p->q;
        if (mont[i] != orig) {
            printf("  \033[1;31mFAIL\033[0m round-trip at %lu: expected=%lu got=%lu\n",
                   i, orig, mont[i]);
            ok = -1; goto done;
        }
    }

done:
    free(tw); free(twi); free(twm); free(twmi); free(ref); free(mont);
    return ok;
}

static double bench_one(const ntt_params_t *p, const uint64_t *tw,
                         void (*fn)(uint64_t *, const uint64_t *, const ntt_params_t *),
                         uint64_t iters)
{
    uint64_t n  = p->n;
    uint64_t *a = malloc(n * sizeof *a);
    if (!a) return -1.0;
    for (uint64_t i = 0; i < n; i++) a[i] = i % p->q;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint64_t it = 0; it < iters; it++) fn(a, tw, p);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    free(a);
    return (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
}

static void run_benchmarks(const ntt_params_t *p, uint64_t iters)
{
    uint64_t *tw   = ntt_alloc_twiddles(p);
    uint64_t *twm  = ntt_alloc_twiddles_mont(p);
    if (!tw || !twm) { free(tw); free(twm); return; }

    /* Warm-up */
    uint64_t *tmp = malloc(p->n * sizeof *tmp);
    if (tmp) {
        for (uint64_t i = 0; i < p->n; i++) tmp[i] = i % p->q;
        ref_ntt_forward(tmp, tw, p);
        ntt_forward_mont(tmp, twm, p);
        free(tmp);
    }

    double t_lazy = bench_one(p, tw,  ref_ntt_forward,  iters);
    double t_mont = bench_one(p, twm, ntt_forward_mont, iters);

    double ntts_lazy = (double)iters / t_lazy;
    double ntts_mont = (double)iters / t_mont;
    double speedup   = t_lazy / t_mont;
    double bf        = (double)(p->n >> 1) * (double)p->log2_n;

    printf(CYN "  ── Benchmark: lazy vs Montgomery (CPU, n=%lu, q=%lu) ──\n" RST, p->n, p->q);
    printf("  ┌──────────────────┬───────────────┬───────────────┬──────────┐\n");
    printf("  │ " CYN "%-16s" RST " │ " CYN "%-13s" RST " │ " CYN "%-13s" RST " │ " CYN "%-8s" RST " │\n",
           "Metric", "Lazy (128-bit)", "Montgomery", "Speedup");
    printf("  ├──────────────────┼───────────────┼───────────────┼──────────┤\n");
    printf("  │ %-16s │ %10.0f /s  │ %10.0f /s  │ %6.2f×  │\n",
           "NTT/s", ntts_lazy, ntts_mont, speedup);
    printf("  │ %-16s │ %10.1f ns  │ %10.1f ns  │ %6.2f×  │\n",
           "ns / NTT",
           t_lazy * 1e9 / (double)iters, t_mont * 1e9 / (double)iters, speedup);
    printf("  │ %-16s │ %10.2f ns  │ %10.2f ns  │ %6.2f×  │\n",
           "ns / butterfly",
           t_lazy * 1e9 / ((double)iters * bf), t_mont * 1e9 / ((double)iters * bf), speedup);
    printf("  └──────────────────┴───────────────┴───────────────┴──────────┘\n\n");

    /* Write to file */
    char fname[64];
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    strftime(fname, sizeof fname, "bench_mont_%Y%m%d_%H%M%S.txt", tm);
    FILE *f = fopen(fname, "w");
    if (f) {
        fprintf(f, "n=%lu q=%lu omega=%lu iters=%lu\n", p->n, p->q, p->omega, iters);
        fprintf(f, "lazy_ntts_per_s=%.0f mont_ntts_per_s=%.0f speedup=%.3f\n",
                ntts_lazy, ntts_mont, speedup);
        fclose(f);
        printf("  Results written to %s\n\n", fname);
    }
    free(tw); free(twm);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    ntt_params_t p;
    p.n     = (argc > 1) ? (uint64_t)atol(argv[1]) : 256;
    p.q     = (argc > 2) ? (uint64_t)atol(argv[2]) : 3329;
    p.omega = (argc > 3) ? (uint64_t)atol(argv[3]) : 17;
    uint64_t iters = (argc > 4) ? (uint64_t)atol(argv[4]) : 200000;

    printf("\n" WHT
           "╔══════════════════════════════════════════════════════════╗\n"
           "║      NTT / MI300A  —  Montgomery Multiply Kernel        ║\n"
           "╚══════════════════════════════════════════════════════════╝\n"
           RST "\n");

    hw_info_t hw;
    hw.n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    hw.page_sz = sysconf(_SC_PAGE_SIZE);
    hw.phys_pages = sysconf(_SC_PHYS_PAGES);
    hw.mem_bytes = (uint64_t)hw.phys_pages * (uint64_t)hw.page_sz;
    printf(CYN "  CPUs: " RST "%ld   " CYN "RAM: " RST "%.1f GB\n\n",
           hw.n_cpus, hw.mem_bytes / (1024.0*1024.0*1024.0));

    if (ntt_params_init(&p) != 0) {
        fprintf(stderr, "error: n=%lu is not a power of 2\n", p.n);
        return 1;
    }

    /* Print Montgomery context */
    mont_ctx_t ctx; mont_ctx_init(&ctx, p.q);
    printf(CYN "  ── Montgomery Setup (q=%lu) ───────────────────────────\n" RST, p.q);
    printf("  ┌──────────────────────────┬─────────────────────────┐\n");
    printf("  │ " CYN "%-24s" RST " │ " CYN "%-23s" RST " │\n", "Parameter", "Value");
    printf("  ├──────────────────────────┼─────────────────────────┤\n");
    printf("  │ %-24s │ %-23s │\n", "R", "2^32");
    printf("  │ %-24s │ %-23lu │\n", "R^2 mod q", ctx.r2);
    printf("  │ %-24s │ 0x%-21X │\n", "-q^{-1} mod R", ctx.q_prime);
    printf("  │ %-24s │ %-23u │\n", "verify: q * q' mod R",
           (uint32_t)((uint64_t)p.q * ctx.q_prime + 1));  /* should be 0 if q*q'≡-1 */
    printf("  └──────────────────────────┴─────────────────────────┘\n\n");

    printf(CYN "  ── Selftest ─────────────────────────────────────────────\n" RST);
    int result = selftest(&p);
    if (result == 0)
        printf("  \033[1;32mPASS\033[0m  mont output == lazy output; round-trip OK\n\n");
    else {
        printf("  \033[1;31mFAIL\033[0m  see above\n\n");
        return 1;
    }

    run_benchmarks(&p, iters);
    return 0;
}
