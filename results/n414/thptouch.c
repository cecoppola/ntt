/* N414 A5: allocate <GB> of anonymous memory with MADV_HUGEPAGE, touch it with all threads, report the THP share and
 * the time, free it.  A user-level way to make the kernel compact (defrag) free memory into 2 MiB blocks.
 * cc -O2 -fopenmp thptouch.c -o thptouch;  ./thptouch <GB> */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static long rollup(const char *key)
{
    FILE *f = fopen("/proc/self/smaps_rollup", "r"); char l[256]; long v = -1;
    if (!f) return -1;
    while (fgets(l, sizeof l, f)) if (!strncmp(l, key, strlen(key))) { v = atol(l + strlen(key)); break; }
    fclose(f); return v;
}
int main(int argc, char **argv)
{
    size_t gb = argc > 1 ? atol(argv[1]) : 100, b = gb << 30;
    double t0 = now();
    char *p = mmap(0, b, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    madvise(p, b, MADV_HUGEPAGE);
    size_t chunks = b >> 21;
#pragma omp parallel for schedule(dynamic, 64)
    for (size_t i = 0; i < chunks; i++) memset(p + (i << 21), 1, 1 << 21);
    double t1 = now();
    long rss = rollup("Rss:"), thp = rollup("AnonHugePages:");
    printf("thptouch: %zu GB touched in %.2f s (%.1f GB/s); Rss %.1f GB, AnonHugePages %.1f GB (%.1f %%)\n", gb, t1 - t0, gb / (t1 - t0), rss / 1048576.0, thp / 1048576.0, rss > 0 ? 100.0 * thp / rss : 0);
    munmap(p, b);
    printf("thptouch: freed in %.2f s\n", now() - t1);
    return 0;
}
