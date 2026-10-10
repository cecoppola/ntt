/* t_gather_probe - S40 micro-test (results/S40.md); no product code is touched, the kernels are copies of k_gather (rns_dist.c).
 * (a) times the k_gather read pattern at the real size (rows x C = 2^29 points per rank, R = 4 ranks, source of 4 quarters of 2^29 limbs):
 *     SRC remote : the four quarters live on the four APUs, the gather runs on APU 0 (3/4 of the reads cross xGMI) -- the product's case
 *     SRC local  : the same source, all four quarters in one HBM allocation of APU 0 (same code, same index arithmetic)
 *     SRC local, no canon / no write-stride are not separate: the variants below vary only the source placement and the quarter pick
 * (b) quarter pick: PICK runtime = `a.q[d]` with a by-value struct (the product), PICK select = four scalar kernel arguments and a
 *     ternary chain.  Scratch per thread is printed by the compiler: build with
 *       hipcc -O3 --offload-arch=gfx942 -x hip tests/t_gather_probe.c -I. -o tests/t_gather_probe -Rpass-analysis=kernel-resource-usage
 *     (ScratchSize in bytes per lane for each kernel; 0 = no scratch)
 * Usage: t_gather_probe [rows (8192)] [C (65536)] [reps (5)]       needs 4 APUs, ~48 GB on APU 0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <hip/hip_runtime.h>
#include "modarith.h"
#define CK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { printf("HIP error %s at %d\n", hipGetErrorString(e_), __LINE__); exit(2); } } while (0)
struct acc { const uint64_t *q[4]; uint64_t *w[4]; size_t qc, lo, n; int flat; void *owner; };
__device__ static inline uint64_t acc_get(const struct acc a, size_t i)
{
    size_t m = a.lo + i;
    if (a.flat) return a.q[0][m];
    size_t d = (m >= a.qc) + (m >= 2 * a.qc) + (m >= 3 * a.qc); return a.q[d][m - d * a.qc];
}
__global__ void k_gather_rt(uint64_t *x, struct acc src, size_t R, size_t rows, size_t row0, size_t C, ec_mod m)      /* as in the product */
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t j = t / rows, il = t % rows, mm = R * j + row0 + il;
        x[il * C + j] = mm < src.n ? ec_canon64(acc_get(src, mm), m.pu, m.mu) : 0;
    }
}
__global__ void k_gather_sel(uint64_t *x, const uint64_t *q0, const uint64_t *q1, const uint64_t *q2, const uint64_t *q3, size_t qc, size_t lo, size_t n,
                             size_t R, size_t rows, size_t row0, size_t C, ec_mod m)      /* the quarter pick as a 4-way select of scalar arguments */
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t j = t / rows, il = t % rows, mm = R * j + row0 + il;
        uint64_t v = 0;
        if (mm < n) {
            size_t g = lo + mm; const uint64_t *p; size_t b;
            if (g >= 3 * qc) { p = q3; b = 3 * qc; } else if (g >= 2 * qc) { p = q2; b = 2 * qc; } else if (g >= qc) { p = q1; b = qc; } else { p = q0; b = 0; }
            v = ec_canon64(p[g - b], m.pu, m.mu);
        }
        x[il * C + j] = v;
    }
}
__global__ void k_fillsrc(uint64_t *p, size_t n, uint64_t seed) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; i < n; i += st) { uint64_t z = (i + seed) * 0x9E3779B97F4A7C15ULL; z ^= z >> 29; z *= 0xBF58476D1CE4E5B9ULL; z ^= z >> 32; p[i] = z; } }
__global__ void k_diff(const uint64_t *a, const uint64_t *b, size_t n, unsigned long long *cnt) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; unsigned long long c = 0; for (; i < n; i += st) c += a[i] != b[i]; if (c) atomicAdd(cnt, c); }
__global__ void k_sum(const uint64_t *p, size_t n, unsigned long long *out) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; unsigned long long c = 0; for (; i < n; i += st) c += p[i]; if (c == 1) out[0] = c; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
int main(int argc, char **argv)
{
    size_t rows = argc > 1 ? strtoull(argv[1], 0, 0) : 8192, C = argc > 2 ? strtoull(argv[2], 0, 0) : 65536; int reps = argc > 3 ? atoi(argv[3]) : 5;
    size_t R = 4 * rows, total = rows * C, qc = total, N = R * C;      /* 4 ranks x rows: rank r gathers limbs R j + r rows + il; the source is the 4 quarters of a sharded operand */
    int nd = 0; CK(hipGetDeviceCount(&nd)); if (nd < 4) { printf("needs 4 APUs\n"); return 2; }
    ec_mod m; memset(&m, 0, sizeof m);      /* a ~2^50 modulus: the canon runs the same instructions as with an ntt prime */
    { uint64_t p = 1125899906842597ULL; m.pu = p; m.p = (double)p; m.pinv = 1.0 / m.p; m.mu = (uint64_t)((((unsigned __int128)1) << 115) / p); }
    uint64_t *rq[4]; uint64_t *lbuf; uint64_t *x0, *x1; unsigned long long *cnt;
    for (int d = 0; d < 4; d++) { CK(hipSetDevice(d)); CK(hipMalloc(&rq[d], qc * 8)); k_fillsrc<<<4096, 256>>>(rq[d], qc, (uint64_t)d * qc); CK(hipDeviceSynchronize()); }
    for (int d = 1; d < 4; d++) { CK(hipSetDevice(0)); CK(hipDeviceEnablePeerAccess(d, 0)); }
    CK(hipSetDevice(0)); CK(hipMalloc(&lbuf, N * 8)); CK(hipMalloc(&x0, total * 8)); CK(hipMalloc(&x1, total * 8)); CK(hipMalloc(&cnt, 8));
    for (int d = 0; d < 4; d++) CK(hipMemcpyPeer(lbuf + (size_t)d * qc, 0, rq[d], d, qc * 8));
    printf("rows %zu C %zu: %zu points, source %zu limbs (%.2f GB), canon prime %llu\n", rows, C, total, N, N * 8e-9, (unsigned long long)m.pu);
    struct acc ar, al; memset(&ar, 0, sizeof ar); memset(&al, 0, sizeof al);
    for (int d = 0; d < 4; d++) { ar.q[d] = rq[d]; al.q[d] = lbuf + (size_t)d * qc; }
    ar.qc = al.qc = qc; ar.lo = al.lo = 0; ar.n = al.n = N;
    const uint64_t *lq[4]; for (int d = 0; d < 4; d++) lq[d] = lbuf + (size_t)d * qc;
    unsigned nb = 228 * 16;
    struct { const char *name; int sel, local; } V[4] = { {"remote src, runtime quarter (product)", 0, 0}, {"local src,  runtime quarter", 0, 1}, {"remote src, 4-way select", 1, 0}, {"local src,  4-way select", 1, 1} };
    double tv[4];
    for (int v = 0; v < 4; v++) {
        uint64_t *xo = (v == 0) ? x0 : x1;
        for (int r = -1; r < reps; r++) {      /* r = -1: warm-up */
            CK(hipDeviceSynchronize()); double t0 = now();
            if (!V[v].sel) k_gather_rt<<<nb, 256>>>(xo, V[v].local ? al : ar, R, rows, rows, C, m);
            else if (V[v].local) k_gather_sel<<<nb, 256>>>(xo, lq[0], lq[1], lq[2], lq[3], qc, 0, N, R, rows, rows, C, m);
            else k_gather_sel<<<nb, 256>>>(xo, rq[0], rq[1], rq[2], rq[3], qc, 0, N, R, rows, rows, C, m);
            CK(hipDeviceSynchronize()); double dt = now() - t0;
            if (r == 0 || dt < tv[v]) tv[v] = dt;      /* best of reps */
        }
        printf("  %-40s  best %.2f ms  read %.1f GB/s (4.29 GB), read+write %.1f GB/s\n", V[v].name, tv[v] * 1e3, total * 8e-9 / tv[v], 2 * total * 8e-9 / tv[v]);
        if (v >= 1) {   /* every variant must write the same plane as variant 0 (variants 1..3 share x1: compare after each) */
            CK(hipMemset(cnt, 0, 8)); k_diff<<<nb, 256>>>(x0, x1, total, cnt); unsigned long long h; CK(hipMemcpy(&h, cnt, 8, hipMemcpyDeviceToHost));
            printf("    mismatches vs variant 0: %llu\n", h); if (h) { printf("FAILED\n"); return 1; }
        }
    }
    {   /* sanity (c): is the remote source really remote?  a coalesced read of one 4.29 GB quarter from APU 0: APU 1's HBM vs APU 0's own */
        const uint64_t *src[2] = { rq[1], lq[1] }; const char *nm[2] = { "quarter on APU 1 (remote)", "quarter in APU 0 (local)" };
        for (int k = 0; k < 2; k++) { double best = 1e9; for (int r = -1; r < reps; r++) { CK(hipDeviceSynchronize()); double t0 = now(); k_sum<<<nb, 256>>>(src[k], qc, cnt); CK(hipDeviceSynchronize()); double dt = now() - t0; if (r == 0 || dt < best) best = dt; }
            printf("  coalesced read, %-28s %.2f ms = %.1f GB/s\n", nm[k], best * 1e3, qc * 8e-9 / best); }
    }
    printf("remote / local = %.2f (runtime quarter), %.2f (select); select / runtime = %.2f (remote), %.2f (local)\n", tv[0] / tv[1], tv[2] / tv[3], tv[2] / tv[0], tv[3] / tv[1]);
    printf("t_gather_probe: OK\n");
    return 0;
}
