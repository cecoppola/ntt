/*
 * ntt_gpu_stockham.hip — Stockham auto-sort NTT, GPU kernel for MI300A / gfx942
 *
 * Purpose:   GPU implementation of the Stockham auto-sort NTT. Eliminates
 *            bit-reversal via double-buffer ping-pong between two device
 *            arrays. Produces natural-order output from natural-order input.
 * Algorithm: Stockham DIT, radix-2. Per-stage kernel launch; each thread
 *            handles one butterfly. After log2(n) stages no bit-reversal pass
 *            is needed — output is ready in-order in the result buffer.
 * Reduction: per-stage, using 64-bit multiply (q < 2^32 guarantees no
 *            overflow: w*(v%q) < q^2 < 2^64). No __uint128_t on device.
 * Ref:       Gentleman & Sande, AFIPS 1966. Stockham, AFIPS 1966.
 * Target:    AMD MI300A (gfx942, CDNA3, wavefront=64). Dev: 6900XT (gfx1100).
 *   Build:   hipcc -O2 -Wall --offload-arch=gfx942 -o ntt_gpu_stockham_mi300a \
 *                  ntt_gpu_stockham.hip
 */

#include <hip/hip_runtime.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── ANSI formatting ─────────────────────────────────────────────────────── */
#define ANSI_WHT "\033[1;37m"
#define ANSI_CYN "\033[1;36m"
#define ANSI_GRN "\033[1;32m"
#define ANSI_YLW "\033[1;33m"
#define ANSI_RED "\033[1;31m"
#define ANSI_RST "\033[0m"

/* ── HIP error check ─────────────────────────────────────────────────────── */
#define HIP_CHECK(call)                                                        \
    do {                                                                       \
        hipError_t _e = (call);                                                \
        if (_e != hipSuccess) {                                                \
            fprintf(stderr, "HIP error %s:%d — %s\n",                         \
                    __FILE__, __LINE__, hipGetErrorString(_e));                 \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/* ── NTT parameter struct (matches ntt.h layout) ────────────────────────── */
typedef struct {
    uint64_t n;
    uint64_t q;
    uint64_t omega;
    uint64_t omega_inv;
    uint64_t n_inv;
    uint32_t log2_n;
} ntt_params_t;

/* ── GPU hardware config ─────────────────────────────────────────────────── */
typedef struct {
    char   name[256];
    int    warp_size;
    int    cu_count;
    int    max_shared_b;
    size_t total_mem;
    int    block_size;
} gpu_config_t;
static gpu_config_t g_gpu;

/* ── Kernel launch bounds ────────────────────────────────────────────────── */
/* 256 threads/block, ≥2 blocks/CU: broad occupancy across CDNA3 and RDNA3. */
#define NTT_MAX_BLOCK  256
#define NTT_MIN_BLOCKS   2

/* ═══════════════════════════════════════════════════════════════════════════
 * DEVICE ARITHMETIC
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * dev_mulmod: compute (a * b) % q on device.
 * Requires a < q and b < q and q < 2^32, so a*b < q^2 < 2^64.
 * This is safe for ML-KEM (q=3329 < 2^12) and ML-DSA (q=8380417 < 2^23).
 */
__device__ __forceinline__
uint64_t dev_mulmod(uint64_t a, uint64_t b, uint64_t q)
{
    return (a * b) % q;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * KERNELS
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * stockham_stage_kernel: one Stockham DIT butterfly stage.
 *
 * Each thread tid in [0, n/2) handles one butterfly.
 *
 * Stage geometry (p = 2^s, qs = n / 2^(s+1)):
 *   j   = tid % p              group index   (j in [0, p))
 *   k   = tid / p              butterfly index within group (k in [0, qs))
 *   src indices: j+k*p (first half) and j+k*p + n/2 (second half)
 *   dst indices: j+k*(2p) and j+k*(2p)+p
 *   twiddle:     tw[j * qs]
 *
 * src[0..n/2) always holds the "top" halves and src[n/2..n) the "bottom"
 * halves due to the write pattern of the previous stage. No branch needed.
 */
__global__ __launch_bounds__(NTT_MAX_BLOCK, NTT_MIN_BLOCKS)
void stockham_stage_kernel(const uint64_t * __restrict__ src,
                                 uint64_t * __restrict__ dst,
                           const uint64_t * __restrict__ tw,
                           uint64_t n, uint64_t q, uint32_t s)
{
    uint64_t tid  = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    uint64_t half = n >> 1;
    if (tid >= half) return;

    uint64_t p  = (uint64_t)1 << s;    /* group stride    */
    uint64_t qs = n >> (s + 1);        /* butterflies/grp  */
    uint64_t p2 = p << 1;

    uint64_t j  = tid & (p - 1);       /* tid % p (p is power of 2) */
    uint64_t k  = tid >> s;            /* tid / p                   */

    uint64_t idx_u = j + k * p;        /* always in [0, n/2)  */
    uint64_t idx_v = idx_u + half;     /* always in [n/2, n)  */

    uint64_t u  = src[idx_u];
    uint64_t v  = src[idx_v];
    uint64_t w  = tw[j * qs];

    /* Per-stage reduction: reduce v before multiplying to keep product < 2^64 */
    uint64_t wv = dev_mulmod(w, v % q, q);

    dst[j + k * p2]      = u + wv;       /* lazy — will be reduced next stage */
    dst[j + k * p2 + p]  = u + q - wv;  /* keep ≥ 0                          */
}

/*
 * stockham_reduce_kernel: final reduction pass — bring all values to [0, q).
 * Each thread reduces one element. Run after all log2(n) butterfly stages.
 */
__global__ __launch_bounds__(NTT_MAX_BLOCK, NTT_MIN_BLOCKS)
void stockham_reduce_kernel(uint64_t * __restrict__ a, uint64_t n, uint64_t q)
{
    uint64_t tid = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (tid < n) a[tid] %= q;
}

/*
 * stockham_scale_kernel: multiply every element by n_inv mod q.
 * Applied after the inverse NTT butterfly stages.
 */
__global__ __launch_bounds__(NTT_MAX_BLOCK, NTT_MIN_BLOCKS)
void stockham_scale_kernel(uint64_t * __restrict__ a, uint64_t n,
                           uint64_t q, uint64_t n_inv)
{
    uint64_t tid = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (tid < n) a[tid] = dev_mulmod(a[tid] % q, n_inv, q);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MODULAR ARITHMETIC  (host-side: twiddle precomputation)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t h_mod_pow(uint64_t base, uint64_t exp, uint64_t m)
{
    uint64_t r = 1; base %= m;
    for (; exp; exp >>= 1) {
        if (exp & 1) r = (uint64_t)((__uint128_t)r * base % m);
        base = (uint64_t)((__uint128_t)base * base % m);
    }
    return r;
}
static uint64_t h_mod_inv(uint64_t a, uint64_t m) { return h_mod_pow(a, m-2, m); }

static int ntt_params_init(ntt_params_t *p)
{
    if (!p->n || (p->n & (p->n-1))) return -1;
    p->omega_inv = h_mod_inv(p->omega, p->q);
    p->n_inv     = h_mod_inv(p->n,     p->q);
    uint64_t t = p->n; p->log2_n = 0;
    while (t > 1) { t >>= 1; p->log2_n++; }
    return 0;
}

static uint64_t *alloc_twiddles(const ntt_params_t *p, uint64_t root)
{
    uint64_t *tw = (uint64_t *)malloc(p->n / 2 * sizeof(uint64_t));
    if (!tw) return NULL;
    tw[0] = 1;
    for (uint64_t k = 1; k < p->n / 2; k++)
        tw[k] = (uint64_t)((__uint128_t)tw[k-1] * root % p->q);
    return tw;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GPU NTT LAUNCH WRAPPER
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * gpu_stockham: run Stockham NTT on device.
 * Allocates two device buffers; ping-pongs between them for log2(n) stages.
 * The result always ends up back in d_a via a final device memcpy if needed.
 * After all stages, applies reduce; for INTT also applies scale.
 *
 * is_inverse: 0 = forward NTT, 1 = inverse (adds scale pass).
 */
static void gpu_stockham(uint64_t *a_host, const uint64_t *tw_host,
                         const ntt_params_t *p, int is_inverse)
{
    uint64_t n    = p->n;
    uint64_t half = n >> 1;
    int      bsz  = g_gpu.block_size;
    int      gsz_half = (int)((half + bsz - 1) / bsz);
    int      gsz_full = (int)((n    + bsz - 1) / bsz);

    uint64_t *d_a, *d_b, *d_tw;
    HIP_CHECK(hipMalloc((void **)&d_a,  n    * sizeof *d_a));
    HIP_CHECK(hipMalloc((void **)&d_b,  n    * sizeof *d_b));
    HIP_CHECK(hipMalloc((void **)&d_tw, half * sizeof *d_tw));

    HIP_CHECK(hipMemcpy(d_a,  a_host,  n    * sizeof *d_a,  hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_tw, tw_host, half * sizeof *d_tw, hipMemcpyHostToDevice));

    /* Ping-pong: src → dst each stage; swap pointers */
    uint64_t *d_src = d_a, *d_dst = d_b;
    for (uint32_t s = 0; s < p->log2_n; s++) {
        hipLaunchKernelGGL(stockham_stage_kernel, gsz_half, bsz, 0, 0,
                           d_src, d_dst, d_tw, n, p->q, s);
        HIP_CHECK(hipGetLastError());
        /* Swap */
        uint64_t *tmp = d_src; d_src = d_dst; d_dst = tmp;
    }

    /* d_src now holds the result. Final reduction. */
    hipLaunchKernelGGL(stockham_reduce_kernel, gsz_full, bsz, 0, 0,
                       d_src, n, p->q);
    HIP_CHECK(hipGetLastError());

    if (is_inverse) {
        hipLaunchKernelGGL(stockham_scale_kernel, gsz_full, bsz, 0, 0,
                           d_src, n, p->q, p->n_inv);
        HIP_CHECK(hipGetLastError());
    }

    HIP_CHECK(hipDeviceSynchronize());

    /* Copy result back; d_src may be d_a or d_b depending on log2_n parity */
    HIP_CHECK(hipMemcpy(a_host, d_src, n * sizeof *d_a, hipMemcpyDeviceToHost));

    HIP_CHECK(hipFree(d_a));
    HIP_CHECK(hipFree(d_b));
    HIP_CHECK(hipFree(d_tw));
}

/* ── Public API (matches ntt.h signatures) ──────────────────────────────── */
void ntt_forward(uint64_t *a, const uint64_t *twiddles, const ntt_params_t *p)
{
    gpu_stockham(a, twiddles, p, 0);
}

void ntt_inverse(uint64_t *a, const uint64_t *twiddles_inv, const ntt_params_t *p)
{
    gpu_stockham(a, twiddles_inv, p, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DISPLAY HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_gpu_summary(void)
{
    double mem_gb = g_gpu.total_mem / (1024.0 * 1024.0 * 1024.0);
    printf(ANSI_CYN "  ── GPU Hardware ─────────────────────────────────────────\n" ANSI_RST);
    printf("  ┌──────────────────────────┬─────────────────────────┐\n");
    printf("  │ %-24s │ %-23s │\n", "Device", g_gpu.name);
    printf("  │ %-24s │ %-23d │\n", "Wavefront size",   g_gpu.warp_size);
    printf("  │ %-24s │ %-23d │\n", "Compute units",    g_gpu.cu_count);
    printf("  │ %-24s │ %-23d │\n", "LDS/block (bytes)",g_gpu.max_shared_b);
    printf("  │ %-24s │ %.2f %-18s │\n", "Total memory", mem_gb, "GB");
    printf("  │ %-24s │ %-23d │\n", "Launch block size",g_gpu.block_size);
    printf("  └──────────────────────────┴─────────────────────────┘\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CPU REFERENCE  (static — for selftest cross-check only)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * ref_stockham_core: CPU Stockham (same algorithm as ntt_stockham.c).
 * Used to generate expected output for the GPU selftest without linking
 * against ntt_stockham.c.
 */
static void ref_stockham_forward(uint64_t *a, const uint64_t *tw,
                                  const ntt_params_t *p)
{
    uint64_t n = p->n, q = p->q, half = n >> 1;
    uint64_t *buf = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!buf) { fprintf(stderr, "ref_stockham: malloc failed\n"); return; }

    uint64_t *src = a, *dst = buf;
    for (uint32_t s = 0; s < p->log2_n; s++) {
        uint64_t pp = (uint64_t)1 << s;
        uint64_t qs = n >> (s + 1);
        uint64_t p2 = pp << 1;
        for (uint64_t j = 0; j < pp; j++) {
            uint64_t w = tw[j * qs];
            for (uint64_t k = 0; k < qs; k++) {
                uint64_t idx = j + k * pp;
                uint64_t u   = src[idx];
                uint64_t wv  = (uint64_t)((__uint128_t)w * (src[idx + half] % q) % q);
                dst[j + k * p2]      = u + wv;
                dst[j + k * p2 + pp] = u + q - wv;
            }
        }
        uint64_t *tmp = src; src = dst; dst = tmp;
    }
    for (uint64_t i = 0; i < n; i++) src[i] %= q;
    if (src != a) memcpy(a, src, n * sizeof(uint64_t));
    free(buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SELFTEST
 * ═══════════════════════════════════════════════════════════════════════════ */

static int selftest(const ntt_params_t *p)
{
    uint64_t n = p->n, q = p->q;
    const char *lbl = (q==3329)?"ML-KEM":(q==8380417)?"ML-DSA":"custom";

    printf(ANSI_CYN "  ── Selftest: %s (n=%lu q=%lu ω=%lu) ──────────────────\n"
           ANSI_RST, lbl, n, q, p->omega);

    uint64_t *tw  = alloc_twiddles(p, p->omega);
    uint64_t *twi = alloc_twiddles(p, p->omega_inv);
    uint64_t *a   = (uint64_t *)malloc(n * sizeof(uint64_t));
    uint64_t *ref = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!tw || !twi || !a || !ref) {
        free(tw); free(twi); free(a); free(ref); return -1;
    }

    /* Fill with pseudo-random pattern */
    for (uint64_t i = 0; i < n; i++) a[i] = ref[i] = (i * 6364136223846793005ULL + 1) % q;

    /* Test 1: GPU round-trip */
    ntt_forward(a, tw,  p);
    ntt_inverse(a, twi, p);
    int rt_ok = 1;
    for (uint64_t i = 0; i < n; i++) if (a[i] != ref[i]) { rt_ok = 0; break; }
    printf("  %-36s %s\n", "GPU round-trip (fwd→inv = identity)",
           rt_ok ? ANSI_GRN "PASS" ANSI_RST : ANSI_RED "FAIL" ANSI_RST);

    /* Test 2: GPU forward vs CPU Stockham reference element-wise */
    for (uint64_t i = 0; i < n; i++) a[i] = ref[i] = (i * 6364136223846793005ULL + 1) % q;
    ntt_forward(a, tw, p);
    ref_stockham_forward(ref, tw, p);
    int cmp_ok = 1;
    for (uint64_t i = 0; i < n; i++) if (a[i] != ref[i]) { cmp_ok = 0; break; }
    printf("  %-36s %s\n", "GPU vs CPU Stockham (element-wise)",
           cmp_ok ? ANSI_GRN "PASS" ANSI_RST : ANSI_RED "FAIL" ANSI_RST);

    /* Test 3: unit impulse → all-ones */
    a[0] = 1; for (uint64_t i = 1; i < n; i++) a[i] = 0;
    ntt_forward(a, tw, p);
    int imp_ok = 1;
    for (uint64_t i = 0; i < n; i++) if (a[i] != 1) { imp_ok = 0; break; }
    printf("  %-36s %s\n", "impulse [1,0,...,0] → all-ones",
           imp_ok ? ANSI_GRN "PASS" ANSI_RST : ANSI_RED "FAIL" ANSI_RST);

    free(tw); free(twi); free(a); free(ref);

    int ok = rt_ok && cmp_ok && imp_ok;
    printf("  %s  n=%-4lu q=%lu\n\n",
           ok ? ANSI_GRN "ALL PASS" ANSI_RST : ANSI_RED "FAIL" ANSI_RST, n, q);
    return ok ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BENCHMARK
 * ═══════════════════════════════════════════════════════════════════════════ */

static void run_benchmark(const ntt_params_t *p, uint64_t iters)
{
    uint64_t n = p->n, q = p->q;
    uint64_t *tw = alloc_twiddles(p, p->omega);
    uint64_t *a  = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!tw || !a) { free(tw); free(a); return; }
    for (uint64_t i = 0; i < n; i++) a[i] = (i * 31337 + 1) % q;

    /* Warm up */
    ntt_forward(a, tw, p);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint64_t it = 0; it < iters; it++) ntt_forward(a, tw, p);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec)*1e-9;
    double ntts_s  = iters / elapsed;
    double ns_bfly = elapsed * 1e9 / ((double)iters * n * p->log2_n / 2.0);

    printf(ANSI_CYN "  ── Benchmark: ntt_forward (GPU Stockham) ───────────────\n" ANSI_RST);
    printf("  ┌──────────────────────────┬─────────────────────────┐\n");
    printf("  │ %-24s │ %-23lu │\n", "n",           n);
    printf("  │ %-24s │ %-23lu │\n", "q",           q);
    printf("  │ %-24s │ %-23lu │\n", "iterations",  iters);
    printf("  │ %-24s │ %-23.6f │\n", "elapsed (s)", elapsed);
    printf("  │ %-24s │ %-23.0f │\n", "NTT/s",       ntts_s);
    printf("  │ %-24s │ %-23.2f │\n", "ns/butterfly",ns_bfly);
    printf("  └──────────────────────────┴─────────────────────────┘\n\n");

    free(tw); free(a);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    ntt_params_t p;
    p.n     = (argc > 1) ? (uint64_t)atol(argv[1]) : 256;
    p.q     = (argc > 2) ? (uint64_t)atol(argv[2]) : 3329;
    p.omega = (argc > 3) ? (uint64_t)atol(argv[3]) : 17;
    uint64_t iters = (argc > 4) ? (uint64_t)atol(argv[4]) : 1000;

    printf("\n" ANSI_WHT
           "╔══════════════════════════════════════════════════════════╗\n"
           "║       NTT / MI300A  —  GPU Stockham auto-sort           ║\n"
           "╚══════════════════════════════════════════════════════════╝\n"
           ANSI_RST "\n");

    hipDeviceProp_t prop;
    HIP_CHECK(hipGetDeviceProperties(&prop, 0));
    strncpy(g_gpu.name, prop.name, sizeof g_gpu.name - 1);
    g_gpu.warp_size   = prop.warpSize;
    g_gpu.cu_count    = prop.multiProcessorCount;
    g_gpu.max_shared_b = (int)prop.sharedMemPerBlock;
    g_gpu.total_mem   = prop.totalGlobalMem;
    g_gpu.block_size  = 128;   /* safe for CDNA3 (wavefront=64) and RDNA3 (wavefront=32) */
    print_gpu_summary();

    if (ntt_params_init(&p) != 0) {
        fprintf(stderr, "error: n=%lu is not a power of 2\n", p.n);
        return 1;
    }

    ntt_params_t mlkem = { 256, 3329, 17, 0, 0, 0 };
    ntt_params_t mldsa = { 256, 8380417, 1753, 0, 0, 0 };
    int fail = 0;
    if (ntt_params_init(&mlkem) == 0) fail |= selftest(&mlkem);
    if (ntt_params_init(&mldsa) == 0) fail |= selftest(&mldsa);
    if (p.n != 256 || (p.q != 3329 && p.q != 8380417)) fail |= selftest(&p);

    if (fail) return 1;

    if (iters > 1) run_benchmark(&p, iters);
    return 0;
}
