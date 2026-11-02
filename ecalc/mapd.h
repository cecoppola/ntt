/* mapd.h -- ECALC_MAPD (default 0): per-APU helper processes create the dbig VMM arena chunks and hand them to the main process as POSIX
 * file-descriptor handles (results/MAPBENCH.md, S1).  Every function is a no-op (returns 0) when the switch is off or a helper failed. */
#ifndef MAPD_H
#define MAPD_H
#include <stddef.h>
int  mapd_helper_main(int argc, char **argv);          /* main(): argv[1] == "--mapd-helper" */
int  mapd_is_helper(int argc, char **argv);
void mapd_early(void);                                 /* first thing in main, before any HIP / PMI / fabric call: spawns the helpers if ECALC_MAPD=1 */
void mapd_request(int dev, int nchunks, size_t chunk); /* db_vmm_arena_alloc: the helper of `dev` creates nchunks chunks of `chunk` bytes, in order */
int  mapd_chunk(int dev, void **handle);               /* 1: the next chunk, imported (handle = hipMemGenericAllocationHandle_t); 0: create it in this process */
int  mapd_stat(int dev, int *n, double *wait, double *imp);   /* 1 if the device used a helper: chunks imported, seconds waiting for the helper / importing */
void mapd_finish(int dev);                             /* the arena is released: reap the helper */
#endif
