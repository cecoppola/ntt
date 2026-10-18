/* comm_trace.h - COMM_TRACE=1 (default off): the per-message trace of the SHMEM / libfabric / proxy exchanges (included by comm_shmem.c only; all static).
 *
 * What it records.  Fixed 40-byte binary records (struct tr_rec) written by whichever thread runs the step, into chunks claimed from one preallocated
 * per-process arena (mmap, zero pages untouched until used): no lock, no malloc, one atomic add per 4096 records.  Each record is a stamp
 * (CLOCK_MONOTONIC ns) of one stage of an exchange, of a round, or of ONE message (sender -> receiver of one pair in one round), or -- in the proxy
 * processes (COMM_SHMEM_PE_PER_APU=1) -- of one command of the proxy's ring.  The arena is written to one file per process at exit
 * (COMM_TRACE_DIR, default /tmp/ecalc_trace_<job>_<step>/trace_pe<PE>.bin): a 64-byte header, a JSON text (node, PE, switches, the communicator table,
 * the clock-offset estimates, the event names, drop counts), then the records.  tools/trace_report.py reads them.
 *
 * Message identity: (comm id, data sender rank, data receiver rank, pair sequence, round).  The pair sequence is the sequence word the offset word and
 * the signal word carry, equal at both ends, so a sender's and a receiver's records of one message meet without any shared state.  xseq is the exchange's
 * sequence on its communicator (the same on every member); the sampling (COMM_TRACE_SAMPLE=N: about every Nth exchange of a communicator, chosen by a
 * hash of xseq, so every member keeps the same ones) is decided at the exchange's post.
 *
 * Node heartbeat (COMM_TRACE_HB=<ms>, tr_hb_start): a thread per process wakes every <ms>; a wake-up later than COMM_TRACE_HB_GAP_MS (default 20) after the previous one stamps H_GAP and a snapshot of cheap OS
 * counters (/proc/vmstat compaction / THP / major faults / allocstall / swap / numa, /proc/pressure (cpu, memory, io), the thread's own context switches and faults), and every 1000 ms a periodic snapshot (so a stall can be
 * correlated even when the heartbeat thread itself stalled).  COMM_TRACE_HB_PX=1 also starts it in the proxy processes.  Nothing is pinned; the cost is one wake-up per <ms> (see ecalc/README.md).
 *
 * Off: g_tr.on is 0 and every site is one predictable branch on it.  Switches: COMM_TRACE, COMM_TRACE_SAMPLE, COMM_TRACE_MAX_MB (node-process arena,
 * default 256; a proxy gets a quarter, at least 16), COMM_TRACE_DIR, COMM_TRACE_HB, COMM_TRACE_HB_GAP_MS, COMM_TRACE_HB_PX, COMM_TRACE_CLOCK_N (ping-pong iterations of the clock estimate, default 50). */
#ifndef EC_COMM_TRACE_H
#define EC_COMM_TRACE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1                               /* (Linux; needs _GNU_SOURCE in the headers) */
#endif

/* event ids start at 1 (0 = an unused record slot).  E_: the exchange (peer -1); R_: a round of the rounds path (round = j); M_: one message (me = the rank
 * that stamps it, peer = the other end); P_: a proxy command (me = node, peer = destination node, aux = ring index k); H_: the heartbeat thread (COMM_TRACE_HB): H_GAP = one observed wake-up gap (bytes = gap ns, aux = snapshot id),
 * H_CNT = one OS counter of a snapshot (aux = snapshot id, round = counter index into the JSON "hbnames", bytes = its value, seq = 0 snapshot taken at a gap, 1 the periodic one) */
#define TR_EVENTS(X) \
    X(E_BEGIN) X(E_COPY_ISS) X(E_COPY_DONE) X(E_PUBLISHED) X(E_POSTED) X(E_WAIT) X(E_JOINED) X(E_SIGS_SEEN) X(E_QUIET_END) X(E_RCOPY_ISS) X(E_RCOPY_DONE) X(E_DONE) \
    X(R_BEGIN) X(R_PUBLISHED) X(R_SCOPY_ISS) X(R_SCOPY_DONE) X(R_SENDS_DONE) X(R_SIGS_DONE) X(R_QUIET_END) X(R_RCOPY_ISS) X(R_END) \
    X(M_ROFF_ISSUE) X(M_ROFF_WAIT) X(M_ROFF_SEEN) X(M_PUT_ISSUE) X(M_PUT_POSTED) X(M_PUT_DONE) X(M_SIG_ISSUE) X(M_SIG_WAIT) X(M_SIG_SEEN) X(M_FLUSH_BEG) X(M_FLUSH_END) \
    X(P_PICKUP) X(P_BATCH) X(P_CHUNK) X(P_SIGNAL) X(P_QUIET_BEG) X(P_QUIET_END) X(P_DONE) \
    X(H_GAP) X(H_CNT)
enum {
    TR_NONE = 0,
#define X(n) n,
    TR_EVENTS(X)
#undef X
    TR_NEV
};
static const char *const tr_ev_name[TR_NEV] = { "none",
#define X(n) #n,
    TR_EVENTS(X)
#undef X
};

#define TR_CHUNK_RECS 4096
/* t: CLOCK_MONOTONIC ns of this process's node; bytes: payload of the stage; comm: communicator id; seq: pair sequence (M_), exchange sequence (E_ R_);
 * xseq: exchange sequence; aux: proxy ring index k (M_PUT_POSTED, P_), the sender count (E_BEGIN: rounds K); round; ev; kp = xtag kind | path << 4
 * (path 0 equal, 1 v, 2 rounds); me, peer: ranks in the communicator (P_: nodes) */
struct tr_rec { uint64_t t, bytes; uint32_t comm, seq, xseq, aux; uint16_t round; uint8_t ev, kp; int16_t me, peer; };
_Static_assert(sizeof(struct tr_rec) == 40, "trace record");
typedef struct tr_buf { uint32_t chunk, pos; } tr_buf;   /* chunk 0: none yet; UINT32_MAX: the arena is full */
#define TR_MAXCOMM 4096
#define TR_MAXNODE 256
struct tr_comm { int id, n, rank, apu, *node; };
struct tr_clk { int64_t off_m, off_r, rtt, rtt_med, t0; int ok; };   /* node j's clock minus node 0's: monotonic, realtime (ns), the best round trip, the median one, node 0's monotonic time of the best */
static struct {
    int on; uint32_t sample, clock_n; size_t mb;
    char *arena; uint32_t nchunks; uint32_t next; uint64_t dropped;
    char dir[256];
    pthread_mutex_t lock; struct tr_comm comm[TR_MAXCOMM]; int ncomm;
    uint64_t m0, r0;                                      /* this process's CLOCK_MONOTONIC and CLOCK_REALTIME ns read together at init */
    struct tr_clk clk[2][TR_MAXNODE]; int nclk;           /* node 0 only: [pass 0 init, 1 finalize][node] */
    int flushed;
} g_tr = { .lock = PTHREAD_MUTEX_INITIALIZER };

static inline uint64_t tr_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static inline uint64_t tr_now_real(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

/* the arena (COMM_TRACE_MAX_MB / div MiB; a proxy passes 4); on = 0 when COMM_TRACE is off or the mapping fails */
static void tr_init(size_t div)
{
    const char *e = getenv("COMM_TRACE"); if (!e || atoi(e) <= 0) return;
    const char *s = getenv("COMM_TRACE_SAMPLE"); g_tr.sample = s && atoi(s) > 1 ? (uint32_t)atoi(s) : 1;
    const char *c = getenv("COMM_TRACE_CLOCK_N"); g_tr.clock_n = c && atoi(c) > 0 ? (uint32_t)atoi(c) : 50;
    const char *m = getenv("COMM_TRACE_MAX_MB"); size_t want = m && atol(m) > 0 ? (size_t)atol(m) : 256; if (div) want = want / div; if (want < 16) want = 16;
    g_tr.mb = want; size_t bytes = want << 20; g_tr.nchunks = (uint32_t)(bytes / (TR_CHUNK_RECS * sizeof(struct tr_rec)));
    void *a = mmap(0, (size_t)g_tr.nchunks * TR_CHUNK_RECS * sizeof(struct tr_rec), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (a == MAP_FAILED) { fprintf(stderr, "comm_trace: cannot map %zu MiB: COMM_TRACE off\n", want); return; }
    g_tr.arena = (char *)a; g_tr.m0 = tr_now(); g_tr.r0 = tr_now_real();
    const char *d = getenv("COMM_TRACE_DIR");
    if (d && *d) snprintf(g_tr.dir, sizeof g_tr.dir, "%s", d);
    else snprintf(g_tr.dir, sizeof g_tr.dir, "/tmp/ecalc_trace_%s_%s", getenv("SLURM_JOB_ID") ? getenv("SLURM_JOB_ID") : "0", getenv("SLURM_STEP_ID") ? getenv("SLURM_STEP_ID") : "0");
    g_tr.on = 1;
}
static inline void tr_put(tr_buf *b, uint8_t ev, uint8_t kp, uint32_t comm, uint32_t seq, uint32_t xseq, uint32_t aux, uint32_t round, int me, int peer, uint64_t bytes)
{
    uint64_t t = tr_now();
    if (b->chunk == UINT32_MAX) { __atomic_fetch_add(&g_tr.dropped, 1, __ATOMIC_RELAXED); return; }
    if (b->chunk == 0 || b->pos >= TR_CHUNK_RECS) {
        uint32_t c = __atomic_fetch_add(&g_tr.next, 1, __ATOMIC_RELAXED) + 1;
        if (c > g_tr.nchunks) { b->chunk = UINT32_MAX; __atomic_fetch_add(&g_tr.dropped, 1, __ATOMIC_RELAXED); return; }
        b->chunk = c; b->pos = 0;
    }
    struct tr_rec *r = (struct tr_rec *)g_tr.arena + (size_t)(b->chunk - 1) * TR_CHUNK_RECS + b->pos++;
    r->t = t; r->bytes = bytes; r->comm = comm; r->seq = seq; r->xseq = xseq; r->aux = aux; r->round = round > 65535 ? 65535 : (uint16_t)round; r->kp = kp; r->me = (int16_t)me; r->peer = (int16_t)peer;
    __atomic_store_n(&r->ev, ev, __ATOMIC_RELEASE);
}
/* the communicators that traced an exchange (the table in the JSON): node[r] = node of rank r */
static void tr_comm_add(int id, int n, int rank, int apu, const int *node)
{
    pthread_mutex_lock(&g_tr.lock);
    if (g_tr.ncomm < TR_MAXCOMM) { struct tr_comm *c = &g_tr.comm[g_tr.ncomm++]; c->id = id; c->n = n; c->rank = rank; c->apu = apu; c->node = (int *)malloc((size_t)n * sizeof(int)); memcpy(c->node, node, (size_t)n * sizeof(int)); }
    pthread_mutex_unlock(&g_tr.lock);
}
/* one record of the clock table at node 0 after a ping-pong series (see comm_shmem.c tr_clock_sync) */
static int tr_cmp_i64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
/* records of the arena in chunk order (used prefix of each chunk); returns the count */
static uint64_t tr_count(void)
{
    uint64_t n = 0; uint32_t nc = g_tr.next < g_tr.nchunks ? g_tr.next : g_tr.nchunks;
    for (uint32_t c = 0; c < nc; c++) { const struct tr_rec *r = (const struct tr_rec *)g_tr.arena + (size_t)c * TR_CHUNK_RECS; uint32_t k = 0; while (k < TR_CHUNK_RECS && r[k].ev) k++; n += k; }
    return n;
}
static uint64_t tr_write_records(FILE *f)
{
    uint64_t n = 0; uint32_t nc = g_tr.next < g_tr.nchunks ? g_tr.next : g_tr.nchunks;
    for (uint32_t c = 0; c < nc; c++) { const struct tr_rec *r = (const struct tr_rec *)g_tr.arena + (size_t)c * TR_CHUNK_RECS; uint32_t k = 0; while (k < TR_CHUNK_RECS && r[k].ev) k++;
        if (k) { if (fwrite(r, sizeof *r, k, f) != k) break; n += k; } }
    return n;
}
/* names of the events as a JSON array */
static void tr_json_events(FILE *f)
{
    fputc('[', f); for (int i = 1; i < TR_NEV; i++) fprintf(f, "%s\"%s\"", i > 1 ? "," : "", tr_ev_name[i]); fputc(']', f);
}
/* the file: 64-byte header (magic "ECTRACE1", u32 record bytes, u32 JSON bytes, u64 JSON offset, u64 record offset, u64 records, u64 dropped), the JSON text (padded to 8), the records.
 * Returns the records written, -1 when the file cannot be written. */
static long tr_write_file(const char *path, const char *json, size_t jlen)
{
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    char hd[64]; memset(hd, 0, sizeof hd); fwrite(hd, 1, sizeof hd, f);
    fwrite(json, 1, jlen, f); size_t pad = (8 - jlen % 8) % 8; for (size_t i = 0; i < pad; i++) fputc(' ', f);
    uint64_t ro = 64 + jlen + pad, jo = 64, w = tr_write_records(f), dr = g_tr.dropped; uint32_t rb = (uint32_t)sizeof(struct tr_rec), jl = (uint32_t)(jlen + pad);
    memcpy(hd, "ECTRACE1", 8); memcpy(hd + 8, &rb, 4); memcpy(hd + 12, &jl, 4); memcpy(hd + 16, &jo, 8); memcpy(hd + 24, &ro, 8); memcpy(hd + 32, &w, 8); memcpy(hd + 40, &dr, 8);
    fseek(f, 0, SEEK_SET); fwrite(hd, 1, sizeof hd, f);
    int bad = ferror(f); if (fclose(f)) bad = 1;
    return bad ? -1 : (long)w;
}

/* ---- the node heartbeat (COMM_TRACE_HB) ---- */
#define TR_HB_MAXC 96
static struct {
    pthread_t th; int run, started; uint32_t ms, gap_ms, snap; tr_buf b;
    char names[TR_HB_MAXC][40]; int nn; int pfd[3]; char buf[65536];
} g_hb;
static int tr_hb_idx(const char *nm)                      /* the index of a counter name (assigned at first sight; the JSON "hbnames" lists them) */
{
    for (int i = 0; i < g_hb.nn; i++) if (!strcmp(g_hb.names[i], nm)) return i;
    if (g_hb.nn >= TR_HB_MAXC) return -1;
    snprintf(g_hb.names[g_hb.nn], sizeof g_hb.names[0], "%s", nm); return g_hb.nn++;
}
static void tr_hb_val(uint32_t snap, int kind, const char *nm, uint64_t v)
{
    int i = tr_hb_idx(nm); if (i < 0) return;
    tr_put(&g_hb.b, H_CNT, 0, 0, (uint32_t)kind, 0, snap, (uint32_t)i, -1, -1, v);
}
static int tr_hb_slurp(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, buf, cap - 1); close(fd); if (n < 0) n = 0; buf[n] = 0; return (int)n;
}
/* one snapshot of the counters, as H_CNT records of snapshot id snap */
static void tr_hb_snap(uint32_t snap, int kind)
{
    static const char *const pref[] = { "compact_", "thp_fault", "thp_collapse", "pgmajfault", "allocstall", "pswpin", "pswpout", "numa_", "pgscan_direct", "pgsteal_direct", "pgmigrate_" };
    if (tr_hb_slurp("/proc/vmstat", g_hb.buf, sizeof g_hb.buf)) {
        for (char *l = g_hb.buf; l && *l; ) {
            char *e = strchr(l, '\n'); if (e) *e = 0;
            for (unsigned i = 0; i < sizeof pref / sizeof *pref; i++) if (!strncmp(l, pref[i], strlen(pref[i]))) {
                char *sp = strchr(l, ' '); if (sp) { *sp = 0; tr_hb_val(snap, kind, l, strtoull(sp + 1, 0, 10)); } break; }
            l = e ? e + 1 : 0;
        }
    }
    static const char *const psi[3] = { "/proc/pressure/cpu", "/proc/pressure/memory", "/proc/pressure/io" }, *const pn[3] = { "psi_cpu", "psi_mem", "psi_io" };
    for (int k = 0; k < 3; k++) if (tr_hb_slurp(psi[k], g_hb.buf, sizeof g_hb.buf)) {          /* "some avg10=.. total=N" / "full ..." lines */
        for (char *l = g_hb.buf; l && *l; ) {
            char *e = strchr(l, '\n'); if (e) *e = 0;
            char *t = strstr(l, "total="); if (t && (!strncmp(l, "some", 4) || !strncmp(l, "full", 4))) { char nm[40]; snprintf(nm, sizeof nm, "%s_%.4s_us", pn[k], l); tr_hb_val(snap, kind, nm, strtoull(t + 6, 0, 10)); }
            l = e ? e + 1 : 0;
        }
    }
    struct rusage ru;
    if (getrusage(RUSAGE_THREAD, &ru) == 0) {
        tr_hb_val(snap, kind, "thr_nivcsw", (uint64_t)ru.ru_nivcsw); tr_hb_val(snap, kind, "thr_nvcsw", (uint64_t)ru.ru_nvcsw);
        tr_hb_val(snap, kind, "thr_minflt", (uint64_t)ru.ru_minflt); tr_hb_val(snap, kind, "thr_majflt", (uint64_t)ru.ru_majflt);
    }
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        tr_hb_val(snap, kind, "proc_nivcsw", (uint64_t)ru.ru_nivcsw); tr_hb_val(snap, kind, "proc_majflt", (uint64_t)ru.ru_majflt); tr_hb_val(snap, kind, "proc_minflt", (uint64_t)ru.ru_minflt);
    }
}
static void *tr_hb_main(void *a)
{
    (void)a;
    struct timespec rq = { (time_t)(g_hb.ms / 1000), (long)(g_hb.ms % 1000) * 1000000L };
    uint64_t prev = tr_now(), lastsnap = prev; uint64_t thr = (uint64_t)g_hb.gap_ms * 1000000ull;
    tr_hb_snap(g_hb.snap++, 1);                                                              /* the first periodic snapshot: the baseline */
    while (__atomic_load_n(&g_hb.run, __ATOMIC_ACQUIRE)) {
        nanosleep(&rq, 0);
        uint64_t t = tr_now(), gap = t - prev; prev = t;
        if (gap >= thr) {                                                                     /* a late wake-up: the gap and the counters right after it */
            uint32_t id = g_hb.snap++; tr_put(&g_hb.b, H_GAP, 0, 0, 0, 0, id, 0, -1, -1, gap); tr_hb_snap(id, 0); }
        if (t - lastsnap >= 1000000000ull) { lastsnap = t; tr_hb_snap(g_hb.snap++, 1); }
    }
    return 0;
}
/* COMM_TRACE_HB=<ms> (0 / unset: off); proxy = 1 from a proxy process (needs COMM_TRACE_HB_PX=1) */
static void tr_hb_start(int proxy)
{
    if (!g_tr.on || g_hb.started) return;
    const char *e = getenv("COMM_TRACE_HB"); if (!e || atoi(e) <= 0) return;
    if (proxy) { const char *px = getenv("COMM_TRACE_HB_PX"); if (!px || atoi(px) <= 0) return; }
    const char *g = getenv("COMM_TRACE_HB_GAP_MS"); g_hb.ms = (uint32_t)atoi(e); g_hb.gap_ms = g && atoi(g) > 0 ? (uint32_t)atoi(g) : 20;
    g_hb.run = 1;
    if (pthread_create(&g_hb.th, 0, tr_hb_main, 0)) { g_hb.run = 0; return; }
    g_hb.started = 1;
}
static void tr_hb_stop(void)
{
    if (!g_hb.started) return;
    __atomic_store_n(&g_hb.run, 0, __ATOMIC_RELEASE); pthread_join(g_hb.th, 0); g_hb.started = 0;
}
/* the counter names as a JSON array (after tr_hb_stop) */
static void tr_json_hbnames(FILE *f)
{
    fputc('[', f); for (int i = 0; i < g_hb.nn; i++) fprintf(f, "%s\"%s\"", i ? "," : "", g_hb.names[i]); fputc(']', f);
}
#endif
