/* verify.c - see verify.h */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <omp.h>
#include "verify.h"
#include "bigint.h"

typedef unsigned __int128 u128;
const uint64_t t1_q[T1_NQ] = {4611686018427388039ULL, 4611686018427388083ULL, 4611686018427388087ULL,
                              4611686018427388151ULL, 4611686018427388219ULL, 4611686018427388223ULL,
                              4611686018427388253ULL, 4611686018427388301ULL};

static inline uint64_t mulmod(uint64_t a, uint64_t b, uint64_t q) { return (uint64_t)((u128)a * b % q); }
uint64_t vf_pow_mod(uint64_t b, unsigned long e, uint64_t q)
{
    uint64_t r = 1; b %= q;
    while (e) { if (e & 1) r = mulmod(r, b, q); b = mulmod(b, b, q); e >>= 1; }
    return r;
}
/* sum a[i] 2^(64 i) mod q: chunks in parallel, each chunk's value and 2^(64 len) */
uint64_t vf_limbs_mod(const uint64_t *a, size_t n, uint64_t q)
{
    if (!n) return 0;
    int T = omp_get_max_threads(); if ((size_t)T > n / 4096 + 1) T = (int)(n / 4096 + 1);
    uint64_t *cv = (uint64_t *)malloc(2 * T * 8), *cw = cv + T;
    uint64_t b64 = bi_decimal ? BI_B10 % q : (uint64_t)(((u128)1 << 64) % q);   /* B mod q */
#pragma omp parallel for num_threads(T) schedule(static)
    for (int t = 0; t < T; t++) {
        size_t k0 = n * t / T, k1 = n * (t + 1) / T;
        u128 v = 0;
        if (bi_decimal) for (size_t k = k1; k-- > k0;) v = (v * BI_B10 + a[k]) % q;
        else for (size_t k = k1; k-- > k0;) v = ((v << 64) | a[k]) % q;
        cv[t] = (uint64_t)v; cw[t] = vf_pow_mod(b64, k1 - k0, q);
    }
    uint64_t r = 0;
    for (int t = T; t-- > 0;) r = (mulmod(r, cw[t], q) + cv[t]) % q;
    free(cv);
    return r;
}
uint64_t vf_digits_mod(const char *s, size_t n, uint64_t q)
{
    if (!n) return 0;
    int T = omp_get_max_threads(); if ((size_t)T > n / 4096 + 1) T = (int)(n / 4096 + 1);
    uint64_t *cv = (uint64_t *)malloc(2 * T * 8), *cw = cv + T;
#pragma omp parallel for num_threads(T) schedule(static)
    for (int t = 0; t < T; t++) {
        size_t k0 = n * t / T, k1 = n * (t + 1) / T;
        uint64_t v = 0, p18 = 1000000000000000000ULL % q;
        size_t k = k0;
        for (; k + 18 <= k1; k += 18) {                     /* 18 digits per modulo */
            uint64_t c = 0;
            for (int i = 0; i < 18; i++) c = c * 10 + (uint64_t)(s[k + i] - '0');
            v = (uint64_t)(((u128)v * p18 + c) % q);
        }
        for (; k < k1; k++) v = (uint64_t)(((u128)v * 10 + (s[k] - '0')) % q);
        cv[t] = v; cw[t] = vf_pow_mod(10, k1 - k0, q);
    }
    uint64_t r = 0;
    for (int t = 0; t < T; t++) r = (mulmod(r, cw[t], q) + cv[t]) % q;
    free(cv);
    return r;
}
/* P(a,b), Q(a,b) mod q over [a,b): right to left, P = Q + P, Q = k Q */
static void pq_span(unsigned long a, unsigned long b, uint64_t q, uint64_t *p, uint64_t *qq)
{
    uint64_t P = 1, Q = (b - 1) % q;
    for (unsigned long k = b - 1; k-- > a;) { P = (P + Q) % q; Q = mulmod(Q, k % q, q); }
    *p = P; *qq = Q;
}
void vf_pq_mod(unsigned long N, uint64_t q, uint64_t *p, uint64_t *qq)
{
    int T = omp_get_max_threads() * 4;
    uint64_t *cp = (uint64_t *)malloc(2 * T * 8), *cq = cp + T;
#pragma omp parallel for schedule(dynamic, 1)
    for (int t = 0; t < T; t++) {
        unsigned long a = 1 + (unsigned long)((u128)N * t / T), b = 1 + (unsigned long)((u128)N * (t + 1) / T);
        if (b > a) pq_span(a, b, q, &cp[t], &cq[t]); else { cp[t] = 0; cq[t] = 1; }
    }
    /* combine left to right: P = P1 Q2 + P2, Q = Q1 Q2 */
    uint64_t P = cp[0], Q = cq[0];
    for (int t = 1; t < T; t++) { P = (mulmod(P, cq[t], q) + cp[t]) % q; Q = mulmod(Q, cq[t], q); }
    free(cp);
    *p = P; *qq = Q;
}

int tier1_res(unsigned long N, unsigned long d, const uint64_t *Pres, const uint64_t *Qres, const bigint *X, const bigint *R, int verbose)
{ return tier1_res_pq(N, d, Pres, Qres, X, R, 0, 0, 0, verbose); }
/* pq_pre / qq_pre: the term recurrence's P, Q mod q already computed (Phase 8: in the background during bs); xres_pre: X mod q likewise */
int tier1_res_pq(unsigned long N, unsigned long d, const uint64_t *Pres, const uint64_t *Qres, const bigint *X, const bigint *R, const uint64_t *pq_pre, const uint64_t *qq_pre, const uint64_t *xres_pre, int verbose)
{
    int bad = 0;
    for (int i = 0; i < T1_NQ; i++) {
        uint64_t q = t1_q[i], p, qq;
        if (pq_pre) { p = pq_pre[i]; qq = qq_pre[i]; } else vf_pq_mod(N, q, &p, &qq);
        uint64_t Pm = Pres[i], Qm = Qres[i];
        uint64_t Xm = xres_pre ? xres_pre[i] : vf_limbs_mod(X->l, X->n, q), Rm = vf_limbs_mod(R->l, R->n, q);
        uint64_t Tm = vf_pow_mod(10, d, q);
        uint64_t lhs = mulmod(Tm, (p + qq) % q, q), rhs = (mulmod(Xm, Qm, q) + Rm) % q;
        int ok = Pm == p && Qm == qq && lhs == rhs;
        if (!ok) bad++;
        if (verbose || !ok) printf("  T1 q%d=%llu: P %s, Q %s, T(P+Q) == XQ+R %s\n", i, (unsigned long long)q,
                                   Pm == p ? "ok" : "BAD", Qm == qq ? "ok" : "BAD", lhs == rhs ? "ok" : "BAD");
    }
    return bad;
}
int tier1_digits_res(const char *digits, size_t ndig, const uint64_t *Xres, int verbose)
{
    int bad = 0;
    for (int i = 0; i < T1_NQ; i++) {
        uint64_t q = t1_q[i];
        uint64_t Dm = vf_digits_mod(digits, ndig, q), Xm = Xres[i];
        if (Dm != Xm) bad++;
        if (verbose || Dm != Xm) printf("  T1 q%d: digits == X %s\n", i, Dm == Xm ? "ok" : "BAD");
    }
    return bad;
}

static const struct { unsigned long off; const char *w; } windows[] = {
    {50,         "59574966967627724076630353547594571382178525166427"},
    {1000000,    "88374711515623968271347126772832291250652542450798"},
    {100000000,  "25522594276661070064277361046962720701530988137395"},
    {1000000000, "90420663734387717556599743385951035678710858200191"},   /* last 50 of e_1e9: window at 10^9 - 49 */
};
/* extra windows from ECALC_WINDOWS (lines "offset 50digits", 1-based fractional offset) */
static int tier2_file(const char *digits, size_t ndig, int verbose)
{
    const char *fn = getenv("ECALC_WINDOWS"); if (!fn) return 0;
    FILE *f = fopen(fn, "r"); if (!f) { printf("  T2: cannot open %s\n", fn); return 0; }
    char line[256]; int bad = 0, n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long o; char w[64];
        if (sscanf(line, "%lu %63s", &o, w) != 2 || strlen(w) != 50) continue;
        if (o + 50 > ndig) continue;
        int ok = !memcmp(digits + o, w, 50); n++;
        if (!ok) bad++;
        if (verbose || !ok) printf("  T2 window@%lu (file) %s\n", o, ok ? "ok" : "BAD");
        if (!ok) printf("     got  %.50s\n     want %s\n", digits + o, w);
    }
    fclose(f);
    if (n) printf("  T2: %d windows from %s, %d bad\n", n, fn, bad);
    return bad;
}
int tier2(const char *digits, size_t ndig, int verbose)
{
    int bad = tier2_file(digits, ndig, verbose);
    for (size_t i = 0; i < sizeof windows / sizeof *windows; i++) {
        unsigned long o = windows[i].off;
        if (o == 1000000000) o = 1000000000 - 49;
        if (o + 50 > ndig) continue;
        int ok = !memcmp(digits + o, windows[i].w, 50);
        if (!ok) bad++;
        if (verbose || !ok) printf("  T2 window@%lu %s%s\n", windows[i].off, ok ? "ok" : "BAD: ", ok ? "" : "");
        if (!ok) printf("     got  %.50s\n     want %s\n", digits + o, windows[i].w);
    }
    return bad;
}

int tier1(unsigned long N, unsigned long d, const bigint *P, const bigint *Q, const bigint *X, const bigint *R, int verbose)
{
    uint64_t Pres[T1_NQ], Qres[T1_NQ];
    for (int i = 0; i < T1_NQ; i++) { Pres[i] = vf_limbs_mod(P->l, P->n, t1_q[i]); Qres[i] = vf_limbs_mod(Q->l, Q->n, t1_q[i]); }
    return tier1_res(N, d, Pres, Qres, X, R, verbose);
}

int tier1_digits(const char *digits, size_t ndig, const bigint *X, int verbose)
{
    uint64_t Xres[T1_NQ];
    for (int i = 0; i < T1_NQ; i++) Xres[i] = vf_limbs_mod(X->l, X->n, t1_q[i]);
    return tier1_digits_res(digits, ndig, Xres, verbose);
}
