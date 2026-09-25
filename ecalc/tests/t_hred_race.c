/* t_hred_race.c - Phase 14 A1 (results/A114.md): the first-use race of dbig.c's old `maxidx` host buffer, measured.
 *
 * The old code: four OpenMP threads (one per APU) each ran `if (!g_hred) g_hred = malloc(...)` unguarded, then copied their
 * quarter's per-block results with hipMemcpy to `g_hred + d * K` and scanned `g_hred[d * K + i]` -- the global read twice.
 * When two threads both see the null pointer, both allocate; a thread whose copy landed in one buffer can scan the other,
 * never-written one, and return heap garbage as a length (job 21222: `leaf P 18385101070989787659 limbs`).
 * This test replays exactly that pattern <iters> times (default 20000), resetting the pointer before each round, and counts
 * (a) rounds where more than one thread allocated, (b) rounds where a thread's scan did not see its own copy (garbage), for
 * the old pattern and for the fix (a per-thread buffer).  VERIFY: the fixed pattern never sees garbage.  The old pattern's
 * counts are the measurement (they depend on scheduling: run it on a loaded node to see the rate rise).
 *   tests/t_hred_race [iters]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <omp.h>
#include <hip/hip_runtime.h>
#include "harness.h"

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NQ 4
#define K (228 * 8)

static size_t *g_hred; static int g_nalloc;
static size_t *g_red[NQ];

__global__ void k_fill(size_t *p, size_t n, size_t v) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; if (i < n) p[i] = v; }

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 20000; int nd = 0; HIP_CHECK(hipGetDeviceCount(&nd)); if (nd > NQ) nd = NQ;
    if (nd < 2) { printf("t_hred_race: %d device(s): the race needs at least two threads\n", nd); verify_done("t_hred_race"); return 0; }
    for (int d = 0; d < nd; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&g_red[d], K * 8)); k_fill<<<(K + 255) / 256, 256>>>(g_red[d], K, (size_t)(d + 1) * 1000003u); HIP_CHECK(hipDeviceSynchronize()); }
    unsigned blocks = 64;
    long old_multi = 0, old_bad = 0, fix_bad = 0; size_t sample = 0;
    /* a few freed heap buffers with a recognisable content, so a never-written allocation is not just zeros */
    for (int i = 0; i < 64; i++) { size_t *junk = (size_t *)malloc(K * 8 * NQ); for (int j = 0; j < K * NQ; j++) junk[j] = 0xff25000400000000ull + j; free(junk); }
    for (long it = 0; it < iters; it++) {
        /* the old pattern */
        free(g_hred); g_hred = 0; g_nalloc = 0; int bad = 0;
#pragma omp parallel num_threads(nd) reduction(+:bad)
        {
            int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d));
            if (!g_hred) { size_t *p = (size_t *)malloc(K * 8 * NQ); __sync_fetch_and_add(&g_nalloc, 1); g_hred = p; }   /* unguarded, as dbig.c's flags_reserve was */
            HIP_CHECK(hipMemcpy(g_hred + d * K, g_red[d], blocks * 8, hipMemcpyDeviceToHost));
            size_t m = 0; for (unsigned i = 0; i < blocks; i++) if (g_hred[d * K + i] > m) m = g_hred[d * K + i];
            if (m != (size_t)(d + 1) * 1000003u) { bad++; if (!sample) sample = m; }
        }
        if (g_nalloc > 1) old_multi++; old_bad += bad;
        /* the fix: a buffer of the thread's own */
        bad = 0;
#pragma omp parallel num_threads(nd) reduction(+:bad)
        {
            int d = omp_get_thread_num(); HIP_CHECK(hipSetDevice(d)); size_t hred[K];
            HIP_CHECK(hipMemcpy(hred, g_red[d], blocks * 8, hipMemcpyDeviceToHost));
            size_t m = 0; for (unsigned i = 0; i < blocks; i++) if (hred[i] > m) m = hred[i];
            if (m != (size_t)(d + 1) * 1000003u) bad++;
        }
        fix_bad += bad;
    }
    printf("t_hred_race: %ld rounds x %d threads: old pattern: %ld rounds with more than one allocation, %ld garbage reads (first garbage 0x%zx); fixed pattern: %ld garbage reads\n",
           iters, nd, old_multi, old_bad, sample, fix_bad);
    VERIFY(fix_bad == 0, "the per-thread buffer saw garbage");
    verify_done("t_hred_race");
    return hv_fails != 0;
}
