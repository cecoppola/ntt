/* memsample.c - see memsample.h (E1, E12) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "memsample.h"
#include "mem.h"
#include "dbig.h"
#include "nodemem.h"
#include "fatal.h"                                   /* the guard stops through ec_fatal (rank, host, phase) */
int mn_rank(void);
static int rank_now(void) { const char *e = getenv("COMM_RANK"); return e ? atoi(e) : mn_rank(); }   /* (the sampler starts before mn_init) */

int mem_meminfo(struct meminfo *m)
{
    memset(m, 0, sizeof *m);
    FILE *f = fopen("/proc/meminfo", "r"); if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char k[64]; unsigned long long v;
        if (sscanf(line, "%63s %llu", k, &v) != 2) continue;
        size_t b = (size_t)v << 10;
        if (!strcmp(k, "MemTotal:")) m->total = b; else if (!strcmp(k, "MemFree:")) m->free = b; else if (!strcmp(k, "MemAvailable:")) m->avail = b;
        else if (!strcmp(k, "Cached:")) m->cached = b; else if (!strcmp(k, "Dirty:")) m->dirty = b; else if (!strcmp(k, "Writeback:")) m->writeback = b;
    }
    fclose(f);
    return 1;
}
int mem_live_on(void)
{
    static int v = -1;
    if (v < 0) { const char *vb = getenv("ECALC_VERBOSE"); v = vb && atoi(vb) >= 2; }
    return v;
}
void mem_live_line(const char *what)
{
    int nd = mem_device_count(); if (nd > DB_NQ || nd < 1) nd = DB_NQ;
    char l[256], p[256], c[256]; int ol = 0, op = 0, oc = 0; size_t tp = 0, tc = 0;
    l[0] = p[0] = c[0] = 0;
    for (int d = 0; d < nd; d++) {
        size_t live = db_pool_live(d), peak = db_pool_window_peak(d, 1), cap = live + db_pool_free_bytes(d);
        if (peak < live) peak = live;
        tp += peak; tc += cap;
        ol += snprintf(l + ol, sizeof l - ol, "%s%.2f", d ? " " : "", live * 1e-9);
        op += snprintf(p + op, sizeof p - op, "%s%.2f", d ? " " : "", peak * 1e-9);
        oc += snprintf(c + oc, sizeof c - oc, "%s%.2f", d ? " " : "", cap * 1e-9);
    }
    struct meminfo m; mem_meminfo(&m);
    printf("live: %-30s pool live %s | window peak %s | pool %s GB per APU; peak/pool %.1f %%; MemAvailable %.1f GB, Cached %.1f GB\n",
           what, l, p, c, tc ? 100.0 * tp / tc : 0, m.avail * 1e-9, m.cached * 1e-9);
}

/* ---- E12: the sampler ---- */
static struct {
    pthread_t th; int on, stop, quiet; double period, t0, guard; FILE *out;   /* quiet = the guard alone (no lines); guard = ECALC_MEM_GUARD_GB in bytes */
    size_t min_avail, max_cached, max_dirty, max_rss, max_live[DB_NQ]; long n;
    int napu, apu_ok, hwm_done, apu_id[DB_NQ], apu_node[DB_NQ]; size_t apu_start[DB_NQ], apu_min[DB_NQ], apu_mf_start[DB_NQ], apu_mf_min[DB_NQ]; double guard_apu;   /* C5: each APU's NUMA node free memory = MemFree + file cache (MEMFIT; *_mf_* = MemFree alone): at the first sample, the minimum since; guard_apu = ECALC_MEM_GUARD_APU_GB in bytes */
    pthread_mutex_t mx; pthread_cond_t cv;
} S = { .mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
static const char *apu_root(void) { const char *r = getenv("ECALC_MEMSAMPLE_SYSFS_ROOT"); return r ? r : ""; }
static int apu_find(int *n, int *ids, int *nodes, int quiet)   /* which APUs this process watches: 1 ok, 0 = the own device is not known yet, -1 = disabled (message unless quiet).
                               * Layout D (one process drives the node) = the 4 NUMA nodes 0..3; layout P (ECALC_PE_PER_GPU, one process per GPU) = its own device's NUMA node.
                               * Any other node count (not 4 in layout D, no node file for the own device) disables the per-APU figure and guard. */
{
    const char *g = getenv("ECALC_PE_PER_GPU"); size_t f;
    if (g && atoi(g) != 0) {
        int o = mem_own_device(); if (o < 0) return 0;
        int nd = mem_numa_node_of_device(o);
        if (nodemem_free(apu_root(), nd, &f)) { *n = 1; ids[0] = o; nodes[0] = nd; return 1; }
        if (!quiet) printf("memsample r%d: per-APU memory figure and guard DISABLED: no readable MemFree for the NUMA node %d of device %d\n", rank_now(), nd, o);
    } else {
        int k = 0; while (k < 64 && nodemem_free(apu_root(), k, &f)) k++;
        if (k == DB_NQ) { *n = k; for (int d = 0; d < k; d++) ids[d] = nodes[d] = d; return 1; }
        if (!quiet) printf("memsample r%d: per-APU memory figure and guard DISABLED: %d NUMA nodes with a MemFree, the layout wants %d APUs\n", rank_now(), k, DB_NQ);
    }
    if (!quiet) fflush(stdout);
    return -1;
}
int mem_apu_free_now(int *ids, size_t *freeb, size_t *memfree)   /* MEMFIT: the free memory (MemFree + file cache) of each APU this process drives, now; returns their number, 0 when not available */
{
    int n = 0, id[DB_NQ], nd[DB_NQ]; if (apu_find(&n, id, nd, 1) != 1) return 0;
    for (int i = 0; i < n; i++) {
        size_t m = 0, fl = 0; if (!nodemem_read(apu_root(), nd[i], &m, &fl)) return 0;
        ids[i] = id[i]; freeb[i] = m + fl; if (memfree) memfree[i] = m;
    }
    return n;
}
static int apu_setup(void)    /* C5: the watch list, once known */
{
    if (S.napu) return S.napu > 0;
    int n = 0, r = apu_find(&n, S.apu_id, S.apu_node, 0);
    if (r == 1) { S.napu = n; return 1; }
    if (r == 0) return 0;                          /* the own device is not known yet: try again at the next sample */
    S.napu = -1; S.guard_apu = 0; return 0;
}
static void apu_sample(size_t *cur)    /* free memory (MemFree + file cache) of each watched APU's NUMA node; cur[i] = 0 when unreadable or not watched */
{
    for (int i = 0; i < DB_NQ; i++) cur[i] = 0;
    if (!apu_setup()) return;
    for (int d = 0; d < S.napu; d++) {
        size_t m = 0, fl = 0;
        if (!nodemem_read(apu_root(), S.apu_node[d], &m, &fl)) continue;
        size_t f = m + fl;
        cur[d] = f;
        if (!S.apu_ok) { S.apu_start[d] = S.apu_min[d] = f; S.apu_mf_start[d] = S.apu_mf_min[d] = m; }
        else { if (f < S.apu_min[d]) S.apu_min[d] = f; if (m < S.apu_mf_min[d]) S.apu_mf_min[d] = m; }
    }
    S.apu_ok = 1;
}
static void apu_hwm_line(void)    /* once: the largest drop of each watched APU's free memory (MemFree + file cache) from its value at the first sample = what the run itself took from that APU's HBM; the old MemFree-only figure follows on a second line */
{
    if (!S.apu_ok || S.hwm_done) return;
    S.hwm_done = 1;
    for (int pass = 0; pass < 2; pass++) {
        const size_t *s0 = pass ? S.apu_mf_start : S.apu_start, *m0 = pass ? S.apu_mf_min : S.apu_min;
        char drop[160], st[160], mn[160]; int od = 0, os = 0, om = 0, w = 0; double wv = -1;
        for (int d = 0; d < S.napu; d++) {
            double dv = s0[d] > m0[d] ? (double)(s0[d] - m0[d]) * 1e-9 : 0;
            if (dv > wv) { wv = dv; w = S.apu_id[d]; }
            od += snprintf(drop + od, sizeof drop - od, "%s%.2f", d ? "," : "", dv); os += snprintf(st + os, sizeof st - os, "%s%.2f", d ? "," : "", s0[d] * 1e-9);
            om += snprintf(mn + om, sizeof mn - om, "%s%.2f", d ? "," : "", m0[d] * 1e-9);
        }
        if (!pass) printf("memsample r%d device_hwm_apu_gb=%.2f apu=%d per_apu_gb=%s start_free_gb=%s min_free_gb=%s (largest drop of each watched APU's NUMA-node free memory = MemFree + Active(file) + Inactive(file) since the sampler start; reclaimable file cache is not counted as used)\n", rank_now(), wv, w, drop, st, mn);
        else printf("memsample r%d device_hwm_apu_memfree_gb=%.2f apu_memfree=%d per_apu_memfree_gb=%s start_memfree_gb=%s min_memfree_gb=%s (the old figure, for comparison: MemFree alone, includes page cache)\n", rank_now(), wv, w, drop, st, mn);
    }
    fflush(stdout);
}
static void sample_once(void)
{
    struct meminfo m; mem_meminfo(&m);
    size_t rss = mem_vmrss(), live[DB_NQ] = {0};
    for (int d = 0; d < DB_NQ; d++) { live[d] = db_pool_live(d); if (live[d] > S.max_live[d]) S.max_live[d] = live[d]; }
    if (!S.n || m.avail < S.min_avail) S.min_avail = m.avail;
    if (m.cached > S.max_cached) S.max_cached = m.cached;
    if (m.dirty > S.max_dirty) S.max_dirty = m.dirty;
    if (rss > S.max_rss) S.max_rss = rss;
    S.n++;
    size_t af[DB_NQ]; apu_sample(af);
    if (S.guard_apu > 0) for (int d = 0; d < S.napu; d++) if (af[d] && (double)af[d] < S.guard_apu) {
        apu_hwm_line();                                  /* the high-water figure is printed before the run stops */
        ec_fatal(EC_RC_MEMGUARD, "per-APU memory guard (ECALC_MEM_GUARD_APU_GB=%.1f): APU %d (NUMA node %d) free memory (MemFree + file cache) %.2f GB is below it on rank %d at %.1f s (its free memory at the start %.2f GB, node MemAvailable %.2f GB, RSS %.2f GB); the run stops, rc %d",
                 S.guard_apu * 1e-9, S.apu_id[d], S.apu_node[d], af[d] * 1e-9, rank_now(), mem_now() - S.t0, S.apu_start[d] * 1e-9, m.avail * 1e-9, rss * 1e-9, EC_RC_MEMGUARD); }
    if (S.guard > 0 && m.avail && (double)m.avail < S.guard) {   /* a clean stop, rank and phase named by ec_fatal's header */
        if (!S.quiet) fflush(S.out);
        apu_hwm_line();
        ec_fatal(EC_RC_MEMGUARD, "memory guard (ECALC_MEM_GUARD_GB=%.1f): MemAvailable %.2f GB is below it on rank %d at %.1f s (MemFree %.2f, Cached %.2f, Dirty %.2f GB; RSS %.2f GB; pool live %.2f %.2f %.2f %.2f GB); the run stops, rc %d",
                 S.guard * 1e-9, m.avail * 1e-9, rank_now(), mem_now() - S.t0, m.free * 1e-9, m.cached * 1e-9, m.dirty * 1e-9, rss * 1e-9, live[0] * 1e-9, live[1] * 1e-9, live[2] * 1e-9, live[3] * 1e-9, EC_RC_MEMGUARD);
    }
    if (S.quiet) return;
    fprintf(S.out, "memsample r%d t %8.1f s: RSS %.2f MemFree %.2f MemAvailable %.2f Cached %.2f Dirty %.2f Writeback %.2f | pool live %.2f %.2f %.2f %.2f GB\n",
            rank_now(), mem_now() - S.t0, rss * 1e-9, m.free * 1e-9, m.avail * 1e-9, m.cached * 1e-9, m.dirty * 1e-9, m.writeback * 1e-9,
            live[0] * 1e-9, live[1] * 1e-9, live[2] * 1e-9, live[3] * 1e-9);
    fflush(S.out);
}
static void *sampler(void *a)
{
    (void)a;
    pthread_mutex_lock(&S.mx);
    while (!S.stop) {
        pthread_mutex_unlock(&S.mx);
        sample_once();
        pthread_mutex_lock(&S.mx);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        double w = S.period; ts.tv_sec += (time_t)w; ts.tv_nsec += (long)((w - (double)(time_t)w) * 1e9);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (!S.stop) pthread_cond_timedwait(&S.cv, &S.mx, &ts);
    }
    pthread_mutex_unlock(&S.mx);
    return 0;
}
void mem_sampler_stop(void)
{
    if (!S.on) return;
    pthread_mutex_lock(&S.mx); S.stop = 1; pthread_cond_signal(&S.cv); pthread_mutex_unlock(&S.mx);
    pthread_join(S.th, 0); S.on = 0;
    if (S.apu_ok) { size_t af[DB_NQ]; apu_sample(af); apu_hwm_line(); }
    if (S.quiet) return;
    sample_once();
    fprintf(S.out, "memsample r%d summary: %ld samples over %.1f s: MemAvailable min %.2f GB, Cached max %.2f, Dirty max %.2f, RSS max %.2f, pool live max %.2f %.2f %.2f %.2f GB\n",
            rank_now(), S.n, mem_now() - S.t0, S.min_avail * 1e-9, S.max_cached * 1e-9, S.max_dirty * 1e-9, S.max_rss * 1e-9,
            S.max_live[0] * 1e-9, S.max_live[1] * 1e-9, S.max_live[2] * 1e-9, S.max_live[3] * 1e-9);
    fflush(S.out);
}
void mem_sampler_start(void)
{
    const char *e = getenv("ECALC_MEM_SAMPLE"), *g = getenv("ECALC_MEM_GUARD_GB");
    double guard = g ? atof(g) * 1e9 : 0;                /* the guard runs the sampler without its lines when ECALC_MEM_SAMPLE is unset */
    const char *ga = getenv("ECALC_MEM_GUARD_APU_GB"); double guard_apu = ga ? atof(ga) * 1e9 : 0;   /* C5: per-APU guard, default off */
    if (S.on || ((!e || atof(e) <= 0) && guard <= 0 && guard_apu <= 0)) return;
    S.quiet = !e || atof(e) <= 0; S.guard = guard; S.guard_apu = guard_apu; S.napu = 0; S.apu_ok = 0; S.hwm_done = 0;
    S.period = S.quiet ? 1.0 : atof(e); if (S.period <= 0) S.period = 1.0;
    S.t0 = mem_now(); S.stop = 0; S.n = 0; S.out = stderr;
    if (guard > 0 && rank_now() == 0) printf("memsample: ECALC_MEM_GUARD_GB=%.1f: the run stops (rc %d) when MemAvailable falls below it (sampled every %.2f s)\n", guard * 1e-9, EC_RC_MEMGUARD, S.period);
    if (guard_apu > 0 && rank_now() == 0) printf("memsample: ECALC_MEM_GUARD_APU_GB=%.1f: the run stops (rc %d) when any APU's NUMA-node free memory (MemFree + file cache) falls below it (sampled every %.2f s)\n", guard_apu * 1e-9, EC_RC_MEMGUARD, S.period);
    if (pthread_create(&S.th, 0, sampler, 0)) { fprintf(stderr, "memsample: cannot start the thread\n"); return; }
    S.on = 1;
    static int reg = 0; if (!reg) { atexit(mem_sampler_stop); reg = 1; }
}
