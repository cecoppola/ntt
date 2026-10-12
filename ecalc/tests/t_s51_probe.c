/* t_s51_probe - S51 micro-benchmark of the dbig add/sub kernels (DBIG_ADDSUB2 / DBIG_ADDSUB3) and k_modq (DBIG_MODQ_MONT).
 * Usage: [DBIG_ADDSUB3=0|1] [DBIG_MODQ_MONT=0|1] t_s51_probe [logn (27)] [reps (5)] [kind 0=uniform 1=all-ones (3)]
 * Prints, per operation, the median and min wall time and a checksum line (residues of the results mod the T1 primes) so two runs can be compared
 * by diff.  Not part of TESTS (built on demand; needs 4 APUs, ~12 B x 2^logn bytes of HBM + host copies). */
#include "harness.h"
#include <hip/hip_runtime.h>
#include "../dbig.h"
#include "../verify.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#include "../rns_mul.h"
#include <string.h>
#include <time.h>
static double wnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static void rep(const char *w, double *t, int n) { qsort(t, n, sizeof *t, cmpd); printf("%-8s median %.4f s  min %.4f s  (n %d)\n", w, t[n / 2], t[0], n); }
int main(int argc, char **argv)
{
    int logn = argc > 1 ? atoi(argv[1]) : 27, reps = argc > 2 ? atoi(argv[2]) : 5, kind = argc > 3 ? atoi(argv[3]) : 0;
    harness_meta("t_s51_probe"); rns_init(31);
    size_t n = (size_t)1 << logn; rng_t rg = { 5151 };
    bigint a, b; bi_init(&a); bi_init(&b);
    bi_reserve(&a, n); gen_limbs(a.l, n, kind, &rg); a.n = n; bi_norm(&a);
    bi_reserve(&b, n); gen_limbs(b.l, n - 3, kind == 1 ? 1 : 0, &rg); b.n = n - 3; bi_norm(&b);
    if (bi_cmp(&a, &b) < 0) { bigint t = a; a = b; b = t; }                /* sub needs a >= b */
    dbig x, y, z; db_init(&x); db_init(&y); db_init(&z); db_from_bi(&x, &a); db_from_bi(&y, &b);
    double *t = (double *)malloc(reps * sizeof *t); uint64_t rs[T1_NQ];
    db_add(&z, &x, &y); db_sub(&z, &z, &y);                                  /* warm-up */
    for (int i = 0; i < reps; i++) { double t0 = wnow(); db_add(&z, &x, &y); t[i] = wnow() - t0; }
    rep("add", t, reps); db_mod_qs(&z, t1_q, T1_NQ, rs); printf("CHK add :"); for (int j = 0; j < T1_NQ; j++) printf(" %llu", (unsigned long long)rs[j]); printf("\n");
    for (int i = 0; i < reps; i++) { double t0 = wnow(); db_sub(&z, &x, &y); t[i] = wnow() - t0; }
    rep("sub", t, reps); db_mod_qs(&z, t1_q, T1_NQ, rs); printf("CHK sub :"); for (int j = 0; j < T1_NQ; j++) printf(" %llu", (unsigned long long)rs[j]); printf("\n");
    for (int i = 0; i < reps; i++) { double t0 = wnow(); db_add(&z, &z, &y); t[i] = wnow() - t0; db_sub(&z, &z, &y); }
    rep("add-inpl", t, reps);
    for (int i = 0; i < reps; i++) { double t0 = wnow(); db_mod_qs(&x, t1_q, T1_NQ, rs); t[i] = wnow() - t0; }
    rep("modqs", t, reps); printf("CHK modq:"); for (int j = 0; j < T1_NQ; j++) printf(" %llu", (unsigned long long)rs[j]); printf("\n");
    return 0;
}
