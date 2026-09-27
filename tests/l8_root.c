/* L8 (Phase 15, throwaway): does ec_root(i, logn) give a primitive 2^logn-th root for logn > EC_LOGN_MAX = 33
 * (the mn tier's plane lengths at g >= 8: mn_logn_cap = 31 + floor(log2 g))?  Prints w^(2^(logn-1)) (must be p - 1).
 * Build: gcc -O2 -o l8_root tests/l8_root.c -lm */
#include <stdio.h>
#include "../ecalc/modarith.h"
int main(void)
{
    for (int i = 0; i < 3; i++)
        for (int logn = 31; logn <= 41; logn++) {
            volatile int ln = logn;                       /* a runtime shift, as in ntt_dist.c plan_create */
            uint64_t w = ec_root(i, ln), p = ec_P[i];
            uint64_t h = ec_powmod(w, 1ULL << (logn - 1), p);
            printf("prime %d logn %d: w = %llu, w^(n/2) = %s\n", i, logn, (unsigned long long)w,
                   h == p - 1 ? "p-1 (primitive)" : h == 1 ? "1 (NOT primitive)" : "other (NOT a root)");
        }
    return 0;
}
