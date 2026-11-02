/* t_args - the digit count argument (count_arg.h): plain integers and exact scientific notation accepted, everything else
 * rejected, exact at the edges of unsigned long.  Host only, no GPU, no node time.  Build and run: make tests/t_args && tests/t_args */
#include <stdio.h>
#include <string.h>
#include "../count_arg.h"

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAILED line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void ok(const char *s, unsigned long want)
{
    unsigned long v = 0; int rc = ec_parse_count(s, &v);
    CHECK(rc == 0 && v == want, "\"%s\" should give %lu (rc %d, got %lu)", s, want, rc, v);
}
static void bad(const char *s)
{
    unsigned long v = 7; int rc = ec_parse_count(s, &v);
    CHECK(rc != 0 && v == 7, "\"%s\" should be rejected (rc %d, value %lu)", s, rc, v);
}

int main(void)
{
    /* plain integers, as before */
    ok("1", 1UL); ok("18", 18UL); ok("1000000000", 1000000000UL); ok("0000042", 42UL);
    /* exact scientific notation */
    ok("1e9", 1000000000UL); ok("1E9", 1000000000UL); ok("1e+9", 1000000000UL); ok("1.6e10", 16000000000UL);
    ok("5.4e13", 54000000000000UL); ok("1.50e1", 15UL); ok("15.0e0", 15UL); ok("0.5e1", 5UL); ok("1.5e3", 1500UL);
    ok("1e0", 1UL);
    ok("1e19", 10000000000000000000UL); ok("1.8446744073709551598e19", 18446744073709551598UL);
    ok("18446744073709551598", 18446744073709551598UL);          /* ULONG_MAX - 17: the largest accepted */
    /* rejected: fractions, signs, zero, trailing text, malformed exponents, overflow */
    bad(""); bad("e5"); bad("e"); bad("1e"); bad("1e+"); bad("1e-3"); bad("1e-"); bad("1x"); bad("1.6e1x");
    bad("1.6e10 "); bad(" 5"); bad("5 "); bad("2.5"); bad("3.0"); bad("2.5e0"); bad("1.5e0"); bad("1..5"); bad("1e5.0");
    bad("1.6e1e1"); bad("-1"); bad("+5"); bad("-1e9"); bad("0"); bad("00"); bad("0e5"); bad("0.0e3"); bad("0x10");
    bad("18446744073709551599"); bad("18446744073709551615"); bad("100000000000000000000"); bad("1e20"); bad("1e21");
    bad(".5e1"); bad("5.e1"); bad(".e1"); bad("1e99999999999999999999"); bad("1e999999999"); bad("2e19"); bad("1.8446744073709551599e19");
    bad("999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999");
    printf("\nt_args: %s (%d of %d checks failed)\n", fails ? "FAILED" : "OK", fails, checks);
    return fails != 0;
}
