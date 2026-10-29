/* t_edge - the node's memory edge, measured the way the pools take it.  Allocates in steps of
 * <step> GB (default 8) up to <cap> GB (default 560) in one form, touches every page (a memset: the mapping is committed),
 * prints the cumulative GB and MemAvailable after each step, stops at the first failure (or the cap), then frees everything.
 * Forms: host (hipHostMalloc NumaUser round-robin over the four NUMA nodes -- mem.c's AF_HOST pool form), dev (hipMalloc
 * round-robin over the four APUs), vmm (hipMemCreate pinned device memory round-robin over the four APUs, mapped with
 * hipMemAddressReserve + hipMemMap + hipMemSetAccess -- dbig.c's VMM arena form, ~L236/L308), malloc (plain malloc + touch:
 * the kernel's overcommit edge).  Run alone on an exclusive node: nothing else may be mapping memory at the same time
 * (archive:results_merged_2026-10-24/AAC7_PORT.md §A16's rule).
 * Form pin (the interplay): t_edge pin [X GB per APU, default 24] [dev step GB, default 2] [dev cap GB, default 440]: holds X GB of hipHostMalloc
 * (NumaUser, touched) on EACH of the devices, then fills hipMalloc in dev-step chunks round-robin over the devices until one fails and prints the
 * hipMalloc edge reached; compare with `t_edge dev 2 440` (nothing pinned): a smaller edge by about the pinned amount = pinned memory counts against it.
 * Usage: t_edge [host|dev|vmm|malloc] [step GB] [cap GB]   |   t_edge pin [X GB per APU] [dev step GB] [dev cap GB] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hip/hip_runtime.h>
static double avail_gb(void) { FILE *f = fopen("/proc/meminfo", "r"); char k[64]; double v = 0, r = -1; while (f && fscanf(f, "%63s %lf kB", k, &v) == 2) if (!strcmp(k, "MemAvailable:")) { r = v * 1024 / 1e9; break; } if (f) fclose(f); return r; }
int main(int argc, char **argv)
{
    const char *form = argc > 1 ? argv[1] : "host"; double step = argc > 2 ? atof(argv[2]) : 8.0, cap = argc > 3 ? atof(argv[3]) : 560.0;
    size_t bytes = (size_t)(step * 1e9); int ndev = 1; (void)hipGetDeviceCount(&ndev); if (ndev < 1) ndev = 1; if (ndev > 4) ndev = 4;
    if (!strcmp(form, "pin")) {
        double x = argc > 2 ? atof(argv[2]) : 24.0, ds = argc > 3 ? atof(argv[3]) : 2.0, dc = argc > 4 ? atof(argv[4]) : 440.0, pinned = 0, devtot = 0; double chunk = 2.0;
        void *hp[512], *dp[512]; int nh = 0, nd = 0; size_t hb = (size_t)(chunk * 1e9), db = (size_t)(ds * 1e9);
        printf("t_edge: pin %.1f GB per APU (hipHostMalloc NumaUser, touched), then hipMalloc in %.1f GB chunks over %d devices up to %.0f GB; MemAvailable %.1f GB\n", x, ds, ndev, dc, avail_gb());
        for (int d = 0; d < ndev && x > 0; d++) {
            double got = 0; (void)hipSetDevice(d);
            while (got + chunk <= x + 1e-9 && nh < 512) { void *q = 0; hipError_t e = hipHostMalloc(&q, hb, hipHostMallocNumaUser);
                if (e != hipSuccess || !q) { printf("t_edge: pin on device %d stopped at %.1f GB (%s)\n", d, got, hipGetErrorString(e)); (void)hipGetLastError(); break; }
                memset(q, 0x5A, hb); hp[nh++] = q; got += chunk; pinned += chunk; }
        }
        printf("t_edge: pinned %.1f GB in total; MemAvailable %.1f GB\n", pinned, avail_gb()); fflush(stdout);
        while (devtot + ds <= dc + 1e-9 && nd < 512) { int d = nd % ndev; void *q = 0; (void)hipSetDevice(d); hipError_t e = hipMalloc(&q, db);
            if (e != hipSuccess || !q) { (void)hipGetLastError(); printf("t_edge: hipMalloc failed after %.1f GB (%s)\n", devtot, hipGetErrorString(e)); break; }
            if (hipMemset(q, 0x5A, db) != hipSuccess || hipDeviceSynchronize() != hipSuccess) { printf("t_edge: touch failed after %.1f GB\n", devtot); (void)hipFree(q); break; }
            dp[nd++] = q; devtot += ds; }
        printf("t_edge: pin result: pinned %.1f GB (%.1f per APU), hipMalloc edge %.1f GB per node = %.2f GB per APU (the cap %.0f GB)\n", pinned, pinned / ndev, devtot, devtot / ndev, dc);
        for (int i = 0; i < nd; i++) (void)hipFree(dp[i]);
        for (int i = 0; i < nh; i++) (void)hipHostFree(hp[i]);
        printf("t_edge: freed; MemAvailable %.1f GB\n", avail_gb());
        return 0;
    }
    void *p[512]; hipMemGenericAllocationHandle_t vh[512]; size_t vbytes[512]; int n = 0; double tot = 0;
    printf("t_edge: form %s, %.1f GB steps up to %.0f GB; %d devices; MemAvailable %.1f GB at the start\n", form, step, cap, ndev, avail_gb());
    while (tot + step <= cap + 1e-9 && n < 512) {
        int d = n % ndev; void *q = 0; hipError_t e = hipSuccess;
        if (!strcmp(form, "host")) { (void)hipSetDevice(d); e = hipHostMalloc(&q, bytes, hipHostMallocNumaUser); }
        else if (!strcmp(form, "dev")) { (void)hipSetDevice(d); e = hipMalloc(&q, bytes); }
        else if (!strcmp(form, "vmm")) {
            hipMemAllocationProp prop; memset(&prop, 0, sizeof prop);
            prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = d;
            size_t gran = 0;
            if (hipMemGetAllocationGranularity(&gran, &prop, hipMemAllocationGranularityRecommended) != hipSuccess || !gran) gran = (size_t)2 << 20;
            size_t rbytes = (bytes + gran - 1) / gran * gran;
            hipMemGenericAllocationHandle_t h = 0;
            if ((e = hipMemCreate(&h, rbytes, &prop, 0)) != hipSuccess) { printf("t_edge: step %d (%.1f GB so far): vmm failed at hipMemCreate (%s)\n", n + 1, tot, hipGetErrorString(e)); fflush(stdout); break; }
            if ((e = hipMemAddressReserve((void **)&q, rbytes, gran, 0, 0)) != hipSuccess) { printf("t_edge: step %d (%.1f GB so far): vmm failed at hipMemAddressReserve (%s)\n", n + 1, tot, hipGetErrorString(e)); (void)hipMemRelease(h); fflush(stdout); break; }
            if ((e = hipMemMap(q, rbytes, 0, h, 0)) != hipSuccess) { printf("t_edge: step %d (%.1f GB so far): vmm failed at hipMemMap (%s)\n", n + 1, tot, hipGetErrorString(e)); (void)hipMemAddressFree(q, rbytes); (void)hipMemRelease(h); fflush(stdout); break; }
            hipMemAccessDesc ad; memset(&ad, 0, sizeof ad); ad.location.type = hipMemLocationTypeDevice; ad.location.id = d; ad.flags = hipMemAccessFlagsProtReadWrite;
            if ((e = hipMemSetAccess(q, rbytes, &ad, 1)) != hipSuccess) { printf("t_edge: step %d (%.1f GB so far): vmm failed at hipMemSetAccess (%s)\n", n + 1, tot, hipGetErrorString(e)); (void)hipMemUnmap(q, rbytes); (void)hipMemAddressFree(q, rbytes); (void)hipMemRelease(h); fflush(stdout); break; }
            vh[n] = h; vbytes[n] = rbytes;
        }
        else { q = malloc(bytes); if (!q) e = hipErrorOutOfMemory; }
        if (e != hipSuccess || !q) { printf("t_edge: step %d (%.1f GB so far): %s failed (%s)\n", n + 1, tot, form, hipGetErrorString(e)); fflush(stdout); break; }
        if (!strcmp(form, "dev")) { if ((e = hipMemset(q, 0x5A, bytes)) != hipSuccess || (e = hipDeviceSynchronize()) != hipSuccess) { printf("t_edge: step %d: touch failed (%s)\n", n + 1, hipGetErrorString(e)); break; } }
        else if (!strcmp(form, "vmm")) { (void)hipSetDevice(d); if ((e = hipMemset(q, 0x5A, bytes)) != hipSuccess || (e = hipDeviceSynchronize()) != hipSuccess) { printf("t_edge: step %d: touch failed (%s)\n", n + 1, hipGetErrorString(e)); break; } }
        else memset(q, 0x5A, bytes);
        p[n++] = q; tot += step;
        printf("t_edge: %6.1f GB committed in %d steps, MemAvailable %.1f GB\n", tot, n, avail_gb()); fflush(stdout);
    }
    printf("t_edge: the edge in form %s: %.1f GB committed and touched (the cap %.0f GB)\n", form, tot, cap);
    for (int i = 0; i < n; i++) {
        if (!strcmp(form, "host")) (void)hipHostFree(p[i]);
        else if (!strcmp(form, "dev")) (void)hipFree(p[i]);
        else if (!strcmp(form, "vmm")) { (void)hipMemUnmap(p[i], vbytes[i]); (void)hipMemAddressFree(p[i], vbytes[i]); (void)hipMemRelease(vh[i]); }
        else free(p[i]);
    }
    printf("t_edge: freed; MemAvailable %.1f GB\n", avail_gb());
    return 0;
}
