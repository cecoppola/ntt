/* target_probe_shmem.c: tiny SHMEM capability probe used by tools/target_probe.sh (TASKS 28).  Prints "PROBE key=value" lines from PE 0.
 * Checks: library version, whether shmem_ptr gives a non-NULL on-node address of a peer's symmetric block (on-node path / XPMEM),
 * whether a put and a get between PEs work, and whether shmem_malloc'd memory is device memory (HIP, loaded with dlopen: no HIP at build time).
 * Build and run only by the probe; no site data. */
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
int main(void) {
    int prov = 0;
    shmem_init_thread(SHMEM_THREAD_MULTIPLE, &prov);
    int me = shmem_my_pe(), n = shmem_n_pes();
    size_t bytes = 64u << 20;
    long *buf = (long *)shmem_malloc(bytes);
    if (!buf) { if (me == 0) printf("PROBE malloc=FAIL bytes=%zu\n", bytes); shmem_finalize(); return 1; }
    memset(buf, 0, 4096);
    buf[0] = 100 + me; shmem_barrier_all();
    if (me == 0) {
        char name[SHMEM_MAX_NAME_LEN]; int maj = 0, min = 0;
        shmem_info_get_name(name); shmem_info_get_version(&maj, &min);
        printf("PROBE lib=%.60s spec=%d.%d npes=%d thread_provided=%d (MULTIPLE=%d)\n", name, maj, min, n, prov, SHMEM_THREAD_MULTIPLE);
    }
    int ptr_ok = 0, ptr_n = 0, get_ok = 0, put_ok = 0;
    for (int p = 0; p < n; p++) if (p != me) { ptr_n++; if (shmem_ptr(buf, p)) ptr_ok++; }
    if (n > 1) {
        int peer = (me + 1) % n;
        get_ok = (shmem_long_g(buf, peer) == 100 + peer);
        shmem_long_p(buf + 1, 7000 + me, peer); shmem_barrier_all();
        put_ok = (buf[1] == 7000 + (me + n - 1) % n);
    }
    int dev = -1; void *h = dlopen("libamdhip64.so", RTLD_NOW);
    if (h) {   /* hipPointerAttribute_t starts with the hipMemoryType: 0 host, 1 device, 2 managed (ROCm 6+) */
        int (*pga)(void *, const void *) = (int (*)(void *, const void *))dlsym(h, "hipPointerGetAttributes");
        int (*gle)(void) = (int (*)(void))dlsym(h, "hipGetLastError");
        if (pga) { int a[64]; memset(a, 0xff, sizeof a); int r = pga(a, buf); if (gle) gle(); dev = (r == 0) ? (a[0] == 1 || a[0] == 2) : -2 - r; }
    }
    printf("PROBE pe=%d shmem_ptr_peers_nonnull=%d/%d get_ok=%d put_ok=%d heap_is_device=%d (1 yes, 0 host, -1 no HIP, <-1 HIP error)\n", me, ptr_ok, ptr_n, get_ok, put_ok, dev);
    shmem_barrier_all(); shmem_free(buf); shmem_finalize();
    return 0;
}
