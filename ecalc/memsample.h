/* memsample.h - Phase 14 S1 (APUMULT_STUDY E1, E12): what the node's memory is doing, measured.
 *
 *  E1   mem_live_line(what): one "live:" line -- the block pool's live bytes per APU now, its peak since the last line
 *       (the window: one tree level, one Newton doubling), the pool's size (live + free) and the peak's share of it,
 *       and /proc/meminfo's MemAvailable and Cached.  Printed when mem_live_on(): ECALC_LIVE=1 or ECALC_VERBOSE >= 2.
 *  E12  a sampler thread (ECALC_MEM_SAMPLE=<seconds>): RSS, MemFree, MemAvailable, Cached, Dirty, Writeback (from
 *       /proc/meminfo, which sees the page cache in HBM; hipMemGetInfo does not) and the pool's live bytes per APU, one
 *       line per sample to stderr or ECALC_MEM_SAMPLE_FILE (a "%d" in the name: the node-process rank); a summary line
 *       at stop (the minimum MemAvailable, the maxima of Cached, Dirty, RSS and each APU's live bytes). */
#ifndef EC_MEMSAMPLE_H
#define EC_MEMSAMPLE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct meminfo { size_t total, free, avail, cached, dirty, writeback; };   /* bytes */
int  mem_meminfo(struct meminfo *m);                 /* /proc/meminfo; 1 ok */
int  mem_live_on(void);
void mem_live_line(const char *what);                /* prints and restarts the window */
void mem_sampler_start(void);                        /* no-op unless ECALC_MEM_SAMPLE is set; idempotent */
void mem_sampler_stop(void);                         /* the summary line; also run at exit */
#ifdef __cplusplus
}
#endif
#endif
