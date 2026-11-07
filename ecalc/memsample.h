/* memsample.h - what the node's memory is doing, measured. mem_live_line(what): one "live:" line when mem_live_on()
 * (ECALC_VERBOSE >= 2): the block pool's live bytes per APU now, its peak since the last line (the window: one tree
 * level, one Newton doubling), the pool's size and the peak's share of it, and /proc/meminfo's MemAvailable and
 * Cached. Sampler thread (ECALC_MEM_SAMPLE=<seconds>): RSS, MemFree, MemAvailable, Cached, Dirty, Writeback
 * (/proc/meminfo sees the page cache in HBM; hipMemGetInfo does not) and the pool's live bytes per APU, one stderr
 * line per sample, and a summary at stop (minimum MemAvailable, maxima of Cached, Dirty, RSS, each APU's live bytes).
 * Guard: ECALC_MEM_GUARD_GB=<x>: when MemAvailable falls below x GB the sampler stops the run through ec_fatal (rc
 * EC_RC_MEMGUARD = 9); without ECALC_MEM_SAMPLE the sampler runs for the guard alone, silent, every second. */
#ifndef EC_MEMSAMPLE_H
#define EC_MEMSAMPLE_H
#include <stddef.h>
#define EC_RC_MEMGUARD 9                              /* the memory guard stopped the run (fatal.h lists the other codes) */
#ifdef __cplusplus
extern "C" {
#endif
struct meminfo { size_t total, free, avail, cached, dirty, writeback; };   /* bytes */
int  mem_meminfo(struct meminfo *m);                 /* /proc/meminfo; 1 ok */
int  mem_live_on(void);
void mem_live_line(const char *what);                /* prints and restarts the window */
int  mem_apu_free_now(int *ids, size_t *freeb, size_t *memfree);  /* per APU this process drives (<= 4): id, free = MemFree + file cache, MemFree (may be 0); returns their number, 0 = not available */
void mem_sampler_start(void);                        /* no-op unless ECALC_MEM_SAMPLE is set; idempotent */
void mem_sampler_stop(void);                         /* the summary line; also run at exit */
#ifdef __cplusplus
}
#endif
#endif
