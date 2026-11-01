/* nodemem.h - one NUMA node's MemFree from /sys/devices/system/node/node<k>/meminfo (C5, results/HEALTH.md).  On an MI300A node each APU's HBM is its own
 * NUMA node (node k = APU k), so this is the APU's own free memory; /proc/meminfo's MemAvailable is the sum over the four.
 * root = a path prefix of "/sys" (test hook ECALC_MEMSAMPLE_SYSFS_ROOT: a mock tree; "" = the real /sys).  Header-only, host only (tests/t_nodemem.c). */
#ifndef EC_NODEMEM_H
#define EC_NODEMEM_H
#include <stdio.h>
#include <string.h>
#include <stddef.h>
static inline int nodemem_free(const char *root, int node, size_t *bytes)   /* 1 ok (bytes = MemFree), 0 = no such node or no MemFree line */
{
    char p[256], line[256]; int ok = 0;
    snprintf(p, sizeof p, "%s/sys/devices/system/node/node%d/meminfo", root ? root : "", node);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        int k; char name[64]; unsigned long long v;
        if (sscanf(line, "Node %d %63s %llu", &k, name, &v) == 3 && !strcmp(name, "MemFree:")) { *bytes = (size_t)v << 10; ok = 1; break; }
    }
    fclose(f);
    return ok;
}
#endif
