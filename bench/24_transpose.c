/* 24_transpose - Phase 16 N1 (TARGET_HW_REVIEW 3 item 5): can the plane transposes of the one-node multiply
 * (rns_dist.c k_transpose, the CRT's rows x C -> C x rows per prime plane, followed by a device copy back;
 * the untiled gathers k_gather / k_gather_mn) be made faster by Infinity-Cache-sized working sets?
 *
 * A plane of 2^logq 8-byte points, rows x C with rows = 2^logrows (the production shape at 2^31 points over
 * four APUs: logR 15 -> rows 2^13 per APU, C 2^16; at 2^30: rows 2^13, C 2^15).  Out of place, x -> y.
 *
 *   copy         hipMemcpyAsync D2D and a uint4 copy kernel: the streaming ceiling (nothing is reused)
 *   t32          the production k_transpose: 32 x 32 LDS tile, block (32, 8), tiles in row-band order
 *   t32+copy     t32 followed by the copy back (the production pair per plane)
 *   t32 st<W>    the same kernel, tiles ordered in supertiles whose source + destination is W MB
 *                (W = 16, 32, 64: the report's Infinity-Cache sweet spot), 1-D grid
 *   t64 [st<W>]  64 x 64 LDS tile (33 KB LDS, block (64, 8)), row-band and supertile order
 *   sread/swrite the untiled forms: one side coalesced, the other strided (k_gather_mn's pattern)
 *
 * GB/s = 16 B x points / t (one read, one write), median and best of 5 after a warm-up; one APU, then all
 * four concurrently.  Every transpose's y is compared bit-for-bit with t32's on the device.
 * Usage: 24_transpose [logq (29)] [logrows (13)] [reps (5)]
 */
#include "common_ntt.h"

#define NBLK_CAP (228 * 16)

__global__ void k_fill(uint64_t *x, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += st) x[i] = i * 0x9E3779B97F4A7C15ULL ^ (i >> 7);
}
__global__ void k_copy4(uint4 *y, const uint4 *x, size_t n4)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    for (; i < n4; i += st) y[i] = x[i];
}
__global__ void k_cmp(const uint64_t *a, const uint64_t *b, size_t n, unsigned long long *bad)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    unsigned long long nb = 0;
    for (; i < n; i += st) nb += a[i] != b[i];
    if (nb) atomicAdd(bad, nb);
}
/* tile (ti, tj) of linear block b: A = 0 -> row-band order (tj fastest, as a 2-D grid (C/T, rows/T) walks);
 * else supertiles of A x B tiles (A along rows, B along C), tiles row-major inside, supertiles row-major */
__device__ static void tile_of(unsigned b, unsigned TJ, unsigned A, unsigned B, unsigned *ti, unsigned *tj)
{
    if (!A) { *ti = b / TJ; *tj = b % TJ; return; }
    unsigned nstj = TJ / B, st = b / (A * B), w = b % (A * B);
    *ti = (st / nstj) * A + w / B;
    *tj = (st % nstj) * B + w % B;
}
/* rows x C -> C x rows, 32 x 32 LDS tile, block (32, 8): rns_dist.c k_transpose's body with the tile mapping */
__global__ void k_t32(const uint64_t *x, uint64_t *y, size_t rows, size_t C, unsigned TJ, unsigned A, unsigned B)
{
    __shared__ uint64_t tile[32][33];
    unsigned ti, tj; tile_of(blockIdx.x, TJ, A, B, &ti, &tj);
    size_t bi = (size_t)ti * 32, bj = (size_t)tj * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += 8) tile[ty + k][tx] = x[(bi + ty + k) * C + bj + tx];
    __syncthreads();
    for (int k = 0; k < 32; k += 8) y[(bj + ty + k) * rows + bi + tx] = tile[tx][ty + k];
}
__global__ __launch_bounds__(512) void k_t64(const uint64_t *x, uint64_t *y, size_t rows, size_t C, unsigned TJ, unsigned A, unsigned B)
{
    __shared__ uint64_t tile[64][65];
    unsigned ti, tj; tile_of(blockIdx.x, TJ, A, B, &ti, &tj);
    size_t bi = (size_t)ti * 64, bj = (size_t)tj * 64;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 64; k += 8) tile[ty + k][tx] = x[(bi + ty + k) * C + bj + tx];
    __syncthreads();
    for (int k = 0; k < 64; k += 8) y[(bj + ty + k) * rows + bi + tx] = tile[tx][ty + k];
}
/* untiled: coalesced read, strided write (k_gather_mn transpose = 1; k_gather) */
__global__ void k_swrite(const uint64_t *x, uint64_t *y, size_t rows, size_t C)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += st) { size_t i = t / C, j = t % C; y[j * rows + i] = x[t]; }
}
/* untiled: strided read, coalesced write */
__global__ void k_sread(const uint64_t *x, uint64_t *y, size_t rows, size_t C)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += st) { size_t j = t / rows, i = t % rows; y[t] = x[i * C + j]; }
}

enum { V_MEMCPY, V_COPY4, V_T32, V_T32C, V_T32_16, V_T32_32, V_T32_64, V_T64, V_T64_16, V_T64_32, V_T64_64, V_SWRITE, V_SREAD, NV };
static const char *vname[NV] = { "memcpy d2d", "copy kernel (uint4)", "t32 (production)", "t32 + copy back",
    "t32 st16MB", "t32 st32MB", "t32 st64MB", "t64", "t64 st16MB", "t64 st32MB", "t64 st64MB", "strided write (gather_mn)", "strided read" };

static unsigned nblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > NBLK_CAP ? NBLK_CAP : b); }
/* supertile A x B of T x T tiles for a working set of ws bytes (source + destination); 0 if the plane does not divide */
static int st_shape(size_t ws, int T, size_t rows, size_t C, unsigned *A, unsigned *B)
{
    size_t n = ws / ((size_t)T * T * 16), TI = rows / T, TJ = C / T, a = 1;
    while (a * a < n) a <<= 1;
    if (a * a > n) a >>= 1;
    size_t b = n / a;
    if (!a || !b || a * b != n || TI % a || TJ % b) return 0;
    *A = (unsigned)a; *B = (unsigned)b; return 1;
}
/* one launch of variant v on the current device; returns 0 if the variant does not apply to the shape */
static int launch(int v, const uint64_t *x, uint64_t *y, size_t rows, size_t C)
{
    size_t q = rows * C; unsigned A = 0, B = 0, TJ;
    switch (v) {
    case V_MEMCPY: HIP_CHECK(hipMemcpyAsync(y, x, q * 8, hipMemcpyDeviceToDevice, 0)); return 1;
    case V_COPY4: k_copy4<<<nblk(q / 2), 256>>>((uint4 *)y, (const uint4 *)x, q / 2); return 1;
    case V_T32: case V_T32C: case V_T32_16: case V_T32_32: case V_T32_64:
        if (rows % 32 || C % 32) return 0;
        TJ = (unsigned)(C / 32);
        if (v >= V_T32_16 && !st_shape((size_t)16 << (20 + v - V_T32_16), 32, rows, C, &A, &B)) return 0;
        k_t32<<<(unsigned)(q / 1024), dim3(32, 8)>>>(x, y, rows, C, TJ, A, B);
        if (v == V_T32C) HIP_CHECK(hipMemcpyAsync((void *)x, y, q * 8, hipMemcpyDeviceToDevice, 0));   /* the copy back, as production (x's content alternates) */
        return 1;
    case V_T64: case V_T64_16: case V_T64_32: case V_T64_64:
        if (rows % 64 || C % 64) return 0;
        TJ = (unsigned)(C / 64);
        if (v >= V_T64_16 && !st_shape((size_t)16 << (20 + v - V_T64_16), 64, rows, C, &A, &B)) return 0;
        k_t64<<<(unsigned)(q / 4096), dim3(64, 8)>>>(x, y, rows, C, TJ, A, B); return 1;
    case V_SWRITE: k_swrite<<<nblk(q), 256>>>(x, y, rows, C); return 1;
    case V_SREAD: k_sread<<<nblk(q), 256>>>(x, y, rows, C); return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int logq = argc > 1 ? atoi(argv[1]) : 29, logrows = argc > 2 ? atoi(argv[2]) : 13, reps = argc > 3 ? atoi(argv[3]) : 5;
    int nd = device_count(), v, d;
    size_t q = (size_t)1 << logq, rows = (size_t)1 << logrows, C = q / rows;
    uint64_t *X[MAXD], *Y[MAXD], *REF[MAXD]; unsigned long long *BAD[MAXD];
    double gbs[MAXD], best[MAXD], med1[NV], best1[NV], med4[NV], tot4[NV]; int ok[NV], applies[NV];
    if (reps > 32) reps = 32;
    printf("== 24_transpose : plane 2^%d points = %zu x %zu (rows x C), %.2f GB; IC-sized supertiles vs the production order ==\n",
           logq, rows, C, q * 8 / 1e9);
    meta("24_transpose");
    for (d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&X[d], q * 8)); HIP_CHECK(hipMalloc(&Y[d], q * 8)); HIP_CHECK(hipMalloc(&REF[d], q * 8)); HIP_CHECK(hipMalloc(&BAD[d], 8));
        k_fill<<<nblk(q), 256>>>(X[d], q);
        HIP_CHECK(hipMemset(REF[d], 0, q * 8));
        HIP_CHECK(hipDeviceSynchronize());
    }
    /* the reference transpose on every device (t32, production order) */
    for (d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); launch(V_T32, X[d], REF[d], rows, C); HIP_CHECK(hipDeviceSynchronize()); }
    for (int conc = 0; conc < 2; conc++) {
        int nuse = conc ? nd : 1;
        printf("\n-- %s --\n", conc ? "all APUs concurrently" : "one APU (APU0)");
        header(conc ? "variant (4 concurrent)" : "variant (1 APU)");
        for (v = 0; v < NV; v++) {
            double t[MAXD][32]; int app = 1;
            unsigned long long nbad = 0;
#pragma omp parallel num_threads(nuse) reduction(+ : nbad) reduction(& : app)
            {
                int dev = omp_get_thread_num(), rep; hipEvent_t e0, e1; float ms;
                HIP_CHECK(hipSetDevice(dev));
                timer_events(&e0, &e1);
                k_fill<<<nblk(q), 256>>>(X[dev], q);                            /* t32+copy overwrites x: restore it */
                HIP_CHECK(hipMemset(Y[dev], 0, q * 8)); HIP_CHECK(hipMemset(BAD[dev], 0, 8)); HIP_CHECK(hipDeviceSynchronize());
                app = launch(v, X[dev], Y[dev], rows, C);                       /* warm-up */
                HIP_CHECK(hipDeviceSynchronize());
                if (app && v != V_MEMCPY && v != V_COPY4) {
                    k_cmp<<<nblk(q), 256>>>(Y[dev], REF[dev], q, BAD[dev]);
                    unsigned long long b; HIP_CHECK(hipMemcpy(&b, BAD[dev], 8, hipMemcpyDeviceToHost)); nbad += b;
                }
                for (rep = 0; rep < reps && app; rep++) {
#pragma omp barrier
                    HIP_CHECK(hipEventRecord(e0, 0));
                    launch(v, X[dev], Y[dev], rows, C);
                    HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
                    HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); t[dev][rep] = ms * 1e-3;
                }
                if (app) {                                                      /* median and best of the reps */
                    for (int a = 0; a < reps; a++) for (int b = a + 1; b < reps; b++) if (t[dev][b] < t[dev][a]) { double s = t[dev][a]; t[dev][a] = t[dev][b]; t[dev][b] = s; }
                    double bytes = 16.0 * (double)q * (v == V_T32C ? 2.0 : 1.0);
                    gbs[dev] = bytes / t[dev][reps / 2] / 1e9; best[dev] = bytes / t[dev][0] / 1e9;
                }
            }
            applies[v] = app;
            if (!app) { printf("  %-26s  (not applicable to this shape)\n", vname[v]); continue; }
            ok[v] = nbad == 0;
            if (!conc) { med1[v] = gbs[0]; best1[v] = best[0]; }
            else { double s = 0; for (d = 0; d < nd; d++) s += gbs[d]; med4[v] = s / nd; tot4[v] = s; }
            { char nm[48]; snprintf(nm, sizeof nm, "%s%s", vname[v], conc ? " x4" : ""); report_sum(nm, "GB/s", gbs, nuse); }
            if (nbad) printf("  ** %s: %llu points differ from t32 **\n", vname[v], nbad);
        }
    }
    printf("\n%-28s %10s %10s %10s %10s %6s\n", "summary (GB/s, 16 B/point)", "1APU med", "1APU best", "4APU/APU", "4APU node", "ident");
    for (v = 0; v < NV; v++) if (applies[v])
        printf("%-28s %10.0f %10.0f %10.0f %10.0f %6s\n", vname[v], med1[v], best1[v], med4[v], tot4[v], v <= V_COPY4 ? "-" : ok[v] ? "yes" : "NO");
    printf("(t32 + copy back counts 32 B/point: the transpose and the copy; production transposes each prime plane this way before the CRT)\n");
    return 0;
}
