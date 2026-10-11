/* packed_fmt.h - Phase 15 IO (W2): the packed digit file (ECALC_OUT_PACKED=1), shared by the writer (mn_out.c) and the
 * converter / checker (tools/unpack_digits.c).
 *
 * A packed file is a 4096-byte header followed by the base-10^18 limbs of X exactly as the run holds them: 8 bytes per
 * limb, little-endian, THE MOST SIGNIFICANT LIMB FIRST (the order the writer streams them and the order the digits are
 * read), so 18 digits cost 8 bytes (0.444 B/digit against 1 B/digit for ASCII).  One file per node-process: the name is
 * the ASCII writer's (<outfile> at size 1, <outfile>.part<k> at size > 1, k = size-1-rank: part 0 holds the top limbs).
 *
 * The digit string of X (the ASCII writer's convention, mn_out.h): X < 10^(d+1) has nl = ceil((d+1)/18) limbs; written
 * from the top limb, 18 digits each, it begins with pad = 18 nl - (d+1) zero characters that are dropped; digit k
 * (0 = the "2", 1..d the fraction) is character pad + k.  The ASCII file is "2." + digits 1..d_out + "\n": a packed part
 * holding the limbs [lo, hi) holds the digits [k0, k1) (header), and the converter emits the ones <= d_out, the "." after
 * digit 0 and the newline after digit d_out.  The digits d_out+1..d (fewer than 18, in the lowest limb) are in the file
 * and not in the ASCII output.
 *
 * The header is written when the file is opened (complete = 0) and rewritten when the run closes it (complete = 1, the
 * residues): dres[i] = the part's digits [k0, k1) as a decimal number mod q[i] -- computed by the run from the bytes it
 * wrote, the same values its T1 "digits == X mod q" check used -- so a converter can check its own output against them.
 * Phase 15 KP: a division correction deferred past the writer (ECALC_CORR_PATCH) is patched in place after the close: the
 * limbs it changes are rewritten at their offsets and dres is moved by the same change (mn_out.c packed_patch), so the header
 * stays the residues of the limbs the file holds.
 */
#ifndef EC_PACKED_FMT_H
#define EC_PACKED_FMT_H
#include <stdint.h>

#define ECP_MAGIC      "ECPACK18"
#define ECP_VERSION    1u
#define ECP_VERSION60  2u            /* M5 (c05): ECALC_OUT_PACKED=2, the 60-bit form below; reserved0 = 60 (the bits per limb) */
#define ECP_HDR_BYTES  4096u
#define ECP_ENDIAN     0x01020304u   /* as the writer's host stores it: 04 03 02 01 on a little-endian host */
#define ECP_NQ         8
#define ECP_TEXT_OFF   1024          /* a human-readable description of the file */

typedef struct {
    char     magic[8];               /* "ECPACK18" (not NUL-terminated) */
    uint32_t version, hdr_bytes;     /* 1, 4096 */
    uint32_t endian;                 /* ECP_ENDIAN */
    uint32_t limb_bytes, limb_digits;/* 8, 18 */
    uint32_t order;                  /* 1: the most significant limb first */
    uint32_t part, nparts;           /* this file's part index (0 = the top limbs) and the number of parts */
    uint32_t reserved0;
    uint64_t d, d_out;               /* the digits computed (a multiple of 18) and written (the ASCII file's fraction) */
    uint64_t nl, pad;                /* X's limbs, the leading zero characters of the top limb */
    uint64_t lo, hi;                 /* this part's limbs [lo, hi) of X (limb 0 the lowest) */
    uint64_t k0, k1;                 /* this part's digits [k0, k1) (before the cut at d_out) */
    uint64_t complete;               /* 1 when the run closed the file (the residues are valid) */
    uint64_t nq;                     /* ECP_NQ */
    uint64_t q[ECP_NQ];              /* the T1 primes */
    uint64_t dres[ECP_NQ];           /* the digits [k0, k1) as a decimal number mod q[i] */
} ecp_hdr;                           /* (the rest of the 4096 bytes: zero, and the text at ECP_TEXT_OFF) */

/* M5 (c05): version 2, the 60-bit packed form (ECALC_OUT_PACKED=2).  A limb of base 10^18 is < 2^60, so the file holds the
 * limbs in file order (the most significant first, index j = 0 .. hi-lo-1) as a little-endian bit stream, limb j in the bits
 * [60 j, 60 j + 60): two limbs a (j even) and b take 15 bytes, the low 8 of  a | b << 60  and then the 7 bytes of  b >> 4;
 * a last odd limb takes 8 bytes (its 60 bits, zero-extended).  A part of n limbs is ecp60_bytes(n) = 15 (n/2) + 8 (n&1)
 * bytes after the header (7.5 B per limb = 0.4167 B/digit against 0.444 for version 1: -6.25 %).  Everything else (header
 * fields, residues, part ranges) is version 1's. */
#define ECP60_BITS 60
static inline uint64_t ecp60_bytes(uint64_t n) { return 15 * (n / 2) + 8 * (n & 1); }
/* the pair at out[0..15) from limbs a, b (< 2^60) */
static inline void ecp60_put_pair(uint8_t *out, uint64_t a, uint64_t b)
{
    uint64_t w0 = a | (b << 60), w1 = b >> 4;
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(w0 >> (8 * i));
    for (int i = 0; i < 7; i++) out[8 + i] = (uint8_t)(w1 >> (8 * i));
}
static inline void ecp60_get_pair(const uint8_t *in, uint64_t *a, uint64_t *b)
{
    uint64_t w0 = 0, w1 = 0;
    for (int i = 0; i < 8; i++) w0 |= (uint64_t)in[i] << (8 * i);
    for (int i = 0; i < 7; i++) w1 |= (uint64_t)in[8 + i] << (8 * i);
    *a = w0 & ((1ULL << 60) - 1); *b = (w0 >> 60) | (w1 << 4);
}
/* n limbs l[0..n) from an even file index to out (ecp60_bytes(n) bytes); the OpenMP loop over pairs */
static inline void ecp60_pack(const uint64_t *l, size_t n, uint8_t *out)
{
    size_t np = n / 2;
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < np; i++) ecp60_put_pair(out + 15 * i, l[2 * i], l[2 * i + 1]);
    if (n & 1) { uint64_t a = l[n - 1]; uint8_t *o = out + 15 * np; for (int i = 0; i < 8; i++) o[i] = (uint8_t)(a >> (8 * i)); }
}
/* n limbs from in (an even file index) to l[0..n) */
static inline void ecp60_unpack(const uint8_t *in, size_t n, uint64_t *l)
{
    size_t np = n / 2;
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < np; i++) ecp60_get_pair(in + 15 * i, &l[2 * i], &l[2 * i + 1]);
    if (n & 1) { uint64_t a = 0; const uint8_t *o = in + 15 * np; for (int i = 0; i < 8; i++) a |= (uint64_t)o[i] << (8 * i); l[n - 1] = a; }
}
/* The writer's chunk step: the chunk's cnt >= 1 limbs in file order (pk) appended to the stream; *has_pend / *pend carry an odd
 * last limb of a non-final chunk to the next one.  Writes the bytes to ob (room for ecp60_bytes(cnt + 1) bytes), returns their number. */
static inline size_t ecp60_stream_chunk(const uint64_t *pk, size_t cnt, int final, int *has_pend, uint64_t *pend, uint8_t *ob)
{
    const uint64_t *src = pk; size_t n = cnt, nb = 0;
    if (*has_pend) { ecp60_put_pair(ob, *pend, pk[0]); nb = 15; src++; n--; *has_pend = 0; }
    size_t keep = (!final && (n & 1)) ? 1 : 0;
    ecp60_pack(src, n - keep, ob + nb); nb += (size_t)ecp60_bytes(n - keep);
    if (keep) { *pend = src[n - 1]; *has_pend = 1; }
    return nb;
}
#endif
