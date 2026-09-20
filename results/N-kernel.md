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

## Tests (aac6, clone `~/ntt-nkernel`, job 20716 on s24-16, then job 2 below)

| test | command | result |
|---|---|---|
| `t_ntt` | `./tests/t_ntt 31` | **VERIFY OK (593 checks)**: DFT, round trips 2^10–2^31 × 4 primes, convolution, STG 3–7, body 0/1/2 × xchg 0/1 forward and inverse hashes all `== tile kernel`, the new §4b layouts (FULL/BCAST/PAIR × fused/unfused × 2^k/3·2^k, k = 10..15, against per-transform product + inverse, and the old entry points) |
| `t_mul 20` (decimal) | `./tests/t_mul 20` | VERIFY OK (189 checks) incl. the new §4b: device-pool pairs sharing B with the added operand, 2^k and 3·2^k, several tiles, odd per-device counts; paired == unpaired bit for bit, normalised lengths checked |
| `t_mul 0 batch` (binary) | `LIMB_BASE=2 ./tests/t_mul 0 batch` | VERIFY OK (72 checks) |
| `t_mul 0 big` | `./tests/t_mul 0 big` | VERIFY OK (102 checks) |
| `t_bs` | `./tests/t_bs`, `LIMB_BASE=2 ./tests/t_bs` | **VERIFY OK (10 checks) in both bases** (job 20729; the first run "failed" 2 of 10 only because the gitignored `ref/e_*.sha256` were not in the clone — linked from `~/ntt` since) |
| 10⁹ | `env <cfg> ./ecalc 1000000000`, `cmp` vs `ref/e_1000000000.txt` | **identical** with `RNS_BATCH_PAIR=0`, `=1` (16 paired levels), `NTT_B16_BODY=1 NTT_B16_XCHG=1`, and `LIMB_BASE=2`; VERIFY OK each |

`t_ntt` rates at 2^31 (APU0, one 4-pass transform): tile kernel 1.12 / 1.17 TB/s (fwd / inv), body 1
1.43 / 1.33, body 2 1.44 / 1.33, **body 1 + ds_swizzle exchange 1.23 / 1.21** — B4 is *slower*
(−14 % fwd, −9 % inv); batched log L = 17: 1485 → 1563 (body 1) → 1370 GB/s (xchg).

## Timing (4 × 10¹⁰, size 1, reference evicted, `RNS_VERBOSE=1 ECALC_VERBOSE=2`; job 20716, s24-16)

| | baseline (§72, s24-26) | this branch, `RNS_BATCH_PAIR=0` | `RNS_BATCH_PAIR=1` |
|---|---:|---:|---:|
| init | 15.3 | 17.4 | 18.2 |
| bs (batch / mdev) | 45.7 (30.5 / 14.9) | 42.3 (29.0 / 13.1) | **38.3 (25.1 / 12.9)** |
| batch-local sums: scatter / ntt / crt / merge | 1.18 / 19.05 / 2.62 / 1.28 | 1.26 / 18.38 / 2.65 / 1.18 | **1.06 / 15.67 / 2.58 / 0.91** |
| dm (recip) | 37.5 (15.4) | 33.7 (14.9) | 33.9 (15.0) |
| **wall** | 98.5 | 93.6 | **90.6** |
| digits | identical | identical | identical (batch 2) |

`RNS_BATCH_PAIR=0` on this branch still carries the radix-3 fusion of the pointwise product (the
unpaired ntt sum 19.05 → 18.38 s, −0.7 s); the pairing takes the ntt sum to 15.67 s (−2.7 s more,
−14.7 %: one transform in six on the paired levels, all of levels 1–21 pair; level 22 stays on the
striped path) and the scatter to 1.06 s. **Batch tier 29.0 → 25.1 s, wall −3.0 s** against the same-day
unpaired run (the day's runs are all ≈ 5 s faster than the §72 baseline — a different node, s24-16 vs
s24-26 — so the comparison is within the batch).

**Batch 2 (job 20729, s24-30, one run each, all VERIFY OK, digits identical to `results/e_4e10.out`
where the `cmp` ran):**

| `RNS_BATCH_PAIR` / `NTT_B16_BODY` / `NTT_B16_XCHG` | 1 / 0 / 0 | 1 / 1 / 0 | 1 / 1 / 1 | 0 / 0 / 0 |
|---|---:|---:|---:|---:|
| init | 19.1 | 17.2 | 18.1 | 18.9 |
| bs (batch / mdev) | 39.5 (25.8 / 13.4) | **39.6 (25.9 / 13.3)** | 40.6 (26.8 / 13.5) | 43.6 (30.0 / 13.3) |
| batch-local sums: scatter / ntt / crt / merge | 1.11 / 16.12 / 2.64 / 0.84 | 1.12 / **15.69** / 2.63 / 0.84 | 1.12 / 16.71 / 2.64 / 0.82 | 1.33 / 19.04 / 2.73 / 1.27 |
| dm (recip) | 36.7 (17.3) | 35.2 (15.4) | 35.7 (15.7) | 35.1 (15.5) |
| **wall** | 95.5 | **92.1** | 94.6 | 97.8 |
| digits | identical | identical | identical | (`cmp` cut by the 45-min job limit; identical in batch 1) |

Reading the two batches together (the node-to-node and run-to-run spread is ≈ 2–3 s on the wall, but
the batch-local sums are stable to ≈ 0.3 s):

* **B1 wins: batch tier 29.0–30.0 → 25.1–25.9 s (−4 s), of which the transforms 18.4–19.0 → 15.7–16.1 s
  and the scatter −0.2 s;** wall −3…−5 s. Adopted (default `RNS_BATCH_PAIR=1`).
* **The register-blocked body (`NTT_B16_BODY=1`) is a small consistent win** on the transforms
  (batch ntt 16.12 → 15.69 s; the 2^31 single transforms +20 % in `t_ntt`) and bit-identical
  everywhere (4 × 10¹⁰ digits identical with it). Made the default in `ntt.c` (`NTT_B16_BODY=0` restores
  the paper's tile kernel). The dm phase's ±1.5 s between the runs is the reciprocal's own spread.
* **B4 loses: the ds_swizzle exchange is slower** — `t_ntt` 2^31: 1.43 → 1.23 TB/s forward, 1.33 →
  1.21 inverse; batched log L = 17: 1563 → 1370 GB/s; 4 × 10¹⁰ batch ntt 15.69 → 16.71 s. The code stays
  behind `NTT_B16_XCHG=1` (default 0) as the measured answer to RESULTS §49's "if it is ever worth 5 %":
  it is not. Why: the LDS round trip it replaces is 8 `ds_write_b64` + barrier + 8 `ds_read_b64` per
  thread; the swizzle path is 8 `ds_swizzle_b32` (two per 64-bit value, 4 values) plus 16 lane-parity
  selects and their live ranges — the same number of LDS-pipe instructions, no LDS bandwidth saved that
  the kernel was short of (RESULTS §48: the LDS ceiling was relieved by body 1 itself), and the extra
  VALU selects and registers cost occupancy in a kernel that is HBM-bound with the LDS traffic
  co-issued. §49's 1.6× per exchange is a latency figure for a dependent chain, not a throughput one for
  this body. Not adopted.

**Batch 3 (job 20744, s24-30): 10⁹ with the branch's defaults (body 1, pair) — decimal 13.5 s and
binary (`LIMB_BASE=2`) 20.3 s, VERIFY OK, both `cmp`-identical to `ref/e_1000000000.txt`; META `body=1`.**

## Where the batch tier stands (4 × 10¹⁰)

Baseline (§72): batch 30.5 s = scatter 1.18 + ntt 19.05 + crt 2.62 + merge 1.28 (+ layout/add-norm).
Now (pair + body 1): batch 25.9 s = scatter 1.12 + **ntt 15.69** + crt 2.63 + merge 0.84. The
transforms are 5 per pair instead of 6 and run at ≈ 1.45 TB/s per APU on the long levels; the next
step inside this tier would be the CRT (2.6 s, unchanged) and the last two levels (M = 1–2 per tile:
level 22 still goes through the striped path at 2.2 s).

## Open issues

* B4 is closed negative; the swizzle path is kept only as the measurement's evidence (delete if unwanted).
* Level 22 (4 pairs at 3·2^28, over the local tier's pool) is not paired (striped path, 2.19 s); a
  pair there needs planes of 2 × 3·2^28 × 4 primes = 51 GB — the 3·2^30 plane variant A-grid measures.
* The batch-local tile budget in pair mode (`2 · rns_batch_tile_bytes / (3 L · 8) / 4`, at least 2)
  is chosen so that the 15 GB budget covers a + b/2 planes; a larger budget (the pools allow 2^29
  points per prime plane) was not explored — tiles at the low levels are already 10⁵ products.
* The distributed tier (`ntt_dist.c`) uses `ntt_pw` (FULL) and 2^15–2^16-point local transforms;
  neither B1 nor body 1 touches it.

## Touched outside my files

`ecalc/Makefile`: one line, `tests/t_ntt` links `ntt3.o` (the layout checks exercise the radix-3
inverse). `ecalc/rns_mul.h`: `extern int rns_batch_pair` and two `rns_stats` fields
(`n_batch_local`, `n_batch_pair`) used by `t_mul`'s new section. `ecalc/ntt3.c` (listed as mine if
needed): `ntt_inv3_pw_y` and the fusion; `k_pw_bcast3` removed (subsumed by `k_pw_y`).

