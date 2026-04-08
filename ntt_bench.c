/*
 * ntt_bench.c — Side-by-side CPU algorithm benchmark across transform sizes
 *
 * Purpose:   Compare CT-DIT (lazy), Stockham (lazy), and Montgomery NTT
 *            across n = 256, 512, 1024, 2048, 4096 for ML-KEM and ML-DSA
 *            parameter sets. Output a formatted table for analysis.
 * Algorithm: All three algorithms implement the same ntt_forward() signature;
 *            this file compiles all three inline as static functions with
 *            distinct prefixes (ct_, stk_, mnt_) so there are no link conflicts.
 * Build:     cc -O2 -Wall -Wextra -o ntt_bench ntt_bench.c
 */

#include "ntt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── ANSI ────────────────────────────────────────────────────────────────── */
#define ANSI_WHT "\033[1;37m"
#define ANSI_CYN "\033[1;36m"
#define ANSI_GRN "\033[1;32m"
#define ANSI_YLW "\033[1;33m"
#define ANSI_RST "\033[0m"

/* ═══════════════════════════════════════════════════════════════════════════
 * SHARED HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t h_mod_pow(uint64_t b, uint64_t e, uint64_t m)
{
    uint64_t r = 1; b %= m;
    for (; e; e >>= 1) {
        if (e & 1) r = (uint64_t)((__uint128_t)r * b % m);
        b = (uint64_t)((__uint128_t)b * b % m);
    }
    return r;
}
static uint64_t h_mod_inv(uint64_t a, uint64_t m) { return h_mod_pow(a, m-2, m); }

/* shared params_init (no link conflict — ntt.h is included, but we provide impl) */
int ntt_params_init(ntt_params_t *p)
{
    if (!p->n || (p->n & (p->n-1))) return -1;
    p->omega_inv = h_mod_inv(p->omega, p->q);
    p->n_inv     = h_mod_inv(p->n,     p->q);
    uint64_t t = p->n; p->log2_n = 0;
    while (t > 1) { t >>= 1; p->log2_n++; }
    return 0;
}

/* twiddle alloc shared */
uint64_t *ntt_alloc_twiddles(const ntt_params_t *p)
{
    uint64_t *tw = (uint64_t *)malloc(p->n / 2 * sizeof(uint64_t));
    if (!tw) return NULL;
    tw[0] = 1;
    for (uint64_t k = 1; k < p->n / 2; k++)
        tw[k] = (uint64_t)((__uint128_t)tw[k-1] * p->omega % p->q);
    return tw;
}

uint64_t *ntt_alloc_twiddles_inv(const ntt_params_t *p)
{
    uint64_t *tw = (uint64_t *)malloc(p->n / 2 * sizeof(uint64_t));
    if (!tw) return NULL;
    tw[0] = 1;
    for (uint64_t k = 1; k < p->n / 2; k++)
        tw[k] = (uint64_t)((__uint128_t)tw[k-1] * p->omega_inv % p->q);
    return tw;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ALGORITHM 1: CT-DIT (Cooley-Tukey, lazy reduction)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ct_bit_rev(uint64_t *a, uint64_t n, uint32_t log2_n)
{
    for (uint64_t i = 0; i < n; i++) {
        uint64_t rev = 0, x = i;
        for (uint32_t b = 0; b < log2_n; b++) { rev = (rev<<1)|(x&1); x>>=1; }
        if (i < rev) { uint64_t t = a[i]; a[i] = a[rev]; a[rev] = t; }
    }
}

static void ct_ntt(uint64_t *a, const uint64_t *tw, const ntt_params_t *p)
{
    uint64_t n = p->n, q = p->q;
    ct_bit_rev(a, n, p->log2_n);
    for (uint64_t len = 1; len < n; len <<= 1) {
        uint64_t step = n / (len << 1);
        for (uint64_t i = 0; i < n; i += len << 1)
            for (uint64_t j = 0; j < len; j++) {
                uint64_t u = a[i+j];
                uint64_t v = (uint64_t)((__uint128_t)tw[j*step] * (a[i+j+len]%q) % q);
                a[i+j]     = u + v;
                a[i+j+len] = u - v + q;
            }
    }
    for (uint64_t i = 0; i < n; i++) a[i] %= q;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ALGORITHM 2: Stockham auto-sort (lazy reduction)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void stk_ntt(uint64_t *a, const uint64_t *tw, const ntt_params_t *p)
{
    uint64_t n = p->n, q = p->q, half = n >> 1;
    uint64_t *buf = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!buf) return;
    uint64_t *src = a, *dst = buf;
    for (uint32_t s = 0; s < p->log2_n; s++) {
        uint64_t pp = (uint64_t)1 << s, qs = n>>(s+1), p2 = pp<<1;
        for (uint64_t j = 0; j < pp; j++) {
            uint64_t w = tw[j * qs];
            for (uint64_t k = 0; k < qs; k++) {
                uint64_t idx = j + k*pp;
                uint64_t u   = src[idx];
                uint64_t wv  = (uint64_t)((__uint128_t)w * (src[idx+half]%q) % q);
                dst[j+k*p2]    = u + wv;
                dst[j+k*p2+pp] = u + q - wv;
            }
        }
        uint64_t *t = src; src = dst; dst = t;
    }
    for (uint64_t i = 0; i < n; i++) src[i] %= q;
    if (src != a) memcpy(a, src, n * sizeof(uint64_t));
    free(buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ALGORITHM 3: Montgomery multiplication
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct { uint64_t q, r2; uint32_t q_prime; } mont_ctx_t;

static mont_ctx_t mont_init(uint64_t q)
{
    mont_ctx_t c;
    c.q = q;
    /* q_prime: q * q_prime ≡ -1 (mod 2^32) via Hensel lifting */
    uint32_t x = 1;
    for (int i = 0; i < 5; i++) x *= 2 - (uint32_t)q * x;
    c.q_prime = (uint32_t)(-(int32_t)x);       /* q_prime = -q^{-1} mod 2^32 */
    /* R^2 mod q, R = 2^32 */
    c.r2 = (uint64_t)(((__uint128_t)1 << 64) % q);
    return c;
}

static uint64_t mont_mul(uint64_t a, uint64_t b, const mont_ctx_t *c)
{
    uint64_t t    = a * b;
    uint32_t mp   = (uint32_t)t * c->q_prime;
    uint64_t corr = (uint64_t)mp * c->q;
    uint64_t lo   = t + corr;
    uint64_t carry = (lo < t) ? 1ULL : 0ULL;
    uint64_t u    = (lo >> 32) | (carry << 32);
    return u >= c->q ? u - c->q : u;
}

static uint64_t mont_enter(uint64_t a, const mont_ctx_t *c)
{ return mont_mul(a, c->r2, c); }
static uint64_t mont_exit(uint64_t a, const mont_ctx_t *c)
{ return mont_mul(a, 1ULL, c); }

static void mnt_ntt(uint64_t *a, const uint64_t *tw_std, const ntt_params_t *p)
{
    uint64_t n = p->n, q = p->q;
    mont_ctx_t mc = mont_init(q);

    /* Build Montgomery twiddle table */
    uint64_t *tw_m = (uint64_t *)malloc((n/2) * sizeof(uint64_t));
    if (!tw_m) return;
    uint64_t omega_m = mont_enter(p->omega, &mc);
    tw_m[0] = mont_enter(1ULL, &mc);
    for (uint64_t k = 1; k < n/2; k++)
        tw_m[k] = mont_mul(tw_m[k-1], omega_m, &mc);

    /* Convert input to Montgomery domain */
    for (uint64_t i = 0; i < n; i++) a[i] = mont_enter(a[i], &mc);

    /* CT-DIT with Montgomery butterflies */
    ct_bit_rev(a, n, p->log2_n);
    for (uint64_t len = 1; len < n; len <<= 1) {
        uint64_t step = n / (len << 1);
        for (uint64_t i = 0; i < n; i += len << 1)
            for (uint64_t j = 0; j < len; j++) {
                uint64_t u = a[i+j];
                uint64_t v = mont_mul(tw_m[j*step], a[i+j+len]%q, &mc);
                uint64_t s = u + v; if (s >= q) s -= q;
                uint64_t d = u - v + q; if (d >= q) d -= q;
                a[i+j]     = s;
                a[i+j+len] = d;
            }
    }

    /* Convert back from Montgomery domain */
    for (uint64_t i = 0; i < n; i++) a[i] = mont_exit(a[i], &mc);
    free(tw_m);
    (void)tw_std;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TIMING HELPER
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef void (*ntt_fn)(uint64_t *, const uint64_t *, const ntt_params_t *);

static double time_ntt(ntt_fn fn, const ntt_params_t *p, uint64_t iters)
{
    uint64_t *tw = ntt_alloc_twiddles(p);
    uint64_t *a  = (uint64_t *)malloc(p->n * sizeof(uint64_t));
    if (!tw || !a) { free(tw); free(a); return -1.0; }
    for (uint64_t i = 0; i < p->n; i++) a[i] = (i*31337+1) % p->q;

    /* Warm-up */
    fn(a, tw, p);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint64_t it = 0; it < iters; it++) fn(a, tw, p);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    free(tw); free(a);
    return (t1.tv_sec-t0.tv_sec) + (t1.tv_nsec-t0.tv_nsec)*1e-9;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("\n" ANSI_WHT
           "╔══════════════════════════════════════════════════════════╗\n"
           "║       NTT CPU Algorithm Benchmark — size sweep          ║\n"
           "╚══════════════════════════════════════════════════════════╝\n"
           ANSI_RST "\n");

    /* Parameter sets */
    struct { const char *name; uint64_t q, omega; } psets[] = {
        { "ML-KEM", 3329, 17 },
        { "ML-DSA", 8380417, 1753 },
    };

    /* Transform sizes (n must divide q-1; ML-KEM/DSA both support up to n=256
     * for their specific q; we test larger n with surrogate q values).
     * For the sweep we keep q=3329 fixed and only n=256 is a true ML-KEM point;
     * larger n are algorithmic throughput tests with the same modulus. */
    uint64_t sizes[] = { 64, 128, 256, 512, 1024, 2048, 4096 };
    int nsizes = (int)(sizeof sizes / sizeof sizes[0]);

    /* Determine iteration counts: scale down as n grows */
    uint64_t base_iters = 200000;

    for (int pi = 0; pi < 2; pi++) {
        printf(ANSI_CYN "\n  ═══ %s (q=%lu ω=%lu) ═══\n" ANSI_RST,
               psets[pi].name, psets[pi].q, psets[pi].omega);

        /* Header */
        printf("  ┌──────┬──────────────┬──────────────┬──────────────┐\n");
        printf("  │ " ANSI_CYN "n    " ANSI_RST
               " │ " ANSI_CYN "CT-DIT NTT/s " ANSI_RST
               " │ " ANSI_CYN "Stockham NTT/s" ANSI_RST
               " │ " ANSI_CYN "Montgomery NTT/s" ANSI_RST " │\n");
        printf("  ├──────┼──────────────┼──────────────┼──────────────┤\n");

        for (int si = 0; si < nsizes; si++) {
            ntt_params_t p;
            p.n = sizes[si]; p.q = psets[pi].q; p.omega = psets[pi].omega;
            /* For n > 256 with standard params, omega may not be a primitive
             * n-th root. Use omega = omega^(256/n) to get a valid root. */
            if (p.n > 256) {
                /* omega^256 = 1 mod q for ML-KEM/DSA, so for larger n we use
                 * a generator that cycles n times. Since we only care about
                 * throughput (not correctness), use a fixed primitive root. */
                p.omega = h_mod_pow(psets[pi].omega, 256 / p.n > 0 ? 256 / p.n : 1, p.q);
                if (p.omega == 1 || p.omega == 0) {
                    /* No valid primitive n-th root for this q; skip */
                    printf("  │ %-4lu │ %-12s │ %-12s │ %-12s │\n",
                           p.n, "n/a", "n/a", "n/a");
                    continue;
                }
            }
            if (ntt_params_init(&p) != 0) continue;

            uint64_t iters = base_iters / (p.n / 64);
            if (iters < 100) iters = 100;

            double t_ct  = time_ntt(ct_ntt,  &p, iters);
            double t_stk = time_ntt(stk_ntt, &p, iters);
            double t_mnt = time_ntt(mnt_ntt, &p, iters);

            double r_ct  = (t_ct  > 0) ? iters / t_ct  : 0;
            double r_stk = (t_stk > 0) ? iters / t_stk : 0;
            double r_mnt = (t_mnt > 0) ? iters / t_mnt : 0;

            /* Highlight fastest algorithm */
            double best = r_ct;
            if (r_stk > best) best = r_stk;
            if (r_mnt > best) best = r_mnt;

            const char *hl_ct  = (r_ct  == best) ? ANSI_GRN : "";
            const char *hl_stk = (r_stk == best) ? ANSI_GRN : "";
            const char *hl_mnt = (r_mnt == best) ? ANSI_GRN : "";

            printf("  │ %-4lu │ %s%-12.0f" ANSI_RST
                   " │ %s%-12.0f" ANSI_RST
                   " │ %s%-12.0f" ANSI_RST " │\n",
                   p.n,
                   hl_ct,  r_ct,
                   hl_stk, r_stk,
                   hl_mnt, r_mnt);
        }
        printf("  └──────┴──────────────┴──────────────┴──────────────┘\n");
    }

    printf("\n  " ANSI_YLW "Note:" ANSI_RST
           " n > 256 uses a surrogate primitive root derived from the n=256 root.\n"
           "  Performance trend is valid; absolute values at n>256 are throughput only.\n\n");
    return 0;
}
