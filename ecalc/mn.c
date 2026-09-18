/* mn.c - the multi-node layer: environment, one TCP mesh per APU thread, the start-up self-test (PLAN.md 17, M1) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <hip/hip_runtime.h>
#include "mn.h"
#include "ntt.h"
#include "ntt_dist.h"
#include "modarith.h"
#include "mem.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NA 4
static int g_rank, g_size = 1; static comm *g_cm[NA];
int mn_rank(void) { return g_rank; }
int mn_size(void) { return g_size; }
comm *mn_comm(int apu) { return g_size > 1 ? g_cm[apu] : 0; }
int mn_init(void)
{
    const char *er = getenv("COMM_RANK"), *es = getenv("COMM_SIZE"), *eh = getenv("COMM_HOSTS"), *ep = getenv("COMM_PORT");
    g_size = es ? atoi(es) : 1; g_rank = er ? atoi(er) : 0;
    if (g_size <= 1) { g_size = 1; g_rank = 0; return 1; }
    if (!eh) { fprintf(stderr, "mn: COMM_SIZE %d needs COMM_HOSTS\n", g_size); exit(1); }
    int base = ep ? atoi(ep) : 27000;
    /* mesh d joins APU thread d of every node: rank = node, size = nodes */
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static)
    for (int d = 0; d < NA; d++) { HIP_CHECK(hipSetDevice(d)); g_cm[d] = comm_tcp_create_at(g_rank, g_size, eh, base + 64 * d); }
    HIP_CHECK(hipSetDevice(0));
    printf("mn: node %d of %d, four meshes of %d ranks on %s (port base %d): connected in %.2f s\n", g_rank, g_size, g_size, eh, base, mem_now() - t0);
    return g_size;
}
void mn_barrier(void) { if (g_size > 1) comm_barrier(g_cm[0]); }
void mn_finalize(void) { if (g_size > 1) for (int d = 0; d < NA; d++) if (g_cm[d]) { comm_destroy(g_cm[d]); g_cm[d] = 0; } }
/* self-test: on every APU thread, prime d, a random cyclic convolution of 2^(logR+logC) points from a seed all
 * nodes share; the distributed fwd/pw/inv over mesh d's `size` ranks must equal the one-rank engine on this
 * rank's block-cyclic rows (rank r holds rows [r R/nr, (r+1) R/nr); row i, column j <-> point i + R j) */
static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
int mn_selftest(int logR, int logC, int verbose)
{
    if (g_size <= 1) return 1;
    int ok = 1, nr = g_size, logn = logR + logC; size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rr = R / nr, rows = n / nr;
    if (rr == 0) { fprintf(stderr, "mn_selftest: R < ranks\n"); return 0; }
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static) reduction(&&:ok)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d));
        int prime = d, r = g_rank; uint64_t p = ec_P[prime], seed = 0x9E3779B97F4A7C15ull + prime;
        uint64_t *hx = (uint64_t *)malloc(n * 8), *hy = (uint64_t *)malloc(n * 8), *ref = (uint64_t *)malloc(n * 8), *tmp = (uint64_t *)malloc(rows * 8);
        for (size_t i = 0; i < n; i++) { hx[i] = xs(&seed) % p; hy[i] = xs(&seed) % p; }
        ntt_ctx *ctx = ntt_ctx_create(prime); hipStream_t s; HIP_CHECK(hipStreamCreate(&s));
        uint64_t *dx, *dy; HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMalloc(&dy, n * 8));
        HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
        ntt_fwd(ctx, dx, logn, 1, s); ntt_fwd(ctx, dy, logn, 1, s); ntt_pw(ctx, dx, dy, n, s); ntt_inv(ctx, dx, logn, 1, s);
        HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(ref, dx, n * 8, hipMemcpyDeviceToHost));
        dist_plan pl; dist_plan_create(&pl, g_cm[d], ctx, prime, logR, logC);
        uint64_t *rx, *ry; HIP_CHECK(hipMalloc(&rx, rows * 8)); HIP_CHECK(hipMalloc(&ry, rows * 8));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hx[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(rx, tmp, rows * 8, hipMemcpyHostToDevice));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hy[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(ry, tmp, rows * 8, hipMemcpyHostToDevice));
        dist_fwd(&pl, rx, s); dist_fwd(&pl, ry, s); dist_pw(&pl, rx, ry, s); dist_inv(&pl, rx, s);
        HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(tmp, rx, rows * 8, hipMemcpyDeviceToHost));
        size_t bad = 0;
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) if (tmp[il * C + j] != ref[(r * rr + il) + R * j]) bad++;
        if (bad || verbose) printf("mn: node %d mesh %d 2^%d points: %zu of %zu differ\n", r, d, logn, bad, rows);
        if (bad) ok = 0;
        dist_plan_free(&pl); HIP_CHECK(hipFree(rx)); HIP_CHECK(hipFree(ry)); HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); HIP_CHECK(hipStreamDestroy(s)); ntt_ctx_free(ctx);
        free(hx); free(hy); free(ref); free(tmp);
    }
    HIP_CHECK(hipSetDevice(0));
    printf("mn: self-test, four meshes over %d nodes at 2^%d points: %s (%.2f s)\n", nr, logn, ok ? "ok" : "FAILED", mem_now() - t0);
    return ok;
}
