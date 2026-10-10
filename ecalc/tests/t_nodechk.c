/* t_nodechk - per-node health probe for target_kit.sh stage nodechk (2026-10-09, the aac7 x9000c1s0b1n0 finding, results/S31.md).
 * Prints ONE machine-readable line per node:  NODECHK host=<h> MemTotal_kB=.. MemFree_kB=.. MemAvailable_kB=.. Cached_kB=..
 * HugePages_Total=.. HugePages_Free=.. Hugepagesize_kB=.. apu0_s=.. apu1_s=.. ...   (apu<d>_s = median of <reps> timed 1 GB
 * host->device hipMemcpy uploads from a pre-touched pageable malloc buffer to device d; the aac7 stall was ~20 s in db_from_bi
 * against ~1 s on the other nodes, so a healthy node shows ~0.05-0.5 s here).  No file is written.
 * Usage: t_nodechk [size GB, default 1] [reps, default 3] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <hip/hip_runtime.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static long mi(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r"); char k[64]; long v = 0, r = -1;
    while (f && fscanf(f, "%63s %ld", k, &v) == 2) { if (!strcmp(k, key)) { r = v; break; } int c; while ((c = fgetc(f)) != '\n' && c != EOF) ; }
    if (f) fclose(f);
    return r;
}
int main(int argc, char **argv)
{
    double gb = argc > 1 ? atof(argv[1]) : 1.0; int reps = argc > 2 ? atoi(argv[2]) : 3; if (reps < 1) reps = 1; if (reps > 16) reps = 16;
    size_t bytes = (size_t)(gb * 1e9); char host[128] = "?"; gethostname(host, sizeof host);
    int ndev = 0; if (hipGetDeviceCount(&ndev) != hipSuccess || ndev < 1) { printf("NODECHK host=%s ERROR=no_hip_device\n", host); return 1; }
    if (ndev > 4) ndev = 4;
    char *h = (char *)malloc(bytes); if (!h) { printf("NODECHK host=%s ERROR=malloc\n", host); return 1; }
    memset(h, 0x5a, bytes);
    printf("NODECHK host=%s MemTotal_kB=%ld MemFree_kB=%ld MemAvailable_kB=%ld Cached_kB=%ld HugePages_Total=%ld HugePages_Free=%ld Hugepagesize_kB=%ld",
           host, mi("MemTotal:"), mi("MemFree:"), mi("MemAvailable:"), mi("Cached:"), mi("HugePages_Total:"), mi("HugePages_Free:"), mi("Hugepagesize:"));
    int bad = 0;
    for (int d = 0; d < ndev; d++) {
        void *p = 0; double t[16];
        if (hipSetDevice(d) != hipSuccess || hipMalloc(&p, bytes) != hipSuccess) { printf(" apu%d_s=ERR", d); bad = 1; continue; }
        for (int r = 0; r < reps; r++) { double t0 = now(); if (hipMemcpy(p, h, bytes, hipMemcpyHostToDevice) != hipSuccess) bad = 1; (void)hipDeviceSynchronize(); t[r] = now() - t0; }
        qsort(t, reps, sizeof t[0], cmpd); printf(" apu%d_s=%.3f", d, t[reps / 2]);
        (void)hipFree(p);
    }
    printf("\n"); free(h);
    return bad;
}
