/* memsample.c - see memsample.h (Phase 14 S1: E1, E12) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "memsample.h"
#include "mem.h"
#include "dbig.h"
int mn_rank(void);

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
    if (v < 0) { const char *e = getenv("ECALC_LIVE"), *vb = getenv("ECALC_VERBOSE"); v = (e && atoi(e)) || (vb && atoi(vb) >= 2); }
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
    pthread_t th; int on, stop; double period, t0; FILE *out;
    size_t min_avail, max_cached, max_dirty, max_rss, max_live[DB_NQ]; long n;
    pthread_mutex_t mx; pthread_cond_t cv;
} S = { .mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
static void sample_once(void)
{
    struct meminfo m; mem_meminfo(&m);
    size_t rss = mem_vmrss(), live[DB_NQ];
    for (int d = 0; d < DB_NQ; d++) { live[d] = db_pool_live(d); if (live[d] > S.max_live[d]) S.max_live[d] = live[d]; }
    if (!S.n || m.avail < S.min_avail) S.min_avail = m.avail;
    if (m.cached > S.max_cached) S.max_cached = m.cached;
    if (m.dirty > S.max_dirty) S.max_dirty = m.dirty;
    if (rss > S.max_rss) S.max_rss = rss;
    S.n++;
    fprintf(S.out, "memsample r%d t %8.1f s: RSS %.2f MemFree %.2f MemAvailable %.2f Cached %.2f Dirty %.2f Writeback %.2f | pool live %.2f %.2f %.2f %.2f GB\n",
            mn_rank(), mem_now() - S.t0, rss * 1e-9, m.free * 1e-9, m.avail * 1e-9, m.cached * 1e-9, m.dirty * 1e-9, m.writeback * 1e-9,
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
    sample_once();
    fprintf(S.out, "memsample r%d summary: %ld samples over %.1f s: MemAvailable min %.2f GB, Cached max %.2f, Dirty max %.2f, RSS max %.2f, pool live max %.2f %.2f %.2f %.2f GB\n",
            mn_rank(), S.n, mem_now() - S.t0, S.min_avail * 1e-9, S.max_cached * 1e-9, S.max_dirty * 1e-9, S.max_rss * 1e-9,
            S.max_live[0] * 1e-9, S.max_live[1] * 1e-9, S.max_live[2] * 1e-9, S.max_live[3] * 1e-9);
    fflush(S.out);
    if (S.out != stderr) fclose(S.out);
    S.out = stderr;
}
void mem_sampler_start(void)
{
    const char *e = getenv("ECALC_MEM_SAMPLE");
    if (S.on || !e || atof(e) <= 0) return;
    S.period = atof(e); S.t0 = mem_now(); S.stop = 0; S.n = 0; S.out = stderr;
    const char *fn = getenv("ECALC_MEM_SAMPLE_FILE");
    if (fn && *fn) {
        char name[4096];
        const char *pc = strstr(fn, "%d");
        if (pc) snprintf(name, sizeof name, "%.*s%d%s", (int)(pc - fn), fn, mn_rank(), pc + 2); else snprintf(name, sizeof name, "%s", fn);
        FILE *f = fopen(name, "w"); if (f) S.out = f; else fprintf(stderr, "memsample: cannot open %s, using stderr\n", name);
    }
    if (pthread_create(&S.th, 0, sampler, 0)) { fprintf(stderr, "memsample: cannot start the thread\n"); return; }
    S.on = 1;
    static int reg = 0; if (!reg) { atexit(mem_sampler_stop); reg = 1; }
}
