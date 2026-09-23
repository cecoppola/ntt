/* t_strategy - PLAN.md 29.2 E0 (Phase 13a, agent S): one dependent product C = A B of n = 2^logn points on one node
 * (four APUs, K4 xGMI) under the three distribution strategies, timed, with each strategy's per-APU plane bytes and its
 * exchanges, and the three results compared limb for limb.
 *
 *   A  product-per-APU: the whole product on APU 0 (operands and result there), the EC_NP primes one after another:
 *      EC_NP result planes + one B plane (the minimum for a device CRT: (EC_NP + 1) n points on one APU).  No exchange.
 *      (The library's batch-local tier is the same strategy with 2 EC_NP planes and L <= 2^30; this is its
 *      memory-minimal form, built here from ntt_load / ntt_fwd / ntt_inv_pw / k_crt_batch.)
 *   B  prime-per-APU: APU d transforms prime d of the whole product (planes X_d, Y_d of n points), reading the operands
 *      from wherever their quarters live (dbig: quarter j on APU j -- a pull "broadcast" over xGMI), then APU d forms
 *      quarter d of the result by the CRT over the four APUs' planes (peer reads -- the gather).  Two xGMI phases, no
 *      all-to-all.  Built here from the per-prime transforms (the library's mdev tier is this strategy through host
 *      staging; there is no device-resident form in the library -- that is PLAN 29.3 E4).
 *   C  four-step distributed: the library's rns_mul_dist_db (the dm phase's product): primes one after another, each
 *      transform split over the four APUs with one all-to-all per transform (3 per prime), planes 7 n/4 points per APU;
 *      above the 2^31 plane cap the grid of 2^31 pieces.
 *
 * Operands: na = nb = n/2 random limbs (top limb nonzero), device-resident as dbigs (quartered over the APUs, as in the
 * dm phase) for B and C, a flat copy on APU 0 for A.  Every strategy's result is compared with the first one computed
 * (all n limbs); TSTRAT_GMP=1 also checks it against GMP (binary limbs).
 *
 *   b  B with a 128-bit operand load (k_load4 here) instead of ntt_load's 64-bit one -- the broadcast's cost.
 *   V  (Phase 13b B) the library's B forms (rns_dist.c, RNS_STRATEGY): products of several shapes (2^k and 3 2^k
 *      transforms, unbalanced, short, operands as views at an offset) through rns_mul_dist_db under C, B, B4 and auto,
 *      each compared limb for limb with C's; then the n-point product (na = nb = n/2) timed under each form
 *      ("STRAT ... strat=L<form>").  After C (its rns_init).
 *   4  B4 (Phase 13b, agent B): B spread over all four APUs at P = 3.  The product mod x^n - 1 splits into the residues
 *      mod x^(n/2) - 1 (the lower half of the forward DIF's bit-reversed output) and mod x^(n/2) + 1 (the upper half):
 *      the n-point DIF's first stage, then two independent n/2-point DIFs.  APU p < 3 (prime p) transforms A whole (n
 *      points) and B's lower-half residue Ylo = y[j] + y[j + n/2] (n/2 points); APU 3 forms the three primes' upper-half
 *      residues Yhi_p = (y[j] - y[j + n/2]) w_n^j (reading B once) and transforms them (3 x n/2 points).  Then APU p:
 *      X_p[0, n/2) *= Ylo_p, X_p[n/2, n) *= Yhi_p (a peer read of APU 3), the n-point inverse, and the CRT as in B.
 *      Planes: 1.5 n points on every APU (12 n bytes, C's at P = 3); transforms 2.5 n on APUs 0-2 (B: 3 n), 1.5 n on 3.
 *
 * usage: t_strategy logn [reps=5] [strategies=ABC, any of ABCb4]
 * env:   LIMB_BASE (2 default, 10 = decimal limbs), TSTRAT_GMP=1, TSTRAT_BUDGET_GB (the per-APU plane budget reported
 *        against; default = the production plane pools at POOL_LOG 31: 2^31 limbs + rns_pool1_default_bytes(31)),
 *        DIST_STATS=1 (C's transform parts)
 * One size per process: C's plane pools are made by rns_init(min(logn, 31)) at exactly the production layout
 * (pool 0 = 2^pool_log limbs, pool 1 = rns_pool1_default_bytes), and only after A and B have freed their planes.
 * Output: one "STRAT" line per strategy (wall median/min/max over reps after one warm-up, peak plane bytes per APU,
 * exchanges), then "VERIFY OK". */
#include "harness.h"
#include <omp.h>
#include <string.h>
#include <hip/hip_runtime.h>
#include "../ntt.h"
#include "../rns_mul.h"
#include "../rns_int.h"
#include "../dbig.h"
#include "../mem.h"
#include "../modarith.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define ND 4
#define MAXREP 32
#define GiB 1073741824.0
/* the prime count: agent P3's runtime ec_np (ECALC_NP=3|4, branch p13-P3) when the library has it, else EC_NP -- weak
 * references, so this test builds on main (no ec_np: NP = EC_NP = 4) and on P3's branch alike */
extern "C" { int ec_np_init(void) __attribute__((weak)); int rns_dist_strategy_set(const char *name) __attribute__((weak)); }
static int NP = EC_NP;

static size_t dev_used(int d) { size_t f = 0, t = 0; HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemGetInfo(&f, &t)); return t - f; }
static size_t dev_free(int d) { size_t f = 0, t = 0; HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemGetInfo(&f, &t)); return f; }
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double median(double *v, int k) { double t[MAXREP]; memcpy(t, v, k * sizeof *v); qsort(t, k, sizeof *t, cmp_d); return k & 1 ? t[k / 2] : 0.5 * (t[k / 2 - 1] + t[k / 2]); }

/* a quartered or flat view of n result limbs in device memory: limb i in q[i / qc] at i % qc */
struct qv { uint64_t *q[ND]; size_t qc; };
static inline uint64_t *qv_ptr(const struct qv *v, size_t i) { size_t j = i / v->qc; return v->q[j] + (i - j * v->qc); }

/* the stripe spills of the segmented CRT (M segments of Lseg coefficients, S stripes each; stripe g = pi S + s spills
 * 4 limbs at pi Lseg + Lseg (s+1)/S) added into the result on the host (CPU access to device memory, a few limbs each) */
static void merge_spills(const struct qv *out, size_t n, const uint64_t *sp, int M, size_t Lseg, int S)
{
    for (int pi = 0; pi < M; pi++) for (int s = 0; s < S; s++) {
        size_t k = (size_t)pi * Lseg + Lseg * (size_t)(s + 1) / S; const uint64_t *w = sp + ((size_t)pi * S + s) * 4; uint64_t cy = 0;
        if (k >= n) { if (w[0] | w[1] | w[2] | w[3]) { fprintf(stderr, "t_strategy: nonzero spill beyond the product\n"); exit(1); } continue; }
        for (int t = 0; t < 4 && k < n; t++, k++) {
            uint64_t *o = qv_ptr(out, k);
            if (bi_decimal) { uint64_t sm = *o + w[t] + cy; cy = sm >= EC_1E18; *o = cy ? sm - EC_1E18 : sm; }
            else { uint64_t sm = *o + w[t], c1 = sm < *o; sm += cy; c1 += sm < cy; *o = sm; cy = c1; }
        }
        while (cy && k < n) { uint64_t *o = qv_ptr(out, k); if (bi_decimal) { uint64_t sm = *o + cy; cy = sm >= EC_1E18; *o = cy ? sm - EC_1E18 : sm; } else { uint64_t sm = *o + cy; cy = sm < cy; *o = sm; } k++; }
        if (cy) { fprintf(stderr, "t_strategy: carry out of the product\n"); exit(1); }
    }
}
/* load a dbig operand (its limbs, from wherever its quarters are) into a plane of n points, canonical mod the context's prime */
static void load_db(ntt_ctx *c, uint64_t *dst, const dbig *x, size_t n, hipStream_t s)
{
    size_t done = 0;
    for (int j = 0; j < ND && done < x->n; j++) {
        size_t m = x->n - done < x->qc ? x->n - done : x->qc;
        ntt_load(c, dst + done, x->q[j], m, m, s); done += m;
    }
    if (done < n) HIP_CHECK(hipMemsetAsync(dst + done, 0, (n - done) * 8, s));
}
/* variant b: the same load with 128-bit accesses, 4 limbs per thread per step (ntt_load's k_load reads one 64-bit limb per
 * thread -- 51 GB/s per APU from peer quarters at 2^31 in E0) */
__global__ void k_load4(uint64_t *dst, const uint64_t *src, size_t n4, ec_mod m)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    const ulonglong2 *s2 = (const ulonglong2 *)src; ulonglong2 *d2 = (ulonglong2 *)dst;
    for (; i < n4; i += stride) {
        ulonglong2 a = s2[2 * i], b = s2[2 * i + 1];
        a.x = ec_canon64(a.x, m.pu, m.mu); a.y = ec_canon64(a.y, m.pu, m.mu); b.x = ec_canon64(b.x, m.pu, m.mu); b.y = ec_canon64(b.y, m.pu, m.mu);
        d2[2 * i] = a; d2[2 * i + 1] = b;
    }
}
static void load_db4(int prime, uint64_t *dst, const dbig *x, size_t n, hipStream_t s)
{
    size_t done = 0; ec_mod m = ec_mod_get(prime);
    for (int j = 0; j < ND && done < x->n; j++) {
        size_t cnt = x->n - done < x->qc ? x->n - done : x->qc, c4 = cnt / 4;
        if (c4) k_load4<<<228 * 16, 256, 0, s>>>(dst + done, x->q[j], c4, m);
        if (cnt > c4 * 4) { fprintf(stderr, "load_db4: a quarter of %zu limbs (not a multiple of 4)\n", cnt); exit(1); }   /* never: quarters are multiples of 4096 and na = n/2 */
        done += cnt;
    }
    if (done < n) HIP_CHECK(hipMemsetAsync(dst + done, 0, (n - done) * 8, s));
}
/* B4's residues of an operand (quartered dbig, ny limbs; limbs past ny are 0), h = n/2 points each:
 *   lo:  dst[j] = y[j] + y[j + h]                     (mod x^h - 1, one prime)
 *   hi:  dst_k[j] = (y[j] - y[j + h]) w_n^j            (mod x^h + 1 twisted to cyclic; nk primes at once, B read once)
 * w_n^j = t2_k[j & 0xffff] t1_k[j >> 16] (two-level tables of the prime's 2^logn-th root); outputs canonical */
struct yq { const uint64_t *q[ND]; size_t qc, ny; };
__device__ static inline uint64_t yq_get(const struct yq y, size_t i) { if (i >= y.ny) return 0; size_t d = i / y.qc; return y.q[d][i - d * y.qc]; }
__global__ void k_b4_lo(uint64_t *dst, struct yq y, size_t h, ec_mod m)
{
    size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; j < h; j += stride) dst[j] = ec_fold(ec_canon64(yq_get(y, j), m.pu, m.mu) + ec_canon64(yq_get(y, j + h), m.pu, m.mu), m.pu);
}
struct b4hi { uint64_t *dst[3]; const uint64_t *t1[3], *t2[3]; ec_mod m[3]; int nk; };
__global__ void k_b4_hi(struct b4hi a, struct yq y, size_t h)
{
    size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; j < h; j += stride) {
        uint64_t u = yq_get(y, j), v = yq_get(y, j + h);
        for (int k = 0; k < a.nk; k++) {
            ec_mod m = a.m[k]; uint64_t w = ec_mmu(a.t2[k][j & 0xffff], a.t1[k][j >> 16], m);
            a.dst[k][j] = ec_mmu(ec_canon64(u, m.pu, m.mu) + m.pu - ec_canon64(v, m.pu, m.mu), w, m);
        }
    }
}
/* the two-level table pair of w = the 2^logn-th root of prime p, entries j < h: t2 = w^(j & 0xffff), t1 = w^(j >> 16 << 16) */
static void b4_tables(int prime, int logn, size_t h, uint64_t **t1, uint64_t **t2)
{
    uint64_t P = ec_P[prime], w = ec_root(prime, logn), a = 1;
    size_t n2 = h < 65536 ? h : 65536, n1 = (h + 65535) / 65536;
    uint64_t *h2 = (uint64_t *)malloc(n2 * 8), *h1 = (uint64_t *)malloc(n1 * 8);
    for (size_t i = 0; i < n2; i++) { h2[i] = a; a = ec_mulmod_ref(a, w, P); }
    uint64_t w16 = ec_powmod(w, 65536, P); a = 1;
    for (size_t i = 0; i < n1; i++) { h1[i] = a; a = ec_mulmod_ref(a, w16, P); }
    HIP_CHECK(hipMalloc(t1, n1 * 8)); HIP_CHECK(hipMalloc(t2, n2 * 8));
    HIP_CHECK(hipMemcpy(*t1, h1, n1 * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(*t2, h2, n2 * 8, hipMemcpyHostToDevice));
    free(h1); free(h2);
}
/* compare n limbs of a device result with the host reference (chunks through a host buffer); returns the first difference or -1 */
static long long cmp_dev(const struct qv *v, size_t n, const uint64_t *ref)
{
    size_t ch = (size_t)1 << 24; uint64_t *h = (uint64_t *)malloc(ch * 8);
    for (size_t i = 0; i < n;) {
        size_t j = i / v->qc, o = i - j * v->qc, m = v->qc - o; if (m > ch) m = ch; if (m > n - i) m = n - i;
        HIP_CHECK(hipMemcpy(h, v->q[j] + o, m * 8, hipMemcpyDefault));
        if (memcmp(h, ref + i, m * 8)) { for (size_t t = 0; t < m; t++) if (h[t] != ref[i + t]) { free(h); return (long long)(i + t); } }
        i += m;
    }
    free(h); return -1;
}
static void fetch_dev(const struct qv *v, size_t n, uint64_t *dst)
{
    for (size_t i = 0; i < n;) {
        size_t j = i / v->qc, o = i - j * v->qc, m = v->qc - o; if (m > n - i) m = n - i;
        HIP_CHECK(hipMemcpy(dst + i, v->q[j] + o, m * 8, hipMemcpyDefault)); i += m;
    }
}

struct res { char name; int ran, fits; double t[MAXREP], tl, tn, tc, tm; size_t plane_b[ND], used_b[ND]; int a2a; double a2a_b, peer_b; const char *why; };
static void report(const struct res *r, int logn, int reps, size_t budget)
{
    if (!r->ran) { printf("STRAT logn=%d P=%d strat=%c skipped: %s\n", logn, NP, r->name, r->why ? r->why : "not requested"); return; }
    size_t pmax = 0, umax = 0; for (int d = 0; d < ND; d++) { if (r->plane_b[d] > pmax) pmax = r->plane_b[d]; if (r->used_b[d] > umax) umax = r->used_b[d]; }
    double mn = r->t[0], mx = r->t[0]; for (int i = 1; i < reps; i++) { if (r->t[i] < mn) mn = r->t[i]; if (r->t[i] > mx) mx = r->t[i]; }
    printf("STRAT logn=%d P=%d strat=%c wall_med=%.4f min=%.4f max=%.4f reps=%d | load %.4f ntt %.4f crt %.4f merge %.4f | plane_GiB/APU=%.2f [%.2f %.2f %.2f %.2f] dev_delta_GiB/APU=%.2f | a2a=%d a2a_GiB/APU=%.2f peer_GiB/APU=%.2f | fits_budget(%.1f GiB)=%s\n",
           logn, NP, r->name, median((double *)r->t, reps), mn, mx, reps, r->tl, r->tn, r->tc, r->tm,
           pmax / GiB, r->plane_b[0] / GiB, r->plane_b[1] / GiB, r->plane_b[2] / GiB, r->plane_b[3] / GiB, umax / GiB,
           r->a2a, r->a2a_b / GiB, r->peer_b / GiB, budget / GiB, pmax <= budget ? "yes" : "NO");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: t_strategy logn [reps] [ABC]\n"); return 2; }
    int logn = atoi(argv[1]), reps = argc > 2 ? atoi(argv[2]) : 5; const char *which = argc > 3 ? argv[3] : "ABC";
    if (reps < 1) reps = 1; if (reps > MAXREP) reps = MAXREP;
    if (logn < 20 || logn > 33) { fprintf(stderr, "logn 20..33\n"); return 2; }
    bi_env_base();
    if (ec_np_init) NP = ec_np_init();
    int nd = 0; HIP_CHECK(hipGetDeviceCount(&nd)); if (nd < ND) { fprintf(stderr, "need %d APUs\n", ND); return 1; }
    for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); for (int c = 0; c < ND; c++) if (c != d) { hipError_t e = hipDeviceEnablePeerAccess(c, 0); if (e != hipSuccess && e != hipErrorPeerAccessAlreadyEnabled) { fprintf(stderr, "peer %d->%d\n", d, c); return 1; } (void)hipGetLastError(); } }
    omp_set_max_active_levels(2);
    const size_t n = (size_t)1 << logn, na = n / 2, nb = n / 2;
    const size_t budget = getenv("TSTRAT_BUDGET_GB") ? (size_t)(atof(getenv("TSTRAT_BUDGET_GB")) * GiB) : ((size_t)8 << 31) + rns_pool1_default_bytes(31);
    const struct gconst G = rns_gconst();
    const int S = 912;                                           /* CRT stripes per segment (rns_gpucrt_blocks) */
    printf("t_strategy: n = 2^%d points, na = nb = %zu limbs, %s limbs, P = %d primes, %d reps after a warm-up, budget %.1f GiB/APU\n",
           logn, na, bi_decimal ? "decimal" : "binary", NP, reps, budget / GiB);

    /* the operands: random limbs on the host, then dbigs (quartered) */
    uint64_t *ha = (uint64_t *)malloc(na * 8), *hb = (uint64_t *)malloc(nb * 8);
    { const uint64_t B10 = EC_1E18;
#pragma omp parallel
      { rng_t r = { 0x5eed0000ULL + 7919ULL * omp_get_thread_num() }; int T = omp_get_num_threads(), t = omp_get_thread_num();
        for (size_t i = na * t / T; i < na * (t + 1) / T; i++) { uint64_t x = rng_next(&r), y = rng_next(&r); ha[i] = bi_decimal ? x % B10 : x; hb[i] = bi_decimal ? y % B10 : y; } }
      if (!ha[na - 1]) ha[na - 1] = 1; if (!hb[nb - 1]) hb[nb - 1] = 1; }
    dbig Ad, Bd; db_init(&Ad); db_init(&Bd);
    { bigint t; t.l = ha; t.n = t.cap = na; db_from_bi(&Ad, &t); t.l = hb; t.n = t.cap = nb; db_from_bi(&Bd, &t); }
    uint64_t *ref = 0; char refname = 0; int bad = 0;
    struct res R[5]; memset(R, 0, sizeof R); R[0].name = 'A'; R[1].name = 'B'; R[2].name = 'C'; R[3].name = 'b'; R[4].name = '4';
    double t0;

    /* ---- A: the whole product on APU 0 ---- */
    if (strchr(which, 'A')) {
        struct res *r = &R[0];
        size_t need = (size_t)(NP + 1) * n * 8 + (na + nb) * 8 + n * 8 + ((size_t)2 << 30);   /* planes + operands + result + 2 GiB slack */
        if (dev_free(0) < need) { r->why = "does not fit APU 0 (planes (P+1) n + operands + result)"; printf("A: needs %.1f GiB on APU 0, %.1f free\n", need / GiB, dev_free(0) / GiB); }
        else {
            HIP_CHECK(hipSetDevice(0));
            hipStream_t s; HIP_CHECK(hipStreamCreate(&s));
            ntt_ctx *ctx[EC_NP]; for (int p = 0; p < EC_NP; p++) ctx[p] = ntt_ctx_create(p);
            size_t u0 = dev_used(0);
            uint64_t *X, *Y, *a0, *b0, *out; HIP_CHECK(hipSetDevice(0));
            HIP_CHECK(hipMalloc(&X, (size_t)NP * n * 8)); HIP_CHECK(hipMalloc(&Y, n * 8));
            size_t u1 = dev_used(0); HIP_CHECK(hipSetDevice(0));
            HIP_CHECK(hipMalloc(&a0, na * 8)); HIP_CHECK(hipMalloc(&b0, nb * 8)); HIP_CHECK(hipMalloc(&out, n * 8));
            HIP_CHECK(hipMemcpy(a0, ha, na * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(b0, hb, nb * 8, hipMemcpyHostToDevice));
            int M = 4; size_t Lseg = n / M;                        /* the CRT in 4 segments (k_crt_batch counts coefficients in 32 bits) */
            struct bdesc hd[4], *dd; for (int i = 0; i < M; i++) { memset(&hd[i], 0, sizeof hd[i]); hd[i].c = out + i * Lseg; hd[i].na = (uint32_t)Lseg; }
            HIP_CHECK(hipMalloc(&dd, sizeof hd)); HIP_CHECK(hipMemcpy(dd, hd, sizeof hd, hipMemcpyHostToDevice));
            uint64_t *sp; HIP_CHECK(hipHostMalloc((void **)&sp, (size_t)M * S * 4 * 8, 0));
            struct qv ov = { { out, 0, 0, 0 }, n };
            for (int it = 0; it <= reps; it++) {
                HIP_CHECK(hipSetDevice(0)); HIP_CHECK(hipDeviceSynchronize());
                double a = now();
                for (int p = 0; p < NP; p++) {
                    uint64_t *xp = X + (size_t)p * n;
                    ntt_load(ctx[p], xp, a0, na, n, s); ntt_fwd(ctx[p], xp, logn, 1, s);
                    ntt_load(ctx[p], Y, b0, nb, n, s); ntt_fwd(ctx[p], Y, logn, 1, s);
                    ntt_inv_pw(ctx[p], xp, Y, logn, 1, s);
                }
                HIP_CHECK(hipStreamSynchronize(s)); double b = now();
                k_crt_batch<<<(unsigned)(M * S), CRT_THREADS, 0, s>>>(X, X + n, X + 2 * n, NP > 3 ? X + 3 * n : X, dd, 0, S, Lseg, G, sp, bi_decimal);
                HIP_CHECK(hipStreamSynchronize(s)); double c = now();
                merge_spills(&ov, n, sp, M, Lseg, S); double e = now();
                if (it) { r->t[it - 1] = e - a; r->tn += (b - a) / reps; r->tc += (c - b) / reps; r->tm += (e - c) / reps; }
            }
            r->ran = 1; r->plane_b[0] = (size_t)(NP + 1) * n * 8; r->used_b[0] = u1 - u0; r->a2a = 0; r->a2a_b = 0; r->peer_b = 0;
            if (!ref) { ref = (uint64_t *)malloc(n * 8); fetch_dev(&ov, n, ref); refname = 'A'; }
            else { long long k = cmp_dev(&ov, n, ref); VERIFY(k < 0, "A differs from %c at limb %lld", refname, k); }
            HIP_CHECK(hipSetDevice(0));
            HIP_CHECK(hipFree(X)); HIP_CHECK(hipFree(Y)); HIP_CHECK(hipFree(a0)); HIP_CHECK(hipFree(b0)); HIP_CHECK(hipFree(out)); HIP_CHECK(hipFree(dd)); HIP_CHECK(hipHostFree(sp));
            for (int p = 0; p < NP; p++) ntt_ctx_free(ctx[p]);
            HIP_CHECK(hipStreamDestroy(s));
        }
    } else R[0].why = "not requested";

    /* ---- B: prime d on APU d, the CRT of quarter d on APU d (b: the same with the 128-bit operand load) ---- */
    for (int var = 0; var < 2; var++) {
    struct res *r = &R[var ? 3 : 1];
    if (strchr(which, var ? 'b' : 'B')) {
        size_t need = 2 * n * 8 + n / 4 * 8 + ((size_t)2 << 30); int fit = 1;
        for (int d = 0; d < ND; d++) if (dev_free(d) < need) fit = 0;
        if (NP > ND) { fit = 0; r->why = "more primes than APUs"; }
        if (!fit) { if (!r->why) r->why = "does not fit (2 n-point planes per APU)"; printf("B: needs %.1f GiB per APU\n", need / GiB); }
        else {
            ntt_ctx *ctx[ND]; hipStream_t st[ND]; uint64_t *X[ND], *Y[ND]; struct bdesc *dd[ND]; size_t u0[ND], u1[ND];
            dbig Cb; db_init(&Cb); db_reserve(&Cb, n);
            if (Cb.qc * ND != n) { fprintf(stderr, "B: result quarters %zu != n/4\n", Cb.qc); return 1; }
            int M = ND; size_t Lseg = n / M;
            uint64_t *sp; HIP_CHECK(hipHostMalloc((void **)&sp, (size_t)M * S * 4 * 8, 0));
            for (int d = 0; d < ND; d++) {
                HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipStreamCreate(&st[d]));
                ctx[d] = d < NP ? ntt_ctx_create(d) : 0;
                u0[d] = dev_used(d); HIP_CHECK(hipSetDevice(d));
                if (d < NP) { HIP_CHECK(hipMalloc(&X[d], n * 8)); HIP_CHECK(hipMalloc(&Y[d], n * 8)); } else X[d] = Y[d] = 0;
                u1[d] = dev_used(d); HIP_CHECK(hipSetDevice(d));
                struct bdesc h[ND]; memset(h, 0, sizeof h); for (int i = 0; i < M; i++) { h[i].c = Cb.q[i]; h[i].na = (uint32_t)Lseg; }
                HIP_CHECK(hipMalloc(&dd[d], sizeof h)); HIP_CHECK(hipMemcpy(dd[d], h, sizeof h, hipMemcpyHostToDevice));
            }
            struct qv ov = { { Cb.q[0], Cb.q[1], Cb.q[2], Cb.q[3] }, Cb.qc };
            double tl[ND], tn[ND], tc[ND];
            for (int it = 0; it <= reps; it++) {
                for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipDeviceSynchronize()); }
                double a = now();
#pragma omp parallel num_threads(ND)
                {
                    int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); double x0 = now(), x1 = x0, x2 = x0;
                    if (d < NP) {
                        if (var) { load_db4(d, X[d], &Ad, n, st[d]); load_db4(d, Y[d], &Bd, n, st[d]); }
                        else { load_db(ctx[d], X[d], &Ad, n, st[d]); load_db(ctx[d], Y[d], &Bd, n, st[d]); }
                        HIP_CHECK(hipStreamSynchronize(st[d])); x1 = now();
                        ntt_fwd(ctx[d], X[d], logn, 1, st[d]); ntt_fwd(ctx[d], Y[d], logn, 1, st[d]); ntt_inv_pw(ctx[d], X[d], Y[d], logn, 1, st[d]);
                        HIP_CHECK(hipStreamSynchronize(st[d])); x2 = now();
                    }
#pragma omp barrier
                    double x3 = now();
                    k_crt_batch<<<(unsigned)S, CRT_THREADS, 0, st[d]>>>(X[0], X[1], X[2], X[3] ? X[3] : X[0], dd[d], (size_t)d * S, S, Lseg, G, sp, bi_decimal);
                    HIP_CHECK(hipStreamSynchronize(st[d]));
                    tl[d] = x1 - x0; tn[d] = x2 - x1; tc[d] = now() - x3;
                }
                double c = now();
                merge_spills(&ov, n, sp, M, Lseg, S); double e = now();
                if (it) { double ml = 0, mn = 0, mc = 0; for (int d = 0; d < ND; d++) { if (tl[d] > ml) ml = tl[d]; if (tn[d] > mn) mn = tn[d]; if (tc[d] > mc) mc = tc[d]; }
                          r->t[it - 1] = e - a; r->tl += ml / reps; r->tn += mn / reps; r->tc += mc / reps; r->tm += (e - c) / reps; }
            }
            r->ran = 1; r->a2a = 0;
            for (int d = 0; d < ND; d++) { r->plane_b[d] = d < NP ? 2 * n * 8 : 0; r->used_b[d] = u1[d] - u0[d]; }
            /* xGMI traffic per APU (from the access pattern): the operands' n limbs read whole, 3/4 remote; the CRT reads its
             * quarter of the NP planes, the NP - 1 (of NP) other APUs' remote */
            r->peer_b = (double)n * 8 * 3 / 4 + (double)(n / 4) * 8 * (NP < ND ? NP : NP - 1);   /* (P = 3: APU 3 reads all three planes remotely) */
            Cb.n = n;
            if (!ref) { ref = (uint64_t *)malloc(n * 8); fetch_dev(&ov, n, ref); refname = r->name; }
            else { long long k = cmp_dev(&ov, n, ref); VERIFY(k < 0, "%c differs from %c at limb %lld", r->name, refname, k); }
            for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); if (X[d]) { HIP_CHECK(hipFree(X[d])); HIP_CHECK(hipFree(Y[d])); } HIP_CHECK(hipFree(dd[d])); if (ctx[d]) ntt_ctx_free(ctx[d]); HIP_CHECK(hipStreamDestroy(st[d])); }
            HIP_CHECK(hipHostFree(sp)); db_free(&Cb);
        }
    } else r->why = "not requested";
    }

    /* ---- B4: B over all four APUs at P = 3 (prime p on APU p: A whole + B's lower residue; APU 3: B's three upper residues) ---- */
    if (strchr(which, '4')) {
        struct res *r = &R[4];
        size_t h = n / 2, need = 3 * h * 8 + n / 4 * 8 + ((size_t)2 << 30); int fit = 1;
        for (int d = 0; d < ND; d++) if (dev_free(d) < need) fit = 0;
        if (NP != 3) { fit = 0; r->why = "B4 is the P = 3 form (P = 4: B uses all four APUs)"; }
        if (!fit) { if (!r->why) r->why = "does not fit (1.5 n points per APU)"; }
        else {
            ntt_ctx *ctx[ND][3] = { { 0 } }; hipStream_t st[ND]; uint64_t *X[ND] = { 0 }, *Ylo[ND] = { 0 }, *Yhi[3], *t1[3], *t2[3]; struct bdesc *dd[ND]; size_t u0[ND], u1[ND];
            dbig Cb; db_init(&Cb); db_reserve(&Cb, n);
            if (Cb.qc * ND != n) { fprintf(stderr, "B4: result quarters %zu != n/4\n", Cb.qc); return 1; }
            int M = ND; size_t Lseg = n / M;
            uint64_t *sp; HIP_CHECK(hipHostMalloc((void **)&sp, (size_t)M * S * 4 * 8, 0));
            for (int d = 0; d < ND; d++) {
                HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipStreamCreate(&st[d]));
                if (d < 3) ctx[d][0] = ntt_ctx_create(d); else for (int k = 0; k < 3; k++) ctx[d][k] = ntt_ctx_create(k);
                if (d == 3) for (int k = 0; k < 3; k++) b4_tables(k, logn, h, &t1[k], &t2[k]);
                u0[d] = dev_used(d); HIP_CHECK(hipSetDevice(d));
                if (d < 3) { HIP_CHECK(hipMalloc(&X[d], n * 8)); HIP_CHECK(hipMalloc(&Ylo[d], h * 8)); }
                else for (int k = 0; k < 3; k++) HIP_CHECK(hipMalloc(&Yhi[k], h * 8));
                u1[d] = dev_used(d); HIP_CHECK(hipSetDevice(d));
                struct bdesc hb[ND]; memset(hb, 0, sizeof hb); for (int i = 0; i < M; i++) { hb[i].c = Cb.q[i]; hb[i].na = (uint32_t)Lseg; }
                HIP_CHECK(hipMalloc(&dd[d], sizeof hb)); HIP_CHECK(hipMemcpy(dd[d], hb, sizeof hb, hipMemcpyHostToDevice));
            }
            struct yq yB; for (int j = 0; j < ND; j++) yB.q[j] = Bd.q[j]; yB.qc = Bd.qc; yB.ny = Bd.n;
            struct b4hi hi; memset(&hi, 0, sizeof hi); hi.nk = 3; for (int k = 0; k < 3; k++) { hi.dst[k] = Yhi[k]; hi.t1[k] = t1[k]; hi.t2[k] = t2[k]; hi.m[k] = ec_mod_get(k); }
            struct qv ov = { { Cb.q[0], Cb.q[1], Cb.q[2], Cb.q[3] }, Cb.qc };
            double tl[ND], tn[ND], tc[ND];
            for (int it = 0; it <= reps; it++) {
                for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipDeviceSynchronize()); }
                double a = now();
#pragma omp parallel num_threads(ND)
                {
                    int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); double x0 = now(), x1, x2;
                    if (d < 3) { load_db(ctx[d][0], X[d], &Ad, n, st[d]); k_b4_lo<<<228 * 16, 256, 0, st[d]>>>(Ylo[d], yB, h, ec_mod_get(d)); }
                    else k_b4_hi<<<228 * 16, 256, 0, st[d]>>>(hi, yB, h);
                    HIP_CHECK(hipStreamSynchronize(st[d])); x1 = now();
                    if (d < 3) { ntt_fwd(ctx[d][0], X[d], logn, 1, st[d]); ntt_fwd(ctx[d][0], Ylo[d], logn - 1, 1, st[d]); ntt_pw(ctx[d][0], X[d], Ylo[d], h, st[d]); }
                    else for (int k = 0; k < 3; k++) ntt_fwd(ctx[d][k], Yhi[k], logn - 1, 1, st[d]);
                    HIP_CHECK(hipStreamSynchronize(st[d]));
#pragma omp barrier
                    if (d < 3) { ntt_pw(ctx[d][0], X[d] + h, Yhi[d], h, st[d]); ntt_inv(ctx[d][0], X[d], logn, 1, st[d]); HIP_CHECK(hipStreamSynchronize(st[d])); }
                    x2 = now();
#pragma omp barrier
                    double x3 = now();
                    k_crt_batch<<<(unsigned)S, CRT_THREADS, 0, st[d]>>>(X[0], X[1], X[2], X[0], dd[d], (size_t)d * S, S, Lseg, G, sp, bi_decimal);
                    HIP_CHECK(hipStreamSynchronize(st[d]));
                    tl[d] = x1 - x0; tn[d] = x2 - x1; tc[d] = now() - x3;
                }
                double c = now();
                merge_spills(&ov, n, sp, M, Lseg, S); double e = now();
                if (it) { double ml = 0, mn = 0, mc = 0; for (int d = 0; d < ND; d++) { if (tl[d] > ml) ml = tl[d]; if (tn[d] > mn) mn = tn[d]; if (tc[d] > mc) mc = tc[d]; }
                          r->t[it - 1] = e - a; r->tl += ml / reps; r->tn += mn / reps; r->tc += mc / reps; r->tm += (e - c) / reps; }
            }
            r->ran = 1; r->a2a = 0;
            for (int d = 0; d < ND; d++) { r->plane_b[d] = 3 * h * 8; r->used_b[d] = u1[d] - u0[d]; }
            /* xGMI per APU (access pattern): APU p reads A (n/2 limbs, 3/4 remote) and B (3/4 remote), then Yhi_p (h points) from APU 3;
             * the CRT reads its quarter of the three planes (APU 3: all remote) */
            r->peer_b = (double)n * 8 * 3 / 4 + (double)h * 8 + (double)(n / 4) * 8 * 3;
            Cb.n = n;
            if (!ref) { ref = (uint64_t *)malloc(n * 8); fetch_dev(&ov, n, ref); refname = r->name; }
            else { long long k = cmp_dev(&ov, n, ref); VERIFY(k < 0, "4 differs from %c at limb %lld", refname, k); }
            for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); if (X[d]) { HIP_CHECK(hipFree(X[d])); HIP_CHECK(hipFree(Ylo[d])); } HIP_CHECK(hipFree(dd[d])); for (int k = 0; k < 3; k++) if (ctx[d][k]) ntt_ctx_free(ctx[d][k]); HIP_CHECK(hipStreamDestroy(st[d])); }
            HIP_CHECK(hipSetDevice(3)); for (int k = 0; k < 3; k++) { HIP_CHECK(hipFree(Yhi[k])); HIP_CHECK(hipFree(t1[k])); HIP_CHECK(hipFree(t2[k])); }
            HIP_CHECK(hipHostFree(sp)); db_free(&Cb);
        }
    } else R[4].why = "not requested";

    /* ---- C: the library's distributed product (rns_mul_dist_db), plane pools at the production layout ---- */
    if (strchr(which, 'C')) {
        struct res *r = &R[2];
        int pl = logn < 31 ? logn : 31;
        size_t u0[ND]; for (int d = 0; d < ND; d++) u0[d] = dev_used(d);
        rns_staging_bytes_req = (size_t)2 << 20; rns_pool1_bytes_req = rns_pool1_default_bytes(pl);
        t0 = now(); rns_init(pl); printf("C: rns_init(%d) %.2f s: pools %.2f + %.2f GiB per APU\n", pl, now() - t0, rns_dpool_cap(0, 0) / GiB, rns_dpool_cap(0, 1) / GiB);
        dbig Cc; db_init(&Cc);
        for (int it = 0; it <= reps; it++) {
            memset(&rns_dist_st, 0, sizeof rns_dist_st);
            for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipDeviceSynchronize()); }
            double a = now();
            rns_mul_dist_db(&Cc, &Ad, &Bd);
            for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipDeviceSynchronize()); }
            double e = now();
            if (it) { r->t[it - 1] = e - a; r->tl += rns_dist_st.t_load / reps; r->tn += rns_dist_st.t_ntt / reps; r->tc += rns_dist_st.t_crt / reps; r->tm += rns_dist_st.t_merge / reps; }
        }
        r->ran = 1;
        size_t qpl = (n < ((size_t)1 << 31) ? n : ((size_t)1 << 31)) / 4;
        for (int d = 0; d < ND; d++) { r->plane_b[d] = (size_t)(NP + 3) * qpl * 8; r->used_b[d] = dev_used(d) - u0[d]; }   /* dist_core's planes: NP q in pool 0, 3 q of pool 1 */
        printf("C: plane pools as allocated %.2f + %.2f GiB per APU (dist_core uses %.2f GiB of them)\n", rns_dpool_cap(0, 0) / GiB, rns_dpool_cap(0, 1) / GiB, r->plane_b[0] / GiB);
        /* all-to-alls: 3 per prime per plane product (fwd A, fwd B, inverse), each rank sending 3/4 of its n/4 points;
         * above the 2^31 cap the grid's pieces (ka x kb products of 2^31 planes) */
        size_t cap = (size_t)1 << 31; int pieces = 1; size_t npl = n;
        if (n > cap) { /* the grid for na = nb = n/2 at the 2^31 cap: 2 x 2 pieces of 2^30 + 2^30 limbs (split_grid_cap's choice) */ pieces = 4; npl = cap; }
        r->a2a = 3 * NP * pieces; r->a2a_b = (double)r->a2a * (npl / 4) * 8 * 3 / 4;
        /* the gathers of the operands' rows (half of each plane nonzero, re-read per prime, 3/4 remote) and the run scatter of the result */
        r->peer_b = pieces * ((double)NP * (npl / 4) * 8 * 3 / 4 + (double)(npl / 4) * 8 * 3 / 4);
        struct qv ov = { { Cc.q[0], Cc.q[1], Cc.q[2], Cc.q[3] }, Cc.qc };
        if (!ref) { ref = (uint64_t *)malloc(n * 8); fetch_dev(&ov, n, ref); refname = 'C'; }
        else { long long k = cmp_dev(&ov, n, ref); VERIFY(k < 0, "C differs from %c at limb %lld", refname, k); }
        db_free(&Cc);
    } else R[2].why = "not requested";


    /* ---- V: the library's B forms against C (rns_mul_dist_db under RNS_STRATEGY = C, B, B4, auto) ---- */
    if (strchr(which, 'V') && rns_dist_strategy_set) {
        int pl = logn < 31 ? logn : 31;
        if (!rns_ndev()) { rns_staging_bytes_req = (size_t)2 << 20; rns_pool1_bytes_req = rns_pool1_default_bytes(pl); rns_init(pl); }
        const char *forms[4] = { "C", "B", "B4", "auto" };
        /* shapes (limbs of A, B, offsets of the views): n/2 x n/2; 3 2^k-sized; unbalanced; short (below 2^20); odd lengths at offsets */
        size_t sh[6][4] = { { na, nb, 0, 0 }, { (size_t)(0.37 * n), (size_t)(0.35 * n), 0, 0 }, { (size_t)(0.45 * n), n / 64 + 3, 0, 0 },
                            { 5000, 7001, 0, 0 }, { na - 12345, nb / 3 + 17, 999, 4097 }, { (size_t)(0.3 * n) + 1, (size_t)(0.2 * n) + 5, 3, 1 } };
        for (int k = 0; k < 6; k++) {
            size_t la = sh[k][0], lb = sh[k][1], oa = sh[k][2], ob = sh[k][3];
            if (oa + la > na) la = na - oa; if (ob + lb > nb) lb = nb - ob;
            dbig va = db_view(&Ad, oa, la), vb = db_view(&Bd, ob, lb); db_norm(&va); db_norm(&vb);
            dbig Cr; db_init(&Cr); uint64_t *h0 = 0; size_t n0 = 0;
            for (int f = 0; f < 4; f++) {
                rns_dist_strategy_set(forms[f]);
                db_free(&Cr); db_init(&Cr);
                double t = now(); rns_mul_dist_db(&Cr, &va, &vb); for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipDeviceSynchronize()); } t = now() - t;
                bigint hb_; bi_init(&hb_); db_to_bi(&hb_, &Cr);
                if (!f) { n0 = hb_.n; h0 = (uint64_t *)malloc((n0 + 1) * 8); memcpy(h0, hb_.l, n0 * 8); }
                else VERIFY(hb_.n == n0 && !memcmp(hb_.l, h0, n0 * 8), "V: shape %d (%zu x %zu at %zu, %zu): %s differs from C (%zu vs %zu limbs)", k, la, lb, oa, ob, forms[f], hb_.n, n0);
                printf("V shape %d: %zu x %zu limbs (views at %zu, %zu) under %-4s: %zu limbs, %.4f s\n", k, va.n, vb.n, oa, ob, forms[f], hb_.n, t);
                bi_free(&hb_);
            }
            free(h0); db_free(&Cr);
        }
        /* the n-point product timed under each form (the planes that do not fit the pools: the forms' extra buffer) */
        for (int f = 0; f < 4; f++) {
            struct res rr; memset(&rr, 0, sizeof rr); rr.name = 'L';
            rns_dist_strategy_set(forms[f]); dbig Cl; db_init(&Cl);
            size_t u0[ND]; for (int d = 0; d < ND; d++) u0[d] = dev_used(d);
            for (int it = 0; it <= reps; it++) {
                memset(&rns_dist_st, 0, sizeof rns_dist_st);
                for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipDeviceSynchronize()); }
                double a = now(); rns_mul_dist_db(&Cl, &Ad, &Bd);
                for (int d = 0; d < ND; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipDeviceSynchronize()); }
                if (it) { rr.t[it - 1] = now() - a; rr.tl += rns_dist_st.t_load / reps; rr.tn += rns_dist_st.t_ntt / reps; rr.tc += rns_dist_st.t_crt / reps; rr.tm += rns_dist_st.t_merge / reps; }
            }
            for (int d = 0; d < ND; d++) { rr.used_b[d] = dev_used(d) - u0[d]; }
            struct qv ov = { { Cl.q[0], Cl.q[1], Cl.q[2], Cl.q[3] }, Cl.qc };
            if (ref) { long long kx = cmp_dev(&ov, n, ref); VERIFY(kx < 0, "L%s differs from %c at limb %lld", forms[f], refname, kx); }
            double mn = rr.t[0], mx = rr.t[0]; for (int i = 1; i < reps; i++) { if (rr.t[i] < mn) mn = rr.t[i]; if (rr.t[i] > mx) mx = rr.t[i]; }
            size_t um = 0; for (int d = 0; d < ND; d++) if (rr.used_b[d] > um) um = rr.used_b[d];
            printf("STRAT logn=%d P=%d strat=L%s wall_med=%.4f min=%.4f max=%.4f reps=%d | load %.4f ntt %.4f crt %.4f merge %.4f | extra_dev_GiB/APU=%.2f (pools %.2f + %.2f)\n",
                   logn, NP, forms[f], median(rr.t, reps), mn, mx, reps, rr.tl, rr.tn, rr.tc, rr.tm, um / GiB, rns_dpool_cap(0, 0) / GiB, rns_dpool_cap(0, 1) / GiB);
            db_free(&Cl);
        }
        rns_dist_cache_release();
        rns_dist_strategy_set(0);
    }
    if (ref && getenv("TSTRAT_GMP") && atoi(getenv("TSTRAT_GMP")) && !bi_decimal) {
        mpz_t x, y, z, w; mpz_inits(x, y, z, w, NULL);
        mpz_import(x, na, -1, 8, 0, 0, ha); mpz_import(y, nb, -1, 8, 0, 0, hb); t0 = now(); mpz_mul(z, x, y);
        mpz_import(w, n, -1, 8, 0, 0, ref);
        VERIFY(mpz_cmp(z, w) == 0, "the product (%c) differs from GMP", refname);
        printf("GMP check %.1f s\n", now() - t0); mpz_clears(x, y, z, w, NULL);
    }
    (void)bad;
    for (int i = 0; i < 5; i++) if (i < 3 || R[i].ran) report(&R[i], logn, reps, budget);
    db_free(&Ad); db_free(&Bd); free(ha); free(hb); free(ref);
    return verify_done("t_strategy");
}
