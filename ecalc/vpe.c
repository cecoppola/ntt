/* vpe.c - virtual PEs: identity, launcher, state index, logging (see vpe.h; stage S1a of the TARGET1 plan).
 * The PE communicators (vpe_comm, vpe_sub) are in mn.c, the NUMA lookup in mem.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <hip/hip_runtime.h>
#include "fatal.h"
#include "mem.h"
#include "vpe.h"

__thread int vpe_t_idx = 0;
int vpe_g_idx = -1;
static __thread int t_in_run = 0;
static int g_on = -1, g_forced = 0, g_lanes = 0, g_node = -1, g_nnodes = -1;
static int g_ppg, g_ppg_local = -1, g_dev = -1;       /* S2: process mode; SLURM_LOCALID; pe_geo's device */
static void (*g_hook)(int);
static pthread_barrier_t g_bar; static int g_bar_init;

/* S2: ECALC_PE_PER_GPU and SLURM_LOCALID, read at process start (a constructor: nothing else runs, nothing is printed or fatal here) so that vpe_idx() is right
 * on every thread from the first line of main; the checks are in vpe_lanes() / mn_init */
void vpe_ppg_init(void)
{
    const char *e = getenv("ECALC_PE_PER_GPU"), *l = getenv("SLURM_LOCALID");
    g_ppg = e && atoi(e) != 0; g_ppg_local = l && *l ? atoi(l) : -1;
    vpe_g_idx = g_ppg && g_ppg_local >= 0 ? g_ppg_local : -1;
    if (vpe_g_idx >= 0) vpe_t_idx = vpe_g_idx;
    if (g_ppg) mem_set_numa_pci(1);                    /* (a plain store: the device -> NUMA node lookup by PCI bus id, mem.c) */
}
static int g_early_dev = -1;
int vpe_early_device(void) { return g_early_dev; }
/* S2: a process-per-PE launch makes this process's device current at process start (before any rns_init / pool of a harness or the flow runs on the main thread): its local id with all
 * GPUs visible (--gpu-bind=none), device 0 with one visible.  mn_init_ppg checks it against the engine's (pe_geo) device: a mismatch is fatal. */
__attribute__((constructor)) static void vpe_ctor(void)
{
    vpe_ppg_init();
    if (!g_ppg || vpe_g_idx < 0) return;
    const char *pg = getenv("XSHMEM_PES_PER_GPU"); int ppgpu = pg && atoi(pg) > 0 ? atoi(pg) : 1, n = 0;
    if (hipGetDeviceCount(&n) != hipSuccess || n < 1) { (void)hipGetLastError(); return; }
    int d = n == 1 ? 0 : (vpe_g_idx / ppgpu) % n;
    if (hipSetDevice(d) == hipSuccess) { g_dev = d; g_early_dev = d; } else (void)hipGetLastError();
}
void vpe_set_ppg(int local)
{
    if (local < 0) { vpe_ppg_init(); g_lanes = 0; return; }
    g_ppg = 1; g_ppg_local = local; vpe_g_idx = local; vpe_t_idx = local; g_lanes = 0; mem_set_numa_pci(1);
}
int vpe_ppg(void) { return g_ppg; }
int vpe_lpp(void) { return vpe_ppg() ? 1 : vpe_lanes(); }
void vpe_set_device(int dev) { g_dev = dev; }
int vpe_device(void) { return g_ppg && g_dev >= 0 ? g_dev : vpe_idx(); }
int vpe_on(void)
{
    if (g_forced || g_ppg) return 1;
    int on = __atomic_load_n(&g_on, __ATOMIC_ACQUIRE);
    if (on < 0) {
        const char *e = getenv("ECALC_VPE"); on = e && atoi(e) != 0;
        if (e && *e) { char *end; (void)strtol(e, &end, 10); if (*end) fprintf(stderr, "warning: ECALC_VPE=%s is not a number: treated as off\n", e); }
        if (on) mem_set_numa_pci(1);                     /* the device -> NUMA node lookup by PCI bus id (mem.c) */
        __atomic_store_n(&g_on, on, __ATOMIC_RELEASE);
    }
    return on;
}
void vpe_set_forced(int mask) { g_forced = mask; if (mask) mem_set_numa_pci(1); }
/* PEs per node of a process-per-PE launch: SLURM_NTASKS_PER_NODE (srun --ntasks-per-node), else a plain SLURM_STEP_TASKS_PER_NODE ("4" or "4(x3)"); 0 = unknown */
static int slurm_ppn(void)
{
    const char *e = getenv("SLURM_NTASKS_PER_NODE"); if (e && *e) return atoi(e);
    e = getenv("SLURM_STEP_TASKS_PER_NODE");
    if (e && *e) { char *end; long v = strtol(e, &end, 10); if (v > 0 && (*end == 0 || !strncmp(end, "(x", 2))) return (int)v; }
    return 0;
}
int vpe_lanes(void)
{
    if (!vpe_on()) return 1;
    if (!g_lanes) {
        const char *e = getenv("ECALC_VPE_LANES"); int l;
        if (g_ppg) {                                     /* S2: PEs per node = the launch's tasks per node (never a literal); ECALC_VPE_LANES, if set, must agree */
            int sp = slurm_ppn(); l = sp > 0 ? sp : (e && *e ? atoi(e) : 0);
            if (l < 1) ec_fatal(EC_RC_FATAL, "ECALC_PE_PER_GPU=1: the PEs per node are unknown (srun --ntasks-per-node sets SLURM_NTASKS_PER_NODE)\n");
            if (sp > 0 && e && *e && atoi(e) != sp) ec_fatal(EC_RC_FATAL, "ECALC_PE_PER_GPU=1: ECALC_VPE_LANES=%s but the launch has %d tasks per node\n", e, sp);
            if (g_ppg_local < 0 || g_ppg_local >= l) ec_fatal(EC_RC_FATAL, "ECALC_PE_PER_GPU=1: SLURM_LOCALID=%d outside the %d PEs per node (one process per PE needs srun)\n", g_ppg_local, l);
        } else l = e ? atoi(e) : 4;
        if (l < 1 || l > VPE_MAX) ec_fatal(EC_RC_FATAL, "ECALC_VPE_LANES=%s: 1..%d\n", e ? e : "", VPE_MAX);
        g_lanes = l;
    }
    return g_lanes;
}
int vpe_lane(void) { return vpe_idx(); }
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
int vpe_pe(void) { return vpe_lanes() * vpe_node() + vpe_idx(); }
int vpe_npe(void) { return vpe_lanes() * vpe_nnodes(); }
int vpe_in_run(void) { return t_in_run; }
void vpe_set_thread_hook(void (*fn)(int)) { g_hook = fn; }

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
void vpe_adopt(int idx)
{
    if (!vpe_on()) return;
    if (idx < 0 || idx >= vpe_lanes()) ec_fatal(EC_RC_FATAL, "vpe_adopt: index %d of %d\n", idx, vpe_lanes());
    if (g_ppg) {                                         /* S2: a helper thread of THE PE of this process: its index is the process's, the engine has one PE (K = 1) */
        if (idx != vpe_g_idx) ec_fatal(EC_RC_FATAL, "vpe_adopt: index %d, this process is PE %d of its node\n", idx, vpe_g_idx);
        vpe_t_idx = idx; HIP_CHECK(hipSetDevice(vpe_device()));
        if (g_hook) g_hook(0);
        return;
    }
    vpe_t_idx = idx; HIP_CHECK(hipSetDevice(idx));
    if (g_hook) g_hook(idx);
}
void vpe_stages_check(void)
{
    if (!vpe_on() || g_forced) return;
    if (g_ppg) {
        int pm = VPE_P_ALL & ~VPE_PSTAGES_MERGED;
        if (pm) {
            char b[32]; int k = 0;
            for (int i = 0; i < 4; i++) if (pm >> i & 1) { if (k) b[k++] = ','; b[k++] = (char)('a' + i); }
            b[k] = 0;
            ec_fatal(EC_RC_FATAL, "ECALC_PE_PER_GPU: stages %s not merged (the switch runs only when S2a..S2d are all in; unit tests use vpe_set_forced)\n", b);
        }
    }
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
    if (g_ppg) { int was = t_in_run; t_in_run = 1; fn(arg); t_in_run = was; return; }   /* S2: this process is the one PE: no threads */
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
void vpe_node_barrier(void)
{
    if (!vpe_on()) return;
    if (g_ppg) {                                         /* S2: the node's PEs are processes: a barrier of the node group (aligned run of ppn PEs, one id slot of its own) */
        int g = vpe_lanes(); if (g < 2 || !vpe_comm()) return;
        int al = (g & (g - 1)) == 0; comm *c = vpe_sub_slot(g * vpe_node(), g, al ? VPE_NODE_SLOT : 3);
        if (c) comm_barrier(c);
        return;
    }
    if (t_in_run) pthread_barrier_wait(&g_bar);
}

const char *ec_log_prefix(void)
{
    static __thread char b[64];
    if (!vpe_on() || (!t_in_run && !g_ppg)) return "";
    snprintf(b, sizeof b, "pe %d (node %d lane %d): ", vpe_pe(), vpe_node(), vpe_idx());
    return b;
}
int vpe_root(void) { return vpe_pe() == 0; }
int vpe_node_leader(void) { return !vpe_on() || vpe_idx() == 0; }
int vpe_proc_leader(void) { return !vpe_on() || g_ppg || vpe_idx() == 0; }

/* S2: this process (all its threads, the existing ones now, the later ones by inheritance) onto the cpus of its PE's NUMA node: pe_geo's cpulist ("0-47,96-143") intersected with
 * the launch mask; an empty intersection leaves the mask alone (a launch that binds differently is the user's: warning).  The default memory policy then places first-touch pages on
 * that node.  One line per process. */
void vpe_pe_affinity(const char *cpulist)
{
    cpu_set_t cur, want, set; CPU_ZERO(&want);
    if (sched_getaffinity(0, sizeof cur, &cur)) return;
    int before = CPU_COUNT(&cur);
    for (const char *p = cpulist ? cpulist : ""; *p; ) {
        char *end; long lo = strtol(p, &end, 10); if (end == p) break; long hi = lo;
        if (*end == '-') { p = end + 1; hi = strtol(p, &end, 10); }
        for (long c = lo; c <= hi && c < CPU_SETSIZE; c++) CPU_SET((int)c, &want);
        p = end; if (*p == ',') p++;
    }
    CPU_AND(&set, &cur, &want);
    int after = CPU_COUNT(&set);
    if (after == 0) { fprintf(stderr, "%swarning: launch mask (%d cpus) and the NUMA node's cpus '%s' do not intersect: affinity left as launched\n", ec_log_prefix(), before, cpulist ? cpulist : ""); return; }
    if (after != before) {
        DIR *d = opendir("/proc/self/task");
        if (d) { struct dirent *e; while ((e = readdir(d))) { int tid = atoi(e->d_name); if (tid > 0) (void)sched_setaffinity((pid_t)tid, sizeof set, &set); } closedir(d); }
    }
    fprintf(stderr, "%saffinity: %d cpus at launch -> %d (NUMA node cpus '%s')\n", ec_log_prefix(), before, after, cpulist ? cpulist : "");
}
