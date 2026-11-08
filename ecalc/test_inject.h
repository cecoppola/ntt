/* test_inject.h - ECALC_TEST="key=value,key=value,...": every test-only injection in one setting (no unit tests need other names).
 * Keys: dr=<kind>[+leafslow] dr_k dr_pe dr_n dr_m (dr.h), p24=<1|2> p24_k p24_s (rns_dist.c), corr=<k> dkm_hi=<k> dkm_carry=1 (newton_db.c, ecalc.c),
 * die_pe=<pe>, fatal=1 (ecalc.c, binsplit.c), mapd_die=<dev>:<k>[:p] (mapd.c), tree_logn=<n> (mn.c, mn_plan.c).  Read at each call (tests change it). */
#ifndef EC_TEST_INJECT_H
#define EC_TEST_INJECT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* the value of `key` in ECALC_TEST copied to buf (n bytes), or NULL when ECALC_TEST has no such key */
static inline const char *ec_test_str(const char *key, char *buf, size_t n)
{
    const char *e = getenv("ECALC_TEST"); size_t kl = strlen(key);
    while (e && *e) {
        const char *end = strchr(e, ','); size_t len = end ? (size_t)(end - e) : strlen(e);
        if (len > kl && !strncmp(e, key, kl) && e[kl] == '=') {
            size_t vl = len - kl - 1; if (vl >= n) vl = n - 1;
            memcpy(buf, e + kl + 1, vl); buf[vl] = 0; return buf;
        }
        e = end ? end + 1 : NULL;
    }
    return NULL;
}
static inline long ec_test_long(const char *key, long dflt) { char b[96]; const char *v = ec_test_str(key, b, sizeof b); return v && *v ? atol(v) : dflt; }
/* ---- validation: unknown key, repeated key, empty or malformed value, a space -> the problem text (rc 2 at start; unit test t_host dr) ---- */
typedef struct { const char *key; char type; } ec_test_key;   /* n integer, f number, s string, k dr kind[+leafslow], l integer list joined by + */
static inline const ec_test_key *ec_test_keys(void)
{
  static const ec_test_key K[] = {
    {"dr", 'k'}, {"dr_k", 'n'}, {"dr_pe", 'n'}, {"dr_n", 'n'}, {"dr_m", 'n'}, {"p24", 'n'}, {"p24_k", 'n'}, {"p24_s", 'n'},
    {"corr", 'n'}, {"dkm_hi", 'n'}, {"dkm_carry", 'n'}, {"die_pe", 'n'}, {"fatal", 'n'}, {"mapd_die", 's'}, {"tree_logn", 'n'}, {"dist_gen", 'n'},
    {"alloc_ntt", 'n'}, {"alloc_seed", 'n'}, {"alloc_probe", 'n'}, {"h4", 'n'}, {"h4_mb", 'n'}, {"h4_reps", 'n'}, {"tbench", 'n'}, {"big", 'n'},
    {"big_k", 'n'}, {"big_primes", 'l'}, {"xgmi", 'n'}, {"tinv", 'n'}, {"layered", 'n'}, {"lbench", 'l'}, {"lreps", 'n'},
    {"patch_dir", 's'}, {"patch_unpack", 's'}, {"strat_budget_gb", 'f'}, {"strat_gmp", 'n'}, {NULL, 0} };
  return K;
}
static inline int ec_test_num_ok(const char *v, int flt) { char *e; if (!*v) return 0; if (flt) strtod(v, &e); else strtol(v, &e, 10); return !*e; }
/* 1 and a message in msg if spec is malformed, else 0 (spec NULL or empty = fine) */
static inline int ec_test_problem(const char *spec, char *msg, size_t n)
{
    const char *seen[48]; int ns = 0;
    if (!spec || !*spec) return 0;
    if (strpbrk(spec, " \t\n")) { snprintf(msg, n, "ECALC_TEST=%s: no spaces allowed", spec); return 1; }
    const char *e = spec;
    while (*e) {
        const char *end = strchr(e, ','); size_t len = end ? (size_t)(end - e) : strlen(e);
        const char *eq = (const char *)memchr(e, '=', len); char key[40], val[96];
        if (!eq || eq == e || len - (size_t)(eq - e) - 1 == 0 || (size_t)(eq - e) >= sizeof key || len - (size_t)(eq - e) - 1 >= sizeof val) { snprintf(msg, n, "ECALC_TEST=%s: expected key=value pairs separated by commas", spec); return 1; }
        memcpy(key, e, (size_t)(eq - e)); key[eq - e] = 0; size_t vl = len - (size_t)(eq - e) - 1; memcpy(val, eq + 1, vl); val[vl] = 0;
        const ec_test_key *K = ec_test_keys(); while (K->key && strcmp(K->key, key)) K++;
        if (!K->key) { snprintf(msg, n, "ECALC_TEST: unknown key '%s'", key); return 1; }
        for (int i = 0; i < ns; i++) if (!strcmp(seen[i], K->key)) { snprintf(msg, n, "ECALC_TEST: key '%s' given twice", key); return 1; }
        if (ns < 48) seen[ns++] = K->key;
        int ok = 1;
        if (K->type == 'n') ok = ec_test_num_ok(val, 0);
        else if (K->type == 'f') ok = ec_test_num_ok(val, 1);
        else if (K->type == 'l') { char b[96]; snprintf(b, sizeof b, "%s", val); char *sp = 0; for (char *t = strtok_r(b, "+", &sp); t; t = strtok_r(NULL, "+", &sp)) ok &= ec_test_num_ok(t, 0); }
        else if (K->type == 'k') { static const char *const kn[] = { "flag", "deliver", "store", "xql", "cyc", "leaf", "leafslow", "0" }; char b[96]; snprintf(b, sizeof b, "%s", val); char *sp = 0; int kinds = 0;
            for (char *t = strtok_r(b, "+", &sp); t; t = strtok_r(NULL, "+", &sp)) { int f = 0; for (unsigned i = 0; i < sizeof kn / sizeof *kn; i++) if (!strcmp(t, kn[i])) f = 1; ok &= f; kinds += strcmp(t, "leafslow") != 0; }
            if (kinds > 1) ok = 0; }
        if (!ok) { snprintf(msg, n, "ECALC_TEST: bad value '%s' for key '%s'", val, key); return 1; }
        e = end ? end + 1 : e + len;
    }
    return 0;
}
/* the integers of a +-joined list value (big_primes=0+2): count, at most max stored */
static inline int ec_test_list(const char *key, long *out, int max)
{
    char b[96]; const char *v = ec_test_str(key, b, sizeof b); int c = 0; if (!v) return 0;
    char *sp = 0; for (char *t = strtok_r(b, "+", &sp); t; t = strtok_r(NULL, "+", &sp)) { if (c < max) out[c] = atol(t); c++; }
    return c;
}
/* start-up check: a malformed ECALC_TEST ends the process with rc 2 (ecalc main, the test binaries' harness_meta) */
static inline void ec_test_validate(void)
{
    char m[220]; if (ec_test_problem(getenv("ECALC_TEST"), m, sizeof m)) { fprintf(stderr, "%s\n", m); exit(2); }
}
#endif
