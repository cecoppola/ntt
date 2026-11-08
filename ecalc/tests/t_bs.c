/* t_bs - step 6 test: binsplit against the GMP recursion and the reference digits.
 *
 *  1. P, Q for N = 10^3 .. 10^6 (10^7 with "long") vs binsplit_ref with the
 *     seed span lowered (8 terms) and at 512 terms (the CPU schoolbook tier)
 *  2. e to 10^6 and 10^7 digits end to end through GMP division and
 *     mpz_get_str, compared with ref/e_<d>.txt (SHA-256 of the whole string)
 *
 * Usage: t_bs [long]      t_bs verify: tier-1 catches a single-bit fault, passes on GMP-produced values (verify.c; no GPU).
 *   1. genuine values (P, Q by GMP, X, R by mpz_tdiv_qr, digits by mpz_get_str) pass tier1, tier1_digits, tier2; 2. injected bit flips are caught;
 *   3. vf_limbs_mod / vf_digits_mod vs mpz_fdiv_ui; 4. vf_pq_range_mod vs the leaf P, Q of the same term range.
 */
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "sha256.h"
#include "vpe.h"
#include "binsplit.h"
#include "../binsplit.h"
#include "../verify.h"
#include "../rns_mul.h"
#include "../mem.h"

static int check_pq(unsigned long N, int seed)
{
    bigint P, Q, Pr, Qr; bi_init(&P); bi_init(&Q); bi_init(&Pr); bi_init(&Qr);
    bs_seed_terms = seed;
    binsplit_e(&P, &Q, N);
    mpz_t p, q; mpz_inits(p, q, NULL);
    double t0 = now();
    binsplit_ref(&Pr, &Qr, 1, N + 1);
    double tr = now() - t0;
    int ok = bi_cmp(&P, &Pr) == 0 && bi_cmp(&Q, &Qr) == 0;
    VERIFY(ok, "N = %lu seed %d: P/Q differ (P %zu vs %zu limbs, Q %zu vs %zu)", N, seed, P.n, Pr.n, Q.n, Qr.n);
    printf("   N %-9lu seed %-4d: %.2f s (levels %d: batch %d mdev %d; seeds %.2f) ref %.2f s  Q %zu limbs\n",
           N, seed, bs_st.t_total, bs_st.levels, bs_st.batch_levels, bs_st.mdev_levels, bs_st.t_seed, tr, Q.n);
    mpz_clears(p, q, NULL); bi_free(&P); bi_free(&Q); bi_free(&Pr); bi_free(&Qr);
    return ok;
}

/* t_bs verify: the tier-1 / tier-2 result checks (verify.c) */
static int run_verify(void)
{
    rng_t rng = {0x5EC0ULL};
    printf("== t_bs verify ==\n");
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
    printf("-- 4. vf_pq_range_mod vs the leaf P, Q of the same term range (the C3 leaf check compares it with db_mod_qs of the leaf; db_mod_qs vs host residues: t_dbig)\n");
    {   unsigned long rg[][2] = { {1, 2}, {1, 3}, {2, 3}, {1, 100}, {7, 8}, {100, 101}, {5, 1000}, {N, N + 1}, {N - 50, N + 1}, {N / 2, N / 2 + 333}, {1, N + 1} };
        for (size_t k = 0; k < sizeof rg / sizeof rg[0]; k++) {
            bigint Pl, Ql; bi_init(&Pl); bi_init(&Ql); binsplit_ref(&Pl, &Ql, rg[k][0], rg[k][1]);
            mpz_t a, b; mpz_inits(a, b, NULL); bi_to_mpz(a, &Pl); bi_to_mpz(b, &Ql);
            for (int i = 0; i < T1_NQ; i++) {
                uint64_t pp, qq; vf_pq_range_mod(rg[k][0], rg[k][1], t1_q[i], &pp, &qq);
                VERIFY(pp == mpz_fdiv_ui(a, t1_q[i]) && qq == mpz_fdiv_ui(b, t1_q[i]) && pp == vf_limbs_mod(Pl.l, Pl.n, t1_q[i]) && qq == vf_limbs_mod(Ql.l, Ql.n, t1_q[i]),
                       "range [%lu, %lu) mod q%d", rg[k][0], rg[k][1], i);
            }
            mpz_clears(a, b, NULL); bi_free(&Pl); bi_free(&Ql);
        }
    }
    free(digits);
    return verify_done("t_bs verify");
}
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "verify")) { harness_meta("t_bs"); return run_verify(); }
    int lng = argc > 1 && !strcmp(argv[1], "long");
    printf("== t_bs ==\n");
    harness_meta("t_bs");
    setenv("DB_POOL_VMM", "0", 0);   /* this test reads P and Q on the host; a VMM range is device-only (the VMM pool, the default, is covered by mnaccept's e9 and full steps) */
    rns_init(31);
    printf("-- 1. P, Q vs the GMP-free reference recursion\n");
    check_pq(1000, 8); check_pq(1000, 512);
    check_pq(100000, 8); check_pq(100000, 512);
    check_pq(1000000, 8); check_pq(1000000, 512);
    if (lng) check_pq(10000000, 32);

    printf("-- 2. e end to end vs ref/\n");
    static const unsigned long ds[] = {1000000, 10000000};
    for (int i = 0; i < 2; i++) {
        unsigned long d = ds[i], N = e_terms(d);
        bigint P, Q; bi_init(&P); bi_init(&Q);
        bs_seed_terms = 512; bs_verbose = 1;
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
    printf("-- 3. planner host terms per process (S2c): ECALC_PE_PER_GPU=1 adds a runtime (7 GB) and a comm (6 GB) per further process of the node; off unchanged\n");
    {
        setenv("ECALC_VPE_LANES", "4", 1); const char *sv = getenv("SLURM_NTASKS_PER_NODE"); char *svs = sv ? strdup(sv) : 0; setenv("SLURM_NTASKS_PER_NODE", "4", 1);
        unsigned long Np = e_terms(100000000000UL), N1 = e_terms(10000000000UL);
        size_t h3off, h3v, h3p, h1off, h1v, h1p, h3off2;
        binsplit_node_bytes(Np, 3, 1, 3, 0, 0, &h3off); binsplit_node_bytes(N1, 1, 1, 3, 0, 0, &h1off);
        vpe_set_forced(1); binsplit_node_bytes(Np, 3, 1, 3, 0, 0, &h3v); binsplit_node_bytes(N1, 1, 1, 3, 0, 0, &h1v); vpe_set_forced(0);
        vpe_set_ppg(0); binsplit_node_bytes(Np, 3, 1, 3, 0, 0, &h3p); binsplit_node_bytes(N1, 1, 1, 3, 0, 0, &h1p);
        size_t stg2 = rns_batch_split_mode() == 1 ? (size_t)4 << 30 : 0; vpe_set_ppg(-1);   /* Q110: RNS_BATCH_SPLIT automatic in process mode raises the pinned staging from 1 to 2 GiB per PE */
        binsplit_node_bytes(Np, 3, 1, 3, 0, 0, &h3off2);
        VERIFY(h3p == h3v + 3 * (size_t)13000000000 + stg2, "host g 3: process mode %zu, one shared process %zu: want + 3 x 13 GB + %zu staging", h3p, h3v, stg2);
        VERIFY(h1p == h1v + 3 * (size_t)7000000000, "host g 1: process mode %zu, one shared process %zu: want + 3 x 7 GB", h1p, h1v);
        VERIFY(h3off2 == h3off && h1off > 0, "host with the switch off unchanged after the process-mode calls: %zu vs %zu", h3off2, h3off);
        printf("   host per node, g 3 at 1e11: off %.1f, one shared process %.1f, processes %.1f GB; g 1 at 1e10: off %.1f, one shared process %.1f, processes %.1f GB\n", h3off / 1e9, h3v / 1e9, h3p / 1e9, h1off / 1e9, h1v / 1e9, h1p / 1e9);
        if (svs) { setenv("SLURM_NTASKS_PER_NODE", svs, 1); free(svs); } else unsetenv("SLURM_NTASKS_PER_NODE");
    }
    printf("VmHWM %.1f GB\n", mem_vmhwm() / 1e9);
    rns_shutdown();
    return verify_done("t_bs");
}
