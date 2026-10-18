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
 * Off: g_tr.on is 0 and every site is one predictable branch on it.  Switches: COMM_TRACE, COMM_TRACE_SAMPLE, COMM_TRACE_MAX_MB (node-process arena,
 * default 256; a proxy gets a quarter, at least 16), COMM_TRACE_DIR, COMM_TRACE_CLOCK_N (ping-pong iterations of the clock estimate, default 50). */
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

/* event ids start at 1 (0 = an unused record slot).  E_: the exchange (peer -1); R_: a round of the rounds path (round = j); M_: one message (me = the rank
 * that stamps it, peer = the other end); P_: a proxy command (me = node, peer = destination node, aux = ring index k) */
#define TR_EVENTS(X) \
    X(E_BEGIN) X(E_COPY_ISS) X(E_COPY_DONE) X(E_PUBLISHED) X(E_POSTED) X(E_WAIT) X(E_JOINED) X(E_SIGS_SEEN) X(E_QUIET_END) X(E_RCOPY_ISS) X(E_RCOPY_DONE) X(E_DONE) \
    X(R_BEGIN) X(R_PUBLISHED) X(R_SCOPY_ISS) X(R_SCOPY_DONE) X(R_SENDS_DONE) X(R_SIGS_DONE) X(R_QUIET_END) X(R_RCOPY_ISS) X(R_END) \
    X(M_ROFF_ISSUE) X(M_ROFF_WAIT) X(M_ROFF_SEEN) X(M_PUT_ISSUE) X(M_PUT_POSTED) X(M_PUT_DONE) X(M_SIG_ISSUE) X(M_SIG_WAIT) X(M_SIG_SEEN) X(M_FLUSH_BEG) X(M_FLUSH_END) \
    X(P_PICKUP) X(P_BATCH) X(P_CHUNK) X(P_SIGNAL) X(P_QUIET_BEG) X(P_QUIET_END) X(P_DONE)
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
#endif
