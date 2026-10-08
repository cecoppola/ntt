/* comm_ofi.c - Phase 17 (docs/code/07_COMM_OFI.md, results/OFI17.md): the multi-NIC data plane of comm_shmem.c.
 *
 * Per HIP device d (= APU thread d), opened at the first communicator created on d: one libfabric `cxi` fabric / domain / EP / CQ / AV
 * per NIC of d's list (COMM_OFI_NICS, else the cxi devices on d's NUMA node), and one comm pool registered in every one of those
 * domains (COMM_OFI_POOL: fine-grained device memory, hipMalloc, or NUMA-local host memory; VMM memory cannot be registered,
 * results/NIC16_experiments.md #18).  The method is NIC16's (one process, one domain per NIC: 93.3 GB/s per node on aac7).
 * Per communicator: the members' pool VA and per-NIC address / key (a blob each, all-gathered by the caller over SHMEM).
 * comm_ofi_write cuts a slab into chunks (COMM_OFI_CHUNK_MB) striped over the device's NICs, posts them delivery-complete with
 * at most COMM_OFI_WINDOW in flight per NIC, and counts them on the caller's counter; whoever progresses the device's CQs
 * lowers the counter of the communicator that posted (the CQ entry's context).  The caller (comm_shmem.c) sends the SHMEM signal
 * once the counter of a peer is 0.  One mutex per device around every call into its domains (FI_THREAD_DOMAIN).
 * The user's decision of 2026-10-06 (results/OFI17.md): COMM_OFI unset defaults to on wherever a cxi NIC is present (Cray
 * Slingshot), off elsewhere (e.g. aac6's TCP/SOS) -- no code change needed there; COMM_OFI=0 always forces the old path. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comm_ofi.h"
#include "fatal.h"
#ifndef COMM_OFI
int comm_ofi_enabled(void)
{
    const char *e = getenv("COMM_OFI");
    if (e && atoi(e)) ec_fatal(EC_RC_FATAL, "comm_ofi: COMM_OFI=%s but this binary was built without libfabric (make OFI=1 LIBFABRIC=<prefix>)\n", e);
    return 0;
}
int comm_ofi_planned(void) { return 0; }              /* Phase 17 OFIMEM: no libfabric, no comm pools */
ofi_dev *comm_ofi_dev(int dev) { (void)dev; return 0; }
char *comm_ofi_pool(ofi_dev *od) { (void)od; return 0; }
int comm_ofi_in_pool(ofi_dev *od, const void *p) { (void)od; (void)p; return 0; }
ofi_dev *comm_ofi_owner(const void *p) { (void)p; return 0; }
size_t comm_ofi_alloc(ofi_dev *od, size_t len, int sym) { (void)od; (void)len; (void)sym; return 0; }
void comm_ofi_free(ofi_dev *od, size_t off) { (void)od; (void)off; }
void comm_ofi_blob(ofi_dev *od, void *blob) { (void)od; (void)blob; }
ofi_peers *comm_ofi_peers_new(ofi_dev *od, int n, int me, const void *blobs) { (void)od; (void)n; (void)me; (void)blobs; return 0; }
void comm_ofi_peers_free(ofi_peers *pp) { (void)pp; }
void comm_ofi_write(ofi_peers *pp, int r, size_t roff, const void *src, size_t n, long *cnt) { (void)pp; (void)r; (void)roff; (void)src; (void)n; (void)cnt; }
void comm_ofi_progress(ofi_dev *od) { (void)od; }
void comm_ofi_finalize(void) { }
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#ifndef COMM_HOST_ONLY
#include <hip/hip_runtime.h>
#endif

#define MAXNIC 8
#define MAXDEV 16
#define ALIGN 256
enum { POOL_FINE, POOL_COARSE, POOL_HOST };
struct nicx {
    int cxi, numa;
    struct fi_info *info; struct fid_fabric *fab; struct fid_domain *dom; struct fid_ep *ep; struct fid_cq *cq; struct fid_av *av; struct fid_mr *mr;
    void *desc; uint64_t key; char addr[64]; size_t alen;
    int out; double bytes; long ops;
};
struct oblk { size_t off, len; int used; struct oblk *next; };
struct ofi_dev {
    int d, k, form, dc;                       /* dc: writes with FI_DELIVERY_COMPLETE */
    struct nicx nic[MAXNIC];
    char *pool; size_t bytes;
    pthread_mutex_t lock;                     /* the domains' calls and the allocator */
    struct oblk *blocks; size_t cur, peak, sym_cur, sym_peak;
};
struct blobf { uint64_t va; uint32_t k, pad; struct { uint64_t key, alen; char addr[64]; } nic[MAXNIC]; };
struct ofi_peers { ofi_dev *od; int n, me; uint64_t *va; uint64_t *key; fi_addr_t *fa; };   /* key / fa: [r * MAXNIC + j] */

static struct { int on, verbose, window; size_t chunk; pthread_mutex_t lock; ofi_dev *dev[MAXDEV]; } G = { -1, 0, 64, 4 << 20, PTHREAD_MUTEX_INITIALIZER, { 0 } };
#define CK(x) do { int r_ = (int)(x); if (r_) ec_fatal(EC_RC_FATAL, "comm_ofi: %s = %d (%s) at %s:%d\n", #x, r_, fi_strerror(-r_), __FILE__, __LINE__); } while (0)

static int read_int(const char *path, int def) { FILE *f = fopen(path, "r"); int v = def; if (f) { if (fscanf(f, "%d", &v) != 1) v = def; fclose(f); } return v; }
/* the user's decision, 2026-10-06 (results/OFI17.md): with COMM_OFI unset, on only where it applies -- a cxi NIC present
 * (Cray Slingshot; aac6's TCP/SOS has none, so it stays inert there without needing to know the SHMEM backend) */
static int cxi_present(void) { char path[64]; snprintf(path, sizeof path, "/sys/class/cxi/cxi0/device/numa_node"); return access(path, F_OK) == 0; }
/* Phase 17 OFIMEM: the decision comm_ofi_enabled makes, without opening anything -- for the memory rule (binsplit.c: the SHMEM pool
 * shrinks to what SHMEM still carries and the four comm pools are counted) and MN_PLAN_ONLY's `plan pool` line.  The plan runs on the
 * login node, which may have no cxi (aac7's uan1): mnrun.sh then passes COMM_OFI_PLAN_CXI=1 when the first compute node has one. */
int comm_ofi_planned(void)
{
    const char *e = getenv("COMM_OFI"); if (e) return atoi(e) != 0;
    const char *p = getenv("COMM_OFI_PLAN_CXI");
    return cxi_present() || (p && atoi(p) != 0);
}
int comm_ofi_enabled(void)
{
    if (G.on >= 0) return G.on;
    const char *e = getenv("COMM_OFI"); G.on = e ? (atoi(e) != 0) : cxi_present();
    if (G.on) {
        G.verbose = getenv("COMM_OFI_VERBOSE") ? atoi(getenv("COMM_OFI_VERBOSE")) : 0;
        if (getenv("COMM_OFI_WINDOW")) G.window = atoi(getenv("COMM_OFI_WINDOW")); if (G.window < 1) G.window = 1;
        if (getenv("COMM_OFI_CHUNK_MB")) { double m = atof(getenv("COMM_OFI_CHUNK_MB")); if (m > 0) G.chunk = (size_t)(m * 1048576.0); }
        G.chunk = (G.chunk + 4095) & ~(size_t)4095; if (!G.chunk) G.chunk = 4096;
    }
    return G.on;
}
/* the NUMA node of device d: its PCI function's numa_node (the host-only build: d) */
static int dev_numa(int d)
{
#ifndef COMM_HOST_ONLY
    char bus[64] = { 0 }, path[160];
    if (hipDeviceGetPCIBusId(bus, sizeof bus, d) == hipSuccess) {
        for (char *c = bus; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 'a' - 'A';
        snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/numa_node", bus);
        int n = read_int(path, -1); if (n >= 0) return n;
    } else (void)hipGetLastError();
#endif
    return d;
}
/* device d's NIC list: COMM_OFI_NICS ("0;1;2;3", "0,4;1,5;2,6;3,7": `;` between devices) or the cxi devices on d's NUMA node */
static int nic_list(int d, int *cx)
{
    int k = 0; const char *e = getenv("COMM_OFI_NICS");
    if (e && *e) {
        const char *s = e; for (int i = 0; i < d && s; i++) { s = strchr(s, ';'); if (s) s++; }
        if (!s || !*s || *s == ';') ec_fatal(EC_RC_FATAL, "comm_ofi: COMM_OFI_NICS=%s names no NIC for device %d\n", e, d);
        while (*s && *s != ';' && k < MAXNIC) { cx[k++] = atoi(s); while (*s && *s != ',' && *s != ';') s++; if (*s == ',') s++; }
        return k;
    }
    int numa = dev_numa(d), ncxi = 0; char path[96];
    for (int i = 0; i < 64; i++) {
        snprintf(path, sizeof path, "/sys/class/cxi/cxi%d/device/numa_node", i);
        int n = read_int(path, -2); if (n == -2) continue;
        ncxi = i + 1; if (n == numa && k < MAXNIC) cx[k++] = i;
    }
    if (!k) { if (!ncxi) ec_fatal(EC_RC_FATAL, "comm_ofi: no cxi device in /sys/class/cxi (set COMM_OFI_NICS)\n"); cx[k++] = d % ncxi; }
    return k;
}
static void open_nic(ofi_dev *od, struct nicx *x)
{
    struct fi_info *h = fi_allocinfo(); char name[16]; snprintf(name, sizeof name, "cxi%d", x->cxi);
    int hmem = od->form != POOL_HOST;
    h->fabric_attr->prov_name = strdup("cxi"); h->domain_attr->name = strdup(name);
    h->ep_attr->type = FI_EP_RDM;
    h->caps = FI_RMA | FI_MSG | (hmem ? FI_HMEM : 0);
    h->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_ENDPOINT | (hmem ? FI_MR_HMEM : 0);
    h->domain_attr->threading = FI_THREAD_DOMAIN;
    h->tx_attr->size = 4096;
    h->tx_attr->op_flags = od->dc ? FI_DELIVERY_COMPLETE : 0;
    int rc = fi_getinfo(FI_VERSION(1, 20), NULL, NULL, 0, h, &x->info);
    if (rc) { h->tx_attr->op_flags = 0; od->dc = 0; CK(fi_getinfo(FI_VERSION(1, 20), NULL, NULL, 0, h, &x->info)); fprintf(stderr, "comm_ofi: cxi%d: no FI_DELIVERY_COMPLETE (%s): transmit-complete writes\n", x->cxi, fi_strerror(-rc)); }
    fi_freeinfo(h);
    CK(fi_fabric(x->info->fabric_attr, &x->fab, NULL));
    CK(fi_domain(x->fab, x->info, &x->dom, NULL));
    struct fi_cq_attr cqa; memset(&cqa, 0, sizeof cqa); cqa.format = FI_CQ_FORMAT_CONTEXT; cqa.size = 8192;
    CK(fi_cq_open(x->dom, &cqa, &x->cq, NULL));
    struct fi_av_attr ava; memset(&ava, 0, sizeof ava); ava.type = FI_AV_TABLE;
    CK(fi_av_open(x->dom, &ava, &x->av, NULL));
    CK(fi_endpoint(x->dom, x->info, &x->ep, NULL));
    CK(fi_ep_bind(x->ep, &x->cq->fid, FI_TRANSMIT | FI_RECV));
    CK(fi_ep_bind(x->ep, &x->av->fid, 0));
    CK(fi_enable(x->ep));
    x->alen = sizeof x->addr; CK(fi_getname(&x->ep->fid, x->addr, &x->alen));
    if (od->form == POOL_HOST) CK(fi_mr_reg(x->dom, od->pool, od->bytes, FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE, 0, 0, 0, &x->mr, NULL));
    else {
        struct iovec iov = { od->pool, od->bytes }; struct fi_mr_attr ma; memset(&ma, 0, sizeof ma);
        ma.mr_iov = &iov; ma.iov_count = 1; ma.access = FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE; ma.iface = FI_HMEM_ROCR; ma.device.reserved = od->d;
        rc = fi_mr_regattr(x->dom, &ma, 0, &x->mr);
        if (rc) ec_fatal(EC_RC_FATAL, "comm_ofi: registering the %s comm pool of device %d (%zu MiB) with cxi%d: %d (%s); COMM_OFI_POOL=host avoids FI_HMEM\n", od->form == POOL_FINE ? "fine-grained" : "hipMalloc", od->d, od->bytes >> 20, x->cxi, rc, fi_strerror(-rc));
    }
    if (x->info->domain_attr->mr_mode & FI_MR_ENDPOINT) { CK(fi_mr_bind(x->mr, &x->ep->fid, 0)); CK(fi_mr_enable(x->mr)); }
    x->desc = fi_mr_desc(x->mr); x->key = fi_mr_key(x->mr);
}
static void *host_pool(size_t bytes, int numa)
{
    void *p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) ec_fatal(EC_RC_OOM, "comm_ofi: mmap of the %zu MiB host comm pool failed\n", bytes >> 20);
    if (numa >= 0 && numa < 64) { unsigned long mask = 1UL << numa; if (syscall(SYS_mbind, p, bytes, 1 /* MPOL_PREFERRED */, &mask, 64, 0)) { /* not fatal: first touch decides */ } }
    memset(p, 0, bytes);                      /* fault in on that node */
    return p;
}
ofi_dev *comm_ofi_dev(int d)
{
    if (!comm_ofi_enabled() || d < 0 || d >= MAXDEV) return 0;
    pthread_mutex_lock(&G.lock);
    ofi_dev *od = G.dev[d];
    if (od) { pthread_mutex_unlock(&G.lock); return od; }
    od = (ofi_dev *)calloc(1, sizeof *od); od->d = d; od->dc = getenv("COMM_OFI_DC") ? atoi(getenv("COMM_OFI_DC")) != 0 : 1;   /* COMM_OFI_DC=0: M2 diagnostic (XEFF X3), transmit-complete writes, UNSAFE (signal may pass data); throughput upper bound only */ pthread_mutex_init(&od->lock, 0);
    int cx[MAXNIC]; od->k = nic_list(d, cx);
    const char *ef = getenv("COMM_OFI_POOL");
#ifdef COMM_HOST_ONLY
    od->form = POOL_HOST; (void)ef;
#else
    od->form = !ef || !strcmp(ef, "fine") ? POOL_FINE : !strcmp(ef, "coarse") ? POOL_COARSE : !strcmp(ef, "host") ? POOL_HOST : -1;
    if (od->form < 0) ec_fatal(EC_RC_FATAL, "comm_ofi: COMM_OFI_POOL=%s: use fine, coarse or host\n", ef);
#endif
    size_t mb = getenv("COMM_OFI_POOL_MB") ? (size_t)atol(getenv("COMM_OFI_POOL_MB")) : (getenv("COMM_SHMEM_POOL_MB") ? (size_t)atol(getenv("COMM_SHMEM_POOL_MB")) : 8192) / 4 + 256;
    od->bytes = mb << 20;
    for (int j = 0; j < od->k; j++) { od->nic[j].cxi = cx[j]; char path[96]; snprintf(path, sizeof path, "/sys/class/cxi/cxi%d/device/numa_node", cx[j]); od->nic[j].numa = read_int(path, -1); }
#ifdef COMM_HOST_ONLY
    od->pool = (char *)host_pool(od->bytes, od->nic[0].numa);
#else
    {
        int cur = -1; hipError_t he = hipGetDevice(&cur); if (he != hipSuccess || cur != d) { (void)hipGetLastError(); if (hipSetDevice(d) != hipSuccess) ec_fatal(EC_RC_FATAL, "comm_ofi: hipSetDevice(%d)\n", d); }
        void *p = 0;
        if (od->form == POOL_FINE) he = hipExtMallocWithFlags(&p, od->bytes, hipDeviceMallocFinegrained);
        else if (od->form == POOL_COARSE) he = hipMalloc(&p, od->bytes);
        else { p = host_pool(od->bytes, od->nic[0].numa); he = hipHostRegister(p, od->bytes, hipHostRegisterPortable); }
        if (he != hipSuccess) ec_fatal(he == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "comm_ofi: the comm pool of device %d (%zu MiB, COMM_OFI_POOL_MB): %s\n", d, mb, hipGetErrorString(he));
        if (od->form != POOL_HOST) { he = hipMemset(p, 0, od->bytes); if (he == hipSuccess) he = hipDeviceSynchronize(); if (he != hipSuccess) ec_fatal(EC_RC_FATAL, "comm_ofi: hipMemset of the pool: %s\n", hipGetErrorString(he)); }
        od->pool = (char *)p;
        if (cur >= 0 && cur != d) (void)hipSetDevice(cur);
    }
#endif
    for (int j = 0; j < od->k; j++) open_nic(od, &od->nic[j]);
    od->blocks = (struct oblk *)calloc(1, sizeof *od->blocks); od->blocks->off = ALIGN; od->blocks->len = od->bytes - ALIGN;   /* offset 0 stays unused: 0 = none */
    G.dev[d] = od;
    pthread_mutex_unlock(&G.lock);
    if (G.verbose) {
        char l[256]; int n = snprintf(l, sizeof l, "comm_ofi: device %d: pool %zu MiB (%s)%s, NICs", d, mb, od->form == POOL_FINE ? "fine-grained device" : od->form == POOL_COARSE ? "hipMalloc" : "host", od->dc ? "" : ", transmit-complete");
        for (int j = 0; j < od->k && n < (int)sizeof l - 24; j++) n += snprintf(l + n, sizeof l - n, " cxi%d(numa %d)", od->nic[j].cxi, od->nic[j].numa);
        fprintf(stderr, "%s, chunk %zu KiB, window %d\n", l, G.chunk >> 10, G.window);
    }
    return od;
}
char *comm_ofi_pool(ofi_dev *od) { return od ? od->pool : 0; }
int comm_ofi_in_pool(ofi_dev *od, const void *p) { return od && (const char *)p >= od->pool && (const char *)p < od->pool + od->bytes; }
ofi_dev *comm_ofi_owner(const void *p) { for (int d = 0; d < MAXDEV; d++) if (comm_ofi_in_pool(G.dev[d], p)) return G.dev[d]; return 0; }
size_t comm_ofi_alloc(ofi_dev *od, size_t len, int sym)
{
    len = (len + ALIGN - 1) & ~(size_t)(ALIGN - 1); if (!len) len = ALIGN;
    pthread_mutex_lock(&od->lock);
    for (struct oblk *b = od->blocks; b; b = b->next) if (!b->used && b->len >= len) {
        if (b->len > len) { struct oblk *nb = (struct oblk *)malloc(sizeof *nb); nb->off = b->off + len; nb->len = b->len - len; nb->used = 0; nb->next = b->next; b->next = nb; b->len = len; }
        b->used = 1 + (sym != 0); od->cur += len; if (od->cur > od->peak) od->peak = od->cur;
        if (sym) { od->sym_cur += len; if (od->sym_cur > od->sym_peak) od->sym_peak = od->sym_cur; }
        size_t off = b->off; pthread_mutex_unlock(&od->lock); return off;
    }
    size_t cur = od->cur; pthread_mutex_unlock(&od->lock);
    ec_fatal(EC_RC_OOM, "comm_ofi: the comm pool of device %d (%zu MiB, COMM_OFI_POOL_MB) cannot hold %zu MiB more (in use %zu MiB): COMM_OFI_POOL_MB >= %zu needed\n",
             od->d, od->bytes >> 20, len >> 20, cur >> 20, ((cur + len) >> 20) + 2);
    return 0;
}
void comm_ofi_free(ofi_dev *od, size_t off)
{
    pthread_mutex_lock(&od->lock);
    struct oblk *p = 0;
    for (struct oblk *b = od->blocks; b; p = b, b = b->next) if (b->off == off && b->used) {
        od->cur -= b->len; if (b->used == 2) od->sym_cur -= b->len; b->used = 0;
        if (b->next && !b->next->used) { struct oblk *n = b->next; b->len += n->len; b->next = n->next; free(n); }
        if (p && !p->used) { p->len += b->len; p->next = b->next; free(b); }
        break;
    }
    pthread_mutex_unlock(&od->lock);
}
void comm_ofi_blob(ofi_dev *od, void *blob)
{
    struct blobf *b = (struct blobf *)blob; memset(blob, 0, COMM_OFI_BLOB);
    b->va = (od->nic[0].info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) ? (uint64_t)(uintptr_t)od->pool : 0;   /* the RMA address of pool offset 0 (offset-based without FI_MR_VIRT_ADDR) */
    b->k = (uint32_t)od->k;
    for (int j = 0; j < od->k; j++) { b->nic[j].key = od->nic[j].key; b->nic[j].alen = od->nic[j].alen; memcpy(b->nic[j].addr, od->nic[j].addr, od->nic[j].alen); }
}
ofi_peers *comm_ofi_peers_new(ofi_dev *od, int n, int me, const void *blobs)
{
    ofi_peers *pp = (ofi_peers *)calloc(1, sizeof *pp); pp->od = od; pp->n = n; pp->me = me;
    pp->va = (uint64_t *)calloc(n, 8); pp->key = (uint64_t *)calloc((size_t)n * MAXNIC, 8); pp->fa = (fi_addr_t *)calloc((size_t)n * MAXNIC, sizeof(fi_addr_t));
    pthread_mutex_lock(&od->lock);
    for (int r = 0; r < n; r++) {
        const struct blobf *b = (const struct blobf *)((const char *)blobs + (size_t)r * COMM_OFI_BLOB);
        if ((int)b->k != od->k) { pthread_mutex_unlock(&od->lock); ec_fatal(EC_RC_FATAL, "comm_ofi: member %d has %u NICs on this mesh, I have %d (COMM_OFI_NICS must give every node the same count)\n", r, b->k, od->k); }
        pp->va[r] = b->va;
        for (int j = 0; j < od->k; j++) {
            pp->key[(size_t)r * MAXNIC + j] = b->nic[j].key;
            if (r == me) continue;
            int rc = fi_av_insert(od->nic[j].av, b->nic[j].addr, 1, &pp->fa[(size_t)r * MAXNIC + j], 0, NULL);
            if (rc != 1) { pthread_mutex_unlock(&od->lock); ec_fatal(EC_RC_FATAL, "comm_ofi: fi_av_insert of member %d NIC %d on cxi%d: %d\n", r, j, od->nic[j].cxi, rc); }
        }
    }
    pthread_mutex_unlock(&od->lock);
    return pp;
}
void comm_ofi_peers_free(ofi_peers *pp) { if (!pp) return; free(pp->va); free(pp->key); free(pp->fa); free(pp); }   /* (the AV entries stay: a table AV, removed with the domain) */
/* under od->lock: retire every completion queued on the device's CQs */
static void progress_locked(ofi_dev *od)
{
    struct fi_cq_entry ce[64];
    for (int j = 0; j < od->k; j++) {
        struct nicx *x = &od->nic[j];
        for (;;) {
            ssize_t m = fi_cq_read(x->cq, ce, 64);
            if (m == -FI_EAGAIN) break;
            if (m < 0) {
                struct fi_cq_err_entry e; memset(&e, 0, sizeof e); fi_cq_readerr(x->cq, &e, 0);
                ec_fatal(EC_RC_FATAL, "comm_ofi: device %d cxi%d: completion error %zd: %s (prov %d: %s)\n", od->d, x->cxi, m, fi_strerror(e.err), e.prov_errno, fi_cq_strerror(x->cq, e.prov_errno, e.err_data, NULL, 0));
            }
            for (ssize_t i = 0; i < m; i++) __atomic_sub_fetch((long *)ce[i].op_context, 1, __ATOMIC_ACQ_REL);
            x->out -= (int)m; x->ops += m;
            if (m < 64) break;
        }
    }
}
void comm_ofi_progress(ofi_dev *od) { pthread_mutex_lock(&od->lock); progress_locked(od); pthread_mutex_unlock(&od->lock); }
void comm_ofi_write(ofi_peers *pp, int r, size_t roff, const void *src, size_t n, long *cnt)
{
    ofi_dev *od = pp->od; int k = od->k;
    if (!n) return;
    if (!comm_ofi_in_pool(od, src) || !comm_ofi_in_pool(od, (const char *)src + n - 1)) ec_fatal(EC_RC_FATAL, "comm_ofi: a write's source lies outside the comm pool of device %d\n", od->d);
    uint64_t base = pp->va[r] + roff; size_t i = 0;
    for (size_t o = 0; o < n; o += G.chunk, i++) {
        size_t len = n - o < G.chunk ? n - o : G.chunk; int j = (int)((i + (size_t)r) % (size_t)k); struct nicx *x = &od->nic[j];
        struct iovec iov = { (char *)src + o, len }; void *desc = x->desc;
        struct fi_rma_iov rio = { base + o, len, pp->key[(size_t)r * MAXNIC + j] };
        struct fi_msg_rma msg; memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov; msg.desc = &desc; msg.iov_count = 1; msg.addr = pp->fa[(size_t)r * MAXNIC + j]; msg.rma_iov = &rio; msg.rma_iov_count = 1; msg.context = cnt;
        __atomic_add_fetch(cnt, 1, __ATOMIC_ACQ_REL);
        pthread_mutex_lock(&od->lock);
        for (unsigned spins = 0;; spins++) {
            if (x->out >= G.window) { progress_locked(od); if (x->out >= G.window) { if (spins > 64) { pthread_mutex_unlock(&od->lock); sched_yield(); pthread_mutex_lock(&od->lock); } continue; } }
            ssize_t rc = fi_writemsg(x->ep, &msg, FI_COMPLETION | (od->dc ? FI_DELIVERY_COMPLETE : 0));
            if (rc == -FI_EAGAIN) { progress_locked(od); continue; }
            if (rc) { pthread_mutex_unlock(&od->lock); ec_fatal(EC_RC_FATAL, "comm_ofi: fi_writemsg of %zu B to member %d on cxi%d: %zd (%s)\n", len, r, x->cxi, rc, fi_strerror((int)-rc)); }
            break;
        }
        x->out++; x->bytes += (double)len;
        pthread_mutex_unlock(&od->lock);
    }
}
void comm_ofi_finalize(void)
{
    for (int d = 0; d < MAXDEV; d++) {
        ofi_dev *od = G.dev[d]; if (!od) continue;
        pthread_mutex_lock(&od->lock); progress_locked(od);
        if (G.verbose) {
            char l[320]; int n = snprintf(l, sizeof l, "comm_ofi: device %d: pool peak %.1f MiB of %zu (symmetric buffers %.1f);", d, od->peak / 1048576.0, od->bytes >> 20, od->sym_peak / 1048576.0);
            for (int j = 0; j < od->k && n < (int)sizeof l - 48; j++) n += snprintf(l + n, sizeof l - n, " cxi%d %.2f GB in %ld writes", od->nic[j].cxi, od->nic[j].bytes / 1e9, od->nic[j].ops);
            printf("%s\n", l);
        }
        for (int j = 0; j < od->k; j++) {
            struct nicx *x = &od->nic[j];
            fi_close(&x->mr->fid); fi_close(&x->ep->fid); fi_close(&x->av->fid); fi_close(&x->cq->fid); fi_close(&x->dom->fid); fi_close(&x->fab->fid); fi_freeinfo(x->info);
        }
#ifdef COMM_HOST_ONLY
        munmap(od->pool, od->bytes);
#else
        if (od->form == POOL_HOST) { (void)hipHostUnregister(od->pool); munmap(od->pool, od->bytes); } else (void)hipFree(od->pool);
#endif
        for (struct oblk *b = od->blocks, *nb; b; b = nb) { nb = b->next; free(b); }
        pthread_mutex_unlock(&od->lock);
        free(od); G.dev[d] = 0;
    }
}
#endif
