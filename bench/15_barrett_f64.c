/* 15_barrett_f64 - the paper's modular multiply, exactly as written.
 *
 * The e-paper (PLAN.md 4.1) uses an FP64-Barrett modmul with Dekker product
 * splitting on four primes p < 2^52, and claims ~775 Gmodmul/s per APU with
 * two conditional corrections in each direction ("single corrections fail
 * 0.57% of the time for P[1]").  This measures, per prime:
 *
 *   exact     10^9 random pairs plus every pair of edge values
 *             {0,1,2,p-2,p-1,2^k,p-2^k} against a 128-bit integer reference,
 *             for canonical inputs, for a lazy [0,2p) value times a canonical
 *             twiddle (what the DIF kernel actually does), and for both lazy
 *             (which two corrections cannot cover: hi*pinv reaches 2^54
 *             where the ulp is 4)
 *   fail1     how often ONE correction per direction would have been wrong
 *   rate      issue throughput, 8 independent chains (Gmodmul/s per APU)
 *             for the paper's modmul, for Shoup u64 on the same primes, and
 *             for the paper's DIF butterfly (u,v) -> (u+v, (u-v)*w)
 *
 * Usage: 15_barrett_f64 [random pairs, millions]
 */
#include "common_ntt.h"

#define NP 4
static const uint64_t PR[NP] = {3923057487904769ULL, 3641582511194113ULL,
                                2867526325239809ULL, 2586051348529153ULL};
#define CH 8
#define THREADS 256

/* the paper's eight lines; ncorr = corrections per direction */
__device__ static inline double mm_f64(double af, double bf, double p, double pinv, int ncorr)
{
    double hi = af * bf;                 /* rounded product */
    double lo = fma(af, bf, -hi);        /* Dekker exact low part */
    double q  = floor(hi * pinv);
    double r  = fma(-q, p, hi) + lo;
    r += (r < 0.0 ? p : 0.0);
    if (ncorr > 1) r += (r < 0.0 ? p : 0.0);
    r -= (r >= p ? p : 0.0);
    if (ncorr > 1) r -= (r >= p ? p : 0.0);
    return r;
}

/* D1 variants: quotient by rint (needs corrections both ways), and a third
 * correction pair -- to see what would bring the rate down to the paper's */
__device__ static inline double mm_f64_rint(double af, double bf, double p, double pinv)
{
    double hi = af * bf, lo = fma(af, bf, -hi), q = rint(hi * pinv);
    double r = fma(-q, p, hi) + lo;
    r += (r < 0.0 ? p : 0.0); r += (r < 0.0 ? p : 0.0);
    r -= (r >= p ? p : 0.0); r -= (r >= p ? p : 0.0);
    return r;
}
__device__ static inline double mm_f64_3c(double af, double bf, double p, double pinv)
{
    double hi = af * bf, lo = fma(af, bf, -hi), q = floor(hi * pinv);
    double r = fma(-q, p, hi) + lo;
    r += (r < 0.0 ? p : 0.0); r += (r < 0.0 ? p : 0.0); r += (r < 0.0 ? p : 0.0);
    r -= (r >= p ? p : 0.0); r -= (r >= p ? p : 0.0); r -= (r >= p ? p : 0.0);
    return r;
}

/* Shoup: wp = floor(w * 2^64 / p); w*y - floor(w*y/p)*p in [0,2p), one fix */
__device__ static inline uint64_t mm_shoup(uint64_t w, uint64_t wp, uint64_t y, uint64_t p)
{
    uint64_t q = __umul64hi(wp, y);
    uint64_t r = w * y - q * p;
    return r >= p ? r - p : r;
}

/* ---- exactness: each thread checks a strided range of pairs -------------- */
__global__ void k_check(uint64_t p, uint64_t seed, size_t n, int lazya, int lazyb,
                        unsigned long long *bad, unsigned long long *fail1)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    double pd = (double)p, pinv = 1.0 / pd;
    unsigned long long nb = 0, nf = 0;
    for (; i < n; i += stride) {
        uint64_t s = seed ^ (i * 0x9E3779B97F4A7C15ULL);
        s ^= s >> 31; s *= 0xBF58476D1CE4E5B9ULL; s ^= s >> 29;
        uint64_t a = s % (lazya * p);
        s *= 0x94D049BB133111EBULL; s ^= s >> 32;
        uint64_t b = s % (lazyb * p);
        uint64_t ref = (uint64_t)(((unsigned __int128)a * b) % p);
        uint64_t r2 = (uint64_t)mm_f64((double)a, (double)b, pd, pinv, 2);
        uint64_t r1 = (uint64_t)mm_f64((double)a, (double)b, pd, pinv, 1);
        nb += (r2 != ref);
        nf += (r1 != ref);
    }
    if (nb) atomicAdd(bad, nb);
    if (nf) atomicAdd(fail1, nf);
}

/* edge values: all pairs from a small list, one thread per pair */
__global__ void k_edges(uint64_t p, const uint64_t *vals, int nv,
                        unsigned long long *bad, unsigned long long *fail1)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= nv * nv) return;
    uint64_t a = vals[i / nv], b = vals[i % nv];
    double pd = (double)p, pinv = 1.0 / pd;
    uint64_t ref = (uint64_t)(((unsigned __int128)a * b) % p);
    if ((uint64_t)mm_f64((double)a, (double)b, pd, pinv, 2) != ref) atomicAdd(bad, 1ULL);
    if ((uint64_t)mm_f64((double)a, (double)b, pd, pinv, 1) != ref) atomicAdd(fail1, 1ULL);
}

/* ---- throughput: 8 independent chains ------------------------------------ */
__global__ __launch_bounds__(THREADS)
void k_rate_f64(double p, double pinv, double c, int iters, double *sink,
                unsigned long long *cyc, unsigned long long *wall)
{
    double x[CH];
    int i, j;
    long long c0 = clock64(), w0 = wall_clock64();
    for (j = 0; j < CH; j++) x[j] = (double)((threadIdx.x * 7 + j * 13 + 1) % 1000);
    for (i = 0; i < iters; i++) {
#pragma unroll
        for (j = 0; j < CH; j++) x[j] = mm_f64(x[j], c, p, pinv, 2);
    }
    double s = 0; for (j = 0; j < CH; j++) s += x[j];
    if (s == -1.0) *sink = s;
    if (threadIdx.x == 0) { atomicAdd(cyc, (unsigned long long)(clock64() - c0));
                            atomicAdd(wall, (unsigned long long)(wall_clock64() - w0)); }
}

#define RATE_KERNEL(NAME, FN)                                                 \
__global__ __launch_bounds__(THREADS)                                         \
void NAME(double p, double pinv, double c, int iters, double *sink)           \
{                                                                             \
    double x[CH]; int i, j;                                                   \
    for (j = 0; j < CH; j++) x[j] = (double)((threadIdx.x * 7 + j * 13 + 1) % 1000); \
    for (i = 0; i < iters; i++) {                                             \
        _Pragma("unroll") for (j = 0; j < CH; j++) x[j] = FN(x[j], c, p, pinv); \
    }                                                                         \
    double s = 0; for (j = 0; j < CH; j++) s += x[j];                         \
    if (s == -1.0) *sink = s;                                                 \
}
RATE_KERNEL(k_rate_f64_rint, mm_f64_rint)
RATE_KERNEL(k_rate_f64_3c, mm_f64_3c)

/* D1: fewer independent chains -> latency-bound rate */
#define RATE_KERNEL_NCH(NAME, NCH)                                            \
__global__ __launch_bounds__(THREADS)                                         \
void NAME(double p, double pinv, double c, int iters, double *sink)           \
{                                                                             \
    double x[NCH]; int i, j;                                                  \
    for (j = 0; j < NCH; j++) x[j] = (double)((threadIdx.x * 7 + j * 13 + 1) % 1000); \
    for (i = 0; i < iters; i++) {                                             \
        _Pragma("unroll") for (j = 0; j < NCH; j++) x[j] = mm_f64(x[j], c, p, pinv, 2); \
    }                                                                         \
    double s = 0; for (j = 0; j < NCH; j++) s += x[j];                        \
    if (s == -1.0) *sink = s;                                                 \
}
RATE_KERNEL_NCH(k_rate_f64_ch1, 1)
RATE_KERNEL_NCH(k_rate_f64_ch2, 2)
RATE_KERNEL_NCH(k_rate_f64_ch4, 4)

__global__ __launch_bounds__(THREADS)
void k_rate_shoup(uint64_t p, uint64_t c, uint64_t cp, int iters, uint64_t *sink,
                  unsigned long long *cyc, unsigned long long *wall)
{
    uint64_t x[CH];
    int i, j;
    long long c0 = clock64(), w0 = wall_clock64();
    for (j = 0; j < CH; j++) x[j] = (threadIdx.x * 7 + j * 13 + 1) % 1000;
    for (i = 0; i < iters; i++) {
#pragma unroll
        for (j = 0; j < CH; j++) x[j] = mm_shoup(c, cp, x[j], p);
    }
    uint64_t s = 0; for (j = 0; j < CH; j++) s += x[j];
    if (s == 1) *sink = s;
    if (threadIdx.x == 0) { atomicAdd(cyc, (unsigned long long)(clock64() - c0));
                            atomicAdd(wall, (unsigned long long)(wall_clock64() - w0)); }
}

/* DIF butterfly with lazy [0,2p): u' = u+v (fold), v' = (u-v+p)*w */
__global__ __launch_bounds__(THREADS)
void k_rate_bfly(double p, double pinv, double w, int iters, double *sink)
{
    double u[CH / 2], v[CH / 2], p2 = 2 * p;
    int i, j;
    for (j = 0; j < CH / 2; j++) { u[j] = (double)((threadIdx.x + j) % 1000); v[j] = u[j] + 1; }
    for (i = 0; i < iters; i++) {
#pragma unroll
        for (j = 0; j < CH / 2; j++) {
            double s = u[j] + v[j], d = u[j] - v[j] + p2;
            s -= (s >= p2 ? p2 : 0.0);
            u[j] = s;
            v[j] = mm_f64(d, w, p, pinv, 2);
        }
    }
    double s = 0; for (j = 0; j < CH / 2; j++) s += u[j] + v[j];
    if (s == -1.0) *sink = s;
}

static uint64_t shoup_pre52(uint64_t w, uint64_t p)
{ return (uint64_t)(((__uint128_t)w << 64) / p); }

int main(int argc, char **argv)
{
    double mpairs = argc > 1 ? atof(argv[1]) : 1000.0;
    size_t npairs = (size_t)(mpairs * 1e6);
    int nd = device_count(), d, pi, i, k, nv, lazy;
    hipDeviceProp_t pr;
    int blocks, iters = 4096;
    uint64_t vals[128];
    double rf[MAXD], rs[MAXD], rb[MAXD], rr[MAXD], r3[MAXD], mf[MAXD], msh[MAXD], c1[MAXD], c2[MAXD], c4[MAXD], o1[MAXD], o3[MAXD];
    int wallkhz = 0;
    unsigned long long hb[2];

    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    HIP_CHECK(hipDeviceGetAttribute(&wallkhz, hipDeviceAttributeWallClockRate, 0));
    blocks = pr.multiProcessorCount * 8;
    printf("== 15_barrett_f64 : the paper's FP64-Barrett modmul ==\n");
    meta("15_barrett_f64");
    printf("%zu random pairs per prime per APU, %d chains x %d iters for rates\n",
           npairs, CH, iters);

    /* ---- exactness, all four primes, all APUs in parallel (different seeds) */
    for (lazy = 0; lazy < 4; lazy++) {
    /* 0: canonical x canonical; 1: [0,2p) x canonical; 2: both [0,2p); 3: [0,4p) x canonical (unfolded u-v+2p) */
    int la = lazy == 0 ? 1 : lazy == 3 ? 4 : 2, lb = lazy == 2 ? 2 : 1, fatal = lazy < 2;
    printf("\ninputs a in [0,%dp), b in [0,%dp)%s:\n%-22s %14s %14s %12s\n", la, lb,
           lazy == 1 ? " (the kernel's case: lazy value x canonical twiddle)" : lazy == 3 ? " (unfolded u-v+2p x twiddle)" : "",
           "prime", "mismatch(2corr)", "wrong(1corr)", "fail1 %");
    for (pi = 0; pi < NP; pi++) {
        uint64_t p = PR[pi];
        unsigned long long tb = 0, tf = 0, eb = 0, ef = 0;
        nv = 0;
        vals[nv++] = 0; vals[nv++] = 1; vals[nv++] = 2; vals[nv++] = p - 2; vals[nv++] = p - 1;
        for (k = 1; k < 52; k++) { vals[nv++] = 1ULL << k; vals[nv++] = p - (1ULL << k); }
#pragma omp parallel num_threads(nd) reduction(+:tb,tf,eb,ef)
        {
            int dev = omp_get_thread_num();
            unsigned long long *db, h[4];
            uint64_t *dv;
            HIP_CHECK(hipSetDevice(dev));
            HIP_CHECK(hipMalloc(&db, 4 * sizeof *db));
            HIP_CHECK(hipMemset(db, 0, 4 * sizeof *db));
            HIP_CHECK(hipMalloc(&dv, nv * 8));
            HIP_CHECK(hipMemcpy(dv, vals, nv * 8, hipMemcpyHostToDevice));
            k_check<<<blocks, THREADS>>>(p, 0x1234567ULL * (dev + 1) + pi, npairs, la, lb, db, db + 1);
            k_edges<<<(nv * nv + 255) / 256, 256>>>(p, dv, nv, db + 2, db + 3);
            HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipMemcpy(h, db, 4 * sizeof *db, hipMemcpyDeviceToHost));
            tb += h[0]; tf += h[1]; eb += h[2]; ef += h[3];
            HIP_CHECK(hipFree(db)); HIP_CHECK(hipFree(dv));
        }
        printf("P[%d]=%-16llu %14llu %14llu %11.3f%%   edges: %llu bad, %llu one-corr\n",
               pi, (unsigned long long)p, tb, tf, 100.0 * tf / (double)(npairs * nd), eb, ef);
        {
            double v[1] = { 100.0 * tf / (double)(npairs * nd) };
            char nm[32]; snprintf(nm, sizeof nm, "fail1_P%d_lazy%d", pi, lazy);
            result(nm, "%", v[0], v, 1);
        }
        if ((tb || eb) && fatal) { printf("VERIFY FAILED: two-correction modmul mismatches reference\n"); return 1; }
        if (tb && !fatal) printf("  (expected: both inputs lazy exceeds what two corrections can fix)\n");
        if (tb && lazy == 3) printf("  D2: [0,4p) x canonical is NOT exact with two corrections\n");
    }
    }
    printf("VERIFY two-correction FP64 Barrett == 128-bit reference, all primes, lazy x canonical: OK\n");

    /* ---- rates, prime P[1] (the paper's worst case), all APUs concurrently */
    header("rate");
    for (pi = 0; pi < NP; pi++) {
        uint64_t p = PR[pi], c = p / 3 + 12345, cp = shoup_pre52(c, p);
        double pd = (double)p, pinv = 1.0 / pd;
#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num(), rep;
            hipEvent_t t0, t1;
            float ms; double best;
            double *ds; uint64_t *dsu; unsigned long long *dc, hc[2];
            HIP_CHECK(hipSetDevice(dev));
            HIP_CHECK(hipMalloc(&ds, 8)); HIP_CHECK(hipMalloc(&dsu, 8)); HIP_CHECK(hipMalloc(&dc, 16));
            timer_events(&t0, &t1);
            k_rate_f64<<<blocks, THREADS>>>(pd, pinv, (double)c, 16, ds, dc, dc + 1);
            HIP_CHECK(hipDeviceSynchronize());
#define RATE(launch, ops, out) do { best = 1e300;                              \
            for (rep = 0; rep < 3; rep++) {                                   \
                _Pragma("omp barrier")                                        \
                HIP_CHECK(hipEventRecord(t0, 0)); launch;                     \
                HIP_CHECK(hipEventRecord(t1, 0)); HIP_CHECK(hipEventSynchronize(t1)); \
                HIP_CHECK(hipEventElapsedTime(&ms, t0, t1)); best = dmin(best, ms); } \
            out[dev] = (double)blocks * THREADS * (ops) / (best * 1e-3) / 1e9; } while (0)
            HIP_CHECK(hipMemset(dc, 0, 16));
            RATE((k_rate_f64<<<blocks, THREADS>>>(pd, pinv, (double)c, iters, ds, dc, dc + 1)), (double)CH * iters, rf);
            HIP_CHECK(hipMemcpy(hc, dc, 16, hipMemcpyDeviceToHost)); mf[dev] = (double)hc[0] / hc[1] * wallkhz / 1000.0;
            HIP_CHECK(hipMemset(dc, 0, 16));
            RATE((k_rate_shoup<<<blocks, THREADS>>>(p, c, cp, iters, dsu, dc, dc + 1)), (double)CH * iters, rs);
            HIP_CHECK(hipMemcpy(hc, dc, 16, hipMemcpyDeviceToHost)); msh[dev] = (double)hc[0] / hc[1] * wallkhz / 1000.0;
            RATE((k_rate_bfly<<<blocks, THREADS>>>(pd, pinv, (double)c, iters, ds)), (double)(CH / 2) * iters, rb);
            if (pi == 1) {
            RATE((k_rate_f64_rint<<<blocks, THREADS>>>(pd, pinv, (double)c, iters, ds)), (double)CH * iters, rr);
            RATE((k_rate_f64_3c<<<blocks, THREADS>>>(pd, pinv, (double)c, iters, ds)), (double)CH * iters, r3);
            RATE((k_rate_f64_ch1<<<blocks, THREADS>>>(pd, pinv, (double)c, iters * 8, ds)), 1.0 * iters * 8, c1);
            RATE((k_rate_f64_ch2<<<blocks, THREADS>>>(pd, pinv, (double)c, iters * 4, ds)), 2.0 * iters * 4, c2);
            RATE((k_rate_f64_ch4<<<blocks, THREADS>>>(pd, pinv, (double)c, iters * 2, ds)), 4.0 * iters * 2, c4);
            /* occupancy: 1 and 3 blocks per CU instead of 8 */
            { int b1 = pr.multiProcessorCount, b3 = pr.multiProcessorCount * 3;
              RATE((k_rate_f64<<<b1, THREADS>>>(pd, pinv, (double)c, iters * 8, ds, dc, dc + 1)), (double)CH * iters * 8 * b1 / blocks, o1);
              RATE((k_rate_f64<<<b3, THREADS>>>(pd, pinv, (double)c, iters * 8 / 3, ds, dc, dc + 1)), (double)CH * (iters * 8 / 3) * b3 / blocks, o3); }
            }
            HIP_CHECK(hipFree(ds)); HIP_CHECK(hipFree(dsu)); HIP_CHECK(hipFree(dc));
        }
        {
            char nm[48];
            snprintf(nm, sizeof nm, "f64 barrett modmul P%d", pi); report_sum(nm, "Gmodmul/s", rf, nd);
            snprintf(nm, sizeof nm, "  sclk during f64 P%d", pi);  report_max(nm, "MHz", mf, nd);
            snprintf(nm, sizeof nm, "shoup u64 modmul P%d", pi);   report_sum(nm, "Gmodmul/s", rs, nd);
            snprintf(nm, sizeof nm, "  sclk during shoup P%d", pi); report_max(nm, "MHz", msh, nd);
            snprintf(nm, sizeof nm, "f64 DIF butterfly P%d", pi);  report_sum(nm, "Gbfly/s", rb, nd);
            if (pi == 1) { report_sum("D1: rint quotient P1", "Gmodmul/s", rr, nd);
                           report_sum("D1: three corrections P1", "Gmodmul/s", r3, nd);
                           report_sum("D1: 1 chain P1", "Gmodmul/s", c1, nd);
                           report_sum("D1: 2 chains P1", "Gmodmul/s", c2, nd);
                           report_sum("D1: 4 chains P1", "Gmodmul/s", c4, nd);
                           report_sum("D1: 1 block/CU P1", "Gmodmul/s", o1, nd);
                           report_sum("D1: 3 blocks/CU P1", "Gmodmul/s", o3, nd); }
        }
    }
    printf("\npaper: ~775 Gmodmul/s per APU (12.4 TF64 / 16 ops).  Per-APU here: see columns.\n");
    (void)i; (void)d; (void)hb;
    return 0;
}
