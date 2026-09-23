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
/* B7: the unequal all-to-all of a communicator -- per-pair counts of 0..8 units (8 B, 1000 B, 1 MiB), the send slabs
 * back to back, the receive slabs in reverse rank order; device op then host op; the pattern (from, to, round, k) */
static size_t vunits(int from, int to, int round) { return ((from + to + round) % 5 == 0) ? 0 : (size_t)((from * 7 + to * 13 + round * 5) % 9); }
static uint64_t vw(int from, int to, int round, size_t k) { return ((uint64_t)from << 48) ^ ((uint64_t)to << 40) ^ ((uint64_t)round << 32) ^ (k * 0x9E3779B97F4A7C15ULL); }
struct vbuf { size_t *scnt, *sdsp, *rcnt, *rdsp, ts, tr; uint64_t *hs, *hr; };
static void vbuf_make(struct vbuf *v, int me, int n, int round, size_t u)
{
    v->scnt = (size_t *)malloc(4 * n * sizeof(size_t)); v->sdsp = v->scnt + n; v->rcnt = v->sdsp + n; v->rdsp = v->rcnt + n;
    for (int r = 0; r < n; r++) { v->scnt[r] = vunits(me, r, round) * u; v->rcnt[r] = vunits(r, me, round) * u; }
    v->ts = comm_prefix(v->scnt, v->sdsp, n); v->tr = 0;
    if (round & 1) { v->ts = 0; for (int r = n - 1; r >= 0; r--) { v->sdsp[r] = v->ts; v->ts += v->scnt[r]; } }   /* odd rounds: the send slabs in reverse order too (not back to back in rank order) */
    for (int r = n - 1; r >= 0; r--) { v->rdsp[r] = v->tr; v->tr += v->rcnt[r]; }
    v->hs = (uint64_t *)malloc(v->ts + 8); v->hr = (uint64_t *)malloc(v->tr + 8); memset(v->hr, 0xEE, v->tr + 8);
    for (int r = 0; r < n; r++) for (size_t k = 0; k < v->scnt[r] / 8; k++) v->hs[v->sdsp[r] / 8 + k] = vw(me, r, round, k);
}
static int vbuf_check(const struct vbuf *v, int me, int n, int round)
{
    int bad = 0;
    for (int r = 0; r < n; r++) for (size_t k = 0; k < v->rcnt[r] / 8; k++) if (v->hr[v->rdsp[r] / 8 + k] != vw(r, me, round, k)) bad++;
    return bad;
}
static void vbuf_free(struct vbuf *v) { free(v->scnt); free(v->hs); free(v->hr); }
static int check_alltoallv(comm *c)
{
    int n = comm_size(c), me = comm_rank(c), bad = 0; size_t units[] = { 8, 1000 * 8, 1 << 20 };
    for (int round = 0; round < 6; round++) {
        struct vbuf v; vbuf_make(&v, me, n, round, units[round % 3]);
        if (round < 3) {
            uint64_t *ds, *dr; HIP_CHECK(hipMalloc(&ds, v.ts + 8)); HIP_CHECK(hipMalloc(&dr, v.tr + 8));
            HIP_CHECK(hipMemcpy(ds, v.hs, v.ts + 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dr, v.hr, v.tr + 8, hipMemcpyHostToDevice));
            comm_alltoallv(c, ds, v.scnt, v.sdsp, dr, v.rcnt, v.rdsp, 0); comm_wait(c);
            HIP_CHECK(hipMemcpy(v.hr, dr, v.tr + 8, hipMemcpyDeviceToHost));
            HIP_CHECK(hipFree(ds)); HIP_CHECK(hipFree(dr));
        } else comm_alltoallv_host(c, v.hs, v.scnt, v.sdsp, v.hr, v.rcnt, v.rdsp);
        bad += vbuf_check(&v, me, n, round); vbuf_free(&v);
    }
    if (bad) printf("  alltoallv over %d ranks: rank %d: %d words wrong\n", n, me, bad);
    return bad == 0;
}
/* the synthetic communicator: the four ranks post, then wait (device); the host op completes at the fourth call */
static int check_alltoallv_sim4(void)
{
    comm *cm[4]; for (int r = 0; r < 4; r++) cm[r] = comm_sim4_create(r);
    int bad = 0;
    for (int round = 0; round < 2; round++) {
        struct vbuf v[4]; uint64_t *ds[4], *dr[4];
        for (int r = 0; r < 4; r++) {
            vbuf_make(&v[r], r, 4, round, round ? 1000 * 8 : 8);
            HIP_CHECK(hipMalloc(&ds[r], v[r].ts + 8)); HIP_CHECK(hipMalloc(&dr[r], v[r].tr + 8));
            HIP_CHECK(hipMemcpy(ds[r], v[r].hs, v[r].ts + 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dr[r], v[r].hr, v[r].tr + 8, hipMemcpyHostToDevice));
        }
        for (int r = 0; r < 4; r++) comm_alltoallv(cm[r], ds[r], v[r].scnt, v[r].sdsp, dr[r], v[r].rcnt, v[r].rdsp, 0);
        for (int r = 0; r < 4; r++) comm_wait(cm[r]);
        for (int r = 0; r < 4; r++) { HIP_CHECK(hipMemcpy(v[r].hr, dr[r], v[r].tr + 8, hipMemcpyDeviceToHost)); bad += vbuf_check(&v[r], r, 4, round); }
        for (int r = 0; r < 4; r++) memset(v[r].hr, 0xEE, v[r].tr + 8);
        for (int r = 0; r < 4; r++) comm_alltoallv_host(cm[r], v[r].hs, v[r].scnt, v[r].sdsp, v[r].hr, v[r].rcnt, v[r].rdsp);
        for (int r = 0; r < 4; r++) { bad += vbuf_check(&v[r], r, 4, round); vbuf_free(&v[r]); HIP_CHECK(hipFree(ds[r])); HIP_CHECK(hipFree(dr[r])); }
    }
    for (int r = 0; r < 4; r++) comm_destroy(cm[r]);
    if (bad) printf("  alltoallv over sim4: %d words wrong\n", bad);
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
    {
    /* the sim communicator needs every rank to post before any waits: drive phase by phase */
    for (int r = r0; r < r1; r++) dist_fwd_pre(&pl[r], rx[r], 0);
    for (int r = r0; r < r1; r++) dist_fwd_post(&pl[r], rx[r], 0);
    for (int r = r0; r < r1; r++) dist_fwd_pre(&pl[r], ry[r], 0);
    for (int r = r0; r < r1; r++) dist_fwd_post(&pl[r], ry[r], 0);
    for (int r = r0; r < r1; r++) dist_pw(&pl[r], rx[r], ry[r], 0);
    for (int r = r0; r < r1; r++) { if (tinv) dist_inv_t_pre(&pl[r], rx[r], 0); else dist_inv_pre(&pl[r], rx[r], 0); }
    for (int r = r0; r < r1; r++) { if (tinv) dist_inv_t_post(&pl[r], rx[r], 0); else dist_inv_post(&pl[r], rx[r], 0); }
    HIP_CHECK(hipDeviceSynchronize());
    }
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
        double t0 = tnow(), t1, t2;
        dist_fwd(&pl, rx, s); HIP_CHECK(hipStreamSynchronize(s)); t1 = tnow();
        dist_fwd(&pl, ry, s); dist_pw(&pl, rx, ry, s);
        if (tinv) dist_inv_t(&pl, rx, s); else dist_inv(&pl, rx, s);
        HIP_CHECK(hipStreamSynchronize(s)); t2 = tnow();
        if (logn >= 26 && r == 0) printf("  xgmi 2^%d (%d chunks): fwd %.4f s, fwd+fwd+pw+inv %.4f s%s\n", logn, pl.K, t1 - t0, t2 - t0, dist_st.on ? "" : " (DIST_STATS=1 for the breakdown)");
        comm_barrier(cm);
        /* the exposed exchange = the compute stream's idle time inside the transforms (total - the kernels' time); the
         * host time blocked in post + wait also covers the packs the exchanges wait for, so it is only an upper bound */
        if (dist_st.on && r == 0 && logn >= 26) {
            double comp = (dist_st.t_local1 + dist_st.t_local2 + dist_st.t_pack) / 4, tot = dist_st.t_total / 4, xf = dist_st.t_xfer / 4, exp_ = tot - comp;
            printf("  DIST_STATS (four ranks summed / 4, three transforms): rows %.4f cols %.4f pack %.4f exchange %.4f, total %.4f s: exposed exchange %.4f (%.0f %% hidden), host blocked %.4f\n",
                   dist_st.t_local1 / 4, dist_st.t_local2 / 4, dist_st.t_pack / 4, xf, tot, exp_, xf > 0 ? 100 * (1 - exp_ / xf) : 0, dist_st.t_a2a / 4);
        }
        if (!check_allgather(cm, 0)) { printf("  xgmi allgather failed on rank %d\n", r); bad_ag = 1; }
        if (!check_alltoallv(cm)) { printf("  xgmi alltoallv failed on rank %d\n", r); bad_ag = 1; }
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
/* ---- Phase 13a X (PLAN.md 29 E9 part 1, H4): the measurement drivers ---- */
extern "C" void comm_layered_stats_report(const char *tag);
extern "C" void comm_xgmi_h4(int me, int mode, size_t bytes, int blocks, int reps, double *out);
/* H4 (DIST_H4=1): the xGMI push's link concurrency, comm_xgmi_h4's modes at DIST_H4_MB per pair (default 256) */
static void h4_bench(void)
{
    static const char *name[] = { "all-to-all (the push, b%3)", "all-to-all, contiguous thirds", "fan-out: APU 0 -> 3 peers", "one link 0->1, 3x blocks",
                                  "one link 0->1, blocks", "one link both ways 0<->1", "all-to-all, 3 kernels / 3 streams", "all-to-all, hipMemcpyAsync / 3 streams",
                                  "all-to-all in 3 rounds (one link at a time)" };
    size_t mb = getenv("DIST_H4_MB") ? atol(getenv("DIST_H4_MB")) : 256, bytes = mb << 20; int reps = getenv("DIST_H4_REPS") ? atoi(getenv("DIST_H4_REPS")) : 10;
    int blist[] = { 19, 38, 76, 152, 304 };
    printf("H4: %zu MiB per (sender, peer), %d repetitions; per APU: wall GB/s = bytes sent / wall; per link = the stamped active time\n", mb, reps);
    double res[4][6];
#pragma omp parallel num_threads(4)
    {
        int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); comm *xg = comm_xgmi_create(d);
        for (int mode = 0; mode < 9; mode++)
            for (int bi = 0; bi < 5; bi++) {
                int blocks = blist[bi];
                if (mode != 0 && mode != 3 && mode != 4 && blocks != 76) continue;   /* the block sweep on the all-to-all and the one-link forms */
                comm_xgmi_h4(d, mode, bytes, blocks, reps, res[d]);
#pragma omp barrier
#pragma omp master
                {
                    printf("H4 mode %d %-44s blocks/peer %3d:", mode, name[mode], blocks);
                    for (int r = 0; r < 4; r++) if (res[r][1] > 0) {
                        printf("  APU%d %.1f GB/s", r, res[r][1] / res[r][0] * 1e-9);
                        int any = 0; for (int q = 0; q < 4; q++) if (res[r][2 + q] > 0) { printf("%s%d:%.1f", any ? "," : " [", q, res[r][2 + q]); any = 1; }
                        if (any) printf("]");
                    }
                    double agg = 0; for (int r = 0; r < 4; r++) if (res[r][1] > 0) agg += res[r][1] / res[r][0];
                    printf("  | node %.1f GB/s\n", agg * 1e-9); fflush(stdout);
                }
#pragma omp barrier
            }
        comm_destroy(xg);
    }
}
/* E9 part 1 (DIST_LBENCH=26,28,30 under DIST_LAYERED): timed layered transforms of one plane over the transform nodes -- per size,
 * DIST_LREPS (3) products' worth of transforms (fwd x, fwd y, inverse with the pointwise fused) after one warm-up, then the layered
 * communicator's stage report (COMM_LAYER_STATS=1|2) and the DIST_STATS parts */
static void layered_bench(int logn, int reps)
{
    int sz = mn_size(), L = 0; while ((1 << L) < sz) L++;
    mn_group *G = mn_group_at(L); int gt = G->gt;
    if (mn_rank() >= gt) return;
    int logR = logn / 2, logC = logn - logR; size_t n = (size_t)1 << logn, rows = n / (4 * (size_t)gt);
    double tp[4] = { 0, 0, 0, 0 };
    memset(&dist_st, 0, sizeof dist_st); dist_st.on = getenv("DIST_STATS") && atoi(getenv("DIST_STATS"));
#pragma omp parallel num_threads(4)
    {
        int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d));
        comm *xg = comm_xgmi_create(d), *cm = comm_layered_create(xg, G->tr[d], d); ntt_ctx *ctx = ntt_ctx_create(0); hipStream_t s; HIP_CHECK(hipStreamCreate(&s));
        dist_plan pl; dist_plan_create(&pl, cm, ctx, 0, logR, logC);
        uint64_t *x, *y; HIP_CHECK(hipMalloc(&x, rows * 8)); HIP_CHECK(hipMalloc(&y, rows * 8)); HIP_CHECK(hipMemset(x, 1, rows * 8)); HIP_CHECK(hipMemset(y, 2, rows * 8));
        for (int it = -1; it < reps; it++) {
            if (it == 0) {
                HIP_CHECK(hipStreamSynchronize(s)); comm_barrier(cm);
#pragma omp barrier
#pragma omp master
                { comm_layered_stats_report(0); memset(&dist_st, 0, sizeof dist_st); dist_st.on = getenv("DIST_STATS") && atoi(getenv("DIST_STATS")); }
#pragma omp barrier
                tp[d] = tnow();
            }
            dist_fwd(&pl, x, s); dist_fwd(&pl, y, s); dist_inv_pw(&pl, x, y, s);
            HIP_CHECK(hipMemset(x, 1, rows * 8));             /* (the values do not matter; keep them bounded) */
        }
        HIP_CHECK(hipStreamSynchronize(s)); comm_barrier(cm);
        tp[d] = tnow() - tp[d];
#pragma omp barrier
#pragma omp master
        {
            char tag[64]; snprintf(tag, sizeof tag, "2^%d x %d ranks", logn, 4 * gt);
            printf("lbench 2^%d over %d x 4 ranks (%d chunks): %.4f s per plane product (3 transforms), %d reps\n", logn, gt, pl.K, tp[0] / reps, reps);
            if (dist_st.on) printf("lbench 2^%d DIST_STATS (sum / 4 per plane product): rows %.4f cols %.4f pack %.4f exchange(device) %.4f total %.4f host-blocked %.4f\n", logn,
                                   dist_st.t_local1 / 4 / reps, dist_st.t_local2 / 4 / reps, dist_st.t_pack / 4 / reps, dist_st.t_xfer / 4 / reps, dist_st.t_total / 4 / reps, dist_st.t_a2a / 4 / reps);
            comm_layered_stats_report(tag);
        }
#pragma omp barrier
        dist_plan_free(&pl); comm_destroy(cm); comm_destroy(xg); ntt_ctx_free(ctx); HIP_CHECK(hipStreamDestroy(s)); HIP_CHECK(hipFree(x)); HIP_CHECK(hipFree(y));
    }
    HIP_CHECK(hipSetDevice(0));
}
int main(int argc, char **argv)
{
    int logmax = argc > 1 ? atoi(argv[1]) : 24;
    harness_meta("t_dist");
    if (getenv("DIST_H4") && atoi(getenv("DIST_H4"))) { h4_bench(); return 0; }
    xgmi = getenv("DIST_XGMI") && atoi(getenv("DIST_XGMI"));
    tinv = getenv("DIST_TINV") && atoi(getenv("DIST_TINV"));
    if (tinv) printf("t_dist: transposed inverse\n");
    if (xgmi) printf("t_dist: four real APUs over xGMI\n");
    if (getenv("DIST_LAYERED") && atoi(getenv("DIST_LAYERED"))) {   /* M3: the layered communicator, one node-process per COMM_RANK driving four APUs (mnrun.sh) */
        int sz = mn_init(); printf("t_dist: layered communicator, node %d of %d\n", mn_rank(), sz);
        if (getenv("DIST_LBENCH")) {                    /* E9 part 1: the timed layered transforms only */
            int reps = getenv("DIST_LREPS") ? atoi(getenv("DIST_LREPS")) : 3;
            char *dup = strdup(getenv("DIST_LBENCH"));
            for (char *t = strtok(dup, ","); t; t = strtok(NULL, ",")) { layered_bench(atoi(t), reps); mn_barrier(); }
            free(dup); mn_barrier(); mn_finalize(); return 0;
        }
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
                    ok = check_alltoallv(cm) && ok;
                    comm_destroy(cm); comm_destroy(xg);
                }
                HIP_CHECK(hipSetDevice(0));
            }
            VERIFY(ok, "layered allgather + alltoallv");
            VERIFY(check_allgather(mn_comm(0), 0), "mesh allgather");
            VERIFY(check_alltoallv(mn_comm(0)), "mesh alltoallv");
        }
        mn_barrier(); mn_finalize();
        return verify_done("t_dist");
    }
    if (!xgmi && getenv("COMM_RANK")) {                 /* one process per rank over TCP (WP6); rank r uses APU r mod 4 */
        int rk = atoi(getenv("COMM_RANK")), nd = 1; HIP_CHECK(hipGetDeviceCount(&nd)); HIP_CHECK(hipSetDevice(rk % nd));
        int shm = getenv("COMM_TRANSPORT") && !strcmp(getenv("COMM_TRANSPORT"), "shmem");   /* Phase 11 S: the same mode over the SHMEM transport (mnrun.sh) */
        tcp = shm ? comm_shmem_create_at(0, 1, comm_shmem_init(), 0) : comm_tcp_create();
        printf("t_dist: rank %d of %d over %s\n", rk, comm_size(tcp), shm ? "SHMEM" : "TCP");
        VERIFY(check_allgather(tcp, 0), "tcp allgather");
        VERIFY(check_alltoallv(tcp), "tcp alltoallv");
    }
    if (!xgmi && !tcp) { VERIFY(check_allgather_sim4(), "sim4 allgather"); VERIFY(check_alltoallv_sim4(), "sim4 alltoallv"); }
    if (!xgmi && !tcp) { comm *lc = comm_local_create(); VERIFY(check_alltoallv(lc), "local alltoallv"); comm_destroy(lc); }
    for (int prime = 0; prime < 4; prime++)
        for (int logR = 10; logR <= 13; logR++)
            for (int logC = 10; logC <= 13; logC++)
                if (logR + logC <= logmax) VERIFY(one(prime, logR, logC), "dist conv prime %d %dx%d", prime, logR, logC);
    if (logmax >= 26) VERIFY(one(0, 13, 13), "dist conv 2^26");
    if (logmax >= 30) VERIFY(one(1, 15, 15), "dist conv 2^30");
    if (logmax >= 31) VERIFY(one(2, 16, 15), "dist conv 2^31");
    if (tcp) { int shm = getenv("COMM_TRANSPORT") && !strcmp(getenv("COMM_TRANSPORT"), "shmem"); comm_destroy(tcp); if (shm) comm_shmem_finalize(); }
    return verify_done("t_dist");
}
