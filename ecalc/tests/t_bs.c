/* t_bs - step 6 test: binsplit against the GMP recursion and the reference digits.
 *
 *  1. P, Q for N = 10^3 .. 10^6 (10^7 with "long") vs binsplit_ref with the
 *     tier thresholds lowered (seed 8 terms, school 4 limbs) so every tier
 *     runs, and at the defaults
 *  2. e to 10^6 and 10^7 digits end to end through GMP division and
 *     mpz_get_str, compared with ref/e_<d>.txt (SHA-256 of the whole string)
 *
 * Usage: t_bs [long]
 */
#include "harness.h"
#include "sha256.h"
#include "../binsplit.h"
#include "../rns_mul.h"
#include "../mem.h"

static int check_pq(unsigned long N, int seed, int school)
{
    bigint P, Q, Pr, Qr; bi_init(&P); bi_init(&Q); bi_init(&Pr); bi_init(&Qr);
    bs_seed_terms = seed; bs_school_nl = school;
    binsplit_e(&P, &Q, N);
    mpz_t p, q; mpz_inits(p, q, NULL);
    double t0 = now();
    binsplit_ref(&Pr, &Qr, 1, N + 1);
    double tr = now() - t0;
    int ok = bi_cmp(&P, &Pr) == 0 && bi_cmp(&Q, &Qr) == 0;
    VERIFY(ok, "N = %lu seed %d school %d: P/Q differ (P %zu vs %zu limbs, Q %zu vs %zu)", N, seed, school, P.n, Pr.n, Q.n, Qr.n);
    printf("   N %-9lu seed %-4d school %-4d: %.2f s (levels %d: school %d batch %d mdev %d; seeds %.2f) ref %.2f s  Q %zu limbs\n",
           N, seed, school, bs_st.t_total, bs_st.levels, bs_st.school_levels, bs_st.batch_levels, bs_st.mdev_levels, bs_st.t_seed, tr, Q.n);
    mpz_clears(p, q, NULL); bi_free(&P); bi_free(&Q); bi_free(&Pr); bi_free(&Qr);
    return ok;
}

int main(int argc, char **argv)
{
    int lng = argc > 1 && !strcmp(argv[1], "long");
    printf("== t_bs ==\n");
    harness_meta("t_bs");
    rns_init(31);
    printf("-- 1. P, Q vs the GMP-free reference recursion\n");
    check_pq(1000, 8, 4); check_pq(1000, 512, 160);
    check_pq(100000, 8, 4); check_pq(100000, 512, 160);
    check_pq(1000000, 8, 4); check_pq(1000000, 512, 160);
    if (lng) check_pq(10000000, 32, 16);

    printf("-- 2. e end to end vs ref/\n");
    static const unsigned long ds[] = {1000000, 10000000};
    for (int i = 0; i < 2; i++) {
        unsigned long d = ds[i], N = e_terms(d);
        bigint P, Q; bi_init(&P); bi_init(&Q);
        bs_seed_terms = 512; bs_school_nl = 160; bs_verbose = 1;
        binsplit_e(&P, &Q, N);
        bs_verbose = 0;
        mpz_t p, q, x; mpz_inits(p, q, x, NULL);
        bi_to_mpz(p, &P); bi_to_mpz(q, &Q);
        mpz_add(p, p, q); mpz_ui_pow_ui(x, 10, d); mpz_mul(x, x, p); mpz_tdiv_q(x, x, q);
        char *s = mpz_get_str(NULL, 10, x);
        char hex[65]; sha256_hex(s + 1, d, hex);
        char path[256]; snprintf(path, sizeof path, "ref/e_%lu.sha256", d);
        FILE *f = fopen(path, "r"); char line[128], want[65] = "";
        if (f) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "all ", 4)) { sscanf(line + 4, "%64s", want); } fclose(f); }
        VERIFY(strlen(s) == d + 1 && s[0] == '2', "e %lu: length %zu", d, strlen(s));
        VERIFY(!strcmp(hex, want), "e %lu: SHA-256 %s vs ref %s", d, hex, want);
        printf("   e to %lu digits: N %lu, bs %.2f s, sha256 %s %s\n", d, N, bs_st.t_total, hex, strcmp(hex, want) ? "MISMATCH" : "== ref");
        free(s); mpz_clears(p, q, x, NULL); bi_free(&P); bi_free(&Q);
    }
    printf("VmHWM %.1f GB\n", mem_vmhwm() / 1e9);
    rns_shutdown();
    return verify_done("t_bs");
}
