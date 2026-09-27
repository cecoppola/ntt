/* t_vmm_par - Phase 15 RL: the VMM block pool's remap under DB_POOL_VMM_PAR (off/on, same checks).
 *   t_vmm_par [arena GiB per APU (16)] [rounds (6)] [chunk GiB (2): sets DB_POOL_VMM_CHUNK_GB unless set]
 * Every round, on each of the four APUs: the arena is filled with chunk-sized blocks carrying a byte pattern, every other one is
 * freed (half the chunks wholly free, none adjacent), and ONE db_reserve asks for quarters of half the arena -- so every APU must
 * remap at once (the case of the division's t and xq at 1e11, results/T215.md 4).  A background thread meanwhile allocates,
 * writes, checks and frees small blocks on every APU (from a small donated region: it exercises the lock being dropped and a
 * its requests proceed while a concurrent remap runs, without taking part of a free chunk).  Checked: the kept blocks' patterns survive the
 * remap, the new quarters are writable and read back, the pool's live / free bytes and mem_report's accounting (the chunks mapped)
 * are exact, one remap per APU per round and no growth, no hipMalloc inside the phase.  Prints db_reserve's time per round
 * (serial remaps: ~4x one APU's; concurrent: ~1x if the driver lets the four overlap). */
#include "harness.h"
#include <hip/hip_runtime.h>
#include <pthread.h>
#include "../dbig.h"
#include "../mem.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
static int sample_ok(const void *p, size_t bytes, unsigned char v)   /* the first, a middle and the last 1 MiB hold byte v */
{
    static unsigned char *h; size_t s = (size_t)1 << 20; if (!h) h = (unsigned char *)malloc(s);
    size_t off[3] = { 0, (bytes / 2) & ~(s - 1), bytes - s };
    for (int k = 0; k < 3; k++) { HIP_CHECK(hipMemcpy(h, (const char *)p + off[k], s, hipMemcpyDeviceToHost)); for (size_t i = 0; i < s; i += 4093) if (h[i] != v) return 0; if (h[s - 1] != v) return 0; }
    return 1;
}
static volatile int g_stop; static int g_bg_bad, g_bg_n; static pthread_mutex_t g_bgmx = PTHREAD_MUTEX_INITIALIZER;   /* held by the test while it reads the pool's totals */
static void *bg_small(void *arg)                                   /* small blocks on every APU while the remaps run */
{
    (void)arg; unsigned char *h = (unsigned char *)malloc(1 << 20); unsigned it = 0;
    while (!g_stop) {
        pthread_mutex_lock(&g_bgmx);
        int d = (int)(it % DB_NQ); unsigned char v = (unsigned char)(it * 7 + 1); it++;
        uint64_t *p = db_pool_alloc(d, (size_t)1 << 20);
        HIP_CHECK(hipMemset(p, v, (size_t)1 << 20)); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(h, p, (size_t)1 << 20, hipMemcpyDeviceToHost)); if (h[0] != v || h[(1 << 20) - 1] != v) g_bg_bad++;
        db_pool_free(d, p); g_bg_n++;
        pthread_mutex_unlock(&g_bgmx);
    }
    free(h); return 0;
}
int main(int argc, char **argv)
{
    double agb = argc > 1 ? atof(argv[1]) : 16, cgb = argc > 3 ? atof(argv[3]) : 2; int rounds = argc > 2 ? atoi(argv[2]) : 6;
    char cs[32]; snprintf(cs, sizeof cs, "%g", cgb); setenv("DB_POOL_VMM_CHUNK_GB", cs, 0); cgb = atof(getenv("DB_POOL_VMM_CHUNK_GB"));
    harness_meta("t_vmm_par");
    const char *pe = getenv("DB_POOL_VMM_PAR"); int par = pe && atoi(pe);
    if (!db_pool_vmm_on()) { printf("t_vmm_par: DB_POOL_VMM is off: nothing to test\n"); return 0; }
    size_t C = (size_t)(cgb * 1073741824.0), A = (size_t)(agb / cgb + 0.5) * C; int k = (int)(A / C); if (k % 2) { k--; A -= C; }
    printf("t_vmm_par: DB_POOL_VMM_PAR=%d, arena %.2f GB per APU = %d chunks of %.2f GiB, %d rounds\n", par, A / 1e9, k, cgb, rounds);
    void *base[DB_NQ], *sreg[DB_NQ]; size_t sbytes = (size_t)64 << 20;
    for (int d = 0; d < DB_NQ; d++) { base[d] = db_vmm_arena_alloc(d, A, 0); db_donate_adjacent(d, base[d], A); mem_dev_note(d, base[d], A);
                                      HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&sreg[d], sbytes)); db_donate(d, sreg[d], sbytes); }
    HIP_CHECK(hipSetDevice(0));
    uint64_t **blk = (uint64_t **)calloc((size_t)DB_NQ * k, sizeof *blk);
    pthread_t bg; g_stop = 0; if (pthread_create(&bg, 0, bg_small, 0)) { fprintf(stderr, "pthread_create\n"); return 1; }
    double tsum = 0, tmin = 1e30, tmax = 0;
    for (int r = 0; r < rounds; r++) {
        size_t rm0[DB_NQ], gr0[DB_NQ]; for (int d = 0; d < DB_NQ; d++) db_pool_vmm_stats(d, &rm0[d], &gr0[d]);
        for (int d = 0; d < DB_NQ; d++) for (int i = 0; i < k; i++) { uint64_t *p = db_pool_alloc(d, C); blk[d * k + i] = p; HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemset(p, (unsigned char)(16 * d + i + r), C)); }
        for (int d = 0; d < DB_NQ; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipDeviceSynchronize()); }
        for (int d = 0; d < DB_NQ; d++) for (int i = 0; i < k; i += 2) { db_pool_free(d, blk[d * k + i]); blk[d * k + i] = 0; }
        dbig x; db_init(&x);
        double t0 = now(); db_reserve(&x, (size_t)DB_NQ * (A / 2) / 8); double t = now() - t0;
        tsum += t; if (t < tmin) tmin = t; if (t > tmax) tmax = t;
        int okp = 1; for (int d = 0; d < DB_NQ; d++) for (int i = 1; i < k; i += 2) okp &= sample_ok(blk[d * k + i], C, (unsigned char)(16 * d + i + r));
        VERIFY(okp, "round %d: the kept blocks' patterns survive the remaps", r);
        for (int d = 0; d < DB_NQ; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemset(x.q[d], (unsigned char)(0xA0 + d), A / 2)); HIP_CHECK(hipDeviceSynchronize()); }
        int okx = 1; for (int d = 0; d < DB_NQ; d++) okx &= sample_ok(x.q[d], A / 2, (unsigned char)(0xA0 + d)) && (char *)x.q[d] >= (char *)base[d] && (char *)x.q[d] < (char *)base[d] + 3 * A + C;
        VERIFY(okx, "round %d: the new quarters lie in their APU's VA range, are writable and read back", r);
        size_t rm1[DB_NQ], gr1[DB_NQ]; int okr = 1;
        for (int d = 0; d < DB_NQ; d++) { db_pool_vmm_stats(d, &rm1[d], &gr1[d]); okr &= rm1[d] == rm0[d] + 1 && gr1[d] == gr0[d]; }
        VERIFY(okr, "round %d: one remap per APU, no growth (remaps %zu %zu %zu %zu)", r, rm1[0] - rm0[0], rm1[1] - rm0[1], rm1[2] - rm0[2], rm1[3] - rm0[3]);
        pthread_mutex_lock(&g_bgmx);
        mem_report("t_vmm_par");                                    /* the accounting: prints the table; the checks below read the pool */
        int oka = 1; for (int d = 0; d < DB_NQ; d++) { size_t fb = db_pool_free_bytes(d), lb = db_pool_live(d);
            /* the arena: k/2 kept + k/2 in x (all chunks live, none free); the small region: 64 MiB less the bg thread's block */
            oka &= fb + lb == A + sbytes && lb >= A && db_pool_largest_free(d) <= sbytes && db_pool_hipmalloc_bytes(d) == 0; }
        pthread_mutex_unlock(&g_bgmx);
        VERIFY(oka, "round %d: live + free = arena + small region on every APU, the arena fully live, no hipMalloc", r);
        printf("  round %d: db_reserve of 4 x %.2f GB (4 remaps of %d chunks) in %.3f s\n", r, A / 2 / 1e9, k / 2, t);
        db_free(&x); for (int d = 0; d < DB_NQ; d++) for (int i = 1; i < k; i += 2) { db_pool_free(d, blk[d * k + i]); blk[d * k + i] = 0; }
        pthread_mutex_lock(&g_bgmx);
        for (int d = 0; d < DB_NQ; d++) VERIFY(db_pool_free_bytes(d) + db_pool_live(d) == A + sbytes, "round %d APU %d: all bytes back", r, d);
        pthread_mutex_unlock(&g_bgmx);
    }
    g_stop = 1; pthread_join(bg, 0);
    VERIFY(g_bg_bad == 0 && g_bg_n > 0, "background small blocks: %d allocations, %d bad", g_bg_n, g_bg_bad);
    printf("t_vmm_par: DB_POOL_VMM_PAR=%d: db_reserve with 4 remaps: mean %.3f s, min %.3f, max %.3f over %d rounds; %d background blocks\n", par, tsum / rounds, tmin, tmax, rounds, g_bg_n);
    harness_result(par ? "reserve_remap4_par_s" : "reserve_remap4_serial_s", "s", tsum / rounds);
    int nf = verify_done("t_vmm_par");
    db_release_pools(); for (int d = 0; d < DB_NQ; d++) db_vmm_arena_release(d);
    return nf;
}
