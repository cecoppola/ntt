/* t_dr - C3 step 1 (results/CORRECTNESS.md): the host helpers of dr.c against GMP.  Host only, no node time.
 * residue algebra (add/sub/mul/pow, B^e, a limb array and a number cut into shifted parts) vs mpz; the D identity's m in {-1, 0, 1}
 * (and a fault giving 2); the residues modulo B+1 and B-1 of random and extreme cyclic products U = A B mod (B^P - 1), P even;
 * the injection parser and latches; the counters and the context text.  Run: tests/t_dr */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dr.h"
#include "harness.h"

static void limbs_to_mpz(mpz_t z, const uint64_t *l, size_t n)
{
    mpz_t b, t; mpz_init_set_ui(b, 1000000000UL); mpz_mul(b, b, b); mpz_init(t); mpz_set_ui(z, 0);
    for (size_t i = n; i-- > 0;) { mpz_mul(z, z, b); mpz_set_ui(t, 0); mpz_set_ui(t, (unsigned long)l[i]); mpz_add(z, z, t); }
    mpz_clear(b); mpz_clear(t);
}
static void mpz_to_n_limbs(uint64_t *l, size_t n, const mpz_t z)             /* z < B^n */
{
    mpz_t b, r, q; mpz_init_set_ui(b, 1000000000UL); mpz_mul(b, b, b); mpz_init_set(q, z); mpz_init(r);
    for (size_t i = 0; i < n; i++) { mpz_tdiv_qr(q, r, q, b); l[i] = mpz_get_ui(r); }
    VERIFY(mpz_sgn(q) == 0, "limb overflow");
    mpz_clear(b); mpz_clear(r); mpz_clear(q);
}
static uint64_t mpz_mod_u(const mpz_t z, uint64_t m) { mpz_t mm, r; mpz_init_set_ui(mm, (unsigned long)m); mpz_init(r); mpz_mod(r, z, mm); uint64_t v = mpz_get_ui(r); mpz_clear(mm); mpz_clear(r); return v; }
static uint64_t rlimb(rng_t *r, int mode) { return mode == 0 ? 0 : mode == 1 ? DR_B - 1 : rng_next(r) % DR_B; }

static void t_arith(rng_t *r)
{
    mpz_t a, b, c; mpz_init(a); mpz_init(b); mpz_init(c);
    for (int it = 0; it < 2000; it++) {
        uint64_t x = rng_next(r) % DR_Q, y = rng_next(r) % DR_Q;
        if (it == 0) x = DR_Q - 1, y = DR_Q - 1;
        if (it == 1) x = 0, y = DR_Q - 1;
        mpz_set_ui(a, (unsigned long)x); mpz_set_ui(b, (unsigned long)y);
        mpz_add(c, a, b); VERIFY(dr_addmod(x, y) == mpz_mod_u(c, DR_Q), "add");
        mpz_sub(c, a, b); VERIFY(dr_submod(x, y) == mpz_mod_u(c, DR_Q), "sub");
        mpz_mul(c, a, b); VERIFY(dr_mulmod(x, y) == mpz_mod_u(c, DR_Q), "mul");
    }
    mpz_t B; mpz_init_set_ui(B, 1000000000UL); mpz_mul(B, B, B);
    for (uint64_t e = 0; e < 300; e += (e < 20 ? 1 : 37)) { mpz_pow_ui(c, B, e); VERIFY(dr_powB(e) == mpz_mod_u(c, DR_Q), "powB %llu", (unsigned long long)e); }
    mpz_ui_pow_ui(c, 2, 62); VERIFY(dr_powmod(2, 62) == mpz_mod_u(c, DR_Q), "2^62");
    mpz_clear(a); mpz_clear(b); mpz_clear(c); mpz_clear(B);
}
static void t_prime(void)
{
    mpz_t z, n; mpz_init_set_ui(z, DR_Q); VERIFY(mpz_probab_prime_p(z, 40) != 0, "DR_Q is prime");
    mpz_init_set_ui(n, 1); mpz_mul_2exp(n, n, 62); mpz_nextprime(n, n); VERIFY(mpz_cmp(n, z) == 0, "DR_Q is the first prime above 2^62");
    mpz_clear(z); mpz_clear(n);
}
static void t_limbs(rng_t *r)                       /* a limb array, and the same cut into shifted parts (totals) */
{
    enum { MAXN = 200 }; uint64_t l[MAXN]; mpz_t z; mpz_init(z);
    for (int it = 0; it < 300; it++) {
        size_t n = 1 + rng_next(r) % MAXN; int mode = it % 4;
        for (size_t i = 0; i < n; i++) l[i] = rlimb(r, mode == 3 ? 2 : mode);
        limbs_to_mpz(z, l, n); uint64_t want = mpz_mod_u(z, DR_Q), got = dr_limbs_res(l, n);
        VERIFY(got == want, "limbs_res n=%zu mode=%d", n, mode);
        size_t cut1 = rng_next(r) % (n + 1), cut2 = cut1 + rng_next(r) % (n - cut1 + 1);   /* [0, cut1) [cut1, cut2) [cut2, n) */
        uint64_t tot = 0;
        dr_res_part(&tot, dr_limbs_res(l, cut1), 0); dr_res_part(&tot, dr_limbs_res(l + cut1, cut2 - cut1), cut1); dr_res_part(&tot, dr_limbs_res(l + cut2, n - cut2), cut2);
        VERIFY(tot == want, "parts n=%zu cuts %zu %zu", n, cut1, cut2);
        VERIFY(dr_res_shift(got, 7) == dr_mulmod(got, dr_powB(7)), "shift");
    }
    uint64_t big[3] = { ~0ULL, ~0ULL, 5 }; mpz_t m; mpz_init(m);   /* limbs >= B are accepted by the helper (reduced first): sum l_i B^i */
    mpz_set_ui(z, 0); mpz_t b; mpz_init_set_ui(b, 1000000000UL); mpz_mul(b, b, b); mpz_t t; mpz_init(t);
    for (int i = 2; i >= 0; i--) { mpz_mul(z, z, b); mpz_set_ui(t, 0); mpz_import(t, 1, -1, 8, 0, 0, &big[i]); mpz_add(z, z, t); }
    VERIFY(dr_limbs_res(big, 3) == mpz_mod_u(z, DR_Q), "limbs >= B");
    mpz_clear(m); mpz_clear(b); mpz_clear(t); mpz_clear(z);
}
static void t_m(rng_t *r)                           /* E = m B^w with m in {-1, 0, 1}: the D identity of a low product; a fault is not */
{
    mpz_t X, Q, S, Aw, xql, E, B, Bw; mpz_inits(X, Q, S, Aw, xql, E, B, Bw, NULL); mpz_set_ui(B, 1000000000UL); mpz_mul(B, B, B);
    for (int it = 0; it < 600; it++) {
        unsigned w = 1 + (unsigned)(rng_next(r) % 12), e = 0; int m = (int)(it % 3) - 1;
        mpz_pow_ui(Bw, B, w);
        uint64_t xl[8], ql[8]; for (int i = 0; i < 8; i++) { xl[i] = rlimb(r, 2); ql[i] = rlimb(r, 2); }
        limbs_to_mpz(X, xl, 8); limbs_to_mpz(Q, ql, 8);
        /* xql = X Q mod B^w; Aw = (X Q mod B^w) + m B^w (the low basis may differ by one B^w); S = X Q / B^e-ish: here S B^e - X Q = 0 exactly by construction */
        mpz_mul(E, X, Q); mpz_mod(xql, E, Bw);
        mpz_set(Aw, xql); if (m == 1) mpz_add(Aw, Aw, Bw); else if (m == -1) mpz_sub(Aw, Aw, Bw);
        (void)e; mpz_set(S, E);                                   /* S B^e == X Q with e = 0 */
        uint64_t rS = mpz_mod_u(S, DR_Q), rX = mpz_mod_u(X, DR_Q), rQ = mpz_mod_u(Q, DR_Q), rA = mpz_mod_u(Aw, DR_Q), rx = mpz_mod_u(xql, DR_Q);
        uint64_t ev = dr_submod(dr_submod(rS, dr_mulmod(rX, rQ)), dr_submod(rA, rx));   /* (R(S) - R(X)R(Q)) - (R(Aw) - R(xql)) = -m B^w */
        uint64_t rBw = mpz_mod_u(Bw, DR_Q);
        VERIFY(dr_match_m(ev, rBw) == -m, "m=%d got %d", m, dr_match_m(ev, rBw));
        VERIFY(dr_match_m(dr_addmod(ev, 1), rBw) == 2, "a fault of +1 matched");
    }
    mpz_clears(X, Q, S, Aw, xql, E, B, Bw, NULL);
}
static void t_cyc(rng_t *r)                         /* U = A B mod (B^P - 1): U = X~ Q'' modulo B+1 and B-1; a changed limb breaks it */
{
    mpz_t A, Bz, U, T, M, B; mpz_inits(A, Bz, U, T, M, B, NULL); mpz_set_ui(B, 1000000000UL); mpz_mul(B, B, B);
    for (int P = 2; P <= 64; P *= 2) for (int it = 0; it < 40; it++) {
        uint64_t a[64], b[64], u[64]; int ma = it % 4, mb = (it / 4) % 4;
        for (int i = 0; i < P; i++) { a[i] = rlimb(r, ma == 3 ? 2 : ma); b[i] = rlimb(r, mb == 3 ? 2 : mb); }
        limbs_to_mpz(A, a, P); limbs_to_mpz(Bz, b, P);
        mpz_pow_ui(M, B, P); mpz_sub_ui(M, M, 1);
        mpz_mul(T, A, Bz); mpz_mod(U, T, M);                         /* in [0, B^P - 2] */
        mpz_to_n_limbs(u, P, U);
        uint64_t up = dr_mod_bp1(u, P), um = dr_mod_bm1(u, P);
        VERIFY(dr_cyc_ok(up, um, dr_mod_bp1(a, P), dr_mod_bm1(a, P), dr_mod_bp1(b, P), dr_mod_bm1(b, P)), "cyclic P=%d it=%d", P, it);
        /* the residues against GMP */
        mpz_add_ui(T, B, 1); mpz_t R; mpz_init(R); mpz_mod(R, U, T); VERIFY(mpz_get_ui(R) == up, "mod B+1");
        mpz_sub_ui(T, B, 1); mpz_mod(R, U, T); VERIFY(mpz_get_ui(R) == um, "mod B-1"); mpz_clear(R);
        /* one limb changed by one (a "wrong limb in range"): never missed (+1 at limb i changes U by B^i, and B^i is a unit mod B+1 and B-1) */
        size_t i = rng_next(r) % P; uint64_t old = u[i]; u[i] = old + 1 < DR_B ? old + 1 : old - 1;
        VERIFY(!dr_cyc_ok(dr_mod_bp1(u, P), dr_mod_bm1(u, P), dr_mod_bp1(a, P), dr_mod_bm1(a, P), dr_mod_bp1(b, P), dr_mod_bm1(b, P)), "single-limb error missed P=%d i=%zu", P, i);
        u[i] = old;
    }
    mpz_clears(A, Bz, U, T, M, B, NULL);
}
static void T(const char *v) { if (v) setenv("ECALC_TEST", v, 1); else unsetenv("ECALC_TEST"); }
static void t_inject(void)
{
    setenv("ECALC_DR", "1", 1); T("dr=flag,dr_k=3,dr_pe=-1,dr_n=2");
    dr_init_from_env();
    VERIFY(dr_level() == 1 && dr_on() && dr_test_kind() == DR_FLAG && dr_test_k() == 3 && dr_test_pe() == -1 && dr_test_n() == 2, "parse");
    VERIFY(!dr_inject(DR_FLAG, 2, 0, 0, 0), "wrong occurrence");
    VERIFY(!dr_inject(DR_XQL, 3, 0, 0, 0), "wrong kind");
    VERIFY(dr_inject(DR_FLAG, 3, 5, 0, 0), "first attempt, any PE");
    VERIFY(!dr_inject(DR_FLAG, 3, 5, 0, 0), "the same attempt twice");
    VERIFY(dr_inject(DR_FLAG, 3, 5, 0, 1), "second consecutive attempt (N = 2)");
    VERIFY(!dr_inject(DR_FLAG, 3, 5, 0, 2), "third attempt");
    VERIFY(!dr_inject(DR_FLAG, 3, 5, 0, 0), "a later piece after N attempts");
    VERIFY(dr_inject(DR_FLAG, 3, 6, 1, 0), "another thread slot has its own latch");
    VERIFY(dr_counter(DR_C_INJECTED) == 3, "injected counter %zu", dr_counter(DR_C_INJECTED));
    T("dr=flag,dr_k=3,dr_pe=2,dr_n=1"); dr_init_from_env();
    VERIFY(!dr_inject(DR_FLAG, 3, 1, 2, 0), "other PE");
    VERIFY(dr_inject(DR_FLAG, 3, 2, 2, 0), "this PE");
    VERIFY(!dr_inject(DR_FLAG, 3, 2, 2, 1), "N = 1: no second attempt");
    T("dr=flag,dr_k=3,dr_pe=-1,dr_n=1,dr_m=2"); dr_init_from_env();
    VERIFY(dr_test_m() == 2 && dr_inject(DR_FLAG, 3, 0, 4, 0) && !dr_inject(DR_FLAG, 3, 0, 4, 0), "M = 2: first product, one latch");
    VERIFY(dr_inject(DR_FLAG, 4, 0, 4, 0) && !dr_inject(DR_FLAG, 5, 0, 4, 0) && !dr_inject(DR_FLAG, 2, 0, 5, 0), "M = 2: the second product fires again, the third and the one before do not");
    setenv("ECALC_DR", "0", 1); T(NULL); dr_init_from_env();
    VERIFY(!dr_on() && dr_test_kind() == -1 && !dr_inject(DR_FLAG, 3, 2, 3, 0), "off");
    dr_count(DR_C_CHECK, 4); dr_count(DR_C_FAIL, 1); VERIFY(dr_counter(DR_C_CHECK) == 4 && dr_counter(DR_C_FAIL) == 1, "counters");
    char w[160]; dr_ctx_set("tree", 3, 0, 4, 2, 1, -1); dr_where(w, sizeof w);
    VERIFY(!strcmp(w, "tree level 3, group [0, 4), sched 2, combine 1"), "where: %s", w);
    dr_ctx_clear(); VERIFY(dr_where(w, sizeof w) == 0 && !w[0], "where cleared");
    VERIFY(dr_total_cap() == 8, "default total cap"); setenv("ECALC_DR_TOTAL", "3", 1); dr_init_from_env(); VERIFY(dr_total_cap() == 3, "total cap");
    unsetenv("ECALC_DR_TOTAL"); dr_init_from_env();
    setenv("ECALC_DR", "3", 1); T("dr=leaf+leafslow"); dr_init_from_env();
    VERIFY(dr_test_kind() == DR_LEAF && dr_test_leafslow(), "kind list: leaf + leafslow");
    T("dr=leafslow"); dr_init_from_env();
    VERIFY(dr_test_kind() == -1 && dr_test_leafslow() && !dr_inject(DR_LEAF, 1, 0, 0, 0), "leafslow alone injects nothing");
    T("dr=xql,dr_k=2"); dr_init_from_env();
    VERIFY(dr_test_kind() == DR_XQL && !dr_test_leafslow() && !dr_inject(DR_XQL, 1, 0, 0, 0) && dr_inject(DR_XQL, 2, 0, 0, 0), "xql, K = 2");
    T("dr=cyc"); dr_init_from_env();
    VERIFY(dr_test_kind() == DR_CYC && dr_inject(DR_CYC, 1, 0, 0, 0), "cyc");
    T(NULL); setenv("ECALC_DR", "0", 1); dr_init_from_env();
}
int main(void)
{
    rng_t r = { 20261101 };
    t_prime(); t_arith(&r); t_limbs(&r); t_m(&r); t_cyc(&r); t_inject();
    return verify_done("t_dr");
}
