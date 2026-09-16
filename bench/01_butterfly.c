/* 01_butterfly - sustained modular NTT butterfly throughput on MI300A.
 *
 * The large-NTT design hinges on which arithmetic engine carries the butterfly,
 * so this measures issue throughput of the candidates with operands already in
 * registers and 8 independent chains (the node report's ILP finding says
 * dependent chains understate the VALU by about 5x).
 *
 * Engines
 *   shoup_u64   Harvey Alg.4 (decimation-in-time) with Shoup precomputed
 *               quotient, lazy [0,4p) representation, p < 2^62
 *   mont_u64    Harvey Alg.5 (Montgomery), p < 2^62
 *   gold_u64    p = 2^64-2^32+1 (Goldilocks), Solinas reduction, no
 *               precomputed quotient
 *   shoup_f64   the same butterfly in FP64 with fma/rint, p < 2^50
 *
 * Reported: Gbutterfly/s and Gbit-butterfly/s = Gbfly/s * log2(p).  The second
 * is the figure of merit, because a larger prime carries proportionally more
 * bits per point and so shortens the transform.
 */
#include "common_ntt.h"

#define CHAINS 8

/* ---------------- 62-bit Shoup (Harvey Alg. 4) -------------------------- */
__device__ static inline uint64_t shoup_mul(uint64_t w, uint64_t wp,
                                            uint64_t y, uint64_t p)
{
    uint64_t q = __umul64hi(wp, y);   /* floor(w*y/p) or one less */
    return w * y - q * p;             /* in [0,2p) */
}

__global__ __launch_bounds__(256)
void k_shoup_u64(uint64_t p, const uint64_t *win, uint64_t *sink, int iters)
{
    uint64_t X[CHAINS], Y[CHAINS], W[CHAINS], WP[CHAINS];
    uint64_t p2 = 2 * p, s = 0;
    int t = blockIdx.x * blockDim.x + threadIdx.x;
    int i, it;

#pragma unroll
    for (i = 0; i < CHAINS; i++) {
        W[i]  = win[(2 * i) & 63];
        WP[i] = win[(2 * i + 1) & 63];
        X[i]  = (uint64_t)(t + i) % p;
        Y[i]  = (uint64_t)(t + 3 * i + 1) % p;
    }
    for (it = 0; it < iters; it++) {
#pragma unroll
        for (i = 0; i < CHAINS; i++) {
            uint64_t x = X[i], T;
            if (x >= p2) x -= p2;                  /* [0,4p) -> [0,2p) */
            T = shoup_mul(W[i], WP[i], Y[i], p);   /* [0,2p) */
            X[i] = x + T;                          /* [0,4p) */
            Y[i] = x - T + p2;                     /* [0,4p) */
        }
    }
#pragma unroll
    for (i = 0; i < CHAINS; i++) s += X[i] ^ Y[i];
    if (s == 0xdeadbeefULL) sink[t] = s;
}

/* ---------------- 62-bit Montgomery (Harvey Alg. 5) --------------------- */
__global__ __launch_bounds__(256)
void k_mont_u64(uint64_t p, const uint64_t *win, uint64_t *sink, int iters)
{
    uint64_t X[CHAINS], Y[CHAINS], W[CHAINS];
    uint64_t p2 = 2 * p, J = win[63], s = 0;
    int t = blockIdx.x * blockDim.x + threadIdx.x;
    int i, it;

#pragma unroll
    for (i = 0; i < CHAINS; i++) {
        W[i] = win[(2 * i) & 63];
        X[i] = (uint64_t)(t + i) % p;
        Y[i] = (uint64_t)(t + 3 * i + 1) % p;
    }
    for (it = 0; it < iters; it++) {
#pragma unroll
        for (i = 0; i < CHAINS; i++) {
            uint64_t x = X[i], r0, r1, q, h, T;
            if (x >= p2) x -= p2;
            r0 = W[i] * Y[i];
            r1 = __umul64hi(W[i], Y[i]);
            q  = r0 * J;
            h  = __umul64hi(q, p);
            T  = r1 - h + p;                       /* [0,2p) */
            X[i] = x + T;
            Y[i] = x - T + p2;
        }
    }
#pragma unroll
    for (i = 0; i < CHAINS; i++) s += X[i] ^ Y[i];
    if (s == 0xdeadbeefULL) sink[t] = s;
}

/* ---------------- Goldilocks p = 2^64 - 2^32 + 1 ------------------------ */
#define GL_P   0xFFFFFFFF00000001ULL
#define GL_EPS 0xFFFFFFFFULL                       /* 2^32 - 1 */

__device__ static inline uint64_t gl_reduce(uint64_t lo, uint64_t hi)
{
    uint32_t n_hi = (uint32_t)(hi >> 32);
    uint32_t n_lo = (uint32_t)hi;
    uint64_t t0, t1, t2;

    t0 = lo - (uint64_t)n_hi;
    if (lo < (uint64_t)n_hi) t0 -= GL_EPS;         /* borrow */
    t1 = (uint64_t)n_lo * GL_EPS;
    t2 = t0 + t1;
    if (t2 < t0) t2 += GL_EPS;                     /* carry */
    return t2;
}

__global__ __launch_bounds__(256)
void k_gold_u64(uint64_t p_unused, const uint64_t *win, uint64_t *sink, int iters)
{
    uint64_t X[CHAINS], Y[CHAINS], W[CHAINS], s = 0;
    int t = blockIdx.x * blockDim.x + threadIdx.x;
    int i, it;
    (void)p_unused;

#pragma unroll
    for (i = 0; i < CHAINS; i++) {
        W[i] = win[(2 * i) & 63] % GL_P;
        X[i] = (uint64_t)(t + i) % GL_P;
        Y[i] = (uint64_t)(t + 3 * i + 1) % GL_P;
    }
    for (it = 0; it < iters; it++) {
#pragma unroll
        for (i = 0; i < CHAINS; i++) {
            uint64_t lo = W[i] * Y[i];
            uint64_t hi = __umul64hi(W[i], Y[i]);
            uint64_t T = gl_reduce(lo, hi);
            uint64_t x = X[i], a, b;
            a = x + T;  if (a < x || a >= GL_P) a -= GL_P;
            b = x - T;  if (x < T)              b += GL_P;
            X[i] = a; Y[i] = b;
        }
    }
#pragma unroll
    for (i = 0; i < CHAINS; i++) s += X[i] ^ Y[i];
    if (s == 0xdeadbeefULL) sink[t] = s;
}

/* ---------------- FP64 Shoup, p < 2^50 ---------------------------------- */
__device__ static inline double f64_mul(double w, double winv, double y,
                                        double p, double p2)
{
    double h = w * y;                 /* rounded product        */
    double l = fma(w, y, -h);         /* exact residual         */
    double q = rint(h * winv);        /* ~floor(w*y/p), off <=1 */
    double r = fma(-q, p, h) + l;     /* w*y - q*p, |r| < 2p    */
    r = r < 0.0  ? r + p2 : r;
    r = r >= p2  ? r - p2 : r;
    return r;                         /* [0,2p) */
}

__global__ __launch_bounds__(256)
void k_shoup_f64(uint64_t pi, const uint64_t *win, uint64_t *sink, int iters)
{
    double X[CHAINS], Y[CHAINS], W[CHAINS], WI[CHAINS];
    double p = (double)pi, p2 = 2.0 * p, p4 = 4.0 * p, s = 0.0;
    int t = blockIdx.x * blockDim.x + threadIdx.x;
    int i, it;

#pragma unroll
    for (i = 0; i < CHAINS; i++) {
        W[i]  = (double)(win[(2 * i) & 63] % pi);
        WI[i] = W[i] / p;
        X[i]  = (double)((uint64_t)(t + i) % pi);
        Y[i]  = (double)((uint64_t)(t + 3 * i + 1) % pi);
    }
    for (it = 0; it < iters; it++) {
#pragma unroll
        for (i = 0; i < CHAINS; i++) {
            double x = X[i], T, a, b;
            x = x >= p2 ? x - p2 : x;
            T = f64_mul(W[i], WI[i], Y[i], p, p2);
            a = x + T;
            b = x - T + p2;
            X[i] = a >= p4 ? a - p4 : a;
            Y[i] = b >= p4 ? b - p4 : b;
        }
    }
#pragma unroll
    for (i = 0; i < CHAINS; i++) s += X[i] + Y[i];
    if (s == 12345.0) sink[t] = (uint64_t)s;
}

/* ------------------------------------------------------------------------ */
#define NE 4

int main(int argc, char **argv)
{
    static const uint64_t P62 = 0x3FFF810000000001ULL;  /* 4194177*2^40+1 */
    static const uint64_t P50 = 0x0003CF0000000001ULL;  /*     975*2^40+1 */
    const char *names[NE] = { "shoup_u64  (p<2^62)", "mont_u64   (p<2^62)",
                              "gold_u64   (2^64-2^32+1)", "shoup_f64  (p<2^50)" };
    double bits[NE] = { 62.0, 62.0, 64.0, 50.0 };
    uint64_t primes[NE];
    uint64_t *win[MAXD], *sink[MAXD];
    double r[MAXD], rb[MAXD], hw[64];
    hipDeviceProp_t pr;
    int iters = argc > 1 ? atoi(argv[1]) : 2000;
    int nd, blocks, thr = 256, d, e, i;

    primes[0] = P62; primes[1] = P62; primes[2] = GL_P; primes[3] = P50;

    nd = device_count();
    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    blocks = pr.multiProcessorCount * 4;   /* 4 blocks/CU, per node report */

    printf("== 01_butterfly : modular NTT butterfly issue throughput ==\n");
    meta("01_butterfly");
    printf("chains/thread=%d  iters=%d  (operands register-resident)\n",
           CHAINS, iters);
    printf("device: %s  CUs=%d  grid=%d x %d  (%d APUs)\n",
           pr.gcnArchName, pr.multiProcessorCount, blocks, thr, nd);

    for (i = 0; i < 64; i++)
        hw[i] = 0x9E3779B97F4A7C15ULL * (uint64_t)(i + 1);
    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&win[d], 64 * sizeof(uint64_t)));
        HIP_CHECK(hipMalloc(&sink[d], (size_t)blocks * thr * sizeof(uint64_t)));
        HIP_CHECK(hipMemcpy(win[d], hw, 64 * sizeof(uint64_t),
                            hipMemcpyHostToDevice));
    }

    header("engine");
    for (e = 0; e < NE; e++) {
#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num();
            hipEvent_t t0, t1;
            double best = 1e300;
            int rep;
            HIP_CHECK(hipSetDevice(dev));
            timer_events(&t0, &t1);
            switch (e) {
            case 0: k_shoup_u64<<<blocks, thr>>>(primes[e], win[dev], sink[dev], iters / 10 + 1); break;
            case 1: k_mont_u64 <<<blocks, thr>>>(primes[e], win[dev], sink[dev], iters / 10 + 1); break;
            case 2: k_gold_u64 <<<blocks, thr>>>(primes[e], win[dev], sink[dev], iters / 10 + 1); break;
            default: k_shoup_f64<<<blocks, thr>>>(primes[e], win[dev], sink[dev], iters / 10 + 1); break;
            }
            HIP_CHECK(hipDeviceSynchronize());
            for (rep = 0; rep < 3; rep++) {
                float ms;
#pragma omp barrier
                HIP_CHECK(hipEventRecord(t0, 0));
                switch (e) {
                case 0: k_shoup_u64<<<blocks, thr>>>(primes[e], win[dev], sink[dev], iters); break;
                case 1: k_mont_u64 <<<blocks, thr>>>(primes[e], win[dev], sink[dev], iters); break;
                case 2: k_gold_u64 <<<blocks, thr>>>(primes[e], win[dev], sink[dev], iters); break;
                default: k_shoup_f64<<<blocks, thr>>>(primes[e], win[dev], sink[dev], iters); break;
                }
                HIP_CHECK(hipEventRecord(t1, 0));
                HIP_CHECK(hipEventSynchronize(t1));
                HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
                best = dmin(best, (double)ms);
            }
            r[dev] = (double)blocks * thr * CHAINS * (double)iters
                     / (best * 1e-3) / 1e9;
        }
        for (i = 0; i < nd; i++) rb[i] = r[i] * bits[e];
        report_sum(names[e], "Gbfly/s", r, nd);
        report_sum("   x log2(p)", "Gbit-bfly/s", rb, nd);
    }
    return 0;
}
