/* 09_logic - per-operation throughput and latency of the logic units, for
 * exactly the operation mix an optimised NTT uses.
 *
 * bench/01 measured whole butterflies; this measures the primitives they are
 * built from, in both regimes:
 *
 *   throughput  8 independent chains  -> issue rate, what a register-blocked
 *                                        kernel with enough ILP can reach
 *   latency     1 dependent chain     -> result-to-operand delay
 *
 * The ratio of the two is the number of independent operations a kernel must
 * keep in flight to saturate the unit; it is what decides how many points per
 * thread the NTT kernel should hold.  Reported as lane-cycles per operation so
 * the numbers can be compared with the ISA directly.
 *
 * Usage: 09_logic [iters]
 */
#include "common_ntt.h"

#define CH 8                       /* independent chains for the throughput run */
/* optimisation barrier: keeps x live and unknown so dependent add chains
 * cannot be strength-reduced (RESULTS.md 9 caveat, PLAN.md 7.2) */
#define OPAQUE(x) asm volatile("" : "+v"(x))

/* ---- the operation set, as single-expression steps ---------------------- */
/* 64-bit integer, the Shoup/Montgomery substrate */
#define S_mul_lo64(x, c)   ((x) * (c))
#define S_mul_hi64(x, c)   (__umul64hi((x), (c)) + 1)
#define S_mad_u64_u32(x,c) ((uint64_t)(uint32_t)(x) * (uint32_t)(c) + (x))
#define S_add64(x, c)      ((x) + (c))
#define S_sub64(x, c)      ((x) - (c))
#define S_sel64(x, c)      ((x) >= (c) ? (x) - (c) : (x))     /* lazy reduction */
#define S_shift64(x, c)    (((x) << 3) ^ ((x) >> 5) ^ (c))
#define S_bfe64(x, c)      ((((x) >> 11) & 2047) + (c))
/* composites */
#define S_shoup(x, c)      shoup_step((x), (c))
#define S_mont(x, c)       mont_step((x), (c))
#define S_bfly(x, c)       bfly_step((x), (c))

/* 32-bit integer, the alternative small-prime substrate and all index math */
#define S32_mul_lo32(x, c) ((x) * (c))
#define S32_mul_hi32(x, c) (__umulhi((x), (c)) + 1u)
#define S32_add32(x, c)    ((x) + (c))
#define S32_xor32(x, c)    ((x) ^ (c))
#define S32_brev32(x, c)   (__brev((x)) + (c))
#define S32_sel32(x, c)    ((x) >= (c) ? (x) - (c) : (x))

/* FP64, the alternative butterfly substrate */
#define SD_fma64(x, c)     (fma((x), 1.0000001, (c)))
#define SD_mul64(x, c)     ((x) * 1.0000001 + (c))
#define SD_rint64(x, c)    (rint((x) * 0.5) + (c))

__device__ static const uint64_t PP = 0x3FFEDF0000000001ULL;

__device__ static inline uint64_t shoup_step(uint64_t x, uint64_t c)
{
    uint64_t q = __umul64hi(c, x);
    return x * c - q * PP;                       /* [0,2p) */
}
__device__ static inline uint64_t mont_step(uint64_t x, uint64_t c)
{
    uint64_t lo = x * c, hi = __umul64hi(x, c);
    uint64_t m = lo * 0x3FFEDF0000000001ULL;     /* stand-in for p^-1 */
    uint64_t u = hi - __umul64hi(m, PP) + PP;
    return u >= PP ? u - PP : u;
}
__device__ static inline uint64_t bfly_step(uint64_t x, uint64_t c)
{
    uint64_t p2 = 2 * PP, u = x, v;
    if (u >= p2) u -= p2;
    v = shoup_step(c, x);
    return (u + v) ^ (u - v + p2);               /* both halves, folded */
}

/* ---- kernel generators -------------------------------------------------- */
#define GEN(TY, NAME, STEP)                                                   \
__global__ __launch_bounds__(256)                                             \
void k_tp_##NAME(TY *sink, TY seed, int iters)                                \
{                                                                             \
    TY x[CH], c[CH];                                                          \
    int t = blockIdx.x * blockDim.x + threadIdx.x, i, it;                     \
    _Pragma("unroll")                                                         \
    for (i = 0; i < CH; i++) { x[i] = seed + (TY)(t + i); c[i] = seed + (TY)(3 * i + 1); } \
    for (it = 0; it < iters; it++) {                                          \
        _Pragma("unroll")                                                     \
        for (i = 0; i < CH; i++) { x[i] = STEP(x[i], c[i]); OPAQUE(x[i]); }   \
    }                                                                         \
    { TY s = 0; _Pragma("unroll") for (i = 0; i < CH; i++) s += x[i];         \
      if (s == (TY)0xdeadbeef) *sink = s; }                                   \
}                                                                             \
__global__ __launch_bounds__(256)                                             \
void k_lat_##NAME(TY *sink, TY seed, int iters)                               \
{                                                                             \
    TY x, c;                                                                  \
    int t = blockIdx.x * blockDim.x + threadIdx.x, it;                        \
    x = seed + (TY)t; c = seed + (TY)7;                                       \
    for (it = 0; it < iters; it++) { x = STEP(x, c); OPAQUE(x); }             \
    if (x == (TY)0xdeadbeef) *sink = x;                                       \
}

GEN(uint64_t, mul_lo64,    S_mul_lo64)
GEN(uint64_t, mul_hi64,    S_mul_hi64)
GEN(uint64_t, mad_u64_u32, S_mad_u64_u32)
GEN(uint64_t, add64,       S_add64)
GEN(uint64_t, sub64,       S_sub64)
GEN(uint64_t, sel64,       S_sel64)
GEN(uint64_t, shift64,     S_shift64)
GEN(uint64_t, bfe64,       S_bfe64)
GEN(uint64_t, shoup,       S_shoup)
GEN(uint64_t, mont,        S_mont)
GEN(uint64_t, bfly,        S_bfly)
GEN(uint32_t, mul_lo32,    S32_mul_lo32)
GEN(uint32_t, mul_hi32,    S32_mul_hi32)
GEN(uint32_t, add32,       S32_add32)
GEN(uint32_t, xor32,       S32_xor32)
GEN(uint32_t, brev32,      S32_brev32)
GEN(uint32_t, sel32,       S32_sel32)
GEN(double,   fma64,       SD_fma64)
GEN(double,   dmul64,      SD_mul64)
GEN(double,   rint64,      SD_rint64)

/* effective shader clock during a VALU-bound burst (as bench/22): the
 * bfly chain with in-kernel clock64/wall_clock64 */
__global__ __launch_bounds__(256)
void k_clock(uint64_t *sink, int iters, unsigned long long *cyc, unsigned long long *wall)
{
    uint64_t x[CH], c = 0x123456789ULL;
    int i, it;
    long long c0 = clock64(), w0 = wall_clock64();
    for (i = 0; i < CH; i++) x[i] = threadIdx.x + i;
    for (it = 0; it < iters; it++) {
        _Pragma("unroll") for (i = 0; i < CH; i++) { x[i] = bfly_step(x[i], c); OPAQUE(x[i]); }
    }
    { uint64_t s2 = 0; for (i = 0; i < CH; i++) s2 += x[i]; if (s2 == 1) *sink = s2; }
    if (threadIdx.x == 0) { atomicAdd(cyc, (unsigned long long)(clock64() - c0));
                            atomicAdd(wall, (unsigned long long)(wall_clock64() - w0)); }
}

/* ---- driver ------------------------------------------------------------- */
typedef void (*k64)(uint64_t *, uint64_t, int);
typedef void (*k32)(uint32_t *, uint32_t, int);
typedef void (*kfd)(double *, double, int);

struct entry { const char *name; int type; const void *tp; const void *lat; };

#define E(TY, NAME) { #NAME, TY, (const void *)k_tp_##NAME, (const void *)k_lat_##NAME }
#define NE 20

int main(int argc, char **argv)
{
    struct entry E_[NE] = {
        E(0, mul_lo64), E(0, mul_hi64), E(0, mad_u64_u32), E(0, add64),
        E(0, sub64), E(0, sel64), E(0, shift64), E(0, bfe64),
        E(0, shoup), E(0, mont), E(0, bfly),
        E(1, mul_lo32), E(1, mul_hi32), E(1, add32), E(1, xor32),
        E(1, brev32), E(1, sel32),
        E(2, fma64), E(2, dmul64), E(2, rint64)
    };
    hipDeviceProp_t pr;
    void *sink;
    double ghz, lanes;
    int iters = argc > 1 ? atoi(argv[1]) : 3000;
    int blocks, thr = 256, e;

    unsigned long long *dc, hc[2]; int wallkhz = 0; double mhz;
    HIP_CHECK(hipSetDevice(0));
    HIP_CHECK(hipGetDeviceProperties(&pr, 0));
    HIP_CHECK(hipDeviceGetAttribute(&wallkhz, hipDeviceAttributeWallClockRate, 0));
    blocks = pr.multiProcessorCount * 4;
    HIP_CHECK(hipMalloc(&sink, 64));
    HIP_CHECK(hipMalloc(&dc, 16)); HIP_CHECK(hipMemset(dc, 0, 16));

    printf("== 09_logic : per-operation throughput and latency ==\n");
    meta("09_logic");
    /* measured clock, not nominal (RESULTS.md 24) */
    k_clock<<<blocks, thr>>>((uint64_t *)sink, 20000, dc, dc + 1);
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(hc, dc, 16, hipMemcpyDeviceToHost));
    mhz = (double)hc[0] / hc[1] * wallkhz / 1000.0;
    ghz = mhz / 1000.0;
    lanes = (double)pr.multiProcessorCount * 64.0 * ghz * 1e9;
    printf("%s  %d CUs @ %.0f MHz measured in-kernel (nominal %d)  ->  %.2f Tlane-cycles/s per APU\n",
           pr.gcnArchName, pr.multiProcessorCount, mhz, pr.clockRate / 1000, lanes / 1e12);
    { double v[1] = { mhz }; result("sclk_measured", "MHz", mhz, v, 1); }
    printf("1 APU, %d blocks x %d threads, %d chains for throughput; latency: 1 wave per CU, dependent chain\n\n",
           blocks, thr, CH);
    printf("%-14s %12s %10s %10s %8s   %s\n", "operation", "Gop/s", "cyc/op",
           "lat cyc", "ILP", "(cycles at the measured clock; ILP = latency/throughput)");

    for (e = 0; e < NE; e++) {
        hipEvent_t t0, t1;
        double best_tp = 1e300, best_lat = 1e300, opstp, opslat, cyc, lat;
        float ms;
        int rep;
        timer_events(&t0, &t1);

        for (rep = 0; rep < 3; rep++) {
            if (E_[e].type == 0) ((k64)E_[e].tp)<<<blocks, thr>>>((uint64_t *)sink, 3, 8);
            else if (E_[e].type == 1) ((k32)E_[e].tp)<<<blocks, thr>>>((uint32_t *)sink, 3, 8);
            else ((kfd)E_[e].tp)<<<blocks, thr>>>((double *)sink, 3.0, 8);
            HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipEventRecord(t0, 0));
            if (E_[e].type == 0) ((k64)E_[e].tp)<<<blocks, thr>>>((uint64_t *)sink, 3, iters);
            else if (E_[e].type == 1) ((k32)E_[e].tp)<<<blocks, thr>>>((uint32_t *)sink, 3, iters);
            else ((kfd)E_[e].tp)<<<blocks, thr>>>((double *)sink, 3.0, iters);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best_tp = dmin(best_tp, (double)ms);

            HIP_CHECK(hipEventRecord(t0, 0));
            /* one wave per CU: nothing to overlap with, so time/iters is the true latency */
            if (E_[e].type == 0) ((k64)E_[e].lat)<<<pr.multiProcessorCount, 64>>>((uint64_t *)sink, 3, iters * 4);
            else if (E_[e].type == 1) ((k32)E_[e].lat)<<<pr.multiProcessorCount, 64>>>((uint32_t *)sink, 3, iters * 4);
            else ((kfd)E_[e].lat)<<<pr.multiProcessorCount, 64>>>((double *)sink, 3.0, iters * 4);
            HIP_CHECK(hipEventRecord(t1, 0));
            HIP_CHECK(hipEventSynchronize(t1));
            HIP_CHECK(hipEventElapsedTime(&ms, t0, t1));
            best_lat = dmin(best_lat, (double)ms);
        }
        opstp = (double)blocks * thr * CH * iters / (best_tp * 1e-3);
        opslat = (double)iters * 4 / (best_lat * 1e-3);          /* per wave */
        cyc = lanes / opstp;
        lat = ghz * 1e9 / opslat;                                 /* cycles per dependent op */
        printf("%-14s %12.1f %10.2f %10.2f %8.1f\n",
               E_[e].name, opstp / 1e9, cyc, lat, lat / cyc);
        { double v[2] = { opstp / 1e9, lat }; char nm[40];
          snprintf(nm, sizeof nm, "%s_Gops", E_[e].name); result(nm, "Gop/s", v[0], v, 1);
          snprintf(nm, sizeof nm, "%s_latcyc", E_[e].name); result(nm, "cyc", v[1], v + 1, 1); }
    }
    return 0;
}
