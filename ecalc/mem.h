/* mem.h - memory placement for the pipeline (PLAN.md 8, step 3; rule 4).
 *
 *  hstage    per-device pinned host staging: malloc + first-touch by threads
 *            pinned to the APU's NUMA node + hipHostRegister.  On this node
 *            APU d <-> NUMA node d (RESULTS.md 29); the wrong node costs 55x
 *            (RESULTS.md 20), so the touch is done here, not left to OpenMP.
 *  dpool     grow-only device pools (hipMalloc, power-of-two growth, never
 *            shrink: hipFree + hipMalloc cost 0.5-1 s each, RESULTS.md 22).
 *  hpool     grow-only host pools (malloc, not calloc; 4 KiB-aligned).
 *  mem_vmhwm peak resident set in bytes, from /proc/self/status.
 */
#ifndef EC_MEM_H
#define EC_MEM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  mem_numa_node_of_device(int dev);              /* node index (== dev here) */
void mem_pin_to_node(int node);                     /* sched_setaffinity to the node's cpulist */
void mem_unpin(void);                               /* back to all cpus */
int  mem_ncpus_node(int node);

/* pinned staging on the device's NUMA node; bytes is rounded up to 2 MiB */
void *mem_hstage_alloc(int dev, size_t bytes, double *touch_s, double *reg_s);
void  mem_hstage_free(void *p);

/* registered host memory not tied to a node (level pools, batch operands/results);
 * pages are first-touched by the caller.  mem_is_registered answers for any
 * pointer inside a block from mem_hstage_alloc or mem_hreg_alloc. */
void *mem_hreg_alloc(size_t bytes);
void  mem_hreg_free(void *p);
int   mem_is_registered(const void *p, size_t bytes);
/* WP3: device pools the CPU also uses (hipMalloc on dev; CPU read/write at the local rate with
 * XNACK off, RESULTS.md 55); mem_dev_of says which device holds a pointer (-1: not a device pool) */
void *mem_dev_alloc(int dev, size_t bytes);
void  mem_dev_free(void *p);
void  mem_dev_forget(void *p);
int   mem_dev_of(const void *p);
size_t mem_dev_pool_bytes(void);
int   mem_device_count(void);
void  mem_dev_copy(void *dst, const void *src, size_t bytes);
void  mem_dev_copy_on(int dev, void *dst, const void *src, size_t bytes);   /* hipMemcpy (DMA): CPU streaming stores into device pools run at ~8 GB/s */                    /* total in device pools (not in RSS) */
/* WP3: pin every OpenMP thread to its home node (thread t -> node t nnodes / nthreads) so that
 * region-aware loops touch node-local memory; mem_unpin() returns a thread to its home */
void mem_pin_threads(int nnodes);
int  mem_thread_home(void);                          /* -1 before mem_pin_threads */
int  mem_region_threads(int *rank);                 /* count of threads on this node, and this thread's rank among them */

typedef struct { void *p; size_t cap; int dev; } dpool;
void *dpool_get(dpool *d, int dev, size_t bytes);   /* grows to pow2 >= bytes on device dev */
void  dpool_free(dpool *d);

typedef struct { void *p; size_t cap; } hpool;
void *hpool_get(hpool *h, size_t bytes);
void  hpool_free(hpool *h);

size_t mem_vmhwm(void);
size_t mem_vmrss(void);
double mem_now(void);

#ifdef __cplusplus
}
#endif
#endif
