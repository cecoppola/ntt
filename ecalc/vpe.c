/* vpe.c - virtual PEs: identity, launcher, state index, logging (see vpe.h; stage S1a of the TARGET1 plan).
 * The PE communicators (vpe_comm, vpe_sub) are in mn.c, the NUMA lookup in mem.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <hip/hip_runtime.h>
#include "fatal.h"
#include "mem.h"
#include "vpe.h"

__thread int vpe_t_idx = 0;
static __thread int t_in_run = 0;
static int g_on = -1, g_forced = 0, g_lanes = 0, g_node = -1, g_nnodes = -1;
static void (*g_hook)(int);
static pthread_barrier_t g_bar; static int g_bar_init;

int vpe_on(void)
{
    if (g_forced) return 1;
    int on = __atomic_load_n(&g_on, __ATOMIC_ACQUIRE);
    if (on < 0) {
        const char *e = getenv("ECALC_VPE"); on = e && atoi(e) != 0;
        if (on) mem_set_numa_pci(1);                     /* the device -> NUMA node lookup by PCI bus id (mem.c) */
        __atomic_store_n(&g_on, on, __ATOMIC_RELEASE);
    }
    return on;
}
void vpe_set_forced(int mask) { g_forced = mask; if (mask) mem_set_numa_pci(1); }
int vpe_lanes(void)
{
    if (!vpe_on()) return 1;
    if (!g_lanes) {
        const char *e = getenv("ECALC_VPE_LANES"); int l = e ? atoi(e) : 4;
        if (l < 1 || l > VPE_MAX) ec_fatal(EC_RC_FATAL, "ECALC_VPE_LANES=%s: 1..%d\n", e ? e : "", VPE_MAX);
        g_lanes = l;
    }
    return g_lanes;
}
int vpe_lane(void) { return vpe_t_idx; }
void vpe_set_nodes(int node, int nnodes) { g_node = node; g_nnodes = nnodes; }
int vpe_node(void)
{
    if (g_node >= 0) return g_node;
    const char *e = getenv("COMM_RANK"); return e ? atoi(e) : 0;
}
int vpe_nnodes(void)
{
    if (g_nnodes >= 0) return g_nnodes;
    const char *e = getenv("COMM_SIZE"); int n = e ? atoi(e) : 1; return n < 1 ? 1 : n;
}
int vpe_pe(void) { return vpe_lanes() * vpe_node() + vpe_t_idx; }
int vpe_npe(void) { return vpe_lanes() * vpe_nnodes(); }
int vpe_in_run(void) { return t_in_run; }
void vpe_set_thread_hook(void (*fn)(int)) { g_hook = fn; }

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
void vpe_adopt(int idx)
{
    if (!vpe_on()) return;
    if (idx < 0 || idx >= vpe_lanes()) ec_fatal(EC_RC_FATAL, "vpe_adopt: index %d of %d\n", idx, vpe_lanes());
    vpe_t_idx = idx; HIP_CHECK(hipSetDevice(idx));
    if (g_hook) g_hook(idx);
}
void vpe_stages_check(void)
{
    if (!vpe_on() || g_forced) return;
    int miss = VPE_S_ALL & ~VPE_STAGES_MERGED;
    if (!miss) return;
    char b[32]; int k = 0;
    for (int i = 0; i < 5; i++) if (miss >> i & 1) { if (k) b[k++] = ','; b[k++] = (char)('b' - 1 + i); }
    b[k] = 0;
    ec_fatal(EC_RC_FATAL, "ECALC_VPE: stages %s not merged (ECALC_VPE=1 runs only when S1a..S1e are all in; unit tests use vpe_set_forced)\n", b);
}

struct vrun { void (*fn)(void *); void *arg; int idx; };
static void *vthread(void *p)
{
    struct vrun *r = (struct vrun *)p;
    t_in_run = 1; vpe_adopt(r->idx);
    r->fn(r->arg);
    return 0;
}
void vpe_run(void (*fn)(void *), void *arg)
{
    if (!vpe_on()) { fn(arg); return; }
    if (t_in_run) ec_fatal(EC_RC_FATAL, "vpe_run inside a vPE thread\n");
    int L = vpe_lanes();
    if (g_bar_init) pthread_barrier_destroy(&g_bar);
    pthread_barrier_init(&g_bar, 0, (unsigned)L); g_bar_init = 1;
    pthread_t th[VPE_MAX]; struct vrun r[VPE_MAX]; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, (size_t)64 << 20);
    for (int i = 0; i < L; i++) {
        r[i].fn = fn; r[i].arg = arg; r[i].idx = i;
        if (pthread_create(&th[i], &at, vthread, &r[i])) ec_fatal(EC_RC_FATAL, "vpe_run: pthread_create %d\n", i);
    }
    for (int i = 0; i < L; i++) pthread_join(th[i], 0);
    pthread_attr_destroy(&at);
}
void vpe_node_barrier(void) { if (vpe_on() && t_in_run) pthread_barrier_wait(&g_bar); }

const char *ec_log_prefix(void)
{
    static __thread char b[64];
    if (!vpe_on() || !t_in_run) return "";
    snprintf(b, sizeof b, "pe %d (node %d lane %d): ", vpe_pe(), vpe_node(), vpe_t_idx);
    return b;
}
int vpe_root(void) { return vpe_pe() == 0; }
int vpe_node_leader(void) { return !vpe_on() || vpe_t_idx == 0; }
