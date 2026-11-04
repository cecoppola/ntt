/* test_inject.h - ECALC_TEST="key=value,key=value,...": every test-only injection in one setting (no unit tests need other names).
 * Keys: dr=<kind>[+leafslow] dr_k dr_pe dr_n dr_m (dr.h), p24=<1|2> p24_k p24_s (rns_dist.c), corr=<k> dkm_hi=<k> dkm_carry=1 (newton_db.c, ecalc.c),
 * die_pe=<pe>, fatal=1 (ecalc.c, binsplit.c), mapd_die=<dev>:<k>[:p] (mapd.c), tree_logn=<n> (mn.c, mn_plan.c).  Read at each call (tests change it). */
#ifndef EC_TEST_INJECT_H
#define EC_TEST_INJECT_H
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
#endif
