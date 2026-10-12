/* t_gather_raw_probe - K1b micro-test (results/K1b.md) of S39 candidate A ("pull the operand once raw, canonicalise per prime").
 * No product code is touched; the kernels are copies of k_gather (rns_dist.c, the S41 select form) at the product's shape
 * (rows x C = 2^29 points per rank, R = 4 rows, source = 4 quarters of 2^29 limbs, one per APU; the gather runs on APU 0).
 *   V0 gather + canon (the product, one prime)          V1 gather raw (no canon)          V2 canon pass plane -> plane (local, coalesced)
 *   V3 canon pass in place                               V4 gather + canon, power-of-two index (shift / mask instead of / and %; information only)
 *   V5 gather raw from a local-only source (S40's local-source check repeated on this node)
 * Model printed at the end: per operand with np primes, now np x V0; raw-once V1 + (np-1) V2 + V3 (the S39 / K1b design).
 * Correctness: V2(V1) and V3(V1) and V4 must equal V0 bit for bit.
 * Usage: t_gather_raw_probe [rows (8192)] [C (65536)] [reps (5)]       needs 4 APUs, ~40 GB on APU 0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <hip/hip_runtime.h>
#include "modarith.h"
#define CK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { printf("HIP error %s at %d\n", hipGetErrorString(e_), __LINE__); exit(2); } } while (0)
template <int CANON, int POW2>
__global__ void k_g(uint64_t *x, const uint64_t *q0, const uint64_t *q1, const uint64_t *q2, const uint64_t *q3, size_t qc, size_t n,
                    size_t R, size_t rows, int lrows, size_t row0, size_t C, ec_mod m)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t j = POW2 ? t >> lrows : t / rows, il = POW2 ? t & (rows - 1) : t % rows, mm = R * j + row0 + il;
        uint64_t v = 0;
        if (mm < n) {
            size_t d = (mm >= qc) + (mm >= 2 * qc) + (mm >= 3 * qc); const uint64_t *p = q0; if (d == 1) p = q1; if (d == 2) p = q2; if (d == 3) p = q3;
            v = p[mm - d * qc]; if (CANON) v = ec_canon64(v, m.pu, m.mu);
        }
        x[il * C + j] = v;
    }
}
__global__ void k_canon(uint64_t *y, const uint64_t *x, size_t n, ec_mod m) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; i < n; i += st) y[i] = ec_canon64(x[i], m.pu, m.mu); }
__global__ void k_fillsrc(uint64_t *p, size_t n, uint64_t seed) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; i < n; i += st) { uint64_t z = (i + seed) * 0x9E3779B97F4A7C15ULL; z ^= z >> 29; z *= 0xBF58476D1CE4E5B9ULL; z ^= z >> 32; p[i] = z; } }
__global__ void k_diff(const uint64_t *a, const uint64_t *b, size_t n, unsigned long long *cnt) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; unsigned long long c = 0; for (; i < n; i += st) c += a[i] != b[i]; if (c) atomicAdd(cnt, c); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static unsigned long long diff(const uint64_t *a, const uint64_t *b, size_t n, unsigned long long *cnt)
{ CK(hipMemset(cnt, 0, 8)); k_diff<<<228 * 16, 256>>>(a, b, n, cnt); unsigned long long h; CK(hipMemcpy(&h, cnt, 8, hipMemcpyDeviceToHost)); return h; }
int main(int argc, char **argv)
{
    size_t rows = argc > 1 ? strtoull(argv[1], 0, 0) : 8192, C = argc > 2 ? strtoull(argv[2], 0, 0) : 65536; int reps = argc > 3 ? atoi(argv[3]) : 5;
    size_t R = 4 * rows, total = rows * C, qc = total, N = R * C; int lrows = 0; while (((size_t)1 << lrows) < rows) lrows++;
    if (((size_t)1 << lrows) != rows) { printf("rows must be a power of two\n"); return 2; }
    int nd = 0; CK(hipGetDeviceCount(&nd)); if (nd < 4) { printf("needs 4 APUs\n"); return 2; }
    ec_mod m; memset(&m, 0, sizeof m);
    { uint64_t p = 1125899906842597ULL; m.pu = p; m.p = (double)p; m.pinv = 1.0 / m.p; m.mu = (uint64_t)((((unsigned __int128)1) << 115) / p); }
    uint64_t *rq[4], *lbuf, *x0, *x1, *x2; unsigned long long *cnt;
    for (int d = 0; d < 4; d++) { CK(hipSetDevice(d)); CK(hipMalloc(&rq[d], qc * 8)); k_fillsrc<<<4096, 256>>>(rq[d], qc, (uint64_t)d * qc); CK(hipDeviceSynchronize()); }
    CK(hipSetDevice(0)); for (int d = 1; d < 4; d++) CK(hipDeviceEnablePeerAccess(d, 0));
    CK(hipMalloc(&lbuf, N * 8)); CK(hipMalloc(&x0, total * 8)); CK(hipMalloc(&x1, total * 8)); CK(hipMalloc(&x2, total * 8)); CK(hipMalloc(&cnt, 8));
    for (int d = 0; d < 4; d++) CK(hipMemcpyPeer(lbuf + (size_t)d * qc, 0, rq[d], d, qc * 8));
    const uint64_t *lq[4]; for (int d = 0; d < 4; d++) lq[d] = lbuf + (size_t)d * qc;
    printf("rows %zu C %zu: %zu points (%.2f GB plane), source %zu limbs\n", rows, C, total, total * 8e-9, N);
    unsigned nb = 228 * 16; double tv[6]; const char *nm[6] = { "V0 gather+canon (product)", "V1 gather raw", "V2 canon pass x->y", "V3 canon pass in place", "V4 gather+canon pow2 index", "V5 gather raw, local src" };
    for (int v = 0; v < 6; v++) {
        for (int r = -1; r < reps; r++) {
            if (v == 3) CK(hipMemcpy(x2, x1, total * 8, hipMemcpyDeviceToDevice));      /* in place needs the raw plane each time (not timed) */
            CK(hipDeviceSynchronize()); double t0 = now();
            switch (v) {
            case 0: k_g<1, 0><<<nb, 256>>>(x0, rq[0], rq[1], rq[2], rq[3], qc, N, R, rows, lrows, rows, C, m); break;
            case 1: k_g<0, 0><<<nb, 256>>>(x1, rq[0], rq[1], rq[2], rq[3], qc, N, R, rows, lrows, rows, C, m); break;
            case 2: k_canon<<<nb, 256>>>(x2, x1, total, m); break;
            case 3: k_canon<<<nb, 256>>>(x2, x2, total, m); break;
            case 4: k_g<1, 1><<<nb, 256>>>(x2, rq[0], rq[1], rq[2], rq[3], qc, N, R, rows, lrows, rows, C, m); break;
            case 5: k_g<0, 0><<<nb, 256>>>(x2, lq[0], lq[1], lq[2], lq[3], qc, N, R, rows, lrows, rows, C, m); break;
            }
            CK(hipDeviceSynchronize()); double dt = now() - t0;
            if (r == 0 || (r > 0 && dt < tv[v])) tv[v] = dt;
        }
        unsigned long long h = 0;
        if (v >= 2 && v <= 4) h = diff(x0, x2, total, cnt);
        if (v == 5) h = diff(x1, x2, total, cnt);
        printf("  %-30s best %7.2f ms%s\n", nm[v], tv[v] * 1e3, v >= 2 ? (h ? "  MISMATCH" : "  identical") : "");
        if (h) { printf("FAILED: %llu mismatches\n", h); return 1; }
    }
    for (int np = 3; np <= 4; np++) {
        double now_ = np * tv[0], raw = tv[1] + (np - 1) * tv[2] + tv[3];
        printf("model np=%d per operand: now %.2f ms, raw-once %.2f ms, ratio %.3f, saving per gather call %.2f ms\n", np, now_ * 1e3, raw * 1e3, raw / now_, (now_ - raw) / np * 1e3);
    }
    printf("RATIO4 %.4f RAWREL %.4f CANONREL %.4f POW2REL %.4f LOCALREL %.4f\n", (tv[1] + 3 * tv[2] + tv[3]) / (4 * tv[0]), tv[1] / tv[0], tv[2] / tv[0], tv[4] / tv[0], tv[5] / tv[1]);
    printf("t_gather_raw_probe: OK\n");
    return 0;
}
