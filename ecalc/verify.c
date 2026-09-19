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
void vf_pq_range_mod(unsigned long a0, unsigned long b1, uint64_t q, uint64_t *p, uint64_t *qq)
{
    int T = omp_get_max_threads() * 4; unsigned long M = b1 > a0 ? b1 - a0 : 0;
    uint64_t *cp = (uint64_t *)malloc(2 * T * 8), *cq = cp + T;
#pragma omp parallel for schedule(dynamic, 1)
    for (int t = 0; t < T; t++) {
        unsigned long a = a0 + (unsigned long)((u128)M * t / T), b = a0 + (unsigned long)((u128)M * (t + 1) / T);
        if (b > a) pq_span(a, b, q, &cp[t], &cq[t]); else { cp[t] = 0; cq[t] = 1; }
    }
    /* combine left to right: P = P1 Q2 + P2, Q = Q1 Q2 */
    uint64_t P = cp[0], Q = cq[0];
    for (int t = 1; t < T; t++) { P = (mulmod(P, cq[t], q) + cp[t]) % q; Q = mulmod(Q, cq[t], q); }
    free(cp);
    *p = P; *qq = Q;
}
void vf_pq_mod(unsigned long N, uint64_t q, uint64_t *p, uint64_t *qq) { vf_pq_range_mod(1, N + 1, q, p, qq); }
void vf_pq_join(uint64_t *P, uint64_t *Q, uint64_t pb, uint64_t qb, uint64_t q) { *P = (mulmod(*P, qb, q) + pb) % q; *Q = mulmod(*Q, qb, q); }
uint64_t vf_add_mod(uint64_t a, uint64_t b, uint64_t q) { return (a + b) % q; }
uint64_t vf_digits_join(uint64_t acc, size_t len, uint64_t v, uint64_t q) { return (mulmod(acc, vf_pow_mod(10, len, q), q) + v) % q; }
uint64_t vf_shift_res(uint64_t res, size_t lo, uint64_t q)
{
    uint64_t b = bi_decimal ? BI_B10 % q : (uint64_t)(((u128)1 << 64) % q);
    return mulmod(res, vf_pow_mod(b, lo, q), q);
}
/* one pass over the string for nq primes: each 18-digit block parsed once, one modmul per prime */
void vf_digits_mods(const char *s, size_t n, const uint64_t *qs, int nq, uint64_t *out)
{
    for (int j = 0; j < nq; j++) out[j] = 0;
    if (!n) return;
    int T = omp_get_max_threads(); if ((size_t)T > n / 4096 + 1) T = (int)(n / 4096 + 1);
    uint64_t *cv = (uint64_t *)malloc((size_t)T * nq * 8);
#pragma omp parallel for num_threads(T) schedule(static)
    for (int t = 0; t < T; t++) {
        size_t k0 = n * t / T, k1 = n * (t + 1) / T;
        uint64_t v[16], p18[16]; for (int j = 0; j < nq; j++) { v[j] = 0; p18[j] = 1000000000000000000ULL % qs[j]; }
        size_t k = k0;
        for (; k + 18 <= k1; k += 18) {
            uint64_t c = 0;
            for (int i = 0; i < 18; i++) c = c * 10 + (uint64_t)(s[k + i] - '0');
            for (int j = 0; j < nq; j++) v[j] = (uint64_t)(((u128)v[j] * p18[j] + c) % qs[j]);
        }
        for (; k < k1; k++) for (int j = 0; j < nq; j++) v[j] = (uint64_t)(((u128)v[j] * 10 + (s[k] - '0')) % qs[j]);
        for (int j = 0; j < nq; j++) cv[(size_t)t * nq + j] = v[j];
    }
    for (int j = 0; j < nq; j++) {
        uint64_t r = 0;
        for (int t = 0; t < T; t++) { size_t k0 = n * t / T, k1 = n * (t + 1) / T; r = (mulmod(r, vf_pow_mod(10, k1 - k0, qs[j]), qs[j]) + cv[(size_t)t * nq + j]) % qs[j]; }
        out[j] = r;
    }
    free(cv);
}

int tier1_res(unsigned long N, unsigned long d, const uint64_t *Pres, const uint64_t *Qres, const bigint *X, const bigint *R, int verbose)
{ return tier1_res_pq(N, d, Pres, Qres, X, R, 0, 0, 0, 0, verbose); }
/* pq_pre / qq_pre: the term recurrence's P, Q mod q already computed (Phase 8: in the background during bs); xres_pre: X mod q likewise */
int tier1_res_pq(unsigned long N, unsigned long d, const uint64_t *Pres, const uint64_t *Qres, const bigint *X, const bigint *R, const uint64_t *pq_pre, const uint64_t *qq_pre, const uint64_t *xres_pre, const uint64_t *rres_pre, int verbose)
{
    int bad = 0;
    for (int i = 0; i < T1_NQ; i++) {
        uint64_t q = t1_q[i], p, qq;
        if (pq_pre) { p = pq_pre[i]; qq = qq_pre[i]; } else vf_pq_mod(N, q, &p, &qq);
        uint64_t Pm = Pres[i], Qm = Qres[i];
        uint64_t Xm = xres_pre ? xres_pre[i] : vf_limbs_mod(X->l, X->n, q), Rm = rres_pre ? rres_pre[i] : vf_limbs_mod(R->l, R->n, q);
        uint64_t Tm = vf_pow_mod(10, d, q);
        uint64_t lhs = mulmod(Tm, (p + qq) % q, q), rhs = (mulmod(Xm, Qm, q) + Rm) % q;
        int ok = Pm == p && Qm == qq && lhs == rhs;
        if (!ok) bad++;
        if (verbose || !ok) printf("  T1 q%d=%llu: P %s, Q %s, T(P+Q) == XQ+R %s\n", i, (unsigned long long)q,
                                   Pm == p ? "ok" : "BAD", Qm == qq ? "ok" : "BAD", lhs == rhs ? "ok" : "BAD");
    }
    return bad;
}
int tier1_digits_cmp(const uint64_t *Dres, const uint64_t *Xres, int verbose)
{
    int bad = 0;
    for (int i = 0; i < T1_NQ; i++) {
        uint64_t Dm = Dres[i], Xm = Xres[i];
        if (Dm != Xm) bad++;
        if (verbose || Dm != Xm) printf("  T1 q%d: digits == X %s\n", i, Dm == Xm ? "ok" : "BAD");
    }
    return bad;
}
int tier1_digits_res(const char *digits, size_t ndig, const uint64_t *Xres, int verbose)
{
    uint64_t Dres[T1_NQ];
    for (int i = 0; i < T1_NQ; i++) Dres[i] = vf_digits_mod(digits, ndig, t1_q[i]);
    return tier1_digits_cmp(Dres, Xres, verbose);
}

static const struct { unsigned long off; const char *w; } windows[] = {
    {50,         "59574966967627724076630353547594571382178525166427"},
    {1000000,    "88374711515623968271347126772832291250652542450798"},
    {100000000,  "25522594276661070064277361046962720701530988137395"},
    {1000000000, "90420663734387717556599743385951035678710858200191"},   /* last 50 of e_1e9: window at 10^9 - 49 */
};
/* the window table: the built-in ones (the 10^9 entry at 10^9 - 49) and, once, the extra windows from ECALC_WINDOWS
 * (lines "offset 50digits", 1-based fractional offset) */
struct win { unsigned long off; char w[51]; int from_file; };
static struct win *g_win; static int g_nwin = -1, g_nwin_file; static const char *g_win_file;
void tier2_windows_reset(void) { free(g_win); g_win = 0; g_nwin = -1; g_nwin_file = 0; }
static void win_load(void)
{
    if (g_nwin >= 0) return;
    int cap = 64; g_win = (struct win *)malloc(cap * sizeof *g_win); g_nwin = 0;
    g_win_file = getenv("ECALC_WINDOWS");
    if (g_win_file) {
        FILE *f = fopen(g_win_file, "r");
        if (!f) printf("  T2: cannot open %s\n", g_win_file);
        else {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                unsigned long o; char w[64];
                if (sscanf(line, "%lu %63s", &o, w) != 2 || strlen(w) != 50) continue;
                if (g_nwin == cap) { cap *= 2; g_win = (struct win *)realloc(g_win, cap * sizeof *g_win); }
                g_win[g_nwin].off = o; memcpy(g_win[g_nwin].w, w, 51); g_win[g_nwin].from_file = 1; g_nwin++;
            }
            fclose(f);
        }
        g_nwin_file = g_nwin;
    }
    for (size_t i = 0; i < sizeof windows / sizeof *windows; i++) {
        if (g_nwin == cap) { cap *= 2; g_win = (struct win *)realloc(g_win, cap * sizeof *g_win); }
        g_win[g_nwin].off = windows[i].off == 1000000000 ? 1000000000 - 49 : windows[i].off;
        memcpy(g_win[g_nwin].w, windows[i].w, 51); g_win[g_nwin].from_file = 0; g_nwin++;
    }
}
int tier2_range(const char *s, size_t k0, size_t k1, const char *head, size_t nhead, size_t ndig, int verbose, int *nchecked)
{
    win_load();
    int bad = 0, n = 0;
    for (int i = 0; i < g_nwin; i++) {
        unsigned long o = g_win[i].off;
        if (o + 50 > ndig) continue;
        if (o + nhead < k0 || o >= k1 || o + 50 > k1) continue;
        char got[51]; got[50] = 0;
        for (int j = 0; j < 50; j++) { size_t idx = o + j; got[j] = idx < k0 ? head[nhead - (k0 - idx)] : s[idx - k0]; }
        int ok = !memcmp(got, g_win[i].w, 50); n++;
        if (!ok) bad++;
        if (verbose || !ok) printf("  T2 window@%lu%s %s\n", g_win[i].from_file ? o : (o == 1000000000 - 49 ? 1000000000 : o), g_win[i].from_file ? " (file)" : "", ok ? "ok" : "BAD");
        if (!ok) printf("     got  %s\n     want %s\n", got, g_win[i].w);
    }
    if (nchecked) *nchecked += n;
    return bad;
}
int tier2(const char *digits, size_t ndig, int verbose)
{
    win_load();
    int bad = tier2_range(digits, 0, ndig, 0, 0, ndig, verbose, 0);
    if (g_win_file && g_nwin_file) {                       /* the file's summary line, as before */
        int n = 0, b = 0;
        for (int i = 0; i < g_nwin_file; i++) { unsigned long o = g_win[i].off; if (o + 50 > ndig) continue; n++; if (memcmp(digits + o, g_win[i].w, 50)) b++; }
        if (n) printf("  T2: %d windows from %s, %d bad\n", n, g_win_file, b);
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
