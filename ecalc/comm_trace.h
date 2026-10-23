/* comm_trace.h - COMM_TRACE=1 (default off): the per-message trace of the SHMEM / libfabric exchanges (included by comm_shmem.c only; all static).
 *
 * What it records.  Fixed 40-byte binary records (struct tr_rec) written by whichever thread runs the step, into chunks claimed from one preallocated
 * per-process arena (mmap, zero pages untouched until used): no lock, no malloc, one atomic add per 4096 records.  Each record is a stamp
 * (CLOCK_MONOTONIC ns) of one stage of an exchange, of a round, or of ONE message (sender -> receiver of one pair in one round).  The arena is written to one file per process at exit
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
 * correlated even when the heartbeat thread itself stalled).  Nothing is pinned; the cost is one wake-up per <ms> (see ecalc/README.md).
 *
 * Off: g_tr.on is 0 and every site is one predictable branch on it.  Switches: COMM_TRACE, COMM_TRACE_SAMPLE, COMM_TRACE_MAX_MB (arena MiB,
 * default 256, at least 16), COMM_TRACE_DIR, COMM_TRACE_HB, COMM_TRACE_HB_GAP_MS, COMM_TRACE_HB_MEM, COMM_TRACE_CLOCK_N (ping-pong iterations of the clock estimate, default 50). */
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
#include <dirent.h>
#include <sys/syscall.h>
#include "comm_phase.h"
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1                               /* (Linux; needs _GNU_SOURCE in the headers) */
#endif

/* event ids start at 1 (0 = an unused record slot).  E_: the exchange (peer -1); R_: a round of the rounds path (round = j); M_: one message (me = the rank
 * that stamps it, peer = the other end); H_: the heartbeat thread (COMM_TRACE_HB): H_GAP = one observed wake-up gap (bytes = gap ns, aux = snapshot id),
 * H_CNT = one OS counter of a snapshot (aux = snapshot id, round = counter index into the JSON "hbnames", bytes = its value, seq = 0 snapshot taken at a gap, 1 the periodic one, 2 a watchdog one),
 * H_PHASE = a thread changed its phase tag (comm_phase.h; comm = tid, aux = new tag, bytes = old tag, me = thread slot), H_THR = one thread of a snapshot (comm = snapshot id, seq = tid, xseq = wchan index and
 * peer = thread name index into the JSON "hbstrs", me = last CPU, round = phase tag, aux = ms in that phase, kp = snapshot kind | 128 for the heartbeat thread itself, bytes = run_delay ns | state char << 56),
 * H_WD = the progress watchdog fired (bytes = ns since the last exchange / round / message record, aux = snapshot id) */
#define TR_EVENTS(X) \
    X(E_BEGIN) X(E_COPY_ISS) X(E_COPY_DONE) X(E_PUBLISHED) X(E_POSTED) X(E_WAIT) X(E_JOINED) X(E_SIGS_SEEN) X(E_QUIET_END) X(E_RCOPY_ISS) X(E_RCOPY_DONE) X(E_DONE) \
    X(R_BEGIN) X(R_PUBLISHED) X(R_SCOPY_ISS) X(R_SCOPY_DONE) X(R_SENDS_DONE) X(R_SIGS_DONE) X(R_QUIET_END) X(R_RCOPY_ISS) X(R_END) \
    X(M_ROFF_ISSUE) X(M_ROFF_WAIT) X(M_ROFF_SEEN) X(M_PUT_ISSUE) X(M_PUT_POSTED) X(M_PUT_DONE) X(M_SIG_ISSUE) X(M_SIG_WAIT) X(M_SIG_SEEN) X(M_FLUSH_BEG) X(M_FLUSH_END) \
    X(H_GAP) X(H_CNT) X(H_PHASE) X(H_THR) X(H_WD)
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
 * xseq: exchange sequence; aux: the sender count (E_BEGIN: rounds K); round; ev; kp = xtag kind | path << 4
 * (path 0 equal, 1 v, 2 rounds); me, peer: ranks in the communicator */
struct tr_rec { uint64_t t, bytes; uint32_t comm, seq, xseq, aux; uint16_t round; uint8_t ev, kp; int16_t me, peer; };
_Static_assert(sizeof(struct tr_rec) == 40, "trace record");
typedef struct tr_buf { uint32_t chunk, pos; } tr_buf;   /* chunk 0: none yet; UINT32_MAX: the arena is full */
#define TR_MAXCOMM 4096
#define TR_MAXNODE 256
/* apu: the device current at the communicator's first exchange (JSON version 2) */
struct tr_comm { int id, n, rank, apu, *node; };
struct tr_clk { int64_t off_m, off_r, rtt, rtt_med, t0; int ok; };   /* node j's clock minus node 0's: monotonic, realtime (ns), the best round trip, the median one, node 0's monotonic time of the best */
static struct {
    int on; uint32_t sample, clock_n; size_t mb;
    char *arena; uint32_t nchunks; uint32_t next; uint64_t dropped;
    char dir[256];
    pthread_mutex_t lock; struct tr_comm comm[TR_MAXCOMM]; int ncomm;
    uint32_t wd_ms; uint64_t m0, r0;                                      /* this process's CLOCK_MONOTONIC and CLOCK_REALTIME ns read together at init */
    struct tr_clk clk[2][TR_MAXNODE]; int nclk;           /* node 0 only: [pass 0 init, 1 finalize][node] */
    int flushed;
} g_tr = { .lock = PTHREAD_MUTEX_INITIALIZER };

static inline uint64_t tr_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static inline uint64_t tr_now_real(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static void tr_ph_init(void);
/* the arena (COMM_TRACE_MAX_MB MiB); on = 0 when COMM_TRACE is off or the mapping fails */
static void tr_init(void)
{
    const char *e = getenv("COMM_TRACE"); if (!e || atoi(e) <= 0) return;
    const char *s = getenv("COMM_TRACE_SAMPLE"); g_tr.sample = s && atoi(s) > 1 ? (uint32_t)atoi(s) : 1;
    const char *c = getenv("COMM_TRACE_CLOCK_N"); g_tr.clock_n = c && atoi(c) > 0 ? (uint32_t)atoi(c) : 50;
    const char *m = getenv("COMM_TRACE_MAX_MB"); size_t want = m && atol(m) > 0 ? (size_t)atol(m) : 256; if (want < 16) want = 16;
    g_tr.mb = want; size_t bytes = want << 20; g_tr.nchunks = (uint32_t)(bytes / (TR_CHUNK_RECS * sizeof(struct tr_rec)));
    void *a = mmap(0, (size_t)g_tr.nchunks * TR_CHUNK_RECS * sizeof(struct tr_rec), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (a == MAP_FAILED) { fprintf(stderr, "comm_trace: cannot map %zu MiB: COMM_TRACE off\n", want); return; }
    g_tr.arena = (char *)a; g_tr.m0 = tr_now(); g_tr.r0 = tr_now_real();
    const char *d = getenv("COMM_TRACE_DIR");
    if (d && *d) snprintf(g_tr.dir, sizeof g_tr.dir, "%s", d);
    else snprintf(g_tr.dir, sizeof g_tr.dir, "/tmp/ecalc_trace_%s_%s", getenv("SLURM_JOB_ID") ? getenv("SLURM_JOB_ID") : "0", getenv("SLURM_STEP_ID") ? getenv("SLURM_STEP_ID") : "0");
    { const char *w = getenv("COMM_TRACE_WD_MS"); g_tr.wd_ms = w && atoi(w) > 0 ? (uint32_t)atoi(w) : 0; }
    g_tr.on = 1; tr_ph_init();
}
static struct { _Alignas(64) uint64_t last; char pad[56]; } g_wdl;   /* the watchdog's progress stamp (own cache line; written only when COMM_TRACE_WD_MS is set) */
static inline void tr_put(tr_buf *b, uint8_t ev, uint8_t kp, uint32_t comm, uint32_t seq, uint32_t xseq, uint32_t aux, uint32_t round, int me, int peer, uint64_t bytes)
{
    uint64_t t = tr_now();
    if (g_tr.wd_ms && ev < H_GAP) __atomic_store_n(&g_wdl.last, t, __ATOMIC_RELAXED);
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

/* ---- phase tags (comm_phase.h): COMM_TRACE_PH = 1 words + H_PHASE events, 2 words only; the watchdog (COMM_TRACE_WD_MS) and COMM_TRACE_THR also turn the words on ---- */
#define TR_PH_SLOTS 64
struct tr_slot { int tid, noev; uint32_t ph; uint64_t since; tr_buf b; };
static struct { int ev; uint32_t n; struct tr_slot s[TR_PH_SLOTS + 1]; } g_ph;
static __thread struct tr_slot *tr_my_slot;
static const char *const tr_ph_name[EC_PH_N] = {
#define X(n) #n,
    EC_PH_LIST(X)
#undef X
};
static int tr_ph_set(int id)
{
    struct tr_slot *s = tr_my_slot;
    if (!s) {
        uint32_t i = __atomic_fetch_add(&g_ph.n, 1, __ATOMIC_RELAXED);
        s = &g_ph.s[i < TR_PH_SLOTS ? i : TR_PH_SLOTS]; if (i >= TR_PH_SLOTS) s->noev = 1;      /* (the spare slot is shared: no events, the word is approximate) */
        else s->tid = (int)syscall(SYS_gettid);
        tr_my_slot = s;
    }
    uint32_t old = __atomic_load_n(&s->ph, __ATOMIC_RELAXED); if (old == (uint32_t)id) return (int)old;
    __atomic_store_n(&s->since, tr_now(), __ATOMIC_RELAXED); __atomic_store_n(&s->ph, (uint32_t)id, __ATOMIC_RELAXED);
    if (g_ph.ev && !s->noev) tr_put(&s->b, H_PHASE, 0, (uint32_t)s->tid, 0, 0, (uint32_t)id, 0, (int)(s - g_ph.s), -1, old);
    return (int)old;
}
static void tr_ph_init(void)
{
    const char *e = getenv("COMM_TRACE_PH"), *t = getenv("COMM_TRACE_THR"); int m = e ? atoi(e) : 0;
    g_ph.ev = m == 1;
    if (m > 0 || g_tr.wd_ms || (t && atoi(t) > 0)) { ec_ph_fn = tr_ph_set; __atomic_store_n(&ec_ph_on, 1, __ATOMIC_RELEASE); }
}
static void tr_json_phases(FILE *f)
{
    fputc('[', f); for (int i = 0; i < EC_PH_N; i++) fprintf(f, "%s\"%s\"", i ? "," : "", tr_ph_name[i]); fputc(']', f);
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
#define TR_HB_MAXC 640
static struct {
    pthread_t th; int run, started; uint32_t ms, gap_ms, snap; int thr, mem; tr_buf b;
    char names[TR_HB_MAXC][40]; int nn; int pfd[3]; char buf[65536];
    int kt_n; int kt_pid[16]; char kt_nm[16][24];                          /* COMM_TRACE_HB_MEM: kcompactd<N> / khugepaged pids */
    int kfd_n, kfd_tried; char kfd_path[24][192], kfd_nm[24][48];          /* its KFD eviction counters (found at the first snapshot that sees them) */
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
/* COMM_TRACE_HB_MEM=1 (needs COMM_TRACE_HB): memory-side counters of a compaction storm -- buddyinfo per NUMA node and zone (orders 0-10 of Normal, free pages below / from order 9 and the
 * fragmentation score 100*lo/(lo+hi) of every zone), selected meminfo keys (kB), the CPU ticks of the kcompactd / khugepaged kernel threads, the amdgpu KFD eviction counters of this
 * process (kfd_present = 0 / 1; the files are only read when /sys/class/kfd/kfd/proc/<pid>/stats_<gpuid>/ exists) and the process context switches. */
static void tr_hb_mem_start(void)
{
    DIR *d = opendir("/proc"); if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g_hb.kt_n < 16) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        char path[64], buf[64]; snprintf(path, sizeof path, "/proc/%s/comm", de->d_name);
        int n = tr_hb_slurp(path, buf, sizeof buf); if (n <= 0) continue;
        if (buf[n - 1] == '\n') buf[n - 1] = 0;
        if (!strncmp(buf, "kcompactd", 9) || !strcmp(buf, "khugepaged")) { g_hb.kt_pid[g_hb.kt_n] = atoi(de->d_name); snprintf(g_hb.kt_nm[g_hb.kt_n], sizeof g_hb.kt_nm[0], "%s", buf); g_hb.kt_n++; }
    }
    closedir(d);
}
static void tr_hb_kfd_find(void)                                   /* the stats_<gpuid> directories of this process; once found they stay */
{
    char dir[96]; snprintf(dir, sizeof dir, "/sys/class/kfd/kfd/proc/%d", (int)getpid());
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "stats_", 6)) continue;
        char sd[160]; if (snprintf(sd, sizeof sd, "%s/%s", dir, de->d_name) >= (int)sizeof sd) continue;
        DIR *d2 = opendir(sd); if (!d2) continue;
        struct dirent *e2;
        while ((e2 = readdir(d2)) && g_hb.kfd_n < 24) {
            if (e2->d_name[0] == '.') continue;
            if (snprintf(g_hb.kfd_path[g_hb.kfd_n], sizeof g_hb.kfd_path[0], "%s/%s", sd, e2->d_name) >= (int)sizeof g_hb.kfd_path[0]) continue;
            snprintf(g_hb.kfd_nm[g_hb.kfd_n], 40, "kfd_%.14s_%.20s", de->d_name + 6, e2->d_name); g_hb.kfd_n++;
        }
        closedir(d2);
    }
    closedir(d);
}
static void tr_hb_mem_snap(uint32_t snap, int kind)
{
    char nm[48];
    if (tr_hb_slurp("/proc/buddyinfo", g_hb.buf, sizeof g_hb.buf)) {         /* "Node 0, zone   Normal  n0 n1 ... n10" */
        for (char *l = g_hb.buf; l && *l; ) {
            char *e = strchr(l, '\n'); if (e) *e = 0;
            int node = -1, off = 0; char zone[16] = "";
            if (sscanf(l, "Node %d, zone %15s%n", &node, zone, &off) >= 2 && node >= 0) {
                unsigned long long c[11] = { 0 }, lo = 0, hi = 0; int k = 0; char *q = l + off;
                while (k < 11) { char *en; unsigned long long v = strtoull(q, &en, 10); if (en == q) break; c[k++] = v; q = en; }
                for (int o = 0; o < k; o++) { unsigned long long pg = c[o] << o; if (o < 9) lo += pg; else hi += pg; }
                if (!strcmp(zone, "Normal")) for (int o = 0; o < k; o++) { snprintf(nm, sizeof nm, "bud_n%d_o%d", node, o); tr_hb_val(snap, kind, nm, c[o]); }
                snprintf(nm, sizeof nm, "bud_n%d_%.6s_lo", node, zone); tr_hb_val(snap, kind, nm, lo);
                snprintf(nm, sizeof nm, "bud_n%d_%.6s_hi", node, zone); tr_hb_val(snap, kind, nm, hi);
                snprintf(nm, sizeof nm, "bud_n%d_%.6s_frag", node, zone); tr_hb_val(snap, kind, nm, lo + hi ? 100 * lo / (lo + hi) : 0);
            }
            l = e ? e + 1 : 0;
        }
    }
    static const char *const mk[] = { "MemFree:", "Dirty:", "Writeback:", "Mapped:", "Shmem:", "AnonPages:", "AnonHugePages:", "ShmemHugePages:", "Unevictable:", "Mlocked:", "KReclaimable:" };
    if (tr_hb_slurp("/proc/meminfo", g_hb.buf, sizeof g_hb.buf)) {
        for (char *l = g_hb.buf; l && *l; ) {
            char *e = strchr(l, '\n'); if (e) *e = 0;
            for (unsigned i = 0; i < sizeof mk / sizeof *mk; i++) if (!strncmp(l, mk[i], strlen(mk[i]))) {
                snprintf(nm, sizeof nm, "mi_%.*s_kb", (int)strlen(mk[i]) - 1, mk[i]); tr_hb_val(snap, kind, nm, strtoull(l + strlen(mk[i]), 0, 10)); break; }
            l = e ? e + 1 : 0;
        }
    }
    for (int i = 0; i < g_hb.kt_n; i++) {                              /* utime + stime: fields 14 and 15 of stat */
        char path[48]; snprintf(path, sizeof path, "/proc/%d/stat", g_hb.kt_pid[i]);
        if (!tr_hb_slurp(path, g_hb.buf, 4096)) continue;
        char *q = strrchr(g_hb.buf, ')'); if (!q) continue;
        unsigned long long ut = 0, st = 0; if (sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        snprintf(nm, sizeof nm, "%.20s_ticks", g_hb.kt_nm[i]); tr_hb_val(snap, kind, nm, ut + st);
    }
    if (!g_hb.kfd_n && g_hb.kfd_tried < 3600) { g_hb.kfd_tried++; tr_hb_kfd_find(); }
    tr_hb_val(snap, kind, "kfd_present", g_hb.kfd_n > 0);
    for (int i = 0; i < g_hb.kfd_n; i++) if (tr_hb_slurp(g_hb.kfd_path[i], g_hb.buf, 256)) tr_hb_val(snap, kind, g_hb.kfd_nm[i], strtoull(g_hb.buf, 0, 10));
    if (!tr_hb_slurp("/proc/self/status", g_hb.buf, sizeof g_hb.buf)) return;   /* "voluntary_ctxt_switches:\tN" */
    char *v = strstr(g_hb.buf, "\nvoluntary_ctxt_switches:"); if (v) tr_hb_val(snap, kind, "proc_nvcsw", strtoull(v + 25, 0, 10));
    v = strstr(g_hb.buf, "\nnonvoluntary_ctxt_switches:"); if (v) tr_hb_val(snap, kind, "proc_nivcsw_st", strtoull(v + 28, 0, 10));
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
    if (g_hb.mem) tr_hb_mem_snap(snap, kind);
}
/* per-thread snapshot (heartbeat v2, COMM_TRACE_THR=1; every watchdog firing): for every thread of the process the state, wchan, run_delay (schedstat), last CPU and phase tag as an H_THR
 * record, then the irq / softirq / steal ticks of /proc/stat for the CPUs those threads last ran on (H_CNT "cpuN_irq" ...) */
#define TR_HB_STRS 96
static char g_hb_strs[TR_HB_STRS][24]; static int g_hb_nstr; static int g_hb_tid;
static int tr_hb_str(const char *s)
{
    for (int i = 0; i < g_hb_nstr; i++) if (!strncmp(g_hb_strs[i], s, 23)) return i;
    if (g_hb_nstr >= TR_HB_STRS) return TR_HB_STRS - 1;
    snprintf(g_hb_strs[g_hb_nstr], sizeof g_hb_strs[0], "%s", s); return g_hb_nstr++;
}
static void tr_json_hbstrs(FILE *f)
{
    fputc('[', f); for (int i = 0; i < g_hb_nstr; i++) fprintf(f, "%s\"%s\"", i ? "," : "", g_hb_strs[i]); fputc(']', f);
}
static void tr_thr_snap(uint32_t snap, int kind)
{
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    static unsigned char cpus[1024 / 8]; memset(cpus, 0, sizeof cpus); uint64_t now = tr_now(); char path[64], buf[1024];
    struct dirent *de;
    while ((de = readdir(d))) {
        int tid = atoi(de->d_name); if (tid <= 0) continue;
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        int n = tr_hb_slurp(path, buf, sizeof buf); if (n <= 0) continue;
        char *cp = strrchr(buf, ')'); if (!cp || !cp[1]) continue;
        char *nm = strchr(buf, '('); char comm[24] = "?"; if (nm && nm < cp) { size_t l = (size_t)(cp - nm - 1); if (l > 23) l = 23; memcpy(comm, nm + 1, l); comm[l] = 0; }
        char *p = cp + 2; char state = *p; int cpu = -1;
        for (int f = 3; f < 39 && p; f++) { p = strchr(p, ' '); if (p) p++; }                         /* field 39: the processor last run on */
        if (p) cpu = atoi(p);
        if (cpu >= 0 && cpu < 1024) cpus[cpu >> 3] |= (unsigned char)(1u << (cpu & 7));
        uint64_t delay = 0;
        snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", tid);
        if (tr_hb_slurp(path, buf, sizeof buf) > 0) { char *sp = strchr(buf, ' '); if (sp) delay = strtoull(sp + 1, 0, 10); }
        char wc[24] = "-"; snprintf(path, sizeof path, "/proc/self/task/%d/wchan", tid);
        { int w = tr_hb_slurp(path, buf, sizeof buf); if (w > 0) { if (buf[w - 1] == '\n') buf[w - 1] = 0; snprintf(wc, sizeof wc, "%s", buf); } }
        uint32_t ph = 0, age = 0;
        for (uint32_t i = 0; i < g_ph.n && i < TR_PH_SLOTS; i++) if (g_ph.s[i].tid == tid) {
            ph = __atomic_load_n(&g_ph.s[i].ph, __ATOMIC_RELAXED); uint64_t sn = __atomic_load_n(&g_ph.s[i].since, __ATOMIC_RELAXED); age = sn && now > sn ? (uint32_t)((now - sn) / 1000000ull) : 0; break; }
        tr_put(&g_hb.b, H_THR, (uint8_t)(kind | (tid == g_hb_tid ? 128 : 0)), snap, (uint32_t)tid, (uint32_t)tr_hb_str(wc), age, ph, cpu, tr_hb_str(comm), (delay & 0x00ffffffffffffffull) | ((uint64_t)(unsigned char)state << 56));
        (void)n;
    }
    closedir(d);
    if (tr_hb_slurp("/proc/stat", g_hb.buf, sizeof g_hb.buf)) {
        for (char *l = g_hb.buf; l && *l; ) {
            char *e = strchr(l, '\n'); if (e) *e = 0;
            if (!strncmp(l, "cpu", 3) && l[3] >= '0' && l[3] <= '9') {
                int c = atoi(l + 3);
                if (c >= 0 && c < 1024 && (cpus[c >> 3] & (1u << (c & 7)))) {
                    unsigned long long v[8] = { 0 }; char *q = strchr(l, ' '); if (q) sscanf(q, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
                    char nm[40]; snprintf(nm, sizeof nm, "cpu%d_irq", c); tr_hb_val(snap, kind, nm, v[5]);
                    snprintf(nm, sizeof nm, "cpu%d_softirq", c); tr_hb_val(snap, kind, nm, v[6]);
                    snprintf(nm, sizeof nm, "cpu%d_steal", c); tr_hb_val(snap, kind, nm, v[7]);
                }
            }
            l = e ? e + 1 : 0;
        }
    }
}
static void *tr_hb_main(void *a)
{
    (void)a; g_hb_tid = (int)syscall(SYS_gettid);
    struct timespec rq = { (time_t)(g_hb.ms / 1000), (long)(g_hb.ms % 1000) * 1000000L };
    uint64_t prev = tr_now(), lastsnap = prev; uint64_t thr = (uint64_t)g_hb.gap_ms * 1000000ull;
    uint64_t wdn = (uint64_t)g_tr.wd_ms * 1000000ull, wdlast = 0, wdthr = wdn; int wdfires = 0;
    tr_hb_snap(g_hb.snap++, 1);                                                              /* the first periodic snapshot: the baseline */
    if (g_hb.thr) tr_thr_snap(g_hb.snap - 1, 1);
    while (__atomic_load_n(&g_hb.run, __ATOMIC_ACQUIRE)) {
        nanosleep(&rq, 0);
        uint64_t t = tr_now(), gap = t - prev; prev = t;
        if (gap >= thr) {                                                                     /* a late wake-up: the gap and the counters right after it */
            uint32_t id = g_hb.snap++; tr_put(&g_hb.b, H_GAP, 0, 0, 0, 0, id, 0, -1, -1, gap); tr_hb_snap(id, 0); if (g_hb.thr) tr_thr_snap(id, 0); }
        if (wdn) {                                                                            /* the progress watchdog: no exchange / round / message record for wd_ms */
            uint64_t last = __atomic_load_n(&g_wdl.last, __ATOMIC_RELAXED); if (!last) last = g_tr.m0;
            if (last != wdlast) { wdlast = last; wdthr = wdn; }
            else if (t - last >= wdthr && wdfires < 400) {                                    /* once per episode, again at 2x, 4x ... the first threshold */
                uint32_t id = g_hb.snap++; wdfires++; wdthr *= 2;
                tr_put(&g_hb.b, H_WD, 0, 0, 0, 0, id, 0, -1, -1, t - last); tr_hb_snap(id, 2); tr_thr_snap(id, 2); }
        }
        if (t - lastsnap >= 1000000000ull) { lastsnap = t; tr_hb_snap(g_hb.snap++, 1); if (g_hb.thr) tr_thr_snap(g_hb.snap - 1, 1); }
    }
    return 0;
}
/* COMM_TRACE_HB=<ms> (0 / unset: off) */
static void tr_hb_start(void)
{
    if (!g_tr.on || g_hb.started) return;
    const char *e = getenv("COMM_TRACE_HB"), *th = getenv("COMM_TRACE_THR"); int ms = e ? atoi(e) : 0;
    if (ms <= 0 && g_tr.wd_ms) ms = 2;                                                       /* the watchdog needs the thread */
    if (ms <= 0) return;
    g_hb.thr = th && atoi(th) > 0; { const char *me = getenv("COMM_TRACE_HB_MEM"); g_hb.mem = me && atoi(me) > 0; }
    const char *g = getenv("COMM_TRACE_HB_GAP_MS"); g_hb.ms = (uint32_t)ms; g_hb.gap_ms = g && atoi(g) > 0 ? (uint32_t)atoi(g) : 20;
    if (g_hb.mem) tr_hb_mem_start();
    g_hb.run = 1;
    if (pthread_create(&g_hb.th, 0, tr_hb_main, 0)) { g_hb.run = 0; return; }
    g_hb.started = 1;
}
static void tr_hb_stop(void)
{
    if (!g_hb.started) return;
    __atomic_store_n(&g_hb.run, 0, __ATOMIC_RELEASE); pthread_join(g_hb.th, 0); g_hb.started = 0;
    if (g_hb.mem && !g_hb.kfd_n) fprintf(stderr, "COMM_TRACE_HB_MEM: KFD eviction stats absent (kfd_present = 0)\n");
}
/* the counter names as a JSON array (after tr_hb_stop) */
static void tr_json_hbnames(FILE *f)
{
    fputc('[', f); for (int i = 0; i < g_hb.nn; i++) fprintf(f, "%s\"%s\"", i ? "," : "", g_hb.names[i]); fputc(']', f);
}
#endif
