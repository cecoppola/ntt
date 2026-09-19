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
 * two chunk buffers, so a chunk is written while the next one is formatted. */
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
typedef struct {
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
    void *priv;                              /* the writer thread while a write is in flight (mn_out_finish joins it) */
} mn_out;
void mn_out_boundaries(mn_out *o, const mn_out_src *src, comm *c);   /* size > 1: all-gather the nodes' tails (49 digits) -> this node's head */
int  mn_out_run(mn_out *o, const mn_out_src *src);                   /* format, residues, T2, write (streamed); 0 = ok.  Returns with the last chunk's write in flight */
void mn_out_finish(mn_out *o);                                       /* wait for the writes, close the part file */
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
#ifdef __cplusplus
}
#endif
#endif
