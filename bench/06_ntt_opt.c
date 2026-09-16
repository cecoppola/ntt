/* 06_ntt_opt - fix the LDS bank conflicts the register blocking introduced.
 *
 * bench/05 cut LDS round trips from 11 to 3 and gained 1.43x, but still only
 * reached 56% of the register-resident bound at 6.3 TB/s of HBM -- neither
 * memory- nor arithmetic-bound.  Counting bank classes explains it: element e
 * of 8 bytes occupies banks 2e,2e+1 of 32, so the conflict class is e mod 16,
 * and three of the six exchange phases collapse 64 lanes onto 4 or 2 classes:
 *
 *   A->B write/read, B->C write   16 classes   (the 64-bit floor)
 *   B->C read,  C->D write         4 classes   16-way conflict
 *   C->D read                      2 classes   32-way conflict
 *
 * An XOR swizzle e ^ ((e>>4)&15) -- a bijection, so no extra LDS -- restores
 * all six to 16 classes.  The node report measured 9.0x recovery for the XOR
 * swizzle against 4.8x for padding at stride 32.
 *
 * Variants measured here:
 *   reg          bench/05 as-is
 *   reg+swizzle  same, XOR-swizzled LDS indices
 *   reg+swz+ldsW twiddle table staged in LDS instead of read from global
 *   reg, no-tw   twiddles replaced by a constant: wrong math, but it isolates
 *                how much of the remaining gap is twiddle-load traffic
 *
 * Layouts (tid in [0,256), k in [0,8)):
 *   A  stride 256 : E_k = tid + 256k              stages h=1024,512,256
 *   B  stride  32 : F_k = 256*(tid>>5) + (tid&31) + 32k    h=128,64,32
 *   C  stride   4 : G_k =  32*(tid>>2) + (tid&3)  +  4k    h=16,8,4
 *   D  contiguous : H_k =   8*tid + k                      h=2,1
 *
 * For a stage of half-size h the lower element j of a pair (j, j+h) uses
 * twiddle w[m + (j >> (log2(h)+1))], m = N/(2h) -- the same recurrence bench/04
 * verified against schoolbook convolution.
 *
 * Usage: 05_ntt_reg [batch_MiB]
 */
#include "ntt_kernels.h"

#define N       2048

/* element e of 8 bytes sits in banks 2e,2e+1 of 32, so class = e mod 16;
 * permuting the low 4 bits by bits 4..7 is a bijection that spreads them */
#define THREADS 256
#define RPT     8            /* registers (points) per thread */

#define shoup_mul smul

/* Harvey Alg.4 in registers: x[a],x[b] in [0,4p) -> [0,4p) */
#define BFLY(a, b, wi)                                                        \
    do {                                                                      \
        uint64_t U = x[a], V;                                                 \
        if (U >= p2) U -= p2;                                                 \
        V = shoup_mul(w[wi], wp[wi], x[b], p);                                \
        x[a] = U + V;                                                         \
        x[b] = U - V + p2;                                                    \
    } while (0)

/* one group of three radix-2 stages on 8 registers: strides 4, 2, 1 in k */
#define GROUP3(base0, base1, base2)                                           \
    do {                                                                      \
        BFLY(0, 4, (base0)); BFLY(1, 5, (base0));                             \
        BFLY(2, 6, (base0)); BFLY(3, 7, (base0));                             \
        BFLY(0, 2, (base1) + 0); BFLY(1, 3, (base1) + 0);                     \
        BFLY(4, 6, (base1) + 1); BFLY(5, 7, (base1) + 1);                     \
        BFLY(0, 1, (base2) + 0); BFLY(2, 3, (base2) + 1);                     \
        BFLY(4, 5, (base2) + 2); BFLY(6, 7, (base2) + 3);                     \
    } while (0)

__global__ __launch_bounds__(THREADS)
void k_ntt_reg(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N];
    uint64_t *g = data + (size_t)blockIdx.x * N;
    uint64_t x[RPT];
    uint64_t p2 = 2 * p;
    int tid = threadIdx.x, k, span, base, grp, b2;

    /* load: lane-consecutive, one 8-byte element each, 8 times */
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = g[tid + 256 * k];

    /* group A: h = 1024, 512, 256; twiddles w[1], w[2+..], w[4+..] */
    GROUP3(1, 2, 4);

    /* exchange A -> B */
#pragma unroll
    for (k = 0; k < RPT; k++) s[tid + 256 * k] = x[k];
    __syncthreads();
    span = tid >> 5; base = tid & 31;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = s[256 * span + base + 32 * k];
    __syncthreads();

    /* group B: h = 128, 64, 32 */
    GROUP3(8 + span, 16 + 2 * span, 32 + 4 * span);

    /* exchange B -> C */
#pragma unroll
    for (k = 0; k < RPT; k++) s[256 * span + base + 32 * k] = x[k];
    __syncthreads();
    grp = tid >> 2; b2 = tid & 3;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = s[32 * grp + b2 + 4 * k];
    __syncthreads();

    /* group C: h = 16, 8, 4 */
    GROUP3(64 + grp, 128 + 2 * grp, 256 + 4 * grp);

    /* exchange C -> D */
#pragma unroll
    for (k = 0; k < RPT; k++) s[32 * grp + b2 + 4 * k] = x[k];
    __syncthreads();
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = s[8 * tid + k];

    /* group D: h = 2, 1 (only two stages; strides 2 and 1 in k) */
    BFLY(0, 2, 512 + 2 * tid + 0); BFLY(1, 3, 512 + 2 * tid + 0);
    BFLY(4, 6, 512 + 2 * tid + 1); BFLY(5, 7, 512 + 2 * tid + 1);
    BFLY(0, 1, 1024 + 4 * tid + 0); BFLY(2, 3, 1024 + 4 * tid + 1);
    BFLY(4, 5, 1024 + 4 * tid + 2); BFLY(6, 7, 1024 + 4 * tid + 3);

    /* store: 8 contiguous per lane = one full 64-byte line per lane */
#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t v = x[k];
        if (v >= p2) v -= p2;
        if (v >= p)  v -= p;
        g[8 * tid + k] = v;
    }
}

__global__ __launch_bounds__(THREADS)
void k_ntt_swz(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N];
    uint64_t *g = data + (size_t)blockIdx.x * N;
    uint64_t x[RPT];
    uint64_t p2 = 2 * p;
    int tid = threadIdx.x, k, span, base, grp, b2;

    /* load: lane-consecutive, one 8-byte element each, 8 times */
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = g[tid + 256 * k];

    /* group A: h = 1024, 512, 256; twiddles w[1], w[2+..], w[4+..] */
    GROUP3(1, 2, 4);

    /* exchange A -> B */
#pragma unroll
    for (k = 0; k < RPT; k++) s[SWZ(tid + 256 * k)] = x[k];
    __syncthreads();
    span = tid >> 5; base = tid & 31;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = s[SWZ(256 * span + base + 32 * k)];
    __syncthreads();

    /* group B: h = 128, 64, 32 */
    GROUP3(8 + span, 16 + 2 * span, 32 + 4 * span);

    /* exchange B -> C */
#pragma unroll
    for (k = 0; k < RPT; k++) s[SWZ(256 * span + base + 32 * k)] = x[k];
    __syncthreads();
    grp = tid >> 2; b2 = tid & 3;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = s[SWZ(32 * grp + b2 + 4 * k)];
    __syncthreads();

    /* group C: h = 16, 8, 4 */
    GROUP3(64 + grp, 128 + 2 * grp, 256 + 4 * grp);

    /* exchange C -> D */
#pragma unroll
    for (k = 0; k < RPT; k++) s[SWZ(32 * grp + b2 + 4 * k)] = x[k];
    __syncthreads();
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = s[SWZ(8 * tid + k)];

    /* group D: h = 2, 1 (only two stages; strides 2 and 1 in k) */
    BFLY(0, 2, 512 + 2 * tid + 0); BFLY(1, 3, 512 + 2 * tid + 0);
    BFLY(4, 6, 512 + 2 * tid + 1); BFLY(5, 7, 512 + 2 * tid + 1);
    BFLY(0, 1, 1024 + 4 * tid + 0); BFLY(2, 3, 1024 + 4 * tid + 1);
    BFLY(4, 5, 1024 + 4 * tid + 2); BFLY(6, 7, 1024 + 4 * tid + 3);

    /* store: 8 contiguous per lane = one full 64-byte line per lane */
#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t v = x[k];
        if (v >= p2) v -= p2;
        if (v >= p)  v -= p;
        g[8 * tid + k] = v;
    }
}


/* twiddle ablation: identical instruction mix minus the table loads */
#define BFLYC(a, b, wi)                                                       \
    do {                                                                      \
        uint64_t U = x[a], V;                                                 \
        if (U >= p2) U -= p2;                                                 \
        V = shoup_mul(w0, wp0, x[b], p);                                      \
        x[a] = U + V;                                                         \
        x[b] = U - V + p2;                                                    \
    } while (0)
#define GROUP3C(base0, base1, base2)                                          \
    do {                                                                      \
        BFLYC(0, 4, (base0)); BFLYC(1, 5, (base0));                           \
        BFLYC(2, 6, (base0)); BFLYC(3, 7, (base0));                           \
        BFLYC(0, 2, (base1) + 0); BFLYC(1, 3, (base1) + 0);                   \
        BFLYC(4, 6, (base1) + 1); BFLYC(5, 7, (base1) + 1);                   \
        BFLYC(0, 1, (base2) + 0); BFLYC(2, 3, (base2) + 1);                   \
        BFLYC(4, 5, (base2) + 2); BFLYC(6, 7, (base2) + 3);                   \
    } while (0)
__global__ __launch_bounds__(THREADS)
void k_ntt_notw(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N];
    uint64_t *g = data + (size_t)blockIdx.x * N;
    uint64_t x[RPT];
    uint64_t p2 = 2 * p, w0 = w[1], wp0 = wp[1];
    int tid = threadIdx.x, k, span, base, grp, b2;

    /* load: lane-consecutive, one 8-byte element each, 8 times */
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = g[tid + 256 * k];

    /* group A: h = 1024, 512, 256; twiddles w[1], w[2+..], w[4+..] */
    GROUP3C(1, 2, 4);

    /* exchange A -> B */
#pragma unroll
    for (k = 0; k < RPT; k++) s[tid + 256 * k] = x[k];
    __syncthreads();
    span = tid >> 5; base = tid & 31;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = s[256 * span + base + 32 * k];
    __syncthreads();

    /* group B: h = 128, 64, 32 */
    GROUP3C(8 + span, 16 + 2 * span, 32 + 4 * span);

    /* exchange B -> C */
#pragma unroll
    for (k = 0; k < RPT; k++) s[256 * span + base + 32 * k] = x[k];
    __syncthreads();
    grp = tid >> 2; b2 = tid & 3;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = s[32 * grp + b2 + 4 * k];
    __syncthreads();

    /* group C: h = 16, 8, 4 */
    GROUP3C(64 + grp, 128 + 2 * grp, 256 + 4 * grp);

    /* exchange C -> D */
#pragma unroll
    for (k = 0; k < RPT; k++) s[32 * grp + b2 + 4 * k] = x[k];
    __syncthreads();
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = s[8 * tid + k];

    /* group D: h = 2, 1 (only two stages; strides 2 and 1 in k) */
    BFLYC(0, 2, 512 + 2 * tid + 0); BFLYC(1, 3, 512 + 2 * tid + 0);
    BFLYC(4, 6, 512 + 2 * tid + 1); BFLYC(5, 7, 512 + 2 * tid + 1);
    BFLYC(0, 1, 1024 + 4 * tid + 0); BFLYC(2, 3, 1024 + 4 * tid + 1);
    BFLYC(4, 5, 1024 + 4 * tid + 2); BFLYC(6, 7, 1024 + 4 * tid + 3);

    /* store: 8 contiguous per lane = one full 64-byte line per lane */
#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t v = x[k];
        if (v >= p2) v -= p2;
        if (v >= p)  v -= p;
        g[8 * tid + k] = v;
    }
}

/* baseline from bench/04, for a same-run comparison */
__global__ __launch_bounds__(THREADS)
void k_ntt_lds(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N];
    uint64_t *g = data + (size_t)blockIdx.x * N;
    uint64_t p2 = 2 * p;
    int tid = threadIdx.x, e, m, h, logh, b;

    for (e = tid; e < N; e += THREADS) s[e] = g[e];
    __syncthreads();
    logh = 10;
    for (m = 1, h = N / 2; m < N; m <<= 1, h >>= 1, logh--) {
        for (b = tid; b < N / 2; b += THREADS) {
            int i = b >> logh;
            int j = ((b >> logh) << (logh + 1)) | (b & (h - 1));
            uint64_t U = s[j], V;
            if (U >= p2) U -= p2;
            V = shoup_mul(w[m + i], wp[m + i], s[j + h], p);
            s[j] = U + V;
            s[j + h] = U - V + p2;
        }
        __syncthreads();
    }
    for (e = tid; e < N; e += THREADS) {
        uint64_t v = s[e];
        if (v >= p2) v -= p2;
        if (v >= p)  v -= p;
        g[e] = v;
    }
}

/* ------------------------------ host ------------------------------------ */

typedef void (*nttk_t)(uint64_t *, const uint64_t *, const uint64_t *, uint64_t);

#define NV 4

int main(int argc, char **argv)
{
    const char *names[NV] = { "LDS radix-2 (bench/04)", "register-blocked (bench/05)",
                              "register + XOR swizzle", "register, twiddles ablated" };
    nttk_t K[NV] = { k_ntt_lds, k_ntt_reg, k_ntt_swz, k_ntt_notw };
    int check[NV] = { 1, 1, 1, 0 };      /* the ablation is wrong math by design */
    uint64_t p = PRIME, *hw, *hwp, *ha, *href, *hgot;
    uint64_t *dd[MAXD], *dw[MAXD], *dwp[MAXD];
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

    printf("== 06_ntt_opt : LDS bank conflicts in the register-blocked NTT ==\n");
    meta("06_ntt_opt");
    printf("%d APUs, %.0f MiB/APU, %d blocks x %d threads, %d points/thread, N=%d\n",
           nd, mib, blocks, THREADS, RPT, N);
    printf("bounds: 3886 Gbfly/s register-resident (bench/01)\n");

    hw = (uint64_t *)malloc(N * sizeof(uint64_t));
    hwp = (uint64_t *)malloc(N * sizeof(uint64_t));
    build_table(hw, hwp, N, powmod(PROOT, (p - 1) / (uint64_t)N, p), p);

    ha = (uint64_t *)malloc(N * sizeof(uint64_t));
    href = (uint64_t *)malloc(N * sizeof(uint64_t));
    hgot = (uint64_t *)malloc(N * sizeof(uint64_t));
    for (i = 0; i < N; i++) ha[i] = (uint64_t)(i * 2654435761u + 12345u) % p;

    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&dd[d], bytes));
        HIP_CHECK(hipMalloc(&dw[d], N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&dwp[d], N * sizeof(uint64_t)));
        HIP_CHECK(hipMemcpy(dw[d], hw, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dwp[d], hwp, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemset(dd[d], 1, bytes));
    }

    /* every real variant must reproduce the kernel bench/04 verified against
     * schoolbook cyclic convolution */
    HIP_CHECK(hipSetDevice(0));
    for (v = 0; v < NV; v++) {
        if (!check[v]) continue;
        HIP_CHECK(hipMemcpy(dd[0], ha, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        K[v]<<<1, THREADS>>>(dd[0], dw[0], dwp[0], p);
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(hgot, dd[0], N * sizeof(uint64_t), hipMemcpyDeviceToHost));
        if (v == 0) { memcpy(href, hgot, N * sizeof(uint64_t)); continue; }
        ok = 1;
        for (i = 0; i < N; i++) if (hgot[i] != href[i]) { ok = 0; break; }
        printf("VERIFY %-28s : %s\n", names[v], ok ? "OK" : "FAILED");
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
            K[v]<<<blocks, THREADS>>>(dd[dev], dw[dev], dwp[dev], p);
            HIP_CHECK(hipDeviceSynchronize());
            for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                HIP_CHECK(hipEventRecord(t0, 0));
                K[v]<<<blocks, THREADS>>>(dd[dev], dw[dev], dwp[dev], p);
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
        printf("  %-26s %8.2fx over LDS baseline, %.0f%% of register bound, HBM %.0f GB/s\n",
               "", tot / base, 100.0 * tot / 3886.0, bw[0] + bw[1] + bw[2] + bw[3]);
    }
    return 0;
}
