/* t_p24 - Phase 15 Batch 3, agent P24 (results/P2415.md): the P24 maps, the regroup (limbs -> 24-digit points -> residues) and the
 * ungroup (the 24-digit CRT's shifted digits, the stripes' carries and 4-limb spills) against slow references (GMP, brute force).
 *   host part (gcc on a login node, or the hipcc build):  gcc -O2 -I. tests/t_p24.c -o /tmp/t_p24 -lgmp -lm
 *   device part (the hipcc build, `make tests/t_p24`, on a node): k_crt24 (p24_crt.c) against the host model of the same stripe,
 *   random and boundary coefficients, and its bound guard (a coefficient past 10^60 at an s = 12 point raises the flag).
 * VERIFY OK / FAILED as the other tests. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <gmp.h>
#include "../p24.h"
#if defined(__HIPCC__) || defined(__HIP__)
#include "../rns_int.h"
#define P24_DEVICE 1
#else
#include <stdarg.h>
void ec_fatal(int code, const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); exit(code); }
#endif

static int fails, checks;
#define VERIFY(c, ...) do { checks++; if (!(c)) { if (fails++ < 30) { printf("  FAILED %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t rs = 0x243F6A8885A308D3ULL;
static uint64_t rnd(void) { uint64_t z = (rs += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
static const uint64_t B = 1000000000000000000ULL;
static size_t part0(size_t R, int nr, int rho) { return R * (size_t)rho / (size_t)nr; }

/* ---- A. the maps against brute force ------------------------------------------------------------------------------------ */
static void test_maps(size_t R, size_t C, int nr, int exhaustive)
{
    size_t N = R * C, Lend = p24_L(N);
    /* the out-runs tile the limbs, and the spill blocks (brute) */
    unsigned char *cov = (unsigned char *)calloc(Lend + 16, 1);
    int *sp_r = (int *)malloc((Lend + 16) * sizeof(int)); size_t *sp_j = (size_t *)malloc((Lend + 16) * sizeof(size_t)), *sp_t = (size_t *)malloc((Lend + 16) * sizeof(size_t));
    for (size_t i = 0; i < Lend + 16; i++) sp_r[i] = -1;
    int bad_tile = 0, bad_seq = 0, bad_start = 0;
    for (int rho = 0; rho < nr; rho++) {
        size_t a = part0(R, nr, rho), r = part0(R, nr, rho + 1) - a, a1 = part0(R, nr, rho + 1);
        for (int ex = 0; ex < 2; ex++) {
            struct p24_run m = p24_run_make(R, C, a, r, ex);
            size_t tot = 0; for (size_t j = 0; j < C; j++) tot += p24_L(R * j + a + r) + ex - p24_L(R * j + a);
            VERIFY(p24_run_total(&m) == tot, "R %zu C %zu nr %d rho %d ex %d: total %zu vs %zu", R, C, nr, rho, ex, p24_run_total(&m), tot);
            size_t *seq = (size_t *)malloc((tot + 1) * sizeof(size_t)), t = 0;
            for (size_t j = 0; j < C; j++) {
                VERIFY(p24_run_S(&m, j) == t, "S(%zu) %zu vs %zu (R %zu nr %d rho %d ex %d)", j, p24_run_S(&m, j), t, R, nr, rho, ex);
                for (size_t l = p24_L(R * j + a); l < p24_L(R * j + a + r) + ex; l++) { seq[t++] = l; if (!ex) { if (cov[l]) bad_tile++; cov[l]++; } }
            }
            for (size_t u = 0; u < tot; u++) { size_t col; if (p24_seq_limb(&m, u, &col) != seq[u]) bad_seq++; }
            /* seq_start(l) = the first index with seq >= l */
            size_t p = 0, step = exhaustive ? 1 : 7;
            for (size_t l = 0; l < Lend + 8; l += step) {
                while (p < tot && seq[p] < l) p++;
                if (p24_seq_start(&m, l) != p) { if (!bad_start) printf("  seq_start(%zu) = %zu, want %zu (R %zu nr %d rho %d ex %d)\n", l, p24_seq_start(&m, l), p, R, nr, rho, ex); bad_start++; }
            }
            free(seq);
        }
        for (size_t j = 0; j < C; j++) {                      /* rank rho's spill of column j: [L(R j + a1), + 4) */
            size_t b0 = p24_L(R * j + a1);
            for (size_t u = 0; u < 4; u++) { if (sp_r[b0 + u] >= 0) bad_tile++; sp_r[b0 + u] = rho; sp_j[b0 + u] = j; sp_t[b0 + u] = u; }
        }
    }
    size_t holes = 0; for (size_t l = 0; l < Lend; l++) if (cov[l] != 1) holes++;
    VERIFY(!bad_tile && !holes, "R %zu C %zu nr %d: the out-runs tile [0, %zu): %d overlaps, %zu holes", R, C, nr, Lend, bad_tile, holes);
    VERIFY(!bad_seq, "R %zu C %zu nr %d: seq_limb %d wrong", R, C, nr, bad_seq);
    VERIFY(!bad_start, "R %zu C %zu nr %d: seq_start %d wrong", R, C, nr, bad_start);
    int bad_sp = 0;
    for (size_t l = 0; l < Lend + 12; l++) {
        int rho = -9; size_t j = 0, t = 0; int hit = p24_spill_at(l, R, C, nr, &rho, &j, &t);
        int want = sp_r[l] >= 0;
        if (hit != want || (hit && (rho != sp_r[l] || j != sp_j[l] || t != sp_t[l]))) { if (!bad_sp) printf("  spill_at(%zu): %d (%d, %zu, %zu), want %d (%d, %zu, %zu)\n", l, hit, rho, j, t, want, sp_r[l], sp_j[l], sp_t[l]); bad_sp++; }
    }
    VERIFY(!bad_sp, "R %zu C %zu nr %d: spill_at %d wrong", R, C, nr, bad_sp);
    int bad_cols = 0;
    for (int it = 0; it < 400; it++) {                        /* windows: random, and at the blocks' edges */
        size_t lo = rnd() % (Lend + 8), hi = lo + rnd() % (it & 1 ? 50 : Lend / 3 + 1);
        if (it % 5 == 0) { size_t jj = rnd() % C; lo = p24_L(R * jj + part0(R, nr, 1 + (int)(rnd() % (size_t)nr))) + (size_t)(rnd() % 5) - 1; if (lo > Lend) lo = 0; hi = lo + (size_t)(rnd() % 6); }
        for (int rho = 0; rho < nr; rho++) {
            size_t a1 = part0(R, nr, rho + 1), j0, j1, b0 = C, b1 = 0; p24_spill_cols(lo, hi, a1, R, C, &j0, &j1);
            for (size_t j = 0; j < C; j++) { size_t s0 = p24_L(R * j + a1); if (s0 + 4 > lo && s0 < hi) { if (j < b0) b0 = j; b1 = j + 1; } }
            if (b1 == 0) { if (j1 != j0) bad_cols++; } else if (j0 != b0 || j1 != b1) { if (!bad_cols) printf("  spill_cols [%zu, %zu) a1 %zu: [%zu, %zu), want [%zu, %zu)\n", lo, hi, a1, j0, j1, b0, b1); bad_cols++; }
        }
    }
    VERIFY(!bad_cols, "R %zu C %zu nr %d: spill_cols %d wrong", R, C, nr, bad_cols);
    free(cov); free(sp_r); free(sp_j); free(sp_t);
}

/* ---- B. the regroup: limbs -> points -> residues ------------------------------------------------------------------------ */
static void digits_of(const uint64_t *l, size_t n, char *s)        /* s[k] = decimal digit k (least significant first), 18 n of them */
{
    for (size_t i = 0; i < n; i++) { uint64_t v = l[i]; for (int k = 0; k < 18; k++) { s[18 * i + k] = (char)(v % 10); v /= 10; } }
}
static void test_regroup(size_t n, int kind)
{
    uint64_t *l = (uint64_t *)malloc((n + 2) * 8); char *s = (char *)malloc(18 * (n + 2) + 48);
    for (size_t i = 0; i < n; i++) l[i] = kind == 0 ? rnd() % B : kind == 1 ? B - 1 : kind == 2 ? (i & 1 ? B - 1 : 0) : (rnd() & 1 ? B - 1 : rnd() % 1000);
    l[n] = l[n + 1] = 0;
    memset(s, 0, 18 * (n + 2) + 48); digits_of(l, n, s);
    size_t P = p24_pts(n); int bad = 0, badm = 0;
    VERIFY(P * 24 >= n * 18 && (P == 0 || (P - 1) * 24 < n * 18), "n %zu: %zu points", n, P);
    for (size_t x = 0; x <= P; x++) {
        unsigned __int128 ref = 0; for (int k = 23; k >= 0; k--) ref = ref * 10 + (unsigned)s[24 * x + k];   /* digits beyond 18 n are 0 */
        size_t L = p24_L(x); int sh = p24_s(x); uint64_t lo = L < n ? l[L] : 0, hi = L + 1 < n ? l[L + 1] : 0, pl, ph;
        p24_parts(lo, hi, sh, &pl, &ph);
        unsigned __int128 v = (unsigned __int128)pl + (unsigned __int128)ph * p24_p10(18 - sh);
        if (v != ref) bad++;
        for (int p = 0; p < 4; p++) {
            uint64_t c[3]; p24_c18(p, c);
            if (p24_point_mod(lo, hi, sh, ec_mod_get(p), c[sh / 6]) != (uint64_t)(ref % ec_P[p])) badm++;
        }
    }
    VERIFY(!bad && !badm, "regroup n %zu kind %d: %d points wrong, %d residues wrong", n, kind, bad, badm);
    free(l); free(s);
}

/* ---- C. the ungroup: coefficients -> limbs by stripes (the host model of k_crt24) against GMP --------------------------- */
/* the four base-B digits of c (c < 10^72) */
static void dec4(const mpz_t c, uint64_t d[4])
{
    mpz_t t, q; mpz_init_set(t, c); mpz_init(q);
    for (int k = 0; k < 4; k++) { d[k] = mpz_fdiv_q_ui(q, t, 1000000000ULL); uint64_t lo = d[k]; d[k] = mpz_fdiv_q_ui(t, q, 1000000000ULL); d[k] = d[k] * 1000000000ULL + lo; }
    mpz_clear(t); mpz_clear(q);
}
/* one column of rank (a, rows): its out-run limbs (len) and its 4-limb spill; returns the fifth-limb / spill-carry flag as k_crt24 */
static unsigned model_stripe(mpz_t *coef, size_t R, size_t j, size_t a, size_t rows, uint64_t *own, uint64_t sp[4])
{
    size_t x0 = R * j + a, A0 = p24_L(x0), len = p24_L(x0 + rows) - A0; unsigned err = 0;
    unsigned __int128 *acc = (unsigned __int128 *)calloc(len + 8, sizeof *acc);
    for (size_t k = 0; k < rows; k++) {
        size_t x = x0 + k; uint64_t d[4], e[4]; dec4(coef[x], d);
        if (p24_shift_digits(d, p24_s(x), e)) err |= 1;
        for (int u = 0; u < 4; u++) acc[p24_L(x) - A0 + u] += e[u];
    }
    unsigned __int128 cy = 0;
    for (size_t i = 0; i < len + 4; i++) { unsigned __int128 v = acc[i] + cy; cy = v / B; uint64_t dgt = (uint64_t)(v % B); if (i < len) own[i] = dgt; else sp[i - len] = dgt; }
    if (cy) err |= 2;
    free(acc); return err;
}
/* the whole plane (nr ranks x C columns) through the model: the out-runs placed by seq_limb, the spills added at L(R j + a1); against
 * sum c_x 10^(24 x) */
static void test_ungroup(size_t R, size_t C, int nr, int kind)
{
    size_t N = R * C, Lend = p24_L(N);
    mpz_t *coef = (mpz_t *)malloc(N * sizeof(mpz_t)), bound, ref, got, t;
    mpz_init(bound); mpz_ui_pow_ui(bound, 10, 60); mpz_init(ref); mpz_init(got); mpz_init(t);
    for (size_t x = 0; x < N; x++) {
        mpz_init(coef[x]);
        if (kind == 0) { mpz_set_ui(coef[x], rnd()); for (int k = 0; k < 3; k++) { mpz_mul_2exp(coef[x], coef[x], 64); mpz_add_ui(coef[x], coef[x], rnd()); } mpz_mod(coef[x], coef[x], bound); }
        else if (kind == 1) mpz_sub_ui(coef[x], bound, 1);                                   /* 10^60 - 1 everywhere: every limb carries */
        else if (kind == 2) { if (rnd() % 3 == 0) mpz_sub_ui(coef[x], bound, 1 + rnd() % 7); else mpz_set_ui(coef[x], x % 5 == 0 ? 0 : rnd() % 1000); }
        else { mpz_ui_pow_ui(coef[x], 10, 24); mpz_sub_ui(coef[x], coef[x], 1); mpz_mul(coef[x], coef[x], coef[x]); mpz_mul_ui(coef[x], coef[x], 1 + (rnd() % 900000)); }   /* k (10^24 - 1)^2 */
    }
    uint64_t *limbs = (uint64_t *)calloc(Lend + 16, 8); unsigned err = 0;
    for (int rho = 0; rho < nr; rho++) {
        size_t a = part0(R, nr, rho), rows = part0(R, nr, rho + 1) - a, a1 = part0(R, nr, rho + 1);
        struct p24_run m = p24_run_make(R, C, a, rows, 0);
        for (size_t j = 0; j < C; j++) {
            size_t len = m.len[j % 3]; uint64_t *own = (uint64_t *)malloc(len * 8), sp[4];
            err |= model_stripe(coef, R, j, a, rows, own, sp);
            for (size_t i = 0; i < len; i++) { size_t col; size_t l = p24_seq_limb(&m, p24_run_S(&m, j) + i, &col); limbs[l] = own[i]; }
            free(own);
            /* the spill: added to the number (GMP) at limb L(R j + a1) */
            size_t b0 = p24_L(R * j + a1);
            mpz_set_ui(t, 0); for (int u = 3; u >= 0; u--) { mpz_mul_ui(t, t, 1000000000ULL); mpz_mul_ui(t, t, 1000000000ULL); mpz_add_ui(t, t, sp[u]); }
            { mpz_t p; mpz_init(p); mpz_ui_pow_ui(p, 10, 18 * b0); mpz_mul(t, t, p); mpz_add(got, got, t); mpz_clear(p); }
        }
    }
    { mpz_set_ui(t, 0); for (size_t l = Lend; l-- > 0; ) { mpz_mul_ui(t, t, 1000000000ULL); mpz_mul_ui(t, t, 1000000000ULL); mpz_add_ui(t, t, limbs[l]); } mpz_add(got, got, t); }
    { mpz_t p; mpz_init(p); for (size_t x = N; x-- > 0; ) { mpz_ui_pow_ui(p, 10, 24); mpz_mul(ref, ref, p); mpz_add(ref, ref, coef[x]); } mpz_clear(p); }
    VERIFY(!err && mpz_cmp(got, ref) == 0, "ungroup R %zu C %zu nr %d kind %d: %s (flags %u)", R, C, nr, kind, mpz_cmp(got, ref) ? "the limbs differ from sum c_x 10^(24x)" : "equal", err);
    for (size_t x = 0; x < N; x++) mpz_clear(coef[x]);
    free(coef); free(limbs); mpz_clear(bound); mpz_clear(ref); mpz_clear(got); mpz_clear(t);
}
/* a regrouped product end to end on the host: A, B (limbs) -> points -> the cyclic-free convolution (GMP coefficients) -> the
 * model's stripes -> limbs; against A B */
static void test_product(size_t na, size_t nb, int kind, size_t R, int nr)
{
    uint64_t *a = (uint64_t *)calloc(na + 2, 8), *b = (uint64_t *)calloc(nb + 2, 8);
    for (size_t i = 0; i < na; i++) a[i] = kind ? B - 1 : rnd() % B;
    for (size_t i = 0; i < nb; i++) b[i] = kind ? B - 1 : rnd() % B;
    size_t Pa = p24_pts(na), Pb = p24_pts(nb), n = Pa + Pb, C = (n + R - 1) / R; if (C < 1) C = 1;
    mpz_t *pa = (mpz_t *)malloc(Pa * sizeof(mpz_t)), *pb = (mpz_t *)malloc(Pb * sizeof(mpz_t)), *c = (mpz_t *)malloc(R * C * sizeof(mpz_t));
    for (size_t x = 0; x < Pa; x++) { uint64_t pl, ph; size_t L = p24_L(x); p24_parts(L < na ? a[L] : 0, L + 1 < na ? a[L + 1] : 0, p24_s(x), &pl, &ph); mpz_init_set_ui(pa[x], ph); mpz_mul_ui(pa[x], pa[x], p24_p10(18 - p24_s(x))); mpz_add_ui(pa[x], pa[x], pl); }
    for (size_t x = 0; x < Pb; x++) { uint64_t pl, ph; size_t L = p24_L(x); p24_parts(L < nb ? b[L] : 0, L + 1 < nb ? b[L + 1] : 0, p24_s(x), &pl, &ph); mpz_init_set_ui(pb[x], ph); mpz_mul_ui(pb[x], pb[x], p24_p10(18 - p24_s(x))); mpz_add_ui(pb[x], pb[x], pl); }
    for (size_t x = 0; x < R * C; x++) mpz_init(c[x]);
    for (size_t i = 0; i < Pa; i++) for (size_t k = 0; k < Pb; k++) mpz_addmul(c[i + k], pa[i], pb[k]);
    size_t Lend = p24_L(R * C); uint64_t *limbs = (uint64_t *)calloc(Lend + 16, 8); mpz_t got, ref, t, p; mpz_init(got); mpz_init(ref); mpz_init(t); mpz_init(p); unsigned err = 0;
    for (int rho = 0; rho < nr; rho++) {
        size_t a0 = part0(R, nr, rho), rows = part0(R, nr, rho + 1) - a0, a1 = part0(R, nr, rho + 1);
        struct p24_run m = p24_run_make(R, C, a0, rows, 0);
        for (size_t j = 0; j < C; j++) {
            size_t len = m.len[j % 3]; uint64_t *own = (uint64_t *)malloc(len * 8), sp[4];
            err |= model_stripe(c, R, j, a0, rows, own, sp);
            for (size_t i = 0; i < len; i++) limbs[p24_seq_limb(&m, p24_run_S(&m, j) + i, 0)] = own[i];
            free(own);
            mpz_set_ui(t, 0); for (int u = 3; u >= 0; u--) { mpz_mul_ui(t, t, 1000000000ULL); mpz_mul_ui(t, t, 1000000000ULL); mpz_add_ui(t, t, sp[u]); }
            mpz_ui_pow_ui(p, 10, 18 * p24_L(R * j + a1)); mpz_mul(t, t, p); mpz_add(got, got, t);
        }
    }
    mpz_set_ui(t, 0); for (size_t l = Lend; l-- > 0; ) { mpz_mul_ui(t, t, 1000000000ULL); mpz_mul_ui(t, t, 1000000000ULL); mpz_add_ui(t, t, limbs[l]); } mpz_add(got, got, t);
    { mpz_t A, Bm; mpz_init(A); mpz_init(Bm);
      for (size_t l = na; l-- > 0; ) { mpz_mul_ui(A, A, 1000000000ULL); mpz_mul_ui(A, A, 1000000000ULL); mpz_add_ui(A, A, a[l]); }
      for (size_t l = nb; l-- > 0; ) { mpz_mul_ui(Bm, Bm, 1000000000ULL); mpz_mul_ui(Bm, Bm, 1000000000ULL); mpz_add_ui(Bm, Bm, b[l]); }
      mpz_mul(ref, A, Bm); mpz_clear(A); mpz_clear(Bm); }
    VERIFY(!err && mpz_cmp(got, ref) == 0, "product %zu x %zu limbs (%zu + %zu points, R %zu, nr %d) %s", na, nb, Pa, Pb, R, nr, kind ? "all 9s" : "random");
    for (size_t x = 0; x < Pa; x++) mpz_clear(pa[x]);
    for (size_t x = 0; x < Pb; x++) mpz_clear(pb[x]);
    for (size_t x = 0; x < R * C; x++) mpz_clear(c[x]);
    free(pa); free(pb); free(c); free(a); free(b); free(limbs); mpz_clear(got); mpz_clear(ref); mpz_clear(t); mpz_clear(p);
}

#ifdef P24_DEVICE
/* ---- D. k_crt24 on the device against the host model ----------------------------------------------------------------------- */
#define HC(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { printf("HIP %s at %d\n", hipGetErrorString(e_), __LINE__); exit(3); } } while (0)
static void test_kernel(size_t R, size_t C, int nr, int rho, int kind)
{
    size_t a = part0(R, nr, rho), rows = part0(R, nr, rho + 1) - a, N = R * C;
    struct p24_run m = p24_run_make(R, C, a, rows, 0); size_t tot = p24_run_total(&m);
    mpz_t *coef = (mpz_t *)malloc(N * sizeof(mpz_t)), bound; mpz_init(bound); mpz_ui_pow_ui(bound, 10, 60);
    for (size_t x = 0; x < N; x++) {
        mpz_init(coef[x]);
        if (kind == 0) { mpz_set_ui(coef[x], rnd()); for (int k = 0; k < 3; k++) { mpz_mul_2exp(coef[x], coef[x], 64); mpz_add_ui(coef[x], coef[x], rnd()); } mpz_mod(coef[x], coef[x], bound); }
        else if (kind == 1) mpz_sub_ui(coef[x], bound, 1);
        else if (kind == 2) { if (rnd() % 2) mpz_sub_ui(coef[x], bound, 1 + rnd() % 3); else mpz_set_ui(coef[x], rnd() % 10); }
        else if (kind == 3) { mpz_set_ui(coef[x], 0); }
        else { mpz_sub_ui(coef[x], bound, 1); }                                              /* kind 4: + one coefficient past the bound (below) */
    }
    size_t bad_x = 0;
    if (kind == 4) { for (size_t k = 0; k < rows; k++) { size_t x = a + k; if (p24_s(x) == 12) { mpz_ui_pow_ui(coef[x], 10, 61); bad_x = x; break; } } }
    uint64_t *h[4], *d[4], *dout, *dsp, *hout = (uint64_t *)malloc((tot + 8) * 8), *hsp = (uint64_t *)malloc(C * 32);
    for (int p = 0; p < 4; p++) {
        h[p] = (uint64_t *)malloc(rows * C * 8);
        for (size_t j = 0; j < C; j++) for (size_t k = 0; k < rows; k++) h[p][j * rows + k] = mpz_fdiv_ui(coef[R * j + a + k], ec_P[p]);
        HC(hipMalloc(&d[p], rows * C * 8)); HC(hipMemcpy(d[p], h[p], rows * C * 8, hipMemcpyHostToDevice));
    }
    HC(hipMalloc(&dout, (tot + 8) * 8)); HC(hipMalloc(&dsp, C * 32)); HC(hipMemset(dout, 0xA5, (tot + 8) * 8));
    struct gconst gc = rns_gconst(); gc.np = 4;
    p24_crt_launch(d[0], d[1], d[2], d[3], &gc, R, C, a, rows, dout, dsp, 0);
    HC(hipDeviceSynchronize());
    unsigned flag = p24_crt_err(0);
    HC(hipMemcpy(hout, dout, (tot + 8) * 8, hipMemcpyDeviceToHost)); HC(hipMemcpy(hsp, dsp, C * 32, hipMemcpyDeviceToHost));
    size_t bad = 0; unsigned merr = 0;
    for (size_t j = 0; j < C; j++) {
        size_t len = m.len[j % 3], o = p24_run_S(&m, j); uint64_t *own = (uint64_t *)malloc(len * 8), sp[4];
        merr |= model_stripe(coef, R, j, a, rows, own, sp);
        for (size_t i = 0; i < len; i++) if (hout[o + i] != own[i]) bad++;
        for (int u = 0; u < 4; u++) if (hsp[4 * j + u] != sp[u]) bad++;
        free(own);
    }
    if (kind == 4) VERIFY(flag == 1 && (merr & 1), "k_crt24 guard: a coefficient 10^61 at the s = 12 point %zu: flag %u (model %u)", bad_x, flag, merr);
    else VERIFY(bad == 0 && flag == 0 && merr == 0, "k_crt24 R %zu C %zu nr %d rho %d (rows %zu, a %zu) kind %d: %zu limbs differ from the model, flag %u", R, C, nr, rho, rows, a, kind, bad, flag);
    for (int p = 0; p < 4; p++) { free(h[p]); HC(hipFree(d[p])); }
    HC(hipFree(dout)); HC(hipFree(dsp)); free(hout); free(hsp);
    for (size_t x = 0; x < N; x++) mpz_clear(coef[x]);
    free(coef); mpz_clear(bound);
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("t_p24: the maps\n");
    { size_t Rs[] = { 16, 24, 1024, 2048 }; int nrs[] = { 4, 8, 12, 24 };
      for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) { if (Rs[i] / nrs[k] < 3) continue; test_maps(Rs[i], Rs[i] >= 1024 ? 64 : 7, nrs[k], 1); }
      test_maps(1024, 1024, 8, 0); test_maps(2048, 1024, 24, 0); }
    printf("t_p24: the regroup\n");
    { size_t ns[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 100, 1001, 4096 };
      for (int i = 0; i < 12; i++) for (int kind = 0; kind < 4; kind++) test_regroup(ns[i], kind); }
    printf("t_p24: the ungroup (the host model of k_crt24's stripes, carries and spills)\n");
    for (int kind = 0; kind < 4; kind++) { test_ungroup(16, 3, 4, kind); test_ungroup(24, 4, 8, kind); test_ungroup(40, 3, 3, kind); test_ungroup(64, 2, 5, kind); }
    printf("t_p24: products through the regroup, the model's stripes and the spills\n");
    { size_t sz[][2] = { {1, 1}, {4, 4}, {5, 3}, {17, 9}, {40, 40}, {61, 7}, {100, 33} };
      for (int i = 0; i < 7; i++) for (int kind = 0; kind < 2; kind++) { test_product(sz[i][0], sz[i][1], kind, 16, 4); test_product(sz[i][0], sz[i][1], kind, 24, 5); } }
#ifdef P24_DEVICE
    printf("t_p24: k_crt24 on the device\n");
    { struct { size_t R, C; int nr; } cf[] = { {1024, 16, 8}, {1024, 16, 2}, {1024, 12, 3}, {2048, 9, 12}, {1024, 8, 1} };
      for (int i = 0; i < 5; i++) for (int kind = 0; kind < 4; kind++) for (int rho = 0; rho < cf[i].nr; rho += (cf[i].nr > 3 ? cf[i].nr / 3 : 1)) test_kernel(cf[i].R, cf[i].C, cf[i].nr, rho, kind);
      test_kernel(1024, 8, 4, 1, 4); }
#endif
    if (fails) printf("\nt_p24: VERIFY FAILED (%d of %d checks)\n", fails, checks); else printf("\nt_p24: VERIFY OK (%d checks)\n", checks);
    return fails != 0;
}
