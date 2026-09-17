/* t_school - CPU limb arithmetic throughput in both bases (WP1 attribution item 1):
 * schoolbook (ns per limb^2), mul_1, and the big passes the Newton loop uses
 * (add, sub, cmp, shr by limbs) at 2^26 limbs.
 * Build: cc -O3 -fopenmp -I.. t_school.c ../bigint.c -o t_school */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "../bigint.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static uint64_t rs = 42; static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
#define TIME(best, it, stmt) do { best = 1e9; for (int rep_ = 0; rep_ < 3; rep_++) { double t0_ = now(); for (size_t k_ = 0; k_ < (it); k_++) { stmt; } double dt_ = (now() - t0_) / (it); if (dt_ < best) best = dt_; } } while (0)
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    size_t sizes[] = { 8, 32, 128, 512, 2048 };
    printf("%-6s %8s %14s %14s\n", "base", "n", "school ns/l^2", "mul_1 ns/limb");
    for (int base = 0; base < 2; base++) {
        bi_set_decimal(base);
        uint64_t B = base ? BI_B10 : 0;
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            size_t n = sizes[si];
            uint64_t *a = malloc(n * 8), *b = malloc(n * 8), *r = malloc(2 * n * 8 + 16);
            for (size_t i = 0; i < n; i++) { a[i] = base ? rnd() % B : rnd(); b[i] = base ? rnd() % B : rnd(); }
            size_t it = (size_t)(5e7 / ((double)n * n)) + 1; double best, best1;
            TIME(best, it, limb_mul_school(r, a, n, b, n));
            bigint x = { a, n, n }, y; bi_init(&y);
            size_t it1 = (size_t)(5e7 / n) + 1;
            TIME(best1, it1, bi_mul_u64(&y, &x, base ? 123456789 : 0x9E3779B97F4A7C15ULL));
            printf("%-6s %8zu %14.3f %14.3f\n", base ? "10^18" : "2^64", n, best * 1e9 / ((double)n * n), best1 * 1e9 / n);
            free(a); free(b); free(r); bi_free(&y);
        }
    }
    printf("\n%-6s %12s %12s %12s %12s %12s   (2^26 limbs, ms per pass)\n", "base", "add", "sub", "cmp", "shr_limbs", "mul_1");
    size_t n = (size_t)1 << 26;
    for (int base = 0; base < 2; base++) {
        bi_set_decimal(base);
        uint64_t B = base ? BI_B10 : 0;
        bigint a, b, r; bi_init(&a); bi_init(&b); bi_init(&r);
        bi_reserve(&a, n); bi_reserve(&b, n); a.n = b.n = n;
        for (size_t i = 0; i < n; i++) { a.l[i] = base ? rnd() % B : rnd(); b.l[i] = base ? rnd() % B : rnd(); }
        if (bi_cmp(&a, &b) < 0) { bigint t = a; a = b; b = t; }
        double tadd, tsub, tcmp, tshr, tmul; volatile int sink = 0;
        TIME(tadd, 3, bi_add(&r, &a, &b));
        TIME(tsub, 3, bi_sub(&r, &a, &b));
        TIME(tcmp, 3, sink += bi_cmp(&a, &b));
        TIME(tshr, 3, bi_shr_limbs(&r, &a, 12345));
        TIME(tmul, 3, bi_mul_u64(&r, &a, base ? 123456789 : 0x9E3779B97F4A7C15ULL));
        printf("%-6s %12.1f %12.1f %12.1f %12.1f %12.1f\n", base ? "10^18" : "2^64", tadd * 1e3, tsub * 1e3, tcmp * 1e3, tshr * 1e3, tmul * 1e3);
        bi_free(&a); bi_free(&b); bi_free(&r);
    }
    return 0;
}
