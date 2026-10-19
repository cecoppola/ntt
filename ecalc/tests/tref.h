/* tref.h - the tests' reference product (TREF, results/CODE_REDUCTION.md §TREF; it replaced the host product tier rns_mul as the reference).
 *
 * tref_ok(a, na, b, nb, c, nc, mode, why)   is c == a b?  (decimal limbs, any lengths; why gets the first failed item)
 *    TREF_AUTO: exact by GMP (the product-tree conversions of harness.h) when na + nb <= TREF_EXACT_LIMBS, else the residue form;
 *    TREF_RES:  the residue form at any size --
 *      (1) every limb of c < B and its normalised length na + nb or na + nb - 1;
 *      (2) c == a b mod three 62-bit primes (t1_q[0..2] of verify.c, vf_limbs_mods: O(n), parallel);
 *      (3) the low TREF_EDGE limbs exact: a b mod B^64 from the low 64 limbs of a and b (GMP);
 *      (4) the top limbs exact up to the carry from below: with A = Ah B^ka + Al (Ah the top 64 limbs, Al < B^ka) and B likewise,
 *          floor(c / B^(ka+kb)) lies in [Ah Bh, Ah Bh + Ah + Bh] (the dropped part is < (Ah + Bh + 1) B^(ka+kb)).
 *      One wrong limb changes c by d B^k with 0 < |d| < B, never a multiple of a prime q > B, so (2) alone catches it; an
 *      unstructured wrong product passes (2) with probability about 2^-186.  (3) and (4) pin both ends exactly.
 * tref_check(what, A, B, C)   VERIFY of tref_ok(TREF_AUTO) on bigints
 * tref_mul(C, A, B)           the reference VALUE: GMP when na + nb <= TREF_EXACT_LIMBS, else the device product rns_mul_dist_db at
 *    the default plane (DIST_LOGN_TEST is lifted for the call: the tests' grids run under a smaller cap, so the value comes from a
 *    different plane size than the product under test) checked by the residue form; a failed check is a VERIFY failure.
 * tref_faults(a, na, b, nb, c, nc, mode, rng)   the reference's own test: c (correct) must pass, and fail with one limb changed
 *    at the low end, the middle and the top (each restored after); returns the number of faults NOT caught (0 = good).
 *
 * Needs -lgmp and verify.o; tref_mul needs rns_init (HIP builds only: define TREF_NO_DEVICE to leave it out).
 */
#ifndef EC_TREF_H
#define EC_TREF_H
#include "harness.h"
#include "../verify.h"
#ifndef TREF_NO_DEVICE
#include "../dbig.h"
#include "../rns_mul.h"
#endif

#define TREF_EXACT_LIMBS ((size_t)1 << 21)
#define TREF_EDGE 64
enum { TREF_AUTO, TREF_RES };

static size_t tref_norm(const uint64_t *a, size_t n) { while (n && a[n - 1] == 0) n--; return n; }
/* the exact compare: the product P (GMP) against c */
static int tref_exact_ok(const mpz_t P, const uint64_t *c, size_t nc, char *why, size_t nw)
{
    for (size_t i = 0; i < nc; i++) if (c[i] >= BI_B10) { snprintf(why, nw, "limb %zu >= B", i); return 0; }
    mpz_t t; mpz_init(t); mpz_from_limbs(t, c, nc);
    int ok = mpz_cmp(t, P) == 0; mpz_clear(t);
    if (!ok) snprintf(why, nw, "differs from the GMP product");
    return ok;
}
static int tref_res_ok(const uint64_t *a, size_t na, const uint64_t *b, size_t nb, const uint64_t *c, size_t nc, char *why, size_t nw)
{
    /* (1) limbs and length */
    size_t big = (size_t)-1;
#pragma omp parallel for schedule(static) reduction(min:big) if (nc > ((size_t)1 << 20))
    for (size_t i = 0; i < nc; i++) if (c[i] >= BI_B10 && i < big) big = i;
    if (big != (size_t)-1) { snprintf(why, nw, "limb %zu >= B", big); return 0; }
    if (nc != na + nb && nc + 1 != na + nb) { snprintf(why, nw, "length %zu (operands %zu + %zu)", nc, na, nb); return 0; }
    /* (2) residues */
    uint64_t ra[3], rb[3], rc[3];
    vf_limbs_mods(a, na, t1_q, 3, ra); vf_limbs_mods(b, nb, t1_q, 3, rb); vf_limbs_mods(c, nc, t1_q, 3, rc);
    for (int j = 0; j < 3; j++) if (hv_mulmod(ra[j], rb[j], t1_q[j]) != rc[j]) { snprintf(why, nw, "residue mod t1_q[%d] differs", j); return 0; }
    /* (3) the low limbs */
    mpz_t x, y, z, m; mpz_inits(x, y, z, m, NULL);
    int ok = 1;
    {
        size_t W = TREF_EDGE, la = na < W ? na : W, lb = nb < W ? nb : W;
        uint64_t lo[TREF_EDGE], want[TREF_EDGE];
        mpz_from_limbs(x, a, la); mpz_from_limbs(y, b, lb); mpz_mul(z, x, y);
        mpz_fdiv_r(z, z, *hv_pow2k(6)); hv_to_limbs_n(want, W, z);             /* z mod B^64 (TREF_EDGE = 2^6) */
        for (size_t i = 0; i < W; i++) lo[i] = i < nc ? c[i] : 0;
        for (size_t i = 0; i < W && ok; i++) if (lo[i] != want[i]) { snprintf(why, nw, "low limb %zu differs", i); ok = 0; }
    }
    /* (4) the top limbs */
    if (ok) {
        size_t W = TREF_EDGE, ka = na > W ? na - W : 0, kb = nb > W ? nb - W : 0;
        mpz_from_limbs(x, a + ka, na - ka); mpz_from_limbs(y, b + kb, nb - kb); mpz_mul(z, x, y);   /* z = Ah Bh */
        mpz_from_limbs(m, c + ka + kb, nc > ka + kb ? nc - ka - kb : 0);                           /* m = floor(c / B^(ka+kb)) */
        if (mpz_cmp(m, z) < 0) { snprintf(why, nw, "top limbs below Ah Bh"); ok = 0; }
        else { mpz_add(z, z, x); mpz_add(z, z, y); if (mpz_cmp(m, z) > 0) { snprintf(why, nw, "top limbs above Ah Bh + Ah + Bh"); ok = 0; } }
    }
    mpz_clears(x, y, z, m, NULL);
    return ok;
}
/* P: the exact product when the caller has it (exact mode only), else 0 */
static int tref_ok_p(const uint64_t *a, size_t na, const uint64_t *b, size_t nb, const uint64_t *c, size_t nc, int mode, const mpz_t *P, char *why, size_t nw)
{
    na = tref_norm(a, na); nb = tref_norm(b, nb); nc = tref_norm(c, nc);
    if (!na || !nb) { if (nc) snprintf(why, nw, "a zero operand, %zu limbs of product", nc); return nc == 0; }
    if (mode == TREF_AUTO && na + nb <= TREF_EXACT_LIMBS) {
        if (P) return tref_exact_ok(*P, c, nc, why, nw);
        mpz_t x, y, z; mpz_inits(x, y, z, NULL);
        mpz_from_limbs(x, a, na); mpz_from_limbs(y, b, nb); mpz_mul(z, x, y);
        int ok = tref_exact_ok(z, c, nc, why, nw);
        mpz_clears(x, y, z, NULL); return ok;
    }
    return tref_res_ok(a, na, b, nb, c, nc, why, nw);
}
static int tref_ok(const uint64_t *a, size_t na, const uint64_t *b, size_t nb, const uint64_t *c, size_t nc, int mode, char *why, size_t nw)
{
    return tref_ok_p(a, na, b, nb, c, nc, mode, 0, why, nw);
}
static int tref_check(const char *what, const bigint *A, const bigint *B, const bigint *C)
{
    char why[160] = "";
    int ok = tref_ok(A->l, A->n, B->l, B->n, C->l, C->n, TREF_AUTO, why, sizeof why);
    VERIFY(ok, "%s: product %zu x %zu wrong (%s; %s)", what, A->n, B->n, why, A->n + B->n <= TREF_EXACT_LIMBS ? "exact" : "residue form");
    return ok;
}
/* one limb changed to another value < B (returns the old value) */
static uint64_t tref_flip(uint64_t *c, size_t k, rng_t *r) { uint64_t o = c[k]; c[k] = (o + 1 + rng_next(r) % (BI_B10 - 1)) % BI_B10; return o; }
static int tref_faults(const uint64_t *a, size_t na, const uint64_t *b, size_t nb, uint64_t *c, size_t nc, int mode, rng_t *r)
{
    char why[160];
    nc = tref_norm(c, nc);
    if (!nc) return 0;
    mpz_t P; int haveP = mode == TREF_AUTO && tref_norm(a, na) + tref_norm(b, nb) <= TREF_EXACT_LIMBS;   /* the exact product once */
    if (haveP) { mpz_t x, y; mpz_inits(P, x, y, NULL); mpz_from_limbs(x, a, na); mpz_from_limbs(y, b, nb); mpz_mul(P, x, y); mpz_clears(x, y, NULL); }
    const mpz_t *pp = haveP ? (const mpz_t *)&P : 0;
    int missed = 0;
    if (!tref_ok_p(a, na, b, nb, c, nc, mode, pp, why, sizeof why)) { printf("   tref_faults: the correct product fails (%s)\n", why); missed++; }
    size_t pos[3] = { nc > 1 ? 1 + rng_next(r) % (nc < TREF_EDGE ? nc - 1 : TREF_EDGE - 1) : 0, nc / 2, nc - 1 };
    static const char *pn[3] = { "low", "middle", "top" };
    for (int i = 0; i < 3; i++) {
        uint64_t o = tref_flip(c, pos[i], r);
        if (tref_ok_p(a, na, b, nb, c, nc, mode, pp, why, sizeof why)) { printf("   tref_faults: a %s limb (%zu of %zu) changed was NOT caught\n", pn[i], pos[i], nc); missed++; }
        c[pos[i]] = o;
    }
    if (haveP) mpz_clear(P);
    return missed;
}

#ifndef TREF_NO_DEVICE
static void tref_mul(bigint *C, const bigint *A, const bigint *B)
{
    size_t na = tref_norm(A->l, A->n), nb = tref_norm(B->l, B->n);
    if (!na || !nb) { bi_reserve(C, 1); C->n = 0; return; }
    if (na + nb <= TREF_EXACT_LIMBS) {
        mpz_t x, y, z; mpz_inits(x, y, z, NULL);
        mpz_from_limbs(x, A->l, na); mpz_from_limbs(y, B->l, nb); mpz_mul(z, x, y); bi_from_mpz(C, z);
        mpz_clears(x, y, z, NULL); return;
    }
    const char *e = getenv("DIST_LOGN_TEST"); char save[32] = ""; if (e) { snprintf(save, sizeof save, "%s", e); unsetenv("DIST_LOGN_TEST"); }
    bigint a = { A->l, na, 0 }, b = { B->l, nb, 0 };
    dbig x, y, z; db_init(&x); db_init(&y); db_init(&z);
    db_from_bi(&x, &a); db_from_bi(&y, &b); rns_mul_dist_db(&z, &x, &y); db_to_bi(C, &z);
    db_free(&x); db_free(&y); db_free(&z);
    if (save[0]) setenv("DIST_LOGN_TEST", save, 1);
    char why[160] = "";
    int ok = tref_ok(a.l, na, b.l, nb, C->l, C->n, TREF_RES, why, sizeof why);
    VERIFY(ok, "tref_mul %zu x %zu: the device reference failed its residue check (%s)", na, nb, why);
}
#endif
#endif
