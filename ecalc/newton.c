/* newton.c - see newton.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "newton.h"
#include "rns_mul.h"
#include "mem.h"

typedef unsigned __int128 u128;
newton_stats newton_st;
int newton_seed_perturb = 0;

/* ---- Knuth algorithm D ----------------------------------------------------- */
void bi_divmod_school(bigint *X, bigint *R, const bigint *A, const bigint *Q)
{
    if (!Q->n) { fprintf(stderr, "divmod: Q = 0\n"); abort(); }
    if (bi_cmp(A, Q) < 0) { bi_set_zero(X); bi_copy(R, A); return; }
    if (Q->n == 1) { uint64_t r = bi_divmod_u64(X, A, Q->l[0]); bi_set_u64(R, r); return; }
    unsigned s = __builtin_clzll(Q->l[Q->n - 1]);
    size_t n = Q->n, m = A->n - n;
    uint64_t *v = (uint64_t *)malloc(n * 8), *u = (uint64_t *)malloc((A->n + 1) * 8);
    limb_shl_bits(v, Q->l, n, s);
    u[A->n] = limb_shl_bits(u, A->l, A->n, s);
    bi_reserve(X, m + 1);
    for (size_t j = m + 1; j-- > 0;) {
        u128 num = ((u128)u[j + n] << 64) | u[j + n - 1];
        u128 qhat = num / v[n - 1], rhat = num % v[n - 1];
        while (qhat >> 64 || (uint64_t)qhat * (u128)v[n - 2] > ((rhat << 64) | u[j + n - 2])) {
            qhat--; rhat += v[n - 1];
            if (rhat >> 64) break;
        }
        /* u[j..j+n] -= qhat * v */
        uint64_t borrow = 0, carry = 0;
        for (size_t i = 0; i < n; i++) {
            u128 p = (u128)(uint64_t)qhat * v[i] + carry;
            carry = (uint64_t)(p >> 64);
            uint64_t pl = (uint64_t)p, t = u[i + j] - pl - borrow;
            borrow = (u[i + j] < pl) | (u[i + j] == pl && borrow);
            u[i + j] = t;
        }
        uint64_t t = u[j + n] - carry - borrow;
        int neg = (u[j + n] < carry) | (u[j + n] == carry && borrow);
        u[j + n] = t;
        if (neg) {                       /* add back */
            qhat--;
            u128 c = 0;
            for (size_t i = 0; i < n; i++) { c += (u128)u[i + j] + v[i]; u[i + j] = (uint64_t)c; c >>= 64; }
            u[j + n] += (uint64_t)c;
        }
        X->l[j] = (uint64_t)qhat;
    }
    X->n = m + 1; bi_norm(X);
    bi_reserve(R, n);
    limb_shr_bits(R->l, u, n, s);
    R->n = n; bi_norm(R);
    free(v); free(u);
}

/* ---- reciprocal ------------------------------------------------------------ */
/* seed: j = 2 limbs of precision by schoolbook on the top 4 limbs of Q */
static void seed(bigint *r, const bigint *Q, size_t *j)
{
    size_t nq = Q->n, top = nq < 4 ? nq : 4;
    bigint qt, num, rem; bi_init(&qt); bi_init(&num); bi_init(&rem);
    bi_set_limbs(&qt, Q->l + (nq - top), top);
    if (top < nq) bi_add_u64(&qt, 1);                 /* round the divisor up so r <= true */
    /* r = 2^(64 (top + 2)) / qt  ~  2^(64 (nq + 2)) / Q */
    bi_reserve(&num, top + 3); memset(num.l, 0, (top + 3) * 8); num.l[top + 2] = 1; num.n = top + 3;
    bi_divmod_school(r, &rem, &num, &qt);
    *j = 2;
    if (newton_seed_perturb) { bi_mul_u64(r, r, newton_seed_perturb); bi_shr(r, r, 4); }
    bi_free(&qt); bi_free(&num); bi_free(&rem);
}

/* grow-only scratch: fresh buffers cost a page fault per 4 KiB on every call */
static bigint g_r, g_r2, g_qt, g_t1, g_t2, g_mu, g_t, g_xq;
void newton_free_scratch(void)
{
    bi_free(&g_r); bi_free(&g_r2); bi_free(&g_qt); bi_free(&g_t1); bi_free(&g_t2); bi_free(&g_mu); bi_free(&g_t); bi_free(&g_xq);
}

void newton_recip_seeded(bigint *mu, const bigint *Q, size_t k, const bigint *seed_r, size_t seed_j)
{
    double t0 = mem_now();
    size_t nq = Q->n, j;
    bigint r = g_r, r2 = g_r2, qt = g_qt, t1 = g_t1, t2 = g_t2;
    if (seed_r) { bi_copy(&r, seed_r); j = seed_j; } else seed(&r, Q, &j);
    while (j < k) {
        size_t jn = 2 * j < k ? 2 * j : k;
        for (;;) {
            /* r ~ 2^(64(nq+j))/Q with precision jp <= j.  u = (Q r) >> 64 (nq - j)
             * ~ 2^(128 j); d = 2^(128 j) - u (signed, ~ j+1 limbs); the Newton
             * correction is (r d) >> 64 j and r' = (r << 64 j) + corr at
             * precision 2j.  Products: Q r (nq + j + 1 limbs) and r d. */
            size_t take = 2 * j + 2 < nq ? 2 * j + 2 : nq;           /* top limbs of Q: enough for precision 2j */
            bi_set_limbs(&qt, Q->l + (nq - take), take);
            rns_mul(&t1, &qt, &r);                                  /* Q_t r ~ 2^(64 (take + j)) */
            if (j <= take) bi_shr(&t2, &t1, 64 * (take - j)); else bi_shl(&t2, &t1, 64 * (j - take));   /* u ~ 2^(128 j) */
            bigint pw; bi_init(&pw); bi_reserve(&pw, 2 * j + 1); memset(pw.l, 0, (2 * j + 1) * 8); pw.l[2 * j] = 1; pw.n = 2 * j + 1;   /* 2^(128 j) */
            int neg = bi_cmp(&t2, &pw) > 0;                         /* u > 2^(128j): r too large */
            if (neg) bi_sub(&t1, &t2, &pw); else bi_sub(&t1, &pw, &t2);   /* |d| */
            bi_free(&pw);
            rns_mul(&r2, &r, &t1);                                  /* r |d| */
            bi_shr(&t1, &r2, 64 * j);                               /* |corr| */
            int converged = t1.n <= j + 1;
            bi_shl(&t2, &r, 64 * j);                                /* r << 64 j */
            if (neg) {
                if (bi_cmp(&t2, &t1) <= 0) {                        /* would go to zero or below: overshoot, shrink */
                    newton_st.overshoots++;
                    bigint d; bi_init(&d); bi_shr(&d, &r, 4); bi_sub(&r, &r, &d); bi_free(&d);
                    continue;
                }
                bi_sub(&r2, &t2, &t1);
            } else bi_add(&r2, &t2, &t1);
            bi_shr(&r, &r2, converged ? 0 : 64 * j);
            if (!converged) { newton_st.repeats++; continue; }
            break;
        }
        if (jn < 2 * j) { bi_shr(&t2, &r, 64 * (2 * j - jn)); bigint sw = r; r = t2; t2 = sw; }
        j = jn;
        newton_st.iters++;
    }
    if (j > k) { bi_shr(&t2, &r, 64 * (j - k)); bigint sw = r; r = t2; t2 = sw; }   /* k below the seed precision */
    bi_copy(mu, &r);
    g_r = r; g_r2 = r2; g_qt = qt; g_t1 = t1; g_t2 = t2;
    newton_st.t_recip += mem_now() - t0;
}
void newton_recip(bigint *mu, const bigint *Q, size_t k) { newton_recip_seeded(mu, Q, k, 0, 0); }

/* ---- division -------------------------------------------------------------- */
void newton_divmod(bigint *X, bigint *R, const bigint *A, const bigint *Q, const bigint *mu_opt)
{
    double t0 = mem_now();
    if (!Q->n) { fprintf(stderr, "newton_divmod: Q = 0\n"); abort(); }
    if (bi_cmp(A, Q) < 0) { bi_set_zero(X); bi_copy(R, A); return; }
    size_t nq = Q->n, na = A->n, k = na - nq + 1;
    if (na + nq < (size_t)rns_school_max * 4) { bi_divmod_school(X, R, A, Q); newton_st.t_div += mem_now() - t0; return; }
    bigint mu = g_mu, t = g_t, xq = g_xq;
    if (mu_opt && mu_opt->n >= k + 1) { bi_shr(&mu, mu_opt, 64 * (mu_opt->n - (k + 1))); }   /* a longer mu: use its top */
    else newton_recip(&mu, Q, k);
    /* X = (A mu) >> 64 (nq + k) = (A mu) >> 64 (na + 1).  The low nq - 1 limbs
     * of A contribute less than 2^(64 (na + 1)) to the product, i.e. under one
     * unit of X: use only Ah = A >> 64 (nq - 1) (k limbs) and shift by k + 1.
     * The corrections below absorb the +-1. */
    {
        bigint Ah = { A->l + (nq - 1), A->n - (nq - 1), 0 };
        rns_mul(&t, &Ah, &mu);
        bi_shr(X, &t, 64 * (k + 1));
    }
    if (t.cap > ((size_t)1 << 28)) { bi_free(&t); }      /* a huge product (the dm phase): give it back before X Q */
    /* R = A - X Q over a window of w = nq + 2 limbs (R is within a few Q of 0):
     * r = (A - low(X Q)) mod 2^(64 w); the top limb's sign says X is too large */
    size_t w = nq + 2;
    rns_mul_low(&xq, X->l, X->n, Q->l, Q->n, w);
    bi_reserve(R, w + 1);
    {
        size_t an = A->n < w ? A->n : w;
        for (size_t i = xq.n; i < w; i++) xq.l[i] = 0;       /* xq has cap >= w + 1 from rns_mul_low */
        limb_sub(R->l, A->l, an, xq.l, an);                   /* mod 2^(64 an); an == w unless A is short */
        for (size_t i = an; i < w; i++) R->l[i] = 0;
        if (an < w) { /* A shorter than the window: the subtraction is exact and non-negative (A >= X Q) */ }
        else {
            /* limbs of xq above an are impossible (xq < 2^(64 w)) */
        }
    }
    size_t nc = 0;
    for (;;) {
        if (R->l[w - 1] >> 63) {                             /* negative: X too large */
            bigint one; bi_init(&one); bi_set_u64(&one, 1); bi_sub(X, X, &one); bi_free(&one);
            uint64_t c = limb_add(R->l, R->l, w, Q->l, Q->n); (void)c;
            newton_st.down_corr++;
        } else {
            R->n = w; bi_norm(R);
            if (bi_cmp(R, Q) < 0) break;
            limb_sub(R->l, R->l, w, Q->l, Q->n);
            bi_add_u64(X, 1);
            newton_st.up_corr++;
        }
        if (++nc > 64) { fprintf(stderr, "newton_divmod: %zu corrections, mu is wrong\n", nc); abort(); }
    }
    R->n = w; bi_norm(R);
    g_mu = mu; g_t = t; g_xq = xq;
    if (xq.cap > ((size_t)1 << 28)) { bi_free(&g_xq); bi_free(&g_mu); }
    newton_st.t_div += mem_now() - t0;
}
