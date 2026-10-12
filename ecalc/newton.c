/* newton.c - see newton.h: the host schoolbook division and the reciprocal seed */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include "newton.h"

typedef unsigned __int128 u128;
newton_stats newton_st;
int newton_seed_perturb = 0;

/* ---- Knuth algorithm D, base B = 10^18 (decimal limbs) ---------------------- */
static void divmod_school_dec(bigint *X, bigint *R, const bigint *A, const bigint *Q)
{
    const u128 B = BI_B10;
    size_t n = Q->n, m = A->n - n;
    uint64_t f = (uint64_t)(B / (Q->l[n - 1] + 1));          /* normalise: v_top >= B/2 */
    uint64_t *v = (uint64_t *)malloc(n * 8), *u = (uint64_t *)malloc((A->n + 1) * 8);
    limb_mul_1(v, Q->l, n, f, 0);
    u[A->n] = limb_mul_1(u, A->l, A->n, f, 0);
    bi_reserve(X, m + 1);
    for (size_t j = m + 1; j-- > 0;) {
        u128 num = (u128)u[j + n] * B + u[j + n - 1];
        u128 qhat = num / v[n - 1], rhat = num % v[n - 1];
        while (qhat >= B || qhat * v[n - 2] > rhat * B + u[j + n - 2]) { qhat--; rhat += v[n - 1]; if (rhat >= B) break; }
        /* u[j..j+n] -= qhat v */
        u128 carry = 0; uint64_t borrow = 0;
        for (size_t i = 0; i < n; i++) {
            carry += qhat * v[i];
            uint64_t pl = (uint64_t)(carry % B); carry /= B;
            uint64_t s = pl + borrow; borrow = u[i + j] < s; u[i + j] = borrow ? u[i + j] + BI_B10 - s : u[i + j] - s;
        }
        { uint64_t s = (uint64_t)carry + borrow; int neg = u[j + n] < s; u[j + n] = neg ? u[j + n] + BI_B10 - s : u[j + n] - s;
          if (neg) {                            /* add back */
              qhat--; uint64_t c = 0;
              for (size_t i = 0; i < n; i++) { uint64_t t = u[i + j] + v[i] + c; c = t >= BI_B10; u[i + j] = c ? t - BI_B10 : t; }
              u[j + n] += c; if (u[j + n] >= BI_B10) u[j + n] -= BI_B10;
          } }
        X->l[j] = (uint64_t)qhat;
    }
    X->n = m + 1; bi_norm(X);
    bigint un = { u, n, 0 }; un.n = limb_norm(u, n);
    bi_divmod_u64(R, &un, f);                                /* remainder / f */
    free(v); free(u);
}

/* ---- schoolbook division (Knuth D above) ------------------------------------ */
void bi_divmod_school(bigint *X, bigint *R, const bigint *A, const bigint *Q)
{
    if (!Q->n) { ec_fatal(EC_RC_FATAL, "divmod: Q = 0\n"); }
    if (bi_cmp(A, Q) < 0) { bi_set_zero(X); bi_copy(R, A); return; }
    if (Q->n == 1) { uint64_t r = bi_divmod_u64(X, A, Q->l[0]); bi_set_u64(R, r); return; }
    divmod_school_dec(X, R, A, Q);
}

/* ---- reciprocal ------------------------------------------------------------ */
/* seed: j = 2 limbs of precision by schoolbook on the top 4 limbs of Q */
void newton_seed_host(bigint *r, const bigint *Q, size_t *j) { size_t top = Q->n < 4 ? Q->n : 4; newton_seed_top(r, Q->l + (Q->n - top), top, Q->n, j); }
/* the seed from Q's top limbs only (Q may live on the device; top4 = its top `top` limbs, nq its length) */
void newton_seed_top(bigint *r, const uint64_t *top4, size_t top, size_t nq, size_t *j)
{
    bigint qt, num, rem; bi_init(&qt); bi_init(&num); bi_init(&rem);
    bi_set_limbs(&qt, top4, top);
    if (top < nq) bi_add_u64(&qt, 1);                 /* round the divisor up so r <= true */
    /* r = B^(top + 2) / qt  ~  B^(nq + 2) / Q */
    bi_set_base_pow(&num, top + 2);
    bi_divmod_school(r, &rem, &num, &qt);
    *j = 2;
    if (newton_seed_perturb) { bi_mul_u64(r, r, newton_seed_perturb); bi_divmod_u64(r, r, 16); }
    bi_free(&qt); bi_free(&num); bi_free(&rem);
}
