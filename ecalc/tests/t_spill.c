/* t_spill.c - Phase 14 S1 (APUMULT_STUDY E3, E4): the spill primitive and the O_DIRECT file layer.
 *
 *   tests/t_spill [dir] [GB ...]      (dir: default $ECALC_SPILL_DIR, else /tmp; sizes default 1)
 *
 * 1. sp_file: odd-sized pieces (host and device, single and double buffered, O_DIRECT and buffered) written and read
 *    back in different odd pieces -- the carry, the padded last block and the truncation.
 * 2. per size: a dbig of G GB filled on the device by a hash of the limb index, spilled (four APUs, background),
 *    zeroed, restored, checked limb by limb on the device; the rates printed; /proc/meminfo Cached before / after
 *    (must not grow: O_DIRECT).  Then the same with SPILL_FREE (the pool's live bytes drop by the number's blocks; the
 *    restore reserves fresh ones), and a partial range [lo, hi) across the quarters (limbs outside it stay zero). */
#include <hip/hip_runtime.h>
#include <sys/stat.h>
#include "harness.h"
#include "../dbig.h"
#include "../spill.h"
#include "../mem.h"
#include "../memsample.h"
#include "../rns_mul.h"

#define CK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
__device__ __host__ static inline uint64_t hsh(uint64_t x) { x += 0x9E3779B97F4A7C15ULL; x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL; x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL; return x ^ (x >> 31); }
__global__ void k_fill(uint64_t *p, size_t n, size_t base, uint64_t seed, size_t top, int zero)
{
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) {
        size_t g = base + i; uint64_t v = zero ? 0 : hsh(g ^ seed); if (!zero && g == top) v |= 1; p[i] = v;
    }
}
__global__ void k_check(const uint64_t *p, size_t n, size_t base, uint64_t seed, size_t top, size_t lo, size_t hi, unsigned long long *bad)
{
    unsigned long long b = 0;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) {
        size_t g = base + i; uint64_t v = 0;
        if (g >= lo && g < hi) { v = hsh(g ^ seed); if (g == top) v |= 1; }
        b += p[i] != v;
    }
    if (b) atomicAdd(bad, b);
}
static void fill(dbig *x, size_t n, uint64_t seed, int zero)
{
    for (int d = 0; d < DB_NQ; d++) {
        CK(hipSetDevice(d));
        k_fill<<<1024, 256>>>(x->q[d], x->qc, (size_t)d * x->qc, seed, n - 1, zero);
        CK(hipDeviceSynchronize());
    }
    CK(hipSetDevice(0));
    if (!zero) x->n = n;
}
/* mismatches in the limbs [0, n): expected the hash inside [lo, hi), 0 outside */
static unsigned long long check(const dbig *x, size_t n, uint64_t seed, size_t lo, size_t hi)
{
    unsigned long long tot = 0, *bad;
    CK(hipHostMalloc((void **)&bad, 8, 0));
    for (int d = 0; d < DB_NQ; d++) {
        size_t a = (size_t)d * x->qc; if (a >= n) break;
        size_t m = n - a < x->qc ? n - a : x->qc;
        *bad = 0; CK(hipSetDevice(d));
        k_check<<<1024, 256>>>(x->q[d], m, a, seed, n - 1, lo, hi, bad);
        CK(hipDeviceSynchronize()); tot += *bad;
    }
    CK(hipSetDevice(0)); CK(hipHostFree(bad));
    return tot;
}
static double gib(size_t b) { return b / 1073741824.0; }

static void t_file(const char *dir)
{
    char path[4096]; snprintf(path, sizeof path, "%s/t_spill_file.%d", dir, (int)getpid());
    size_t N = ((size_t)37 << 20) + 12345;                      /* bytes, odd */
    unsigned char *h = (unsigned char *)malloc(N), *r = (unsigned char *)malloc(N);
    for (size_t i = 0; i < N; i++) h[i] = (unsigned char)hsh(i);
    void *dsrc, *ddst; CK(hipSetDevice(1)); CK(hipMalloc(&dsrc, N)); CK(hipMalloc(&ddst, N)); CK(hipMemcpy(dsrc, h, N, hipMemcpyHostToDevice)); CK(hipSetDevice(0));
    size_t bcap = (size_t)4 << 20; void *b0, *b1; if (posix_memalign(&b0, 1 << 21, bcap) || posix_memalign(&b1, 1 << 21, bcap)) exit(1);
    CK(hipHostRegister(b0, bcap, 0)); CK(hipHostRegister(b1, bcap, 0));
    const size_t wp[] = { 1, 4095, 4097, 3 << 20, 10 << 20, 7, 8191 }, rp[] = { 13, (5 << 20) + 1, 4096, 3, 9 << 20 };   /* piece sizes, cycled */
    for (int mode = 0; mode < 8; mode++) {
        int direct = mode & 1, two = (mode >> 1) & 1, dev = (mode >> 2) & 1 ? 1 : -1;
        void *bufs[2] = { b0, two ? b1 : 0 };
        sp_file f; int ok = spf_open(&f, path, 1, direct);
        for (size_t o = 0, k = 0; ok && o < N; k++) { size_t n = wp[k % 7]; if (n > N - o) n = N - o; ok = spf_write_dev(&f, dev, (dev >= 0 ? (unsigned char *)dsrc : h) + o, n, bufs, bcap, 0); o += n; }
        int direct_got = f.direct;
        ok = spf_close(&f, ok);
        struct stat st; stat(path, &st);
        VERIFY(ok && (size_t)st.st_size == N, "sp_file write mode %d: ok %d size %zu of %zu", mode, ok, (size_t)st.st_size, N);
        memset(r, 0, N); if (dev >= 0) CK(hipMemset(ddst, 0, N));
        ok = spf_open(&f, path, 0, direct);
        for (size_t o = 0, k = 0; ok && o < N; k++) { size_t n = rp[k % 5]; if (n > N - o) n = N - o; ok = spf_read_dev(&f, dev, (dev >= 0 ? (unsigned char *)ddst : r) + o, n, bufs, bcap); o += n; }
        ok = spf_close(&f, ok);
        if (dev >= 0) CK(hipMemcpy(r, ddst, N, hipMemcpyDeviceToHost));
        VERIFY(ok && !memcmp(h, r, N), "sp_file read mode %d (direct %d, got O_DIRECT %d, two %d, dev %d)", mode, direct, direct_got, two, dev);
        printf("  sp_file mode %d: %s, %s buffer, %s: %s (O_DIRECT %s)\n", mode, direct ? "direct" : "buffered", two ? "double" : "single", dev >= 0 ? "device" : "host", ok && !memcmp(h, r, N) ? "identical" : "DIFFER", direct_got ? "yes" : "no");
    }
    unlink(path);
    CK(hipHostUnregister(b0)); CK(hipHostUnregister(b1)); free(b0); free(b1); CK(hipFree(dsrc)); CK(hipFree(ddst)); free(h); free(r);
}

static void t_size(const char *dir, double G)
{
    size_t n = (size_t)(G * 1073741824.0 / 8), bytes = n * 8; uint64_t seed = 0xE5E5 + (uint64_t)(G * 100);
    dbig x; db_init(&x); db_reserve(&x, n); fill(&x, n, seed, 0);
    VERIFY(check(&x, n, seed, 0, n) == 0, "fill");
    struct meminfo m0, m1, m2; struct spill_stats st;
    /* A: keep the blocks, zero them, restore */
    mem_meminfo(&m0);
    spill *s = spill_start_dir(&x, 0, n, dir, "t_spill", 0);
    VERIFY(s != 0, "spill_start");
    if (!s) return;
    VERIFY(spill_wait(s), "spill_wait %.1f GB", G);
    mem_meminfo(&m1);
    fill(&x, n, seed, 1);
    VERIFY(spill_restore(s, &x), "restore");
    mem_meminfo(&m2);
    spill_get_stats(s, &st);
    unsigned long long bad = check(&x, n, seed, 0, n);
    VERIFY(bad == 0 && x.n == n, "%.1f GB round trip: %llu limbs differ", G, bad);
    printf("  %6.2f GiB  A (kept):  write %.2f s = %.2f GB/s, read %.2f s = %.2f GB/s, %s; Cached %.2f -> %.2f (after write) -> %.2f GiB (after read), Dirty %.2f, MemAvailable %.1f -> %.1f GiB\n",
           gib(bytes), st.t_write, st.rate_w * 1e-9, st.t_read, st.rate_r * 1e-9, bad ? "DIFFER" : "identical", gib(m0.cached), gib(m1.cached), gib(m2.cached), gib(m1.dirty), gib(m0.avail), gib(m2.avail));
    VERIFY(m1.cached < m0.cached + (size_t)(0.25 * bytes) + ((size_t)1 << 30) && m2.cached < m0.cached + (size_t)(0.25 * bytes) + ((size_t)1 << 30),
           "Cached grew: %.2f -> %.2f -> %.2f GiB", gib(m0.cached), gib(m1.cached), gib(m2.cached));
    spill_drop(s);
    /* B: SPILL_FREE -- the blocks go back to the pool on completion; the restore reserves fresh ones */
    size_t live0 = 0; for (int d = 0; d < DB_NQ; d++) live0 += db_pool_live(d);
    size_t cap = x.cap;
    s = spill_start_dir(&x, 0, n, dir, "t_spill_free", SPILL_FREE);
    VERIFY(x.cap == 0 && x.n == 0, "SPILL_FREE: the caller's descriptor is cleared");
    VERIFY(spill_wait(s), "spill_wait (free)");
    size_t live1 = 0; for (int d = 0; d < DB_NQ; d++) live1 += db_pool_live(d);
    VERIFY(live0 - live1 >= cap * 8, "SPILL_FREE: pool live %.2f -> %.2f GiB (the number %.2f GiB)", gib(live0), gib(live1), gib(cap * 8));
    dbig y; db_init(&y);
    VERIFY(spill_restore_start(s, &y), "restore_start (free)");   /* the background form */
    VERIFY(spill_restore_wait(s), "restore_wait (free)");
    spill_get_stats(s, &st);
    bad = check(&y, n, seed, 0, n);
    VERIFY(bad == 0 && y.n == n, "SPILL_FREE round trip: %llu differ", bad);
    printf("  %6.2f GiB  B (freed): write %.2f GB/s, read %.2f GB/s, %s; pool live %.2f -> %.2f GiB while on disk\n", gib(bytes), st.rate_w * 1e-9, st.rate_r * 1e-9, bad ? "DIFFER" : "identical", gib(live0), gib(live1));
    spill_drop(s);
    /* C: a partial range across the quarters (unaligned ends), restored into zeroed blocks; the buffered fallback too */
    for (int buffered = 0; buffered < 2; buffered++) {
        if (buffered) setenv("SPILL_BUFFERED", "1", 1);
        size_t lo = n / 5 + 3, hi = n - n / 7 - 5;
        mem_meminfo(&m0);
        s = spill_start_dir(&y, lo, hi, dir, "t_spill_part", 0);
        VERIFY(spill_wait(s), "spill_wait (part)");
        fill(&y, n, seed, 1);
        VERIFY(spill_restore(s, &y), "restore (part)");
        mem_meminfo(&m1);
        bad = check(&y, n, seed, lo, hi);
        VERIFY(bad == 0, "partial [%zu, %zu) of %zu%s: %llu differ", lo, hi, n, buffered ? " (buffered)" : "", bad);
        spill_get_stats(s, &st);
        printf("  %6.2f GiB  C (limbs [%zu, %zu)%s): write %.2f GB/s, read %.2f GB/s, %s; Cached %.2f -> %.2f GiB\n", gib(bytes), lo, hi, buffered ? ", buffered + fsync + DONTNEED" : "",
               st.rate_w * 1e-9, st.rate_r * 1e-9, bad ? "DIFFER" : "identical", gib(m0.cached), gib(m1.cached));
        spill_drop(s);
        fill(&y, n, seed, 0);
        if (buffered) unsetenv("SPILL_BUFFERED");
    }
    db_free(&y);
}

int main(int argc, char **argv)
{
    harness_meta("t_spill"); bi_env_base(); rns_init(24);
    double tp = now(); spill_prealloc(); tp = now() - tp;
    const char *dir = argc > 1 ? argv[1] : getenv("ECALC_SPILL_DIR") ? getenv("ECALC_SPILL_DIR") : "/tmp";
    printf("t_spill: dir %s, bounce %s MB x 2 per APU (allocated and pinned in %.2f s)\n", dir, getenv("SPILL_CHUNK_MB") ? getenv("SPILL_CHUNK_MB") : "256", tp);
    t_file(dir);
    if (argc <= 2) t_size(dir, 1);
    for (int i = 2; i < argc; i++) t_size(dir, atof(argv[i]));
    spill_fini();
    return verify_done("t_spill");
}
