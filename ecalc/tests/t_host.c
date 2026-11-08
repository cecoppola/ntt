/* t_host - the host-only unit tests, one driver (no GPU, no node time): t_host [args|residue|nodemem|hosttl|dr ...]  (no argument = all)
 *   args     the digit count argument (count_arg.h): plain integers and exact scientific notation accepted, everything else rejected
 *   residue  the Shoup Horner step (residue_shoup.h) against the 128-bit % form, bit for bit (primes of verify.c and random ones, chains of 10^6 steps)
 *   nodemem  nodemem.h on a mock sysfs tree: the per-NUMA-node MemFree reader of the memory sampler
 *   hosttl   the host timeline bookkeeping of hosttl.h (ECALC_HOST_TL), injected times
 *   dr       the host helpers of dr.c against GMP (residue algebra, the D identity, cyclic residues, the ECALC_TEST parser and latches)
 * Build and run: make tests/t_host && tests/t_host */
#include "harness.h"
#include <math.h>
#include <sys/stat.h>
#include "../count_arg.h"
#include "../residue_shoup.h"
#include "../nodemem.h"
#include "../hosttl.h"
#include "../dr.h"
/* ---- args ---- */
static void ok(const char *s, unsigned long want)
{
    unsigned long v = 0; int rc = ec_parse_count(s, &v);
    VERIFY(rc == 0 && v == want, "\"%s\" should give %lu (rc %d, got %lu)", s, want, rc, v);
}
static void bad(const char *s)
{
    unsigned long v = 7; int rc = ec_parse_count(s, &v);
    VERIFY(rc != 0 && v == 7, "\"%s\" should be rejected (rc %d, value %lu)", s, rc, v);
}

static void host_args(void)
{
    /* plain integers, as before */
    ok("1", 1UL); ok("18", 18UL); ok("1000000000", 1000000000UL); ok("0000042", 42UL);
    /* exact scientific notation */
    ok("1e9", 1000000000UL); ok("1E9", 1000000000UL); ok("1e+9", 1000000000UL); ok("1.6e10", 16000000000UL);
    ok("5.4e13", 54000000000000UL); ok("1.50e1", 15UL); ok("15.0e0", 15UL); ok("0.5e1", 5UL); ok("1.5e3", 1500UL);
    ok("1e0", 1UL);
    ok("1e19", 10000000000000000000UL); ok("1.8446744073709551598e19", 18446744073709551598UL);
    ok("18446744073709551598", 18446744073709551598UL);          /* ULONG_MAX - 17: the largest accepted */
    /* rejected: fractions, signs, zero, trailing text, malformed exponents, overflow */
    bad(""); bad("e5"); bad("e"); bad("1e"); bad("1e+"); bad("1e-3"); bad("1e-"); bad("1x"); bad("1.6e1x");
    bad("1.6e10 "); bad(" 5"); bad("5 "); bad("2.5"); bad("3.0"); bad("2.5e0"); bad("1.5e0"); bad("1..5"); bad("1e5.0");
    bad("1.6e1e1"); bad("-1"); bad("+5"); bad("-1e9"); bad("0"); bad("00"); bad("0e5"); bad("0.0e3"); bad("0x10");
    bad("18446744073709551599"); bad("18446744073709551615"); bad("100000000000000000000"); bad("1e20"); bad("1e21");
    bad(".5e1"); bad("5.e1"); bad(".e1"); bad("1e99999999999999999999"); bad("1e999999999"); bad("2e19"); bad("1.8446744073709551599e19");
    bad("999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999");
}
/* ---- residue ---- */
typedef unsigned __int128 u128;
#define NQ 8
static const uint64_t t1_q[NQ] = {4611686018427388039ULL, 4611686018427388073ULL, 4611686018427388081ULL, 4611686018427388091ULL,   /* = verify.c t1_q */
                                  4611686018427388093ULL, 4611686018427388097ULL, 4611686018427388157ULL, 4611686018427388181ULL};
static void check_pair(uint64_t q, uint64_t b, uint64_t v, uint64_t x)
{
    uint64_t w = rs_w(b, q), ref = (uint64_t)(((u128)v * b + x) % q), got = rs_step(v, b, w, q, x);
    VERIFY(got == ref, "q=%llu b=%llu v=%llu x=%llu got=%llu ref=%llu", (unsigned long long)q, (unsigned long long)b, (unsigned long long)v, (unsigned long long)x, (unsigned long long)got, (unsigned long long)ref);
}
static void check_modulus(uint64_t q, rng_t *r)
{
    const uint64_t B = 1000000000000000000ULL, XM = B - 1;
    uint64_t bs[3] = {B % q, 10 % q, q - 1};
    uint64_t vs[6] = {0, 1, q - 1, q / 2, B % q, XM % q}, xs[5] = {0, 1, XM < q ? XM : q - 1, 9 % q, q - 1};
    for (int bi = 0; bi < 3; bi++) {
        uint64_t b = bs[bi];
        for (int i = 0; i < 6; i++) for (int j = 0; j < 5; j++) check_pair(q, b, vs[i], xs[j]);
        for (int it = 0; it < 20000; it++) {
            uint64_t v = rng_next(r) % q, x = rng_next(r) % (XM < q ? B : q);
            check_pair(q, b, v, x);
        }
        uint64_t w = rs_w(b, q), a = 0, c = 0;      /* a Horner chain, both forms */
        for (int it = 0; it < 1000000; it++) { uint64_t x = rng_next(r) % (XM < q ? B : q); a = rs_step(a, b, w, q, x); c = (uint64_t)(((u128)c * b + x) % q); }
        VERIFY(a == c, "chain q=%llu b=%llu", (unsigned long long)q, (unsigned long long)b);
    }
}
static void host_residue(void)
{
    rng_t r = {12345};
    for (int j = 0; j < NQ; j++) check_modulus(t1_q[j], &r);
    for (int it = 0; it < 200; it++) {
        uint64_t q = (rng_next(&r) >> 1 | ((uint64_t)1 << 40)) | 1;   /* in [2^40, 2^63), odd: the formula needs only b < q < 2^63 */
        check_modulus(q, &r);
    }
}
/* ---- nodemem ---- */
#define CHECK(c) VERIFY(c, "nodemem")
static void node(const char *root, int k, const char *body)
{
    char d[512], p[560]; snprintf(d, sizeof d, "%s/sys", root); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/sys/devices", root); mkdir(d, 0755); snprintf(d, sizeof d, "%s/sys/devices/system", root); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/sys/devices/system/node", root); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/sys/devices/system/node/node%d", root, k); mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/meminfo", d); FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); } fputs(body, f); fclose(f);
}
static void host_nodemem(void)
{
    char root[] = "/tmp/t_host_XXXXXX"; if (!mkdtemp(root)) { perror("mkdtemp"); exit(2); }
    node(root, 0, "Node 0 MemTotal:       130880096 kB\nNode 0 MemFree:         43073848 kB\nNode 0 MemUsed:         87806248 kB\n");
    node(root, 1, "Node 1 MemTotal:       131809008 kB\nNode 1 MemFree:        108302508 kB\n");
    node(root, 4, "Node 4 MemTotal:       130880096 kB\nNode 4 MemFree:         10000000 kB\nNode 4 Active(anon):     5000000 kB\nNode 4 Inactive(anon):   1000000 kB\nNode 4 Active(file):    20000000 kB\nNode 4 Inactive(file):  30000000 kB\nNode 4 Mapped:           9000000 kB\n");   /* file cache present */
    node(root, 2, "Node 2 MemTotal:       131762524 kB\n");   /* no MemFree line */
    size_t b = 0;
    CHECK(nodemem_free(root, 0, &b) && b == (size_t)43073848 * 1024);
    CHECK(nodemem_free(root, 1, &b) && b == (size_t)108302508 * 1024);
    size_t mf = 1, fl = 1;   /* MEMFIT: MemFree + Active(file) + Inactive(file) */
    CHECK(nodemem_read(root, 4, &mf, &fl) && mf == (size_t)10000000 * 1024 && fl == (size_t)50000000 * 1024);
    CHECK(nodemem_free(root, 4, &b) && b == (size_t)10000000 * 1024);                      /* the MemFree-only reader ignores the file lines */
    CHECK(nodemem_read(root, 0, &mf, &fl) && mf == (size_t)43073848 * 1024 && fl == 0);   /* no file lines = 0 */
    CHECK(nodemem_read(root, 1, 0, 0) && !nodemem_read(root, 2, &mf, &fl) && !nodemem_read(root, 9, &mf, &fl));
    CHECK(!nodemem_free(root, 2, &b)); CHECK(!nodemem_free(root, 3, &b));
    CHECK(!nodemem_free("/nonexistent_t_nodemem", 0, &b));
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); VERIFY(!system(cmd), "rm");
}
/* ---- hosttl ---- */
static const char *const nm[3] = { "other", "layeq", "addsh" };
static void host_hosttl(void)
{
    htl_t h; htl_init(&h, 1);
    htl_post(&h, 1, 10.0); htl_wait(&h, 1, 0.5); htl_end(&h, 12.0);     /* first post: no gap */
    htl_post(&h, 1, 15.0); htl_wait(&h, 1, 1.0); htl_end(&h, 16.0);     /* gap 3 to layeq */
    htl_post(&h, 2, 16.5); htl_post(&h, 1, 17.0); htl_end(&h, 17.5); htl_end(&h, 18.0);   /* gap .5 to addsh; second post overlaps */
    htl_post(&h, 9, 20.0); htl_end(&h, 21.0);                           /* a tag out of range counts as 0 (gap 2) */
    VERIFY(h.cnt[1] == 1 && fabs(h.host[1] - 3.0) < 1e-12, "layeq gap");
    VERIFY(h.cnt[2] == 1 && fabs(h.host[2] - 0.5) < 1e-12, "addsh gap");
    VERIFY(h.cnt[0] == 1 && fabs(h.host[0] - 2.0) < 1e-12, "bad tag -> other");
    VERIFY(h.ovl[1] == 1 && h.ovl[2] == 0, "overlap counted, no gap");
    VERIFY(fabs(h.wait[1] - 1.5) < 1e-12 && fabs(h.wmax[1] - 1.0) < 1e-12 && fabs(h.hmax[1] - 3.0) < 1e-12, "wait and maxima");
    double span = h.last_end - h.first; VERIFY(fabs(span - h.busy - (h.host[0] + h.host[1] + h.host[2])) < 1e-12, "span = busy + host");
    VERIFY(h.npend == 0, "all ended");
    htl_t o; htl_init(&o, 0); htl_post(&o, 1, 1.0); htl_wait(&o, 1, 1.0); htl_end(&o, 2.0);
    VERIFY(o.first == 0 && o.busy == 0 && o.cnt[1] == 0 && o.wait[1] == 0, "off: nothing recorded");
    char *buf = 0; size_t bl = 0; FILE *f = open_memstream(&buf, &bl); htl_print(f, &h, nm, 3, 7, 22.0); fclose(f);
    VERIFY(strstr(buf, "hosttl pe 7: layeq") && strstr(buf, "TOTAL") && strstr(buf, "check 0.000000") && strstr(buf, "tail (last end to report) 1.000"), "table");
    free(buf);
}
/* ---- dr ---- */
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
static void t_validate(void)                         /* ECALC_TEST: the whitelist, the grammar, the list values */
{
    char m[220]; long v[8];
    const char *ok[] = { "", "dr=flag,dr_k=3,dr_pe=-1", "dr=leaf+leafslow", "p24=2,p24_k=3", "lbench=26+28+30", "strat_budget_gb=1.5", "mapd_die=x" };
    for (unsigned i = 0; i < sizeof ok / sizeof *ok; i++) VERIFY(!ec_test_problem(ok[i], m, sizeof m), "accepted: %s (%s)", ok[i], m);
    VERIFY(!ec_test_problem(NULL, m, sizeof m), "unset");
    const char *bad[] = { "bogus=1", "dr=flag,dr=flag", "dr_k=x", "dr_k=1 ", "dr_k", "dr_k=", "=1", "dr=flag+deliver", "dr=nope", "lbench=26,28", "lbench=26+x", "dr_k=1,,dr_n=1", "p24=1,p24=2" };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) { m[0] = 0; VERIFY(ec_test_problem(bad[i], m, sizeof m) && m[0], "rejected: %s", bad[i]); }
    ec_test_problem("bogus=1", m, sizeof m); VERIFY(strstr(m, "bogus"), "the message names the key: %s", m);
    ec_test_problem("dr_k=x", m, sizeof m); VERIFY(strstr(m, "dr_k"), "the message names the key: %s", m);
    T("lbench=26+28+30"); VERIFY(ec_test_list("lbench", v, 8) == 3 && v[0] == 26 && v[1] == 28 && v[2] == 30, "list parse");
    T("lbench=26"); VERIFY(ec_test_list("lbench", v, 8) == 1 && v[0] == 26, "list of one");
    T(NULL); VERIFY(ec_test_list("lbench", v, 8) == 0, "list unset"); T(NULL);
}
static void host_dr(void)
{
    rng_t r = { 20261101 };
    t_prime(); t_arith(&r); t_limbs(&r); t_m(&r); t_cyc(&r); t_inject(); t_validate();
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*fn)(void); } T[] = { {"args", host_args}, {"residue", host_residue}, {"nodemem", host_nodemem}, {"hosttl", host_hosttl}, {"dr", host_dr} };
    int ran = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        int want = argc < 2;
        for (int a = 1; a < argc; a++) if (!strcmp(argv[a], T[i].name)) want = 1;
        if (want) { printf("t_host %s\n", T[i].name); T[i].fn(); ran++; }
    }
    if (!ran) { fprintf(stderr, "usage: t_host [args|residue|nodemem|hosttl|dr ...]\n"); return 2; }
    return verify_done("t_host");
}
