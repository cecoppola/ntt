/* t_dist - the distributed four-step transform on four synthetic ranks (WP5).
 * For each (logR, logC): random plane x, y; distributed fwd/pw/inv over 4
 * ranks must equal the single-rank ntt_fwd/ntt_pw/ntt_inv cyclic convolution. */
#include "harness.h"
#include <omp.h>
#include <string.h>
#include "../ntt.h"
#include "../ntt_dist.h"
#include "../modarith.h"
#include "../mn.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
static rng_t rg = { 12345 };
static comm *tcp;            /* set when run as one process per rank (COMM_RANK in the environment) */
static int xgmi;             /* DIST_XGMI=1: four real APUs, four host threads */
static int tinv;             /* DIST_TINV=1: the transposed inverse (result in contiguous ownership) */
static int one_xgmi(int prime, int logR, int logC);
/* M7: the all-gathers of a communicator (device and host blocks; in place) against the pattern (rank, k); over the
 * synthetic communicator the four rank objects must all call before any result is read (the caller drives them) */
static uint64_t agw(int r, int round, size_t k) { return ((uint64_t)r << 40) ^ ((uint64_t)round << 32) ^ k; }
static int check_allgather(comm *c, int host_only)
{
    int n = comm_size(c), me = comm_rank(c), bad = 0;
    size_t sizes[] = { 8, 4096, 4104, 1 << 20 };
    for (int round = 0; round < 4; round++) {
        size_t words = sizes[round] / 8, bytes = words * 8;
        uint64_t *hs = (uint64_t *)malloc(bytes), *hr = (uint64_t *)malloc(bytes * n);
        for (size_t k = 0; k < words; k++) hs[k] = agw(me, round, k);
        comm_allgather_host(c, hs, hr, bytes);
        for (int r = 0; r < n; r++) for (size_t k = 0; k < words; k++) if (hr[r * words + k] != agw(r, round, k)) bad++;
        if (!host_only) {
            uint64_t *ds, *dr; HIP_CHECK(hipMalloc(&ds, bytes)); HIP_CHECK(hipMalloc(&dr, bytes * n));
            HIP_CHECK(hipMemcpy(ds, hs, bytes, hipMemcpyHostToDevice));
            comm_allgather(c, ds, dr, bytes);
            memset(hr, 0, bytes * n); HIP_CHECK(hipMemcpy(hr, dr, bytes * n, hipMemcpyDeviceToHost));
            for (int r = 0; r < n; r++) for (size_t k = 0; k < words; k++) if (hr[r * words + k] != agw(r, round, k)) bad++;
            HIP_CHECK(hipMemcpy(dr + me * words, ds, bytes, hipMemcpyDeviceToDevice));   /* in place */
            comm_allgather(c, dr + me * words, dr, bytes);
            memset(hr, 0, bytes * n); HIP_CHECK(hipMemcpy(hr, dr, bytes * n, hipMemcpyDeviceToHost));
            for (int r = 0; r < n; r++) for (size_t k = 0; k < words; k++) if (hr[r * words + k] != agw(r, round, k)) bad++;
            HIP_CHECK(hipFree(ds)); HIP_CHECK(hipFree(dr));
        }
        free(hs); free(hr);
    }
    if (bad) printf("  allgather over %d ranks: rank %d: %d words wrong\n", n, me, bad);
    return bad == 0;
}
/* the synthetic communicator: all four ranks post before any completes, so run the four in lock step per call */
static int check_allgather_sim4(void)
{
    comm *cm[4]; for (int r = 0; r < 4; r++) cm[r] = comm_sim4_create(r);
    size_t words = 4096 / 8, bytes = words * 8; int bad = 0;
    uint64_t *hs[4], *hr[4], *ds[4], *dr[4];
    for (int r = 0; r < 4; r++) {
        hs[r] = (uint64_t *)malloc(bytes); hr[r] = (uint64_t *)malloc(bytes * 4); for (size_t k = 0; k < words; k++) hs[r][k] = agw(r, 7, k);
        HIP_CHECK(hipMalloc(&ds[r], bytes)); HIP_CHECK(hipMalloc(&dr[r], bytes * 4)); HIP_CHECK(hipMemcpy(ds[r], hs[r], bytes, hipMemcpyHostToDevice));
    }
    for (int r = 0; r < 4; r++) comm_allgather_host(cm[r], hs[r], hr[r], bytes);
    for (int r = 0; r < 4; r++) comm_allgather(cm[r], ds[r], dr[r], bytes);
    for (int me = 0; me < 4; me++) {
        for (int r = 0; r < 4; r++) for (size_t k = 0; k < words; k++) if (hr[me][r * words + k] != agw(r, 7, k)) bad++;
        HIP_CHECK(hipMemcpy(hr[me], dr[me], bytes * 4, hipMemcpyDeviceToHost));
        for (int r = 0; r < 4; r++) for (size_t k = 0; k < words; k++) if (hr[me][r * words + k] != agw(r, 7, k)) bad++;
        free(hs[me]); free(hr[me]); HIP_CHECK(hipFree(ds[me])); HIP_CHECK(hipFree(dr[me])); comm_destroy(cm[me]);
    }
    if (bad) printf("  allgather over sim4: %d words wrong\n", bad);
    return bad == 0;
}
static double tnow(void) { return omp_get_wtime(); }
static int one(int prime, int logR, int logC)
{
    if (xgmi) return one_xgmi(prime, logR, logC);
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
    for (int r = r0; r < r1; r++) { if (tinv) dist_inv_t_pre(&pl[r], rx[r], 0); else dist_inv_pre(&pl[r], rx[r], 0); }
    for (int r = r0; r < r1; r++) { if (tinv) dist_inv_t_post(&pl[r], rx[r], 0); else dist_inv_post(&pl[r], rx[r], 0); }
    HIP_CHECK(hipDeviceSynchronize());
    size_t bad = 0, first = n;
    for (int r = r0; r < r1; r++) {
        HIP_CHECK(hipMemcpy(tmp, rx[r], rows * 8, hipMemcpyDeviceToHost));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) got[(r * rr + il) + R * j] = tmp[il * C + j];   /* both inverses: block-cyclic */
        dist_plan_free(&pl[r]); if (!tcp) comm_destroy(cm[r]); HIP_CHECK(hipFree(rx[r])); HIP_CHECK(hipFree(ry[r]));
    }
    for (size_t i = 0; i < n; i++) if (tcp && (size_t)((i % R) / rr) != (size_t)r0) continue; else if (got[i] != ref[i]) { if (first == n) first = i; bad++; }
    if (bad) printf("  prime %d logR %d logC %d: %zu of %zu points differ (first at %zu)\n", prime, logR, logC, bad, n, first);
    free(tmp);
    ntt_ctx_free(ctx); HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); free(hx); free(hy); free(ref); free(got);
    return bad == 0;
}
/* four real ranks: the reference on device 0, then each rank on its own device with its own context */
static int one_xgmi(int prime, int logR, int logC)
{
    int logn = logR + logC; size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rr = R / 4, rows = n / 4;
    uint64_t p = ec_P[prime];
    uint64_t *hx = (uint64_t *)malloc(n * 8), *hy = (uint64_t *)malloc(n * 8), *ref = (uint64_t *)malloc(n * 8), *got = (uint64_t *)malloc(n * 8);
    for (size_t i = 0; i < n; i++) { hx[i] = rng_next(&rg) % p; hy[i] = rng_next(&rg) % p; }
    HIP_CHECK(hipSetDevice(0)); int bad_ag = 0; memset(&dist_st, 0, sizeof dist_st); dist_st.on = getenv("DIST_STATS") && atoi(getenv("DIST_STATS"));
    { ntt_ctx *ctx = ntt_ctx_create(prime); uint64_t *dx, *dy; HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMalloc(&dy, n * 8));
      HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
      ntt_fwd(ctx, dx, logn, 1, 0); ntt_fwd(ctx, dy, logn, 1, 0); ntt_pw(ctx, dx, dy, n, 0); ntt_inv(ctx, dx, logn, 1, 0);
      HIP_CHECK(hipMemcpy(ref, dx, n * 8, hipMemcpyDeviceToHost)); HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); ntt_ctx_free(ctx); }
#pragma omp parallel num_threads(4)
    {
        int r = omp_get_thread_num();
        HIP_CHECK(hipSetDevice(r));
        comm *cm = comm_xgmi_create(r); ntt_ctx *ctx = ntt_ctx_create(prime); hipStream_t s; HIP_CHECK(hipStreamCreate(&s));
        dist_plan pl; dist_plan_create(&pl, cm, ctx, prime, logR, logC);
        uint64_t *tmp = (uint64_t *)malloc(rows * 8), *rx, *ry; HIP_CHECK(hipMalloc(&rx, rows * 8)); HIP_CHECK(hipMalloc(&ry, rows * 8));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hx[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(rx, tmp, rows * 8, hipMemcpyHostToDevice));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hy[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(ry, tmp, rows * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipStreamSynchronize(s)); comm_barrier(cm);
        double t0 = tnow();
        dist_fwd(&pl, rx, s); HIP_CHECK(hipStreamSynchronize(s)); double t1 = tnow();
        dist_fwd(&pl, ry, s); dist_pw(&pl, rx, ry, s);
        if (tinv) dist_inv_t(&pl, rx, s); else dist_inv(&pl, rx, s);
        HIP_CHECK(hipStreamSynchronize(s)); double t2 = tnow();
        if (logn >= 26 && r == 0) printf("  xgmi 2^%d (%d chunks): fwd %.4f s, fwd+fwd+pw+inv %.4f s%s\n", logn, pl.K, t1 - t0, t2 - t0, dist_st.on ? "" : " (DIST_STATS=1 for the exposed exchange time)");
        comm_barrier(cm);
        if (dist_st.on && r == 0) printf("  DIST_STATS (four ranks summed / 4): rows %.4f cols %.4f pack %.4f exposed exchange %.4f s\n", dist_st.t_local1 / 4, dist_st.t_local2 / 4, dist_st.t_pack / 4, dist_st.t_a2a / 4);
        if (!check_allgather(cm, 0)) { printf("  xgmi allgather failed on rank %d\n", r); bad_ag = 1; }
        HIP_CHECK(hipMemcpy(tmp, rx, rows * 8, hipMemcpyDeviceToHost));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) got[(r * rr + il) + R * j] = tmp[il * C + j];
        dist_plan_free(&pl); comm_destroy(cm); ntt_ctx_free(ctx); HIP_CHECK(hipStreamDestroy(s)); HIP_CHECK(hipFree(rx)); HIP_CHECK(hipFree(ry)); free(tmp);
    }
    size_t bad = 0, first = n;
    for (size_t i = 0; i < n; i++) if (got[i] != ref[i]) { if (first == n) first = i; bad++; }
    if (bad) printf("  xgmi prime %d logR %d logC %d: %zu of %zu points differ (first at %zu)\n", prime, logR, logC, bad, n, first);
    free(hx); free(hy); free(ref); free(got);
    return bad == 0 && !bad_ag;
}
int main(int argc, char **argv)
{
    int logmax = argc > 1 ? atoi(argv[1]) : 24;
    harness_meta("t_dist");
    xgmi = getenv("DIST_XGMI") && atoi(getenv("DIST_XGMI"));
    tinv = getenv("DIST_TINV") && atoi(getenv("DIST_TINV"));
    if (tinv) printf("t_dist: transposed inverse\n");
    if (xgmi) printf("t_dist: four real APUs over xGMI\n");
    if (getenv("DIST_LAYERED") && atoi(getenv("DIST_LAYERED"))) {   /* M3: the layered communicator, one node-process per COMM_RANK driving four APUs (mnrun.sh) */
        int sz = mn_init(); printf("t_dist: layered communicator, node %d of %d\n", mn_rank(), sz);
        for (int logR = 10; logR <= 12; logR++) for (int logC = 10; logC <= 12; logC++) if (logR + logC <= logmax) VERIFY(mn_selftest_layered(logR, logC, 0), "layered conv %dx%d", logR, logC);
        if (logmax >= 26) VERIFY(mn_selftest_layered(13, 13, 0), "layered conv 2^26");
        {   /* M7: the layered communicator's all-gathers (4 gt ranks, rank gt d + node) and the mesh's */
            int L = 0; while ((1 << L) < sz) L++; mn_group *G = mn_group_at(L); int ok = 1;
            if (mn_rank() < G->gt && G->tr[0]) {
#pragma omp parallel for num_threads(4) schedule(static) reduction(&&:ok)
                for (int d = 0; d < 4; d++) {
                    HIP_CHECK(hipSetDevice(d));
                    comm *xg = comm_xgmi_create(d), *cm = comm_layered_create(xg, G->tr[d], d);
                    ok = check_allgather(cm, 0) && ok;
                    comm_destroy(cm); comm_destroy(xg);
                }
                HIP_CHECK(hipSetDevice(0));
            }
            VERIFY(ok, "layered allgather");
            VERIFY(check_allgather(mn_comm(0), 0), "mesh allgather");
        }
        mn_barrier(); mn_finalize();
        return verify_done("t_dist");
    }
    if (!xgmi && getenv("COMM_RANK")) {                 /* one process per rank over TCP (WP6); rank r uses APU r mod 4 */
        int rk = atoi(getenv("COMM_RANK")), nd = 1; HIP_CHECK(hipGetDeviceCount(&nd)); HIP_CHECK(hipSetDevice(rk % nd));
        tcp = comm_tcp_create();
        printf("t_dist: rank %d of %d over TCP\n", rk, comm_size(tcp));
        VERIFY(check_allgather(tcp, 0), "tcp allgather");
    }
    if (!xgmi && !tcp) VERIFY(check_allgather_sim4(), "sim4 allgather");
    for (int prime = 0; prime < 4; prime++)
        for (int logR = 10; logR <= 13; logR++)
            for (int logC = 10; logC <= 13; logC++)
                if (logR + logC <= logmax) VERIFY(one(prime, logR, logC), "dist conv prime %d %dx%d", prime, logR, logC);
    if (logmax >= 26) VERIFY(one(0, 13, 13), "dist conv 2^26");
    if (logmax >= 30) VERIFY(one(1, 15, 15), "dist conv 2^30");
    if (tcp) comm_destroy(tcp);
    return verify_done("t_dist");
}
