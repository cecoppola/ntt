/* dr.c - C3 "detect and retry": switches, fault-injection parser, counters, context and the residue helpers (dr.h; results/DR.md).  Host only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dr.h"

typedef unsigned __int128 u128;

/* ---- switches ---- */
static struct { int init, level, total, kind, k, pe, n; } g = { 0, 0, 8, -1, 1, 0, 1 };
static const char *const KN[DR_NKINDS] = { "flag", "deliver", "store", "xql", "cyc", "leaf" };
const char *dr_kind_name(int kind) { return kind >= 0 && kind < DR_NKINDS ? KN[kind] : "none"; }
static long atol_d(const char *e, long d) { return e && *e ? atol(e) : d; }
void dr_init_from_env(void)
{
    long lv = atol_d(getenv("ECALC_DR"), 0);
    if (lv < 0 || lv > 3) { fprintf(stderr, "ECALC_DR=%ld: must be 0, 1, 2 or 3\n", lv); exit(2); }
    g.level = (int)lv;
    long tot = atol_d(getenv("ECALC_DR_TOTAL"), 8); g.total = tot < 1 ? 1 : (int)tot;
    g.kind = -1; g.k = 1; g.pe = 0; g.n = 1;
    const char *t = getenv("ECALC_DR_TEST");
    if (t && *t && strcmp(t, "0")) {
        for (int i = 0; i < DR_NKINDS; i++) if (!strcmp(t, KN[i])) g.kind = i;
        if (g.kind < 0) { fprintf(stderr, "ECALC_DR_TEST=%s: unknown kind (flag, deliver, store, xql, cyc, leaf)\n", t); exit(2); }
        long k = atol_d(getenv("ECALC_DR_TEST_K"), 1); g.k = k < 1 ? 1 : (int)k;
        long pe = atol_d(getenv("ECALC_DR_TEST_PE"), 0); g.pe = pe < 0 ? -1 : (int)pe;
        long n = atol_d(getenv("ECALC_DR_TEST_N"), 1); g.n = n < 1 ? 1 : (int)n;
    }
    g.init = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
static inline void ini(void) { if (!__atomic_load_n(&g.init, __ATOMIC_ACQUIRE)) dr_init_from_env(); }
int dr_level(void) { ini(); return g.level; }
int dr_total_cap(void) { ini(); return g.total; }
int dr_test_kind(void) { ini(); return g.kind; }
int dr_test_k(void) { ini(); return g.k; }
int dr_test_pe(void) { ini(); return g.pe; }
int dr_test_n(void) { ini(); return g.n; }

static size_t fired[DR_SLOTS], cnt[DR_C_N];
int dr_inject(int kind, size_t occ, int pe, int slot, int attempt)
{
    ini();
    if (g.kind < 0 || kind != g.kind || occ != (size_t)g.k || slot < 0 || slot >= DR_SLOTS) return 0;
    if (g.pe >= 0 && pe != g.pe) return 0;
    if (attempt < 0 || (size_t)attempt != fired[slot] || fired[slot] >= (size_t)g.n) return 0;
    fired[slot]++; dr_count(DR_C_INJECTED, 1); return 1;
}

/* ---- counters ---- */
void dr_count(int c, size_t v) { if (c >= 0 && c < DR_C_N) __atomic_fetch_add(&cnt[c], v, __ATOMIC_RELAXED); }
size_t dr_counter(int c) { return c >= 0 && c < DR_C_N ? __atomic_load_n(&cnt[c], __ATOMIC_RELAXED) : 0; }
void dr_summary(void)
{
    if (!dr_on()) return;
    fprintf(stdout, "DR: %zu checks, %zu injected, %zu failures, %zu recovered (pieces %zu, products %zu, division %zu), %zu fatal\n",
            dr_counter(DR_C_CHECK), dr_counter(DR_C_INJECTED), dr_counter(DR_C_FAIL),
            dr_counter(DR_C_REC_PIECE) + dr_counter(DR_C_REC_PRODUCT) + dr_counter(DR_C_REC_DIV),
            dr_counter(DR_C_REC_PIECE), dr_counter(DR_C_REC_PRODUCT), dr_counter(DR_C_REC_DIV), dr_counter(DR_C_FATAL));
    fflush(stdout);
}
void dr_arm(void) { static int on; if (!on) { on = 1; atexit(dr_summary); } }

/* ---- context ---- */
static struct { char phase[48]; int level, g0, g, sched, combine, step; } cx = { "", -1, -1, -1, -1, -1, -1 };
void dr_ctx_set(const char *phase, int level, int g0, int g_, int sched, int combine, int step)
{
    snprintf(cx.phase, sizeof cx.phase, "%s", phase ? phase : "");
    cx.level = level; cx.g0 = g0; cx.g = g_; cx.sched = sched; cx.combine = combine; cx.step = step;
}
void dr_ctx_clear(void) { dr_ctx_set("", -1, -1, -1, -1, -1, -1); }
int dr_where(char *buf, size_t n)
{
    if (!n) return 0;
    buf[0] = 0;
    if (!cx.phase[0]) return 0;
    int k = snprintf(buf, n, "%s", cx.phase);
    if (cx.level >= 0 && k < (int)n) k += snprintf(buf + k, n - k, " level %d", cx.level);
    if (cx.g0 >= 0 && cx.g > 0 && k < (int)n) k += snprintf(buf + k, n - k, ", group [%d, %d)", cx.g0, cx.g0 + cx.g);
    if (cx.sched >= 0 && k < (int)n) k += snprintf(buf + k, n - k, ", sched %d", cx.sched);
    if (cx.combine >= 0 && k < (int)n) k += snprintf(buf + k, n - k, ", combine %d", cx.combine);
    if (cx.step >= 0 && k < (int)n) k += snprintf(buf + k, n - k, ", step %d", cx.step);
    return (int)strlen(buf);
}

/* ---- residues modulo DR_Q ---- */
uint64_t dr_addmod(uint64_t a, uint64_t b) { uint64_t s = a + b; return s >= DR_Q ? s - DR_Q : s; }      /* a, b < DR_Q < 2^63: no overflow */
uint64_t dr_submod(uint64_t a, uint64_t b) { return a >= b ? a - b : a + DR_Q - b; }
uint64_t dr_mulmod(uint64_t a, uint64_t b) { return (uint64_t)((u128)a * b % DR_Q); }
uint64_t dr_powmod(uint64_t b, uint64_t e) { uint64_t r = 1, x = b % DR_Q; for (; e; e >>= 1) { if (e & 1) r = dr_mulmod(r, x); x = dr_mulmod(x, x); } return r; }
uint64_t dr_powB(uint64_t e) { return dr_powmod(DR_B % DR_Q, e); }
uint64_t dr_limbs_res(const uint64_t *l, size_t n)
{
    uint64_t r = 0, b = DR_B % DR_Q;
    for (size_t i = n; i-- > 0;) r = (uint64_t)(((u128)r * b + l[i] % DR_Q) % DR_Q);
    return r;
}
uint64_t dr_res_shift(uint64_t r, uint64_t e) { return dr_mulmod(r, dr_powB(e)); }
void dr_res_part(uint64_t *total, uint64_t part, uint64_t off) { *total = dr_addmod(*total, dr_res_shift(part, off)); }
int dr_match_m(uint64_t e_res, uint64_t rBw)
{
    e_res %= DR_Q; rBw %= DR_Q;
    if (e_res == 0) return 0;
    if (e_res == rBw) return 1;
    if (e_res == dr_submod(0, rBw)) return -1;
    return 2;
}

/* ---- residues modulo B+1, B-1 ---- */
uint64_t dr_mulmod_m(uint64_t a, uint64_t b, uint64_t m) { return (uint64_t)((u128)a * b % m); }
uint64_t dr_mod_bp1(const uint64_t *l, size_t n)
{
    const uint64_t m = DR_B + 1; uint64_t even = 0, odd = 0;
    for (size_t i = 0; i < n; i++) { uint64_t *t = i & 1 ? &odd : &even; *t += l[i] % m; if (*t >= m) *t -= m; }
    return even >= odd ? even - odd : even + m - odd;
}
uint64_t dr_mod_bm1(const uint64_t *l, size_t n)
{
    const uint64_t m = DR_B - 1; uint64_t s = 0;
    for (size_t i = 0; i < n; i++) { s += l[i] % m; if (s >= m) s -= m; }
    return s;
}
int dr_cyc_ok(uint64_t up, uint64_t um, uint64_t ap, uint64_t am, uint64_t bp, uint64_t bm)
{
    return up == dr_mulmod_m(ap, bp, DR_B + 1) && um == dr_mulmod_m(am, bm, DR_B - 1);
}
