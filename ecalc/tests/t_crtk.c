/* t_crtk - the device CRT kernels with the carry-lookahead (k_crt_batch in rns_mul.c, k_crt24 in p24_crt.c) against a host reference
 * (the plain base-B addition of the known coefficients, a __int128 carry loop on the host) or GMP.  Usage: t_crtk [batch|col24|flag ...] (no argument = all)
 *  batch  k_crt_batch, three and four primes: stripes of a product (the C form, ragged stripes, nc < S, the B form batch with an added
 *         operand x and the top carry limb); coefficient kinds 0 random, 1 long carry ripples (B - 1 with sparse 2B - 1), 2 the
 *         largest digits under the primes' product (every limb carries).
 *  col24  k_crt24: columns of a rank's plane (R, C, nr, rank rho: rows < 256, rows not a multiple of 256, every a mod 3, rows 1 and 3);
 *         coefficient kinds 0 random below 10^60, 1 ripples (10^24 - 1 with sparse 2 10^24 - 1), 2 a generator at each column's first
 *         point (the ripple crosses every chunk into the spill), 3 the bound 10^60 - 1, 4 a mix of 0, 10^24 - 1, 10^48 - 1, 10^60 - 1 and random.
 *  flag   k_crt24 against GMP (p24_crt_launch / p24_crt_err): in-bound digits equal and flag 0; one coefficient c >= 10^60 (to the primes' product - 1):
 *         the default form (p24_crt_set_check(1), P24_CRT_ANY=1) raises bit 4 exactly, the P24_CRT_ANY=0 form (set_check(0)) bit 1 only at an s = 12 point;
 *         p24_crt_err_pos is the planted point.
 */
#include "harness.h"
#include "../rns_int.h"
#include "../modarith.h"
#include "../p24.h"
#include <hip/hip_runtime.h>

#define HC(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(2); } } while (0)

typedef unsigned __int128 u128;
static const uint64_t B = EC_1E18;
static uint64_t mix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ULL; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31);
}
/* the residue of d[0] + d[1] B + d[2] B^2 + d[3] B^3 mod p */
static uint64_t resid(const uint64_t d[4], uint64_t p)
{
    u128 r = 0;
    for (int k = 3; k >= 0; k--) r = (r * B + d[k]) % p;
    return (uint64_t)r;
}

/* ---- A. k_crt_batch ---- */
/* coefficient k of np primes: digits below B, the top one capped so that the value stays under the product of the first np primes
 * (3 primes ~ 5.8e46: d[2] < 1e10; 4 primes ~ 4e62: d[3] < 1e8) */
static void coefA(int mode, int np, size_t k, uint64_t seed, uint64_t d[4])
{
    uint64_t h = mix64(seed ^ (k * 0x632BE59BD9B4E019ULL)), cap = np == 3 ? 10000000000ULL : 100000000ULL;
    d[0] = d[1] = d[2] = d[3] = 0;
    if (mode == 0) { for (int i = 0; i < np; i++) d[i] = mix64(h + 1 + i) % (i == np - 1 ? cap : B); }
    else if (mode == 1) { d[0] = B - 1; d[1] = h % 997 == 0 ? 1 : 0; }                    /* B - 1, or 2B - 1 */
    else { for (int i = 0; i < np - 1; i++) d[i] = B - 1; d[np - 1] = cap - 1; }
}
/* the host sum of one stripe [K0, K1) of a product: the limbs out[K0..K1), the spill sp[0..3] (the limbs K1 .. K1 + 2 and the carry after them) */
static void ref_stripe(const uint64_t (*cd)[4], const uint64_t *x, size_t nx, size_t K0, size_t K1, uint64_t *out, uint64_t sp[4])
{
    u128 carry = 0;
    for (size_t k = K0; k < K1 + 3; k++) {
        u128 sm = carry;
        for (size_t j = 0; j < 4; j++) if (k >= j + K0 && k - j < K1) sm += cd[k - j][j];
        if (k < K1 && k < nx) sm += x[k];
        if (k < K1) out[k] = (uint64_t)(sm % B); else sp[k - K1] = (uint64_t)(sm % B);
        carry = sm / B;
    }
    sp[3] = (uint64_t)carry;
}
/* M products; product i has nc[i] coefficients at plane offset i * Lpts, S stripes, an added operand of nx[i] limbs */
static void run_batch(int np, size_t M, const uint32_t *nc, const uint32_t *nx, size_t Lpts, int S, int mode, uint64_t seed, const char *tag)
{
    uint64_t *hp[4], *dp[4], *hx = 0, *dx = 0; size_t tot = 0, xtot = 0;
    size_t *oo = (size_t *)malloc(M * sizeof *oo), *xo = (size_t *)malloc(M * sizeof *xo);
    for (size_t i = 0; i < M; i++) { oo[i] = tot; tot += nc[i] + 1; xo[i] = xtot; xtot += nx[i]; }
    uint64_t (*cd)[4] = (uint64_t (*)[4])calloc(M * Lpts, sizeof *cd);
    for (int p = 0; p < 4; p++) hp[p] = (uint64_t *)calloc(M * Lpts, 8);
    for (size_t i = 0; i < M; i++)
        for (size_t k = 0; k < nc[i]; k++) {
            uint64_t *d = cd[i * Lpts + k]; coefA(mode, np, k, seed + 31 * i, d);
            for (int p = 0; p < np; p++) hp[p][i * Lpts + k] = resid(d, ec_P[p]);
        }
    for (int p = 0; p < 4; p++) { HC(hipMalloc(&dp[p], M * Lpts * 8)); HC(hipMemcpy(dp[p], hp[p], M * Lpts * 8, hipMemcpyHostToDevice)); }
    if (xtot) {
        hx = (uint64_t *)malloc(xtot * 8); for (size_t i = 0; i < xtot; i++) hx[i] = mix64(seed + 7 * i) % B;
        if (mode == 2) for (size_t i = 0; i < xtot; i++) hx[i] = B - 1;
        HC(hipMalloc(&dx, xtot * 8)); HC(hipMemcpy(dx, hx, xtot * 8, hipMemcpyHostToDevice));
    }
    struct bdesc *hd = (struct bdesc *)calloc(M, sizeof *hd), *dd; uint64_t *out, *sp;
    HC(hipMalloc(&out, tot * 8)); HC(hipMemset(out, 0x5A, tot * 8)); HC(hipMalloc(&sp, M * S * 32)); HC(hipMemset(sp, 0x3C, M * S * 32));
    for (size_t i = 0; i < M; i++) { hd[i].c = out + oo[i]; hd[i].na = nc[i] - nc[i] / 3; hd[i].nb = nc[i] / 3; hd[i].nx = nx[i]; hd[i].x = nx[i] ? dx + xo[i] : NULL; }
    HC(hipMalloc(&dd, M * sizeof *dd)); HC(hipMemcpy(dd, hd, M * sizeof *dd, hipMemcpyHostToDevice));
    struct gconst g = rns_gconst(); g.np = np;
    k_crt_batch<<<(unsigned)(M * S), CRT_THREADS>>>(dp[0], dp[1], dp[2], dp[3], dd, 0, S, Lpts, g, sp);
    HC(hipDeviceSynchronize());
    uint64_t *ho = (uint64_t *)malloc(tot * 8), *hs = (uint64_t *)malloc(M * S * 32);
    HC(hipMemcpy(ho, out, tot * 8, hipMemcpyDeviceToHost)); HC(hipMemcpy(hs, sp, M * S * 32, hipMemcpyDeviceToHost));
    size_t bad = 0, ripple = 0; uint64_t *eo = (uint64_t *)malloc((size_t)(Lpts + 1) * 8);
    for (size_t i = 0; i < M; i++) {
        for (int s = 0; s < S; s++) {
            size_t K0 = (size_t)((uint64_t)nc[i] * s / S), K1 = (size_t)((uint64_t)nc[i] * (s + 1) / S); uint64_t esp[4];
            ref_stripe((const uint64_t (*)[4])(cd + i * Lpts), hx ? hx + xo[i] : NULL, nx[i], K0, K1, eo, esp);
            for (size_t k = K0; k < K1; k++) { bad += ho[oo[i] + k] != eo[k]; ripple += eo[k] == B - 1 || eo[k] == 0; }
            for (int u = 0; u < 4; u++) bad += hs[(i * S + s) * 4 + u] != esp[u];
            if (nx[i] && s == S - 1) bad += ho[oo[i] + nc[i]] != esp[0];             /* the top limb: the final carry */
        }
    }
    printf("  %-34s np %d M %3zu S %5d mode %d: %s (limbs %zu, B-1/0 limbs %zu)\n", tag, np, M, S, mode, bad ? "DIFFER" : "ok", tot, ripple);
    VERIFY(bad == 0, "%s np %d M %zu S %d mode %d: %zu words differ from the host sum (limbs %zu)", tag, np, M, S, mode, bad, tot);
    HC(hipFree(out)); HC(hipFree(sp)); HC(hipFree(dd)); if (dx) HC(hipFree(dx));
    for (int p = 0; p < 4; p++) { HC(hipFree(dp[p])); free(hp[p]); }
    free(cd); free(hx); free(hd); free(oo); free(xo); free(ho); free(hs); free(eo);
}

/* ---- B. k_crt24 ---- */
/* the coefficient of point x as four base-B digits (d[3] < 10^6 keeps it below 10^60) */
static void coefB(int mode, size_t x, size_t xfirst, uint64_t seed, uint64_t d[4])
{
    uint64_t h = mix64(seed ^ (x * 0x632BE59BD9B4E019ULL));
    d[0] = d[1] = d[2] = d[3] = 0;
    switch (mode) {
    case 0: d[0] = mix64(h + 1) % B; d[1] = mix64(h + 2) % B; d[2] = mix64(h + 3) % B; d[3] = mix64(h + 4) % 1000000; break;
    case 1: d[0] = B - 1; d[1] = (h % 997 == 0) ? 1999999 : 999999; break;                 /* 10^24 - 1, or 2 10^24 - 1 */
    case 2: d[0] = B - 1; d[1] = x == xfirst ? 1999999 : 999999; break;
    case 3: d[0] = d[1] = d[2] = B - 1; d[3] = 999999; break;                                /* 10^60 - 1 */
    default: switch (h % 5) {
        case 0: break;
        case 1: d[0] = B - 1; d[1] = 999999; break;
        case 2: d[0] = d[1] = B - 1; d[2] = 999999999999ULL; break;                          /* 10^48 - 1 */
        case 3: d[0] = d[1] = d[2] = B - 1; d[3] = 999999; break;
        default: d[0] = mix64(h + 1) % B; d[1] = mix64(h + 2) % B; d[2] = mix64(h + 3) % B; d[3] = mix64(h + 4) % 1000000; }
    }
}
/* rank rho of nr over R, C columns */
static void run24(size_t R, size_t C, int nr, int rho, int mode, uint64_t seed)
{
    size_t a = R * (size_t)rho / (size_t)nr, rows = R * (size_t)(rho + 1) / (size_t)nr - a, q = rows * C;
    struct p24_run m = p24_run_make(R, C, a, rows, 0); size_t tot = p24_run_total(&m) + 8;
    uint64_t *h[4], *dp[4]; uint64_t (*cd)[4] = (uint64_t (*)[4])malloc(q * sizeof *cd);
    for (int p = 0; p < 4; p++) h[p] = (uint64_t *)malloc(q * 8);
    for (size_t j = 0; j < C; j++)
        for (size_t k = 0; k < rows; k++) {
            uint64_t *d = cd[j * rows + k]; coefB(mode, R * j + a + k, R * j + a, seed, d);
            for (int p = 0; p < 4; p++) h[p][j * rows + k] = resid(d, ec_P[p]);
        }
    for (int p = 0; p < 4; p++) { HC(hipMalloc(&dp[p], q * 8)); HC(hipMemcpy(dp[p], h[p], q * 8, hipMemcpyHostToDevice)); }
    uint64_t *out, *sp; struct gconst gc = rns_gconst(); gc.np = 4;
    HC(hipMalloc(&out, tot * 8)); HC(hipMemset(out, 0xA5, tot * 8)); HC(hipMalloc(&sp, C * 32)); HC(hipMemset(sp, 0x3C, C * 32));
    (void)p24_crt_err(0);
    p24_crt_launch(dp[0], dp[1], dp[2], dp[3], &gc, R, C, a, rows, out, sp, 0);
    HC(hipDeviceSynchronize()); unsigned fl = p24_crt_err(0);
    uint64_t *ho = (uint64_t *)malloc(tot * 8), *hs = (uint64_t *)malloc(C * 32);
    HC(hipMemcpy(ho, out, tot * 8, hipMemcpyDeviceToHost)); HC(hipMemcpy(hs, sp, C * 32, hipMemcpyDeviceToHost));
    size_t bad = 0, nines = 0;
    for (size_t j = 0; j < C; j++) {                                   /* the host sum of column j: points at limbs L(x) .. L(x) + 3, one carry pass */
        size_t x0 = R * j + a, A0 = p24_L(x0), L1 = p24_L(x0 + rows), n = L1 + 4 - A0, ob = p24_run_S(&m, j);
        u128 *acc = (u128 *)calloc(n, sizeof *acc);
        for (size_t k = 0; k < rows; k++) {
            uint64_t e[4]; size_t x = x0 + k; p24_shift_digits(cd[j * rows + k], p24_s(x), e);
            for (int t = 0; t < 4; t++) acc[p24_L(x) - A0 + t] += e[t];
        }
        u128 carry = 0;
        for (size_t l = 0; l < n; l++) {
            u128 sm = acc[l] + carry; uint64_t v = (uint64_t)(sm % B); carry = sm / B;
            if (l < L1 - A0) { bad += ho[ob + l] != v; nines += v == B - 1; } else bad += hs[4 * j + (l - (L1 - A0))] != v;
        }
        bad += carry != 0; free(acc);
    }
    printf("  R %6zu C %3zu nr %d rho %d (rows %5zu, a mod 3 = %zu) mode %d: %s (limbs %zu, B-1 limbs %zu, flag %u)\n",
           R, C, nr, rho, rows, a % 3, mode, bad || fl ? "DIFFER" : "ok", tot - 8, nines, fl);
    VERIFY(bad == 0, "R %zu C %zu nr %d rho %d mode %d: %zu words differ from the host sum", R, C, nr, rho, mode, bad);
    VERIFY(fl == 0, "R %zu C %zu nr %d rho %d mode %d: device flag %u below the bound", R, C, nr, rho, mode, fl);
    HC(hipFree(out)); HC(hipFree(sp)); free(ho); free(hs); free(cd);
    for (int p = 0; p < 4; p++) { HC(hipFree(dp[p])); free(h[p]); }
}

/* ---- C. k_crt24 against GMP: the digits, and the bound flag exactly at c >= 10^60 (both check forms) ---- */
static uint64_t rs = 0x13198A2E03707344ULL;
static uint64_t rnd(void) { uint64_t r = mix64(rs); rs += 0x9E3779B97F4A7C15ULL; return r; }
static size_t part0(size_t R, int nr, int rho) { return R * (size_t)rho / (size_t)nr; }

struct cfg { size_t R, C, a, rows, N, tot; struct p24_run m; };
static struct cfg mk(size_t R, size_t C, int nr, int rho)
{
    struct cfg f; f.R = R; f.C = C; f.a = part0(R, nr, rho); f.rows = part0(R, nr, rho + 1) - f.a; f.N = R * C;
    f.m = p24_run_make(R, C, f.a, f.rows, 0); f.tot = p24_run_total(&f.m); return f;
}
static void rand_below(mpz_t o, const mpz_t bound)
{
    mpz_set_ui(o, rnd()); for (int k = 0; k < 4; k++) { mpz_mul_2exp(o, o, 64); mpz_add_ui(o, o, rnd()); }
    mpz_mod(o, o, bound);
}
/* run the kernel on coef[x] (indexed by the plane point x = R j + a + k; only this rank's points are used); returns the flag, checks the digits when digits != 0 */
static size_t g_pos;                                  /* p24_crt_err_pos() of the last run that flagged, else -1 */
static unsigned run_flag(const struct cfg *f, mpz_t *coef, int chk, int digits, const char *what)
{
    uint64_t *h[4], *d[4], *dout, *dsp, *hout = (uint64_t *)malloc((f->tot + 8) * 8), *hsp = (uint64_t *)malloc(f->C * 32);
    for (int p = 0; p < 4; p++) {
        h[p] = (uint64_t *)malloc(f->rows * f->C * 8);
        for (size_t j = 0; j < f->C; j++) for (size_t k = 0; k < f->rows; k++) h[p][j * f->rows + k] = mpz_fdiv_ui(coef[f->R * j + f->a + k], ec_P[p]);
        HC(hipMalloc(&d[p], f->rows * f->C * 8)); HC(hipMemcpy(d[p], h[p], f->rows * f->C * 8, hipMemcpyHostToDevice));
    }
    HC(hipMalloc(&dout, (f->tot + 8) * 8)); HC(hipMalloc(&dsp, f->C * 32)); HC(hipMemset(dout, 0xA5, (f->tot + 8) * 8)); HC(hipMemset(dsp, 0xA5, f->C * 32));
    struct gconst gc = rns_gconst(); gc.np = 4;
    p24_crt_set_check(chk);
    p24_crt_launch(d[0], d[1], d[2], d[3], &gc, f->R, f->C, f->a, f->rows, dout, dsp, 0);
    HC(hipDeviceSynchronize());
    unsigned flag = p24_crt_err(0); g_pos = flag ? p24_crt_err_pos() : (size_t)-1;
    HC(hipMemcpy(hout, dout, (f->tot + 8) * 8, hipMemcpyDeviceToHost)); HC(hipMemcpy(hsp, dsp, f->C * 32, hipMemcpyDeviceToHost));
    if (digits) {                                   /* sum of the kernel's limbs (absolute positions) against sum c_x 10^(24 x) */
        mpz_t got, ref, t, B; mpz_inits(got, ref, t, B, NULL); mpz_ui_pow_ui(B, 10, 18);
        for (size_t j = 0; j < f->C; j++) {
            size_t len = f->m.len[j % 3], o = p24_run_S(&f->m, j), A = p24_L(f->R * j + f->a), L1 = p24_L(f->R * j + f->a + f->rows);
            for (size_t i = 0; i < len; i++) { mpz_pow_ui(t, B, A + i); mpz_mul_ui(t, t, hout[o + i]); mpz_add(got, got, t); }
            for (int u = 0; u < 4; u++) { mpz_pow_ui(t, B, L1 + (size_t)u); mpz_mul_ui(t, t, hsp[4 * j + u]); mpz_add(got, got, t); }
            for (size_t k = 0; k < f->rows; k++) { mpz_ui_pow_ui(t, 10, 24 * (f->R * j + f->a + k)); mpz_mul(t, t, coef[f->R * j + f->a + k]); mpz_add(ref, ref, t); }
        }
        VERIFY(mpz_cmp(got, ref) == 0, "%s: the kernel's digits differ from GMP (R %zu C %zu a %zu rows %zu)", what, f->R, f->C, f->a, f->rows);
        mpz_clears(got, ref, t, B, NULL);
    }
    for (int p = 0; p < 4; p++) { free(h[p]); HC(hipFree(d[p])); }
    HC(hipFree(dout)); HC(hipFree(dsp)); free(hout); free(hsp);
    return flag;
}
static void test_cfg(size_t R, size_t C, int nr, int rho)
{
    struct cfg f = mk(R, C, nr, rho);
    mpz_t *coef = (mpz_t *)malloc(f.N * sizeof(mpz_t)), b60, M, v; mpz_inits(b60, M, v, NULL); mpz_ui_pow_ui(b60, 10, 60);
    mpz_set_ui(M, 1); for (int p = 0; p < 4; p++) mpz_mul_ui(M, M, ec_P[p]);                        /* the primes' product (1.5e62): the CRT's range */
    for (size_t x = 0; x < f.N; x++) mpz_init(coef[x]);
    char w[96];
    /* in bound: random, the maximum everywhere, edge mixes, zeros */
    for (int kind = 0; kind < 5; kind++) {
        for (size_t x = 0; x < f.N; x++) {
            if (kind == 0) rand_below(coef[x], b60);
            else if (kind == 1) mpz_sub_ui(coef[x], b60, 1);
            else if (kind == 2) { switch (rnd() % 4) { case 0: mpz_set_ui(coef[x], 0); break; case 1: mpz_set_ui(coef[x], 1); break; case 2: mpz_ui_pow_ui(coef[x], 10, rnd() % 61); mpz_sub_ui(coef[x], coef[x], 1); if (mpz_sgn(coef[x]) < 0) mpz_set_ui(coef[x], 0); break; default: mpz_sub_ui(coef[x], b60, 1 + rnd() % 5); } }
            else if (kind == 3) mpz_set_ui(coef[x], 0);
            else { mpz_set_ui(coef[x], rnd() & 1); if (rnd() % 7 == 0) mpz_sub_ui(coef[x], b60, 1); }
        }
        snprintf(w, sizeof w, "in bound kind %d R %zu C %zu nr %d rho %d", kind, R, C, nr, rho);
        for (int chk = 0; chk < 2; chk++) { unsigned fl = run_flag(&f, coef, chk, 1, w); VERIFY(fl == 0, "%s chk %d (1 = default all-shift, 0 = P24_CRT_ANY=0): flag %u (a coefficient below 10^60)", w, chk, fl); }
    }
    /* one coefficient at or past 10^60 among random in-bound ones, at each s class (three consecutive points) */
    for (size_t x = 0; x < f.N; x++) rand_below(coef[x], b60);
    mpz_t bad[7]; for (int i = 0; i < 7; i++) mpz_init(bad[i]);
    mpz_set(bad[0], b60); mpz_add_ui(bad[1], b60, 1); mpz_mul_ui(bad[2], b60, 2); mpz_add_ui(bad[2], bad[2], 7);
    mpz_ui_pow_ui(bad[3], 10, 61); mpz_ui_pow_ui(bad[4], 10, 62); mpz_add_ui(bad[4], bad[4], 12345); mpz_sub_ui(bad[5], M, 1); mpz_mul_ui(bad[6], b60, 99);
    for (int bi = 0; bi < 7; bi++) {
        for (size_t k0 = 0; k0 < 3 && k0 + 3 <= f.rows; k0++) {
            size_t col = (size_t)(rnd() % f.C), x = f.R * col + f.a + f.rows / 2 + k0;           /* a point inside the rank's rows of one column */
            mpz_set(v, coef[x]); mpz_set(coef[x], bad[bi]);
            int s = p24_s(x);
            snprintf(w, sizeof w, "bad %d at x %zu (s %d)", bi, x, s);
            unsigned f0 = run_flag(&f, coef, 0, 0, w), f1 = run_flag(&f, coef, 1, 0, w);
            VERIFY(((f0 & 1) != 0) == (s == 12), "%s: P24_CRT_ANY=0 flag %u (bit 1 expected exactly at s = 12)", w, f0);
            VERIFY((f1 & 4) != 0, "%s: default (all-shift) flag %u (bit 4 expected for c >= 10^60 at any s)", w, f1);
            VERIFY(g_pos == x, "%s: p24_crt_err_pos %zu, expected the planted point %zu", w, g_pos, x);
            mpz_set(coef[x], v);
        }
    }
    for (int i = 0; i < 7; i++) mpz_clear(bad[i]);
    for (size_t x = 0; x < f.N; x++) mpz_clear(coef[x]);
    free(coef); mpz_clears(b60, M, v, NULL);
}

static int g_argc; static char **g_argv;
static int want(const char *w)
{
    if (g_argc < 2) return 1;
    for (int i = 1; i < g_argc; i++) if (!strcmp(g_argv[i], w)) return 1;
    return 0;
}
int main(int argc, char **argv)
{
    g_argc = argc; g_argv = argv;
    HC(hipSetDevice(0)); harness_meta("t_crtk");
    if (want("batch")) {
        printf("== t_crtk batch: k_crt_batch against the host sum ==\n");
        for (int np = 3; np <= 4; np++)
            for (int mode = 0; mode < 3; mode++) {
                uint64_t seed = 1000 * np + mode;
                { size_t q = 1 << 18; uint32_t nc = (uint32_t)q, nx = 0; run_batch(np, 1, &nc, &nx, q, (int)(q / 8192), mode, seed, "C form 2^18 / 32"); }
                { size_t q = 3 << 16; uint32_t nc = (uint32_t)q, nx = 0; run_batch(np, 1, &nc, &nx, q, (int)(q / 8192), mode, seed + 1, "C form 3*2^16 / 24"); }
                { uint32_t nc = 777 * 13 + 5, nx = 0; run_batch(np, 1, &nc, &nx, nc, 13, mode, seed + 2, "ragged 10106 / 13"); }
                { uint32_t nc = 300001, nx = 0; run_batch(np, 1, &nc, &nx, nc, 7, mode, seed + 3, "ragged 300001 / 7"); }
                { uint32_t nc = 5, nx = 0; run_batch(np, 1, &nc, &nx, 8, 9, mode, seed + 4, "nc 5 < S 9"); }
                { uint32_t nc = 1, nx = 1; run_batch(np, 1, &nc, &nx, 1, 1, mode, seed + 5, "nc 1 + x"); }
                {   /* B-form batch: 24 products of varied lengths, every other one with an added operand */
                    enum { MB = 24 }; uint32_t nc[MB], nx[MB]; size_t L = 1 << 16;   /* > 37 * 23^2 + 3, so every nc stays positive */
                    for (int i = 0; i < MB; i++) { nc[i] = (uint32_t)(L - 37 * i * i - (i & 3)); nx[i] = (i & 1) ? nc[i] - 11 * i : 0; }
                    run_batch(np, MB, nc, nx, L, 5, mode, seed + 6, "B-form batch 24 x 2^16 / 5 + x");
                    run_batch(np, MB, nc, nx, L, 1, mode, seed + 7, "B-form batch 24 x 2^16 / 1 + x");
                }
            }
    }
    if (want("col24")) {
        printf("== t_crtk col24: k_crt24 against the host sum ==\n");
        struct { size_t R, C; int nr; } sh[] = { {1024, 16, 8}, {1000, 9, 3}, {4096, 6, 8}, {2048, 5, 1}, {777, 7, 1}, {65536, 12, 8}, {12, 5, 4}, {8, 3, 8}, {3000, 4, 7} };
        for (size_t i = 0; i < sizeof sh / sizeof sh[0]; i++)
            for (int mode = 0; mode < 5; mode++) {
                int rh[3] = { 0, sh[i].nr / 2, sh[i].nr - 1 };
                for (int t = 0; t < 3; t++) { if (t && rh[t] == rh[t - 1]) continue; run24(sh[i].R, sh[i].C, sh[i].nr, rh[t], mode, 1000 * i + 10 * mode + t); }
            }
    }
    if (want("flag")) {
        printf("== t_crtk flag: k_crt24 against GMP, the bound flag ==\n");
        test_cfg(1024, 4, 4, 1);            /* rows 256 (one chunk) */
        test_cfg(1024, 3, 3, 2);            /* rows 342 (two chunks: 256 + 86) */
        test_cfg(1024, 3, 5, 3);            /* rows 205 / 204: unequal parts */
    }
    return verify_done("t_crtk");
}
