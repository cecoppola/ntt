/* xshmem_boot.h -- process bootstrap for the xshmem libfabric backend (built only with XSHMEM=ofi).
 * One file pair, xshmem_boot.c: BOOT=pmi2 (default, Cray PMI2, plain srun without --mpi) or BOOT=pmix (stub in P3: aborts).
 * Rank and size come from PMI2 and are cross-checked against SLURM_PROCID / SLURM_NTASKS when those are set (mismatch aborts).
 * Not thread safe: call from one thread (the init thread). All return 0 on success; failures abort with a message. */
#ifndef XSHMEM_BOOT_H
#define XSHMEM_BOOT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* init PMI2 once (repeat calls return the same values); *rank in [0,*size) */
int  boot_init(int *rank, int *size);
/* every rank passes a blob of the same len; out (size*len bytes, caller-owned) receives the blobs in rank order.
 * Collective; includes its own fence. Values are chunked at 900 B per KVS key, so len is unlimited. */
int  boot_allgather(const void *blob, size_t len, void *out);
/* collective barrier (PMI2 KVS fence) */
void boot_fence(void);
/* abort the whole job (PMI2_Abort) so that peers blocked in a fence or a wait do not hang; falls back to abort() */
void boot_abort(const char *msg);
/* PMI2_Finalize; boot_* calls after it are errors */
void boot_finalize(void);
#ifdef __cplusplus
}
#endif
#endif
