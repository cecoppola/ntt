/* crt.c - see crt.h.  From bench/20_cpu (garner4, crt_carry_par_tight). */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "crt.h"
#include "modarith.h"
#include "mem.h"
#include "bigint.h"

typedef unsigned __int128 u128;
#define PR ec_P

static inline uint64_t mod3(const uint64_t *x, uint64_t p)
{
    u128 t = x[2] % p;
    t = ((t << 64) | x[1]) % p;
    t = ((t << 64) | x[0]) % p;
    return (uint64_t)t;
}
static struct garner { uint64_t c1, c2, c3; uint64_t M1[2], M2[3]; int ready; } G;

/* ---- Phase 13a P3: the prime count (modarith.h) ------------------------------------------------------------------ */
int ec_np = EC_NP;
size_t ec_np3_max_terms;
static int g_np_init;
/* floor((M - 1) / D) for M = p0 p1 p2 (3 words) and D = (10^18 - 1)^2 (2 words): the estimate from long double, then
 * corrected by exact 192-bit compares (the quotient is < 2^36) */
static size_t np3_max_terms(void)
{
    u128 m1 = (u128)PR[0] * PR[1], lo = (u128)(uint64_t)m1 * PR[2], hi = (u128)(uint64_t)(m1 >> 64) * PR[2] + (lo >> 64);
    uint64_t M[3] = { (uint64_t)lo, (uint64_t)hi, (uint64_t)(hi >> 64) };      /* p0 p1 p2 */
    u128 D = (u128)(EC_1E18 - 1) * (EC_1E18 - 1);
    long double Mf = (long double)M[2] * 0x1p128L + (long double)M[1] * 0x1p64L + (long double)M[0], Df = (long double)D;
    uint64_t q = (uint64_t)(Mf / Df) + 2;
    for (;;) {                                  /* the largest q with q D <= M - 1, i.e. q D < M */
        u128 a = (u128)(uint64_t)D * q, b = (u128)(uint64_t)(D >> 64) * q + (a >> 64);
        uint64_t P0 = (uint64_t)a, P1 = (uint64_t)b, P2 = (uint64_t)(b >> 64);
        int less = P2 != M[2] ? P2 < M[2] : P1 != M[1] ? P1 < M[1] : P0 < M[0];
        if (less) return (size_t)q;
        q--;
    }
}
int ec_np_init(void)
{
    if (g_np_init) return ec_np;
    g_np_init = 1;
    const char *e = getenv("ECALC_NP");
    if (e) {
        int v = atoi(e);
        if (v != 3 && v != 4) { ec_fatal(EC_RC_FATAL, "ECALC_NP=%s: the prime count must be 3 or 4\n", e); }
        ec_np = v;
    }
    ec_np3_max_terms = np3_max_terms();
    return ec_np;
}
void ec_np_check(size_t nterms, int decimal, const char *where)
{
    if (ec_np == 4) return;
    if (ec_np != 3) { ec_fatal(EC_RC_FATAL, "%s: ec_np = %d, must be 3 or 4\n", where, ec_np); }
    if (!ec_np3_max_terms) ec_np3_max_terms = np3_max_terms();
    if (!decimal) {
        ec_fatal(EC_RC_FATAL, "%s: ECALC_NP=3 needs base-10^18 limbs: binary 2^64 limbs need all four primes (LIMB_BASE=2 with ECALC_NP=3 is refused)\n", where);
    }
    if (nterms > ec_np3_max_terms) {
        ec_fatal(EC_RC_FATAL, "%s: ECALC_NP=3: a product of %zu terms exceeds the three-prime bound (at most %zu terms: nterms (10^18-1)^2 < p0 p1 p2)\n", where, nterms, ec_np3_max_terms);
    }
}

void crt_init(void)
{
    if (G.ready) return;
    u128 m1 = (u128)PR[0] * PR[1];
    G.c1 = ec_inv(PR[0] % PR[1], PR[1]);
    G.M1[0] = (uint64_t)m1; G.M1[1] = (uint64_t)(m1 >> 64);
    G.c2 = ec_inv((uint64_t)(m1 % PR[2]), PR[2]);
    { u128 lo = (u128)G.M1[0] * PR[2], hi = (u128)G.M1[1] * PR[2] + (lo >> 64);
      G.M2[0] = (uint64_t)lo; G.M2[1] = (uint64_t)hi; G.M2[2] = (uint64_t)(hi >> 64); }
    G.c3 = ec_inv(mod3(G.M2, PR[3]), PR[3]);
    ec_np_init();
    G.ready = 1;
}
/* P3: three primes -- Garner's first two steps of garner4: x = r0 + t1 p0 + t2 p0 p1 < p0 p1 p2 < 2^156 in 3 words */
static inline void garner3(const uint64_t r[3], uint64_t out[3])
{
    uint64_t x0 = r[0], x1;
    { uint64_t xm = x0 % PR[1]; uint64_t t1 = (uint64_t)((u128)((r[1] + PR[1] - xm) % PR[1]) * G.c1 % PR[1]);
      u128 t = (u128)t1 * PR[0] + x0; x0 = (uint64_t)t; x1 = (uint64_t)(t >> 64); }
    { uint64_t xm = (uint64_t)((((u128)x1 % PR[2]) << 64 | x0) % PR[2]);
      uint64_t t2 = (uint64_t)((u128)((r[2] + PR[2] - xm) % PR[2]) * G.c2 % PR[2]);
      u128 lo = (u128)t2 * G.M1[0] + x0, hi = (u128)t2 * G.M1[1] + x1 + (lo >> 64);
      out[0] = (uint64_t)lo; out[1] = (uint64_t)hi; out[2] = (uint64_t)(hi >> 64); }
}
void crt_garner3(const uint64_t r[3], uint64_t out[3]) { crt_init(); garner3(r, out); }

static inline void garner4(const uint64_t r[4], uint64_t out[4])
{
    uint64_t x[4] = { r[0], 0, 0, 0 };
    u128 t;
    { uint64_t xm = x[0] % PR[1]; uint64_t t1 = (uint64_t)((u128)((r[1] + PR[1] - xm) % PR[1]) * G.c1 % PR[1]);
      t = (u128)t1 * PR[0] + x[0]; x[0] = (uint64_t)t; x[1] = (uint64_t)(t >> 64); }
    { uint64_t xm = (uint64_t)((((u128)x[1] % PR[2]) << 64 | x[0]) % PR[2]);
      uint64_t t2 = (uint64_t)((u128)((r[2] + PR[2] - xm) % PR[2]) * G.c2 % PR[2]);
      u128 lo = (u128)t2 * G.M1[0] + x[0], hi = (u128)t2 * G.M1[1] + x[1] + (lo >> 64);
      x[0] = (uint64_t)lo; x[1] = (uint64_t)hi; x[2] = (uint64_t)(hi >> 64); }
    { uint64_t xm = mod3(x, PR[3]);
      uint64_t t3 = (uint64_t)((u128)((r[3] + PR[3] - xm) % PR[3]) * G.c3 % PR[3]);
      u128 s0 = (u128)t3 * G.M2[0] + x[0];
      u128 s1 = (u128)t3 * G.M2[1] + x[1] + (s0 >> 64);
      u128 s2 = (u128)t3 * G.M2[2] + x[2] + (s1 >> 64);
      out[0] = (uint64_t)s0; out[1] = (uint64_t)s1; out[2] = (uint64_t)s2; out[3] = (uint64_t)(s2 >> 64); }
}
void crt_garner4(const uint64_t r[4], uint64_t out[4]) { crt_init(); garner4(r, out); }

/* P3: the three-prime decimal stripe: Garner on planes 0..2, three digits, a window of 3 digits + carry (the same spill
 * format as the four-prime stripe, spill[3] = 0) */
static void crt3_stripe(uint64_t *const res[4], size_t k0, size_t k1, uint64_t *out, uint64_t sp[4])
{
    const uint64_t *r0 = res[0], *r1 = res[1], *r2 = res[2];
    uint64_t w0 = 0, w1 = 0, w2 = 0;
    for (size_t k = k0; k < k1; k++) {
        uint64_t r[3] = { r0[k], r1[k], r2[k] }, c[3], d[3];
        garner3(r, c); ec_words_to_dec3(c, d);
        uint64_t s = w0 + d[0], cy = s >= BI_B10; out[k] = cy ? s - BI_B10 : s;
        s = w1 + d[1] + cy; cy = s >= BI_B10; w0 = cy ? s - BI_B10 : s;
        s = w2 + d[2] + cy; cy = s >= BI_B10; w1 = cy ? s - BI_B10 : s;
        w2 = cy;
    }
    sp[0] = w0; sp[1] = w1; sp[2] = w2; sp[3] = 0;
}
void crt_carry_par4(uint64_t *const res[4], size_t n, uint64_t *out, int T)
{
    crt_init();
    if (ec_np == 3) ec_np_check(n, bi_decimal, "crt_carry_par4");      /* n coefficients: at most n terms each */
    for (size_t i = n; i < n + 4; i++) out[i] = 0;
    if (T < 1) T = 1;
    if ((size_t)T > n / 64 + 1) T = (int)(n / 64 + 1);
    uint64_t (*spill)[4] = (uint64_t (*)[4])calloc(T, sizeof *spill);
    int t;
#pragma omp parallel for num_threads(T) schedule(static)
    for (t = 0; t < T; t++) {
        size_t k0 = n * t / T, k1 = n * (t + 1) / T, k;
        if (ec_np == 3) { crt3_stripe(res, k0, k1, out, spill[t]); continue; }
        uint64_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
        for (k = k0; k < k1; k++) {
            uint64_t r[4] = { res[0][k], res[1][k], res[2][k], res[3][k] }, c[4];
            garner4(r, c);
            if (bi_decimal) {                            /* four base-B digits; window of 4 digits + carry */
                uint64_t d[4]; ec_words_to_dec4(c, d);
                uint64_t s = w0 + d[0], cy = s >= BI_B10; out[k] = cy ? s - BI_B10 : s;
                s = w1 + d[1] + cy; cy = s >= BI_B10; w0 = cy ? s - BI_B10 : s;
                s = w2 + d[2] + cy; cy = s >= BI_B10; w1 = cy ? s - BI_B10 : s;
                s = w3 + d[3] + cy; cy = s >= BI_B10; w2 = cy ? s - BI_B10 : s;
                w3 = cy;
            } else {
                u128 s;
                s = (u128)w0 + c[0]; out[k] = (uint64_t)s;
                s = (s >> 64) + w1 + c[1]; w0 = (uint64_t)s;
                s = (s >> 64) + w2 + c[2]; w1 = (uint64_t)s;
                w2 = (uint64_t)(s >> 64) + c[3];
            }
        }
        spill[t][0] = w0; spill[t][1] = w1; spill[t][2] = w2; spill[t][3] = w3;
    }
    for (t = 0; t < T; t++) {
        size_t k1 = n * (t + 1) / T, i;
        if (bi_decimal) {
            uint64_t cy = 0;
            for (i = 0; i < 4 && k1 + i < n + 4; i++) { uint64_t s = out[k1 + i] + spill[t][i] + cy; cy = s >= BI_B10; out[k1 + i] = cy ? s - BI_B10 : s; }
            for (; cy && k1 + i < n + 4; i++) { uint64_t s = out[k1 + i] + cy; cy = s >= BI_B10; out[k1 + i] = cy ? s - BI_B10 : s; }
        } else {
            u128 s = 0;
            for (i = 0; i < 3; i++) { s += (u128)out[k1 + i] + spill[t][i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
            for (i = 3; s && k1 + i < n + 4; i++) { s += out[k1 + i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
        }
    }
    free(spill);
}

void crt_carry_par4_q(uint64_t *const buf[4], size_t Q, size_t n, uint64_t *out, int T)
{
    crt_init();
    if (bi_decimal) { ec_fatal(EC_RC_FATAL, "crt_carry_par4_q: binary base only\n"); }
    ec_np_check(n, bi_decimal, "crt_carry_par4_q");                    /* (binary: refuses ec_np = 3) */
    for (size_t i = n; i < n + 4; i++) out[i] = 0;
    if (T < 4) T = 4;
    T = T / 4 * 4;
    int per = T / 4;
    uint64_t (*spill)[4] = (uint64_t (*)[4])calloc(T, sizeof *spill);
    size_t *k1s = (size_t *)calloc(T, sizeof *k1s);
    int t;
#pragma omp parallel for num_threads(T) schedule(static)
    for (t = 0; t < T; t++) {
        int q = t / per, j = t % per;
        size_t q0 = q * Q, q1 = (q + 1) * Q < n ? (q + 1) * Q : n;
        if (q0 > n) q0 = n;
        size_t len = q1 - q0, k0 = q0 + len * j / per, k1 = q0 + len * (j + 1) / per, k;
        const uint64_t *r0 = buf[q] - q0, *r1 = buf[q] + Q - q0, *r2 = buf[q] + 2 * Q - q0, *r3 = buf[q] + 3 * Q - q0;
        mem_pin_to_node(q);
        uint64_t w0 = 0, w1 = 0, w2 = 0;
        for (k = k0; k < k1; k++) {
            uint64_t r[4] = { r0[k], r1[k], r2[k], r3[k] }, c[4];
            u128 s;
            garner4(r, c);
            s = (u128)w0 + c[0]; out[k] = (uint64_t)s;
            s = (s >> 64) + w1 + c[1]; w0 = (uint64_t)s;
            s = (s >> 64) + w2 + c[2]; w1 = (uint64_t)s;
            w2 = (uint64_t)(s >> 64) + c[3];
        }
        spill[t][0] = w0; spill[t][1] = w1; spill[t][2] = w2; k1s[t] = k1;
        mem_unpin();
    }
    for (t = 0; t < T; t++) {
        size_t k1 = k1s[t], i; u128 s = 0;
        for (i = 0; i < 3; i++) { s += (u128)out[k1 + i] + spill[t][i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
        for (i = 3; s && k1 + i < n + 4; i++) { s += out[k1 + i]; out[k1 + i] = (uint64_t)s; s >>= 64; }
    }
    free(spill); free(k1s);
}
