/* 12_ntt_inv - the missing building block: a register-blocked inverse.
 *
 * bench/05..07 register-blocked the forward (decimation-in-time) transform and
 * got 1.50x over the simple LDS kernel.  bench/08's multiply still runs the
 * simple kernel in both directions because no register-blocked
 * Gentleman-Sande inverse existed.  This adds it and verifies it three ways.
 *
 * The inverse runs the stages in the opposite order, h = 1,2,4,...,1024, so it
 * needs the mirror-image set of register layouts.  In a layout {base + s*k}
 * with k in [0,8), a stage of half-size h is register-resident iff
 * h is s, 2s or 4s:
 *
 *   G1  s=  1  {8*tid + k}                        h = 1, 2, 4
 *   G2  s=  8  {64*(tid>>3) + (tid&7)  +  8k}     h = 8, 16, 32
 *   G3  s= 64  {512*(tid>>6) + (tid&63) + 64k}    h = 64, 128, 256
 *   G4  s=256  {tid + 256k}                       h = 512, 1024
 *
 * All four are bijections on [0,2048); three exchanges, three barriers.  Two of
 * the six exchange phases have 32-way and 8-way bank conflicts, which the XOR
 * swizzle takes back to the 16-class floor (bench/10 quantifies the cost:
 * 2 classes is 0.10x, 8 classes is 0.73x).
 *
 * Twiddle index for a stage of half-size h is w[N/(2h) + (j >> (log2 h + 1))]
 * where j is the lower element of the pair -- the same rule the forward uses.
 *
 * Usage: 12_ntt_inv [batch_MiB]
 */
#include "ntt_kernels.h"

#define N       2048
#define THREADS 256
#define RPT     8

#define shoup_mul smul

__global__ __launch_bounds__(THREADS)
void k_inv_reg(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N];
    uint64_t *g = data + (size_t)blockIdx.x * N;
    uint64_t x[RPT], p2 = 2 * p;
    int tid = threadIdx.x, k, q, r, u, v;

#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = g[8 * tid + k];

    GINV3(1024 + 4 * tid, 512 + 2 * tid, 256 + tid);      /* h = 1, 2, 4    */

    EXW(8 * tid + k);
    q = tid >> 3; r = tid & 7;
    EXR(64 * q + r + 8 * k);
    GINV3(128 + 4 * q, 64 + 2 * q, 32 + q);               /* h = 8, 16, 32  */

    EXW(64 * q + r + 8 * k);
    u = tid >> 6; v = tid & 63;
    EXR(512 * u + v + 64 * k);
    GINV3(16 + 4 * u, 8 + 2 * u, 4 + u);                  /* h = 64,128,256 */

    EXW(512 * u + v + 64 * k);
    EXR(tid + 256 * k);
    GB(0, 2, 2); GB(1, 3, 2); GB(4, 6, 3); GB(5, 7, 3);   /* h = 512        */
    GB(0, 4, 1); GB(1, 5, 1); GB(2, 6, 1); GB(3, 7, 1);   /* h = 1024       */

#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t z = x[k];
        if (z >= p) z -= p;
        g[tid + 256 * k] = z;
    }
}

/* simple LDS reference, the kernel bench/08 verified against schoolbook */
__global__ __launch_bounds__(THREADS)
void k_inv_lds(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N];
    uint64_t *g = data + (size_t)blockIdx.x * N;
    uint64_t p2 = 2 * p;
    int tid = threadIdx.x, e, m, h, logh, b;

    for (e = tid; e < N; e += THREADS) s[e] = g[e];
    __syncthreads();
    logh = 0;
    for (m = N / 2, h = 1; m >= 1; m >>= 1, h <<= 1, logh++) {
        for (b = tid; b < N / 2; b += THREADS) {
            int i = b >> logh;
            int j = ((b >> logh) << (logh + 1)) | (b & (h - 1));
            uint64_t U = s[j], V = s[j + h], X;
            X = U + V;
            if (X >= p2) X -= p2;
            s[j] = X;
            s[j + h] = shoup_mul(w[m + i], wp[m + i], U - V + p2, p);
        }
        __syncthreads();
    }
    for (e = tid; e < N; e += THREADS) {
        uint64_t z = s[e];
        if (z >= p) z -= p;
        g[e] = z;
    }
}

__global__ __launch_bounds__(THREADS)
void k_fwd_reg(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N];
    uint64_t *g = data + (size_t)blockIdx.x * N;
    uint64_t x[RPT], p2 = 2 * p;
    int tid = threadIdx.x, k, span, base, grp, b2;

#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = g[tid + 256 * k];
    FWD3(1, 2, 4);
    EXW(tid + 256 * k);
    span = tid >> 5; base = tid & 31;
    EXR(256 * span + base + 32 * k);
    FWD3(8 + span, 16 + 2 * span, 32 + 4 * span);
    EXW(256 * span + base + 32 * k);
    grp = tid >> 2; b2 = tid & 3;
    EXR(32 * grp + b2 + 4 * k);
    FWD3(64 + grp, 128 + 2 * grp, 256 + 4 * grp);
    EXW(32 * grp + b2 + 4 * k);
    EXR(8 * tid + k);
    FB(0, 2, 512 + 2 * tid + 0); FB(1, 3, 512 + 2 * tid + 0);
    FB(4, 6, 512 + 2 * tid + 1); FB(5, 7, 512 + 2 * tid + 1);
    FB(0, 1, 1024 + 4 * tid + 0); FB(2, 3, 1024 + 4 * tid + 1);
    FB(4, 5, 1024 + 4 * tid + 2); FB(6, 7, 1024 + 4 * tid + 3);
#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t z = x[k];
        if (z >= p2) z -= p2;
        if (z >= p)  z -= p;
        g[8 * tid + k] = z;
    }
}

__global__ void k_scale(uint64_t *a, uint64_t f, uint64_t fp, uint64_t p, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t st = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += st) {
        uint64_t q = __umul64hi(fp, a[i]);
        uint64_t v = f * a[i] - q * p;
        a[i] = v >= p ? v - p : v;
    }
}

/* ------------------------------ host ------------------------------------ */

int main(int argc, char **argv)
{
    uint64_t p = PRIME, root, iroot, ninv;
    uint64_t *hw, *hwp, *hiw, *hiwp, *ha, *hb, *hc;
    uint64_t *dd, *dw, *dwp, *diw, *diwp;
    uint64_t *dbat[MAXD], *dwB[MAXD], *dwpB[MAXD];
    double rl[MAXD], rr[MAXD];
    hipDeviceProp_t pr;
    double mib = argc > 1 ? atof(argv[1]) : 512.0;
    size_t bytes, npt;
    int nd = device_count(), blocks, i, d, ok;

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    bytes = (size_t)(mib * 1048576.0);
    npt = bytes / sizeof(uint64_t);
    blocks = (int)(npt / N);
    root = powmod(PROOT, (p - 1) / (uint64_t)N, p);
    iroot = powmod(root, (uint64_t)N - 1, p);
    ninv = powmod((uint64_t)N, p - 2, p);

    printf("== 12_ntt_inv : register-blocked Gentleman-Sande inverse ==\n");
    meta("12_ntt_inv");
    printf("N=%d, %d threads, %d points/thread, %d APUs, %.0f MiB/APU\n",
           N, THREADS, RPT, nd, mib);

    hw = (uint64_t *)malloc(N * 8); hwp = (uint64_t *)malloc(N * 8);
    hiw = (uint64_t *)malloc(N * 8); hiwp = (uint64_t *)malloc(N * 8);
    ha = (uint64_t *)malloc(N * 8); hb = (uint64_t *)malloc(N * 8);
    hc = (uint64_t *)malloc(N * 8);
    build_table(hw, hwp, N, root, p);
    build_table(hiw, hiwp, N, iroot, p);
    for (i = 0; i < N; i++) ha[i] = (uint64_t)(i * 2654435761u + 999u) % p;

    HIP_CHECK(hipSetDevice(0));
    HIP_CHECK(hipMalloc(&dd, N * 8));
    HIP_CHECK(hipMalloc(&dw, N * 8)); HIP_CHECK(hipMalloc(&dwp, N * 8));
    HIP_CHECK(hipMalloc(&diw, N * 8)); HIP_CHECK(hipMalloc(&diwp, N * 8));
    HIP_CHECK(hipMemcpy(dw, hw, N * 8, hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(dwp, hwp, N * 8, hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(diw, hiw, N * 8, hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(diwp, hiwp, N * 8, hipMemcpyHostToDevice));

    /* 1. register inverse == simple LDS inverse */
    HIP_CHECK(hipMemcpy(dd, ha, N * 8, hipMemcpyHostToDevice));
    k_inv_lds<<<1, THREADS>>>(dd, diw, diwp, p);
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(hb, dd, N * 8, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(dd, ha, N * 8, hipMemcpyHostToDevice));
    k_inv_reg<<<1, THREADS>>>(dd, diw, diwp, p);
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(hc, dd, N * 8, hipMemcpyDeviceToHost));
    ok = 1;
    for (i = 0; i < N; i++) if (hb[i] != hc[i]) { ok = 0; break; }
    printf("\nVERIFY register inverse == simple LDS inverse : %s", ok ? "OK" : "FAILED");
    if (!ok) printf("  (index %d: reg %llu lds %llu)", i,
                    (unsigned long long)hc[i], (unsigned long long)hb[i]);
    printf("\n");

    /* 2. round trip: register forward then register inverse, scaled by 1/N */
    HIP_CHECK(hipMemcpy(dd, ha, N * 8, hipMemcpyHostToDevice));
    k_fwd_reg<<<1, THREADS>>>(dd, dw, dwp, p);
    k_inv_reg<<<1, THREADS>>>(dd, diw, diwp, p);
    k_scale<<<8, 256>>>(dd, ninv, shoup_pre(ninv, p), p, N);
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(hc, dd, N * 8, hipMemcpyDeviceToHost));
    ok = 1;
    for (i = 0; i < N; i++) if (hc[i] != ha[i]) { ok = 0; break; }
    printf("VERIFY round trip fwd_reg -> inv_reg -> /N == identity : %s", ok ? "OK" : "FAILED");
    if (!ok) printf("  (index %d: got %llu want %llu)", i,
                    (unsigned long long)hc[i], (unsigned long long)ha[i]);
    printf("\n");

    /* 3. throughput */
    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&dbat[d], bytes));
        HIP_CHECK(hipMalloc(&dwB[d], N * 8));
        HIP_CHECK(hipMalloc(&dwpB[d], N * 8));
        HIP_CHECK(hipMemcpy(dwB[d], hiw, N * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dwpB[d], hiwp, N * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipMemset(dbat[d], 1, bytes));
    }
    header("inverse kernel");
#pragma omp parallel num_threads(nd)
    {
        int dev = omp_get_thread_num(), rep;
        hipEvent_t t0, t1;
        double best;
        float ms;
        HIP_CHECK(hipSetDevice(dev));
        timer_events(&t0, &t1);
        best = 1e300;
        for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_inv_lds<<<blocks, THREADS>>>(dbat[dev], dwB[dev], dwpB[dev], p);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best = dmin(best, (double)ms);
        }
        rl[dev] = (double)blocks * (N / 2) * 11 / (best * 1e-3) / 1e9;
        best = 1e300;
        for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(t0, 0));
            k_inv_reg<<<blocks, THREADS>>>(dbat[dev], dwB[dev], dwpB[dev], p);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best = dmin(best, (double)ms);
        }
        rr[dev] = (double)blocks * (N / 2) * 11 / (best * 1e-3) / 1e9;
    }
    report_sum("simple LDS", "Gbfly/s", rl, nd);
    report_sum("register-blocked", "Gbfly/s", rr, nd);
    printf("\nspeedup %.2fx  (forward achieved 1.50x in bench/06)\n",
           (rr[0] + rr[1] + rr[2] + rr[3]) / (rl[0] + rl[1] + rl[2] + rl[3]));
    return 0;
}
