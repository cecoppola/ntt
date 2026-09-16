/* t_dec - step 7 test: todec against mpz_get_str and the reference digits.
 *
 *  1. random X < 10^ndig for ndig = 10^3 .. 10^6 (all generators of the limb
 *     array, reduced mod 10^ndig) vs mpz_get_str, with dec_leaf_u_max
 *     lowered so TOP, MID, DEEP and LEAF all run; leading zeros kept
 *  2. e to 10^6, 10^7, 10^8 (and 10^9 with "long") from ref/e_<d>.txt: X =
 *     mpz_set_str of the digits, todec(X) must reproduce the file
 *
 * Usage: t_dec [long]
 */
#include "harness.h"
#include "../todec.h"
#include "../rns_mul.h"
#include "../mem.h"

static int check_dec(const char *what, const bigint *X, unsigned long ndig)
{
    mpz_t x; mpz_init(x); bi_to_mpz(x, X);
    char *want = mpz_get_str(NULL, 10, x);
    size_t wl = strlen(want);
    char *got = (char *)malloc(ndig + 1);
    double t0 = now();
    todec(got, X, ndig);
    double dt = now() - t0;
    got[ndig] = 0;
    int ok = wl <= ndig;
    if (ok) { for (unsigned long i = 0; i < ndig - wl; i++) if (got[i] != '0') ok = 0; ok = ok && !memcmp(got + (ndig - wl), want, wl); }
    VERIFY(ok, "%s (%lu digits): mismatch", what, ndig);
    printf("   %-28s %lu digits: %.3f s (levels %d: top %d mid %d deep %d, %zu leaf pieces) %s\n", what, ndig, dt, dec_st.levels,
           dec_st.top_levels, dec_st.mid_levels, dec_st.deep_levels, dec_st.pieces, ok ? "ok" : "MISMATCH");
    free(want); free(got); mpz_clear(x);
    return ok;
}

int main(int argc, char **argv)
{
    int lng = argc > 1 && !strcmp(argv[1], "long");
    rng_t rng = {0xDEC0ULL};
    bigint X; bi_init(&X);
    mpz_t x, t; mpz_inits(x, t, NULL);
    printf("== t_dec ==\n");
    harness_meta("t_dec");
    rns_init(31);
    printf("-- 1. random values vs mpz_get_str\n");
    static const unsigned long nds[] = {1000, 5000, 100000, 1000000};
    for (int i = 0; i < 4; i++) for (int kind = 0; kind < GEN_KINDS; kind++) {
        unsigned long nd = nds[i];
        size_t nl = (size_t)(nd * 3.33 / 64) + 2;
        bi_random(&X, nl, kind, &rng);
        bi_to_mpz(x, &X); mpz_ui_pow_ui(t, 10, nd); mpz_mod(x, x, t); bi_from_mpz(&X, x);
        dec_leaf_u_max = 128 + 16 * (i % 3);      /* leaf pieces of 1 152 .. 2 590 digits */
        rns_school_max = 1024;
        char what[64]; snprintf(what, sizeof what, "%s", gen_name[kind]);
        check_dec(what, &X, nd);
    }
    dec_leaf_u_max = 256;
    printf("-- 2. e from ref/\n");
    static const unsigned long ds[] = {1000000, 10000000, 100000000, 1000000000};
    for (int i = 0; i < (lng ? 4 : 3); i++) {
        unsigned long d = ds[i];
        char path[256]; snprintf(path, sizeof path, "ref/e_%lu.txt", d);
        FILE *f = fopen(path, "r"); if (!f) { printf("   %s missing\n", path); continue; }
        char *s = (char *)malloc(d + 4);
        size_t got = fread(s, 1, d + 2, f); fclose(f); s[d + 2] = 0;
        VERIFY(got == d + 2 && s[1] == '.', "%s: short read %zu", path, got);
        s[1] = s[0]; /* "22718..." -> digits start at s+1: "2718..." */
        double t0 = now();
        mpz_set_str(t, s + 1, 10);                  /* s+1 = "2" + fractional digits */
        double ts = now() - t0;
        bi_from_mpz(&X, t);
        char *out = (char *)mem_hreg_alloc(d + 2);
        dec_verbose = d >= 100000000;
        t0 = now();
        todec(out, &X, d + 1);
        double dt = now() - t0;
        dec_verbose = 0;
        int ok = !memcmp(out, s + 1, d + 1);
        VERIFY(ok, "e %lu: digits differ from %s", d, path);
        printf("   e %lu digits: todec %.2f s (mpz_set_str %.2f s) %s; levels %d, leaf pieces %zu\n", d, dt, ts, ok ? "== ref" : "MISMATCH", dec_st.levels, dec_st.pieces);
        char nm[32]; snprintf(nm, sizeof nm, "todec_%lu", d); harness_result(nm, "s", dt);
        mem_hreg_free(out); free(s);
    }
    printf("VmHWM %.1f GB\n", mem_vmhwm() / 1e9);
    rns_shutdown();
    return verify_done("t_dec");
}
