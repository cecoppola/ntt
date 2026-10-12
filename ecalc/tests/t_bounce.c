/* t_bounce - O2 microbenchmark: the first (cold) and steady db_to_bi / db_from_bi of a small number, as in the reciprocal's
 * gather (one node's share of 2 kp + 2 = 109206 limbs at 4 nodes: 27302 limbs).  Run once per arm in a fresh process:
 *   tests/t_bounce [limbs] [reps]                     (DBIG_SMALL_BOUNCE=0 / 1 picks the arm)
 * The device copy is filled with plain hipMemcpy so the first db_to_bi is the process's first bounce use (as in a multi-node run).
 * Prints one line "BOUNCE limbs n small s first_to_bi t first_from_bi t steady_to_bi t steady_from_bi t ok k". */
#include "harness.h"
#include <hip/hip_runtime.h>
#include "../dbig.h"
#include "../rns_mul.h"
#include <string.h>
#include <time.h>
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
static double tb_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static rng_t rg = { 7 };
int main(int argc, char **argv)
{
    size_t n = argc > 1 ? (size_t)atoll(argv[1]) : 27302; int reps = argc > 2 ? atoi(argv[2]) : 9;
    harness_meta("t_bounce"); rns_init(31);
    bigint a, r; bi_init(&a); bi_init(&r); bi_reserve(&a, n); gen_limbs(a.l, n, 0, &rg); a.n = n; bi_norm(&a); n = a.n;
    dbig x, y; db_init(&x); db_init(&y); db_reserve(&x, n); x.n = n;
    for (int d = 0; d < DB_NQ; d++) {                      /* plain copies: no bounce buffer yet */
        size_t lo = (size_t)d * x.qc; if (lo >= n) break; size_t len = n - lo < x.qc ? n - lo : x.qc;
        HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemcpy(x.q[d], a.l + lo, len * 8, hipMemcpyHostToDevice));
    }
    for (int d = 0; d < DB_NQ; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipDeviceSynchronize()); }
    int ok = 0;
    double t0 = tb_now(); db_to_bi(&r, &x); double t1 = tb_now(); ok += bi_cmp(&r, &a) == 0;
    db_from_bi(&y, &a); double t2 = tb_now(); bi_free(&r); bi_init(&r); db_to_bi(&r, &y); ok += bi_cmp(&r, &a) == 0;
    double st = 1e9, sf = 1e9;
    for (int i = 0; i < reps; i++) {
        double u0 = tb_now(); bi_free(&r); bi_init(&r); db_to_bi(&r, &x); double u1 = tb_now(); db_from_bi(&y, &a); double u2 = tb_now();
        ok += bi_cmp(&r, &a) == 0; if (u1 - u0 < st) st = u1 - u0; if (u2 - u1 < sf) sf = u2 - u1;
    }
    const char *sw = getenv("DBIG_SMALL_BOUNCE");
    printf("BOUNCE limbs %zu small %s first_to_bi %.4f first_from_bi %.4f steady_to_bi %.6f steady_from_bi %.6f ok %d/%d\n",
           n, sw ? sw : "0", t1 - t0, t2 - t1, st, sf, ok, reps + 2);
    VERIFY(ok == reps + 2, "round trip");
    return verify_done("t_bounce");
}
