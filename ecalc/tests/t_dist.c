/* t_dist - the distributed four-step transform on four synthetic ranks (WP5).
 * For each (logR, logC): random plane x, y; distributed fwd/pw/inv over 4
 * ranks must equal the single-rank ntt_fwd/ntt_pw/ntt_inv cyclic convolution. */
#include "harness.h"
#include "../ntt.h"
#include "../ntt_dist.h"
#include "../modarith.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
static rng_t rg = { 12345 };
static comm *tcp;            /* set when run as one process per rank (COMM_RANK in the environment) */
static int one(int prime, int logR, int logC)
{
    int logn = logR + logC, nr = tcp ? comm_size(tcp) : 4; size_t n = (size_t)1 << logn, rows = n / nr;
    uint64_t p = ec_P[prime];
    uint64_t *hx = (uint64_t *)malloc(n * 8), *hy = (uint64_t *)malloc(n * 8), *ref = (uint64_t *)malloc(n * 8), *got = (uint64_t *)malloc(n * 8);
    for (size_t i = 0; i < n; i++) { hx[i] = rng_next(&rg) % p; hy[i] = rng_next(&rg) % p; }
    ntt_ctx *ctx = ntt_ctx_create(prime);
    uint64_t *dx, *dy; HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMalloc(&dy, n * 8));
    HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
    ntt_fwd(ctx, dx, logn, 1, 0); ntt_fwd(ctx, dy, logn, 1, 0); ntt_pw(ctx, dx, dy, n, 0); ntt_inv(ctx, dx, logn, 1, 0);
    HIP_CHECK(hipMemcpy(ref, dx, n * 8, hipMemcpyDeviceToHost));
    /* four ranks: rank r owns rows [r R/4, (r+1) R/4); row i, column j holds point m = i + R j (see ntt_dist.h) */
    size_t R = (size_t)1 << logR, C = (size_t)1 << logC, rr = R / nr;
    uint64_t *tmp = (uint64_t *)malloc(rows * 8);
    comm *cm[64]; dist_plan pl[64]; uint64_t *rx[64], *ry[64];
    int r0 = tcp ? comm_rank(tcp) : 0, r1 = tcp ? r0 + 1 : 4;     /* the ranks this process holds */
    for (int r = r0; r < r1; r++) {
        cm[r] = tcp ? tcp : comm_sim4_create(r); dist_plan_create(&pl[r], cm[r], ctx, prime, logR, logC);
        HIP_CHECK(hipMalloc(&rx[r], rows * 8)); HIP_CHECK(hipMalloc(&ry[r], rows * 8));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hx[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(rx[r], tmp, rows * 8, hipMemcpyHostToDevice));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hy[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(ry[r], tmp, rows * 8, hipMemcpyHostToDevice));
    }
    /* the sim communicator needs every rank to post before any waits: drive phase by phase */
    for (int r = r0; r < r1; r++) dist_fwd_pre(&pl[r], rx[r], 0);
    for (int r = r0; r < r1; r++) dist_fwd_post(&pl[r], rx[r], 0);
    for (int r = r0; r < r1; r++) dist_fwd_pre(&pl[r], ry[r], 0);
    for (int r = r0; r < r1; r++) dist_fwd_post(&pl[r], ry[r], 0);
    for (int r = r0; r < r1; r++) dist_pw(&pl[r], rx[r], ry[r], 0);
    for (int r = r0; r < r1; r++) dist_inv_pre(&pl[r], rx[r], 0);
    for (int r = r0; r < r1; r++) dist_inv_post(&pl[r], rx[r], 0);
    HIP_CHECK(hipDeviceSynchronize());
    size_t bad = 0, first = n;
    for (int r = r0; r < r1; r++) {
        HIP_CHECK(hipMemcpy(tmp, rx[r], rows * 8, hipMemcpyDeviceToHost));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) got[(r * rr + il) + R * j] = tmp[il * C + j];
        dist_plan_free(&pl[r]); if (!tcp) comm_destroy(cm[r]); HIP_CHECK(hipFree(rx[r])); HIP_CHECK(hipFree(ry[r]));
    }
    for (size_t i = 0; i < n; i++) if (tcp && (size_t)((i % R) / rr) != (size_t)r0) continue; else if (got[i] != ref[i]) { if (first == n) first = i; bad++; }
    if (bad) printf("  prime %d logR %d logC %d: %zu of %zu points differ (first at %zu)\n", prime, logR, logC, bad, n, first);
    free(tmp);
    ntt_ctx_free(ctx); HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); free(hx); free(hy); free(ref); free(got);
    return bad == 0;
}
int main(int argc, char **argv)
{
    int logmax = argc > 1 ? atoi(argv[1]) : 24;
    harness_meta("t_dist");
    if (getenv("COMM_RANK")) {                 /* one process per rank over TCP (WP6); rank r uses APU r mod 4 */
        int rk = atoi(getenv("COMM_RANK")), nd = 1; HIP_CHECK(hipGetDeviceCount(&nd)); HIP_CHECK(hipSetDevice(rk % nd));
        tcp = comm_tcp_create();
        printf("t_dist: rank %d of %d over TCP\n", rk, comm_size(tcp));
    }
    for (int prime = 0; prime < 4; prime++)
        for (int logR = 10; logR <= 13; logR++)
            for (int logC = 10; logC <= 13; logC++)
                if (logR + logC <= logmax) VERIFY(one(prime, logR, logC), "dist conv prime %d %dx%d", prime, logR, logC);
    if (logmax >= 26) VERIFY(one(0, 13, 13), "dist conv 2^26");
    if (logmax >= 30) VERIFY(one(1, 15, 15), "dist conv 2^30");
    if (tcp) comm_destroy(tcp);
    return verify_done("t_dist");
}
