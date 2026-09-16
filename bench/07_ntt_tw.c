/* 07_ntt_tw - get the twiddle traffic off the critical path.
 *
 * bench/06 ablation: replacing every twiddle with a constant lifted the
 * register-blocked NTT from 2325 to 2817 Gbfly/s, so twiddle loads cost 21%.
 * Counting them explains why.  Per block (N=2048, 16 KiB of data):
 *
 *   27 distinct twiddle indices per thread, held in two arrays w[] and wp[],
 *   so 54 loads and 432 B per thread = 108 KiB of twiddles per 16 KiB of data.
 *   The table itself is 32 KiB -- exactly the L1 -- and the 4 blocks/CU stream
 *   128 KiB of data through that same L1, evicting it.
 *   Groups A,B,C together need only w[1..511] (8 KiB); group D needs
 *   w[512..2047] (24 KiB, 75%) with no reuse inside a block.
 *
 * Three fixes, measured independently:
 *   iw    interleave w and wp into one ulong2, halving load instructions
 *   nt    non-temporal loads/stores for the data, so it stops evicting L1
 *   lds   stage the hot 8 KiB (groups A,B,C) in LDS; costs occupancy
 *         (24 KiB/block -> 2 blocks/CU instead of 4)
 *
 * Usage: 07_ntt_tw [batch_MiB]
 */
#include "ntt_kernels.h"

#define N       2048
#define THREADS 256
#define RPT     8
#define TWLDS   512          /* table entries staged in LDS by the lds variant */


#define shoup_mul smul

/* Generates the four kernels from one body.
 *   IW  1 = interleaved ulong2 table, 0 = separate w[]/wp[]
 *   NT  1 = non-temporal data accesses
 *   LD  1 = groups A,B,C read the table from LDS
 */
#define DEFINE_KERNEL(NAME, IW, NT, LD)                                       \
__global__ __launch_bounds__(THREADS)                                         \
void NAME(uint64_t *data, const ulong2 *tw, const uint64_t *w,                \
          const uint64_t *wp, uint64_t p)                                     \
{                                                                             \
    __shared__ uint64_t s[N];                                                 \
    __shared__ ulong2 stw[LD ? TWLDS : 1];                                    \
    uint64_t *g = data + (size_t)blockIdx.x * N;                              \
    uint64_t x[RPT];                                                          \
    uint64_t p2 = 2 * p;                                                      \
    int tid = threadIdx.x, k, span, base, grp, b2;                            \
                                                                              \
    if (LD) {                                                                 \
        for (k = tid; k < TWLDS; k += THREADS) stw[k] = tw[k];                \
    }                                                                         \
    _Pragma("unroll")                                                         \
    for (k = 0; k < RPT; k++)                                                 \
        x[k] = NT ? __builtin_nontemporal_load(&g[tid + 256 * k])             \
                  : g[tid + 256 * k];                                         \
    if (LD) __syncthreads();                                                  \
                                                                              \
    GROUP3(IW, LD, 1, 2, 4);                                                  \
                                                                              \
    EXCH_W(tid + 256 * k);                                                    \
    span = tid >> 5; base = tid & 31;                                         \
    EXCH_R(256 * span + base + 32 * k);                                       \
    GROUP3(IW, LD, 8 + span, 16 + 2 * span, 32 + 4 * span);                   \
                                                                              \
    EXCH_W(256 * span + base + 32 * k);                                       \
    grp = tid >> 2; b2 = tid & 3;                                             \
    EXCH_R(32 * grp + b2 + 4 * k);                                            \
    GROUP3(IW, LD, 64 + grp, 128 + 2 * grp, 256 + 4 * grp);                   \
                                                                              \
    EXCH_W(32 * grp + b2 + 4 * k);                                            \
    EXCH_R(8 * tid + k);                                                      \
                                                                              \
    BFLY(IW, 0, 0, 2, 512 + 2 * tid + 0);                                     \
    BFLY(IW, 0, 1, 3, 512 + 2 * tid + 0);                                     \
    BFLY(IW, 0, 4, 6, 512 + 2 * tid + 1);                                     \
    BFLY(IW, 0, 5, 7, 512 + 2 * tid + 1);                                     \
    BFLY(IW, 0, 0, 1, 1024 + 4 * tid + 0);                                    \
    BFLY(IW, 0, 2, 3, 1024 + 4 * tid + 1);                                    \
    BFLY(IW, 0, 4, 5, 1024 + 4 * tid + 2);                                    \
    BFLY(IW, 0, 6, 7, 1024 + 4 * tid + 3);                                    \
                                                                              \
    _Pragma("unroll")                                                         \
    for (k = 0; k < RPT; k++) {                                               \
        uint64_t v = x[k];                                                    \
        if (v >= p2) v -= p2;                                                 \
        if (v >= p)  v -= p;                                                  \
        if (NT) __builtin_nontemporal_store(v, &g[8 * tid + k]);              \
        else    g[8 * tid + k] = v;                                           \
    }                                                                         \
}

/* one radix-2 butterfly, Harvey Alg.4, twiddle from LDS when L and wi < TWLDS */
#define BFLY(IW, L, a, b, wi)                                                 \
    do {                                                                      \
        uint64_t U = x[a], V, W_, WP_;                                        \
        int wi_ = (wi);                                                       \
        if (L) { W_ = stw[wi_].x; WP_ = stw[wi_].y; }                         \
        else if (IW) { ulong2 t_ = tw[wi_]; W_ = t_.x; WP_ = t_.y; }          \
        else { W_ = w[wi_]; WP_ = wp[wi_]; }                                  \
        if (U >= p2) U -= p2;                                                 \
        V = shoup_mul(W_, WP_, x[b], p);                                      \
        x[a] = U + V;                                                         \
        x[b] = U - V + p2;                                                    \
    } while (0)

#define GROUP3(IW, L, base0, base1, base2)                                    \
    do {                                                                      \
        BFLY(IW, L, 0, 4, (base0)); BFLY(IW, L, 1, 5, (base0));               \
        BFLY(IW, L, 2, 6, (base0)); BFLY(IW, L, 3, 7, (base0));               \
        BFLY(IW, L, 0, 2, (base1) + 0); BFLY(IW, L, 1, 3, (base1) + 0);       \
        BFLY(IW, L, 4, 6, (base1) + 1); BFLY(IW, L, 5, 7, (base1) + 1);       \
        BFLY(IW, L, 0, 1, (base2) + 0); BFLY(IW, L, 2, 3, (base2) + 1);       \
        BFLY(IW, L, 4, 5, (base2) + 2); BFLY(IW, L, 6, 7, (base2) + 3);       \
    } while (0)

#define EXCH_W(idx)                                                           \
    do {                                                                      \
        _Pragma("unroll")                                                     \
        for (k = 0; k < RPT; k++) s[SWZ(idx)] = x[k];                         \
        __syncthreads();                                                      \
    } while (0)

#define EXCH_R(idx)                                                           \
    do {                                                                      \
        _Pragma("unroll")                                                     \
        for (k = 0; k < RPT; k++) x[k] = s[SWZ(idx)];                         \
        __syncthreads();                                                      \
    } while (0)

DEFINE_KERNEL(k_base,   0, 0, 0)
DEFINE_KERNEL(k_iw,     1, 0, 0)
DEFINE_KERNEL(k_iw_nt,  1, 1, 0)
DEFINE_KERNEL(k_iw_lds, 1, 0, 1)
DEFINE_KERNEL(k_all,    1, 1, 1)

/* ------------------------------ host ------------------------------------ */

typedef void (*nttk_t)(uint64_t *, const ulong2 *, const uint64_t *,
                       const uint64_t *, uint64_t);
#define NV 5

int main(int argc, char **argv)
{
    const char *names[NV] = { "separate w/wp (bench/06)", "+ interleaved ulong2",
                              "+ interleaved + nontemporal", "+ interleaved + LDS table",
                              "+ all three" };
    nttk_t K[NV] = { k_base, k_iw, k_iw_nt, k_iw_lds, k_all };
    uint64_t p = PRIME, *hw, *hwp, *ha, *href, *hgot;
    ulong2 *htw;
    uint64_t *dd[MAXD], *dw[MAXD], *dwp[MAXD];
    ulong2 *dtw[MAXD];
    double rate[MAXD], bw[MAXD];
    hipDeviceProp_t pr;
    double mib = argc > 1 ? atof(argv[1]) : 512.0;
    double base = 0.0;
    size_t bytes, npt;
    int nd = device_count(), blocks, i, d, v, ok;

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    bytes = (size_t)(mib * 1048576.0);
    npt = bytes / sizeof(uint64_t);
    blocks = (int)(npt / N);

    printf("== 07_ntt_tw : twiddle traffic in the register-blocked NTT ==\n");
    meta("07_ntt_tw");
    printf("%d APUs, %.0f MiB/APU, %d blocks x %d threads, N=%d\n",
           nd, mib, blocks, THREADS, N);
    printf("bounds: 3886 register-resident, 2817 with twiddles ablated, 2325 as built\n");

    hw = (uint64_t *)malloc(N * sizeof(uint64_t));
    hwp = (uint64_t *)malloc(N * sizeof(uint64_t));
    htw = (ulong2 *)malloc(N * sizeof(ulong2));
    build_table(hw, hwp, N, powmod(PROOT, (p - 1) / (uint64_t)N, p), p);
    for (i = 0; i < N; i++) { htw[i].x = hw[i]; htw[i].y = hwp[i]; }

    ha = (uint64_t *)malloc(N * sizeof(uint64_t));
    href = (uint64_t *)malloc(N * sizeof(uint64_t));
    hgot = (uint64_t *)malloc(N * sizeof(uint64_t));
    for (i = 0; i < N; i++) ha[i] = (uint64_t)(i * 2654435761u + 12345u) % p;

    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&dd[d], bytes));
        HIP_CHECK(hipMalloc(&dw[d], N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&dwp[d], N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&dtw[d], N * sizeof(ulong2)));
        HIP_CHECK(hipMemcpy(dw[d], hw, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dwp[d], hwp, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dtw[d], htw, N * sizeof(ulong2), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemset(dd[d], 1, bytes));
    }

    HIP_CHECK(hipSetDevice(0));
    for (v = 0; v < NV; v++) {
        HIP_CHECK(hipMemcpy(dd[0], ha, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        K[v]<<<1, THREADS>>>(dd[0], dtw[0], dw[0], dwp[0], p);
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(hgot, dd[0], N * sizeof(uint64_t), hipMemcpyDeviceToHost));
        if (v == 0) { memcpy(href, hgot, N * sizeof(uint64_t)); continue; }
        ok = 1;
        for (i = 0; i < N; i++) if (hgot[i] != href[i]) { ok = 0; break; }
        printf("VERIFY %-30s : %s\n", names[v], ok ? "OK" : "FAILED");
        if (!ok) { printf("  index %d: got %llu want %llu\n", i,
                          (unsigned long long)hgot[i], (unsigned long long)href[i]); return 1; }
    }
    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMemset(dd[d], 1, bytes));
    }

    header("variant");
    for (v = 0; v < NV; v++) {
        double tot = 0.0;
#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num(), rep;
            hipEvent_t t0, t1;
            double best = 1e300;
            float ms;
            HIP_CHECK(hipSetDevice(dev));
            timer_events(&t0, &t1);
            K[v]<<<blocks, THREADS>>>(dd[dev], dtw[dev], dw[dev], dwp[dev], p);
            HIP_CHECK(hipDeviceSynchronize());
            for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                HIP_CHECK(hipEventRecord(t0, 0));
                K[v]<<<blocks, THREADS>>>(dd[dev], dtw[dev], dw[dev], dwp[dev], p);
                HIP_CHECK(hipEventRecord(t1, 0));
                HIP_CHECK(hipEventSynchronize(t1));
                HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
                best = dmin(best, (double)ms);
            }
            rate[dev] = (double)blocks * (N / 2) * 11 / (best * 1e-3) / 1e9;
            bw[dev] = 2.0 * bytes / (best * 1e-3) / 1e9;
        }
        for (i = 0; i < nd; i++) tot += rate[i];
        if (v == 0) base = tot;
        report_sum(names[v], "Gbfly/s", rate, nd);
        printf("  %-26s %8.2fx, %.0f%% of register bound, HBM %.0f GB/s\n", "",
               tot / base, 100.0 * tot / 3886.0, bw[0] + bw[1] + bw[2] + bw[3]);
    }
    return 0;
}
