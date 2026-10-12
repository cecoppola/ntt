/* config_line.c -- see config_line.h.  Reads the environment only; changes nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_line.h"
#include "switches_gen.h"

static char *g_set; static unsigned g_hash; static int g_n, g_have;

void cfg_snapshot(void)
{
    size_t cap = 1;
    for (int i = 0; i < CFG_NAMES_N; i++) { const char *v = getenv(cfg_names[i]); if (v) cap += strlen(cfg_names[i]) + strlen(v) + 2; }
    g_set = (char *)malloc(cap); if (!g_set) return;
    char *p = g_set; unsigned h = 2166136261u; g_n = 0;
    for (int i = 0; i < CFG_NAMES_N; i++) {
        const char *v = getenv(cfg_names[i]); if (!v) continue;
        int w = sprintf(p, "%s%s=%s", g_n ? " " : "", cfg_names[i], v); p += w;
        for (const char *s = cfg_names[i]; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
        h = (h ^ '=') * 16777619u;
        for (const char *s = v; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
        h = (h ^ '\n') * 16777619u;
        g_n++;
    }
    *p = 0; g_hash = h; g_have = 1;
}

void cfg_print(const char *eff)
{
    const char *r = getenv("COMM_RANK");
    if (!g_have || (r && atoi(r) != 0)) return;          /* one line per run (rank 0); other launchers: every rank prints */
    printf("config: hash=%08x set=%s eff=%s\n", g_hash, g_n ? g_set : "-", eff ? eff : "-");
}
