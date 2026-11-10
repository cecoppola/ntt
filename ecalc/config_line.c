/* config_line.c -- see config_line.h. Reads the environment only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_line.h"
#include "switches_gen.h"

extern char **environ;
static char *g_set; static unsigned g_hash; static int g_n, g_have;

int cfg_guard(void)
{
    int bad = 0;
    for (char **e = environ; e && *e; e++) {
        size_t n = strcspn(*e, "=");
        int pre = 0, known = 0;
        for (int i = 0; i < CFG_PREFIXES_N; i++) if (!strncmp(*e, cfg_prefixes[i], strlen(cfg_prefixes[i]))) pre = 1;
        if (!pre) continue;
        /* launcher-only names: a prefix, or an exact name when the entry ends in '=' (*e is "NAME=value") */
        for (int i = 0; i < CFG_EXEMPT_N && !known; i++) known = !strncmp(cfg_exempt[i], *e, strlen(cfg_exempt[i]));
        if (known) continue;
        for (int i = 0; i < CFG_REG_N && !known; i++) known = cfg_reg[i].kind == 'x' ? !strncmp(cfg_reg[i].name, *e, strlen(cfg_reg[i].name)) : strlen(cfg_reg[i].name) == n && !strncmp(cfg_reg[i].name, *e, n);
        if (!known) { fprintf(stderr, "ecalc: unknown setting %.*s in the environment (not in ecalc/switches.tsv; a typo? `ecalc --settings` lists the names)\n", (int)n, *e); bad = 1; }
    }
    return bad;
}

void cfg_settings(void)
{
    printf("%-34s %-14s %s\n", "setting", "default", "in force");
    for (int i = 0; i < CFG_REG_N; i++) {
        if (cfg_reg[i].kind != 'h' && cfg_reg[i].kind != 'p') continue;
        const char *v = getenv(cfg_reg[i].name);
        printf("%-34s %-14s %s\n", cfg_reg[i].name, cfg_reg[i].def, v ? (*v ? v : "(empty)") : "-");
    }
}

void cfg_snapshot(void)
{
    #define IS_H(i) (cfg_reg[i].kind == 'h')
    size_t cap = 1;
    for (int i = 0; i < CFG_REG_N; i++) { const char *v = IS_H(i) ? getenv(cfg_reg[i].name) : 0; if (v) cap += strlen(cfg_reg[i].name) + strlen(v) + 2; }
    g_set = (char *)malloc(cap); if (!g_set) return;
    char *p = g_set; unsigned h = 2166136261u; g_n = 0;
    for (int i = 0; i < CFG_REG_N; i++) {
        const char *v = IS_H(i) ? getenv(cfg_reg[i].name) : 0; if (!v) continue;
        const char *nm = cfg_reg[i].name;
        int w = sprintf(p, "%s%s=%s", g_n ? " " : "", nm, v); p += w;
        for (const char *s = nm; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
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
