/* 23_d2h - is the D2H blit really 58 GB/s? (PLAN.md D7)
 *
 * bench/17 and 18 measured hipMemcpy device->host at a flat 58.5 GB/s per APU:
 * one copy on the null stream into registered host memory.  This varies what
 * a careful implementation would vary -- created streams, several copies in
 * flight per device, chunk size, and hipHostMalloc vs registered targets --
 * on all four APUs at once, 4 GiB per APU, and reports the best.
 *
 * Usage: 23_d2h
 */
#include "common_ntt.h"

int main(void)
{
    const size_t bytes = (size_t)4 << 30;
    int nd = device_count(), tgt, nf, ci;
    int inflight[4] = { 1, 2, 4, 8 };
    size_t chunks[3] = { (size_t)64 << 20, (size_t)512 << 20, (size_t)4 << 30 };
    double r[MAXD], bestall = 0; char bestnm[64] = "";
    uint64_t *D[MAXD], *H[MAXD][2];

    printf("== 23_d2h : device->host memcpy, streams x in-flight x chunk x target (4 GiB per APU) ==\n");
    meta("23_d2h");
    for (int d = 0; d < nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipMalloc(&D[d], bytes)); HIP_CHECK(hipMemset(D[d], 1, bytes));
        HIP_CHECK(hipHostMalloc((void **)&H[d][0], bytes, hipHostMallocNonCoherent));
        H[d][1] = (uint64_t *)aligned_alloc(1 << 21, bytes); memset(H[d][1], 0, bytes);
        HIP_CHECK(hipHostRegister(H[d][1], bytes, hipHostRegisterDefault));
    }
    header("configuration");
    for (tgt = 0; tgt < 2; tgt++)
    for (ci = 0; ci < 3; ci++)
    for (nf = 0; nf < 4; nf++) {
        size_t chunk = chunks[ci]; int nin = inflight[nf];
        if ((size_t)nin * chunk > bytes) continue;
        char nm[64];
#pragma omp parallel num_threads(nd)
        {
            int dev = omp_get_thread_num(), rep, s;
            hipStream_t st[8]; hipEvent_t e0, e1; float ms; double best = 1e300;
            HIP_CHECK(hipSetDevice(dev));
            for (s = 0; s < nin; s++) HIP_CHECK(hipStreamCreateWithFlags(&st[s], hipStreamNonBlocking));
            timer_events(&e0, &e1);
            for (rep = 0; rep < 3; rep++) {
                size_t off = 0; int k = 0;
#pragma omp barrier
                HIP_CHECK(hipEventRecord(e0, 0));
                for (off = 0; off < bytes; off += chunk, k++)
                    HIP_CHECK(hipMemcpyAsync((char *)H[dev][tgt] + off, (char *)D[dev] + off, chunk,
                                             hipMemcpyDeviceToHost, st[k % nin]));
                for (s = 0; s < nin; s++) HIP_CHECK(hipStreamSynchronize(st[s]));
                HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
                HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); best = dmin(best, ms);
            }
            r[dev] = (double)bytes / (best * 1e-3) / 1e9;
            for (s = 0; s < nin; s++) HIP_CHECK(hipStreamDestroy(st[s]));
        }
        snprintf(nm, sizeof nm, "%s %zuMiB x%d", tgt ? "registered" : "hostMalloc", chunk >> 20, nin);
        report_sum(nm, "GB/s", r, nd);
        if (r[0] > bestall) { bestall = r[0]; strcpy(bestnm, nm); }
    }
    /* and the 17/18 baseline: null stream, one copy */
#pragma omp parallel num_threads(nd)
    {
        int dev = omp_get_thread_num(), rep; hipEvent_t e0, e1; float ms; double best = 1e300;
        HIP_CHECK(hipSetDevice(dev)); timer_events(&e0, &e1);
        for (rep = 0; rep < 3; rep++) {
#pragma omp barrier
            HIP_CHECK(hipEventRecord(e0, 0));
            HIP_CHECK(hipMemcpyAsync(H[dev][1], D[dev], bytes, hipMemcpyDeviceToHost, 0));
            HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
            HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); best = dmin(best, ms);
        }
        r[dev] = (double)bytes / (best * 1e-3) / 1e9;
    }
    report_sum("null stream, one copy (17/18)", "GB/s", r, nd);
    printf("\nbest per APU: %.1f GB/s with %s.  Kernel store (18): ~1 800 GB/s.\n", bestall, bestnm);
    return 0;
}
