/* 04_ntt_lds - what does a real LDS-resident NTT sustain?
 *
 * bench/01 measured 3886 Gbfly/s with operands already in registers.  That is
 * an upper bound: a real transform also pays LDS traffic, barrier
 * synchronisation, twiddle loads and index arithmetic.  The whole time model
 * in DESIGN.md rests on the ratio between the two, so this measures it.
 *
 * Structure is the inner sub-transform of a four-step NTT: each workgroup
 * loads N points into LDS, runs log2(N) radix-2 Cooley-Tukey stages with
 * Harvey Alg.4 lazy Shoup butterflies, and writes back.  A grid of such
 * workgroups is exactly the batched sub-transform a four-step pass performs.
 *
 * Correctness is checked two ways on the host: a cyclic convolution against
 * schoolbook, and a forward/inverse round trip.
 *
 * Usage: 04_ntt_lds [batch_MiB]
 */
#include "common_ntt.h"

/* 62-bit prime, 2^40 * 3 * 5 * 7 | p-1, primitive root 11 */
#define PRIME 0x3FFEDF0000000001ULL
#define PROOT 11ULL

#define THREADS 256

/* ------------------------------ device ---------------------------------- */

__device__ static inline uint64_t shoup_mul(uint64_t w, uint64_t wp,
                                            uint64_t y, uint64_t p)
{
    uint64_t q = __umul64hi(wp, y);
    return w * y - q * p;              /* [0,2p) for y < 4p, p < 2^62 */
}

/* XOR swizzle for 64-bit LDS elements: 32 banks x 4 B means element e sits in
 * bank pair (2e mod 32), so 16 distinct pairs.  Permuting the low 4 bits by
 * bits 4..7 is a bijection and breaks the power-of-two stride conflicts that
 * the late stages of an in-LDS transform generate. */
#define SWZ(e)  ((e) ^ (((e) >> 4) & 15))

/* Forward: decimation in time, natural order in, bit-reversed out.
 * SW selects the swizzle so its cost can be measured. */
#define DEFINE_NTT(NAME, N, LOGN, SW)                                         \
__global__ __launch_bounds__(THREADS)                                         \
void NAME(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)  \
{                                                                             \
    __shared__ uint64_t s[N];                                                 \
    uint64_t *g = data + (size_t)blockIdx.x * N;                              \
    uint64_t p2 = 2 * p, p4 = 4 * p;                                          \
    int tid = threadIdx.x, e, m, t, logt, b;                                  \
                                                                              \
    for (e = tid; e < N; e += THREADS) s[SW ? SWZ(e) : e] = g[e];             \
    __syncthreads();                                                          \
                                                                              \
    logt = LOGN - 1;                                                          \
    for (m = 1, t = N / 2; m < N; m <<= 1, t >>= 1, logt--) {                 \
        for (b = tid; b < N / 2; b += THREADS) {                              \
            int i = b >> logt;                                                \
            int j = ((b >> logt) << (logt + 1)) | (b & (t - 1));              \
            int a0 = SW ? SWZ(j) : j;                                         \
            int a1 = SW ? SWZ(j + t) : (j + t);                               \
            uint64_t U = s[a0], V, W = w[m + i], WP = wp[m + i];              \
            if (U >= p2) U -= p2;                                             \
            V = shoup_mul(W, WP, s[a1], p);                                   \
            s[a0] = U + V;                                                    \
            s[a1] = U - V + p2;                                               \
        }                                                                     \
        __syncthreads();                                                      \
    }                                                                         \
    for (e = tid; e < N; e += THREADS) {                                      \
        uint64_t v = s[SW ? SWZ(e) : e];                                      \
        if (v >= p2) v -= p2;                                                 \
        if (v >= p)  v -= p;                                                  \
        g[e] = v;                                                             \
    }                                                                         \
    (void)p4;                                                                 \
}

DEFINE_NTT(k_ntt_512_sw,   512,  9, 1)
DEFINE_NTT(k_ntt_512_ns,   512,  9, 0)
DEFINE_NTT(k_ntt_1024_sw, 1024, 10, 1)
DEFINE_NTT(k_ntt_2048_sw, 2048, 11, 1)
DEFINE_NTT(k_ntt_2048_ns, 2048, 11, 0)
DEFINE_NTT(k_ntt_4096_sw, 4096, 12, 1)

/* Inverse: decimation in frequency (Gentleman-Sande), bit-reversed in,
 * natural out.  Used only for the correctness check. */
__global__ __launch_bounds__(THREADS)
void k_intt_512(uint64_t *data, const uint64_t *w, const uint64_t *wp,
                uint64_t p, uint64_t ninv, uint64_t ninvp)
{
    __shared__ uint64_t s[512];
    uint64_t *g = data + (size_t)blockIdx.x * 512;
    uint64_t p2 = 2 * p;
    int tid = threadIdx.x, e, m, t, logt, b;

    for (e = tid; e < 512; e += THREADS) s[e] = g[e];
    __syncthreads();

    logt = 0;
    for (m = 512 / 2, t = 1; m >= 1; m >>= 1, t <<= 1, logt++) {
        for (b = tid; b < 512 / 2; b += THREADS) {
            int i = b >> logt;
            int j = ((b >> logt) << (logt + 1)) | (b & (t - 1));
            uint64_t U = s[j], V = s[j + t];
            uint64_t W = w[m + i], WP = wp[m + i], X, Y;
            X = U + V;
            if (X >= p2) X -= p2;
            Y = shoup_mul(W, WP, U - V + p2, p);
            s[j] = X;
            s[j + t] = Y;
        }
        __syncthreads();
    }
    for (e = tid; e < 512; e += THREADS) {
        uint64_t v = shoup_mul(ninv, ninvp, s[e], p);
        if (v >= p) v -= p;
        g[e] = v;
    }
}

/* ------------------------------ host ------------------------------------ */

static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t p)
{
    return (uint64_t)((__uint128_t)a * b % p);
}

static uint64_t powmod(uint64_t a, uint64_t e, uint64_t p)
{
    uint64_t r = 1;
    a %= p;
    while (e) {
        if (e & 1) r = mulmod(r, a, p);
        a = mulmod(a, a, p);
        e >>= 1;
    }
    return r;
}

static uint64_t shoup_pre(uint64_t w, uint64_t p)
{
    return (uint64_t)(((__uint128_t)w << 64) / p);
}

static int brv(int i, int bits)
{
    int r = 0, k;
    for (k = 0; k < bits; k++) if (i & (1 << k)) r |= 1 << (bits - 1 - k);
    return r;
}

/* table[m+i] = root^((N/(2m)) * bitrev(i, log2 m)) */
static void build_table(uint64_t *w, uint64_t *wp, int N, uint64_t root,
                        uint64_t p)
{
    int m, i, lg = 0;
    while ((1 << lg) < N) lg++;
    for (m = 1; m < N; m <<= 1) {
        int lgm = 0;
        while ((1 << lgm) < m) lgm++;
        for (i = 0; i < m; i++) {
            uint64_t e = (uint64_t)(N / (2 * m)) * (uint64_t)brv(i, lgm);
            w[m + i] = powmod(root, e, p);
            wp[m + i] = shoup_pre(w[m + i], p);
        }
    }
    w[0] = 1; wp[0] = shoup_pre(1, p);
}

struct variant {
    const char *name;
    void (*k)(uint64_t *, const uint64_t *, const uint64_t *, uint64_t);
    int n;
    int logn;
};

int main(int argc, char **argv)
{
    struct variant V[6];
    uint64_t p = PRIME, *hw, *hwp, *ha, *hb, *href;
    uint64_t *dd[MAXD], *dw[MAXD], *dwp[MAXD];
    double rate[MAXD], bw[MAXD];
    hipDeviceProp_t pr;
    double batch_mib = argc > 1 ? atof(argv[1]) : 512.0;
    size_t bytes, npt;
    int nd = device_count(), nv = 0, v, i, ok, N, blocks;

    V[nv].name = "N=512   swizzle";  V[nv].k = k_ntt_512_sw;  V[nv].n = 512;  V[nv].logn = 9;  nv++;
    V[nv].name = "N=512   plain";    V[nv].k = k_ntt_512_ns;  V[nv].n = 512;  V[nv].logn = 9;  nv++;
    V[nv].name = "N=1024  swizzle";  V[nv].k = k_ntt_1024_sw; V[nv].n = 1024; V[nv].logn = 10; nv++;
    V[nv].name = "N=2048  swizzle";  V[nv].k = k_ntt_2048_sw; V[nv].n = 2048; V[nv].logn = 11; nv++;
    V[nv].name = "N=2048  plain";    V[nv].k = k_ntt_2048_ns; V[nv].n = 2048; V[nv].logn = 11; nv++;
    V[nv].name = "N=4096  swizzle";  V[nv].k = k_ntt_4096_sw; V[nv].n = 4096; V[nv].logn = 12; nv++;

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    bytes = (size_t)(batch_mib * 1048576.0);
    npt = bytes / sizeof(uint64_t);

    printf("== 04_ntt_lds : batched LDS-resident NTT, p = 0x%016llX ==\n",
           (unsigned long long)p);
    meta("04_ntt_lds");
    printf("%d APUs, %.0f MiB of points per APU (%zu points), %d threads/block\n",
           nd, batch_mib, npt, THREADS);
    printf("reference: register-resident butterfly rate was 3886 Gbfly/s (bench 01)\n");

    /* ---- correctness: N=512 cyclic convolution vs schoolbook ---- */
    N = 512;
    hw = (uint64_t *)malloc(N * sizeof(uint64_t));
    hwp = (uint64_t *)malloc(N * sizeof(uint64_t));
    ha = (uint64_t *)malloc(N * sizeof(uint64_t));
    hb = (uint64_t *)malloc(N * sizeof(uint64_t));
    href = (uint64_t *)malloc(N * sizeof(uint64_t));
    {
        uint64_t root = powmod(PROOT, (p - 1) / (uint64_t)N, p);
        uint64_t iroot = powmod(root, (uint64_t)N - 1, p);
        uint64_t ninv = powmod((uint64_t)N, p - 2, p);
        uint64_t *iw = (uint64_t *)malloc(N * sizeof(uint64_t));
        uint64_t *iwp = (uint64_t *)malloc(N * sizeof(uint64_t));
        uint64_t *da, *dwf, *dwfp, *diw, *diwp;
        int j;

        build_table(hw, hwp, N, root, p);
        build_table(iw, iwp, N, iroot, p);
        for (i = 0; i < N; i++) {
            ha[i] = (uint64_t)(i * 2654435761u) % p;
            hb[i] = (uint64_t)(i * 40503u + 7) % p;
        }
        for (i = 0; i < N; i++) {
            uint64_t acc = 0;
            for (j = 0; j < N; j++)
                acc = (acc + mulmod(ha[j], hb[(i - j + N) % N], p)) % p;
            href[i] = acc;
        }
        HIP_CHECK(hipSetDevice(0));
        HIP_CHECK(hipMalloc(&da, 2 * N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&dwf, N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&dwfp, N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&diw, N * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&diwp, N * sizeof(uint64_t)));
        HIP_CHECK(hipMemcpy(da, ha, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(da + N, hb, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dwf, hw, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dwfp, hwp, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(diw, iw, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(diwp, iwp, N * sizeof(uint64_t), hipMemcpyHostToDevice));

        k_ntt_512_ns<<<2, THREADS>>>(da, dwf, dwfp, p);
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(ha, da, N * sizeof(uint64_t), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(hb, da + N, N * sizeof(uint64_t), hipMemcpyDeviceToHost));
        for (i = 0; i < N; i++) ha[i] = mulmod(ha[i], hb[i], p);
        HIP_CHECK(hipMemcpy(da, ha, N * sizeof(uint64_t), hipMemcpyHostToDevice));
        k_intt_512<<<1, THREADS>>>(da, diw, diwp, p, ninv, shoup_pre(ninv, p));
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(ha, da, N * sizeof(uint64_t), hipMemcpyDeviceToHost));

        ok = 1;
        for (i = 0; i < N; i++) if (ha[i] != href[i]) { ok = 0; break; }
        printf("\nVERIFY cyclic convolution N=512 vs schoolbook: %s",
               ok ? "OK" : "FAILED");
        if (!ok) printf(" (first mismatch at %d: got %llu want %llu)",
                        i, (unsigned long long)ha[i], (unsigned long long)href[i]);
        printf("\n");
        free(iw); free(iwp);
        HIP_CHECK(hipFree(da)); HIP_CHECK(hipFree(dwf)); HIP_CHECK(hipFree(dwfp));
        HIP_CHECK(hipFree(diw)); HIP_CHECK(hipFree(diwp));
    }
    free(hw); free(hwp); free(ha); free(hb); free(href);

    /* ---- throughput ---- */
    header("variant");
    for (v = 0; v < nv; v++) {
        N = V[v].n;
        hw = (uint64_t *)malloc(N * sizeof(uint64_t));
        hwp = (uint64_t *)malloc(N * sizeof(uint64_t));
        build_table(hw, hwp, N, powmod(PROOT, (p - 1) / (uint64_t)N, p), p);
        blocks = (int)(npt / (size_t)N);

        for (i = 0; i < nd; i++) {
            HIP_CHECK(hipSetDevice(i));
            if (v == 0) HIP_CHECK(hipMalloc(&dd[i], bytes));
            HIP_CHECK(hipMalloc(&dw[i], N * sizeof(uint64_t)));
            HIP_CHECK(hipMalloc(&dwp[i], N * sizeof(uint64_t)));
            HIP_CHECK(hipMemcpy(dw[i], hw, N * sizeof(uint64_t), hipMemcpyHostToDevice));
            HIP_CHECK(hipMemcpy(dwp[i], hwp, N * sizeof(uint64_t), hipMemcpyHostToDevice));
            if (v == 0) HIP_CHECK(hipMemset(dd[i], 1, bytes));
        }

#pragma omp parallel num_threads(nd)
        {
            int d = omp_get_thread_num(), rep;
            hipEvent_t t0, t1;
            double best = 1e300;
            float ms;
            HIP_CHECK(hipSetDevice(d));
            timer_events(&t0, &t1);
            V[v].k<<<blocks, THREADS>>>(dd[d], dw[d], dwp[d], p);
            HIP_CHECK(hipDeviceSynchronize());
            for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
                HIP_CHECK(hipEventRecord(t0, 0));
                V[v].k<<<blocks, THREADS>>>(dd[d], dw[d], dwp[d], p);
                HIP_CHECK(hipEventRecord(t1, 0));
                HIP_CHECK(hipEventSynchronize(t1));
                HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
                best = dmin(best, (double)ms);
            }
            rate[d] = (double)blocks * (V[v].n / 2) * V[v].logn
                      / (best * 1e-3) / 1e9;
            bw[d] = 2.0 * bytes / (best * 1e-3) / 1e9;
        }
        report_sum(V[v].name, "Gbfly/s", rate, nd);
        report_sum("   HBM traffic", "GB/s", bw, nd);

        for (i = 0; i < nd; i++) {
            HIP_CHECK(hipSetDevice(i));
            HIP_CHECK(hipFree(dw[i])); HIP_CHECK(hipFree(dwp[i]));
        }
        free(hw); free(hwp);
    }
    return 0;
}
