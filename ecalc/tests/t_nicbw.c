// nicbw.c -- one process drives N cxi NICs at once with raw libfabric RMA writes.
// Each process opens one fabric/domain/EP/CQ/AV per NIC; thread i drives NIC i.
// Peers: P processes (SLURM_PROCID/SLURM_NPROCS or -r/-P); address exchange via files in DIR.
// Thread i of rank r writes to rank (r+1)%P, NIC (i+shift)%N  (shift default: 1 if P==1 else 0).
// Usage: nicbw -n 0,1,2,3 -s bytes -w window -t seconds [-g] [-d dir] [-x shift] [-b]
//   -g : buffers from hipMalloc on the GPU on the NIC's NUMA node (FI_HMEM_ROCR)
//   -b : bidirectional is implicit in ring; -b not used
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/stat.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#ifdef USE_SHMEM
#include <shmem.h>
#endif
#ifdef USE_HIP
#include <hip/hip_runtime.h>
#endif

#define MAXN 16
#define CK(x) do { int _r = (int)(x); if (_r) { fprintf(stderr, "rank %d: %s = %d (%s) at %s:%d\n", g_rank, #x, _r, fi_strerror(-_r), __FILE__, __LINE__); exit(1);} } while (0)

static int g_uni = 0, g_vmm = 0, g_rc = 0, g_one = 0, g_rank = 0, g_np = 1, g_n = 0, g_nic[MAXN], g_win = 64, g_gpu = 0, g_shift = -1;
static size_t g_size = 1 << 20, g_bufsz;
static double g_secs = 5;
static const char *g_dir = ".";
static const char *g_tag = "x";

struct nic {
  int dev, numa;
  struct fi_info *info;
  struct fid_fabric *fab;
  struct fid_domain *dom;
  struct fid_ep *ep;
  struct fid_cq *cq;
  struct fid_av *av;
  struct fid_mr *mr;
  struct fid_cntr *rcntr;
  void *buf;
  void *desc;
  char addr[64];
  size_t addrlen;
  uint64_t key;
  // peer
  fi_addr_t peer;
  uint64_t pkey, pbase;
  double gbs; uint64_t bytes, ops;
};
static struct nic N[MAXN];

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }

static int numa_of(int dev) {
  char p[128]; snprintf(p, sizeof p, "/sys/class/cxi/cxi%d/device/numa_node", dev);
  FILE *f = fopen(p, "r"); int n = 0; if (f) { if (fscanf(f, "%d", &n) != 1) n = 0; fclose(f);} return n < 0 ? 0 : n;
}
static void pin_numa(int numa) {
  char p[128], s[4096]; snprintf(p, sizeof p, "/sys/devices/system/node/node%d/cpulist", numa);
  FILE *f = fopen(p, "r"); if (!f) return; if (!fgets(s, sizeof s, f)) { fclose(f); return; } fclose(f);
  cpu_set_t cs; CPU_ZERO(&cs); char *t = strtok(s, ",\n");
  while (t) { int a, b; if (sscanf(t, "%d-%d", &a, &b) == 2) { for (int i = a; i <= b; i++) CPU_SET(i, &cs);} else CPU_SET(atoi(t), &cs); t = strtok(NULL, ",\n"); }
  pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
}

static void open_nic(struct nic *x) {
  struct fi_info *h = fi_allocinfo();
  char name[16]; snprintf(name, sizeof name, "cxi%d", x->dev);
  h->fabric_attr->prov_name = strdup("cxi");
  h->domain_attr->name = strdup(name);
  h->ep_attr->type = FI_EP_RDM;
  h->caps = FI_RMA | FI_MSG | (g_gpu ? FI_HMEM : 0) | (g_rc ? FI_RMA_EVENT : 0);
  h->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_ENDPOINT | (g_gpu ? FI_MR_HMEM : 0);
  h->domain_attr->threading = FI_THREAD_DOMAIN;
  h->tx_attr->size = 4096;
  CK(fi_getinfo(FI_VERSION(1, 20), NULL, NULL, 0, h, &x->info));
  fi_freeinfo(h);
  CK(fi_fabric(x->info->fabric_attr, &x->fab, NULL));
  CK(fi_domain(x->fab, x->info, &x->dom, NULL));
  struct fi_cq_attr cqa = { .format = FI_CQ_FORMAT_CONTEXT, .size = 8192 };
  CK(fi_cq_open(x->dom, &cqa, &x->cq, NULL));
  struct fi_av_attr ava = { .type = FI_AV_TABLE };
  CK(fi_av_open(x->dom, &ava, &x->av, NULL));
  CK(fi_endpoint(x->dom, x->info, &x->ep, NULL));
  CK(fi_ep_bind(x->ep, &x->cq->fid, FI_TRANSMIT | FI_RECV));
  CK(fi_ep_bind(x->ep, &x->av->fid, 0));
  CK(fi_enable(x->ep));
  x->addrlen = sizeof x->addr;
  CK(fi_getname(&x->ep->fid, x->addr, &x->addrlen));
  // buffer
  if (g_gpu) {
#ifdef USE_HIP
    int nd = 0; hipGetDeviceCount(&nd);
    int gd = x->numa % (nd ? nd : 1);
    hipError_t he = hipSetDevice(gd); if (he == hipSuccess) he = hipMalloc(&x->buf, g_bufsz);
    if (g_vmm && he == hipSuccess) { // VMM: hipMemCreate + hipMemMap (as ecalc's dbig arena)
      hipFree(x->buf); x->buf = NULL;
      hipMemAllocationProp prop = {0}; prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = gd;
      size_t gran = 0; hipMemGetAllocationGranularity(&gran, &prop, hipMemAllocationGranularityRecommended); if (!gran) gran = 2 << 20;
      g_bufsz = (g_bufsz + gran - 1) / gran * gran;
      hipMemGenericAllocationHandle_t hd; void *va = NULL;
      he = hipMemCreate(&hd, g_bufsz, &prop, 0);
      if (he == hipSuccess) he = hipMemAddressReserve(&va, g_bufsz, gran, NULL, 0);
      if (he == hipSuccess) he = hipMemMap(va, g_bufsz, 0, hd, 0);
      hipMemAccessDesc ad = {0}; ad.location.type = hipMemLocationTypeDevice; ad.location.id = gd; ad.flags = hipMemAccessFlagsProtReadWrite;
      if (he == hipSuccess) he = hipMemSetAccess(va, g_bufsz, &ad, 1);
      x->buf = va;
    }
    if (he != hipSuccess) { fprintf(stderr, "hip failed nd=%d gd=%d: %s\n", nd, gd, hipGetErrorString(he)); exit(1); }
    hipMemset(x->buf, 1, g_bufsz); hipDeviceSynchronize();
    struct iovec iov = { x->buf, g_bufsz };
    struct fi_mr_attr ma = { .mr_iov = &iov, .iov_count = 1, .access = FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE,
                             .iface = getenv("NICBW_SYS") ? FI_HMEM_SYSTEM : FI_HMEM_ROCR, .device.reserved = gd };
    CK(fi_mr_regattr(x->dom, &ma, 0, &x->mr));
#else
    fprintf(stderr, "built without HIP\n"); exit(1);
#endif
  } else {
#ifdef USE_HIP
    if (getenv("NICBW_HHM")) { if (hipHostMalloc(&x->buf, g_bufsz, getenv("NICBW_HHM")[0] == 'n' ? hipHostMallocNumaUser : 0) != hipSuccess) { fprintf(stderr, "hipHostMalloc failed\n"); exit(1);} } else
#endif
    if (x->buf) memset(x->buf, 1, g_bufsz); else   /* preset: a slice of the SHMEM symmetric heap (NICBW_SHHEAP) */
    CK(posix_memalign(&x->buf, 1 << 21, g_bufsz));
    memset(x->buf, 1, g_bufsz);
    CK(fi_mr_reg(x->dom, x->buf, g_bufsz, FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE, 0, 0, 0, &x->mr, NULL));
  }
  if (g_rc) { struct fi_cntr_attr ca = { .events = FI_CNTR_EVENTS_COMP }; CK(fi_cntr_open(x->dom, &ca, &x->rcntr, NULL)); CK(fi_mr_bind(x->mr, &x->rcntr->fid, FI_REMOTE_WRITE)); }
  if (x->info->domain_attr->mr_mode & FI_MR_ENDPOINT) { CK(fi_mr_bind(x->mr, &x->ep->fid, 0)); CK(fi_mr_enable(x->mr)); }
  x->desc = fi_mr_desc(x->mr);
  x->key = fi_mr_key(x->mr);
}

static void *opener(void *a) { struct nic *x = a; pin_numa(x->numa); open_nic(x); return NULL; }

static void fbarrier(const char *what) {
  char p[512]; snprintf(p, sizeof p, "%s/%s.%s.%d", g_dir, g_tag, what, g_rank);
  FILE *f = fopen(p, "w"); fclose(f);
  for (int r = 0; r < g_np; r++) { snprintf(p, sizeof p, "%s/%s.%s.%d", g_dir, g_tag, what, r); struct stat st; while (stat(p, &st)) usleep(20000); }
}

static void *runner(void *a) {
  struct nic *x = a; pin_numa(x->numa);
  int out = 0; uint64_t ops = 0; size_t nslots = g_bufsz / g_size; size_t slot = 0;
  struct fi_cq_entry ce[64];
  double t0 = now(), tend = t0 + g_secs, t;
  if (g_uni && (g_rank & 1)) tend = t0;   /* -u: odd ranks only receive */
  while ((t = now()) < tend || out) {
    while (out < g_win && t < tend) {
      size_t off = (slot++ % nslots) * g_size;
      ssize_t r = fi_write(x->ep, (char *)x->buf + off, g_size, x->desc, x->peer, x->pbase + off, x->pkey, NULL);
      if (r == -FI_EAGAIN) break;
      if (r) { fprintf(stderr, "fi_write %zd %s\n", r, fi_strerror(-r)); exit(1);} out++;
    }
    ssize_t n = fi_cq_read(x->cq, ce, 64);
    if (n > 0) { out -= n; ops += n; }
    else if (n != -FI_EAGAIN) {
      struct fi_cq_err_entry e = {0}; fi_cq_readerr(x->cq, &e, 0);
      fprintf(stderr, "rank %d cxi%d cq err %zd: %s prov %d %s\n", g_rank, x->dev, n, fi_strerror(e.err), e.prov_errno,
              fi_cq_strerror(x->cq, e.prov_errno, e.err_data, NULL, 0)); exit(1);
    }
  }
  double dt = now() - t0;
  x->ops = ops; x->bytes = ops * g_size; x->gbs = x->bytes / dt / 1e9;
  return NULL;
}

// single thread drives all NICs round-robin
static void run_one(void) {
  int out[MAXN] = {0}; uint64_t ops[MAXN] = {0}; size_t slot[MAXN] = {0}; size_t nslots = g_bufsz / g_size;
  struct fi_cq_entry ce[64];
  double t0 = now(), tend = t0 + g_secs, t; int any;
  do { t = now(); any = 0;
    for (int i = 0; i < g_n; i++) { struct nic *x = &N[i];
      while (out[i] < g_win && t < tend) {
        size_t off = (slot[i]++ % nslots) * g_size;
        ssize_t r = fi_write(x->ep, (char *)x->buf + off, g_size, x->desc, x->peer, x->pbase + off, x->pkey, NULL);
        if (r == -FI_EAGAIN) break; if (r) { fprintf(stderr, "fi_write %zd\n", r); exit(1);} out[i]++; }
      ssize_t n = fi_cq_read(x->cq, ce, 64);
      if (n > 0) { out[i] -= n; ops[i] += n; } else if (n != -FI_EAGAIN) { fprintf(stderr, "cq err %zd\n", n); exit(1);}
      any |= out[i]; }
  } while (t < tend || any);
  double dt = now() - t0;
  for (int i = 0; i < g_n; i++) { N[i].ops = ops[i]; N[i].bytes = ops[i] * g_size; N[i].gbs = N[i].bytes / dt / 1e9; }
}

int main(int argc, char **argv) {
  const char *nl = "0,1,2,3"; int c;
  char *e;
#ifdef USE_SHMEM
  // hybrid: the process is also a Cray OpenSHMEMX PE (one NIC); the extra domains are opened beside it
  shmem_init(); { static long *sp; sp = shmem_malloc(1 << 20); sp[0] = shmem_my_pe(); shmem_barrier_all(); }
#endif
  if ((e = getenv("SLURM_PROCID"))) g_rank = atoi(e);
  if ((e = getenv("SLURM_NPROCS"))) g_np = atoi(e);
  while ((c = getopt(argc, argv, "n:s:w:t:gd:x:T:r:P:1VCu")) != -1) switch (c) {
    case 'n': nl = optarg; break; case 's': g_size = strtoull(optarg, 0, 0); break; case 'w': g_win = atoi(optarg); break;
    case 't': g_secs = atof(optarg); break; case 'g': g_gpu = 1; break; case 'd': g_dir = optarg; break;
    case 'x': g_shift = atoi(optarg); break; case 'T': g_tag = optarg; break; case 'r': g_rank = atoi(optarg); break; case 'P': g_np = atoi(optarg); break; case '1': g_one = 1; break; case 'V': g_vmm = 1; g_gpu = 1; break; case 'C': g_rc = 1; break; case 'u': g_uni = 1; break;
  }
  char tmp[256]; strncpy(tmp, nl, 255); for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) g_nic[g_n++] = atoi(t);
  if (g_shift < 0) g_shift = g_np == 1 ? 1 : 0;
  g_bufsz = g_size * (size_t)(g_win < 16 ? 16 : g_win); if (g_bufsz < (64u << 20)) g_bufsz = 64u << 20;
  pthread_t th[MAXN];
  double t0 = now();
#ifdef USE_SHMEM
  if (getenv("NICBW_SHHEAP")) { char *h = shmem_malloc(g_bufsz * g_n); if (!h) { fprintf(stderr, "shmem_malloc failed\n"); exit(1);} for (int i = 0; i < g_n; i++) N[i].buf = h + i * g_bufsz; printf("buffers in the SHMEM heap at %p\n", (void *)h); }
#endif
  for (int i = 0; i < g_n; i++) { N[i].dev = g_nic[i]; N[i].numa = numa_of(g_nic[i]); pthread_create(&th[i], 0, opener, &N[i]); }
  for (int i = 0; i < g_n; i++) pthread_join(th[i], 0);
  double topen = now() - t0;
  // publish
  char p[512]; snprintf(p, sizeof p, "%s/%s.addr.%d", g_dir, g_tag, g_rank);
  FILE *f = fopen(p, "wb");
  for (int i = 0; i < g_n; i++) { uint64_t base = (N[i].info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) ? (uint64_t)N[i].buf : 0;
    fwrite(&N[i].addrlen, sizeof(size_t), 1, f); fwrite(N[i].addr, 1, 64, f); fwrite(&N[i].key, 8, 1, f); fwrite(&base, 8, 1, f); }
  fclose(f);
  fbarrier("pub");
  int pr = (g_rank + 1) % g_np;
  snprintf(p, sizeof p, "%s/%s.addr.%d", g_dir, g_tag, pr);
  f = fopen(p, "rb");
  struct { size_t len; char a[64]; uint64_t key, base; } pe[MAXN];
  for (int i = 0; i < g_n; i++) { if (fread(&pe[i].len, sizeof(size_t), 1, f) != 1 || fread(pe[i].a, 1, 64, f) != 64 || fread(&pe[i].key, 8, 1, f) != 1 || fread(&pe[i].base, 8, 1, f) != 1) { fprintf(stderr, "short addr file\n"); exit(1);} }
  fclose(f);
  for (int i = 0; i < g_n; i++) { int j = (i + g_shift) % g_n;
    if (fi_av_insert(N[i].av, pe[j].a, 1, &N[i].peer, 0, NULL) != 1) { fprintf(stderr, "av_insert failed\n"); exit(1);}
    N[i].pkey = pe[j].key; N[i].pbase = pe[j].base; }
  fbarrier("go");
  if (g_one) run_one(); else {
  for (int i = 0; i < g_n; i++) pthread_create(&th[i], 0, runner, &N[i]);
  for (int i = 0; i < g_n; i++) pthread_join(th[i], 0); }
  fbarrier("done");
  double tot = 0;
  printf("rank %d/%d %s%s size %zu win %d secs %.1f open %.3fs:", g_rank, g_np, g_gpu ? "GPU" : "host", g_one ? " 1thread" : "", g_size, g_win, g_secs, topen);
  for (int i = 0; i < g_n; i++) { printf(" cxi%d->r%d.cxi%d %.2f", N[i].dev, pr, g_nic[(i + g_shift) % g_n], N[i].gbs); tot += N[i].gbs; }
  printf(" | total %.2f GB/s%s%s\n", tot, g_vmm ? " (VMM)" : "", getenv("NICBW_HHM") && !g_gpu ? " (hipHostMalloc)" : "");
  if (g_rc) { printf("rank %d remote-write counters:", g_rank); for (int i = 0; i < g_n; i++) printf(" cxi%d %lu (sent %lu)", N[i].dev, (unsigned long)fi_cntr_read(N[i].rcntr), (unsigned long)N[i].ops); printf("\n"); }
#ifdef USE_SHMEM
  { // prove SHMEM still works after the extra domains: a putmem ring of 64 MiB, timed
    size_t B = 64 << 20; char *sb = shmem_malloc(B); memset(sb, 1, B); shmem_barrier_all();
    double t = now(); for (int k = 0; k < 16; k++) shmem_putmem(sb, sb, B, (shmem_my_pe() + 1) % shmem_n_pes()); shmem_quiet(); t = now() - t;
    shmem_barrier_all(); printf("rank %d shmem putmem ring 16x64MiB: %.2f GB/s\n", g_rank, 16.0 * B / t / 1e9); shmem_free(sb); }
#endif
  for (int i = 0; i < g_n; i++) { fi_close(&N[i].mr->fid); if (N[i].rcntr) fi_close(&N[i].rcntr->fid); fi_close(&N[i].ep->fid); fi_close(&N[i].av->fid); fi_close(&N[i].cq->fid); fi_close(&N[i].dom->fid); fi_close(&N[i].fab->fid); fi_freeinfo(N[i].info); }
#ifdef USE_SHMEM
  shmem_finalize();
#endif
  return 0;
}
