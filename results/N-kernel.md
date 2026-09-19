# N-kernel — B1 (Q₂ transformed once per pair) and B4 (ds_swizzle exchange in the register-blocked body)

Branch `nkernel` (base `main` @ a75474d). Files: `ecalc/ntt.c/.h`, `ecalc/ntt3.c`, the batch-local
path of `ecalc/rns_mul.c` (`rns_mul_batch_local`, `k_scatter4`), `ecalc/rns_mul.h` (one `extern` and two
stat fields), `ecalc/tests/t_ntt.c`, `ecalc/tests/t_mul.c`, `ecalc/Makefile` (one line: `t_ntt` links
`ntt3.o`).

## Design

### B1 — the pointwise operand's layout (`ntt.h`: `NTT_Y_FULL / BCAST / PAIR`)

The fused inverse (`k_b1<LGL, 2>`, `k_b1s<LGL, 2>`) read `y[(base + k) & ymask]` — a mask that could
express "one y per x" and "one y for all" but not "one y per pair", and only for power-of-two lengths.
The mask is replaced by a per-block index: with `Lt` points per transform (2^k, or 3·2^k when the
b1 pass runs over the thirds of a radix-3 transform), block `base` belongs to transform `t = base / Lt`
at point `k = base − t·Lt`, and reads y transform `t` (FULL), `0` (BCAST) or `t >> 1` (PAIR) at the same
`k` (`y_base()` in `ntt.c`; one 64-bit division per block of 1024 points). The b1 blocks never straddle
a transform (Lt is a multiple of 1024), so the mapping is uniform in a block. The unfused pointwise
gets the same layouts (`k_pw_y`, `ntt_pw_y`), with `t = (i >> logk) / 3` for the radix-3 case.

Entry points: `ntt_inv_pw_y(c, x, y, ymode, logn, batch)`, `ntt_inv3_pw_y(c, x, y, ymode, logk, batch)`;
the old `ntt_inv_pw`, `_bcast`, `ntt_inv3_pw`, `_bcast` are wrappers. Two consequences beyond the pair
mode:
* the **radix-3 lengths are not excluded** (PLAN I6 assumed a power-of-two mask); since nearly every
  batch level at 4 × 10¹⁰ runs at 3·2^k (levels 4–22 in the baseline log) this is where the gain is;
* the radix-3 inverse now **fuses the pointwise product** into its first pass (`ntt_inv3_core_pw`,
  from logk + 1 ≥ PW_FUSE) instead of a separate `k_pw` pass — the same modmul on the same canonical
  operands, so bit-identical, and one plane read+write per prime per product less.

The batch-local tier (`rns_mul_batch_local`): `pair` when N is even and every product 2j, 2j+1 shares
`b`/`nb` and the result device (the tree's `P₁Q₂ + P₂`, `Q₁Q₂` — `binsplit.c` builds exactly this);
pairs stay adjacent in each device's list (both go to the region's device, in order) and tiles start at
even positions because Mmax is forced even. `k_scatter4` writes B once per pair into planes of M/2
transforms (`plane_b`), B is transformed with batch M/2, and the inverse runs with `NTT_Y_PAIR`. The
tile budget in pair mode counts a + b/2 planes (`2 · rns_batch_tile_bytes / (3 L · 8) / 4`, at least
2) so the last levels (M = 1 before) can pair up to the pool limit; the non-pair tiling is unchanged.
`RNS_BATCH_PAIR=0` disables it; `RNS_VERBOSE` lines carry " pair".

Per pair the tier does 5 transforms instead of 6 (a₁, a₂, b, two fused inverses); the scatter writes
half the B planes.

### B4 — the register-blocked body's last exchange

`k_b16r` (body 1/2): group B → group C (forward) and C → B (inverse) move rows between threads tt and
tt ^ 1, i.e. lanes l and l ^ 16 of one wavefront (lane = 16 tt + bb), and each thread keeps half of its
8 rows: 4 values cross per thread. With `NTT_B16_XCHG=1` the exchange is 4 × 2 `ds_swizzle_b32`
(bitmask mode xor 16, pattern 0x401F) selected by lane parity, no LDS traffic and no barrier, instead
of 8 stores + barrier + 8 loads; the A ↔ B exchange (rows across all 16 tt, i.e. across four
wavefronts) stays in LDS. The values are the same, so the output is bit-identical (`t_ntt` §4 hashes
the forward output and an inverse pass output against the tile kernel for body 1/2 × xchg 0/1).

Note: `ecalc` runs with **body 0 (the paper's tile kernel)** — `ntt_b16_body = 0` is the default in
`ntt.c` and the 98.5 s baseline's META line says `body=0`. RESULTS §43 recommended body 1 but left the
default so the accepted numbers stood. Body 1 and B4 only touch 7-stage passes, i.e. batch levels with
logk ≥ 17 (≈ 14.6 s of the batch tier's 19 s of transforms) — the distributed tier's local transforms
are 2^15–2^16 (tile kernel only).

## Tests

(filled in below as the batches complete)

## Timing (4 × 10¹⁰, size 1, reference evicted)

(filled in below)

## Open issues

