/* 05_ntt_reg - register-blocked NTT: cut LDS round trips from 11 to 3.
 *
 * bench/04 showed a straightforward LDS radix-2 NTT sustains only ~41% of the
 * register-resident butterfly rate, while moving just 5.3 TB/s of HBM.  It is
 * bound by neither memory nor arithmetic, so the suspects are LDS traffic and
 * barriers: the baseline does one LDS read-modify-write pair and one
 * __syncthreads per stage, 11 of each for N=2048.
 *
 * This holds 8 points per thread in registers, so each group of 3 radix-2
 * stages runs entirely in registers and LDS is used only as a transpose buffer
 * between groups.  N=2048 = 3+3+3+2 stages -> 3 exchanges, 3 barriers.
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
#include "common_ntt.h"

#define PRIME 0x3FFEDF0000000001ULL
#define PROOT 11ULL
#define N       2048
#define THREADS 256
#define RPT     8            /* registers (points) per thread */

__device__ static inline uint64_t shoup_mul(uint64_t w, uint64_t wp,
                                            uint64_t y, uint64_t p)
{
    uint64_t q = __umul64hi(wp, y);
    return w * y - q * p;
}

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
static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t p)
{ return (uint64_t)((__uint128_t)a * b % p); }

static uint64_t powmod(uint64_t a, uint64_t e, uint64_t p)
{
    uint64_t r = 1;
    a %= p;
    while (e) { if (e & 1) r = mulmod(r, a, p); a = mulmod(a, a, p); e >>= 1; }
    return r;
}

static uint64_t shoup_pre(uint64_t w, uint64_t p)
{ return (uint64_t)(((__uint128_t)w << 64) / p); }

static int brv(int i, int bits)
{
    int r = 0, k;
    for (k = 0; k < bits; k++) if (i & (1 << k)) r |= 1 << (bits - 1 - k);
    return r;
}

static void build_table(uint64_t *w, uint64_t *wp, int n, uint64_t root, uint64_t p)
{
    int m, i;
    for (m = 1; m < n; m <<= 1) {
        int lgm = 0;
        while ((1 << lgm) < m) lgm++;
        for (i = 0; i < m; i++) {
            uint64_t e = (uint64_t)(n / (2 * m)) * (uint64_t)brv(i, lgm);
            w[m + i] = powmod(root, e, p);
            wp[m + i] = shoup_pre(w[m + i], p);
        }
    }
    w[0] = 1; wp[0] = shoup_pre(1, p);
}

int main(int argc, char **argv)
{
    uint64_t p = PRIME, *hw, *hwp, *ha, *hbase, *hreg;
    uint64_t *dd[MAXD], *dw[MAXD], *dwp[MAXD];
    double rr[MAXD], rb[MAXD], bwr[MAXD];
    hipDeviceProp_t pr;
    double mib = argc > 1 ? atof(argv[1]) : 512.0;
    size_t bytes, npt;
    int nd = device_count(), blocks, i, d, ok;

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    bytes = (size_t)(mib * 1048576.0);
    npt = bytes / sizeof(uint64_t);
    blocks = (int)(npt / N);

    printf("== 05_ntt_reg : register-blocked vs LDS-resident NTT, N=%d ==\n", N);
    meta("05_ntt_reg");
    printf("%d APUs, %.0f MiB/APU, %d blocks x %d threads, %d points/thread\n",
           nd, mib, blocks, THREADS, RPT);
    printf("bench/01 register-resident bound: 3886 Gbfly/s;  bench/04 LDS: ~1600\n");

    hw = (uint64_t *)malloc(N * sizeof(uint64_t));
    hwp = (uint64_t *)malloc(N * sizeof(uint64_t));
    build_table(hw, hwp, N, powmod(PROOT, (p - 1) / (uint64_t)N, p), p);

    /* correctness: the register kernel must agree with the bench/04 kernel,
     * which was itself verified against schoolbook cyclic convolution */
    ha = (uint64_t *)malloc(N * sizeof(uint64_t));
    hbase = (uint64_t *)malloc(N * sizeof(uint64_t));
    hreg = (uint64_t *)malloc(N * sizeof(uint64_t));
    for (i = 0; i < N; i++) ha[i] = (uint64_t)(i * 2654435761u + 12345u) % p;
    {
        uint64_t *da, *dwf, *dwfp;
        HIP_CHECK(hipSetDevice(0));
        HIP_CHECK(hipMalloc(&da, N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&dwf, N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&dwfp, N * sizeof(uint64_t)));
        HIP_CHECK(hipMemcpy(dwf, hw, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dwfp, hwp, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(da, ha, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        k_ntt_lds<<<1, THREADS>>>(da, dwf, dwfp, p);
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(hbase, da, N * sizeof(uint64_t), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(da, ha, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        k_ntt_reg<<<1, THREADS>>>(da, dwf, dwfp, p);
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(hreg, da, N * sizeof(uint64_t), hipMemcpyDeviceToHost));
        ok = 1;
        for (i = 0; i < N; i++) if (hbase[i] != hreg[i]) { ok = 0; break; }
        printf("\nVERIFY register kernel == verified LDS kernel: %s", ok ? "OK" : "FAILED");
        if (!ok) printf(" (index %d: reg %llu vs lds %llu)", i,
                        (unsigned long long)hreg[i], (unsigned long long)hbase[i]);
        printf("\n");
        HIP_CHECK(hipFree(da)); HIP_CHECK(hipFree(dwf)); HIP_CHECK(hipFree(dwfp));
    }

    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&dd[d], bytes));
        HIP_CHECK(hipMalloc(&dw[d], N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&dwp[d], N * sizeof(uint64_t)));
        HIP_CHECK(hipMemcpy(dw[d], hw, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dwp[d], hwp, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemset(dd[d], 1, bytes));
    }

    header("kernel");
#pragma omp parallel num_threads(nd)
    {
        int dev = omp_get_thread_num(), rep;
        hipEvent_t t0, t1;
        double best;
        float ms;
        HIP_CHECK(hipSetDevice(dev));
        timer_events(&t0, &t1);

        k_ntt_lds<<<blocks, THREADS>>>(dd[dev], dw[dev], dwp[dev], p);
        HIP_CHECK(hipDeviceSynchronize());
        best = 1e300;
        for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_ntt_lds<<<blocks, THREADS>>>(dd[dev], dw[dev], dwp[dev], p);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best = dmin(best, (double)ms);
        }
        rb[dev] = (double)blocks * (N / 2) * 11 / (best * 1e-3) / 1e9;

        k_ntt_reg<<<blocks, THREADS>>>(dd[dev], dw[dev], dwp[dev], p);
        HIP_CHECK(hipDeviceSynchronize());
        best = 1e300;
        for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_ntt_reg<<<blocks, THREADS>>>(dd[dev], dw[dev], dwp[dev], p);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best = dmin(best, (double)ms);
        }
        rr[dev] = (double)blocks * (N / 2) * 11 / (best * 1e-3) / 1e9;
        bwr[dev] = 2.0 * bytes / (best * 1e-3) / 1e9;
    }
    report_sum("LDS radix-2 (baseline)", "Gbfly/s", rb, nd);
    report_sum("register-blocked", "Gbfly/s", rr, nd);
    report_sum("   HBM traffic", "GB/s", bwr, nd);
    printf("\nspeedup: %.2fx\n",
           (rr[0] + rr[1] + rr[2] + rr[3]) / (rb[0] + rb[1] + rb[2] + rb[3]));
    return 0;
}
