/* t_nodemem.c -- nodemem.h on a mock sysfs tree (host only, no GPU): the per-NUMA-node MemFree reader of the memory sampler (C5).
 * Build: make tests/t_nodemem      Run: ./tests/t_nodemem   (prints "t_nodemem: N checks, 0 failed") */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../nodemem.h"
static int nchk, nbad;
#define CHECK(c) do { nchk++; if (!(c)) { nbad++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void node(const char *root, int k, const char *body)
{
    char d[512], p[560]; snprintf(d, sizeof d, "%s/sys", root); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/sys/devices", root); mkdir(d, 0755); snprintf(d, sizeof d, "%s/sys/devices/system", root); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/sys/devices/system/node", root); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/sys/devices/system/node/node%d", root, k); mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/meminfo", d); FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); } fputs(body, f); fclose(f);
}
int main(void)
{
    char root[] = "/tmp/t_nodemem_XXXXXX"; if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    node(root, 0, "Node 0 MemTotal:       130880096 kB\nNode 0 MemFree:         43073848 kB\nNode 0 MemUsed:         87806248 kB\n");
    node(root, 1, "Node 1 MemTotal:       131809008 kB\nNode 1 MemFree:        108302508 kB\n");
    node(root, 4, "Node 4 MemTotal:       130880096 kB\nNode 4 MemFree:         10000000 kB\nNode 4 Active(anon):     5000000 kB\nNode 4 Inactive(anon):   1000000 kB\nNode 4 Active(file):    20000000 kB\nNode 4 Inactive(file):  30000000 kB\nNode 4 Mapped:           9000000 kB\n");   /* file cache present */
    node(root, 2, "Node 2 MemTotal:       131762524 kB\n");   /* no MemFree line */
    size_t b = 0;
    CHECK(nodemem_free(root, 0, &b) && b == (size_t)43073848 * 1024);
    CHECK(nodemem_free(root, 1, &b) && b == (size_t)108302508 * 1024);
    size_t mf = 1, fl = 1;   /* MEMFIT: MemFree + Active(file) + Inactive(file) */
    CHECK(nodemem_read(root, 4, &mf, &fl) && mf == (size_t)10000000 * 1024 && fl == (size_t)50000000 * 1024);
    CHECK(nodemem_free(root, 4, &b) && b == (size_t)10000000 * 1024);                      /* the MemFree-only reader ignores the file lines */
    CHECK(nodemem_read(root, 0, &mf, &fl) && mf == (size_t)43073848 * 1024 && fl == 0);   /* no file lines = 0 */
    CHECK(nodemem_read(root, 1, 0, 0) && !nodemem_read(root, 2, &mf, &fl) && !nodemem_read(root, 9, &mf, &fl));
    CHECK(!nodemem_free(root, 2, &b)); CHECK(!nodemem_free(root, 3, &b));
    CHECK(!nodemem_free("/nonexistent_t_nodemem", 0, &b));
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); if (system(cmd)) nbad++;
    printf("t_nodemem: %d checks, %d failed\n", nchk, nbad);
    return nbad ? 1 : 0;
}
