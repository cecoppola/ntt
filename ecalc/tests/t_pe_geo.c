/* t_pe_geo.c -- pe_geo on mock sysfs trees (host only, no GPU, no boot), and with `live` on a real launch.
 * Build: make XSHMEM=ofi tests/t_pe_geo (host-only cc: -DPE_GEO_NOBOOT -I.. t_pe_geo.c ../pe_geo.c)     Run: ./t_pe_geo   (prints "t_pe_geo: N checks, 0 failed")
 * live (boot over PMI2 + pe_geo, one line per PE, rank 0 the verdict; expected shape in results/XSHMEM.md):
 *   srun -N<n> --ntasks-per-node=<p> ... ./tests/t_pe_geo live
 * Checks: boot allgather of a 3000 B blob (chunking) returns every rank's bytes; all PEs agree on nics; no two PEs of a node share a NIC when pes_per_gpu == 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pe_geo.h"
#ifndef PE_GEO_NOBOOT
#include "xshmem_boot.h"
#endif

static int nchk, nbad;
#define CHECK(c) do { nchk++; if (!(c)) { nbad++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char base[256];
static void mk(const char *rel) { char p[512], q[512]; snprintf(p, sizeof p, "%s/%s", base, rel); snprintf(q, sizeof q, "%s", p);
    for (char *c = q + strlen(base) + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(q, 0755); *c = '/'; } mkdir(q, 0755); }
static void wr(const char *rel, const char *txt) { char p[512]; snprintf(p, sizeof p, "%s/%s", base, rel); FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); } fputs(txt, f); fclose(f); }
/* ncxi cxi devices, device i on NUMA i % nnuma; nnuma NUMA nodes, node n cpus [n*48, n*48+47]; GPU g bus 0000:0(g+1):00.0 on NUMA g */
static void tree(int nnuma, int ncxi, int ngpu)
{
    char rel[256], v[64];
    for (int n = 0; n < nnuma; n++) { snprintf(rel, sizeof rel, "sys/devices/system/node/node%d", n); mk(rel);
        snprintf(rel, sizeof rel, "sys/devices/system/node/node%d/cpulist", n); snprintf(v, sizeof v, "%d-%d\n", n * 48, n * 48 + 47); wr(rel, v); }
    for (int i = 0; i < ncxi; i++) { snprintf(rel, sizeof rel, "sys/class/cxi/cxi%d/device", i); mk(rel);
        snprintf(rel, sizeof rel, "sys/class/cxi/cxi%d/device/numa_node", i); snprintf(v, sizeof v, "%d\n", i % nnuma); wr(rel, v); }
    for (int g = 0; g < ngpu; g++) { snprintf(rel, sizeof rel, "sys/bus/pci/devices/0000:0%d:00.0", g + 1); mk(rel);
        snprintf(rel, sizeof rel, "sys/bus/pci/devices/0000:0%d:00.0/numa_node", g + 1); snprintf(v, sizeof v, "%d\n", g % nnuma); wr(rel, v); }
}
static int newroot(const char *name)
{
    const char *tmp = getenv("TMPDIR"); if (!tmp) tmp = "/tmp";
    snprintf(base, sizeof base, "%s/pegeo_%d_%s", tmp, (int)getpid(), name); mkdir(base, 0755);
    setenv("PE_GEO_SYSFS_ROOT", base, 1); return 0;
}
static void rmroot(void) { char c[600]; snprintf(c, sizeof c, "rm -rf '%s'", base); if (system(c)) {} }
static void clearenv_all(void) { unsetenv("COMM_OFI_NICS"); unsetenv("XSHMEM_LANES"); unsetenv("XSHMEM_PES_PER_GPU"); }
static char bus(int g, char *b) { sprintf(b, "0000:0%d:00.0", g + 1); return 0; }

#ifndef PE_GEO_NOBOOT
static int live(void)
{
    pe_topo tv, *t = &tv; pe_geo_init(t);
    char s[400]; pe_geo_str(t, s, sizeof s);
    int bad = 0, len = 3000;
    unsigned char *blob = (unsigned char *)malloc(len), *all = (unsigned char *)malloc((size_t)t->npes * len);
    for (int i = 0; i < len; i++) blob[i] = (unsigned char)(t->pe * 31 + i * 7);
    boot_allgather(blob, len, all);
    for (int r = 0; r < t->npes; r++) for (int i = 0; i < len; i++) if (all[(size_t)r * len + i] != (unsigned char)(r * 31 + i * 7)) { bad++; break; }
    /* gather the topo structs */
    pe_topo *tt = (pe_topo *)malloc((size_t)t->npes * sizeof tv); boot_allgather(t, sizeof tv, tt);
    for (int r = 0; r < t->npes; r++) {
        if (tt[r].nics != t->nics) bad++;
        if (tt[r].node == t->node && r != t->pe && t->pes_per_gpu == 1 && tt[r].nics && t->nics && tt[r].nic[0] == t->nic[0] && t->nics == 1) { bad++; fprintf(stderr, "PE %d and %d share NIC %d\n", r, t->pe, t->nic[0]); }
    }
    printf("%s%s\n", s, bad ? "  BAD" : "");
    fflush(stdout); boot_fence();
    if (t->pe == 0) printf("t_pe_geo live: %d PEs, %s\n", t->npes, bad ? "problems on rank 0 (see BAD lines)" : "rank 0 ok");
    boot_finalize();
    return bad != 0;
}
#endif

int main(int argc, char **argv)
{
#ifndef PE_GEO_NOBOOT
    if (argc > 1 && !strcmp(argv[1], "live")) return live();
#else
    (void)argc; (void)argv;
#endif
    pe_topo tv, *t = &tv; char b[16];
    /* aac7 shape: 4 NUMA, 1 cxi each, 4 PEs per node, 2 nodes */
    newroot("aac7"); tree(4, 4, 4); clearenv_all();
    for (int pe = 0; pe < 8; pe++) { int l = pe % 4; bus(l, b);
        CHECK(!pe_geo_from(t, pe, 8, l, 4, 4, b));
        CHECK(t->node == pe / 4 && t->nnodes == 2 && t->local == l && t->gpu == l && t->numa == l && t->nics == 1 && t->nic[0] == l && t->nlanes == 1);
        char want[32]; snprintf(want, sizeof want, "%d-%d", l * 48, l * 48 + 47); CHECK(!strcmp(t->cpus, want)); }
    /* 1 PE per node, 4 GPUs visible: PE uses device 0 -> NUMA 0 */
    bus(0, b); CHECK(!pe_geo_from(t, 1, 3, 0, 1, 4, b)); CHECK(t->node == 1 && t->nnodes == 3 && t->gpu == 0 && t->nics == 1 && t->nic[0] == 0);
    /* --gpus-per-task=1: one visible device (ordinal 0), bus id of the real GPU given by the caller */
    bus(2, b); CHECK(!pe_geo_from(t, 2, 4, 2, 4, 1, b)); CHECK(t->gpu == 0 && t->gpu_node == 2 && t->numa == 2 && t->nic[0] == 2);
    /* no bus id: numa = node gpu % NUMA nodes */
    CHECK(!pe_geo_from(t, 3, 4, 3, 4, 0, NULL)); CHECK(t->gpu == -1 && t->numa == 3 && t->nic[0] == 3);
    /* override COMM_OFI_NICS */
    setenv("COMM_OFI_NICS", "3;2;1;0", 1); bus(1, b);
    CHECK(!pe_geo_from(t, 1, 4, 1, 4, 4, b)); CHECK(t->nics == 1 && t->nic[0] == 2);
    setenv("COMM_OFI_NICS", "0;1", 1); bus(3, b);
    CHECK(pe_geo_from(t, 3, 4, 3, 4, 4, b) && strstr(pe_geo_err(), "COMM_OFI_NICS"));
    clearenv_all();
    /* lanes override, bad values */
    setenv("XSHMEM_LANES", "4", 1); bus(0, b); CHECK(!pe_geo_from(t, 0, 4, 0, 4, 4, b)); CHECK(t->nlanes == 4 && t->nics == 1);
    setenv("XSHMEM_LANES", "0", 1); CHECK(pe_geo_from(t, 0, 4, 0, 4, 4, b)); clearenv_all();
    CHECK(pe_geo_from(t, 4, 4, 0, 4, 4, b)); CHECK(pe_geo_from(t, 0, 6, 0, 4, 4, b)); CHECK(pe_geo_from(t, 0, 4, 4, 4, 4, b));
    rmroot();

    /* target shape: 4 NUMA, 8 cxi (cxi i on NUMA i%4: NUMA n has n and n+4), 4 GPUs */
    newroot("target"); tree(4, 8, 4); clearenv_all();
    for (int l = 0; l < 4; l++) { bus(l, b); CHECK(!pe_geo_from(t, l, 4, l, 4, 4, b)); CHECK(t->nics == 2 && t->nic[0] == l && t->nic[1] == l + 4 && t->numa == l && t->nlanes == 1); }
    bus(1, b); CHECK(!pe_geo_from(t, 5, 8, 1, 4, 4, b)); CHECK(t->node == 1 && t->nics == 2);
    setenv("XSHMEM_LANES", "2", 1); CHECK(!pe_geo_from(t, 5, 8, 1, 4, 4, b)); CHECK(t->nlanes == 2 && t->nics == 2); clearenv_all();
    /* 1 PE per node on the target: 2 NICs, GPU 0 */
    bus(0, b); CHECK(!pe_geo_from(t, 0, 2, 0, 1, 4, b)); CHECK(t->nics == 2 && t->gpu == 0);
    /* two PEs per GPU: 8 PEs per node over 4 GPUs, each PE takes one of the GPU's 2 NICs */
    setenv("XSHMEM_PES_PER_GPU", "2", 1);
    for (int l = 0; l < 8; l++) { bus(l / 2, b); CHECK(!pe_geo_from(t, l, 8, l, 8, 4, b));
        CHECK(t->gpu_node == l / 2 && t->gpu == l / 2 && t->nics == 1 && t->nic[0] == l / 2 + 4 * (l % 2) && t->numa == l / 2); }
    CHECK(pe_geo_from(t, 0, 6, 0, 3, 4, b));   /* 3 PEs per node not a multiple of 2 */
    clearenv_all();
    /* multi-NIC override with commas */
    setenv("COMM_OFI_NICS", "0,4;1,5;2,6;3,7", 1); bus(2, b);
    CHECK(!pe_geo_from(t, 2, 4, 2, 4, 4, b)); CHECK(t->nics == 2 && t->nic[0] == 2 && t->nic[1] == 6);
    clearenv_all(); rmroot();

    /* NIC-less box: no cxi, one NUMA node */
    newroot("nonic"); tree(1, 0, 0); clearenv_all();
    CHECK(!pe_geo_from(t, 0, 1, 0, 1, 0, NULL)); CHECK(t->nics == 0 && t->gpu == -1 && t->numa == 0 && t->nlanes == 1 && !strcmp(t->cpus, "0-47"));
    CHECK(!pe_geo_from(t, 3, 4, 3, 4, 0, NULL)); CHECK(t->nics == 0 && t->numa == 0);
    rmroot();
    /* no sysfs at all */
    newroot("empty"); CHECK(!pe_geo_from(t, 0, 1, 0, 1, 0, NULL)); CHECK(t->nics == 0 && t->cpus[0] == 0); rmroot();

    char s[300]; newroot("str"); tree(4, 8, 4); bus(1, b); pe_geo_from(t, 5, 8, 1, 4, 4, b); pe_geo_str(t, s, sizeof s); CHECK(strstr(s, "nics 2 [1,5]")); rmroot();
    printf("t_pe_geo: %d checks, %d failed\n", nchk, nbad);
    return nbad != 0;
}
