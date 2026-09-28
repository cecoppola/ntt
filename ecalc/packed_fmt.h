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

#endif
