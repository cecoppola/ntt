/* t_params - check the constants the paper specifies, with GMP.
 *
 * For each of the four RNS primes: primality, p < 2^52, 2^33 | p-1, that the
 * stated g is a generator (order p-1), that w33 = g^((p-1)/2^33) has exact
 * order 2^33, and mu115 = floor(2^115 / p).  Also mu = floor(2^123 / 10^18)
 * for the LEAF kernel, and the sum of log2 p.  Also checks ~/ntt bench primes.
 *
 * Build: gcc -O2 -o t_params t_params.c -lgmp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gmp.h>

static int fails = 0;
#define CHECK(cond, ...) do { printf("  %-52s ", #cond); if (cond) printf("OK\n"); \
    else { printf("FAILED "); printf("%s", ""); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* order of g mod p is p-1 iff g^((p-1)/q) != 1 for every prime q | p-1 */
static int is_generator(mpz_t g, mpz_t p)
{
    mpz_t pm1, n, q, t, e;
    int ok = 1;
    mpz_inits(pm1, n, q, t, e, NULL);
    mpz_sub_ui(pm1, p, 1);
    mpz_set(n, pm1);
    mpz_set_ui(q, 2);
    while (mpz_cmp_ui(n, 1) > 0 && ok) {
        if (mpz_divisible_p(n, q)) {
            mpz_divexact(e, pm1, q);
            mpz_powm(t, g, e, p);
            if (mpz_cmp_ui(t, 1) == 0) ok = 0;
            while (mpz_divisible_p(n, q)) mpz_divexact(n, n, q);
        }
        if (mpz_cmp_ui(n, 1) > 0) {
            mpz_t qq; mpz_init(qq); mpz_mul(qq, q, q);
            if (mpz_cmp(qq, n) > 0) { /* n is prime */
                mpz_divexact(e, pm1, n);
                mpz_powm(t, g, e, p);
                if (mpz_cmp_ui(t, 1) == 0) ok = 0;
                mpz_set_ui(n, 1);
            }
            mpz_clear(qq);
        }
        mpz_nextprime(q, q);
    }
    mpz_clears(pm1, n, q, t, e, NULL);
    return ok;
}

static void check_prime(const char *name, unsigned long long pv, unsigned g, int need2, int need_gen)
{
    mpz_t p, pm1, gg, w, t, e, mu, two;
    unsigned long long muv;
    mpz_inits(p, pm1, gg, w, t, e, mu, two, NULL);
    mpz_set_ui(p, pv);
    mpz_sub_ui(pm1, p, 1);
    printf("%s = %llu  (g = %u)\n", name, pv, g);
    CHECK(mpz_probab_prime_p(p, 50) >= 1, "composite");
    CHECK(pv < (1ULL << 52) || need2 == 40, "log2p = %.3f", log2((double)pv));
    mpz_set_ui(two, 1); mpz_mul_2exp(two, two, need2);
    CHECK(mpz_divisible_p(pm1, two), "2^%d does not divide p-1", need2);
    mpz_set_ui(gg, g);
    if (need_gen) CHECK(is_generator(gg, p), "g is not a generator");
    mpz_divexact(e, pm1, two);
    mpz_powm(w, gg, e, p);                    /* w = g^((p-1)/2^need2) */
    mpz_powm_ui(t, w, 1, p);
    mpz_set(t, w); { int k; for (k = 0; k < need2 - 1; k++) mpz_powm_ui(t, t, 2, p); }
    CHECK(mpz_cmp_ui(t, 1) != 0, "w^(2^(k-1)) == 1: order too small");
    mpz_powm_ui(t, t, 2, p);
    CHECK(mpz_cmp_ui(t, 1) == 0, "w^(2^k) != 1");
    mpz_set_ui(mu, 1); mpz_mul_2exp(mu, mu, 115); mpz_fdiv_q(mu, mu, p);
    muv = mpz_get_ui(mu);
    printf("  w%d = %s\n  mu115 = %llu (%d bits)\n", need2, mpz_get_str(NULL, 10, w), muv, (int)mpz_sizeinbase(mu, 2));
    mpz_clears(p, pm1, gg, w, t, e, mu, two, NULL);
}

int main(void)
{
    static const unsigned long long P[4] = {3923057487904769ULL, 3641582511194113ULL,
                                            2867526325239809ULL, 2586051348529153ULL};
    static const unsigned G[4] = {3, 5, 3, 10};
    double sumlog = 0;
    int i;
    mpz_t mu, d, t;

    printf("== paper primes (p < 2^52, 2^33 | p-1) ==\n");
    for (i = 0; i < 4; i++) { check_prime("P", P[i], G[i], 33, 1); sumlog += log2((double)P[i]); }
    printf("sum log2 p = %.2f (paper: ~206)\n", sumlog);
    CHECK(sumlog > 205.0 && sumlog < 207.0, "%.2f", sumlog);
    CHECK(128 + 32 <= (int)sumlog, "convolution bound b=64, n=2^32");
    CHECK(128 + 33 <= (int)sumlog, "convolution bound b=64, n=2^33");

    printf("\n== LEAF Barrett constant ==\n");
    mpz_inits(mu, d, t, NULL);
    mpz_ui_pow_ui(d, 10, 18);
    mpz_set_ui(mu, 1); mpz_mul_2exp(mu, mu, 123); mpz_fdiv_q(mu, mu, d);
    printf("  floor(2^123 / 10^18) = %s\n", mpz_get_str(NULL, 10, mu));
    CHECK(mpz_cmp_ui(mu, 10633823966279326983ULL) == 0, "mismatch");

    printf("\n== ~/ntt bench primes (p < 2^62, 2^40*3*5*7 | p-1, root 11) ==\n");
    check_prime("p1", 0x3FFEDF0000000001ULL, 11, 40, 1);
    check_prime("p2", 0x3FF6420000000001ULL, 11, 40, 1);

    printf("\n== tier-1 verification: 8 primes near 2^62 (paper does not list them; propose) ==\n");
    mpz_set_ui(t, 1); mpz_mul_2exp(t, t, 62);
    for (i = 0; i < 8; i++) { mpz_nextprime(t, t); printf("  q%d = %s\n", i, mpz_get_str(NULL, 10, t)); }

    printf("\n%s (%d failures)\n", fails ? "VERIFY FAILED" : "VERIFY OK", fails);
    return fails != 0;
}
