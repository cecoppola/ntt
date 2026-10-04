/* t_edge - Phase 16 C (results/C16.md): the node's memory edge, measured the way the pools take it.  Allocates in steps of
 * <step> GB (default 8) up to <cap> GB (default 560) in one form, touches every page (a memset: the mapping is committed),
 * prints the cumulative GB and MemAvailable after each step, stops at the first failure (or the cap), then frees everything.
 * Forms: host (hipHostMalloc NumaUser round-robin over the four NUMA nodes -- mem.c's AF_HOST pool form), dev (hipMalloc
 * round-robin over the four APUs), malloc (plain malloc + touch: the kernel's overcommit edge).  Run alone on an exclusive
 * node: nothing else may be mapping memory at the same time (results/A16.md's rule).
 * Usage: t_edge [host|dev|malloc] [step GB] [cap GB] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hip/hip_runtime.h>
static double avail_gb(void) { FILE *f = fopen("/proc/meminfo", "r"); char k[64]; double v = 0, r = -1; while (f && fscanf(f, "%63s %lf kB", k, &v) == 2) if (!strcmp(k, "MemAvailable:")) { r = v * 1024 / 1e9; break; } if (f) fclose(f); return r; }
int main(int argc, char **argv)
{
    const char *form = argc > 1 ? argv[1] : "host"; double step = argc > 2 ? atof(argv[2]) : 8.0, cap = argc > 3 ? atof(argv[3]) : 560.0;
    size_t bytes = (size_t)(step * 1e9); int ndev = 1; (void)hipGetDeviceCount(&ndev); if (ndev < 1) ndev = 1;
    void *p[512]; int n = 0; double tot = 0;
    printf("t_edge: form %s, %.1f GB steps up to %.0f GB; %d devices; MemAvailable %.1f GB at the start\n", form, step, cap, ndev, avail_gb());
    while (tot + step <= cap + 1e-9 && n < 512) {
        int d = n % ndev; void *q = 0; hipError_t e = hipSuccess;
        if (!strcmp(form, "host")) { (void)hipSetDevice(d); e = hipHostMalloc(&q, bytes, hipHostMallocNumaUser); }
        else if (!strcmp(form, "dev")) { (void)hipSetDevice(d); e = hipMalloc(&q, bytes); }
        else { q = malloc(bytes); if (!q) e = hipErrorOutOfMemory; }
        if (e != hipSuccess || !q) { printf("t_edge: step %d (%.1f GB so far): %s failed (%s)\n", n + 1, tot, form, hipGetErrorString(e)); fflush(stdout); break; }
        if (!strcmp(form, "dev")) { if ((e = hipMemset(q, 0x5A, bytes)) != hipSuccess || (e = hipDeviceSynchronize()) != hipSuccess) { printf("t_edge: step %d: touch failed (%s)\n", n + 1, hipGetErrorString(e)); break; } }
        else memset(q, 0x5A, bytes);
        p[n++] = q; tot += step;
        printf("t_edge: %6.1f GB committed in %d steps, MemAvailable %.1f GB\n", tot, n, avail_gb()); fflush(stdout);
    }
    printf("t_edge: the edge in form %s: %.1f GB committed and touched (the cap %.0f GB)\n", form, tot, cap);
    for (int i = 0; i < n; i++) { if (!strcmp(form, "host")) (void)hipHostFree(p[i]); else if (!strcmp(form, "dev")) (void)hipFree(p[i]); else free(p[i]); }
    printf("t_edge: freed; MemAvailable %.1f GB\n", avail_gb());
    return 0;
}
