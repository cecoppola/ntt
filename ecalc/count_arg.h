/* count_arg.h - the digit count on the command line (ecalc.c main; tests/t_args.c).
 * Accepted: a plain integer (decimal digits), or an exact decimal in scientific notation: mantissa digits with an optional
 * fraction part, then e or E, an optional +, and the exponent digits: 1e9, 1.6e10, 5.4e13, 1.50e1 (= 15).  The value must be a
 * positive whole number <= ULONG_MAX - 17 (room for the rounding to a multiple of 18 in ecalc.c).  Anything else -- trailing
 * text, a fraction, a sign, zero, more than 64 mantissa characters, overflow -- returns -1.  Exact integer arithmetic only. */
#ifndef COUNT_ARG_H
#define COUNT_ARG_H
#include <ctype.h>
#include <limits.h>

static int ec_parse_count(const char *s, unsigned long *out)
{
    const unsigned long lim = ULONG_MAX - 17;
    char m[64]; int ml = 0, nfrac = 0, dot = 0; long long ex = 0;
    const char *p = s;
    for (; *p; p++) {                                   /* mantissa: the digits, with at most one point */
        if (isdigit((unsigned char)*p)) { if (ml == (int)sizeof m) return -1; m[ml++] = *p; if (dot) nfrac++; }
        else if (*p == '.' && !dot) dot = 1;
        else break;
    }
    if (ml == 0) return -1;
    if (*p == 'e' || *p == 'E') {                       /* exponent: optional +, then digits (at least one) */
        p++; if (*p == '+') p++;
        if (!isdigit((unsigned char)*p)) return -1;
        for (; isdigit((unsigned char)*p); p++) if (ex < 1000000) ex = ex * 10 + (*p - '0');
    } else if (dot) return -1;                          /* a fraction without an exponent */
    if (*p) return -1;                                  /* trailing text */
    long long e = ex - nfrac;                           /* value = m * 10^e, m the mantissa digits as an integer */
    int i = 0; while (i < ml && m[i] == '0') i++;
    if (i == ml) return -1;                             /* zero */
    int L = ml - i;                                     /* significant digits, trailing zeros included */
    if (e < 0) {                                        /* exact only if the last -e digits are zeros */
        if (-e > L) return -1;
        for (long long k = 0; k < -e; k++) if (m[ml - 1 - k] != '0') return -1;
        L -= (int)-e; e = 0;
    }
    if (L + e > 20) return -1;                          /* 21 or more digits: above 2^64 */
    unsigned long v = 0;
    for (int k = 0; k < L; k++) { unsigned d = (unsigned)(m[i + k] - '0'); if (v > (lim - d) / 10) return -1; v = v * 10 + d; }
    for (long long k = 0; k < e; k++) { if (v > lim / 10) return -1; v *= 10; }
    *out = v;
    return 0;
}
#endif
