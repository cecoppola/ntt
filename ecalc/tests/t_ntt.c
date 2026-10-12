/* t_ntt - step 2 test: ntt.h against an O(n^2) DFT, round trips, convolution
 * against schoolbook and mpz_mul, pass-split independence, and rates.
 *
 *  1. n = 2^10, 2^11, 2^13: ntt_fwd vs the O(n^2) DFT in 128-bit arithmetic,
 *     in bit-reversed order; ntt_host_fwd vs the same
 *  2. round trip inv(fwd(x)) == x for logn = 10 .. LOGMAX (default 31) on
 *     APU0, uniform inputs; all five generators up to 2^24; every prime
 *  3. convolution: (a) cyclic convolution mod p vs schoolbook at 2^12;
 *     (b) integer product with 16-bit limbs vs mpz_mul at logn 16 .. 20,
 *     fused and unfused pointwise, every prime
 *  4. forward output and round trip bit-identical for 3 .. 7 stages per b16 pass (ntt_stg)
 *     at 2^24 (STG 8 would need a 256-row tile: LDS 34 KB, 1 block/CU - not
 *     built, RESULTS.md 33)
 *  5. rate: ms per pass at 2^LOGMAX on all APUs (paper 1.08 TB/s effective =
 *     16 B x n per pass; bench/16: 1.17); batched log L = 14, 17
 *
 * Usage: t_ntt [LOGMAX (31)]
 *        t_ntt hash [KMAX2 KMAX3]   (Phase 15 N3x: output hashes of every transform, for old / new diffs)
 *        t_ntt bench [LOGMAX [h2,h7,stride,slo,q4,q4sweep]]   (rates; the kernel variants' benches were removed with their
 *                                   switches 2026-10-12: NTT_MODMUL, NTT_B1R, NTT_PLAN, NTT_B16_BODY, NTT_R3_FUSE)
 */
#include "harness.h"
#include "../ntt.h"
#include <omp.h>

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

static int brv(size_t i, int bits) { size_t r = 0; for (int b = 0; b < bits; b++) r = (r << 1) | ((i >> b) & 1); return (int)r; }

static uint64_t hash_arr(const uint64_t *a, size_t n)
{
    uint64_t h = 0x243F6A8885A308D3ULL;
    for (size_t i = 0; i < n; i++) { h ^= a[i]; h *= 0x9E3779B97F4A7C15ULL; h ^= h >> 29; }
    return h;
}
static size_t count_diff(const uint64_t *a, const uint64_t *b, size_t n, size_t *first)
{
    size_t bad = 0; *first = n;
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) { if (!bad) *first = i; bad++; }
    return bad;
}

/* ---- Phase 13a K: device-side compare and the benchmarks for H2 (MALL), H3 (modmul), H7 (non-temporal stores) */
__global__ void k_cmp(const uint64_t *a, const uint64_t *b, size_t n, unsigned long long *bad)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    unsigned long long nb = 0;
    for (; i < n; i += st) nb += a[i] != b[i];
    if (nb) atomicAdd(bad, nb);
}
static size_t dev_diff(const uint64_t *a, const uint64_t *b, size_t n)
{
    unsigned long long *d, h = 0;
    HIP_CHECK(hipMalloc(&d, 8)); HIP_CHECK(hipMemset(d, 0, 8));
    k_cmp<<<228 * 8, 256>>>(a, b, n, d);
    HIP_CHECK(hipMemcpy(&h, d, 8, hipMemcpyDeviceToHost)); HIP_CHECK(hipFree(d));
    return (size_t)h;
}
__global__ void k_fill(uint64_t *x, size_t n, uint64_t p, uint64_t seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += st) {
        uint64_t s = (i + 1) * 0x9E3779B97F4A7C15ULL ^ seed;
        s ^= s >> 31; s *= 0xBF58476D1CE4E5B9ULL; s ^= s >> 29; s *= 0x94D049BB133111EBULL; s ^= s >> 32;
        x[i] = s % p;
    }
}
static void dev_fill(uint64_t *x, size_t n, uint64_t p, uint64_t seed) { k_fill<<<228 * 8, 256>>>(x, n, p, seed); HIP_CHECK(hipDeviceSynchronize()); }

/* timing: median of 5 trials of `reps` calls, ms per call */
#define TIME_MS(out, reps, stmt) do {                                                                  \
        hipEvent_t e0_, e1_; HIP_CHECK(hipEventCreate(&e0_)); HIP_CHECK(hipEventCreate(&e1_));          \
        { stmt; } HIP_CHECK(hipDeviceSynchronize());                                                   \
        float tr_[5];                                                                                  \
        for (int t_ = 0; t_ < 5; t_++) {                                                               \
            HIP_CHECK(hipEventRecord(e0_, 0));                                                         \
            for (int r_ = 0; r_ < (reps); r_++) { stmt; }                                              \
            HIP_CHECK(hipEventRecord(e1_, 0)); HIP_CHECK(hipEventSynchronize(e1_));                    \
            HIP_CHECK(hipEventElapsedTime(&tr_[t_], e0_, e1_)); tr_[t_] /= (reps);                     \
        }                                                                                              \
        for (int a_ = 0; a_ < 5; a_++) for (int b_ = a_ + 1; b_ < 5; b_++) if (tr_[b_] < tr_[a_]) { float z_ = tr_[a_]; tr_[a_] = tr_[b_]; tr_[b_] = z_; } \
        out = tr_[2];                                                                                  \
        HIP_CHECK(hipEventDestroy(e0_)); HIP_CHECK(hipEventDestroy(e1_));                              \
    } while (0)

/* H7: the slab pack / unpack of ntt_dist.c (k_pack, k_unpack: the same index maps), with plain and with
 * non-temporal stores */
template <int NT>
__global__ void kb_pack(const uint64_t *x, uint64_t *sb, size_t rows, size_t C, size_t cols, int size)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t i = t / C, j = t % C, s = j / cols, jl = j % cols;
        if (NT) __builtin_nontemporal_store(x[t], &sb[s * (cols * rows) + jl * rows + i]);
        else sb[s * (cols * rows) + jl * rows + i] = x[t];
    }
}
template <int NT>
__global__ void kb_unpack(const uint64_t *rb, uint64_t *x, size_t rows_k, size_t i0, size_t rows, size_t cols, int size)
{
    size_t R = rows * size, total = cols * rows_k * size, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t jl = t / (rows_k * size), i = t % (rows_k * size), r = i / rows_k, il = i % rows_k;
        if (NT) __builtin_nontemporal_store(rb[r * (cols * rows_k) + jl * rows_k + il], &x[jl * R + r * rows + i0 + il]);
        else x[jl * R + r * rows + i0 + il] = rb[r * (cols * rows_k) + jl * rows_k + il];
    }
}
__global__ void kb_read(const uint64_t *a, size_t n, uint64_t *sink)     /* a consumer that streams a buffer (the push's read) */
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    uint64_t acc = 0;
    for (; i < n; i += st) acc ^= a[i];
    if (acc == 0x5A5A5A5A5A5A5A5AULL) *sink = acc;
}
/* Phase 13b K: the stride microbench (see bench "stride") */
template <int ROT>
__global__ __launch_bounds__(256) void kb_strd(uint64_t *x, size_t S, size_t slabs, size_t rot)
{
    const int tt = threadIdx.x >> 4, bb = threadIdx.x & 15;
    const size_t b = blockIdx.x, blk_hi = b / slabs, slab = b % slabs, base = blk_hi * 128 * S;
    uint64_t v[8];
#pragma unroll
    for (int i = 0; i < 8; i++) { size_t j = tt + 16 * i; v[i] = x[base + j * S + ((slab + (ROT ? j * rot : 0)) % slabs) * 16 + bb]; }
#pragma unroll
    for (int i = 0; i < 8; i++) v[i] += 1;
    __syncthreads();                     /* as in k_b16r; the stored rows are other rows than the loaded ones (values do not matter) */
#pragma unroll
    for (int i = 0; i < 8; i++) { size_t j = 8 * tt + i; x[base + j * S + ((slab + (ROT ? j * rot : 0)) % slabs) * 16 + bb] = v[i]; }
}
static unsigned nblk(size_t n) { size_t b = (n + 255) / 256; return (unsigned)(b > 228 * 16 ? 228 * 16 : b); }

static int bench(int LOGMAX, const char *what)
{
    HIP_CHECK(hipSetDevice(0));
    size_t nmax = (size_t)1 << LOGMAX;
    uint64_t *dx, *dy;
    HIP_CHECK(hipMalloc(&dx, nmax * 8)); HIP_CHECK(hipMalloc(&dy, nmax * 8));
    ntt_ctx *c = ntt_ctx_create(0);
    dev_fill(dx, nmax, ec_P[0], 1); dev_fill(dy, nmax, ec_P[0], 2);
    printf("== t_ntt bench %s: LOGMAX %d, APU0, P0, %d stages per b16 pass; median of 5 ==\n", what, LOGMAX, ntt_stg);

    if (strstr(what, "h2")) {
        /* H2 part 1: per-pass rate against the working set.  logL = transform length, tot = points in the batch.
         * ns/pt = ms per pass per point; GB/s = 16 B x tot (read + write) per pass */
        printf("-- H2 per-pass sweep: logL tot pass [s_lo..s_hi] ms ns/pt GB/s (fwd | inv)\n");
        static const int Ls[] = {14, 17, 20, 24, 0};
        for (int li = 0; li < 5; li++) for (int lt = 18; lt <= LOGMAX; lt++) {
            int logL = Ls[li] ? Ls[li] : lt;
            if (logL > lt) continue;
            size_t tot = (size_t)1 << lt, B = tot >> logL;
            int np = ntt_npass(logL), reps = lt >= 26 ? 1 : 1 << (26 - lt);
            double sf = 0, si = 0;
            for (int ps = 0; ps < np; ps++) {
                int lo, hi; ntt_pass_bounds(logL, ps, &lo, &hi);
                float mf, mi;
                TIME_MS(mf, reps, ntt_pass(c, dx, logL, B, 0, ps, 0));
                TIME_MS(mi, reps, ntt_pass(c, dx, logL, B, 1, ps, 0));
                sf += mf; si += mi;
                printf("   H2 L%2d tot 2^%2d (%7.1f MB) pass %d [%2d..%2d] %8.3f ms %6.3f ns/pt %6.0f GB/s | %8.3f ms %6.0f GB/s\n", logL, lt, tot * 8 / 1048576.0, ps, lo, hi,
                       mf, mf * 1e6 / tot, 16.0 * tot / (mf * 1e-3) / 1e9, mi, 16.0 * tot / (mi * 1e-3) / 1e9);
            }
            float tf, ti;
            TIME_MS(tf, reps, ntt_fwd(c, dx, logL, B, 0));
            TIME_MS(ti, reps, ntt_inv(c, dx, logL, B, 0));
            printf("   H2 L%2d tot 2^%2d whole: fwd %8.3f ms (sum of passes %8.3f) inv %8.3f ms (%8.3f); %d passes, fwd %.3f ns/pt/pass\n", logL, lt, tf, sf, ti, si, np, tf * 1e6 / tot / np);
        }
    }
    if (strstr(what, "h7")) {
        /* H7: pack / unpack with plain and non-temporal stores: the kernel alone, then its effect on what follows
         * (a) the consumer of its output (a streaming read of the slabs, standing in for the push), (b) a transform
         * on a MALL-sized plane that was warm before the pack (the pollution effect) */
        printf("-- H7 non-temporal stores in pack / unpack (size 4 ranks; rows x C points)\n");
        uint64_t *sink; HIP_CHECK(hipMalloc(&sink, 8));
        for (int lt = 22; lt <= LOGMAX - 1 && lt <= 30; lt += 2) {
            size_t tot = (size_t)1 << lt, C = (size_t)1 << (lt / 2), rows = tot / C, cols = C / 4;
            uint64_t *A = dy + tot;                      /* a 2^22-point (32 MB) plane that should stay MALL-resident */
            if (tot + ((size_t)1 << 22) > nmax) break;
            for (int nt = 0; nt < 2; nt++) {
                float tp, tu, tr, ta, tpa, tua;
                if (nt) {
                    TIME_MS(tp, 3, (kb_pack<1><<<nblk(tot), 256>>>(dx, dy, rows, C, cols, 4)));
                    TIME_MS(tu, 3, (kb_unpack<1><<<nblk(tot), 256>>>(dy, dx, rows, 0, rows, cols, 4)));
                    TIME_MS(tr, 1, (kb_pack<1><<<nblk(tot), 256>>>(dx, dy, rows, C, cols, 4), kb_read<<<228 * 8, 256>>>(dy, tot, sink)));
                    TIME_MS(tpa, 1, (ntt_fwd(c, A, 22, 1, 0), kb_pack<1><<<nblk(tot), 256>>>(dx, dy, rows, C, cols, 4), ntt_fwd(c, A, 22, 1, 0)));
                    TIME_MS(tua, 1, (ntt_fwd(c, A, 22, 1, 0), kb_unpack<1><<<nblk(tot), 256>>>(dy, dx, rows, 0, rows, cols, 4), ntt_fwd(c, A, 22, 1, 0)));
                } else {
                    TIME_MS(tp, 3, (kb_pack<0><<<nblk(tot), 256>>>(dx, dy, rows, C, cols, 4)));
                    TIME_MS(tu, 3, (kb_unpack<0><<<nblk(tot), 256>>>(dy, dx, rows, 0, rows, cols, 4)));
                    TIME_MS(tr, 1, (kb_pack<0><<<nblk(tot), 256>>>(dx, dy, rows, C, cols, 4), kb_read<<<228 * 8, 256>>>(dy, tot, sink)));
                    TIME_MS(tpa, 1, (ntt_fwd(c, A, 22, 1, 0), kb_pack<0><<<nblk(tot), 256>>>(dx, dy, rows, C, cols, 4), ntt_fwd(c, A, 22, 1, 0)));
                    TIME_MS(tua, 1, (ntt_fwd(c, A, 22, 1, 0), kb_unpack<0><<<nblk(tot), 256>>>(dy, dx, rows, 0, rows, cols, 4), ntt_fwd(c, A, 22, 1, 0)));
                }
                TIME_MS(ta, 3, ntt_fwd(c, A, 22, 1, 0));
                printf("   H7 tot 2^%2d (%6.0f MB) %s: pack %8.3f ms (%5.0f GB/s)  unpack %8.3f ms  pack+read %8.3f ms  "
                       "fwd(32MB)+pack+fwd %8.3f  fwd+unpack+fwd %8.3f  (fwd alone %.3f)\n", lt, tot * 8 / 1048576.0, nt ? "NT   " : "plain",
                       tp, 16.0 * tot / (tp * 1e-3) / 1e9, tu, tr, tpa, tua, ta);
            }
        }
        /* correctness of the NT variants: same output */
        {
            size_t tot = (size_t)1 << 24, C = 4096, rows = tot / C, cols = C / 4;
            kb_pack<0><<<nblk(tot), 256>>>(dx, dy, rows, C, cols, 4);
            kb_pack<1><<<nblk(tot), 256>>>(dx, dy + tot, rows, C, cols, 4);
            HIP_CHECK(hipDeviceSynchronize());
            VERIFY(dev_diff(dy, dy + tot, tot) == 0, "H7: NT pack output differs");
        }
        HIP_CHECK(hipFree(sink));
    }

    if (strstr(what, "stride")) {
        /* Phase 13b K: the stride penalty, memory only.  kb_strd moves 128 rows x 16 columns per block like k_b16r
         * (load rows tt + 16 i, store rows 8 tt + i, x + 1 in between), row stride S = 2^s + pad points; rot > 0 shifts
         * row j's slab by j rot slabs (mod the row) - a rotated layout.  If the power-of-two strides 17 / 24 are slow and
         * the padded / rotated ones are not, the penalty is address aliasing */
        printf("-- K13b stride: N s pad rot  ms  GB/s (16 B x points touched)\n");
        static const int Ns[] = {31, 28};
        static const int cfg[][2] = {{0, 0}, {16, 0}, {128, 0}, {512, 0}, {0, 1}, {0, 33}};
        for (int ni = 0; ni < 2; ni++) {
            int lN = Ns[ni]; if (lN > LOGMAX) continue;
            size_t N = (size_t)1 << lN;
            for (int sl = 10; sl + 7 <= lN; sl++) for (int ci = 0; ci < 6; ci++) {
                if (ni && ci) continue;
                size_t pad = cfg[ci][0], rot = cfg[ci][1], S = ((size_t)1 << sl) + pad, slabs = ((size_t)1 << sl) / 16, nhi = N / (128 * S);
                if (!nhi) { if (sl + 7 == lN && pad) { printf("   STRIDE N 2^%d s %2d pad %4zu: no room\n", lN, sl, pad); } continue; }
                size_t touched = nhi * 128 * ((size_t)1 << sl);
                unsigned nb = (unsigned)(nhi * slabs);
                float ms;
                if (rot) TIME_MS(ms, 1, (kb_strd<1><<<nb, 256>>>(dx, S, slabs, rot)));
                else TIME_MS(ms, 1, (kb_strd<0><<<nb, 256>>>(dx, S, slabs, 0)));
                printf("   STRIDE N 2^%d s %2d pad %4zu rot %2zu: %8.3f ms %6.0f GB/s\n", lN, sl, pad, rot, ms, 16.0 * touched / (ms * 1e-3) / 1e9);
            }
        }
    }
    if (strstr(what, "slo")) {
        /* Phase 13b K: one b16 pass at every row stride 2^s_lo (k_b16r for 7 stages, the tile kernel for 5 and 6), in
         * one transform of 2^L points: the pass-rate table the plan choice needs */
        printf("-- K13b per-s_lo pass rates: L stg s_lo fwd ms GB/s | inv ms GB/s\n");
        static const int Ls3[] = {31, 28, 24}, Lsw[] = {24, 25, 26, 27, 28, 29, 30, 31};   /* S22: bench <LOGMAX> q4sweep = every L from 24 to 31 */
        const int *Ls = strstr(what, "q4sweep") ? Lsw : Ls3; const int nLs = strstr(what, "q4sweep") ? 8 : 3;
        for (int li = 0; li < nLs; li++) {
            int L = Ls[li]; if (L > LOGMAX) continue;
            for (int stg = 7; stg >= 5; stg--) for (int sl = 10; sl + stg <= L; sl++) {
                if (stg < 7 && sl + stg != L) continue;         /* partial passes: only at the top */
                float mf, mi;
                TIME_MS(mf, 1, ntt_pass_at(c, dx, L, 1, sl, stg, 0, 0));
                TIME_MS(mi, 1, ntt_pass_at(c, dx, L, 1, sl, stg, 1, 0));
                printf("   SLO L%2d stg %d s_lo %2d: fwd %8.3f ms %6.0f GB/s | inv %8.3f ms %6.0f GB/s\n", L, stg, sl,
                       mf, 16.0 * ((size_t)1 << L) / (mf * 1e-3) / 1e9, mi, 16.0 * ((size_t)1 << L) / (mi * 1e-3) / 1e9);
            }
        }
    }
    if (strstr(what, "q4")) {
        /* S21 (A37-Q4): the production configuration (the only one since 2026-10-12), whole forward / inverse and every pass, at 2^31, 2^28 and
         * 2^24 single.  Run by tests/t_ntt (real modmul) and tests/t_ntt_nop (ntt.c built with -DNTT_NOP_MODMUL: every modmul one FMA, the data movement
         * left; its outputs are wrong by design and are not checked here).  The two outputs side by side say what share of the time is the arithmetic. */
        printf("-- S21 Q4: production plan; L, fwd ms, inv ms, then per pass [s_lo..s_hi] fwd ms (median of 5)\n");
        static const int Ls3[] = {31, 28, 24}, Lsw[] = {24, 25, 26, 27, 28, 29, 30, 31};   /* S22: bench <LOGMAX> q4sweep = every L from 24 to 31 */
        const int *Ls = strstr(what, "q4sweep") ? Lsw : Ls3; const int nLs = strstr(what, "q4sweep") ? 8 : 3;
        for (int li = 0; li < nLs; li++) {
            int L = Ls[li]; if (L > LOGMAX) continue;
            float tf, ti;
            TIME_MS(tf, 1, ntt_fwd(c, dx, L, 1, 0));
            TIME_MS(ti, 1, ntt_inv(c, dx, L, 1, 0));
            printf("   Q4 L%2d whole: fwd %8.3f ms inv %8.3f ms\n", L, tf, ti);
            float sf = 0;
            for (int ps = 0; ps < ntt_npass(L); ps++) {
                int lo, hi; ntt_pass_bounds(L, ps, &lo, &hi);
                float mf; TIME_MS(mf, 1, ntt_pass(c, dx, L, 1, 0, ps, 0));
                sf += mf;
                printf("   Q4 L%2d pass %d [%2d..%2d]: fwd %8.3f ms %6.0f GB/s\n", L, ps, lo, hi, mf, 16.0 * ldexp(1.0, L) / (mf * 1e-3) / 1e9);
            }
            printf("   Q4 L%2d sum of passes: fwd %8.3f ms\n", L, sf);
        }
    }
    ntt_ctx_free(c);
    HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy));
    return verify_done("t_ntt bench");
}

/* ---- Phase 15 N3x: `t_ntt hash [KMAX2 (31)] [KMAX3 (29)]` and `t_ntt r3bench [KMIN (10)] [KMAX (29)]`.
 * hash: every transform the code uses -- 2^k (k = 10 .. KMAX2) and 3 2^k (k = 10 .. KMAX3), every prime, batch 1 and 3 (3
 * while the batch fits 3 2^29 points), forward, inverse and the inverse with the pointwise product in the three y layouts,
 * fused into the b1 pass (PW_FUSE 10) and not (99) -- on deterministic canonical inputs; one line per case with a
 * position-dependent device hash of the output.  Run it with the old build and the new one and diff the `H ` lines: the
 * transforms must be bitwise identical. */
__device__ static inline uint64_t n3x_mix(uint64_t z)
{
    z ^= z >> 30; z *= 0xBF58476D1CE4E5B9ULL; z ^= z >> 27; z *= 0x94D049BB133111EBULL; z ^= z >> 31; return z;
}
__global__ void k_n3x_hash(const uint64_t *x, size_t n, unsigned long long *h)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    unsigned long long a = 0;
    for (; i < n; i += st) a += n3x_mix(x[i] + (i + 1) * 0x9E3779B97F4A7C15ULL);
    atomicAdd(h, a);
}
static uint64_t n3x_hash(const uint64_t *x, size_t n)
{
    unsigned long long *d, h = 0;
    HIP_CHECK(hipMalloc(&d, 8)); HIP_CHECK(hipMemset(d, 0, 8));
    k_n3x_hash<<<228 * 8, 256>>>(x, n, d);
    HIP_CHECK(hipMemcpy(&h, d, 8, hipMemcpyDeviceToHost)); HIP_CHECK(hipFree(d));
    return (uint64_t)h;
}
static int n3x_hash_mode(int kmax2, int kmax3)
{
    printf("== t_ntt hash ==\n");
    HIP_CHECK(hipSetDevice(0));
    ntt_ctx *ctx[EC_NP];
    for (int pr = 0; pr < EC_NP; pr++) ctx[pr] = ntt_ctx_create(pr);
    const size_t cap = (size_t)1 << 31;                     /* points per buffer */
    uint64_t *dx, *dy;
    HIP_CHECK(hipMalloc(&dx, cap * 8)); HIP_CHECK(hipMalloc(&dy, cap * 8));
    long ncase = 0;
    double t0 = omp_get_wtime();
    for (int r3 = 0; r3 < 2; r3++) {
        if (r3 && !ec_has_radix3()) break;
        for (int logk = 10; logk <= (r3 ? kmax3 : kmax2); logk++) {
            const size_t len = (size_t)(r3 ? 3 : 1) << logk;
            for (int batch = 1; batch <= 3; batch += 2) {
                if (batch * len > cap || (batch > 1 && batch * len > ((size_t)3 << 29))) continue;
                const size_t n = batch * len;
                for (int pr = 0; pr < EC_NP; pr++) {
                    ntt_ctx *c = ctx[pr];
                    for (int op = 0; op < 8; op++) {
                        const uint64_t seed = 0xA5A5ULL + (uint64_t)r3 * 1000003 + (uint64_t)logk * 7919 + (uint64_t)batch * 131 + (uint64_t)pr * 17 + (uint64_t)op;
                        dev_fill(dx, n, ec_P[pr], seed);
                        if (op >= 2) dev_fill(dy, n, ec_P[pr], seed ^ 0x5A5A5A5AULL);
                        int save = ntt_pw_fuse, ym = (op - 2) % 3;
                        if (op >= 2) ntt_pw_fuse = op < 5 ? 10 : 99;
                        if (op == 0) { if (r3) ntt_fwd3(c, dx, logk, batch, 0); else ntt_fwd(c, dx, logk, batch, 0); }
                        else if (op == 1) { if (r3) ntt_inv3(c, dx, logk, batch, 0); else ntt_inv(c, dx, logk, batch, 0); }
                        else { if (r3) ntt_inv3_pw_y(c, dx, dy, ym, logk, batch, 0); else ntt_inv_pw_y(c, dx, dy, ym, logk, batch, 0); }
                        ntt_pw_fuse = save;
                        HIP_CHECK(hipDeviceSynchronize());
                        static const char *opn[] = {"fwd", "inv", "invpw_full_f", "invpw_bcast_f", "invpw_pair_f", "invpw_full_u", "invpw_bcast_u", "invpw_pair_u"};
                        printf("H %s2^%d b%d P%d %-14s %016llx\n", r3 ? "3*" : "", logk, batch, pr, opn[op], (unsigned long long)n3x_hash(dx, n));
                        ncase++;
                    }
                }
            }
            fflush(stdout);
        }
    }
    printf("t_ntt hash: %ld cases in %.1f s\n", ncase, omp_get_wtime() - t0);
    for (int pr = 0; pr < EC_NP; pr++) ntt_ctx_free(ctx[pr]);
    HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy));
    return 0;
}
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "hash")) return n3x_hash_mode(argc > 2 ? atoi(argv[2]) : 31, argc > 3 ? atoi(argv[3]) : 29);   /* Phase 15 N3x */
    if (argc > 1 && !strcmp(argv[1], "bench")) return bench(argc > 2 ? atoi(argv[2]) : 31, argc > 3 ? argv[3] : "h2,h7");   /* Phase 13b K: stride, slo; S21 q4 */
    int LOGMAX = argc > 1 ? atoi(argv[1]) : 31;
    int nd = 0, pr, logn;
    HIP_CHECK(hipGetDeviceCount(&nd));
    printf("== t_ntt: LOGMAX %d, %d devices, %d stages per b16 pass ==\n", LOGMAX, nd, ntt_stg);
    harness_meta("t_ntt");
    rng_t rng = {0x7E57ULL};

    HIP_CHECK(hipSetDevice(0));
    ntt_ctx *ctx[EC_NP];
    for (pr = 0; pr < EC_NP; pr++) ctx[pr] = ntt_ctx_create(pr);

    size_t nmax = (size_t)1 << LOGMAX;
    uint64_t *hx = (uint64_t *)malloc(nmax * 8), *hy = (uint64_t *)malloc(nmax * 8), *hz = (uint64_t *)malloc(nmax * 8);
    uint64_t *dx, *dy;
    HIP_CHECK(hipMalloc(&dx, nmax * 8)); HIP_CHECK(hipMalloc(&dy, nmax * 8));

    /* 1. O(n^2) DFT */
    printf("-- 1. fwd vs O(n^2) DFT\n");
    for (pr = 0; pr < EC_NP; pr++) {
        static const int ls[] = {10, 11, 13};
        for (int li = 0; li < 3; li++) {
            logn = ls[li];
            size_t n = (size_t)1 << logn;
            uint64_t p = ec_P[pr], wn = ec_root(pr, logn);
            for (size_t i = 0; i < n; i++) hx[i] = rng_next(&rng) % p;
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(ctx[pr], dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            memcpy(hz, hx, n * 8); ntt_host_fwd(hz, logn, pr);
            size_t bad_dev = 0, bad_host = 0;
#pragma omp parallel for reduction(+:bad_dev,bad_host)
            for (size_t k = 0; k < n; k++) {
                uint64_t wk = ec_powmod(wn, k, p), acc = 0, wj = 1;
                for (size_t j = 0; j < n; j++) { acc = (acc + ec_mulmod_ref(hx[j], wj, p)) % p; wj = ec_mulmod_ref(wj, wk, p); }
                bad_dev  += hy[brv(k, logn)] != acc;
                bad_host += hz[brv(k, logn)] != acc;
            }
            VERIFY(bad_dev == 0, "P%d 2^%d: device fwd %zu mismatches vs DFT", pr, logn, bad_dev);
            VERIFY(bad_host == 0, "P%d 2^%d: host fwd %zu mismatches vs DFT", pr, logn, bad_host);
        }
    }

    /* 2. round trips */
    printf("-- 2. round trips\n");
    for (pr = 0; pr < EC_NP; pr++) {
        for (logn = 10; logn <= LOGMAX; logn++) {
            size_t n = (size_t)1 << logn;
            int kinds = logn <= 24 ? GEN_KINDS : 1;
            for (int kind = 0; kind < kinds; kind++) {
                gen_limbs(hx, n, kind, &rng);
#pragma omp parallel for
                for (size_t i = 0; i < n; i++) hx[i] %= ec_P[pr];
                HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
                ntt_fwd(ctx[pr], dx, logn, 1, 0);
                ntt_inv(ctx[pr], dx, logn, 1, 0);
                HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
                size_t first, bad = count_diff(hx, hy, n, &first);
                VERIFY(bad == 0, "P%d 2^%d %s: %zu mismatches, first at %zu (%llu vs %llu)", pr, logn, gen_name[kind],
                       bad, first, first < n ? (unsigned long long)hy[first] : 0ULL, first < n ? (unsigned long long)hx[first] : 0ULL);
                if (logn <= 20 && kind == 0) {          /* inverse alone vs host inverse */
                    HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
                    ntt_inv(ctx[pr], dx, logn, 1, 0);
                    HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
                    memcpy(hz, hx, n * 8); ntt_host_inv(hz, logn, pr);
                    bad = count_diff(hz, hy, n, &first);
                    VERIFY(bad == 0, "P%d 2^%d: device inv vs host inv %zu mismatches", pr, logn, bad);
                }
            }
            if (pr == 0 && logn >= 28) printf("   2^%d ok\n", logn);
        }
    }
    /* batched round trip: 64 transforms of 2^14 in one call */
    {
        size_t n = (size_t)1 << 14, B = 64;
        gen_limbs(hx, n * B, GEN_UNIFORM, &rng);
#pragma omp parallel for
        for (size_t i = 0; i < n * B; i++) hx[i] %= ec_P[1];
        HIP_CHECK(hipMemcpy(dx, hx, n * B * 8, hipMemcpyHostToDevice));
        ntt_fwd(ctx[1], dx, 14, B, 0); ntt_inv(ctx[1], dx, 14, B, 0);
        HIP_CHECK(hipMemcpy(hy, dx, n * B * 8, hipMemcpyDeviceToHost));
        size_t first, bad = count_diff(hx, hy, n * B, &first);
        VERIFY(bad == 0, "batched 64 x 2^14 round trip: %zu mismatches", bad);
    }

    /* 3a. cyclic convolution vs schoolbook mod p */
    printf("-- 3. convolution\n");
    for (pr = 0; pr < EC_NP; pr++) {
        logn = 12; size_t n = (size_t)1 << logn; uint64_t p = ec_P[pr];
        for (size_t i = 0; i < n; i++) { hx[i] = rng_next(&rng) % p; hy[i] = rng_next(&rng) % p; }
        HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
        ntt_fwd(ctx[pr], dx, logn, 1, 0); ntt_fwd(ctx[pr], dy, logn, 1, 0);
        ntt_pw(ctx[pr], dx, dy, n, 0); ntt_inv(ctx[pr], dx, logn, 1, 0);
        HIP_CHECK(hipMemcpy(hz, dx, n * 8, hipMemcpyDeviceToHost));
        size_t bad = 0;
#pragma omp parallel for reduction(+:bad)
        for (size_t k = 0; k < n; k++) {
            uint64_t acc = 0;
            for (size_t j = 0; j < n; j++) acc = (acc + ec_mulmod_ref(hx[j], hy[(k - j) & (n - 1)], p)) % p;
            bad += acc != hz[k];
        }
        VERIFY(bad == 0, "P%d cyclic convolution 2^12: %zu mismatches", pr, bad);
    }
    /* 3b. integer product, 16-bit limbs, vs mpz_mul */
    {
        mpz_t A, B, C, D;
        mpz_inits(A, B, C, D, NULL);
        for (pr = 0; pr < EC_NP; pr++)
        for (logn = 16; logn <= 20; logn++)
        for (int fuse = 0; fuse < 2; fuse++) {
            size_t n = (size_t)1 << logn, half = n / 2;
            uint16_t *a16 = (uint16_t *)malloc(half * 2), *b16 = (uint16_t *)malloc(half * 2);
            for (size_t i = 0; i < half; i++) { a16[i] = (uint16_t)rng_next(&rng); b16[i] = (uint16_t)rng_next(&rng); }
            a16[half - 1] |= 0x8000; b16[half - 1] |= 0x8000;
            for (size_t i = 0; i < n; i++) { hx[i] = i < half ? a16[i] : 0; hy[i] = i < half ? b16[i] : 0; }
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(ctx[pr], dx, logn, 1, 0); ntt_fwd(ctx[pr], dy, logn, 1, 0);
            if (fuse) { int save = ntt_pw_fuse; ntt_pw_fuse = 10; ntt_inv_pw(ctx[pr], dx, dy, logn, 1, 0); ntt_pw_fuse = save; }
            else      { ntt_pw(ctx[pr], dx, dy, n, 0); ntt_inv(ctx[pr], dx, logn, 1, 0); }
            HIP_CHECK(hipMemcpy(hz, dx, n * 8, hipMemcpyDeviceToHost));
            /* coefficients < 2^32 * 2^(logn-1) < p: carry into a GMP integer */
            /* B08: carry-propagate into 16-bit limbs and import once (the old Horner loop was O(n^2): ~266 s of this section on aac7) */
            { uint16_t *c16 = (uint16_t *)malloc((n + 8) * 2); unsigned __int128 cy = 0; size_t i;
              for (i = 0; i < n; i++) { cy += hz[i]; c16[i] = (uint16_t)cy; cy >>= 16; }
              for (; i < n + 8; i++) { c16[i] = (uint16_t)cy; cy >>= 16; }
              mpz_import(C, n + 8, -1, 2, 0, 0, c16); free(c16); }
            mpz_import(A, half, -1, 2, 0, 0, a16); mpz_import(B, half, -1, 2, 0, 0, b16);
            mpz_mul(D, A, B);
            VERIFY(mpz_cmp(C, D) == 0, "P%d 2^%d %s: product != mpz_mul", pr, logn, fuse ? "fused" : "unfused");
            free(a16); free(b16);
        }
        mpz_clears(A, B, C, D, NULL);
    }

    /* 4. STG independence at 2^24 */
    printf("-- 4. pass splits\n");
    {
        logn = 24; size_t n = (size_t)1 << logn; uint64_t href = 0;
        gen_limbs(hx, n, GEN_UNIFORM, &rng);
#pragma omp parallel for
        for (size_t i = 0; i < n; i++) hx[i] %= ec_P[0];
        for (int stg = 7; stg >= 3; stg--) {
            ntt_stg = stg;
            ntt_ctx *c = ntt_ctx_create(0);                 /* fresh twiddle cache for this split */
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            uint64_t h = hash_arr(hy, n);
            if (stg == 7) href = h;
            VERIFY(h == href, "STG %d forward differs from STG 7", stg);
            ntt_inv(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            size_t first, bad = count_diff(hx, hy, n, &first);
            VERIFY(bad == 0, "STG %d round trip: %zu mismatches", stg, bad);
            printf("   STG %d: %d passes, fwd hash %016llx\n", stg, ntt_npass(logn), (unsigned long long)h);
            ntt_ctx_free(c);
        }
        ntt_stg = 7;
    }

    /* 4b. Phase 9 B1: the pointwise operand's layouts (FULL, BCAST, PAIR), fused into the b1 pass and not,
     * for 2^k and 3 2^k transforms: a batch of 6 x transforms against 6 / 1 / 3 y transforms must equal
     * the per-transform product + inverse (bit-identical) */
    printf("-- 4b. pointwise layouts (full / bcast / pair, fused and unfused, 2^k and 3 2^k)\n");
    for (int r3 = 0; r3 < 2; r3++) for (int lg = 10; lg <= 15; lg++) for (int fuse = 0; fuse < 2; fuse++) {
        int logk = lg; size_t Lt = (size_t)(r3 ? 3 : 1) << logk, B = 6, tot = B * Lt;
        if (2 * tot > nmax) continue;
        pr = (lg + r3 + fuse) % EC_NP;
        for (size_t i = 0; i < tot; i++) { hx[i] = rng_next(&rng) % ec_P[pr]; hy[i] = rng_next(&rng) % ec_P[pr]; }
        int save = ntt_pw_fuse; ntt_pw_fuse = fuse ? 10 : 40;
        for (int ym = 0; ym < 3; ym++) {
            /* reference: per transform t, y transform sel(t): product, then the inverse */
            for (size_t t = 0; t < B; t++) {
                size_t ty = ym == NTT_Y_FULL ? t : ym == NTT_Y_BCAST ? 0 : t >> 1;
                HIP_CHECK(hipMemcpy(dx, hx + t * Lt, Lt * 8, hipMemcpyHostToDevice));
                HIP_CHECK(hipMemcpy(dy, hy + ty * Lt, Lt * 8, hipMemcpyHostToDevice));
                ntt_pw(ctx[pr], dx, dy, Lt, 0);
                if (r3) ntt_inv3(ctx[pr], dx, logk, 1, 0); else ntt_inv(ctx[pr], dx, logk, 1, 0);
                HIP_CHECK(hipMemcpy(hz + t * Lt, dx, Lt * 8, hipMemcpyDeviceToHost));
            }
            HIP_CHECK(hipMemcpy(dx, hx, tot * 8, hipMemcpyHostToDevice));
            HIP_CHECK(hipMemcpy(dy, hy, tot * 8, hipMemcpyHostToDevice));
            if (r3) ntt_inv3_pw_y(ctx[pr], dx, dy, ym, logk, B, 0); else ntt_inv_pw_y(ctx[pr], dx, dy, ym, logk, B, 0);
            HIP_CHECK(hipMemcpy(hy + tot, dx, tot * 8, hipMemcpyDeviceToHost));    /* hy's tail is scratch */
            size_t first, bad = count_diff(hz, hy + tot, tot, &first);
            VERIFY(bad == 0, "P%d %s2^%d layout %d %s: %zu mismatches, first at %zu", pr, r3 ? "3*" : "", logk, ym, fuse ? "fused" : "unfused", bad, first);
            /* the old entry points agree with the layout ones */
            if (ym != NTT_Y_PAIR) {
                HIP_CHECK(hipMemcpy(dx, hx, tot * 8, hipMemcpyHostToDevice));
                if (r3) { if (ym) ntt_inv3_pw_bcast(ctx[pr], dx, dy, logk, B, 0); else ntt_inv3_pw(ctx[pr], dx, dy, logk, B, 0); }
                else    { if (ym) ntt_inv_pw_bcast(ctx[pr], dx, dy, logk, B, 0); else ntt_inv_pw(ctx[pr], dx, dy, logk, B, 0); }
                HIP_CHECK(hipMemcpy(hy + tot, dx, tot * 8, hipMemcpyDeviceToHost));
                bad = count_diff(hz, hy + tot, tot, &first);
                VERIFY(bad == 0, "P%d %s2^%d layout %d %s (old entry point): %zu mismatches", pr, r3 ? "3*" : "", logk, ym, fuse ? "fused" : "unfused", bad);
            }
        }
        ntt_pw_fuse = save;
    }

    /* 5. rates on every device */
    printf("-- 5. rates (P0)\n");
    {
        double sum_fwd = 0, sum_inv = 0;
        int np = ntt_npass(LOGMAX);
        for (int d = 0; d < nd; d++) {
            HIP_CHECK(hipSetDevice(d));
            ntt_ctx *c = ntt_ctx_create(0);
            uint64_t *ddx; HIP_CHECK(hipMalloc(&ddx, nmax * 8));
            HIP_CHECK(hipMemset(ddx, 0, nmax * 8));
            hipEvent_t e0, e1; HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1));
            float ms_f, ms_i;
            ntt_fwd(c, ddx, LOGMAX, 1, 0); ntt_inv(c, ddx, LOGMAX, 1, 0);      /* warm: tables */
            HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipEventRecord(e0, 0)); ntt_fwd(c, ddx, LOGMAX, 1, 0); HIP_CHECK(hipEventRecord(e1, 0));
            HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms_f, e0, e1));
            HIP_CHECK(hipEventRecord(e0, 0)); ntt_inv(c, ddx, LOGMAX, 1, 0); HIP_CHECK(hipEventRecord(e1, 0));
            HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms_i, e0, e1));
            double tb_f = 16.0 * nmax * np / (ms_f * 1e-3) / 1e12, tb_i = 16.0 * nmax * np / (ms_i * 1e-3) / 1e12;
            printf("   APU%d 2^%d: fwd %.1f ms (%.2f TB/s), inv %.1f ms (%.2f TB/s), %d passes\n", d, LOGMAX, ms_f, tb_f, ms_i, tb_i, np);
            sum_fwd += tb_f; sum_inv += tb_i;
            /* batched */
            for (int lgl = 14; lgl <= 17; lgl += 3) {
                size_t B = nmax >> lgl; int npb = ntt_npass(lgl);
                HIP_CHECK(hipEventRecord(e0, 0)); ntt_fwd(c, ddx, lgl, B, 0); HIP_CHECK(hipEventRecord(e1, 0));
                HIP_CHECK(hipEventSynchronize(e1)); HIP_CHECK(hipEventElapsedTime(&ms_f, e0, e1));
                double gb = 16.0 * nmax * npb / (ms_f * 1e-3) / 1e9;
                printf("      batched log L = %d: %zu transforms, %.1f ms, %.0f GB/s\n", lgl, B, ms_f, gb);
                if (d == 0) { char nm[32]; snprintf(nm, sizeof nm, "batched_logL%d", lgl); harness_result(nm, "GB/s", gb); }
            }
            HIP_CHECK(hipFree(ddx)); ntt_ctx_free(c);
        }
        harness_result("fwd_TBps_per_apu", "TB/s", sum_fwd / nd);
        harness_result("inv_TBps_per_apu", "TB/s", sum_inv / nd);
        VERIFY(sum_fwd / nd > 0.9, "forward rate %.2f TB/s below 0.9 (bench/16: 1.17)", sum_fwd / nd);
    }
    return verify_done("t_ntt");
}
