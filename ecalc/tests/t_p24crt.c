/* t_p24crt - (results/PRIMES_STRATEGY.md §P24 audit): the P24 CRT kernel (p24_crt.c k_crt24, via p24_crt_launch / p24_crt_err) against GMP.
 * Residues are built from chosen coefficients c_x (x the plane point; the residue of c_x at the four primes), the kernel's out-runs and spills are summed
 * into one number and compared with sum c_x 10^(24 x) (GMP), and the flag is checked:
 *   in bound (c < 10^60: random, all 10^60 - 1, edge mixes)         flag 0, digits equal;
 *   one coefficient c >= 10^60 (10^60 .. the primes' product - 1)    default form (p24_crt_set_check(1), what rns_dist.c sets unless P24_CRT_ANY=0): bit 4 exactly (c >= 10^60 at any s: a flag
 *                                                                    raised exactly when c >= 10^60); P24_CRT_ANY=0 form (p24_crt_set_check(0)): bit 1 only at an s = 12 point (c 10^12 >= 10^72), none at s = 0, 6.
 * Needs a GPU: the hipcc build (`make tests/t_p24crt`, on a node).  VERIFY OK / FAILED as the other tests. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <gmp.h>
#include "../p24.h"
#include "../rns_int.h"

static int fails, checks;
#define VERIFY(c, ...) do { checks++; if (!(c)) { if (fails++ < 30) { printf("  FAILED %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } } while (0)
#define HC(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { printf("HIP %s at %d\n", hipGetErrorString(e_), __LINE__); exit(3); } } while (0)
static uint64_t rs = 0x13198A2E03707344ULL;
static uint64_t rnd(void) { uint64_t z = (rs += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
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
static unsigned run(const struct cfg *f, mpz_t *coef, int chk, int digits, const char *what)
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
        for (int chk = 0; chk < 2; chk++) { unsigned fl = run(&f, coef, chk, 1, w); VERIFY(fl == 0, "%s chk %d (1 = default all-shift, 0 = P24_CRT_ANY=0): flag %u (a coefficient below 10^60)", w, chk, fl); }
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
            unsigned f0 = run(&f, coef, 0, 0, w), f1 = run(&f, coef, 1, 0, w);
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
int main(void)
{
    printf("t_p24crt: k_crt24 against GMP (digits, and the flag exactly at c >= 10^60)\n");
    test_cfg(1024, 4, 4, 1);            /* rows 256 (one chunk) */
    test_cfg(1024, 3, 3, 2);            /* rows 342 (two chunks: 256 + 86) */
    test_cfg(1024, 3, 5, 3);            /* rows 205 / 204: unequal parts */
    if (fails) printf("\nt_p24crt: VERIFY FAILED (%d of %d checks)\n", fails, checks); else printf("\nt_p24crt: VERIFY OK (%d checks)\n", checks);
    return fails != 0;
}
