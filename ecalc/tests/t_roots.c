/* t_roots - Phase 15 P (PLAN 36): the roots of unity above 2^33 (modarith.h ec_root / ec_root3), host only.
 *
 * Before Phase 15 P, ec_root(i, logn) = w33^(2^(33 - logn)): a negative shift for logn > 33 (agent L8 measured the "root" = 1
 * for logn 34..41 on p0, p1, p2), reached by the mn tier's planes from 8 nodes on (2^40 at 576).  Checks, for every prime:
 *   1. bit-identity: ec_root(i, logn) == the old formula for logn 0..33, ec_root3(i, logk) == the old one for logk 0..33
 *      (the old code copied here: the values every transform so far has used);
 *   2. the derivation: ec_root(i, logn) == g^((p-1) / 2^logn) by GMP (mpz_powm) for logn 0..v2(p-1), and ec_root3 ==
 *      g^((p-1) / (3 2^logk)) for logk 0..v2(p-1);
 *   3. the order: w^(2^logn) = 1 and w^(2^(logn-1)) = p - 1 (exact order 2^logn), logn 1..v2(p-1); root3: w^(3 2^logk) = 1,
 *      w^(3 2^(logk-1)) = p - 1 (logk >= 1) and w^(2^logk) != 1 (the factor 3);
 *   4. the chain: root(k+1)^2 = root(k) across the table/generator boundary (33 -> 34), root3(k)^3 = root(k), inverses;
 *   5. the refusal: ec_root(i, v2 + 1) and ec_root3(i, v2 + 1) stop the process through ec_fatal (rc 3), in a child;
 *   6. ec_logn_limit(3) = ec_logn_limit(4) = 44 (the WP8 set), and L8's measurement reproduced for the old formula
 *      (logn 34 gives 1 or garbage, never a root of order 2^34) -- only printed.
 * Build (no GPU): gcc -O2 -fopenmp -DEC_FATAL_NO_HIP -I. tests/t_roots.c fatal.c -lgmp -lpthread -o tests/t_roots */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <gmp.h>
/* harness.h's VERIFY / verify_done (harness.h itself needs bigint.o: this test links fatal.c only) */
static int hv_fails, hv_checks;
#define VERIFY(cond, ...) do { hv_checks++; if (!(cond)) { hv_fails++; printf("  FAILED %s:%d  %s  ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int verify_done(const char *name)
{
    if (hv_fails) printf("\n%s: VERIFY FAILED (%d of %d checks)\n", name, hv_fails, hv_checks);
    else printf("\n%s: VERIFY OK (%d checks)\n", name, hv_checks);
    return hv_fails != 0;
}
#include "../modarith.h"

static uint64_t old_root(int i, int logn) { return ec_powmod(ec_W33[i], 1ULL << (EC_LOGN_MAX - logn), ec_P[i]); }
static uint64_t old_root3(int i, int logk) { return ec_W3X33[i] ? ec_powmod(ec_W3X33[i], 1ULL << (EC_LOGN_MAX - logk), ec_P[i]) : 0; }
static uint64_t gmp_pow(uint64_t g, uint64_t e, uint64_t p)
{
    mpz_t b, x, m; mpz_init_set_ui(b, g); mpz_init_set_ui(x, e); mpz_init_set_ui(m, p);
    mpz_powm(b, b, x, m); uint64_t r = mpz_get_ui(b); mpz_clear(b); mpz_clear(x); mpz_clear(m); return r;
}
/* 1 when calling f(i, k) in a child process ends with exit code 3 (ec_fatal) */
static int refuses(uint64_t (*f)(int, int), int i, int k)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { int fd = open("/dev/null", O_WRONLY); if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); } (void)f(i, k); _exit(0); }
    int st = 0; waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == EC_RC_FATAL;
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("t_roots: EC_PRIMES %d, EC_LOGN_MAX %d (the table order), ec_logn_limit(3) %d, ec_logn_limit(4) %d\n", EC_PRIMES, EC_LOGN_MAX, ec_logn_limit(3), ec_logn_limit(4));
#if EC_PRIMES
    VERIFY(ec_logn_limit(3) == 44 && ec_logn_limit(4) == 44, "the WP8 set's 2-adic limit is 44: %d, %d", ec_logn_limit(3), ec_logn_limit(4));
#endif
    for (int i = 0; i < EC_NP; i++) {
        uint64_t p = ec_P[i], g = ec_G[i]; int v2 = ec_v2(i), n1 = 0, n2 = 0, n3 = 0;
        VERIFY(((p - 1) >> v2) & 1, "prime %d: v2", i);
        printf("prime %d: p = %llu = %llu * 2^%d + 1, g = %llu\n", i, (unsigned long long)p, (unsigned long long)((p - 1) >> v2), v2, (unsigned long long)g);
        /* 1. bit-identity for logn <= 33 */
        for (int k = 0; k <= EC_LOGN_MAX; k++) {
            VERIFY(ec_root(i, k) == old_root(i, k), "prime %d: root(%d) %llu, old %llu", i, k, (unsigned long long)ec_root(i, k), (unsigned long long)old_root(i, k));
            VERIFY(ec_root_inv(i, k) == ec_inv(old_root(i, k), p), "prime %d: root_inv(%d)", i, k);
            if (ec_has_radix3()) {
                VERIFY(ec_root3(i, k) == old_root3(i, k), "prime %d: root3(%d) %llu, old %llu", i, k, (unsigned long long)ec_root3(i, k), (unsigned long long)old_root3(i, k));
                VERIFY(ec_root3_inv(i, k) == ec_inv(old_root3(i, k), p), "prime %d: root3_inv(%d)", i, k);
            }
            n1++;
        }
        /* 2-4. the derivation, the order and the chain up to v2 */
        for (int k = 0; k <= v2; k++) {
            uint64_t w = ec_root(i, k);
            VERIFY(w == gmp_pow(g, (p - 1) >> k, p), "prime %d: root(%d) == g^((p-1)/2^%d) (GMP)", i, k, k);
            VERIFY(ec_powmod(w, 1ULL << k, p) == 1, "prime %d: root(%d)^(2^%d) = 1", i, k, k);
            if (k >= 1) VERIFY(ec_powmod(w, 1ULL << (k - 1), p) == p - 1, "prime %d: root(%d)^(2^%d) = p - 1 (exact order)", i, k, k - 1);
            if (k >= 1) VERIFY(ec_mulmod_ref(w, w, p) == ec_root(i, k - 1), "prime %d: root(%d)^2 = root(%d)", i, k, k - 1);
            VERIFY(ec_mulmod_ref(w, ec_root_inv(i, k), p) == 1, "prime %d: root(%d) root_inv(%d) = 1", i, k, k);
            n2++;
            if (ec_has_radix3() && ((p - 1) >> k) % 3 == 0) {
                uint64_t w3 = ec_root3(i, k);
                VERIFY(w3 == gmp_pow(g, ((p - 1) >> k) / 3, p), "prime %d: root3(%d) == g^((p-1)/(3 2^%d)) (GMP)", i, k, k);
                VERIFY(ec_powmod(w3, 3ULL << k, p) == 1, "prime %d: root3(%d)^(3 2^%d) = 1", i, k, k);
                if (k >= 1) VERIFY(ec_powmod(w3, 3ULL << (k - 1), p) == p - 1, "prime %d: root3(%d)^(3 2^%d) = p - 1", i, k, k - 1);
                VERIFY(ec_powmod(w3, 1ULL << k, p) != 1, "prime %d: root3(%d)^(2^%d) != 1 (order divisible by 3)", i, k, k);
                VERIFY(ec_powmod(w3, 3, p) == w, "prime %d: root3(%d)^3 = root(%d)", i, k, k);
                if (k >= 1) VERIFY(ec_mulmod_ref(w3, w3, p) == ec_root3(i, k - 1), "prime %d: root3(%d)^2 = root3(%d)", i, k, k - 1);
                VERIFY(ec_mulmod_ref(w3, ec_root3_inv(i, k), p) == 1, "prime %d: root3 inverse", i);
                n3++;
            }
        }
        /* 5. the refusal beyond v2 (and below 0) */
        VERIFY(refuses(ec_root, i, v2 + 1), "prime %d: ec_root(%d) stops (ec_fatal rc 3)", i, v2 + 1);
        VERIFY(refuses(ec_root, i, -1), "prime %d: ec_root(-1) stops", i);
        if (ec_has_radix3()) VERIFY(refuses(ec_root3, i, v2 + 1), "prime %d: ec_root3(%d) stops", i, v2 + 1);
        /* 6. the old formula at 2^34 (L8's finding; volatile: the shift is a runtime one, as in ntt_dist.c) */
        { volatile int k = 34; uint64_t w = ec_powmod(ec_W33[i], 1ULL << (EC_LOGN_MAX - k), p), h = ec_powmod(w, 1ULL << 33, p);
          printf("  old formula at logn 34: w = %llu, w^(2^33) = %llu (%s); new root(34) = %llu\n", (unsigned long long)w, (unsigned long long)h,
                 h == p - 1 ? "a root" : "NOT a root of order 2^34", (unsigned long long)ec_root(i, 34)); }
        printf("  identical to the old roots for logn 0..%d (%d, radix-3 too); order + GMP + chain checked for logn 0..%d (2^k) and %d radix-3 orders\n", EC_LOGN_MAX, n1, v2, n3);
        (void)n2;
    }
    return verify_done("t_roots");
}
