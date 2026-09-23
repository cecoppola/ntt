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
 *  4. forward output and round trip bit-identical for NTT_B16_STG = 3 .. 7
 *     at 2^24 (STG 8 would need a 256-row tile: LDS 34 KB, 1 block/CU - not
 *     built, RESULTS.md 33)
 *  5. rate: ms per pass at 2^LOGMAX on all APUs (paper 1.08 TB/s effective =
 *     16 B x n per pass; bench/16: 1.17); batched log L = 14, 17
 *
 * Usage: t_ntt [LOGMAX (31)]
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

/* one configuration of the switches */
struct kcfg { int mm, mall; const char *name; int var, b1r, plan; };      /* Phase 13b K: + NTT_B1R, NTT_PLAN */
static void kcfg_set(const struct kcfg *k) { ntt_modmul = k->mm; ntt_mall = k->mall; ntt_b16_var = k->var; ntt_b1r = k->b1r; ntt_plan = k->plan; }

/* the H3 / H2 identity check: every operation under configuration k against the default (MM 0, MALL off), on the
 * device.  ops: fwd, inv (input canonical random: no round-trip identity to hide behind), fused inverse with the
 * pointwise product in the three layouts, and the radix-3 pair. */
static void ident_check(const struct kcfg *k, int pr, int logn, size_t batch, ntt_ctx *c, uint64_t *dx, uint64_t *dr, uint64_t *dy, int with_y)
{
    struct kcfg ref = {0, 0, "ref"};
    size_t n = batch << logn;
    uint64_t p = ec_P[pr];
    for (int op = 0; op < (with_y ? 6 : 2); op++) {
        if (op == 5 && !ec_has_radix3()) continue;
        size_t nn = op == 5 ? 3 * n : n;
        for (int pass = 0; pass < 2; pass++) {
            kcfg_set(pass ? k : &ref);
            uint64_t *out = pass ? dx : dr;
            dev_fill(out, nn, p, 0x1234 + logn * 77 + op);
            if (op >= 2) dev_fill(dy, nn, p, 0x5678 + logn);
            switch (op) {
            case 0: ntt_fwd(c, out, logn, batch, 0); break;
            case 1: ntt_inv(c, out, logn, batch, 0); break;
            case 2: case 3: case 4: { int save = ntt_pw_fuse; ntt_pw_fuse = 10; ntt_inv_pw_y(c, out, dy, op - 2, logn, batch, 0); ntt_pw_fuse = save; break; }
            case 5: ntt_fwd3(c, out, logn, batch, 0); ntt_inv3_pw_y(c, out, dy, NTT_Y_FULL, logn, batch, 0); break;
            }
            HIP_CHECK(hipDeviceSynchronize());
        }
        size_t bad = dev_diff(dx, dr, nn);
        static const char *opn[] = {"fwd", "inv", "inv_pw full", "inv_pw bcast", "inv_pw pair", "fwd3+inv3_pw"};
        VERIFY(bad == 0, "%s P%d 2^%d x %zu %s: %zu mismatches vs default", k->name, pr, logn, batch, opn[op], bad);
    }
    kcfg_set(&ref);
}

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
    struct kcfg ref = {0, 0, "ref"};
    printf("== t_ntt bench %s: LOGMAX %d, APU0, P0, NTT_B16_STG %d, body %d; median of 5 ==\n", what, LOGMAX, ntt_stg, ntt_b16_body);

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
    if (strstr(what, "mall")) {
        /* H2 part 2: the MALL-chunked schedule (NTT_MALL = lc) against the default */
        printf("-- H2 NTT_MALL chunking: logL tot lc fwd ms inv ms (vs lc 0)\n");
        static const int Ls[] = {14, 17, 20, 0};
        static const int lcs[] = {0, 20, 21, 22, 23, 24, 25, 26};
        for (int li = 0; li < 4; li++) for (int lt = 24; lt <= LOGMAX; lt += 2) {
            int logL = Ls[li] ? Ls[li] : lt;
            size_t tot = (size_t)1 << lt, B = tot >> logL;
            float f0 = 0, i0 = 0;
            for (int k = 0; k < 8; k++) {
                if (lcs[k] >= lt) continue;
                struct kcfg kc = {0, lcs[k], ""};
                kcfg_set(&kc);
                float tf, ti;
                TIME_MS(tf, 1, ntt_fwd(c, dx, logL, B, 0));
                TIME_MS(ti, 1, ntt_inv(c, dx, logL, B, 0));
                if (!lcs[k]) { f0 = tf; i0 = ti; }
                printf("   MALL L%2d tot 2^%2d lc %2d: fwd %8.3f ms (%.3fx) inv %8.3f ms (%.3fx)\n", logL, lt, lcs[k], tf, f0 / tf, ti, i0 / ti);
            }
            kcfg_set(&ref);
        }
    }
    if (strstr(what, "h3")) {
        /* H3: the modmul variants on the whole transform */
        printf("-- H3 modmul: logL tot MM fwd ms inv ms (vs MM 0)\n");
        static const int Ls[] = {14, 17, 20, 0};
        for (int li = 0; li < 4; li++) for (int lt = 20; lt <= LOGMAX; lt++) {
            int logL = Ls[li] ? Ls[li] : lt;
            if (Ls[li] && lt != 28 && lt != LOGMAX) continue;
            size_t tot = (size_t)1 << lt, B = tot >> logL;
            int reps = lt >= 26 ? 1 : 1 << (26 - lt);
            float f0 = 0, i0 = 0;
            for (int mm = 0; mm < 3; mm++) {
                struct kcfg kc = {mm, 0, ""};
                kcfg_set(&kc);
                float tf, ti;
                TIME_MS(tf, reps, ntt_fwd(c, dx, logL, B, 0));
                TIME_MS(ti, reps, ntt_inv(c, dx, logL, B, 0));
                if (!mm) { f0 = tf; i0 = ti; }
                printf("   MM L%2d tot 2^%2d mm %d: fwd %8.3f ms (%.3fx) inv %8.3f ms (%.3fx)\n", logL, lt, mm, tf, f0 / tf, ti, i0 / ti);
            }
            kcfg_set(&ref);
        }
    }
    if (strstr(what, "h6")) {
        /* H6: the register-blocked body's variants (NTT_B16_VAR: 1 = 4 blocks/CU, 2 = block order, 3 = both),
         * with the default modmul and with NTT_MODMUL 1; whole transforms, then per pass at 2^31 and 2^24 */
        printf("-- H6 k_b16r variants: logL tot var mm fwd ms inv ms (vs var 0 mm 0)\n");
        static const int cf[][2] = {{31, 31}, {30, 30}, {27, 27}, {24, 24}, {24, 28}, {20, 28}, {17, 28}, {14, 28}};
        for (int ci = 0; ci < 8; ci++) {
            int logL = cf[ci][0], lt = cf[ci][1];
            if (lt > LOGMAX) continue;
            size_t tot = (size_t)1 << lt, B = tot >> logL;
            int reps = lt >= 26 ? 1 : 1 << (26 - lt);
            float f0 = 0, i0 = 0;
            for (int mm = 0; mm < 2; mm++) for (int var = 0; var < 4; var++) {
                struct kcfg kc = {mm, 0, "", var};
                kcfg_set(&kc);
                float tf, ti;
                TIME_MS(tf, reps, ntt_fwd(c, dx, logL, B, 0));
                TIME_MS(ti, reps, ntt_inv(c, dx, logL, B, 0));
                if (!mm && !var) { f0 = tf; i0 = ti; }
                printf("   H6 L%2d tot 2^%2d var %d mm %d: fwd %8.3f ms (%.3fx) inv %8.3f ms (%.3fx)\n", logL, lt, var, mm, tf, f0 / tf, ti, i0 / ti);
            }
            kcfg_set(&ref);
        }
        static const int pl_[] = {31, 24};
        for (int k = 0; k < 2; k++) {
            int logL = pl_[k]; if (logL > LOGMAX) continue;
            for (int var = 0; var < 4; var++) {
                struct kcfg kc = {0, 0, "", var}; kcfg_set(&kc);
                for (int ps = 0; ps < ntt_npass(logL) - 1; ps++) {
                    int lo, hi; ntt_pass_bounds(logL, ps, &lo, &hi);
                    float mf, mi;
                    TIME_MS(mf, 1, ntt_pass(c, dx, logL, 1, 0, ps, 0));
                    TIME_MS(mi, 1, ntt_pass(c, dx, logL, 1, 1, ps, 0));
                    printf("   H6 pass L%2d var %d pass %d [%2d..%2d] fwd %8.3f ms (%5.0f GB/s) inv %8.3f ms (%5.0f GB/s)\n", logL, var, ps, lo, hi,
                           mf, 16.0 * ((size_t)1 << logL) / (mf * 1e-3) / 1e9, mi, 16.0 * ((size_t)1 << logL) / (mi * 1e-3) / 1e9);
                }
            }
            kcfg_set(&ref);
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
        static const int Ls[] = {31, 28, 24};
        for (int li = 0; li < 3; li++) {
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
    if (strstr(what, "b1")) {
        /* Phase 13b K: the b1 pass alone: the paper's kernel against the register-blocked one (NTT_B1R 3, 4) and the
         * b1 lengths 2^11, 2^12, at MM 1 (the default) and MM 0 */
        printf("-- K13b b1 pass: tot mm b1r lb fwd ms GB/s | inv ms GB/s (x vs b1r 0)\n");
        for (int lt = 20; lt <= LOGMAX; lt += (lt < 24 ? 4 : lt < 28 ? 2 : 1)) for (int mm = 1; mm >= 0; mm--) {
            size_t tot = (size_t)1 << lt;
            int reps = lt >= 26 ? 1 : 1 << (26 - lt);
            float f0 = 0, i0 = 0;
            for (int r = 0; r <= 4; r++) for (int lb = 10; lb <= 12; lb++) {
                if (r == 1 || r == 2 || (!r && lb > 10)) continue;
                struct kcfg kc = {mm, 0, "", 0, r, lb == 10 ? 0 : lb * 10}; kcfg_set(&kc);
                int np = ntt_npass(20);
                float mf, mi;
                TIME_MS(mf, reps, ntt_pass(c, dx, 20, tot >> 20, 0, np - 1, 0));
                TIME_MS(mi, reps, ntt_pass(c, dx, 20, tot >> 20, 1, np - 1, 0));
                if (!r) { f0 = mf; i0 = mi; }
                printf("   B1 tot 2^%2d mm %d b1r %d lb %d: fwd %8.3f ms %6.0f GB/s (%.3fx) | inv %8.3f ms %6.0f GB/s (%.3fx)\n", lt, mm, r, lb,
                       mf, 16.0 * tot / (mf * 1e-3) / 1e9, f0 / mf, mi, 16.0 * tot / (mi * 1e-3) / 1e9, i0 / mi);
            }
            kcfg_set(&ref);
        }
    }
    if (strstr(what, "whole")) {
        /* Phase 13b K: whole transforms (fwd, inv, fused inverse) under the switches, against the default (MM 1, b1r 0,
         * plan 0); single transforms 2^20 .. 2^LOGMAX and batches of 2^14, 2^17, 2^20, 2^24 */
        printf("-- K13b whole transforms: logL tot config fwd ms inv ms inv_pw ms (x vs default)\n");
        static const struct kcfg wc[] = {{1, 0, "default"}, {1, 0, "B1R3", 0, 3}, {1, 0, "B1R4", 0, 4}, {1, 0, "B1R4+P101", 0, 4, 101},
                                         {1, 0, "B1R4+P110", 0, 4, 110}, {1, 0, "B1R4+P111", 0, 4, 111}, {1, 0, "B1R4+P120", 0, 4, 120},
                                         {1, 0, "B1R4+P121", 0, 4, 121}, {1, 0, "B1R3+P111", 0, 3, 111}, {1, 0, "B1R4+P1", 0, 4, 1}};
        const int nw = sizeof wc / sizeof wc[0];
        static const int Ls[] = {14, 17, 20, 24, 0};
        for (int li = 0; li < 5; li++) for (int lt = 20; lt <= LOGMAX; lt++) {
            int logL = Ls[li] ? Ls[li] : lt;
            if (Ls[li] && (lt < 26 || (lt != 28 && lt != LOGMAX)) ) continue;
            if (logL > lt) continue;
            size_t tot = (size_t)1 << lt, B = tot >> logL;
            int reps = lt >= 26 ? 1 : 1 << (26 - lt);
            float f0 = 0, i0 = 0, p0 = 0;
            for (int k = 0; k < nw; k++) {
                kcfg_set(&wc[k]);
                float tf, ti, tp;
                TIME_MS(tf, reps, ntt_fwd(c, dx, logL, B, 0));
                TIME_MS(ti, reps, ntt_inv(c, dx, logL, B, 0));
                TIME_MS(tp, reps, ntt_inv_pw(c, dx, dy, logL, B, 0));
                if (!k) { f0 = tf; i0 = ti; p0 = tp; }
                char pl[64] = ""; int np = ntt_npass(logL);
                for (int ps = 0; ps < np; ps++) { int lo, hi; ntt_pass_bounds(logL, ps, &lo, &hi); snprintf(pl + strlen(pl), sizeof pl - strlen(pl), "%s%d", ps ? "," : "", lo); }
                printf("   W L%2d tot 2^%2d %-10s [s_lo %s]: fwd %8.3f ms (%.3fx) inv %8.3f ms (%.3fx) inv_pw %8.3f ms (%.3fx)\n", logL, lt, wc[k].name, pl,
                       tf, f0 / tf, ti, i0 / ti, tp, p0 / tp);
            }
            kcfg_set(&ref);
        }
    }
    if (strstr(what, "pass")) {
        /* Phase 13b K: every pass of the forward and the inverse under the plans, at 2^31, 2^30, 2^28, 2^24 single */
        printf("-- K13b per pass under the plans: L config pass [s_lo..s_hi] fwd ms GB/s | inv ms GB/s\n");
        static const struct kcfg pc[] = {{1, 0, "default"}, {1, 0, "B1R4", 0, 4}, {1, 0, "B1R4+P111", 0, 4, 111}, {1, 0, "B1R4+P121", 0, 4, 121}, {1, 0, "B1R4+P101", 0, 4, 101}};
        static const int Ls[] = {31, 30, 28, 24};
        for (int li = 0; li < 4; li++) for (int k = 0; k < 5; k++) {
            int L = Ls[li]; if (L > LOGMAX) continue;
            kcfg_set(&pc[k]);
            size_t tot = (size_t)1 << L;
            float sf = 0, si = 0;
            for (int ps = 0; ps < ntt_npass(L); ps++) {
                int lo, hi; ntt_pass_bounds(L, ps, &lo, &hi);
                float mf, mi;
                TIME_MS(mf, 1, ntt_pass(c, dx, L, 1, 0, ps, 0));
                TIME_MS(mi, 1, ntt_pass(c, dx, L, 1, 1, ps, 0));
                sf += mf; si += mi;
                printf("   P L%2d %-10s pass %d [%2d..%2d]: fwd %8.3f ms %6.0f GB/s | inv %8.3f ms %6.0f GB/s\n", L, pc[k].name, ps, lo, hi,
                       mf, 16.0 * tot / (mf * 1e-3) / 1e9, mi, 16.0 * tot / (mi * 1e-3) / 1e9);
            }
            printf("   P L%2d %-10s sum: fwd %8.3f ms inv %8.3f ms\n", L, pc[k].name, sf, si);
            kcfg_set(&ref);
        }
    }
    ntt_ctx_free(c);
    HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy));
    return verify_done("t_ntt bench");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "bench")) return bench(argc > 2 ? atoi(argv[2]) : 31, argc > 3 ? argv[3] : "h2,mall,h3,h7");   /* Phase 13b K: stride, slo, b1, whole, pass */
    int LOGMAX = argc > 1 ? atoi(argv[1]) : 31;
    int nd = 0, pr, logn;
    HIP_CHECK(hipGetDeviceCount(&nd));
    printf("== t_ntt: LOGMAX %d, %d devices, NTT_B16_STG %d ==\n", LOGMAX, nd, ntt_stg);
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
            mpz_set_ui(C, 0);
            for (size_t i = n; i-- > 0;) { mpz_mul_2exp(C, C, 16); mpz_add_ui(C, C, hz[i]); }
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
        /* register-blocked body (1) and with radix-4 stages (2), each with the LDS and the ds_swizzle B<->C
         * exchange (Phase 9 B4): bit-identical forward, exact round trip, and the inverse pass output itself
         * identical to the tile kernel's (the forward output, inverted without the scale fusion, compared) */
        uint64_t hinv_ref = 0;
        for (int sw = 0; sw < 2; sw++) for (ntt_b16_body = 0; ntt_b16_body <= 2; ntt_b16_body++) {
            if (ntt_b16_body == 0 && sw) continue;
            ntt_b16_xchg = sw;
            ntt_ctx *c = ntt_ctx_create(0);
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            uint64_t h = hash_arr(hy, n);
            VERIFY(h == href, "body %d xchg %d: forward differs from the tile kernel", ntt_b16_body, sw);
            ntt_inv(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            size_t first, bad = count_diff(hx, hy, n, &first);
            VERIFY(bad == 0, "body %d xchg %d: round trip %zu mismatches", ntt_b16_body, sw, bad);
            /* the inverse's intermediate: inv over the forward output of a batch of 2 (no round-trip identity to hide behind) */
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            ntt_inv(c, dx, logn - 1, 2, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            uint64_t hi = hash_arr(hy, n);
            if (ntt_b16_body == 0) hinv_ref = hi;
            VERIFY(hi == hinv_ref, "body %d xchg %d: inverse output differs from the tile kernel", ntt_b16_body, sw);
            printf("   body %d xchg %d: fwd hash %016llx %s, inv hash %016llx %s\n", ntt_b16_body, sw, (unsigned long long)h, h == href ? "== tile kernel" : "DIFFERS",
                   (unsigned long long)hi, hi == hinv_ref ? "==" : "DIFFERS");
            ntt_ctx_free(c);
        }
        ntt_b16_body = 0; ntt_b16_xchg = 0;
        /* Shoup b1 pass: forward output canonical and identical; round trip exact */
        ntt_b1_shoup = 1;
        {
            ntt_ctx *c = ntt_ctx_create(0);
            HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice));
            ntt_fwd(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            uint64_t h = hash_arr(hy, n);
            VERIFY(h == href, "Shoup b1: forward differs");
            ntt_inv(c, dx, logn, 1, 0);
            HIP_CHECK(hipMemcpy(hy, dx, n * 8, hipMemcpyDeviceToHost));
            size_t first, bad = count_diff(hx, hy, n, &first);
            VERIFY(bad == 0, "Shoup b1: round trip %zu mismatches", bad);
            printf("   Shoup b1 pass: fwd hash %016llx %s\n", (unsigned long long)h, h == href ? "== tile kernel" : "DIFFERS");
            ntt_ctx_free(c);
        }
        ntt_b1_shoup = 0;
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

    /* 4c. Phase 13a K: the switches (NTT_MODMUL 1, 2 = H3; NTT_MALL = H2, alone and combined) give outputs
     * bit-identical to the default for fwd, inv, the fused pointwise inverse in all three layouts and the
     * radix-3 pair, at every length 2^10 .. 2^LOGMAX and at batches (incl. a non-power-of-two one) */
    printf("-- 4c. NTT_MODMUL / NTT_MALL variants bit-identical to the default\n");
    {
        uint64_t *dr; HIP_CHECK(hipMalloc(&dr, nmax * 8));
        static const struct kcfg ks[] = {{1, 0, "MM1"}, {2, 0, "MM2"}, {0, 16, "MALL16"}, {1, 20, "MM1+MALL20"}, {2, 24, "MM2+MALL24"}, {0, 22, "MALL22"},
                                         {0, 0, "VAR1", 1}, {0, 0, "VAR2", 2}, {1, 0, "MM1+VAR3", 3},
                                         /* Phase 13b K: the register-blocked b1 (NTT_B1R) and the pass plans (NTT_PLAN) */
                                         {0, 0, "B1R3", 0, 3}, {1, 0, "MM1+B1R3", 0, 3}, {0, 0, "B1R4", 0, 4}, {1, 0, "MM1+B1R4", 0, 4},
                                         {2, 0, "MM2+B1R4", 0, 4, 111}, {0, 0, "P101", 0, 0, 101}, {1, 0, "MM1+B1R4+P110", 0, 4, 110},
                                         {1, 0, "MM1+B1R4+P111", 0, 4, 111}, {0, 0, "B1R3+P111", 0, 3, 111}, {1, 0, "MM1+B1R4+P120", 0, 4, 120},
                                         {1, 0, "MM1+B1R3+P121", 0, 3, 121}, {1, 0, "MM1+B1R4+P121", 0, 4, 121}, {1, 20, "MM1+MALL20+B1R4+P111", 0, 4, 111},
                                         {1, 0, "MM1+B1R4+P1", 0, 4, 1}};
        int save_body = ntt_b16_body; ntt_b16_body = 1; ntt_b16_xchg = 0;
        for (size_t ki = 0; ki < sizeof ks / sizeof ks[0]; ki++) {
            for (logn = 10; logn <= LOGMAX; logn++) {
                pr = (logn + (int)ki) % EC_NP;
                ident_check(&ks[ki], pr, logn, 1, ctx[pr], dx, dr, dy, logn + 2 <= LOGMAX && logn <= 27);
            }
            static const int bl[][2] = {{11, 1000}, {14, 64}, {17, 16}, {20, 6}, {12, 3}};
            for (int b = 0; b < 5; b++) if (((size_t)3 * bl[b][1] << bl[b][0]) <= nmax)
                ident_check(&ks[ki], (b + (int)ki) % EC_NP, bl[b][0], bl[b][1], ctx[(b + (int)ki) % EC_NP], dx, dr, dy, 1);
            printf("   %-21s ok to 2^%d\n", ks[ki].name, LOGMAX);
        }
        ntt_b16_body = save_body; ntt_modmul = 0; ntt_mall = 0; ntt_b16_var = 0; ntt_b1r = 0; ntt_plan = 0;
        HIP_CHECK(hipFree(dr));
    }

    /* 5. rates on every device */
    for (int body = 0; body < 5; body++) {
    ntt_b16_body = body >= 3 ? 1 : body; ntt_b1_shoup = body == 3; ntt_b16_xchg = body == 4;
    printf("-- 5. rates (P0, STG 7, body %d = %s)\n", body, body == 4 ? "register-blocked + ds_swizzle exchange" : body == 3 ? "register-blocked + Shoup b1" : body == 2 ? "register-blocked + radix-4" : body ? "register-blocked" : "tile kernel");
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
        char nm[40]; snprintf(nm, sizeof nm, "fwd_TBps_per_apu_body%d", body); harness_result(nm, "TB/s", sum_fwd / nd);
        snprintf(nm, sizeof nm, "inv_TBps_per_apu_body%d", body); harness_result(nm, "TB/s", sum_inv / nd);
        VERIFY(sum_fwd / nd > 0.9, "forward rate %.2f TB/s below 0.9 (bench/16: 1.17)", sum_fwd / nd);
    }
    }
    ntt_b16_body = 0; ntt_b1_shoup = 0; ntt_b16_xchg = 0;
    return verify_done("t_ntt");
}
