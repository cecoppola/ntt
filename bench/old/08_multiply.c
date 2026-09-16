/* 08_multiply - a complete big-integer multiply through the 2-prime NTT.
 *
 * This is the load-bearing milestone: split -> forward 4-step NTT -> pointwise
 * -> inverse -> CRT -> carry, verified end to end.  Everything above it in the
 * plan (binary splitting, Newton division, radix conversion) is scheduling;
 * everything below is arithmetic already validated by bench/01..07.
 *
 * Correctness first: the row transforms here are the simple LDS radix-2
 * kernels bench/04 verified against schoolbook cyclic convolution, not the
 * faster register-blocked ones.  bench/06 showed the fast kernel is
 * bit-identical, so it can be substituted once the pipeline is trusted.
 *
 * Structure, L = N1*N2 points of b bits, row-major N1 rows x N2 columns:
 *     fwd(x) = rowNTT (DIT, bit-reversed out) ; twiddle w_L^(i1*brv(s))
 *              ; transpose ; rowNTT
 *     inv(y) = rowINTT (GS, bit-reversed in) ; transpose ; untwiddle
 *              ; rowINTT ; scale by 1/L
 * inv is the exact step-by-step reverse of fwd, so the pair is correct by
 * construction whatever permutation fwd leaves the frequencies in -- all a
 * convolution needs.
 *
 * The four-step twiddle w_L^(i1*k2) would need an L-entry table (a whole extra
 * plane).  Instead m = i1*k2 is split as q*N2 + r and looked up in two tables
 * of N2 entries, costing one modular multiply -- DESIGN.md 4.5.
 *
 * Usage: 08_multiply [limbs] [reps]
 */
#include "common_ntt.h"

#define P1 0x3FFEDF0000000001ULL     /* 39943 * 105 * 2^40 + 1 */
#define P2 0x3FF6420000000001ULL     /* 39922 * 105 * 2^40 + 1 */
#define G  11ULL
#define N1 2048
#define N2 2048
#define LL ((size_t)N1 * N2)
#define BPP 48                        /* bits per point */
#define THREADS 256

/* ------------------------------ device ---------------------------------- */
__device__ static inline uint64_t dmulmod_shoup(uint64_t w, uint64_t wp,
                                                uint64_t y, uint64_t p)
{
    uint64_t q = __umul64hi(wp, y);
    return w * y - q * p;                     /* [0,2p) */
}

/* Montgomery multiply for the one place both operands vary (the pointwise
 * product).  Returns a*b*2^-64 mod p; the constant 2^64 is folded into the
 * final 1/L scaling, so nothing else has to know. */
__device__ static inline uint64_t dmulmont(uint64_t a, uint64_t b,
                                           uint64_t p, uint64_t J)
{
    uint64_t lo = a * b;
    uint64_t hi = __umul64hi(a, b);
    uint64_t m  = lo * J;
    uint64_t mh = __umul64hi(m, p);
    uint64_t u  = hi - mh + p;          /* [0,2p) */
    return u >= p ? u - p : u;
}

/* DIT forward, natural in, bit-reversed out; lazy [0,4p) */
__global__ __launch_bounds__(THREADS)
void k_rows_fwd(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N2];
    uint64_t *g = data + (size_t)blockIdx.x * N2;
    uint64_t p2 = 2 * p;
    int tid = threadIdx.x, e, m, h, logh, b;

    for (e = tid; e < N2; e += THREADS) s[e] = g[e];
    __syncthreads();
    logh = 10;
    for (m = 1, h = N2 / 2; m < N2; m <<= 1, h >>= 1, logh--) {
        for (b = tid; b < N2 / 2; b += THREADS) {
            int i = b >> logh;
            int j = ((b >> logh) << (logh + 1)) | (b & (h - 1));
            uint64_t U = s[j], V;
            if (U >= p2) U -= p2;
            V = dmulmod_shoup(w[m + i], wp[m + i], s[j + h], p);
            s[j] = U + V;
            s[j + h] = U - V + p2;
        }
        __syncthreads();
    }
    for (e = tid; e < N2; e += THREADS) {
        uint64_t v = s[e];
        if (v >= p2) v -= p2;
        if (v >= p)  v -= p;
        g[e] = v;
    }
}

/* Gentleman-Sande inverse, bit-reversed in, natural out */
__global__ __launch_bounds__(THREADS)
void k_rows_inv(uint64_t *data, const uint64_t *w, const uint64_t *wp, uint64_t p)
{
    __shared__ uint64_t s[N2];
    uint64_t *g = data + (size_t)blockIdx.x * N2;
    uint64_t p2 = 2 * p;
    int tid = threadIdx.x, e, m, h, logh, b;

    for (e = tid; e < N2; e += THREADS) s[e] = g[e];
    __syncthreads();
    logh = 0;
    for (m = N2 / 2, h = 1; m >= 1; m >>= 1, h <<= 1, logh++) {
        for (b = tid; b < N2 / 2; b += THREADS) {
            int i = b >> logh;
            int j = ((b >> logh) << (logh + 1)) | (b & (h - 1));
            uint64_t U = s[j], V = s[j + h], X;
            X = U + V;
            if (X >= p2) X -= p2;
            s[j] = X;
            s[j + h] = dmulmod_shoup(w[m + i], wp[m + i], U - V + p2, p);
        }
        __syncthreads();
    }
    for (e = tid; e < N2; e += THREADS) {
        uint64_t v = s[e];
        if (v >= p) v -= p;
        g[e] = v;
    }
}

/* Four-step twiddle: element (i1,s) *= root^(i1 * brv(s)).
 * m = i1*brv(s) splits as q*N2 + r, and root^m = t1[q] * t2[r], so this is
 * two Shoup multiplies against precomputed tables -- no general modular
 * multiply and no 128-bit division anywhere. */
__global__ void k_twiddle(uint64_t *data, const uint64_t *t1, const uint64_t *t1p,
                          const uint64_t *t2, const uint64_t *t2p, uint64_t p)
{
    size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; idx < LL; idx += stride) {
        uint32_t i1 = (uint32_t)(idx >> 11);
        uint32_t k2 = __brev((uint32_t)(idx & 2047)) >> 21;
        uint64_t m = ((uint64_t)i1 * k2) & (LL - 1);
        uint32_t q = (uint32_t)(m >> 11), r = (uint32_t)(m & 2047);
        uint64_t v = dmulmod_shoup(t1[q], t1p[q], data[idx], p);   /* [0,2p) */
        v = dmulmod_shoup(t2[r], t2p[r], v, p);                    /* [0,2p) */
        data[idx] = v;
    }
}

/* In-place transpose of the square N1 x N2 array, by swapping tile (bi,bj)
 * with tile (bj,bi).  N1 == N2, so this needs no second plane -- which is what
 * lets a whole multiply run in 3 planes instead of 6, and memory capacity is
 * what sets the digit count. */
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

__global__ void k_transpose(const uint64_t *in, uint64_t *out)
{
    __shared__ uint64_t tile[32][33];
    int bx = blockIdx.x * 32, by = blockIdx.y * 32, i;
    for (i = 0; i < 32; i += 8) {
        int r = by + threadIdx.y + i, c = bx + threadIdx.x;
        if (r < N1 && c < N2) tile[threadIdx.y + i][threadIdx.x] = in[(size_t)r * N2 + c];
    }
    __syncthreads();
    for (i = 0; i < 32; i += 8) {
        int r = bx + threadIdx.y + i, c = by + threadIdx.x;
        if (r < N2 && c < N1) out[(size_t)r * N1 + c] = tile[threadIdx.x][threadIdx.y + i];
    }
}

__global__ void k_pointwise(uint64_t *a, const uint64_t *b, uint64_t p, uint64_t J)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < LL; i += stride)
        a[i] = dmulmont(a[i], b[i], p, J);
}

__global__ void k_scale(uint64_t *a, uint64_t f, uint64_t fp, uint64_t p)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < LL; i += stride) {
        uint64_t v = dmulmod_shoup(f, fp, a[i], p);
        a[i] = v >= p ? v - p : v;
    }
}

/* Split a limb array into BPP-bit points; every point is < 2^48 < p so no
 * modular reduction is needed.
 *
 * Points are stored COLUMN-major: memory offset o = r*N2 + c holds point
 * index j = c*N1 + r.  The four-step's first transform must run over the
 * logical index with stride n2, and the row kernel transforms the contiguous
 * index c -- so c has to be that index.  Writes stay coalesced; only the
 * bit-extraction read is strided. */
__global__ void k_split(const uint64_t *limbs, size_t nlimb, uint64_t *pl)
{
    size_t o = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; o < LL; o += stride) {
        size_t j = (o % N2) * (size_t)N1 + o / N2;
        uint64_t bit = j * BPP, v = 0;
        size_t li = bit >> 6;
        int off = (int)(bit & 63);
        if (li < nlimb) {
            v = limbs[li] >> off;
            if (off && li + 1 < nlimb) v |= limbs[li + 1] << (64 - off);
        }
        pl[o] = v & ((1ULL << BPP) - 1);
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

/* mod 2^61-1 hash of a limb array, the production error check (Yee) */
#define M61 0x1FFFFFFFFFFFFFFFULL
static uint64_t hash61(const uint64_t *a, size_t n)
{
    __uint128_t h = 0;
    size_t i;
    uint64_t sh = 1;
    for (i = 0; i < n; i++) {
        h += (__uint128_t)(a[i] % M61) * sh;
        h = (h >> 61) + (h & M61);
        sh = (uint64_t)(((__uint128_t)sh * (((__uint128_t)1 << 64) % M61)) % M61);
    }
    h = (h >> 61) + (h & M61);
    return (uint64_t)(h % M61);
}

struct plan {
    uint64_t p, root, iroot, ninv, J, scale, scalep;
    uint64_t *w, *wp, *iw, *iwp;     /* row tables, N2 entries */
    uint64_t *t1, *t1p, *t2, *t2p;   /* four-step twiddle + Shoup quotients */
    uint64_t *it1, *it1p, *it2, *it2p;
};

/* p^-1 mod 2^64 by Newton iteration (6 doublings from 1 bit to 64).
 * dmulmont uses the subtractive REDC form u = hi - hi(m*p) + p, which wants
 * the plain inverse, not its negation. */
static uint64_t mont_j(uint64_t p)
{
    uint64_t x = 1, i;
    for (i = 0; i < 6; i++) x *= 2 - p * x;
    return x;
}

static void plan_build(struct plan *pl, uint64_t p)
{
    uint64_t wL, iwL;
    int i;
    pl->p = p;
    wL = powmod(G, (p - 1) / (uint64_t)LL, p);          /* L-th root */
    iwL = powmod(wL, (uint64_t)LL - 1, p);
    pl->root = powmod(wL, N1, p);                        /* N2-th root */
    pl->iroot = powmod(pl->root, (uint64_t)N2 - 1, p);
    pl->ninv = powmod((uint64_t)LL % p, p - 2, p);
    pl->J = mont_j(p);
    /* the pointwise Montgomery product leaves a factor 2^-64, so fold 2^64
     * into the 1/L scaling */
    pl->scale = mulmod(pl->ninv,
                       (uint64_t)((((__uint128_t)1 << 64) % p)), p);
    pl->scalep = shoup_pre(pl->scale, p);
    HIP_CHECK(hipMalloc(&pl->w,  N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->wp, N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->iw, N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->iwp,N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->t1, N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->t1p,N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->t2, N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->t2p,N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->it1,N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->it1p,N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->it2,N2 * sizeof(uint64_t)));
    HIP_CHECK(hipMalloc(&pl->it2p,N2 * sizeof(uint64_t)));
    {
        uint64_t *hw = (uint64_t *)malloc(N2 * sizeof(uint64_t));
        uint64_t *hwp = (uint64_t *)malloc(N2 * sizeof(uint64_t));
        build_table(hw, hwp, N2, pl->root, p);
        HIP_CHECK(hipMemcpy(pl->w, hw, N2 * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(pl->wp, hwp, N2 * sizeof(uint64_t), hipMemcpyHostToDevice));
        build_table(hw, hwp, N2, pl->iroot, p);
        HIP_CHECK(hipMemcpy(pl->iw, hw, N2 * sizeof(uint64_t), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(pl->iwp, hwp, N2 * sizeof(uint64_t), hipMemcpyHostToDevice));
#define FILL(dst, dstp, expr)                                                  \
        do {                                                                   \
            for (i = 0; i < N2; i++) { hw[i] = (expr); hwp[i] = shoup_pre(hw[i], p); } \
            HIP_CHECK(hipMemcpy(dst, hw, N2 * sizeof(uint64_t), hipMemcpyHostToDevice));  \
            HIP_CHECK(hipMemcpy(dstp, hwp, N2 * sizeof(uint64_t), hipMemcpyHostToDevice));\
        } while (0)
        FILL(pl->t1, pl->t1p, powmod(wL, (uint64_t)i * N2, p));
        FILL(pl->t2, pl->t2p, powmod(wL, (uint64_t)i, p));
        FILL(pl->it1, pl->it1p, powmod(iwL, (uint64_t)i * N2, p));
        FILL(pl->it2, pl->it2p, powmod(iwL, (uint64_t)i, p));
#undef FILL
        free(hw); free(hwp);
    }
}

/* both transforms are fully in place */
static void fwd(struct plan *pl, uint64_t *d)
{
    dim3 tg(32, 8), gg(N2 / 32, N1 / 32);
    k_rows_fwd<<<N1, THREADS>>>(d, pl->w, pl->wp, pl->p);
    k_twiddle<<<4096, 256>>>(d, pl->t1, pl->t1p, pl->t2, pl->t2p, pl->p);
    k_transpose_ip<<<gg, tg>>>(d);
    k_rows_fwd<<<N2, THREADS>>>(d, pl->w, pl->wp, pl->p);
}

static void inv(struct plan *pl, uint64_t *d)
{
    dim3 tg(32, 8), gg(N2 / 32, N1 / 32);
    k_rows_inv<<<N2, THREADS>>>(d, pl->iw, pl->iwp, pl->p);
    k_transpose_ip<<<gg, tg>>>(d);
    k_twiddle<<<4096, 256>>>(d, pl->it1, pl->it1p, pl->it2, pl->it2p, pl->p);
    k_rows_inv<<<N1, THREADS>>>(d, pl->iw, pl->iwp, pl->p);
    k_scale<<<4096, 256>>>(d, pl->scale, pl->scalep, pl->p);
}

int main(int argc, char **argv)
{
    size_t nlimb = argc > 1 ? (size_t)atol(argv[1]) : 2000;
    int reps = argc > 2 ? atoi(argv[2]) : 3;
    size_t maxlimb = (LL * BPP / 2) / 64;
    uint64_t *ha, *hb, *hr, *hchk;
    uint64_t *da, *db, *plane[3];
    uint64_t *r1, *r2;
    struct plan pl[2];
    size_t i, j, outlimb;
    double t0, t1;
    int rep, ok;

    if (nlimb > maxlimb) nlimb = maxlimb;
    outlimb = 2 * nlimb + 2;

    printf("== 08_multiply : full 2-prime NTT big-integer multiply ==\n");
    meta("08_multiply");
    printf("L = %d x %d = %zu points, %d bits/point -> %.2f Mbit product max\n",
           N1, N2, LL, BPP, LL * (double)BPP / 1e6);
    printf("operands: %zu limbs = %.3f Mbit each (max %zu limbs)\n",
           nlimb, nlimb * 64 / 1e6, maxlimb);
    printf("workspace: 3 planes x %.1f MB = %.1f MB for %.2f M digits\n",
           LL * 8 / 1e6, 3.0 * LL * 8 / 1e6,
           LL * (double)BPP / 3.3219 / 1e6);

    HIP_CHECK(hipSetDevice(0));
    ha = (uint64_t *)calloc(maxlimb + 2, 8);
    hb = (uint64_t *)calloc(maxlimb + 2, 8);
    hr = (uint64_t *)calloc(2 * maxlimb + 4, 8);
    hchk = (uint64_t *)calloc(2 * maxlimb + 4, 8);
    r1 = (uint64_t *)malloc(LL * 8);
    r2 = (uint64_t *)malloc(LL * 8);

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

    t0 = omp_get_wtime();
    for (rep = 0; rep < reps; rep++) {
        for (i = 0; i < 2; i++) {
            uint64_t *X = plane[i], *Y = plane[i + 1];
            k_split<<<4096, 256>>>(da, nlimb, X);
            fwd(&pl[i], X);
            k_split<<<4096, 256>>>(db, nlimb, Y);
            fwd(&pl[i], Y);
            k_pointwise<<<4096, 256>>>(X, Y, pl[i].p, pl[i].J);
            inv(&pl[i], X);          /* result for prime i stays in plane[i] */
        }
        HIP_CHECK(hipDeviceSynchronize());
    }
    t1 = omp_get_wtime();

    HIP_CHECK(hipMemcpy(r1, plane[0], LL * 8, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(r2, plane[1], LL * 8, hipMemcpyDeviceToHost));

    /* Garner CRT then carry propagation, on the CPU for now */
    {
        uint64_t inv_p1_mod_p2 = powmod(P1 % P2, P2 - 2, P2);
        __uint128_t carry = 0;
        size_t bitpos;
        memset(hr, 0, (2 * maxlimb + 4) * 8);
        for (j = 0; j < LL; j++) {
            __uint128_t c;
            size_t coef = (j % N2) * (size_t)N1 + j / N2;   /* undo the layout */
            uint64_t t = (r2[j] + P2 - r1[j] % P2) % P2;
            t = mulmod(t, inv_p1_mod_p2, P2);
            c = (__uint128_t)r1[j] + (__uint128_t)P1 * t;
            if (c == 0) continue;
            bitpos = coef * BPP;
            {   /* add c << bitpos into hr */
                size_t li = bitpos >> 6;
                int off = (int)(bitpos & 63);
                __uint128_t lo = c << off;
                uint64_t hi = off ? (uint64_t)(c >> (128 - off)) : 0;
                __uint128_t acc = (__uint128_t)hr[li] + (uint64_t)lo;
                hr[li] = (uint64_t)acc;
                acc = (__uint128_t)hr[li + 1] + (uint64_t)(lo >> 64) + (uint64_t)(acc >> 64);
                hr[li + 1] = (uint64_t)acc;
                acc = (__uint128_t)hr[li + 2] + hi + (uint64_t)(acc >> 64);
                hr[li + 2] = (uint64_t)acc;
                {   /* propagate any remaining carry */
                    size_t k = li + 3;
                    uint64_t cc = (uint64_t)(acc >> 64);
                    while (cc) {
                        __uint128_t a2 = (__uint128_t)hr[k] + cc;
                        hr[k] = (uint64_t)a2;
                        cc = (uint64_t)(a2 >> 64);
                        k++;
                    }
                }
            }
        }
        (void)carry;
    }

    /* Homomorphic check, valid at any size: hash61 is a ring homomorphism
     * onto Z/(2^61-1), so hash(a*b) must equal hash(a)*hash(b).  This is the
     * production error detector (Yee); a mismatch proves a fault. */
    {
        uint64_t ha61 = hash61(ha, nlimb), hb61 = hash61(hb, nlimb);
        uint64_t want = (uint64_t)(((__uint128_t)ha61 * hb61) % M61);
        uint64_t got = hash61(hr, outlimb) % M61;
        printf("\nCHECK hash61(a)*hash61(b) = %016llx, hash61(product) = %016llx : %s\n",
               (unsigned long long)want, (unsigned long long)got,
               want == got ? "OK" : "FAILED");
    }

    /* schoolbook reference, only where it is affordable */
    if (nlimb > 20000) {
        printf("VERIFY vs schoolbook: skipped (%zu limbs would be O(n^2))\n", nlimb);
        goto timing;
    }
    for (i = 0; i < nlimb; i++) {
        uint64_t cc = 0;
        for (j = 0; j < nlimb; j++) {
            __uint128_t t = (__uint128_t)ha[i] * hb[j] + hchk[i + j] + cc;
            hchk[i + j] = (uint64_t)t;
            cc = (uint64_t)(t >> 64);
        }
        {
            size_t k = i + nlimb;
            while (cc) {
                __uint128_t t = (__uint128_t)hchk[k] + cc;
                hchk[k] = (uint64_t)t;
                cc = (uint64_t)(t >> 64);
                k++;
            }
        }
    }

    ok = 1;
    for (i = 0; i < outlimb; i++) if (hr[i] != hchk[i]) { ok = 0; break; }
    printf("\nVERIFY product vs schoolbook: %s", ok ? "OK" : "FAILED");
    if (!ok) printf("  (limb %zu: ntt %016llx school %016llx)", i,
                    (unsigned long long)hr[i], (unsigned long long)hchk[i]);
    printf("\nhash61 ntt=%016llx school=%016llx  %s\n",
           (unsigned long long)hash61(hr, outlimb),
           (unsigned long long)hash61(hchk, outlimb),
           hash61(hr, outlimb) == hash61(hchk, outlimb) ? "match" : "MISMATCH");

timing:
    printf("\ntime %.2f ms/multiply (1 APU, %d reps)\n",
           (t1 - t0) * 1e3 / reps, reps);
    {
        double ms = (t1 - t0) * 1e3 / reps;
        double bf = 6.0 * (LL / 2.0) * 22.0;      /* 3 transforms x 2 primes */
        printf("product capacity %.2f M decimal digits; operands %.2f M digits each\n",
               LL * (double)BPP / 3.3219 / 1e6, LL * (double)BPP / 2 / 3.3219 / 1e6);
        printf("effective %.0f Gbfly/s on 1 APU (bench/04 kernel alone: ~390)\n",
               bf / (ms * 1e-3) / 1e9);
        printf("workspace %.0f MB for a %.2f M digit product = %.2f bytes/digit\n",
               3.0 * LL * 8 / 1e6, LL * (double)BPP / 3.3219 / 1e6,
               3.0 * LL * 8.0 / (LL * (double)BPP / 3.3219));
    }
    return 0;
}
