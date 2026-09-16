/* 13_multiply2 - the multiply with the fast kernels and the glue fused in.
 *
 * bench/08 established a correct pipeline but ran the simple LDS transform in
 * both directions and spent 12 of its 16 passes on glue (split, twiddle,
 * transpose, pointwise, scale).  This version:
 *
 *   - uses the register-blocked forward (bench/06, 1.50x) and the
 *     register-blocked Gentleman-Sande inverse (bench/12, 1.69x)
 *   - folds the operand split into the first forward transform's prologue
 *   - folds the four-step twiddle into that transform's epilogue
 *   - folds the inverse four-step twiddle into the last inverse transform's
 *     prologue and the 1/L scaling into its epilogue
 *
 * Passes per prime: 8.5 + 8.5 + 3 + 10 = 30 plane-touches becomes
 *                   5.5 + 5.5 + 3 + 6  = 20.
 *
 * Layout and index rules are inherited unchanged from bench/08: points are
 * stored column-major (offset o = r*N2 + c holds point c*N1 + r) because
 * Bailey's first transform must run over the stride-n2 index, and the
 * four-step twiddle root^(r*brv(s)) is split as t1[m>>11]*t2[m&2047] so it
 * costs two Shoup multiplies rather than a general modular multiply.
 *
 * Usage: 13_multiply2 [limbs] [reps]
 */
#include "ntt_kernels.h"

#define N1 2048
#define N2 2048
#define LL ((size_t)N1 * N2)
#define BPP 48
#define THREADS 256
#define RPT 8
/* The 11 stages plus the three LDS exchanges, shared by every forward kernel.
 * Parameterless so that prologue and epilogue can be written inline: code
 * blocks cannot be macro arguments because braces do not protect commas and
 * #pragma cannot appear inside them. */
#define FWD_MID()                                                             \
    do {                                                                      \
        int span, base, grp, b2;                                              \
        FWD3(1, 2, 4);                                                        \
        EXW(tid + 256 * k);                                                   \
        span = tid >> 5; base = tid & 31;                                     \
        EXR(256 * span + base + 32 * k);                                      \
        FWD3(8 + span, 16 + 2 * span, 32 + 4 * span);                         \
        EXW(256 * span + base + 32 * k);                                      \
        grp = tid >> 2; b2 = tid & 3;                                         \
        EXR(32 * grp + b2 + 4 * k);                                           \
        FWD3(64 + grp, 128 + 2 * grp, 256 + 4 * grp);                         \
        EXW(32 * grp + b2 + 4 * k);                                           \
        EXR(8 * tid + k);                                                     \
        FB(0,2,512+2*tid+0); FB(1,3,512+2*tid+0);                             \
        FB(4,6,512+2*tid+1); FB(5,7,512+2*tid+1);                             \
        FB(0,1,1024+4*tid+0); FB(2,3,1024+4*tid+1);                           \
        FB(4,5,1024+4*tid+2); FB(6,7,1024+4*tid+3);                           \
    } while (0)

#define INV_MID()                                                             \
    do {                                                                      \
        int q, r_, u, v;                                                      \
        GINV3(1024 + 4 * tid, 512 + 2 * tid, 256 + tid);                      \
        EXW(8 * tid + k);                                                     \
        q = tid >> 3; r_ = tid & 7;                                           \
        EXR(64 * q + r_ + 8 * k);                                             \
        GINV3(128 + 4 * q, 64 + 2 * q, 32 + q);                               \
        EXW(64 * q + r_ + 8 * k);                                             \
        u = tid >> 6; v = tid & 63;                                           \
        EXR(512 * u + v + 64 * k);                                            \
        GINV3(16 + 4 * u, 8 + 2 * u, 4 + u);                                  \
        EXW(512 * u + v + 64 * k);                                            \
        EXR(tid + 256 * k);                                                   \
        GB(0,2,2); GB(1,3,2); GB(4,6,3); GB(5,7,3);                           \
        GB(0,4,1); GB(1,5,1); GB(2,6,1); GB(3,7,1);                           \
    } while (0)

/* the four-step twiddle applied to slot (row, sl) */
#define TWIDDLE(z, row, sl)                                                   \
    do { uint32_t k2_ = __brev((uint32_t)(sl)) >> 21;                         \
         uint64_t m_ = ((uint64_t)(row) * k2_) & (LL - 1);                    \
         uint32_t q_ = (uint32_t)(m_ >> 11);                                  \
         uint32_t r2_ = (uint32_t)(m_ & 2047);                                \
         (z) = smul(t1[q_], t1p[q_], (z), p);                                 \
         (z) = smul(t2[r2_], t2p[r2_], (z), p); } while (0)

/* forward pass 1: read packed limbs, transform, apply the four-step twiddle */
__global__ __launch_bounds__(THREADS)
void k_fwd1(const uint64_t *limbs, size_t nlimb, uint64_t *data,
            const uint64_t *w, const uint64_t *wp,
            const uint64_t *t1, const uint64_t *t1p,
            const uint64_t *t2, const uint64_t *t2p, uint64_t p)
{
    __shared__ uint64_t s[N2];
    uint64_t *g = data + (size_t)blockIdx.x * N2;
    uint64_t x[RPT], p2 = 2 * p;
    int tid = threadIdx.x, k, row = blockIdx.x;

    /* split fused in: element (row, c) of the plane is point c*N1 + row */
#pragma unroll
    for (k = 0; k < RPT; k++) {
        size_t c = (size_t)tid + 256 * k;
        size_t j = c * (size_t)N1 + row;
        uint64_t bit = j * BPP;
        uint64_t val = 0;
        size_t li = bit >> 6;
        int off = (int)(bit & 63);
        if (li < nlimb) {
            val = limbs[li] >> off;
            if (off && li + 1 < nlimb) val |= limbs[li + 1] << (64 - off);
        }
        x[k] = val & ((1ULL << BPP) - 1);
    }
    FWD_MID();
#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t z = x[k];
        if (z >= p2) z -= p2;
        if (z >= p)  z -= p;
        TWIDDLE(z, row, 8 * tid + k);
        if (z >= p) z -= p;
        g[8 * tid + k] = z;
    }
}

__global__ __launch_bounds__(THREADS)
void k_fwd2(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N2];
    uint64_t *g = data + (size_t)blockIdx.x * N2;
    uint64_t x[RPT], p2 = 2 * p;
    int tid = threadIdx.x, k;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = g[tid + 256 * k];
    FWD_MID();
#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t z = x[k];
        if (z >= p2) z -= p2;
        if (z >= p)  z -= p;
        g[8 * tid + k] = z;
    }
}

__global__ __launch_bounds__(THREADS)
void k_inv1(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N2];
    uint64_t *g = data + (size_t)blockIdx.x * N2;
    uint64_t x[RPT], p2 = 2 * p;
    int tid = threadIdx.x, k;
#pragma unroll
    for (k = 0; k < RPT; k++) x[k] = g[8 * tid + k];
    INV_MID();
#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t z = x[k];
        if (z >= p) z -= p;
        g[tid + 256 * k] = z;
    }
}

/* inverse pass 2: untwiddle in the prologue, 1/L scaling in the epilogue */
__global__ __launch_bounds__(THREADS)
void k_inv2(uint64_t *data, const uint64_t *w, const uint64_t *wp,
            const uint64_t *t1, const uint64_t *t1p,
            const uint64_t *t2, const uint64_t *t2p,
            uint64_t sc, uint64_t scp, uint64_t p)
{
    __shared__ uint64_t s[N2];
    uint64_t *g = data + (size_t)blockIdx.x * N2;
    uint64_t x[RPT], p2 = 2 * p;
    int tid = threadIdx.x, k, row = blockIdx.x;
#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t z = g[8 * tid + k];
        TWIDDLE(z, row, 8 * tid + k);
        if (z >= p) z -= p;
        x[k] = z;
    }
    INV_MID();
#pragma unroll
    for (k = 0; k < RPT; k++) {
        uint64_t z = smul(sc, scp, x[k], p);
        if (z >= p) z -= p;
        g[tid + 256 * k] = z;
    }
}

__global__ void k_transpose_ip(uint64_t *d)
{
    __shared__ uint64_t t1[32][33], t2[32][33];
    int bi = blockIdx.y, bj = blockIdx.x, i;
    if (bj < bi) return;
    for (i = 0; i < 32; i += 8) {
        int y = threadIdx.y + i, x = threadIdx.x;
        t1[y][x] = d[(size_t)(bi * 32 + y) * N2 + bj * 32 + x];
        if (bj != bi) t2[y][x] = d[(size_t)(bj * 32 + y) * N2 + bi * 32 + x];
    }
    __syncthreads();
    for (i = 0; i < 32; i += 8) {
        int y = threadIdx.y + i, x = threadIdx.x;
        d[(size_t)(bj * 32 + y) * N2 + bi * 32 + x] = t1[x][y];
        if (bj != bi) d[(size_t)(bi * 32 + y) * N2 + bj * 32 + x] = t2[x][y];
    }
}

__global__ void k_pointwise(uint64_t *a, const uint64_t *b, uint64_t p, uint64_t J)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t st = (size_t)gridDim.x * blockDim.x;
    for (; i < LL; i += st) {
        uint64_t lo = a[i] * b[i], hi = __umul64hi(a[i], b[i]);
        uint64_t m = lo * J, u = hi - __umul64hi(m, p) + p;
        a[i] = u >= p ? u - p : u;
    }
}

/* ------------------------------ host ------------------------------------ */
struct plan {
    uint64_t p, J, scale, scalep;
    uint64_t *w, *wp, *iw, *iwp, *t1, *t1p, *t2, *t2p, *it1, *it1p, *it2, *it2p;
};

static void plan_build(struct plan *pl, uint64_t p)
{
    uint64_t wL, iwL, root, iroot, ninv, *hw, *hwp;
    int i;
    pl->p = p;
    wL = powmod(G, (p - 1) / (uint64_t)LL, p);
    iwL = powmod(wL, (uint64_t)LL - 1, p);
    root = powmod(wL, N1, p);
    iroot = powmod(root, (uint64_t)N2 - 1, p);
    ninv = powmod((uint64_t)LL % p, p - 2, p);
    pl->J = mont_j(p);
    pl->scale = mulmod(ninv, (uint64_t)(((__uint128_t)1 << 64) % p), p);
    pl->scalep = shoup_pre(pl->scale, p);
    hw = (uint64_t *)malloc(N2 * 8); hwp = (uint64_t *)malloc(N2 * 8);
#define ALLOC(x) HIP_CHECK(hipMalloc(&pl->x, N2 * 8))
    ALLOC(w); ALLOC(wp); ALLOC(iw); ALLOC(iwp);
    ALLOC(t1); ALLOC(t1p); ALLOC(t2); ALLOC(t2p);
    ALLOC(it1); ALLOC(it1p); ALLOC(it2); ALLOC(it2p);
#undef ALLOC
#define UP(d, dp) do { HIP_CHECK(hipMemcpy(pl->d, hw, N2*8, hipMemcpyHostToDevice)); \
                       HIP_CHECK(hipMemcpy(pl->dp, hwp, N2*8, hipMemcpyHostToDevice)); } while (0)
    build_table(hw, hwp, N2, root, p);   UP(w, wp);
    build_table(hw, hwp, N2, iroot, p);  UP(iw, iwp);
    for (i = 0; i < N2; i++) { hw[i] = powmod(wL, (uint64_t)i * N2, p); hwp[i] = shoup_pre(hw[i], p); }
    UP(t1, t1p);
    for (i = 0; i < N2; i++) { hw[i] = powmod(wL, (uint64_t)i, p); hwp[i] = shoup_pre(hw[i], p); }
    UP(t2, t2p);
    for (i = 0; i < N2; i++) { hw[i] = powmod(iwL, (uint64_t)i * N2, p); hwp[i] = shoup_pre(hw[i], p); }
    UP(it1, it1p);
    for (i = 0; i < N2; i++) { hw[i] = powmod(iwL, (uint64_t)i, p); hwp[i] = shoup_pre(hw[i], p); }
    UP(it2, it2p);
#undef UP
    free(hw); free(hwp);
}

int main(int argc, char **argv)
{
    size_t nlimb = argc > 1 ? (size_t)atol(argv[1]) : 2000;
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    size_t maxlimb = (LL * BPP / 2) / 64;
    uint64_t *ha, *hb, *hr, *hchk, *r1, *r2;
    uint64_t *da, *db, *plane[3];
    struct plan pl[2];
    dim3 tg(32, 8), gg(N2 / 32, N1 / 32);
    size_t i, j, outlimb;
    double t0, t1;
    int rep, ok;

    if (nlimb > maxlimb) nlimb = maxlimb;
    outlimb = 2 * nlimb + 2;

    printf("== 13_multiply2 : fast kernels + fused glue ==\n");
    meta("13_multiply2");
    printf("L = %d x %d, %d bits/point, operands %zu limbs (%.2f M digits each)\n",
           N1, N2, BPP, nlimb, nlimb * 64 / 3.3219 / 1e6);
    printf("bench/08 reference: 1.98 ms, 1.66 bytes/digit\n");

    HIP_CHECK(hipSetDevice(0));
    ha = (uint64_t *)calloc(maxlimb + 2, 8); hb = (uint64_t *)calloc(maxlimb + 2, 8);
    hr = (uint64_t *)calloc(2 * maxlimb + 4, 8); hchk = (uint64_t *)calloc(2 * maxlimb + 4, 8);
    r1 = (uint64_t *)malloc(LL * 8); r2 = (uint64_t *)malloc(LL * 8);
    for (i = 0; i < nlimb; i++) {
        ha[i] = 0x9E3779B97F4A7C15ULL * (i + 1) ^ (i * 2654435761u);
        hb[i] = 0xC2B2AE3D27D4EB4FULL * (i + 3) ^ (i * 40503u);
    }
    HIP_CHECK(hipMalloc(&da, (maxlimb + 2) * 8));
    HIP_CHECK(hipMalloc(&db, (maxlimb + 2) * 8));
    for (i = 0; i < 3; i++) HIP_CHECK(hipMalloc(&plane[i], LL * 8));
    for (i = 0; i < 2; i++) plan_build(&pl[i], i == 0 ? P1 : P2);
    HIP_CHECK(hipMemcpy(da, ha, (nlimb + 1) * 8, hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(db, hb, (nlimb + 1) * 8, hipMemcpyHostToDevice));

    /* per-kernel breakdown, one prime, so the remaining time can be attributed */
    {
        static const char *nm[7] = { "fwd1 (split+ntt+twiddle)", "transpose", "fwd2 (ntt)",
                                     "pointwise", "inv1 (ntt)", "transpose", "inv2 (tw+ntt+scale)" };
        double acc[7]; hipEvent_t e0, e1; float ms; int q, w2;
        struct plan *P = &pl[0];
        uint64_t *X = plane[0], *Y = plane[1];
        HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1));
        for (q = 0; q < 7; q++) acc[q] = 0;
        for (w2 = 0; w2 < 5; w2++) {
            for (q = 0; q < 7; q++) {
                HIP_CHECK(hipEventRecord(e0, 0));
                switch (q) {
                case 0: k_fwd1<<<N1, THREADS>>>(da, nlimb, X, P->w, P->wp, P->t1, P->t1p, P->t2, P->t2p, P->p); break;
                case 1: case 5: k_transpose_ip<<<gg, tg>>>(X); break;
                case 2: k_fwd2<<<N2, THREADS>>>(X, P->w, P->wp, P->p); break;
                case 3: k_pointwise<<<4096, 256>>>(X, Y, P->p, P->J); break;
                case 4: k_inv1<<<N2, THREADS>>>(X, P->iw, P->iwp, P->p); break;
                default: k_inv2<<<N1, THREADS>>>(X, P->iw, P->iwp, P->it1, P->it1p,
                             P->it2, P->it2p, P->scale, P->scalep, P->p); break;
                }
                HIP_CHECK(hipEventRecord(e1, 0));
                HIP_CHECK(hipEventSynchronize(e1));
                HIP_CHECK(hipEventElapsedTime(&ms, e0, e1));
                if (w2) acc[q] += ms / 4.0;
            }
        }
        printf("\nper-kernel, one prime (x2 for both, and fwd1/transpose/fwd2 run twice):\n");
        { double tot = 0;
          for (q = 0; q < 7; q++) tot += acc[q];
          for (q = 0; q < 7; q++)
              printf("  %-26s %7.3f ms  %5.1f%%\n", nm[q], acc[q], 100.0 * acc[q] / tot);
          printf("  %-26s %7.3f ms\n", "sum (one prime, one pass)", tot); }
    }

    t0 = omp_get_wtime();
    for (rep = 0; rep < reps; rep++) {
        for (i = 0; i < 2; i++) {
            uint64_t *X = plane[i], *Y = plane[i + 1];
            struct plan *P = &pl[i];
            k_fwd1<<<N1, THREADS>>>(da, nlimb, X, P->w, P->wp, P->t1, P->t1p, P->t2, P->t2p, P->p);
            k_transpose_ip<<<gg, tg>>>(X);
            k_fwd2<<<N2, THREADS>>>(X, P->w, P->wp, P->p);
            k_fwd1<<<N1, THREADS>>>(db, nlimb, Y, P->w, P->wp, P->t1, P->t1p, P->t2, P->t2p, P->p);
            k_transpose_ip<<<gg, tg>>>(Y);
            k_fwd2<<<N2, THREADS>>>(Y, P->w, P->wp, P->p);
            k_pointwise<<<4096, 256>>>(X, Y, P->p, P->J);
            k_inv1<<<N2, THREADS>>>(X, P->iw, P->iwp, P->p);
            k_transpose_ip<<<gg, tg>>>(X);
            k_inv2<<<N1, THREADS>>>(X, P->iw, P->iwp, P->it1, P->it1p, P->it2, P->it2p,
                                    P->scale, P->scalep, P->p);
        }
        HIP_CHECK(hipDeviceSynchronize());
    }
    t1 = omp_get_wtime();

    HIP_CHECK(hipMemcpy(r1, plane[0], LL * 8, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(r2, plane[1], LL * 8, hipMemcpyDeviceToHost));
    {
        uint64_t inv_p1_mod_p2 = powmod(P1 % P2, P2 - 2, P2);
        memset(hr, 0, (2 * maxlimb + 4) * 8);
        for (j = 0; j < LL; j++) {
            __uint128_t c;
            size_t coef = (j % N2) * (size_t)N1 + j / N2, bitpos, li, kk;
            uint64_t t = (r2[j] + P2 - r1[j] % P2) % P2, hi, cc;
            __uint128_t lo, acc;
            int off;
            t = mulmod(t, inv_p1_mod_p2, P2);
            c = (__uint128_t)r1[j] + (__uint128_t)P1 * t;
            if (c == 0) continue;
            bitpos = coef * BPP; li = bitpos >> 6; off = (int)(bitpos & 63);
            lo = c << off; hi = off ? (uint64_t)(c >> (128 - off)) : 0;
            acc = (__uint128_t)hr[li] + (uint64_t)lo;              hr[li] = (uint64_t)acc;
            acc = (__uint128_t)hr[li+1] + (uint64_t)(lo >> 64) + (uint64_t)(acc >> 64);
            hr[li+1] = (uint64_t)acc;
            acc = (__uint128_t)hr[li+2] + hi + (uint64_t)(acc >> 64);
            hr[li+2] = (uint64_t)acc;
            kk = li + 3; cc = (uint64_t)(acc >> 64);
            while (cc) { __uint128_t a2 = (__uint128_t)hr[kk] + cc;
                         hr[kk] = (uint64_t)a2; cc = (uint64_t)(a2 >> 64); kk++; }
        }
    }
    {
        uint64_t ha61 = hash61(ha, nlimb), hb61 = hash61(hb, nlimb);
        uint64_t want = (uint64_t)(((__uint128_t)ha61 * hb61) % M61);
        uint64_t got = hash61(hr, outlimb) % M61;
        printf("\nCHECK hash61(a)*hash61(b) vs hash61(product): %s\n",
               want == got ? "OK" : "FAILED");
    }
    if (nlimb <= 20000) {
        for (i = 0; i < nlimb; i++) {
            uint64_t cc = 0;
            for (j = 0; j < nlimb; j++) {
                __uint128_t t = (__uint128_t)ha[i] * hb[j] + hchk[i + j] + cc;
                hchk[i + j] = (uint64_t)t; cc = (uint64_t)(t >> 64);
            }
            { size_t k2 = i + nlimb;
              while (cc) { __uint128_t t = (__uint128_t)hchk[k2] + cc;
                           hchk[k2] = (uint64_t)t; cc = (uint64_t)(t >> 64); k2++; } }
        }
        ok = 1;
        for (i = 0; i < outlimb; i++) if (hr[i] != hchk[i]) { ok = 0; break; }
        printf("VERIFY product vs schoolbook: %s", ok ? "OK" : "FAILED");
        if (!ok) printf("  (limb %zu: ntt %016llx school %016llx)", i,
                        (unsigned long long)hr[i], (unsigned long long)hchk[i]);
        printf("\n");
    } else {
        printf("VERIFY vs schoolbook: skipped (%zu limbs)\n", nlimb);
    }
    {
        double ms = (t1 - t0) * 1e3 / reps;
        printf("\ntime %.2f ms/multiply (1 APU)  -> %.2fx over bench/08\n", ms, 1.98 / ms);
        { double v[1] = { ms }; result("multiply_ms_1APU", "ms", ms, v, 1); }
        printf("workspace %.0f MB = %.2f bytes/digit\n", 3.0 * LL * 8 / 1e6,
               3.0 * LL * 8.0 / (LL * (double)BPP / 3.3219));
    }
    return 0;
}
