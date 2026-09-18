/* t_mul - step 3 test: rns_mul against mpz_mul, every tier and boundary.
 *
 *  1. pool 2^POOL (default 20 for the first part) so the split tiers run at
 *     small sizes: products crossing 2^10 (schoolbook -> mdev), 2^POOL
 *     (mdev -> Karatsuba), unbalanced (chunked), all five generators, vs
 *     mpz_mul exactly
 *  2. pool 2^31 (the paper): sizes crossing 2^17, 2^18, 2^27, 2^28 limbs per
 *     operand vs mpz_mul (<= 2^26) or a mod-(2^61-1) residue check (above),
 *     per-phase times of the mdev multiply at 2^31 points
 *  4. batch tier: products of <= 2^logL points for logL 10 .. 18, GPU CRT (N >= 8)
 *     and CPU CRT, registered and unregistered (staged) operands, vs mpz_mul
 *  3. optional (arg "big"): 2.08e9 x 2.08e9 limbs (the paper's 10dP product,
 *     4.16e9 > 2^31 points) through the Karatsuba split (RESULTS.md 35), twice
 *
 * Usage: t_mul [POOL (20); 0 skips part 1] [big]
 */
#include "harness.h"
#include "../rns_mul.h"
#include "../mem.h"
#include <hip/hip_runtime.h>

static void check_product(const char *what, const bigint *A, const bigint *B, const bigint *C, int exact)
{
    if (exact) {
        mpz_t a, b, c; mpz_inits(a, b, c, NULL);
        bi_to_mpz(a, A); bi_to_mpz(b, B); mpz_mul(c, a, b);
        int ok = bi_eq_mpz(C, c);
        VERIFY(ok, "%s: product differs from mpz_mul (C has %zu limbs)", what, C->n);
        mpz_clears(a, b, c, NULL);
    } else {
        uint64_t ra = limbs_mod_m61(A->l, A->n), rb = limbs_mod_m61(B->l, B->n), rc = limbs_mod_m61(C->l, C->n);
        VERIFY(m61_mul(ra, rb) == rc, "%s: residue mod 2^61-1 differs", what);
        VERIFY(C->n == A->n + B->n || C->n == A->n + B->n - 1, "%s: length %zu", what, C->n);
    }
}

int main(int argc, char **argv)
{
    int pool = argc > 1 ? atoi(argv[1]) : 20;
    int big = argc > 2 && !strcmp(argv[2], "big");
    int big32 = argc > 2 && !strcmp(argv[2], "big32");     /* Q1 (ii): 2^32 pools, the 10dP product as one mdev */
    if (big32) big = 1;
    int only_batch = argc > 2 && !strcmp(argv[2], "batch");
    int only_dist = argc > 2 && !strcmp(argv[2], "dist");     /* WP5: the distributed tier against GMP, and against mdev for time */
    rng_t rng = {0xB1B0ULL};
    bigint A, B, C; bi_init(&A); bi_init(&B); bi_init(&C);
    printf("== t_mul ==\n");
    harness_meta("t_mul");

    if (only_dist) {
        rns_init(pool ? pool : 31);
        struct { size_t na, nb; } dc[] = { {600000, 500000}, {1u << 20, 1u << 20}, {(1u << 22) + 3, (1u << 22) - 5}, {1u << 25, 1u << 25}, {(1u << 27) + 11, (1u << 27) - 11} };
        for (size_t i = 0; i < sizeof dc / sizeof *dc; i++) for (int kind = 0; kind < 2; kind++) {
            bi_random(&A, dc[i].na, kind, &rng); bi_random(&B, dc[i].nb, kind, &rng);
            double t0 = now(); rns_mul_dist(&C, &A, &B); double t1 = now();
            char what[96]; snprintf(what, sizeof what, "dist %zux%zu %s", dc[i].na, dc[i].nb, gen_name[kind]);
            check_product(what, &A, &B, &C, 1);
            if (kind == 0) { bigint D; bi_init(&D); double t2 = now(); rns_mul(&D, &A, &B); double t3 = now();
                             VERIFY(bi_cmp(&C, &D) == 0, "dist == rns_mul %zux%zu", dc[i].na, dc[i].nb);
                             printf("   %-40s dist %.3f s   rns_mul %.3f s\n", what, t1 - t0, t3 - t2); bi_free(&D); }
        }
        bi_free(&A); bi_free(&B); bi_free(&C);
        return verify_done("t_mul");
    }
    /* 1. small pool: every tier */
    printf("-- 1. pool 2^%d: tiers and boundaries\n", pool);
    if (pool) {
    rns_init(pool);
        size_t P = (size_t)1 << pool;
        struct { size_t na, nb; const char *why; } cases[] = {
            {3, 5, "tiny"}, {511, 512, "school below 2^10"}, {512, 512, "exactly 2^10 -> mdev"},
            {513, 512, "just over 2^10"}, {1000, 30, "unbalanced small"},
            {P / 2, P / 2, "exactly the pool"}, {P / 2 + 1, P / 2, "one over the pool -> karatsuba"},
            {P / 2 + 7, P / 2 - 3, "over, uneven"}, {P, P, "2x pool -> karatsuba"},
            {3 * P, P / 8, "chunked 24:1"}, {P / 8, 3 * P, "chunked 1:24 (swapped)"},
            {2 * P + 5, P / 2 + 3, "ratio 4: karatsuba 2-piece"}, {5 * P, 5 * P, "10x pool, two levels"},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
            for (int kind = 0; kind < GEN_KINDS; kind++) {
                bi_random(&A, cases[i].na, kind, &rng); bi_random(&B, cases[i].nb, kind == GEN_KINDS - 1 ? GEN_UNIFORM : kind, &rng);
                memset(&rns_st, 0, sizeof rns_st);
                rns_mul(&C, &A, &B);
                char what[128]; snprintf(what, sizeof what, "%s %zux%zu %s", cases[i].why, cases[i].na, cases[i].nb, gen_name[kind]);
                check_product(what, &A, &B, &C, 1);
                if (kind == 0) printf("   %-44s mdev %zu split %zu school %zu  %.3f s\n", what, rns_st.n_mdev, rns_st.n_split, rns_st.n_school, rns_st.t_total);
            }
        /* low products (A B) mod 2^(64 w) across the split tiers */
        {
            mpz_t a, b, c, m; mpz_inits(a, b, c, m, NULL);
            struct { size_t na, nb, w; } lc[] = { {P, P, P + 2}, {P, P, P / 2 + 1}, {P + 7, P - 3, P + 2}, {3 * P, P / 2, P / 2 + 2},
                                                  {2 * P + 5, 2 * P + 5, 2 * P + 7}, {2 * P, 2 * P, 3}, {5 * P, 5 * P, 5 * P + 2}, {P / 2, P / 2, 100} };
            for (size_t i = 0; i < sizeof lc / sizeof *lc; i++) for (int kind = 0; kind < GEN_KINDS; kind += 2) {
                bi_random(&A, lc[i].na, kind, &rng); bi_random(&B, lc[i].nb, kind == GEN_ZEROS ? GEN_ONES : kind, &rng);
                rns_mul_low(&C, A.l, A.n, B.l, B.n, lc[i].w);
                bi_to_mpz(a, &A); bi_to_mpz(b, &B); mpz_mul(c, a, b);
                if (bi_decimal) { mpz_ui_pow_ui(m, BI_B10, lc[i].w); mpz_fdiv_r(c, c, m); } else mpz_fdiv_r_2exp(c, c, 64 * lc[i].w);
                VERIFY(bi_eq_mpz(&C, c), "low product %zux%zu w %zu %s", lc[i].na, lc[i].nb, lc[i].w, gen_name[kind]);
            }
            mpz_clears(a, b, c, m, NULL);
        }
        /* zero and one */
        bi_random(&A, 5000, GEN_UNIFORM, &rng); bi_set_zero(&B); rns_mul(&C, &A, &B); VERIFY(C.n == 0, "x * 0");
        bi_set_u64(&B, 1); rns_mul(&C, &A, &B); VERIFY(bi_cmp(&C, &A) == 0, "x * 1");
    rns_shutdown();
    }

    /* 2. the paper's pool */
    printf("-- 2. pool 2^%d: paper's boundaries and the 2^31-point mdev\n", big32 ? 32 : 31);
    rns_init(big32 ? 32 : 31);
    if (!only_batch) {
        static const size_t sizes[] = {1 << 16, (1 << 17) - 1, 1 << 17, (1 << 18) - 1, 1 << 18, 1 << 20, 1 << 24, 1 << 26, 1 << 27, 1 << 28, 1 << 30};
        for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            size_t n = sizes[i];
            bi_random(&A, n, GEN_UNIFORM, &rng); bi_random(&B, n, GEN_UNIFORM, &rng);
            memset(&rns_st, 0, sizeof rns_st);
            rns_mul(&C, &A, &B);
            char what[64]; snprintf(what, sizeof what, "2^31 pool %zu x %zu limbs", n, n);
            check_product(what, &A, &B, &C, n <= (bi_decimal ? (1 << 20) : (1 << 26)));   /* decimal: the GMP string bridge is slow above 2^20 */
            printf("   %-36s %.3f s: repack %.3f h2d %.3f fwd %.3f inv %.3f d2h %.3f crt %.3f\n", what, rns_st.t_total,
                   rns_st.t_repack, rns_st.t_h2d, rns_st.t_fwd, rns_st.t_inv, rns_st.t_d2h, rns_st.t_crt);
            if (n == (1 << 30)) {
                harness_result("mdev_2e31_total", "s", rns_st.t_total);
                harness_result("mdev_2e31_crt", "s", rns_st.t_crt);
                harness_result("mdev_2e31_ntt", "s", rns_st.t_fwd + rns_st.t_inv);
                /* repeat for a steady-state time */
                for (int lay = 1; lay >= 0; lay--) {
                    rns_crt_layout = lay;
                    memset(&rns_st, 0, sizeof rns_st); rns_mul(&C, &A, &B);
                    check_product(what, &A, &B, &C, 0);
                    printf("   %-36s %.3f s (layout %d): repack %.3f h2d %.3f fwd %.3f inv %.3f d2h %.3f crt %.3f\n", what, rns_st.t_total, lay,
                           rns_st.t_repack, rns_st.t_h2d, rns_st.t_fwd, rns_st.t_inv, rns_st.t_d2h, rns_st.t_crt);
                    char nm[48]; snprintf(nm, sizeof nm, "mdev_2e31_total_layout%d", lay); harness_result(nm, "s", rns_st.t_total);
                    snprintf(nm, sizeof nm, "mdev_2e31_crt_layout%d", lay); harness_result(nm, "s", rns_st.t_crt);
                }
                rns_crt_layout = 0;
            }
        }
    }
    /* mdev_pair vs two products */
    if (!only_batch) {
        bigint A2, C2; bi_init(&A2); bi_init(&C2);
        static const size_t ps[] = {1 << 12, 1 << 20, (1 << 27) + 5};
        for (size_t i = 0; i < 3; i++) {
            bi_random(&A, ps[i], GEN_UNIFORM, &rng); bi_random(&A2, ps[i] - 3, GEN_ONES, &rng); bi_random(&B, ps[i] + 1, GEN_UNIFORM, &rng);
            memset(&rns_st, 0, sizeof rns_st);
            rns_mul_pair(&C, &A, &C2, &A2, &B);
            check_product("pair C1", &A, &B, &C, ps[i] <= (1 << 24)); check_product("pair C2", &A2, &B, &C2, ps[i] <= (1 << 24));
            printf("   pair %zu limbs: %.3f s, %zu mdev (fwd %.3f)\n", ps[i], rns_st.t_total, rns_st.n_mdev, rns_st.t_fwd);
        }
        bi_free(&A2); bi_free(&C2);
    }
    if (big) {
        printf("-- 3. 2.08e9 x 2.08e9 limbs (the 10dP product, > 2^31 points) through the split\n");
        size_t n = 2080000000;
        bi_random(&A, n, GEN_UNIFORM, &rng); bi_random(&B, n, GEN_UNIFORM, &rng);
        memset(&rns_st, 0, sizeof rns_st);
        double t0 = mem_now();
        rns_mul(&C, &A, &B);
        double dt = mem_now() - t0;
        check_product("10dP-size split product", &A, &B, &C, 0);
        printf("   %.2f s: %zu mdev, %zu splits; VmHWM %.1f GB\n", dt, rns_st.n_mdev, rns_st.n_split, mem_vmhwm() / 1e9);
        harness_result("split_10dP_total_first", "s", dt);
        memset(&rns_st, 0, sizeof rns_st); t0 = mem_now(); rns_mul(&C, &A, &B); dt = mem_now() - t0;
        check_product("10dP-size split product (2nd)", &A, &B, &C, 0);
        printf("   %.2f s second run: mdev total %.2f (repack %.2f h2d %.2f ntt %.2f d2h %.2f crt %.2f)\n", dt, rns_st.t_total,
               rns_st.t_repack, rns_st.t_h2d, rns_st.t_fwd + rns_st.t_inv, rns_st.t_d2h, rns_st.t_crt);
        harness_result("split_10dP_total", "s", dt);
    }
    /* 4. batch tier */
    printf("-- 4. batch: N products of <= 2^logL points, GPU and CPU CRT, registered and staged operands\n");
    {
        struct { int logL; size_t N; int reg; const char *why; } bc[] = {
            {14, 200, 3, "grpB: 200 A x one B, 2^14"}, {10, 5, 3, "grpB: CPU CRT"}, {12, 300, 2, "grpB staged"}, {17, 3000, 3, "grpB: 3000 x 2^17, tiles"},
            {22, 12, 1, "2^22 (max)"}, {20, 6, 1, "2^20 CPU CRT"},
            {10, 5, 1, "N < 8: CPU CRT"}, {10, 8, 1, "N = 8: GPU CRT"}, {10, 1000, 1, "many tiny"},
            {12, 300, 1, "2^12"}, {14, 200, 1, "2^14"}, {17, 100, 1, "2^17"}, {18, 40, 1, "2^18"},
            {14, 200, 0, "2^14 staged (unregistered)"}, {10, 5, 0, "tiny staged, CPU CRT"},
            {14, 40000, 1, "2^14 x 40000: several tiles"},
        };
        mpz_t a, b, c, d; mpz_inits(a, b, c, d, NULL);
        for (size_t ci = 0; ci < sizeof bc / sizeof *bc; ci++) {
            size_t L = (size_t)1 << bc[ci].logL, N = bc[ci].N;
            rns_prod *P = (rns_prod *)calloc(1, N * sizeof *P);
            size_t need = 0;
            uint64_t *pool;
            size_t *na = (size_t *)malloc(N * 8), *nb = (size_t *)malloc(N * 8);
            for (size_t i = 0; i < N; i++) {
                size_t nc = L / 2 + rng_next(&rng) % (L / 2) + 1; if (nc > L) nc = L;    /* nc in (L/2, L] */
                if (i == 0) nc = L;                                                     /* exactly L */
                na[i] = 1 + rng_next(&rng) % (nc - 1); nb[i] = nc - na[i];
                if (i == 0) { na[i] = L / 2; nb[i] = L / 2; }
            }
            int reg = bc[ci].reg & 1, shared = bc[ci].reg >= 2;
            if (shared) for (size_t i = 1; i < N; i++) { nb[i] = nb[0]; if (na[i] + nb[i] > L) na[i] = L - nb[i]; }
            for (size_t i = 0; i < N; i++) need += 2 * na[i] + nb[i] + (shared && i ? 0 : nb[i]);
            pool = reg ? (uint64_t *)mem_hreg_alloc(need * 8) : (uint64_t *)malloc(need * 8);
            size_t off = 0;
            for (size_t i = 0; i < N; i++) {
                P[i].a = pool + off; P[i].na = na[i]; off += na[i];
                P[i].b = (shared && i) ? P[0].b : pool + off; P[i].nb = nb[i]; if (!shared || i == 0) off += nb[i];
                P[i].c = pool + off; off += na[i] + nb[i];
                int kind = (int)(i % GEN_KINDS);
                gen_limbs((uint64_t *)P[i].a, na[i], kind, &rng);
                if (!shared || i == 0) gen_limbs((uint64_t *)P[i].b, nb[i], kind == GEN_ZEROS ? GEN_ONES : kind, &rng);
            }
            memset(&rns_st, 0, sizeof rns_st);
            fprintf(stderr, "   running %s (need %zu limbs, pool %p)\n", bc[ci].why, need, (void *)pool);
            double t0 = mem_now();
            rns_mul_batch(P, N);
            double dt = mem_now() - t0;
            size_t bad = 0, pts = 0;
            for (size_t i = 0; i < N; i++) {
                mpz_from_limbs(a, P[i].a, P[i].na); mpz_from_limbs(b, P[i].b, P[i].nb);
                mpz_mul(c, a, b); mpz_from_limbs(d, P[i].c, P[i].na + P[i].nb);
                if (mpz_cmp(c, d) != 0) { if (!bad) printf("   first bad product %zu: na %zu nb %zu\n", i, P[i].na, P[i].nb); bad++; }
                pts += L;
            }
            VERIFY(bad == 0, "batch %s: %zu of %zu products wrong", bc[ci].why, bad, N);
            printf("   %-34s N=%-6zu %.3f s  %.2f Gpoint/s\n", bc[ci].why, N, dt, pts / dt / 1e9);
            if (N == 40000) harness_result("batch_2e14_Gpoints_per_s", "Gpoint/s", pts / dt / 1e9);
            if (reg) mem_hreg_free(pool); else free(pool);
            free(P); free(na); free(nb);
        }
        mpz_clears(a, b, c, d, NULL);
    }
    printf("VmHWM %.1f GB\n", mem_vmhwm() / 1e9);
    rns_shutdown();
    return verify_done("t_mul");
}
