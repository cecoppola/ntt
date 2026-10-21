/* xshmem_ofi_priv.h -- private types of the libfabric backend xshmem_ofi.c (built only with make XSHMEM=ofi).
 * One pe_t per PE the process hosts: K = XSHMEM_LOOPBACK_PES (default 1 = one PE per process, the original shape).  Each pe_t owns its NIC
 * domains, endpoints, heaps and its own mutex; lanes point back to their pe_t, so every context call finds its PE from the context. */
#ifndef XSHMEM_OFI_PRIV_H
#define XSHMEM_OFI_PRIV_H
#include <stdint.h>
#include <pthread.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include "xshmem.h"
#include "pe_geo.h"

#define MAXD   16          /* NIC slots (domains) per PE */
#define MAXEP  16          /* endpoints per PE: default + lanes + spares */
#define MAXADDR 16         /* bytes of one endpoint address (cxi: 8); small so that the blob is one PMI2 KVS value per PE */
#define MAXLPE 8           /* loopback PEs per process */
#define HALIGN ((size_t)2 << 20)
#define SPARE  2           /* contexts beyond the N lanes that xshmem_ctx_create can still hand out */

struct pe_s;
typedef struct {
    int cxi, open;
    struct fi_info *info; struct fid_fabric *fab; struct fid_domain *dom; struct fid_av *av;
    int first_ep;
} dom_t;
typedef struct {
    int e, slot, used;
    struct pe_s *pe;                           /* owner PE (loopback: the context call derives its PE from here) */
    struct fid_ep *ep; struct fid_cq *cq; struct fid_cntr *cn;
    struct fid_mr *mr[2]; void *desc[2]; uint64_t key[2];   /* one MR per endpoint and heap: cxi binds an MR to one endpoint only (FI_MR_ENDPOINT) */
    uint64_t issued;                           /* posted operations (under pe lock) */
    fi_addr_t *fa;                             /* [pe] */
    unsigned char *touch; int *tlist; int nt;  /* flush mode: target PEs written since the last quiet */
} lane_t;
typedef struct { uint32_t nics, nep; struct { uint32_t alen; char addr[MAXADDR]; } ep[MAXEP]; } ablob_t;
typedef struct { uint64_t base, len; uint64_t key[MAXEP]; } hblob_t;

typedef struct pe_s {
    int lpe;                                   /* index among the PEs of this process */
    int mype, npes, nnodes, nics, nep, vaddr, pkey, sig_mode, log_sp, log_key;
    xshmem_cfg_t cfg; pe_topo topo; double quiet_s;
    size_t inject, maxmsg;
    pthread_mutex_t lock;
    dom_t dom[MAXD]; lane_t lane[MAXEP];
    char *hbase[2]; size_t hlen[2]; int hhost[2]; size_t flush_off;   /* flush mode: 8 B flush slot (and a source word at +64) in the DATA heap tail beyond hlen */
    hblob_t *peer[2];                          /* [space][pe] */
    xshmem_ofi_cnt_t cnt;
} pe_t;
#endif
