/* pe_geo.c -- per-PE topology (see pe_geo.h).  NIC logic was copied from the removed comm_ofi.c (dev_numa/nic_list; see archive:code_2026-10-25/). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pe_geo.h"
#include "env_int.h"
#ifndef PE_GEO_NOBOOT
#include <stdint.h>
#include "xshmem_boot.h"
#ifdef USE_HIP
#include <hip/hip_runtime_api.h>
#endif
#endif

static char g_err[256];
const char *pe_geo_err(void) { return g_err; }
#define FAIL(...) do { snprintf(g_err, sizeof g_err, __VA_ARGS__); return -1; } while (0)

static const char *root(void) { const char *r = getenv("PE_GEO_SYSFS_ROOT"); return r ? r : ""; }
static int read_int(const char *path, int def) { FILE *f = fopen(path, "r"); int v = def; if (f) { if (fscanf(f, "%d", &v) != 1) v = def; fclose(f); } return v; }

/* NUMA nodes present: highest nodeN dir + 1 (>= 1) */
static int numa_count(void)
{
    char p[256]; int n = 0;
    for (int i = 0; i < 256; i++) {
        snprintf(p, sizeof p, "%s/sys/devices/system/node/node%d", root(), i);
        if (access(p, F_OK) == 0) n = i + 1;
    }
    return n ? n : 1;
}

int pe_geo_from(pe_topo *t, int pe, int npes, int local, int ppn, int ngpu, const char *busid)
{
    char path[320];
    memset(t, 0, sizeof *t);
    if (npes < 1 || pe < 0 || pe >= npes) FAIL("pe %d outside npes %d", pe, npes);
    if (ppn < 1 || local < 0 || local >= ppn) FAIL("local %d outside pes per node %d", local, ppn);
    if (npes % ppn) FAIL("npes %d not a multiple of pes per node %d", npes, ppn);
    t->pe = pe; t->npes = npes; t->local = local; t->pes_per_node = ppn;
    t->node = pe / ppn; t->nnodes = npes / ppn;
    t->pes_per_gpu = ec_env_int_ne("XSHMEM_PES_PER_GPU", 1); if (t->pes_per_gpu < 1) FAIL("XSHMEM_PES_PER_GPU < 1");
    if (ppn % t->pes_per_gpu) FAIL("pes per node %d not a multiple of XSHMEM_PES_PER_GPU %d", ppn, t->pes_per_gpu);
    t->gpu_node = local / t->pes_per_gpu;
    t->gpu = ngpu >= 1 ? (ngpu == 1 ? 0 : t->gpu_node % ngpu) : -1;
    t->nlanes = ec_env_int_ne("XSHMEM_LANES", 1); if (t->nlanes < 1) FAIL("XSHMEM_LANES < 1");

    /* NUMA node of the GPU: PCI function's numa_node, else gpu_node modulo the NUMA nodes present (comm_ofi dev_numa fallback) */
    t->numa = -1;
    if (busid && *busid) { snprintf(path, sizeof path, "%s/sys/bus/pci/devices/%s/numa_node", root(), busid); t->numa = read_int(path, -1); }
    if (t->numa < 0) t->numa = t->gpu_node % numa_count();

    /* NICs: COMM_OFI_NICS entry of the node-local GPU, else the cxi devices on the GPU's NUMA node */
    int all[PE_GEO_MAXNIC], k = 0; const char *e = getenv("COMM_OFI_NICS");
    if (e && *e) {
        const char *s = e; for (int i = 0; i < t->gpu_node && s; i++) { s = strchr(s, ';'); if (s) s++; }
        if (!s || !*s || *s == ';') FAIL("COMM_OFI_NICS=%s names no NIC for GPU %d", e, t->gpu_node);
        while (*s && *s != ';' && k < PE_GEO_MAXNIC) { all[k++] = atoi(s); while (*s && *s != ',' && *s != ';') s++; if (*s == ',') s++; }
    } else {
        int ncxi = 0;
        for (int i = 0; i < 64; i++) {
            snprintf(path, sizeof path, "%s/sys/class/cxi/cxi%d/device/numa_node", root(), i);
            int n = read_int(path, -2); if (n == -2) continue;
            ncxi = i + 1; if (n == t->numa && k < PE_GEO_MAXNIC) all[k++] = i;
        }
        if (!k && ncxi) { all[k++] = t->gpu_node % ncxi; fprintf(stderr, "pe_geo: WARNING no cxi device on NUMA node %d of GPU %d: falling back to cxi%d\n", t->numa, t->gpu_node, all[0]); }   /* comm_ofi fallback */
    }
    /* several PEs on one GPU: each takes its own slice of the GPU's NICs when there are enough, else all share */
    int pj = local % t->pes_per_gpu, lo = 0, cnt = k;
    if (t->pes_per_gpu > 1 && k >= t->pes_per_gpu) { cnt = k / t->pes_per_gpu; lo = pj * cnt; }
    for (int i = 0; i < cnt; i++) t->nic[i] = all[lo + i];
    t->nics = cnt;

    snprintf(path, sizeof path, "%s/sys/devices/system/node/node%d/cpulist", root(), t->numa);
    FILE *f = fopen(path, "r");
    if (f) { if (fgets(t->cpus, sizeof t->cpus, f)) { size_t l = strlen(t->cpus); while (l && (t->cpus[l - 1] == '\n' || t->cpus[l - 1] == ' ')) t->cpus[--l] = 0; } fclose(f); }
    return 0;
}

void pe_geo_str(const pe_topo *t, char *buf, int n)
{
    char nl[96] = ""; int o = 0;
    for (int i = 0; i < t->nics && o < (int)sizeof nl - 8; i++) o += snprintf(nl + o, sizeof nl - o, "%s%d", i ? "," : "", t->nic[i]);
    snprintf(buf, n, "pe %d/%d node %d/%d local %d/%d gpu %d (node gpu %d) numa %d nics %d [%s] lanes %d cpus %s",
             t->pe, t->npes, t->node, t->nnodes, t->local, t->pes_per_node, t->gpu, t->gpu_node, t->numa, t->nics, nl, t->nlanes, t->cpus[0] ? t->cpus : "-");
}

#ifndef PE_GEO_NOBOOT
static uint64_t host_hash(void)
{
    char h[256] = { 0 }; gethostname(h, sizeof h - 1);
    uint64_t x = 1469598103934665603ull; for (const char *c = h; *c; c++) { x ^= (unsigned char)*c; x *= 1099511628211ull; }
    return x;
}
void pe_geo_init(pe_topo *t)
{
    int pe, npes, ppn = 0; boot_init(&pe, &npes);
    int local = ec_env_int_ne("SLURM_LOCALID", -1);
    uint64_t *hh = (uint64_t *)malloc((size_t)npes * sizeof *hh), me = host_hash();
    boot_allgather(&me, sizeof me, hh);
    int cnt = 0, first = -1; for (int r = 0; r < npes; r++) if (hh[r] == me) { if (first < 0) first = r; cnt++; }
    ppn = ec_env_int_ne("SLURM_NTASKS_PER_NODE", 0);
    if (!ppn) { const char *s = getenv("SLURM_STEP_TASKS_PER_NODE"); if (s && *s) { char *end; long v = strtol(s, &end, 10); if (v > 0 && (*end == 0 || !strncmp(end, "(x", 2))) ppn = (int)v; } }
    if (!ppn) ppn = cnt;
    if (local < 0) local = pe - first;
    /* block launch check: every PE of one node block shares the host, and the block has ppn members */
    int nodeb = pe / ppn;
    for (int r = nodeb * ppn; r < (nodeb + 1) * ppn && r < npes; r++)
        if (hh[r] != me) { fprintf(stderr, "pe_geo: PE %d is not on the host of PE %d: launch is not a block distribution with %d PEs per node\n", r, pe, ppn); abort(); }
    free(hh);
    int ngpu = 0; char *bp = NULL, bus[64] = {0}; (void)bus;
#ifdef USE_HIP
    if (hipGetDeviceCount(&ngpu) != hipSuccess) { (void)hipGetLastError(); ngpu = 0; }
    if (ngpu > 0) {
        int envp = ec_env_int_ne("XSHMEM_PES_PER_GPU", 1), gn = (local / (envp > 0 ? envp : 1));
        int dev = ngpu == 1 ? 0 : gn % ngpu;
        if (hipDeviceGetPCIBusId(bus, sizeof bus, dev) == hipSuccess) { for (char *c = bus; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 'a' - 'A'; bp = bus; }
        else (void)hipGetLastError();
    }
#endif
    if (pe_geo_from(t, pe, npes, local, ppn, ngpu, bp)) { fprintf(stderr, "pe_geo: %s\n", pe_geo_err()); abort(); }
    { char l[400]; pe_geo_str(t, l, sizeof l); if (pe == 0 || t->nics < 1) fprintf(stderr, "pe_geo: %s%s\n", l, t->nics < 1 ? "  WARNING: no NIC" : ""); }
}
#endif
