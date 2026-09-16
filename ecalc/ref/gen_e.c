/* gen_e - reference digits of e by GMP binary splitting (PLAN.md 8, step 0).
 *
 * e = sum 1/k!, k = 0..N, with N = min{m : lgamma(m+1)/ln10 >= d + 50} as the
 * paper chooses.  Two-variable recursion over [a,b):  Q = a(a+1)...(b-1),
 * P = sum_{k=a}^{b-1} (k+1)...(b-1), so P/Q = sum_{k=a}^{b-1} 1/(a...k) and
 * e = 1 + P(1,N+1)/Q(1,N+1).  Combine: P = P1 Q2 + P2, Q = Q1 Q2 (the paper's
 * bs).  The top levels of the tree run as OpenMP tasks.
 *
 * Output (in DIR, default ref/):
 *   e_<d>.txt      "2." followed by d fractional digits and a newline
 *   e_<d>.sha256   one line per 10^6-digit block of the fractional digits:
 *                  "<block> <sha256>", plus a final line for the whole string
 * Also prints the 50-digit windows at the paper's tier-2 offsets.
 *
 * Usage: gen_e <digits> [DIR]        e.g. gen_e 1000000000 ref
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include <gmp.h>
#include "../tests/sha256.h"

static double now(void) { return omp_get_wtime(); }

/* P, Q over [a, b) */
static void bs(unsigned long a, unsigned long b, mpz_t P, mpz_t Q, int depth)
{
    if (b - a == 1) { mpz_set_ui(P, 1); mpz_set_ui(Q, a); return; }
    if (b - a < 64) {                      /* schoolbook leaf: right to left */
        unsigned long k;
        mpz_set_ui(P, 1); mpz_set_ui(Q, b - 1);           /* [b-1, b) */
        for (k = b - 1; k-- > a;) {                       /* prepend term k */
            mpz_add(P, P, Q);                             /* P = 1*Q + P */
            mpz_mul_ui(Q, Q, k);
        }
        return;
    }
    unsigned long m = (a + b) / 2;
    mpz_t P2, Q2;
    mpz_inits(P2, Q2, NULL);
    if (depth < 5) {
#pragma omp task shared(P, Q) firstprivate(a, m, depth)
        bs(a, m, P, Q, depth + 1);
#pragma omp task shared(P2, Q2) firstprivate(m, b, depth)
        bs(m, b, P2, Q2, depth + 1);
#pragma omp taskwait
    } else {
        bs(a, m, P, Q, depth + 1);
        bs(m, b, P2, Q2, depth + 1);
    }
    mpz_mul(P, P, Q2);
    mpz_add(P, P, P2);
    mpz_mul(Q, Q, Q2);
    mpz_clears(P2, Q2, NULL);
}

int main(int argc, char **argv)
{
    unsigned long d = argc > 1 ? strtoul(argv[1], 0, 10) : 1000000;
    const char *dir = argc > 2 ? argv[2] : "ref";
    unsigned long N = 1;
    while (lgamma((double)N + 1.0) / log(10.0) < (double)d + 50.0) N++;
    printf("gen_e: d = %lu digits, N = %lu terms, %d threads\n", d, N, omp_get_max_threads());

    mpz_t P, Q, X;
    mpz_inits(P, Q, X, NULL);
    double t0 = now();
#pragma omp parallel
#pragma omp single
    bs(1, N + 1, P, Q, 0);
    double t1 = now();
    printf("  bs   %8.2f s   P %zu limbs, Q %zu limbs\n", t1 - t0, mpz_size(P), mpz_size(Q));

    /* X = floor(10^d (Q + P) / Q) = digits of e with the point removed */
    mpz_add(P, P, Q);
    mpz_ui_pow_ui(X, 10, d);
    mpz_mul(X, X, P);
    mpz_tdiv_q(X, X, Q);
    double t2 = now();
    printf("  div  %8.2f s   X %zu limbs\n", t2 - t1, mpz_size(X));

    char *s = mpz_get_str(NULL, 10, X);
    size_t len = strlen(s);
    double t3 = now();
    printf("  dec  %8.2f s   %zu digits\n", t3 - t2, len);
    if (len != d + 1 || s[0] != '2') { fprintf(stderr, "unexpected length %zu / lead %c\n", len, s[0]); return 1; }

    char path[512];
    snprintf(path, sizeof path, "%s/e_%lu.txt", dir, d);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    fputc('2', f); fputc('.', f); fwrite(s + 1, 1, d, f); fputc('\n', f);
    fclose(f);

    snprintf(path, sizeof path, "%s/e_%lu.sha256", dir, d);
    f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    char hex[65];
    size_t blk = 1000000, nblk = (d + blk - 1) / blk, i;
    for (i = 0; i < nblk; i++) {
        size_t n = i * blk + blk <= d ? blk : d - i * blk;
        sha256_hex(s + 1 + i * blk, n, hex);
        fprintf(f, "%zu %s\n", i, hex);
    }
    sha256_hex(s + 1, d, hex);
    fprintf(f, "all %s\n", hex);
    fclose(f);
    printf("  hash %8.2f s   %zu blocks, all = %s\n", now() - t3, nblk, hex);

    /* tier-2 windows: 50 digits starting at fractional offset o (1-based) */
    static const unsigned long off[] = {50, 1000000, 100000000, 1000000000, 10000000000UL, 40000000000UL};
    for (i = 0; i < sizeof off / sizeof *off; i++) {
        if (off[i] + 50 > d) break;
        printf("  window@%-11lu %.50s\n", off[i], s + off[i]);
    }
    printf("  first 60: %.60s\n", s);
    printf("  last  50: %s\n", s + len - 50);
    printf("total %.2f s\n", now() - t0);
    free(s);
    mpz_clears(P, Q, X, NULL);
    return 0;
}
