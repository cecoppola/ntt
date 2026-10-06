/* comm_ofi.h - Phase 17 (docs/code/07_COMM_OFI.md): the multi-NIC data plane of the SHMEM transport.  COMM_OFI=1 moves the data of
 * comm_shmem.c's device-buffer exchanges onto libfabric fi_writes over the NICs of the calling APU thread's device; SHMEM keeps the
 * control words.  Built with -DCOMM_OFI (Makefile OFI=1); without it comm_ofi_enabled() is 0 (and COMM_OFI=1 aborts). */
#ifndef EC_COMM_OFI_H
#define EC_COMM_OFI_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define COMM_OFI_BLOB 1024                     /* bytes of one member's address blob (pool VA, per NIC: address, MR key) */
typedef struct ofi_dev ofi_dev;                /* per device: the NICs (a domain / EP / CQ / AV each) and the registered comm pool */
typedef struct ofi_peers ofi_peers;            /* per communicator: the members' pool VAs, addresses and keys */
int      comm_ofi_enabled(void);               /* COMM_OFI=1 (aborts when set on a build without libfabric) */
ofi_dev *comm_ofi_dev(int dev);                /* the context of HIP device dev, opened at the first call (thread-safe) */
char    *comm_ofi_pool(ofi_dev *od);           /* the pool's base */
int      comm_ofi_in_pool(ofi_dev *od, const void *p);
ofi_dev *comm_ofi_owner(const void *p);        /* the device whose pool holds p, or 0 */
size_t   comm_ofi_alloc(ofi_dev *od, size_t len, int sym);   /* an offset in the pool (never 0); aborts when full */
void     comm_ofi_free(ofi_dev *od, size_t off);
void     comm_ofi_blob(ofi_dev *od, void *blob);             /* my blob (COMM_OFI_BLOB bytes) */
ofi_peers *comm_ofi_peers_new(ofi_dev *od, int n, int me, const void *blobs);   /* blobs: n x COMM_OFI_BLOB in rank order */
void     comm_ofi_peers_free(ofi_peers *pp);
/* post the writes of n bytes from src (inside my pool) to member r's pool at offset roff, in chunks striped over the NICs; *cnt is
 * raised by the number of writes and lowered as each completes (delivery complete) -- by whichever thread progresses the device */
void     comm_ofi_write(ofi_peers *pp, int r, size_t roff, const void *src, size_t n, long *cnt);
void     comm_ofi_progress(ofi_dev *od);       /* drain the device's completion queues */
void     comm_ofi_finalize(void);              /* the per-NIC bytes (COMM_OFI_VERBOSE / pe 0), then close everything */
#ifdef __cplusplus
}
#endif
#endif
