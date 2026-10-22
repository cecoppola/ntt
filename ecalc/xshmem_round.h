/* xshmem_round.h - the default of XSHMEM_ROUND_MB (2026-10-21, user decision Q73: xshmem is the default multi-node transport).
 * Unset = 128 (rounds on, staging at most 128 MiB each way; plain xshmem without rounds fails the memory guard at full size);
 * an explicit value wins, 0 = off (whole-exchange staging).  One definition for the engine (comm_xshmem.c) and the planners
 * (binsplit.c, rns_dist.c), so they never disagree. */
#ifndef XSHMEM_ROUND_H
#define XSHMEM_ROUND_H
#include <stdlib.h>
#ifdef __cplusplus
extern "C" int vpe_on(void);
#else
int vpe_on(void);
#endif   /* vpe.c (not vpe.h: this header is included before the HIP platform macros are set) */
/* S2d (s2heap): the default depends on the exchange mode, as the engine decides it (comm_xshmem_init): the rounds run in the worker thread, and poll mode (vPE or ECALC_PE_PER_GPU
 * default, or ECALC_XEX_THREAD=0) has none, so an unset XSHMEM_ROUND_MB there means whole exchanges ("0").  Before this the planners assumed 128 MiB rounds for a poll-mode run, while
 * the engine staged whole exchanges: the XSHMEM_HEAP_MB it sized was too small whenever the v-slots did not dominate (e.g. ECALC_PE_PER_GPU=1 DIST_CHUNKS=8). */
static inline const char *xshmem_round_env(void)
{
    const char *e = getenv("XSHMEM_ROUND_MB"); if (e) return e;
    const char *t = getenv("ECALC_XEX_THREAD"); int thr = t ? atoi(t) != 0 : !vpe_on();
    return thr ? "128" : "0";
}
#endif
