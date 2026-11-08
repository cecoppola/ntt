/* nodemem.h - one NUMA node's MemFree (and file cache) from /sys/devices/system/node/node<k>/meminfo. On an MI300A
 * node each APU's HBM is its own NUMA node (k = APU k), so this is the APU's own free memory; /proc/meminfo's
 * MemAvailable is the sum over the four. nodemem_read gives MemFree and the reclaimable file cache (Active(file) +
 * Inactive(file)); the usable free memory is their sum. root = a path prefix of "/sys" (test hook
 * ECALC_MEMSAMPLE_SYSFS_ROOT: a mock tree; "" = the real /sys). Header-only, host only (tests/t_host.c nodemem). */
#ifndef EC_NODEMEM_H
#define EC_NODEMEM_H
#include <stdio.h>
#include <string.h>
#include <stddef.h>
static inline int nodemem_read(const char *root, int node, size_t *memfree, size_t *file)   /* 1 ok (MemFree found; file = Active(file) + Inactive(file), 0 when the lines are absent), 0 = no such node or no MemFree line */
{
    char p[256], line[256]; int ok = 0; size_t fr = 0, fl = 0;
    snprintf(p, sizeof p, "%s/sys/devices/system/node/node%d/meminfo", root ? root : "", node);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        int k; char name[64]; unsigned long long v;
        if (sscanf(line, "Node %d %63s %llu", &k, name, &v) != 3) continue;
        if (!strcmp(name, "MemFree:")) { fr = (size_t)v << 10; ok = 1; }
        else if (!strcmp(name, "Active(file):") || !strcmp(name, "Inactive(file):")) fl += (size_t)v << 10;
    }
    fclose(f);
    if (ok) { if (memfree) *memfree = fr; if (file) *file = fl; }
    return ok;
}
static inline int nodemem_free(const char *root, int node, size_t *bytes)   /* 1 ok (bytes = MemFree only), 0 = no such node or no MemFree line */
{
    return nodemem_read(root, node, bytes, 0);
}
#endif
