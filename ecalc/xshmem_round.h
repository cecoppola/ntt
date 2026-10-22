/* xshmem_round.h - the default of XSHMEM_ROUND_MB (2026-10-21, user decision Q73: xshmem is the default multi-node transport).
 * Unset = 128 (rounds on, staging at most 128 MiB each way; plain xshmem without rounds fails the memory guard at full size);
 * an explicit value wins, 0 = off (whole-exchange staging).  One definition for the engine (comm_xshmem.c) and the planners
 * (binsplit.c, rns_dist.c), so they never disagree. */
#ifndef XSHMEM_ROUND_H
#define XSHMEM_ROUND_H
#include <stdlib.h>
static inline const char *xshmem_round_env(void) { const char *e = getenv("XSHMEM_ROUND_MB"); return e ? e : "128"; }
#endif
