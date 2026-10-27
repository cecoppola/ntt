/* omp_team.h - the size of an OpenMP team for the host-side passes (A1, ECALC_OMP_TEAM).
 * omp_get_max_threads() is latched when the OpenMP runtime starts, from the launch CPU mask: a launch that gives the task one core (srun without -c,
 * SLURM_HINT=nomultithread) leaves it at 1-2 while ecalc widens its own mask later (mem_unpin -> sched_setaffinity).
 * ECALC_OMP_TEAM=omp (default): ec_team() = omp_get_max_threads(), as before.  =mask: ec_team() = the CPUs the calling thread may run on right now
 * (its affinity mask; fallback omp_get_num_procs()).  The team is then named with num_threads() where it is used. */
#ifndef EC_OMP_TEAM_H
#define EC_OMP_TEAM_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
static inline int ec_team_mask_count(void)      /* the affinity mask of the calling thread */
{
    cpu_set_t m; CPU_ZERO(&m);
    int n = sched_getaffinity(0, sizeof m, &m) == 0 ? CPU_COUNT(&m) : 0;
    return n < 1 ? omp_get_num_procs() : n;
}
static inline int ec_team_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("ECALC_OMP_TEAM"); v = (e && !strcmp(e, "mask")) ? 1 : 0; } return v; }
/* a background thread keeps its explicit team: ec_team_set(n) = omp_set_num_threads(n) plus a thread-local override that ec_team() returns with the switch on
 * (the mask team would otherwise ignore ECALC_BG_THREADS and compete with the foreground).  The variable lives in fatal.c. */
extern __thread int ec_team_override;
static inline void ec_team_set(int n) { omp_set_num_threads(n); ec_team_override = n; }
static inline int ec_team(void) { if (ec_team_on() && ec_team_override > 0) return ec_team_override; int n = ec_team_on() ? ec_team_mask_count() : omp_get_max_threads(); return n < 1 ? 1 : n; }
#endif
