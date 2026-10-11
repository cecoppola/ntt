/* test_pack60 - M5 (c05): host-only test of the 60-bit packer in ecalc/packed_fmt.h: round trips for every length 0..70, the
 * chunked stream of the writer (ecp60_stream_chunk, random chunk sizes incl. 1, odd, a part ending on an odd limb) against the
 * one-shot pack, the byte count, and limbs at the extremes (0, 10^18 - 1).  Exit 0 = pass.  Build: make -C tools test_pack60 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ecalc/packed_fmt.h"
static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint64_t limb(void) { uint64_t k = rnd() % 8; return k == 0 ? 0 : k == 1 ? 999999999999999999ULL : rnd() % 1000000000000000000ULL; }
int main(void)
{
    int fails = 0;
    for (size_t n = 0; n <= 70; n++) {
        uint64_t *l = malloc((n + 1) * 8), *u = calloc(n + 1, 8); uint8_t *b = calloc(ecp60_bytes(n) + 16, 1);
        for (size_t i = 0; i < n; i++) l[i] = limb();
        ecp60_pack(l, n, b); ecp60_unpack(b, n, u);
        if (memcmp(l, u, n * 8) || ecp60_bytes(n) != (n * 60 + 7) / 8) { printf("round trip n=%zu FAIL\n", n); fails++; }
        for (size_t i = 0; i < 16; i++) if (b[ecp60_bytes(n) + i]) { printf("overrun n=%zu FAIL\n", n); fails++; break; }
        free(l); free(u); free(b);
    }
    for (int it = 0; it < 3000; it++) {
        size_t n = 1 + rnd() % 3000; uint64_t *l = malloc(n * 8), *u = malloc(n * 8); uint8_t *one = malloc(ecp60_bytes(n) + 16), *st = malloc(ecp60_bytes(n) + 64), *ob = malloc(ecp60_bytes(n) + 64);
        for (size_t i = 0; i < n; i++) l[i] = limb();
        ecp60_pack(l, n, one);
        size_t pos = 0, tot = 0; int hp = 0; uint64_t pend = 0;
        while (pos < n) {
            size_t c = 1 + rnd() % (rnd() % 4 == 0 ? 3 : 200); if (c > n - pos) c = n - pos;
            size_t nb = ecp60_stream_chunk(l + pos, c, pos + c == n, &hp, &pend, ob);
            memcpy(st + tot, ob, nb); tot += nb; pos += c;
        }
        if (tot != ecp60_bytes(n) || hp || memcmp(st, one, tot)) { printf("stream n=%zu FAIL (bytes %zu vs %llu)\n", n, tot, (unsigned long long)ecp60_bytes(n)); fails++; }
        /* a read starting at any even limb gives the limbs from there (the patch / recheck reads) */
        size_t ja = (rnd() % n) & ~(size_t)1; ecp60_unpack(one + 15 * (ja / 2), n - ja, u);
        if (memcmp(u, l + ja, (n - ja) * 8)) { printf("tail read n=%zu ja=%zu FAIL\n", n, ja); fails++; }
        free(l); free(u); free(one); free(st); free(ob);
    }
    printf("test_pack60: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
