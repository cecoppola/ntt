/* t_verify - step 8 test: tier-1 catches a single-bit fault; passes on GMP-produced values.
 *
 *  1. for d = 10^5: P, Q by GMP (the reference recursion), X, R by mpz_tdiv_qr
 *     of 10^d (P+Q) by Q -> tier1 passes; digits by mpz_get_str -> tier1_digits
 *     and tier2 pass
 *  2. one bit flipped in X (low, middle, top limb), in R, in the digits ->
 *     every prime reports a failure
 *  3. vf_limbs_mod and vf_digits_mod vs mpz_fdiv_ui on random values
 */
#include "harness.h"
#include "../verify.h"
#include "../binsplit.h"

int main(void)
{
    rng_t rng = {0x5EC0ULL};
    printf("== t_verify ==\n");
    harness_meta("t_verify");
    mpz_t p, q, x, r, t; mpz_inits(p, q, x, r, t, NULL);
    unsigned long d = 100000, N = e_terms(d);
    bigint P, Q, X, R; bi_init(&P); bi_init(&Q); bi_init(&X); bi_init(&R);
    binsplit_ref(&P, &Q, 1, N + 1);
    bi_to_mpz(p, &P); bi_to_mpz(q, &Q);
    mpz_add(t, p, q); mpz_ui_pow_ui(x, 10, d); mpz_mul(t, t, x);
    mpz_tdiv_qr(x, r, t, q);
    bi_from_mpz(&X, x); bi_from_mpz(&R, r);
    char *digits = mpz_get_str(NULL, 10, x);
    VERIFY(strlen(digits) == d + 1, "digits length");
    printf("-- 1. genuine values\n");
    VERIFY(tier1(N, d, &P, &Q, &X, &R, 1) == 0, "tier1 on GMP values");
    VERIFY(tier1_digits(digits, d + 1, &X, 1) == 0, "tier1_digits on GMP values");
    VERIFY(tier2(digits, d + 1, 1) == 0, "tier2 on GMP digits");

    printf("-- 2. injected faults\n");
    size_t pos[3] = {0, X.n / 2, X.n - 1};
    for (int i = 0; i < 3; i++) {
        X.l[pos[i]] ^= 1ULL << (rng_next(&rng) & 63);
        int bad = tier1(N, d, &P, &Q, &X, &R, 0);
        VERIFY(bad == T1_NQ, "bit flip in X limb %zu: %d of %d primes caught it", pos[i], bad, T1_NQ);
        bad = tier1_digits(digits, d + 1, &X, 0);
        VERIFY(bad == T1_NQ, "bit flip in X limb %zu vs digits: %d caught", pos[i], bad);
        bi_from_mpz(&X, x);
    }
    R.l[R.n / 3] ^= 1ULL << 17;
    VERIFY(tier1(N, d, &P, &Q, &X, &R, 0) == T1_NQ, "bit flip in R caught");
    bi_from_mpz(&R, r);
    digits[d / 2] = digits[d / 2] == '7' ? '3' : '7';
    VERIFY(tier1_digits(digits, d + 1, &X, 0) == T1_NQ, "changed digit caught");
    digits[d / 2] = digits[d / 2] == '3' ? '7' : '3';
    { char *dd = strdup(digits); dd[52] ^= 1; VERIFY(tier2(dd, d + 1, 0) == 1, "tier2 window@50 catches a changed digit"); free(dd); }
    VERIFY(tier1(N, d, &P, &Q, &X, &R, 0) == 0, "restored values pass again");

    printf("-- 3. residues vs GMP\n");
    for (int it = 0; it < 20; it++) {
        bigint a; bi_init(&a); bi_random(&a, 1 + rng_next(&rng) % 100000, it % GEN_KINDS, &rng);
        bi_to_mpz(t, &a);
        for (int i = 0; i < T1_NQ; i++) VERIFY(vf_limbs_mod(a.l, a.n, t1_q[i]) == mpz_fdiv_ui(t, t1_q[i]), "limbs mod q%d", i);
        char *s = mpz_get_str(NULL, 10, t);
        for (int i = 0; i < T1_NQ; i++) VERIFY(vf_digits_mod(s, strlen(s), t1_q[i]) == mpz_fdiv_ui(t, t1_q[i]), "digits mod q%d", i);
        free(s); bi_free(&a);
    }
    free(digits);
    return verify_done("t_verify");
}
