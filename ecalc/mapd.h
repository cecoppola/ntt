/* mapd.h -- ECALC_MAPD (default 0): per-APU helper processes create the plane-pool and dbig VMM arena chunks and hand
 * them to the main process as POSIX file-descriptor handles. Every function is a no-op (returns 0) when the switch is
 * off or a helper failed. */
#ifndef MAPD_H
#define MAPD_H
#include <stddef.h>
int  mapd_helper_main(int argc, char **argv);          /* main(): argv[1] == "--mapd-helper" */
int  mapd_is_helper(int argc, char **argv);
void mapd_early(void);                                 /* first thing in main, before any HIP / PMI / fabric call: spawns the helpers if ECALC_MAPD=1 */
int  mapd_active(void);                                /* 1 if ECALC_MAPD=1 took effect (helpers were started) */
void mapd_request(int dev, int kind, int nchunks, size_t chunk, int final);   /* queue (kind 0 pool / 1 arena, count, size) for the helper of `dev`; final: it exits after this one; chunks come back in request order */
void mapd_sync(int dev);                               /* rns_init: wait until the helper's runtime is up (so device-memory measurements include it) */
void mapd_ready(int dev);                              /* wait for the next chunk without holding a lock (the 60 s limit lives here) */
void mapd_end_init(void);                              /* end of rns_init: finish and reap the helpers that have no request coming */
void mapd_pool_request(int dev, size_t bytes, size_t C);  /* the helper creates the chunks of one plane pool */
void *mapd_pool_alloc(int dev, size_t bytes, size_t C);    /* NULL: use hipMalloc; else a VA range over those chunks (imported, mapped, access for all APUs) */
int  mapd_pool_free(int dev, void *p);                 /* 1 if p came from mapd_pool_alloc (released) */
int  mapd_chunk(int dev, void **handle);               /* 1: the next chunk, imported (handle = hipMemGenericAllocationHandle_t); 0: create it in this process */
int  mapd_stat(int dev, int *n, double *wait, double *imp);   /* 1 if the device used a helper: chunks imported, seconds waiting for the helper / importing */
void mapd_finish(int dev);                             /* the arena is released: reap the helper */
#endif
