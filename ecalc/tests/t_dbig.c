/* t_dbig - the device bigint against the host bigint: shifts, add, sub, cmp, norm, set_base_pow,
 * from/to host, across quarter boundaries and both bases (LIMB_BASE). */
#include "harness.h"
#include <hip/hip_runtime.h>
#include "../dbig.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#include "../rns_mul.h"
#include <string.h>
static rng_t rg = { 99 };
static void rnd_bi(bigint *a, size_t n, int kind) { bi_reserve(a, n ? n : 1); gen_limbs(a->l, n, kind, &rg); a->n = n; bi_norm(a); }
static int same(const dbig *x, const bigint *a, const char *what)
{
    bigint t; bi_init(&t); db_to_bi(&t, x); int ok = bi_cmp(&t, a) == 0;
    if (!ok) printf("  %s: n %zu vs %zu\n", what, t.n, a->n);
    bi_free(&t); return ok;
}
int main(int argc, char **argv)
{
    int logmax = argc > 1 ? atoi(argv[1]) : 24;
    harness_meta("t_dbig"); bi_env_base(); rns_init(argc > 3 ? atoi(argv[3]) : 31);
    bigint a, b, r; bi_init(&a); bi_init(&b); bi_init(&r);
    dbig x, y, z; db_init(&x); db_init(&y); db_init(&z);
    size_t sizes[] = { 1, 5, 1023, 1024, 1025, 4097, 100000, (size_t)1 << 20, ((size_t)1 << 22) + 3, (size_t)1 << logmax };
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) for (int kind = 0; kind < GEN_KINDS; kind++) {
        size_t n = sizes[si];
        rnd_bi(&a, n, kind); rnd_bi(&b, n > 7 ? n - 7 : n, kind == 1 ? 0 : kind);
        db_from_bi(&x, &a); db_from_bi(&y, &b);
        VERIFY(same(&x, &a, "from/to"), "from/to n %zu %s", n, gen_name[kind]);
        bi_add(&r, &a, &b); db_add(&z, &x, &y); VERIFY(same(&z, &r, "add"), "add n %zu %s", n, gen_name[kind]);
        if (bi_cmp(&a, &b) >= 0) { bi_sub(&r, &a, &b); db_sub(&z, &x, &y); VERIFY(same(&z, &r, "sub"), "sub n %zu %s", n, gen_name[kind]); }
        else { bi_sub(&r, &b, &a); db_sub(&z, &y, &x); VERIFY(same(&z, &r, "sub"), "sub(b,a) n %zu %s", n, gen_name[kind]); }
        VERIFY(db_cmp(&x, &y) == bi_cmp(&a, &b), "cmp n %zu %s", n, gen_name[kind]);
        VERIFY(db_cmp(&x, &x) == 0, "cmp self n %zu", n);
        size_t k = n / 3 + 1;
        bi_shr_limbs(&r, &a, k); db_shr_limbs(&z, &x, k); VERIFY(same(&z, &r, "shr"), "shr n %zu k %zu %s", n, k, gen_name[kind]);
        bi_shl_limbs(&r, &a, k); db_shl_limbs(&z, &x, k); VERIFY(same(&z, &r, "shl"), "shl n %zu k %zu %s", n, k, gen_name[kind]);
        db_add(&z, &x, &x); bi_add(&r, &a, &a); VERIFY(same(&z, &r, "add self"), "add a+a n %zu %s", n, gen_name[kind]);
        db_copy(&z, &x); db_add(&z, &z, &y); bi_add(&r, &a, &b); VERIFY(same(&z, &r, "add in place"), "add in place n %zu %s", n, gen_name[kind]);
        VERIFY(db_top(&x) == (a.n ? a.l[a.n - 1] : 0), "top n %zu", n);
    }
    for (size_t k = 0; k < 5000; k += 1237) { bi_set_base_pow(&r, k); db_set_base_pow(&z, k); VERIFY(same(&z, &r, "base pow"), "base_pow %zu", k); }
    /* products on device operands against rns_mul (which is GMP-checked in t_mul) */
    if (argc > 2) {
        int big = !strcmp(argv[2], "big");                    /* include the products that split (> 2^31 points) */
        struct { size_t na, nb; } pc[] = { {1000, 1000}, {600000, 500000}, {1u << 20, (1u << 20) + 7}, {(1u << 24) + 3, 1u << 23}, {1u << 27, 1u << 27}, {(size_t)1 << 30, ((size_t)1 << 30) + 5}, {(size_t)1 << 31, (size_t)1 << 30} };
        size_t npc = big ? 7 : 5;
        for (size_t i = 0; i < npc; i++) for (int kind = 0; kind < 2; kind++) {
            rnd_bi(&a, pc[i].na, kind); rnd_bi(&b, pc[i].nb, kind);
            db_from_bi(&x, &a); db_from_bi(&y, &b);
            double t0 = now(); rns_mul_dist_db(&z, &x, &y); double t1 = now();
            rns_mul(&r, &a, &b);
            VERIFY(same(&z, &r, "dist_db"), "dist_db %zux%zu %s", pc[i].na, pc[i].nb, gen_name[kind]);
            if (kind == 0) printf("   dist_db %zux%zu: %.3f s\n", pc[i].na, pc[i].nb, t1 - t0);
        }
        /* the grid split at small sizes: DIST_LOGN_TEST=24 caps the plane at 2^24, so 9.3e6 x 9.3e6 is the shape of
         * decimal's 4e10 top product (2 x 3 pieces), 1.5e7 x 4e6 a 4 x 1 grid, 1.2e7 x 1.2e7 2 x 2 */
        if (big) {
            setenv("DIST_LOGN_TEST", "24", 1);
            struct { size_t na, nb; } gc[] = { {9300000, 9300000}, {15000000, 4000000}, {12000000, 12000000}, {16777216, 8388608}, {30000000, 7000000} };
            for (size_t i = 0; i < 5; i++) for (int kind = 0; kind < 2; kind++) {
                rnd_bi(&a, gc[i].na, kind); rnd_bi(&b, gc[i].nb, kind);
                db_from_bi(&x, &a); db_from_bi(&y, &b);
                rns_mul_dist_db(&z, &x, &y); rns_mul(&r, &a, &b);
                VERIFY(same(&z, &r, "grid"), "grid %zux%zu %s", gc[i].na, gc[i].nb, gen_name[kind]);
                for (int wv = 0; wv < 3; wv++) {                              /* low products: pieces above w skipped */
                    size_t w = wv == 0 ? gc[i].na + 2 : wv == 1 ? gc[i].na / 2 + 1 : r.n - 1;
                    rns_mul_low_db(&z, &x, &y, w);
                    bigint rl; bi_init(&rl); bi_reserve(&rl, r.n); memcpy(rl.l, r.l, (r.n < w ? r.n : w) * 8); rl.n = r.n < w ? r.n : w; bi_norm(&rl);
                    VERIFY(same(&z, &rl, "low grid"), "low grid %zux%zu w %zu %s", gc[i].na, gc[i].nb, w, gen_name[kind]);
                    bi_free(&rl);
                }
            }
            unsetenv("DIST_LOGN_TEST");
        }
    }
    bi_free(&a); bi_free(&b); bi_free(&r); db_free(&x); db_free(&y); db_free(&z);
    db_release_pools();                                       /* nothing live now: the pool can be reset for the stress test */
    /* the block pool under a bs-like pattern: a donated region per device, then many numbers of mixed sizes
     * allocated and freed in waves; with coalescing the pool must never need more than the donation */
    {
        size_t region = (size_t)1 << 30; void *reg[DB_NQ];
        for (int d = 0; d < DB_NQ; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&reg[d], region)); db_donate(d, reg[d], region); }
        size_t before = db_pool_bytes(); dbig w[24]; for (int i = 0; i < 24; i++) db_init(&w[i]);
        rng_t r2 = { 5 };
        for (int wave = 0; wave < 6; wave++) {
            for (int i = 0; i < 24; i++) { size_t n = (rng_next(&r2) % (1u << 24)) + 1000; db_reserve(&w[i], n); w[i].n = n; }
            for (int i = wave % 2; i < 24; i += 2) db_free(&w[i]);
        }
        for (int i = 0; i < 24; i++) db_free(&w[i]);
        VERIFY(db_pool_bytes() == before, "block pool grew beyond the donation: +%.2f GB (extents %d)", (db_pool_bytes() - before) / 1e9, db_pool_extents(0));
        VERIFY(db_pool_extents(0) == 1, "free extents coalesced back to one per device (have %d)", db_pool_extents(0));
        db_release_pools();
    }
    return verify_done("t_dbig");
}
