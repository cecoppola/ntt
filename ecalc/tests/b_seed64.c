/* b_seed64 - S21 / A37-R2 (results/A37CMP.md section 2, R2): a CPU-only microbench of the decimal seed leaf.
 *
 *   ./tests/b_seed64 [digits per node = 64410000000] [nodes = 10] [ranks = 0,<nodes/2>] [cores for the projection = 192] [samples = 20000]
 *
 * Compares, on the same term ranges, ecalc's leaf (binsplit.c span(): P = 1, Q = b - 1; for k = b-2 .. a: bi_span_step(P, Q, k), the
 * decimal base 10^18 fused Horner step, BI_MUL1_FAST) with the a37v1 leaf (spec sections 4 and 2b): the leaf split into K sub-ranges,
 * each accumulated in base 2^64 (multiply-by-k one word at a time, P += Q), converted to base 10^19 by the schoolbook peel (repeated
 * division of the whole number by 10^19, a Barrett divmod128 per word), then combined by a schoolbook binary tree in base 10^19,
 * P = P_R + Q_R P_L, Q = Q_R Q_L (R the higher sub-range).  K = 4, 8, 16, 32 (a37v1 uses 32 on 352-term leaves).
 *
 * Leaf size and term range are ecalc's own at the given share: N = e_terms(digits per node * nodes) (binsplit.c's bisection), rank r holds
 * [1 + N r / nodes, 1 + N (r + 1) / nodes) (ecalc.c:488), the span S = bs_seed_terms_for(b1) with BS_SEED_FILL = 128 (binsplit.c).
 * Correctness: for 400 leaves per rank (the first 50, the last, and evenly spaced) the P and Q of every scheme are written out as decimal
 * strings and compared with ecalc's -- exit 2 on any difference.  Timing: single thread over `samples` evenly spaced leaves (after a
 * warm-up), then all cores over min(nspan, 4000 x threads) of them; ns per leaf, and the projected seed seconds = nspan * ns_per_leaf_1t / cores
 * (assumes linear scaling; the measured all-core wall is printed beside it).  A phase breakdown of the K = 32 scheme follows.
 * Nothing here is linked into ecalc: built on demand (make tests/b_seed64), bigint.o and fatal.o only. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <omp.h>
#include "../bigint.h"

typedef unsigned __int128 u128;
#define D19 10000000000000000000ULL
static uint64_t MU_LO;                                   /* floor(2^128 / 10^19) = 2^64 + MU_LO */
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }

/* ---- binsplit.c's e_terms and bs_seed_terms_for (BS_SEED_FILL = 128, decimal), copied ---- */
static unsigned long e_terms(unsigned long d)
{
    double target = (double)d + 50.0, l10 = log(10.0);
    unsigned long lo = 1, hi = 2;
    while (lgamma((double)hi + 1.0) / l10 < target) hi *= 2;
    while (hi - lo > 1) { unsigned long mid = lo + (hi - lo) / 2; if (lgamma((double)mid + 1.0) / l10 < target) lo = mid; else hi = mid; }
    return lgamma((double)lo + 1.0) / l10 >= target ? lo : hi;
}
static unsigned long seed_terms_for(unsigned long bend, long fill)
{
    long double dpl = 18.0L, l10 = logl(10.0L), lb = lgammal((long double)bend);
#define SEED_FILL_LIMBS(S) ((long)floorl((lb - lgammal((long double)(bend - (S)))) / l10 / dpl) + 1)
    unsigned long S = (unsigned long)(fill * dpl / log10l((long double)bend)); if (S < 1) S = 1; if (S > bend - 2) S = bend - 2;
    while (S > 1 && SEED_FILL_LIMBS(S) > fill) S--;
    while (S + 2 < bend && SEED_FILL_LIMBS(S + 1) <= fill) S++;
#undef SEED_FILL_LIMBS
    return S;
}

/* ---- ecalc's leaf (binsplit.c span(), the BI_MUL1_FAST branch) ---- */
static void leaf_dec18(bigint *P, bigint *Q, unsigned long a, unsigned long b)
{
    bi_set_u64(P, 1); bi_set_u64(Q, b - 1);
    for (unsigned long k = b - 1; k-- > a;) bi_span_step(P, Q, k);
}

/* ---- a37v1's leaf ---- */
/* q = floor((hi 2^64 + lo) / 10^19) for hi < 10^19, *rem the remainder: Barrett with mu = 2^64 + MU_LO; the estimate never exceeds the
 * quotient, the fix-up loop runs a few times (the spec's 2-shift form: <= 2; this form: <= 4) */
static inline uint64_t dm19(uint64_t hi, uint64_t lo, uint64_t *rem)
{
    u128 v = ((u128)hi << 64) | lo;
    uint64_t q = hi + (uint64_t)(((u128)hi * MU_LO) >> 64);
    u128 r = v - (u128)q * D19;
    while (r >= D19) { r -= D19; q++; }
    *rem = (uint64_t)r; return q;
}
/* [a, b) in base 2^64: P = 1, Q = b - 1; for k = b-2 .. a: P += Q, Q *= k.  Returns the word counts. */
static void span64(uint64_t *p, int *pn_, uint64_t *q, int *qn_, unsigned long a, unsigned long b)
{
    int pn = 1, qn = 1; p[0] = 1; q[0] = b - 1;
    for (unsigned long k = b - 1; k-- > a;) {
        uint64_t c = 0; int i;
        for (i = 0; i < qn; i++) { uint64_t x = i < pn ? p[i] : 0, y = q[i], s = x + y, c1 = s < x, s2 = s + c, c2 = s2 < s; p[i] = s2; c = c1 | c2; }
        for (; i < pn && c; i++) { p[i] += c; c = p[i] == 0; }
        pn = pn > qn ? pn : qn; if (c) p[pn++] = 1;
        uint64_t m = 0;
        for (i = 0; i < qn; i++) { u128 t = (u128)q[i] * k + m; q[i] = (uint64_t)t; m = (uint64_t)(t >> 64); }
        if (m) q[qn++] = m;
    }
    *pn_ = pn; *qn_ = qn;
}
/* base 2^64 (n words, destroyed) -> base 10^19, least significant limb first; returns the limb count */
static int b64_to_bd(uint64_t *x, int n, uint64_t *out)
{
    int m = 0;
    while (n > 0 && x[n - 1] == 0) n--;
    while (n > 0) {
        uint64_t rem = 0;
        for (int i = n - 1; i >= 0; i--) x[i] = dm19(rem, x[i], &rem);
        out[m++] = rem;
        while (n > 0 && x[n - 1] == 0) n--;
    }
    return m;
}
/* r = a b in base 10^19, column by column (a 192-bit accumulator, reduced by two divmods); returns the trimmed length (r has na + nb limbs) */
static int mul19(uint64_t *r, const uint64_t *a, int na, const uint64_t *b, int nb)
{
    int n = na + nb; u128 carry = 0;
    for (int k = 0; k < n; k++) {
        int i0 = k >= nb ? k - nb + 1 : 0, i1 = k < na ? k : na - 1;
        uint64_t c0 = (uint64_t)carry, c1 = (uint64_t)(carry >> 64), c2 = 0;
        for (int i = i0; i <= i1; i++) {
            u128 p = (u128)a[i] * b[k - i];
            u128 s = (u128)c0 + (uint64_t)p; c0 = (uint64_t)s;
            s = (s >> 64) + c1 + (uint64_t)(p >> 64); c1 = (uint64_t)s;
            c2 += (uint64_t)(s >> 64);
        }
        uint64_t r1, r0, q1 = dm19(c2, c1, &r1), q0 = dm19(r1, c0, &r0);
        r[k] = r0; carry = ((u128)q1 << 64) | q0;
    }
    while (n > 0 && r[n - 1] == 0) n--;
    return n;
}
/* r += a in base 10^19 (a no longer than r's used length n; r has room for one more); returns the new length */
static int add19(uint64_t *r, int n, const uint64_t *a, int na)
{
    uint64_t c = 0; int i;
    for (i = 0; i < na; i++) { u128 s = (u128)(i < n ? r[i] : 0) + a[i] + c; c = s >= D19; r[i] = (uint64_t)(c ? s - D19 : s); }   /* (two 10^19 limbs overflow 64 bits) */
    for (; c; i++) { u128 s = (u128)(i < n ? r[i] : 0) + c; c = s >= D19; r[i] = (uint64_t)(c ? s - D19 : s); }
    return i > n ? i : n;
}
struct tws { uint64_t *w0, *w1, *w2, *bump; size_t bump_cap; double t_acc, t_conv, t_comb; };   /* w0, w1: the base-2^64 P, Q; w2: scratch for the conversion */
static void tws_init(struct tws *t) { memset(t, 0, sizeof *t); size_t W = 4096; t->w0 = malloc(W * 8); t->w1 = malloc(W * 8); t->w2 = malloc(W * 8); t->bump_cap = 1 << 16; t->bump = malloc(t->bump_cap * 8); }
static void tws_free(struct tws *t) { free(t->w0); free(t->w1); free(t->w2); free(t->bump); }

/* the leaf [a, b) with K sub-ranges; the result limbs (base 10^19) are in t->bump; *Pp, *Qp, *Pn, *Qn say where.  timed != 0: phase clocks */
static void leaf_b64(struct tws *t, unsigned long a, unsigned long b, int K, uint64_t **Pp, int *Pn, uint64_t **Qp, int *Qn, int timed)
{
    uint64_t *pp[64], *qp[64]; int pn[64], qn[64];
    if ((unsigned long)K > b - a) K = (int)(b - a);
    size_t bump = 0; double t0 = 0, t1 = 0;
    for (int j = 0; j < K; j++) {
        unsigned long aj = a + (b - a) * j / K, bj = a + (b - a) * (j + 1) / K;
        int wp, wq;
        if (timed) t0 = now();
        span64(t->w0, &wp, t->w1, &wq, aj, bj);
        if (timed) { t1 = now(); t->t_acc += t1 - t0; }
        pp[j] = t->bump + bump; bump += (size_t)wp * 64 / 63 + 4;      /* 10^19 limbs hold 63.1 bits */
        qp[j] = t->bump + bump; bump += (size_t)wq * 64 / 63 + 4;
        pn[j] = b64_to_bd(t->w0, wp, pp[j]);
        qn[j] = b64_to_bd(t->w1, wq, qp[j]);
        if (timed) t->t_conv += now() - t1;
    }
    if (timed) t0 = now();
    int m = K;
    while (m > 1) {
        int h = m / 2;
        for (int i = 0; i < h; i++) {
            int L = 2 * i, R = 2 * i + 1;
            uint64_t *oq = t->bump + bump; int nq = mul19(oq, qp[R], qn[R], qp[L], qn[L]); bump += (size_t)qn[R] + qn[L] + 2;
            uint64_t *op = t->bump + bump; int np_ = mul19(op, qp[R], qn[R], pp[L], pn[L]); bump += (size_t)qn[R] + pn[L] + 2;
            op[qn[R] + pn[L]] = 0; op[qn[R] + pn[L] + 1] = 0;
            np_ = add19(op, np_, pp[R], pn[R]);
            pp[i] = op; pn[i] = np_; qp[i] = oq; qn[i] = nq;
        }
        if (m & 1) { pp[h] = pp[m - 1]; pn[h] = pn[m - 1]; qp[h] = qp[m - 1]; qn[h] = qn[m - 1]; h++; }
        m = h;
    }
    if (bump > t->bump_cap) { fprintf(stderr, "b_seed64: workspace overflow (%zu limbs)\n", bump); exit(3); }
    if (timed) t->t_comb += now() - t0;
    *Pp = pp[0]; *Pn = pn[0]; *Qp = qp[0]; *Qn = qn[0];
}

/* ---- comparison: decimal strings of a limb array (digits per limb: 18 or 19) ---- */
static char *to_str(const uint64_t *l, int n, int dpl)
{
    while (n > 0 && l[n - 1] == 0) n--;
    char *s = malloc((size_t)n * dpl + 2), *o = s;
    if (!n) { strcpy(s, "0"); return s; }
    o += sprintf(o, "%llu", (unsigned long long)l[n - 1]);
    for (int i = n - 2; i >= 0; i--) { uint64_t v = l[i]; for (int d = dpl - 1; d >= 0; d--) { o[d] = (char)('0' + v % 10); v /= 10; } o += dpl; }
    *o = 0; return s;
}

static volatile uint64_t g_sink;

static unsigned long leaf_a(unsigned long a0, unsigned long S, unsigned long nspan, unsigned long j, unsigned long nsamp, unsigned long *b, unsigned long bend)
{
    unsigned long i = (unsigned long)(((u128)j * nspan) / nsamp), a = a0 + i * S; *b = a + S > bend ? bend : a + S; return a;
}

int main(int argc, char **argv)
{
    unsigned long dpn = argc > 1 ? strtoul(argv[1], 0, 10) : 64410000000UL;
    int nodes = argc > 2 ? atoi(argv[2]) : 10;
    int ranks[16], nr = 0;
    if (argc > 3) { char *s = strdup(argv[3]), *tok = strtok(s, ","); while (tok && nr < 16) { ranks[nr++] = atoi(tok); tok = strtok(0, ","); } }
    else { ranks[nr++] = 0; ranks[nr++] = nodes / 2; }
    int cores = argc > 4 ? atoi(argv[4]) : 192;
    unsigned long nsamp1 = argc > 5 ? strtoul(argv[5], 0, 10) : 20000;
    setvbuf(stdout, 0, _IOLBF, 0); MU_LO = (uint64_t)(~(u128)0 / D19);
    bi_set_decimal(1);
    { char model[160] = "?"; FILE *f = fopen("/proc/cpuinfo", "r"); char ln[256]; while (f && fgets(ln, sizeof ln, f)) if (!strncmp(ln, "model name", 10)) { char *c = strchr(ln, ':'); if (c) { strncpy(model, c + 2, sizeof model - 1); model[strcspn(model, "\n")] = 0; } break; } if (f) fclose(f);
      printf("b_seed64: cpu %s; omp threads %d; BI_MUL1_FAST %d; digits per node %lu, %d nodes\n", model, omp_get_max_threads(), bi_mul1_fast_on(), dpn, nodes); }
    unsigned long N = e_terms(dpn * (unsigned long)nodes);
    printf("b_seed64: N = %lu terms\n", N);
    static const int Ks[] = { 4, 8, 16, 32 }; const int nK = 4;
    int bad = 0;
    for (int ri = 0; ri < nr; ri++) {
        int rk = ranks[ri];
        unsigned long a0 = 1 + (unsigned long)((u128)N * rk / nodes), b1 = 1 + (unsigned long)((u128)N * (rk + 1) / nodes);
        unsigned long S = seed_terms_for(b1, 128), nspan = (b1 - a0 + S - 1) / S;
        printf("R2 nodes=%d rank=%d terms=[%lu,%lu) S=%lu nspan=%lu (log2 b1 %.1f)\n", nodes, rk, a0, b1, S, nspan, log2((double)b1));
        /* ---- correctness ---- */
        struct tws w; tws_init(&w); bigint P, Q; bi_init(&P); bi_init(&Q);
        unsigned long ncheck = 400 < nspan ? 400 : nspan, nchk = 0;
        for (unsigned long j = 0; j < ncheck + 51; j++) {
            unsigned long i = j < 50 ? (j < nspan ? j : nspan - 1) : j == 50 ? nspan - 1 : (unsigned long)(((u128)(j - 51) * nspan) / ncheck);
            unsigned long a = a0 + i * S, b = a + S > b1 ? b1 : a + S;
            leaf_dec18(&P, &Q, a, b);
            char *sp = to_str(P.l, (int)P.n, 18), *sq = to_str(Q.l, (int)Q.n, 18);
            for (int ki = 0; ki < nK; ki++) {
                uint64_t *Pp, *Qp; int Pn, Qn; leaf_b64(&w, a, b, Ks[ki], &Pp, &Pn, &Qp, &Qn, 0);
                char *tp = to_str(Pp, Pn, 19), *tq = to_str(Qp, Qn, 19);
                if (strcmp(sp, tp) || strcmp(sq, tq)) { bad++; if (bad < 5) printf("  MISMATCH rank %d leaf [%lu,%lu) K=%d\n", rk, a, b, Ks[ki]); }
                free(tp); free(tq);
            }
            free(sp); free(sq); nchk++;
        }
        printf("R2 nodes=%d rank=%d correctness: %lu leaves x %d schemes compared (P and Q as decimal strings): %s\n", nodes, rk, nchk, nK, bad ? "MISMATCH" : "identical");
        /* ---- single thread ---- */
        unsigned long ns1 = nsamp1 < nspan ? nsamp1 : nspan;
        double ns_leaf[8], ns_mt[8], tot[8];
        for (int sc = 0; sc <= nK; sc++) {
            unsigned long b;
            for (unsigned long j = 0; j < 1000 && j < ns1; j++) { unsigned long a = leaf_a(a0, S, nspan, j, ns1, &b, b1); if (!sc) leaf_dec18(&P, &Q, a, b); else { uint64_t *Pp, *Qp; int Pn, Qn; leaf_b64(&w, a, b, Ks[sc - 1], &Pp, &Pn, &Qp, &Qn, 0); } }
            double t0 = now(); uint64_t sink = 0;
            for (unsigned long j = 0; j < ns1; j++) {
                unsigned long a = leaf_a(a0, S, nspan, j, ns1, &b, b1);
                if (!sc) { leaf_dec18(&P, &Q, a, b); sink += P.l[0] + Q.l[0]; }
                else { uint64_t *Pp, *Qp; int Pn, Qn; leaf_b64(&w, a, b, Ks[sc - 1], &Pp, &Pn, &Qp, &Qn, 0); sink += Pp[0] + Qp[0]; }
            }
            double dt = now() - t0; g_sink += sink;
            ns_leaf[sc] = dt / ns1 * 1e9;
        }
        /* ---- all cores ---- */
        int T = omp_get_max_threads(); unsigned long nsm = nspan < 4000UL * T ? nspan : 4000UL * T;   /* about 0.25 s per scheme at 64 us a leaf */
        for (int sc = 0; sc <= nK; sc++) {
            double t0 = now(); uint64_t sink = 0;
#pragma omp parallel reduction(+:sink)
            {
                struct tws tw; tws_init(&tw); bigint p, q; bi_init(&p); bi_init(&q);
#pragma omp for schedule(dynamic, 16)
                for (unsigned long j = 0; j < nsm; j++) {
                    unsigned long b, a = leaf_a(a0, S, nspan, j, nsm, &b, b1);
                    if (!sc) { leaf_dec18(&p, &q, a, b); sink += p.l[0] + q.l[0]; }
                    else { uint64_t *Pp, *Qp; int Pn, Qn; leaf_b64(&tw, a, b, Ks[sc - 1], &Pp, &Pn, &Qp, &Qn, 0); sink += Pp[0] + Qp[0]; }
                }
                bi_free(&p); bi_free(&q); tws_free(&tw);
            }
            double dt = now() - t0; g_sink += sink;
            ns_mt[sc] = dt / nsm * 1e9 * T;                       /* thread-ns per leaf = wall x threads / leaves */
            tot[sc] = ns_leaf[sc] * 1e-9 * (double)nspan / cores;
            printf("R2 nodes=%d rank=%d scheme=%-6s ns/leaf: 1 thread %8.1f | %d threads %8.1f (wall x threads / leaves) | projected seed at %d cores (1t ns x nspan / cores) %6.2f s | measured all-core wall for %lu leaves %.3f s -> x nspan/%lu = %.2f s\n",
                   nodes, rk, sc ? (sc == 1 ? "b64k4" : sc == 2 ? "b64k8" : sc == 3 ? "b64k16" : "b64k32") : "dec18", ns_leaf[sc], T, ns_mt[sc], cores, tot[sc], nsm, dt, nsm, dt * (double)nspan / nsm);
        }
        printf("R2 nodes=%d rank=%d ratio dec18 / b64k32 (1 thread) = %.2f\n", nodes, rk, ns_leaf[0] / ns_leaf[nK]);
        /* ---- phase breakdown of K = 32 ---- */
        { w.t_acc = w.t_conv = w.t_comb = 0; unsigned long nb = ns1 < 3000 ? ns1 : 3000; double t0 = now();
          for (unsigned long j = 0; j < nb; j++) { unsigned long b, a = leaf_a(a0, S, nspan, j, nb, &b, b1); uint64_t *Pp, *Qp; int Pn, Qn; leaf_b64(&w, a, b, 32, &Pp, &Pn, &Qp, &Qn, 1); g_sink += Pp[0]; }
          double dt = now() - t0;
          printf("R2 nodes=%d rank=%d b64k32 phases (clocked, %lu leaves, %.1f ns/leaf with the clocks): base-2^64 accumulate %.1f%% convert %.1f%% combine %.1f%% (other %.1f%%)\n", nodes, rk, nb, dt / nb * 1e9,
                 100 * w.t_acc / dt, 100 * w.t_conv / dt, 100 * w.t_comb / dt, 100 * (dt - w.t_acc - w.t_conv - w.t_comb) / dt); }
        bi_free(&P); bi_free(&Q); tws_free(&w);
    }
    printf("R2 done: %s\n", bad ? "MISMATCH" : "all identical");
    return bad ? 2 : 0;
}
