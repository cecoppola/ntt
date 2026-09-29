/* mn_out.h - per-node output and verification (Phase 9 A-out: PLAN.md 19, M5 + C1).
 *
 * X < 10^(d+1) in base 10^18 has nl = ceil((d+1)/18) limbs; the digit string is the limbs written from the top,
 * 18 digits each, the leading pad = 18 nl - (d+1) zeros dropped: digit k (0 = the "2", k = 1..d the fraction) is
 * char pad + k of that string, and limb i holds the chars [18 (nl-1-i), 18 (nl-i)).  The file is "2." + digits
 * 1..d_out + "\n".  A node holding the contiguous limbs [lo, lo+cnt) of X (the share of an mdb, or a range of
 * a host number) holds the digits [k0, k1) with k0 = max(0, 18 (nl-hi) - pad), k1 = 18 (nl-lo) - pad; it
 * formats them in chunks (MN_OUT_CHUNK_MB of digits, 256) from the top down and writes each chunk to its part
 * file as soon as it is formatted, so no node ever holds more than two chunks of the string; the part files
 * are <outfile>.part<k>, k = size-1-rank zero-padded (the top limbs are on the last node: part 0 is the head
 * "2.", the last part carries the newline), so `cat <outfile>.part*` (sorted) is the single-file output.
 * At size 1 the file is <outfile> itself.  Per chunk: the digit residues mod the T1 primes (Horner over the
 * chunks with 10^len, joined across the nodes the same way) and the T2 windows (a window straddling a chunk or
 * a node boundary is checked by the piece it ends in, from the 49 chars before it: the previous chunk's tail,
 * or the tails of the nodes above, all-gathered before the run).  The writes go through a helper thread with
 * two chunk buffers, so a chunk is written while the next one is formatted.
 *
 * Phase 15 IO (results/IO15.md): the write mode ECALC_OUT_MODE (direct, buffered, sync, drop, auto) and MN_OUT_THREADS; the
 * packed form ECALC_OUT_PACKED=1 (the limbs as they are, packed_fmt.h: no ASCII formatting in the run; the T1 digit residues
 * from the packed bytes, the T2 windows from the digits they cover; tools/unpack_digits converts and checks); MN_OUT_STRIPE
 * (a Lustre layout per part file); MN_OUT_WAVES (at most ceil(size/n) nodes write at once).  ECALC_RECHECK reads packed
 * files too. */
#ifndef EC_MN_OUT_H
#define EC_MN_OUT_H
#include <stdint.h>
#include <stddef.h>
#include "verify.h"
#include "dbig.h"
#include "comm.h"
#ifdef __cplusplus
extern "C" {
#endif
/* the limbs this node holds of X: global limbs [lo, lo + cnt), either a host array (host[0..cnt)) or a device
 * number (its limbs [0, cnt); a share of an mdb, dev->n = cnt, or a view) */
typedef struct { const uint64_t *host; const dbig *dev; size_t lo, cnt; } mn_out_src;
typedef struct mn_out_s {
    /* in */
    unsigned long d, d_out; const char *outfile; int rank, size, verbose;
    size_t chunk_limbs;                      /* 0: MN_OUT_CHUNK_MB (256 MB of digits) */
    /* the T2 head: the digits before this node's range (mn_out_boundaries; empty at size 1) */
    char head[64]; size_t nhead;
    /* out */
    size_t k0, k1, ndig;                     /* this node's digit range and its length (= the chars in dres) */
    uint64_t dres[T1_NQ];                    /* the node's digits as a number mod q */
    int bad2, nwin;                          /* T2: failing / checked windows */
    size_t bytes; int nchunks;               /* written to the part file */
    double t_fmt, t_res, t_t2, t_write, t_fetch, t_wait;   /* per part (write: the helper thread's time in write(); wait: the formatter blocked on it) */
    char first[80], last[24];                /* the first 62 and the last 20 digits of the node's range */
    char tail[24]; size_t ntail;             /* Phase 11 V: the d - d_out computed digits after d_out (the node holding limb 0) */
    void *priv;                              /* the writer thread while a write is in flight (mn_out_finish joins it) */
    size_t t2_defer;                         /* Phase 15 K: > 0 -- the T2 windows ending in the last t2_defer digits (>= d_out + 1 - t2_defer) are left to mn_out_tail_fix */
    /* Phase 15 IO */
    comm *c;                                 /* set by mn_out_boundaries: MN_OUT_WAVES's barrier (0: no waves) */
    int packed;                              /* out: the part was written packed (ECALC_OUT_PACKED=1, packed_fmt.h) */
    int wave, nwaves; double t_wave, t_wave_own;   /* out: MN_OUT_WAVES -- this rank's wave, the waves, the seconds waiting for the others / in its own */
    /* Phase 15 EW (MN_OUT_DKM_HI, results/EW15.md 1.3): X = X_hi B^s + X_lo held apart */
    size_t split; const mn_out_src *src_hi;  /* split > 0 (size 1, one file in two ranges): the limbs >= split are read from src_hi (its lo = split), the
                                              * rest from the main source; no chunk crosses split.  src_hi also serves mn_out_tail_fix's gather (both sizes) */
    void (*at_split)(void *arg, struct mn_out_s *o); void *split_arg;   /* with split: called once from the writer when every limb >= split is formatted
                                              * and handed to the writer thread, before any limb below (the gate: it may wait, and may set head / nhead
                                              * when the part had no chunk above split) */
    size_t lim;                              /* > 0 (size > 1, X_lo's layer): the part holds limbs below lim only; the top rank's reaches lim (not nl) */
    int part, nparts;                        /* nparts > 0: this part's index and the count (the file <outfile>.part<part> when nparts > 1); 0: size - 1 - rank of size */
} mn_out;
void mn_out_boundaries(mn_out *o, const mn_out_src *src, comm *c);   /* size > 1: all-gather the nodes' tails (49 digits) -> this node's head */
int  mn_out_run(mn_out *o, const mn_out_src *src);                   /* format, residues, T2, write (streamed); 0 = ok.  Returns with the last chunk's write in flight */
void mn_out_finish(mn_out *o);                                       /* wait for the writes, close the part file */
/* Phase 15 K (ECALC_CORR_PATCH): the division's corrections (X + dx, |dx| <= 64) applied to digits already written from the uncorrected X.
 * Only the tail its carry / borrow reaches changes: the low limbs of X (gathered over the nodes, more until the carry ends inside
 * them) give the old and the new digits of that tail; every node whose part holds some of them checks the bytes on disk are the
 * old ones, rewrites the 4 KiB blocks holding them (O_DIRECT when the writer used it) and reads them back; the digit residues move
 * by new - old of the tail (so digits == X mod q is checked against the patched bytes), the T2 windows ending in the last
 * o->t2_defer digits (skipped by the writer) are checked on the new digits, and last / tail / first follow.  Call it after
 * mn_out_finish, on every node (collective at size > 1), also when dx == 0 (the deferred windows).  Returns this node's
 * failures (the bytes on disk were not the expected ones, an I/O error, or a carry past X's top); f gets the adjustment. */
typedef struct { uint64_t dres_adj[T1_NQ]; size_t kp, w, bytes; int parts, nwin; double t; } mn_out_fix;
int  mn_out_tail_fix(mn_out *o, const mn_out_src *src, comm *c, long dx, mn_out_fix *f);
/* the node-local part: low = the global low limbs [0, w) of the uncorrected X (every node the same); returns -1 (nothing done) when
 * the carry or the windows' context reaches past them -- the caller gathers more -- else this node's failures */
int  mn_out_tail_core(mn_out *o, const uint64_t *low, size_t w, long dx, mn_out_fix *f);
/* Phase 15 IO (W5d): MN_OUT_EARLY=1 at size > 1 -- the part file streamed during the division's low product, as the one-node
 * path does (the division's hook, newton_mn_x_hook, gives X before the low product).  mn_out_early_start runs on every rank at
 * the hook (it all-gathers the T2 tails: a collective) and starts a background thread running mn_out_run over this rank's
 * share of X (read on the device, as out_stage reads it); mn_out_early_join returns the finished mn_out (its last write still
 * in flight: mn_out_finish as usual).  A correction to X after the hook (newton_st's corrections) makes the caller redo the
 * part file (mn_out_run again), as at size 1.  No waves in the background (MN_OUT_WAVES is ignored with it). */
typedef struct mn_out_early mn_out_early;
struct mdb_s;
mn_out_early *mn_out_early_start(const struct mdb_s *X, unsigned long d, unsigned long d_out, const char *outfile, int rank, int size, int verbose, comm *c, size_t t2_defer);
mn_out *mn_out_early_join(mn_out_early *e, double *t_run);            /* the thread joined; t_run: its seconds */
void mn_out_early_free(mn_out_early *e);                              /* after mn_out_finish */
/* Phase 15 EW (MN_OUT_DKM_HI, results/EW15.md 1.3) at size > 1: two part files per rank -- part size-1-rank = the rank's share of X_hi
 * (global limbs s + its share; the top rank's up to nl), part 2 size-1-rank = its share of X_lo below s (the top rank's up to s); nparts =
 * 2 size, so the sorted parts are the file.  mn_out_early_hi_start (every rank, at the division's X_hi hook: a collective) starts the
 * thread on the X_hi part (finished in the thread), which then waits; mn_out_early_lo_release (every rank: a collective -- the low layer's
 * T2 heads) gives it X_lo and lets it go on to the low part; mn_out_early_join returns the low part's mn_out (its last write in flight),
 * mn_out_early_hi the finished high part's.  X_hi's share must live until mn_out_early_free (the low part's patch reads it) */
mn_out_early *mn_out_early_hi_start(const struct mdb_s *Xh, size_t s, unsigned long d, unsigned long d_out, const char *outfile, int rank, int size, int verbose, comm *c, size_t t2_defer);
void mn_out_early_lo_release(mn_out_early *e, const struct mdb_s *Xl, comm *c);
mn_out *mn_out_early_hi(mn_out_early *e);
/* the digit residues of the whole string from J layers of parts (layer j's parts before layer j+1's; in a layer the top node first) */
void mn_out_digit_res_layers(const mn_out *const *os, int J, comm *c, uint64_t *Dres);
/* the digit residues of the whole string from the nodes' (ndig, dres) (all-gathered over c; c = 0 at size 1) */
void mn_out_digit_res(const mn_out *o, comm *c, uint64_t *Dres);
/* residues of a limb share modulo the T1 primes: the device kernel (db_mod_qs) or the host Horner */
void mn_out_res_share(const mn_out_src *src, uint64_t *res);
/* small host-vector collectives over a mesh, built on comm_allgather (device buffers on APU 0; c = 0: size 1) */
void mn_out_allgather_u64(comm *c, const uint64_t *v, int k, uint64_t *out);
void mn_out_res_combine(comm *c, const uint64_t *res, size_t lo, uint64_t *out);   /* X mod q = sum_r res_r B^lo_r over the nodes */
void mn_out_pq_combine(comm *c, const uint64_t *p, const uint64_t *qq, uint64_t *P, uint64_t *Q);   /* the term ranges in node order */
void mn_out_bcast_u64(comm *c, uint64_t *v, int k, int root);
int  mn_out_allreduce_or(comm *c, int v);
/* the stand-in for the distributed division's X (until A-div lands): node 0's host X scattered over mesh 0 into
 * the nodes' device shares, basis N = X->n; every node returns its share (a dbig, cnt = hi - lo limbs) */
void mn_out_scatter_standin(comm *c, const uint64_t *X, size_t xn, dbig *share, size_t *lo, size_t *cnt, size_t *n_out);
/* Phase 11 V: the recheck mode (ECALC_RECHECK=1).  The run writes <outfile>.t1 (node 0): N, d, d_out, size, the residues of
 * X, R, P, Q it checked with, and the d - d_out digits after d_out.  mn_out_recheck recomputes, without the run: the digit
 * residues from the digit file (the part files at size > 1, each node its own), X mod q from them and the tail, P and Q
 * mod q from the checkpointed top-level shares (BS_CKPT_DIR: the tree set at size > 1; the level-0 tree set a size-1 run
 * writes with ECALC_CKPT_TOP=1), the term recurrence over every node's range, the T2 windows over the file; then T1 with
 * R's residues from the sidecar (the one value that cannot be recomputed without the division) and every recomputed
 * residue against the run's.  Returns 0 when everything agrees (RECHECK OK). */
const char *mn_out_ckpt_default(const char *outfile);   /* Phase 12 W: the top-level set's default directory, <outfile>.top (BS_CKPT_DIR unset); 0 without an outfile */
void mn_out_sidecar_write(const char *outfile, unsigned long N, unsigned long d, unsigned long d_out, int size, const uint64_t *Xres, const uint64_t *Rres, const uint64_t *Pres, const uint64_t *Qres, const char *tail, size_t ntail);
int  mn_out_recheck(unsigned long N, unsigned long d, unsigned long d_out, const char *outfile, comm *c, int rank, int size, unsigned long a0, unsigned long b1, int verbose);
#ifdef __cplusplus
}
#endif
#endif
