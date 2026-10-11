/* t_nodechk - per-node health probe for target_kit.sh stage nodechk (2026-10-09, the aac7 aac7-n01 finding, results/S31.md).
 * Prints ONE machine-readable line per node:  NODECHK host=<h> MemTotal_kB=.. MemFree_kB=.. MemAvailable_kB=.. Cached_kB=..
 * HugePages_Total=.. HugePages_Free=.. Hugepagesize_kB=.. apu0_s=.. apu1_s=.. ...   (apu<d>_s = median of <reps> timed 1 GB
 * host->device hipMemcpy uploads from a pre-touched pageable malloc buffer to device d; the aac7 stall was ~20 s in db_from_bi
 * against ~1 s on the other nodes, so a healthy node shows ~0.05-0.5 s here).  No file is written.
 * T10 (2026-10-11, results/T10.md): optional COLD-PATH mode (3rd arg cold GB per APU, default 0 = off, output unchanged).  It reproduces
 * ecalc's init on a fresh process: per APU hipMemAddressReserve of 3x, then 2 GiB chunks hipMemCreate + hipMemMap, one hipMemSetAccess
 * for all APUs, hipMemset of the mapped range (db_vmm_arena_alloc / vmm_map_run, dbig.c), then a first-touch of a fresh pageable host
 * buffer of that size and ONE cold upload of it into the VMM range (db_from_bi).  Prints one extra line per node:
 *   NODECHK_COLD host=<h> cold_gb=.. touch_s=.. apu<d>_total_s=.. apu<d>_create_s=.. _map_s=.. _access_s=.. _zero_s=.. _up_s=..
 * Usage: t_nodechk [size GB, default 1] [reps, default 3] [cold GB per APU, default 0] */
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
    double cgb = argc > 3 ? atof(argv[3]) : 0.0;
    if (cgb > 0) {
        const size_t C = (size_t)2 << 30; size_t want = (size_t)(cgb * 1e9), nch = (want + C - 1) / C, tot = nch * C;
        double t0 = now(); char *hb = (char *)malloc(tot); if (!hb) { printf("NODECHK_COLD host=%s ERROR=malloc\n", host); return 1; }
        memset(hb, 0x3c, tot); double tt = now() - t0;
        printf("NODECHK_COLD host=%s cold_gb=%.2f touch_s=%.3f", host, tot * 1e-9, tt);
        hipMemAccessDesc ad[4]; memset(ad, 0, sizeof ad);
        for (int c = 0; c < ndev; c++) { ad[c].location.type = hipMemLocationTypeDevice; ad[c].location.id = c; ad[c].flags = hipMemAccessFlagsProtReadWrite; }
        for (int d = 0; d < ndev; d++) {
            hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = d;
            size_t gran = 0; if (hipMemGetAllocationGranularity(&gran, &prop, hipMemAllocationGranularityRecommended) != hipSuccess || !gran) gran = (size_t)2 << 20;
            char *base = 0; hipMemGenericAllocationHandle_t *hs = (hipMemGenericAllocationHandle_t *)calloc(nch, sizeof *hs);
            double tc = 0, tm = 0, ta, tz, tu, T0; int ok = 1;
            hipSetDevice(d); T0 = now();
            if (hipMemAddressReserve((void **)&base, 3 * tot, gran, 0, 0) != hipSuccess) ok = 0;
            for (size_t i = 0; ok && i < nch; i++) {
                double a = now(); if (hipMemCreate(&hs[i], C, &prop, 0) != hipSuccess) { ok = 0; break; }
                double b = now(); if (hipMemMap(base + i * C, C, 0, hs[i], 0) != hipSuccess) { ok = 0; break; }
                tc += b - a; tm += now() - b;
            }
            double a = now(); if (ok && hipMemSetAccess(base, tot, ad, ndev) != hipSuccess) ok = 0; ta = now() - a;
            a = now(); if (ok && (hipMemset(base, 0, tot) != hipSuccess || hipDeviceSynchronize() != hipSuccess)) ok = 0; tz = now() - a;
            a = now(); if (ok && (hipMemcpy(base, hb, tot, hipMemcpyHostToDevice) != hipSuccess || hipDeviceSynchronize() != hipSuccess)) ok = 0; tu = now() - a;
            if (!ok) { printf(" apu%d_total_s=ERR", d); bad = 1; }
            else printf(" apu%d_total_s=%.3f apu%d_create_s=%.3f apu%d_map_s=%.3f apu%d_access_s=%.3f apu%d_zero_s=%.3f apu%d_up_s=%.3f",
                        d, now() - T0 + 0.0, d, tc, d, tm, d, ta, d, tz, d, tu);
            if (base) { (void)hipMemUnmap(base, tot); for (size_t i = 0; i < nch; i++) if (hs[i]) (void)hipMemRelease(hs[i]); (void)hipMemAddressFree(base, 3 * tot); }
            free(hs);
        }
        printf("\n"); free(hb);
    }
    return bad;
}
