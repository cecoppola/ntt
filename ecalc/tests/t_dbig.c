/* t_dbig - the device bigint against the host bigint: shifts, add, sub, cmp, norm, set_base_pow,
 * from/to host, across quarter boundaries and both bases (LIMB_BASE). */
#include "harness.h"
#include "../dbig.h"
#include "../rns_mul.h"
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
    harness_meta("t_dbig"); bi_env_base(); rns_init(20);
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
    bi_free(&a); bi_free(&b); bi_free(&r); db_free(&x); db_free(&y); db_free(&z);
    return verify_done("t_dbig");
}
