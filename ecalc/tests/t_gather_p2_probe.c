/* t_gather_p2_probe - P2 micro-test (results/P2.md): what bounds k_gather_raw at the product's shape?
 * Same setup as t_gather_raw_probe (rows x C = 2^29 points, R = 4 rows, source = 4 quarters on 4 APUs, runs on APU 0).
 *   G0 raw gather strided write, remote src (k_gather_raw today)     G1 same, local src
 *   T<ti>x<tj>: tiled raw gather (LDS tile ti (il) x tj (j), coalesced read and write), remote src; L<..>: local src
 *   RD remote read floor: coalesced read of the 4 quarters' data the gather reads (3/4 remote), reduce, no big write
 *   CP: coalesced copy of the 3 remote quarters into local memory by a kernel (pull) and by hipMemcpyPeerAsync (DMA)
 * All gathers compared with G0 (0 mismatches required).  Usage: t_gather_p2_probe [rows 8192] [C 65536] [reps 5] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <hip/hip_runtime.h>
#define CK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { printf("HIP error %s at %d\n", hipGetErrorString(e_), __LINE__); exit(2); } } while (0)
struct Q { const uint64_t *q[4]; };
__device__ static inline uint64_t getq(const Q &a, size_t qc, size_t mm)
{ size_t d = (mm >= qc) + (mm >= 2 * qc) + (mm >= 3 * qc); const uint64_t *p = a.q[0]; if (d == 1) p = a.q[1]; if (d == 2) p = a.q[2]; if (d == 3) p = a.q[3]; return p[mm - d * qc]; }
__global__ void k_g0(uint64_t *x, Q src, size_t qc, size_t n, size_t R, size_t rows, size_t row0, size_t C)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t j = t / rows, il = t % rows, mm = R * j + row0 + il; x[il * C + j] = mm < n ? getq(src, qc, mm) : 0; }
}
template <int TI, int TJ>
__global__ void __launch_bounds__(256) k_gt(uint64_t *x, Q src, size_t qc, size_t n, size_t R, size_t rows, size_t row0, size_t C)
{
    __shared__ uint64_t tile[TI][TJ + 1];
    size_t nbi = rows / TI, bi = blockIdx.x % nbi, bj = blockIdx.x / nbi, i0 = bi * TI, j0 = bj * TJ; int id = threadIdx.x;
    for (int k = id; k < TI * TJ; k += 256) { int a = k % TI, b = k / TI; size_t mm = R * (j0 + b) + row0 + i0 + a; tile[a][b] = mm < n ? getq(src, qc, mm) : 0; }
    __syncthreads();
    for (int k = id; k < TI * TJ; k += 256) { int b = k % TJ, a = k / TJ; x[(i0 + a) * C + j0 + b] = tile[a][b]; }
}
__global__ void k_rd(Q src, size_t qc, size_t n, size_t R, size_t rows, size_t row0, size_t C, unsigned long long *sink)
{   /* the same set of limbs read, in sequence order t = j*rows+il (reads only; sum so it is not elided) */
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x; uint64_t s = 0;
    for (; t < total; t += stride) { size_t j = t / rows, il = t % rows, mm = R * j + row0 + il; if (mm < n) s += getq(src, qc, mm); }
    if (s == 0x123456789ULL) *sink = s;
}
__global__ void k_cp(uint64_t *y, const uint64_t *x, size_t n) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; i < n; i += st) y[i] = x[i]; }
__global__ void k_fill(uint64_t *p, size_t n, uint64_t seed) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; i < n; i += st) { uint64_t z = (i + seed) * 0x9E3779B97F4A7C15ULL; z ^= z >> 29; z *= 0xBF58476D1CE4E5B9ULL; z ^= z >> 32; p[i] = z; } }
__global__ void k_diff(const uint64_t *a, const uint64_t *b, size_t n, unsigned long long *cnt) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; unsigned long long c = 0; for (; i < n; i += st) c += a[i] != b[i]; if (c) atomicAdd(cnt, c); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
int main(int argc, char **argv)
{
    size_t rows = argc > 1 ? strtoull(argv[1], 0, 0) : 8192, C = argc > 2 ? strtoull(argv[2], 0, 0) : 65536; int reps = argc > 3 ? atoi(argv[3]) : 5;
    size_t R = 4 * rows, total = rows * C, qc = total, N = R * C;
    int nd = 0; CK(hipGetDeviceCount(&nd)); if (nd < 4) { printf("needs 4 APUs\n"); return 2; }
    uint64_t *rq[4], *lbuf, *x0, *x1; unsigned long long *cnt;
    for (int d = 0; d < 4; d++) { CK(hipSetDevice(d)); CK(hipMalloc(&rq[d], qc * 8)); k_fill<<<4096, 256>>>(rq[d], qc, (uint64_t)d * qc); CK(hipDeviceSynchronize()); }
    CK(hipSetDevice(0)); for (int d = 1; d < 4; d++) CK(hipDeviceEnablePeerAccess(d, 0));
    CK(hipMalloc(&lbuf, N * 8)); CK(hipMalloc(&x0, total * 8)); CK(hipMalloc(&x1, total * 8)); CK(hipMalloc(&cnt, 8));
    for (int d = 0; d < 4; d++) CK(hipMemcpyPeer(lbuf + (size_t)d * qc, 0, rq[d], d, qc * 8));
    Q rem, loc; for (int d = 0; d < 4; d++) { rem.q[d] = rq[d]; loc.q[d] = lbuf + (size_t)d * qc; }
    printf("rows %zu C %zu: %zu points (%.2f GB plane), source %zu limbs\n", rows, C, total, total * 8e-9, N);
    unsigned nb = 228 * 16;
    enum { NV = 14 };
    const char *nm[NV] = { "G0 strided, remote (today)", "G1 strided, local", "T32x32 remote", "T32x32 local", "T64x32 remote", "T64x32 local", "T32x64 remote", "T32x64 local",
                           "T16x128 remote", "T16x128 local", "T128x16 remote", "RD read-only remote", "CP kernel pull 3 quarters", "CP DMA 3 quarters" };
    double tv[NV]; int ok = 1;
    for (int v = 0; v < NV; v++) {
        uint64_t *out = v == 0 ? x0 : x1;
        for (int r = -1; r < reps; r++) {
            CK(hipDeviceSynchronize()); double t0 = now();
            switch (v) {
            case 0: k_g0<<<nb, 256>>>(x0, rem, qc, N, R, rows, rows, C); break;
            case 1: k_g0<<<nb, 256>>>(x1, loc, qc, N, R, rows, rows, C); break;
#define TILE(vv, TI, TJ, S) case vv: k_gt<TI, TJ><<<(unsigned)((rows / TI) * (C / TJ)), 256>>>(x1, S, qc, N, R, rows, rows, C); break;
            TILE(2, 32, 32, rem) TILE(3, 32, 32, loc) TILE(4, 64, 32, rem) TILE(5, 64, 32, loc) TILE(6, 32, 64, rem) TILE(7, 32, 64, loc) TILE(8, 16, 128, rem) TILE(9, 16, 128, loc) TILE(10, 128, 16, rem)
            case 11: k_rd<<<nb, 256>>>(rem, qc, N, R, rows, rows, C, cnt); break;
            case 12: for (int d = 1; d < 4; d++) k_cp<<<nb, 256>>>(lbuf + (size_t)d * qc, rq[d], qc); break;
            case 13: for (int d = 1; d < 4; d++) CK(hipMemcpyPeerAsync(lbuf + (size_t)d * qc, 0, rq[d], d, qc * 8, 0)); break;
            }
            CK(hipDeviceSynchronize()); double dt = now() - t0;
            if (r == 0 || (r > 0 && dt < tv[v])) tv[v] = dt;
        }
        unsigned long long h = 0;
        if (v >= 1 && v <= 10) { CK(hipMemset(cnt, 0, 8)); k_diff<<<nb, 256>>>(x0, out, total, cnt); CK(hipMemcpy(&h, cnt, 8, hipMemcpyDeviceToHost)); }
        printf("  %-30s best %7.2f ms (%.2fx G0)%s\n", nm[v], tv[v] * 1e3, tv[v] / tv[0], v >= 1 && v <= 10 ? (h ? "  MISMATCH" : "  identical") : "");
        if (h) ok = 0;
    }
    printf(ok ? "t_gather_p2_probe: OK\n" : "t_gather_p2_probe: FAILED\n");
    return ok ? 0 : 1;
}
