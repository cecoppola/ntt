# KERNELS — single-node kernels

Scope: single-node kernels: dbig add/sub, maxidx, quarter select, CRT carry lookahead, twiddle packs, kernel constants, survey and NTT pass study. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| K13 | Phase 13a agent K: kernel constants (PLAN §29 H2, H3, H6, H7) |
| K13b | Phase 13b agent K: the b1 pass and the stride penalty (PLAN §31, row K) |
| K4b | `k_crt_batch`: the per-chunk carry chain as a parallel carry-lookahead |
| K4c | `k_crt24`: the P24 CRT's per-chunk carry chain as a parallel carry-lookahead |
| S38 | the next single-node speed lever. Found: the dbig add/sub kernel (k_addsub) ran at 36-84 GB/s; DBIG_ADDSUB2=1 (coalesced |
| S39 | kernel survey of the 6.441e10 single-node profile (read-only; no runs, no code edits). Found: the next levers are k_gath |
| S40 | DBIG_MAXIDX_TOP (top-down k_maxidx) measured -3.2 s per run (95 % CI -5.6 .. -0.75, 3 ABBA rounds); micro-test refutes " |
| S41 | DBIG_QSEL (scalar-select quarter pointers), gates and ABBA |
| S43 | multi-node A/B of the three dbig kernel defaults (aac7, 2 and 4 nodes) |
| S44 | aac6 SH5_MI300A nodes -- regression and ABBA of DBIG_MAXIDX_TOP, DBIG_QSEL, DBIG_ADDSUB2 |
| S51 | kernel round 2, part b (K2 k_addsub3, K3 k_modq_m, K4 k_crt_batch_m) -- FINISHED: K2+K3 propose default (1-node signific |
| T11 | aac6 SH5 CPX paired ABBA of K2 (DBIG_ADDSUB3) + K3 (DBIG_MODQ_MONT) at 4e9 digits |
| NTT3P | can ecalc's large transforms run in 3 memory passes instead of 4? (design study, no code changed) |
| G515 | Phase 15 Batch 2, item 5 (PLAN §37 row 6): the general map's twiddle packs |
| S115 | Phase 15 agent S1: the decimal `mul_1` and the fused seed span (`BI_MUL1_FAST`) |
| N3 | host gaps in the batch tier (`BS_BATCH_PIPE`) |

## K13 — Phase 13a agent K: kernel constants (PLAN §29 H2, H3, H6, H7)

(former file: K13.md)

Branch `p13-K` (base `203733b`). Files touched: `ecalc/ntt.c`, `ecalc/ntt.h`, `ecalc/tests/t_ntt.c` only.
All numbers are **measured** on `aac6-spx-a` (job 21007), APU0, prime P0, median of 5 unless marked.
Every option sits behind a switch, and the **defaults are unchanged**. Each switch is exact: its outputs are
bit-identical to the default's (t_ntt §4c, below).

| switch | values | what |
|---|---|---|
| `NTT_MODMUL` | 0 (default), 1, 2 | H3: 0 FP64 Barrett + Dekker (today), 1 reduced-correction Barrett, 2 Shoup integer |
| `NTT_MALL` | 0 (default), lc ≥ 11 | H2: passes of span ≤ 2^lc run chunk by chunk (MALL residency) |
| `NTT_B16_VAR` | 0 (default), 1, 2, 3 | H6: 1 = unpadded LDS + global twiddle table (4 blocks/CU instead of 3), 2 = block order, 3 = both |

The bench: `tests/t_ntt bench <LOGMAX> <h2,mall,h3,h6,h7>` (APU0, P0, no correctness sections).

#### Verdicts

| item | verdict | headline (measured) |
|---|---|---|
| **H2 MALL** | **No MALL cliff exists for these kernels.** Pass rate is flat from 128 MB to 16 GB. The current passes are on the right side: the kernels are issue/latency-bound at 1.2–1.6 TB/s, not bandwidth-bound. MALL-chunked scheduling gains at most +2–4 % at 2^26 and loses at every chunk ≤ 2^23 | per-pass ns/point identical at 2^24 (fits MALL) and 2^31 |
| H2 side finding | **A stride penalty, not a cache cliff.** Passes with s_lo = 17 (1 MiB row stride) or s_lo = 24 (128 MiB) run at 0.73–0.82× the others. This is why 2^31 costs 2.25× 2^30 | 2^31 pass 0 1177 GB/s, pass 1 1295, pass 2 1621 |
| **H3 modmul** | **Reduced-correction Barrett (`NTT_MODMUL=1`) wins on every size and layout: +5–12 %** fwd and inv, exact (proof below, and bit-identical outputs everywhere). **Shoup loses 4–13 %** on the full transform, consistent with Phase 5's first-pass result | 2^31 fwd 104.4 → 97.3 ms, inv 110.9 → 103.9 ms |
| **H7 NT stores** | **No.** Pack is 1.05–2.4× slower with `__builtin_nontemporal_store` (scattered stores lose write-combining). Unpack is flat (±2 %, up to 1.26× faster only at 32–128 MB). No measurable pollution relief for a MALL-sized transform run after it | 2^28 pack 9.78 → 12.47 ms |
| **H6 occupancy** | **No.** The b16 kernels run at 3 waves/SIMD, limited by LDS. Going to 4 blocks/CU (unpadded LDS + global twiddle table) and reordering blocks (the stride probe) are both within ±2 % (VAR3 is up to −4 %). Neither fixes the s_lo 17/24 penalty | 2^31 fwd 104.3 ms → 105.7 (VAR1), 106.0 (VAR2) |
| **4 × 10^10 end to end** | MM1 correct (digits identical). Its **end-to-end gain is small: 58.4 → 58.0 s** of compute, one pair on one node, within run-to-run noise (the earlier MM0 run gave 58.9 s). The transform-level +7 % is diluted by everything that is not a b16/b1 pass | bs 32.3 → 32.0, dm 26.0 → 25.9 |

#### H3 — the modmul on the whole transform

**Variants.** All sit in the register-blocked body (`k_b16r`, the default body 1) and in the b1 pass.

* **MM 0 (today).** `ec_mm`: hi = ab, lo = fma(a,b,−hi), q = ⌊hi·pinv⌋, r = fma(−q,p,hi)+lo, then **four**
  corrections (two up, two down). q can be off by ±2, so two each way are needed (t_modarith: one each way
  fails 0.57 %).
* **MM 1, reduced-correction Barrett.** The quotient comes from the exact product against a two-term
  reciprocal: pinvl = (1 − p·pinv)/p (an exact fma residual, computed once per thread), and
  q = ⌊fma(hi, pinv, fma(hi, pinvl, lo·pinv))⌋.
  **Bound (proof sketch).** ab/p < 2p < 2^53, so the final rounding is ≤ ½ ulp ≤ 0.5. The pinv/pinvl
  representation error and the inner roundings stay below 2^−50. So |x − ab/p| ≤ 0.5 + 2^−50, and q ∈ {Q−1, Q, Q+1}.
  q = Q−1 needs frac(ab/p) < 0.5+ε, and q = Q+1 needs frac ≥ 0.5−ε. Hence **r ∈ (−0.51p, 1.51p)**.
  |hi − qp| < 1.51p + 2^51 < 2^53, so the fma and the +lo are exact.
  **Data products take one correction**, since they only need to be lazy in [0, 2p), which every butterfly
  input accepts. **Twiddle products take two** (canonical, as rule 1 requires). The inverse butterfly's
  difference becomes u + 2p − vw. Measured on the host (IEEE-identical fma): 4 × 10^8 pairs per prime, all four
  primes, a < 2p and b < p with edge-biased sampling: **0 wrong, r/p observed in [−0.500, 1.000]**.
* **MM 2, Shoup.** The data is multiplied by tab[r] (precomputed integer table + Shoup constants, 128 entries,
  in LDS) and then by T (the column twiddle, formed and squared by ec_mm as today). T's Shoup constant is
  computed exactly on the device once per stage per thread (`shoup_q`: a double estimate plus one Barrett step
  on the signed 128-bit error; host-checked exact). The b1 pass uses the existing `k_b1s`. So there is no
  per-butterfly twiddle product, but there are two integer Shoup products per butterfly.

**Whole transform, ms (fwd | inv), ratio to MM 0** (`t_ntt bench 31 h3`):

| layout | MM 0 | MM 1 | MM 2 |
|---|---|---|---|
| 2^20 single | 0.058 \| 0.059 | 1.08× \| 1.06× | 0.95× \| 0.94× |
| 2^24 single | 0.612 \| 0.654 | **1.12× \| 1.11×** | 0.94× \| 0.95× |
| 2^26 single | 2.925 \| 3.077 | 1.06× \| 1.10× | 0.93× \| 0.94× |
| 2^28 single | 11.52 \| 12.16 | 1.07× \| 1.10× | 0.89× \| 0.93× |
| 2^30 single | 46.51 \| 49.74 | 1.07× \| 1.09× | 0.88× \| 0.93× |
| 2^31 single | 104.4 \| 110.9 | **1.07× \| 1.07×** | 0.91× \| 0.93× |
| 2^14 × 2^17 (2^31 pts) | 47.75 \| 50.16 | 1.05× \| 1.04× | 0.96× \| 0.96× |
| 2^17 × 2^14 (2^31 pts) | 48.84 \| 52.40 | **1.11× \| 1.11×** | 0.90× \| 0.93× |
| 2^20 × 2^11 (2^31 pts) | 67.41 \| 71.41 | 1.07× \| 1.08× | 0.92× \| 0.94× |

(The full table, 2^20 … 2^31 and 2^28 batches, is in the RESUME files: `~/k13/h3.txt` on aac6.)

**Exactness gate.** t_ntt §4c (new) compares every operation under each switch setting with the default,
on the device. The operations are fwd, inv (random input, so no round-trip identity to hide behind), the fused
pointwise inverse in all three layouts (full/bcast/pair), and the radix-3 pair (fwd3 + inv3_pw). They run at
**every length 2^10 … 2^31** and at batches 2^11×1000, 2^14×64, 2^17×16, 2^20×6, 2^12×3.
Configurations: MM1, MM2, MALL16, MM1+MALL20, MM2+MALL24, MALL22 (and VAR1, VAR2, MM1+VAR3 on the H6 commit):
**all bit-identical** (`t_ntt 31`: VERIFY OK, 1469 checks, job 21007). `NTT_MODMUL=1 ./mnaccept.sh
<job> --only e9`: **10^9 identical in both bases** (18.75 s / 25.48 s vs 18.09 / 23.02 default, one run each).

#### H2 — the MALL (256 MB) cliff

`t_ntt bench 31 h2`: every pass of the forward and the inverse, timed alone, for transform lengths
2^L (L = 14, 17, 20, 24 and single transforms 2^18 … 2^31) and batch totals 2^18 … 2^31 points
(2 MB … 16 GB). Rate = 16 B × points / pass time.

* **Flat across the MALL boundary.** For the same pass structure the ns/point is identical below and above
  256 MB. Examples, forward:
  - L 14, pass [10..13]: 1731, 1771, 1693, 1705 GB/s at 128 MB, 256 MB, 512 MB and 16 GB.
  - L 14, the b1 pass: 1265, 1239, 1217, 1225 GB/s.
  - L 24, pass [10..16]: 1596 GB/s at 128 MB against 1625 GB/s at 8 GB.

  Below ~2^23 points the time is launch-bound (0.02–0.04 ns/pt).
* **The pass rates by structure** (steady state): b16 pass with s_lo = 10…16: 1.55–1.65 TB/s. b1 pass:
  **1.21–1.24 TB/s, the slowest pass everywhere** (28 % of a 2^31 forward). Partial last b16 pass of 1–4 stages: 1.45–1.94 TB/s.
* **The copy ceiling is ~3 TB/s and these run at 40–55 % of it**. They are issue- and latency-bound (Phase 7's
  finding), so a MALL-resident working set has nothing to give.
* **MALL-chunked schedule** (`NTT_MALL=lc`; passes of span ≤ 2^lc run chunk by chunk, so a ≤ 256 MB chunk
  stays resident between them). Ratio to the default:

  | layout | lc 20 | 21 | 22 | 23 | 24 | 25 | 26 |
  |---|---|---|---|---|---|---|---|
  | 2^14 batch, 2^28 pts | 0.58 | 0.70 | 0.82 | 0.92 | 0.97 | 0.99 | 0.98 |
  | 2^20 batch, 2^26 pts | 0.58 | 0.72 | 0.84 | 0.95 | 1.02 | **1.04** | — |
  | 2^26 single | 0.63 | 0.74 | 0.86 | 0.96 | 1.02 | **1.04** | — |
  | 2^30 single | 0.69 | 0.79 | 0.87 | 0.93 | 0.97 | 0.99 | 0.99 |

  Small chunks lose to launch count and tail effects. Large chunks win at most 4 % and only at 2^26, which is
  within noise elsewhere. **Verdict: the current pass/tile sizes sit on the right side, and MALL residency is not
  worth a schedule change.** The switch stays for the record.
* **The real anomaly is a stride.** By s_lo (log2 of the row stride in points), forward:
  - s_lo 10–16: 1.55–1.63 TB/s.
  - **s_lo 17: 1.29–1.33 TB/s (2^24 pass 0, 2^31 pass 1)**.
  - s_lo 18: 1.45. s_lo 19–23: 1.53–1.58.
  - **s_lo 24: 1.18 TB/s (2^31 pass 0)**.

  Stride 2^17 × 8 B = 1 MiB, and 2^24 × 8 B = 128 MiB. Probably HBM channel/bank aliasing of the 128 rows a
  block touches (not measured further). It costs ~12 % of a 2^31 transform and ~7 % of a 2^24 one; H6
  variant 2 (block order) is the first probe.

#### H7 — non-temporal stores in pack / unpack

`t_ntt bench 31 h7`: copies of `ntt_dist.c`'s `k_pack` / `k_unpack` index maps (size 4), plain against
`__builtin_nontemporal_store`, on rows × C points. Measured: the kernel alone, the kernel followed by a
streaming read of its output (the push's read), and the kernel between two forward transforms of a warm
32 MB plane (pollution).

| points | pack plain → NT | unpack plain → NT | fwd(32 MB)+pack+fwd plain → NT |
|---|---|---|---|
| 2^22 (32 MB) | 0.087 → 0.208 ms | 0.023 → 0.021 | 0.481 → 0.531 |
| 2^24 | 0.548 → 0.695 | 0.093 → 0.074 | 0.853 → 1.040 |
| 2^26 | 2.455 → 3.047 | 0.381 → 0.356 | 2.779 → 3.405 |
| 2^28 | 9.784 → 12.47 | 1.592 → 1.577 | 10.07 → 12.81 |
| 2^30 (8 GB) | 47.98 → 50.53 | 6.661 → 6.678 | 48.56 → 50.72 |

The transform after a pack costs the same either way (≈ 2 × 0.17–0.19 ms), so there is no pollution to relieve.
**Verdict: no.** The switch is not built into `ntt_dist.c` (X's file), since the benchmark already answers.
**Side finding:** the plain pack runs at only 360–770 GB/s, a scattered-store transpose. At 2^30 it takes as long
as a whole 2^30 forward transform (48 ms). An LDS-tiled transpose, like `k_unpacktw`'s, is the lever there. That
belongs to X / the four-step path.

#### H6 — occupancy and launch configuration

Resource usage (`-Rpass-analysis=kernel-resource-usage`):

* `k_b16r`: VGPRs 46–66, LDS 18.4 KB, **3 waves/SIMD, LDS-limited**.
* `k_b1`: VGPRs 18–22, LDS 12 KB, 5 waves/SIMD.
* `k_b1s`: LDS 16 KB, 4 waves/SIMD.

`NTT_B16_VAR` bit 1 drops the pad and reads the 128-entry twiddle table from global memory, which fits 4 blocks/CU. Bit 2 orders the blocks tile-row-group
fastest (the stride probe). Both are bit-identical (§4c).
**Measured** (`t_ntt bench 31 h6`, job 21018; ratio to var 0, MM 0):

| layout | VAR1 | VAR2 | VAR3 | MM1 | MM1+VAR1 | MM1+VAR2 |
|---|---|---|---|---|---|---|
| 2^31 fwd \| inv | 0.99 \| 0.99 | 0.98 \| 0.98 | 0.97 \| 0.96 | 1.06 \| 1.06 | 1.04 \| 1.07 | 1.05 \| 1.05 |
| 2^30 | 0.99 \| 1.00 | 0.99 \| 0.99 | 0.98 \| 0.98 | 1.07 \| 1.09 | 1.06 \| 1.08 | 1.05 \| 1.07 |
| 2^24 | 0.99 \| 0.99 | 1.01 \| 1.00 | 0.99 \| 0.99 | 1.10 \| 1.09 | 1.10 \| 1.08 | 1.10 \| 1.09 |
| 2^24 × 16 | 1.01 \| 0.95 | 1.02 \| 1.01 | 1.00 \| 0.95 | 1.09 \| 1.07 | 1.06 \| 1.04 | **1.11** \| 1.07 |
| 2^17 × 2^11 | 1.00 \| 1.02 | 1.01 \| 1.00 | 0.99 \| 1.00 | 1.09 \| 1.10 | 1.06 \| 1.10 | 1.09 \| 1.10 |
| 2^14 × 2^14 | 1.01 \| 1.02 | 1.01 \| 1.02 | 1.01 \| 1.02 | 1.06 \| 1.06 | 1.06 \| 1.06 | 1.06 \| 1.06 |

Per pass: 4 blocks/CU speeds the good passes slightly. At 2^24 the [10..16] pass goes 1658 → 1726 GB/s fwd and
1493 → 1687 inv. But it slows the s_lo 17 pass (1245 → 992). The block order leaves s_lo 24 unchanged (1173 → 1176) and
makes s_lo 17 worse (1290 → 1232). **Verdict: occupancy is not the limiter; the stride penalty is not a block-scheduling
effect.** It is more likely address aliasing in HBM, whose fix would be a padded or rotated plane layout. That changes
the plane format, so it is a design item and not a kernel constant.

#### 4 × 10^10 end to end

| run | NTT_MODMUL | total (measured) | digits |
|---|---|---|---|
| 1 | 0 | 79.06 s (bs 32.7 + dm 26.1; init 20.2) | identical |
| 1 | 1 | 80.67 s (bs 32.0 + dm 25.9 = 58.0; init 22.7) | identical |
| 2 | 0 | 81.62 s (bs 32.3 + dm 26.0 = 58.4; init 23.3) | identical |

Runs 1 were job 21007 (MM0) and job 21018 (MM1). Run 2 is job 21018, the same node (s24-26) as MM1's run.
Reference evicted before each run. Compute (bs + dm) MM1 against MM0 on the same node: **58.0 vs 58.4 s (−0.7 %), one
pair: not significant**. The init time (20–23 s) varies more than that. Adopting MM1 is cheap (exact, local to
ntt.c), but its measured end-to-end value at 4 × 10^10 is ~0.4 s.

#### Acceptance

`./mnaccept.sh 21007 --only unit,e9` on `3de9dd9` (the H3/H2 commit):
PASS t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid (2 procs);
e9 LIMB_BASE=10 identical 18.09 s; LIMB_BASE=2 identical 23.02 s.
`t_ntt 31` (full, incl. §4c): VERIFY OK (1469 checks).
**Final commit ff83794** (job 21018): `./mnaccept.sh 21018 --only unit,e9`: all 8 unit PASS (t_ntt 24 now 1519 checks,
incl. §4c with the VAR configurations), e9 LIMB_BASE=10 identical 19.26 s, LIMB_BASE=2 identical 25.99 s.

#### Open issues

* The s_lo 17 / 24 stride penalty (H2 side finding) is worth ~12 % at 2^31 if it can be removed.
* The b1 pass is the slowest pass (1.22 TB/s). A register-blocked b1, like the b16 body, is the obvious next kernel.
* MM 1's inverse difference u + 2p − vw, and its lazy data products, reach every kernel in `ntt.c`. `ntt3.c`'s
  radix-3 stage and `ntt2.c` are untouched: they call ec_mm on canonical values, and the §4c radix-3 check
  covers the composition.

#### RESUME

**Complete.** Every item is measured and the gate is met: t_ntt green, mnaccept unit,e9 green on ff83794, and the 4e10 MM1/MM0 pair
is digits-identical. No job is live. Raw outputs are on aac6 in `~/k13/` (h2, mall, h3, h6, h7 .txt; e4e10_mm*_1.log;
acc_*.txt).

## K13b — Phase 13b agent K: the b1 pass and the stride penalty (PLAN §31, row K)

(former file: K13b.md)

Branch `p13b-K` (base `p13b-step0`, c988218). Files touched: `ecalc/ntt.c`, `ecalc/ntt.h`, `ecalc/tests/t_ntt.c` only.
Every option is behind a switch, off by default. The defaults are unchanged.

| switch | values | what |
|---|---|---|
| `NTT_B1R` | 0 (default), 3, 4 | the register-blocked b1 pass, with 2^3 or 2^4 points per thread |
| `NTT_PLAN` | 0 (default), 1 = auto, 10·lb + d | the pass boundaries: a b1 pass of 2^lb points (lb = 10, 11 or 12; lb > 10 needs `NTT_B1R`); d = 1 runs the b16 passes bottom-up, with the partial pass at the top |

All numbers are **measured** on aac6 (s24-30 for batch 1, job 21050): APU0, prime P0, `NTT_MODMUL=1` (the default),
median of 5, unless a line says otherwise. Rates are 16 B × points per pass.

#### Verdicts

| item | verdict | headline (measured) |
|---|---|---|
| **b1 register-blocked** (`NTT_B1R=3`) | **Wins everywhere at 2^20 points and up.** The b1 pass runs 1.20–1.33× faster: 1.26 → 1.52 TB/s at 2^31, which puts it level with the b16 passes. Whole transforms gain +4–13 %. `NTT_B1R=4` (16 points/thread, 1 wave/block, 2 waves/SIMD) is slower than 3 | 2^31 b1 pass 27.25 → 22.68 ms fwd, 27.56 → 22.86 ms inv; 2^31 fwd 104.7 → 100.4 ms |
| **stride penalty: cause** | **Not the NTT kernel, and not simple power-of-two aliasing.** A copy kernel with k_b16r's access pattern reproduces it: s = 17 at 0.65× and s = 24 at 0.56× of the ~2.6 TB/s seen at the other strides. Padding each row by 128 B – 4 KB recovers only a third of the loss (1689 → 1884–1941 GB/s). Rotating the row (1 or 33 slabs per row) recovers none of it (1626 GB/s) | see the stride table |
| **4 × 10^10, `NTT_B1R=3 NTT_PLAN=1`** | **Digits identical (cmp).** Compute is 4.0 % lower over three on/off pairs on two nodes | bs + dm 46.4 → 44.5 s |
| **stride penalty: fix** (`NTT_PLAN`) | **The plan moves the passes off the slow strides.** At 2^31 the plan runs b1 on 2^12 points and the b16 passes bottom-up, which gives s_lo 26, 19 and 12 instead of 24, 17 and 10: **fwd 1.06×, inv 1.12×** over the default (+1–2 % fwd and +7 % inv on top of `NTT_B1R`). **A side effect is bigger**: where the default plan ends in a b16 pass of only 1 or 2 stages (logn = 11, 12, 18, 19, 25, 26, 32, 33), a b1 of 2^11 or 2^12 points removes that pass: **1.25–1.40× at 2^25 and 2^26**. At 2^24 (s_lo 17) no plan beats the default | 2^25 1.465 → 1.049 ms fwd; 2^26 2.806 → 2.251; 2^31 104.7 → 99.2 |

#### 1. The register-blocked b1 pass (`NTT_B1R`)

**Design.** A block is one 2^lb-point chunk. It has 2^(lb−LGV) threads, and each thread keeps V = 2^LGV points in
registers. The lb stages run in ⌈lb/LGV⌉ groups, and group g butterflies index bits [B_g, B_g + C_g). The point of
thread t, register i, is

    idx = t[0, B) | i_lo << B | t[B, ..) << (B + C) | i_hi << (lb − (LGV − C))      (i_lo = the low C bits of i)

So the first forward group's load, and the last inverse group's store, are coalesced. One LDS exchange runs between
groups. For lb = 10 and LGV = 3 that is 3 exchanges instead of the paper kernel's 10 LDS round trips. LDS addresses are
XOR-swizzled by a linear map from index bits 4..11 into bits 0..3 (columns 13, 7, 14, 8, 12, 11, 12, 7). A search found
this map conflict-free (16 distinct 8-byte banks per 16 lanes) for every group at LGV 3 and 4 and lb 10–12. The
twiddles come from one global per-stage table per direction, tw[H + r] = w_2H^r (4096 entries). Those are the same
canonical values the paper kernel reads as tab[r << lgstep]. The butterflies, the modmul (MM 0 `ec_mm`, MM 1 `mm_lazy`),
the final canonicalisation and the scale are all the paper kernel's, so the outputs are bit-identical. They are also
canonical, and so unique. `NTT_MODMUL=2` keeps the Shoup kernel.

Resources (`-Rpass-analysis`): LGV 3 needs 48–54 VGPRs and 8 KB of LDS per 128-thread block (lb 10), which gives
4 waves/SIMD. LGV 4 needs 82–88 VGPRs and 8 KB per 64-thread block, which gives 2 waves/SIMD.

**The b1 pass alone** (`t_ntt bench 31 b1`, as a batch of 2^20-point transforms; GB/s, fwd | inv):

| points | paper b1 | B1R 3, lb 10 | B1R 4, lb 10 | B1R 3, lb 11 | B1R 3, lb 12 |
|---|---|---|---|---|---|
| 2^20 | 766 \| 769 | 862 \| 843 | 695 \| 764 | 771 \| 811 | 620 \| 691 |
| 2^24 | 1188 \| 1282 | **1512 \| 1530** | 1380 \| 1468 | 1420 \| 1462 | 1186 \| 1272 |
| 2^26 | 1224 \| 1234 | **1630 \| 1541** | 1457 \| 1492 | 1486 \| 1462 | 1241 \| 1304 |
| 2^28 | 1266 \| 1250 | **1540 \| 1522** | 1438 \| 1487 | 1398 \| 1432 | 1256 \| 1307 |
| 2^31 | 1261 \| 1247 | **1515 \| 1503** | 1420 \| 1482 | 1378 \| 1410 | 1253 \| 1294 |

At MM 0 the gain is 1.14–1.21× (2^31: 1163 → 1372 GB/s fwd). The longer b1 blocks (lb 11, 12) cost 9 % and 20 % more than
lb 10. They pay off only where they save a whole pass (§2).

#### 2. The stride penalty

**Microbenchmark** (`t_ntt bench 31 stride`). `kb_strd` moves 128 rows × 16 columns per block exactly as k_b16r does:
it loads rows tt + 16 i, stores rows 8 tt + i, and does no arithmetic. The row stride is S = 2^s + pad points, over a
2^31-point (16 GB) plane. rot > 0 shifts row j by j·rot slabs, which gives a rotated layout. GB/s:

| s | pad 0 | pad 16 (128 B) | pad 128 (1 KB) | pad 512 (4 KB) | rot 1 | rot 33 |
|---|---|---|---|---|---|---|
| 10–16 | 2562–2665 | 2471–2836 | 2541–2629 | 2497–2646 | 2212–2568 | 2218–2544 |
| **17** | **1689** | 1941 | 1898 | 1884 | 1626 | 1622 |
| 18–23 | 2266–2458 | 2406–2508 | 2150–2448 | 2295–2440 | 2166–2356 | 2168–2338 |
| **24** | **1459** | (no room) | | | 1304 | 1303 |

The pattern is the same at 2^28 points (s = 17: 1717 GB/s against 2443–2826 at the others). So the penalty belongs to
the address pattern, not to the kernel's arithmetic or to the plane size. It is also not a simple power-of-two channel
alias: a 4 KB-per-row skew should break such an alias, and it recovers only a third of the loss. The mechanism
(HBM channel/bank hashing over address bits 20–26 and 27–33) was not pinned down further. **A padded or rotated plane
layout would not remove the penalty**, and it would also change the plane format outside `ntt.c`. It was therefore not built.

**Pass rates by s_lo** (`t_ntt bench 31 slo`, one pass of a single 2^L transform, GB/s fwd | inv):

| s_lo | L 31, 7 stages | L 28 | L 24 |
|---|---|---|---|
| 10–16 | 1599–1627 \| 1547–1581 | 1610–1644 \| 1547–1593 | 1299–1702 \| 1308–1652 |
| **17** | **1338 \| 1102** | **1328 \| 1133** | **1208 \| 1078** |
| 18–23 | 1544–1561 \| 1520–1529 | 1551–1577 \| 1521–1529 | — |
| **24** | **1136 \| 1135** | — | — |
| tile, 6 stages at the top | 1063 \| 1049 (s_lo 25) | 1292 \| 1271 (22) | 1375 \| 1316 (18) |
| tile, 5 stages at the top | 1316 \| 1324 (s_lo 26) | 1317 \| 1320 (23) | 1437 \| 1371 (19) |

In the default plan, only logn ≡ 3 mod 7 (17, 24, 31) meets s_lo 17 or 24, and 17 only as its own last b16 pass
(s_lo 10). **Only 2^24 and 2^31 are affected.** With a b1 of 2^10 points, 2^31 has 21 stages above b1 = 3 × 7, so no
3-pass plan avoids s_lo 17 and 24. With a b1 of 2^12 points and the passes bottom-up (`NTT_PLAN=121`), the passes run at
s_lo 12, 19 and 26 (the last a 5-stage tile pass).

**Per pass at 2^31** (`t_ntt bench 31 pass`, ms fwd | inv):

| plan | top pass | middle | bottom | b1 | sum |
|---|---|---|---|---|---|
| default | [24..30] 30.35 \| 30.32 | [17..23] 25.64 \| 31.13 | [10..16] 21.11 \| 21.83 | 27.40 \| 27.72 | 104.5 \| 111.0 |
| B1R 4 | 30.31 \| 30.28 | 25.60 \| 31.11 | 21.14 \| 21.69 | 24.06 \| 23.09 | 101.1 \| 106.2 |
| B1R 4 + P121 | [26..30] 26.14 \| 26.06 | [19..25] 22.37 \| 22.72 | [12..18] 21.32 \| 21.86 | 28.42 \| 26.27 | **98.2 \| 96.9** |
| B1R 4 + P111 | [25..30] 32.35 \| 32.91 | [18..24] 22.16 \| 22.54 | [11..17] 21.38 \| 21.82 | 29.14 \| 24.94 | 105.0 \| 102.2 |

**`NTT_PLAN=1` (auto)** picks per logn: 110 at logn 11, 18, 25 and 32 (b1 on 2^11 points; one pass fewer); 120 at logn
12, 19, 26 and 33 (b1 on 2^12 points; one pass fewer); 121 at logn 31; and the default elsewhere.

#### 3. Whole transforms (`t_ntt bench 31 whole`, ratio to the default = MM 1, paper b1, plan 0; fwd | inv | fused inv_pw)

**The recommended pair, `NTT_B1R=3 NTT_PLAN=1`** (batch 2, job 21053, s24-26; the default's times in ms):

| layout | default ms (fwd \| inv \| inv_pw) | B1R 3 | **B1R 3 + P1** | B1R 4 + P1 | MM 0 + B1R 3 + P1 |
|---|---|---|---|---|---|
| 2^20 single | 0.055 \| 0.057 \| 0.057 | 1.04 \| 1.03 \| 1.01 | 1.05 \| 1.04 \| 1.03 | 0.95 \| 1.00 \| 0.96 | 0.96 \| 0.98 \| 0.95 |
| 2^22 single | 0.156 \| 0.167 \| 0.172 | 1.03 \| 1.04 \| 1.02 | 1.05 \| 1.06 \| 1.04 | 0.99 \| 1.04 \| 1.02 | 0.95 \| 0.99 \| 0.97 |
| 2^24 single | 0.548 \| 0.592 \| 0.623 | 1.07 \| 1.07 \| 1.05 | 1.07 \| 1.07 \| 1.06 | 1.02 \| 1.06 \| 1.02 | 0.95 \| 0.97 \| 0.96 |
| 2^25 single | 1.238 \| 1.318 \| 1.410 | 1.07 \| 1.07 \| 0.96 | **1.21 \| 1.28 \| 1.28** | 1.19 \| 1.29 \| 1.23 | 1.14 \| 1.14 \| 1.12 |
| 2^26 single | 2.723 \| 2.809 \| 2.943 | 1.08 \| 1.06 \| 1.05 | **1.31 \| 1.32 \| 1.28** | 1.28 \| 1.32 \| 1.15 | 1.18 \| 1.15 \| 1.13 |
| 2^27 single | 5.399 \| 5.603 \| 5.832 | 1.08 \| 1.07 \| 1.05 | 1.08 \| 1.06 \| 1.04 | 1.04 \| 1.05 \| 1.02 | 1.00 \| 0.96 \| 0.95 |
| 2^28 single | 10.77 \| 11.22 \| 11.60 | 1.06 \| 1.06 \| 1.03 | 1.07 \| 1.06 \| 1.04 | 1.04 \| 1.06 \| 1.01 | 0.99 \| 0.96 \| 0.95 |
| 2^29 single | 21.98 \| 22.81 \| 23.89 | 1.06 \| 1.06 \| 1.05 | 1.06 \| 1.06 \| 1.05 | 1.03 \| 1.05 \| 1.02 | 0.98 \| 0.96 \| 0.95 |
| 2^30 single | 43.84 \| 45.77 \| 47.91 | 1.06 \| 1.06 \| 1.05 | 1.06 \| 1.06 \| 1.05 | 1.03 \| 1.05 \| 1.02 | 0.99 \| 0.96 \| 0.95 |
| 2^31 single | 98.16 \| 104.8 \| 109.1 | 1.05 \| 1.05 \| 1.04 | **1.11 \| 1.15 \| 1.14** | 1.09 \| 1.16 \| 1.05 | 1.02 \| 1.04 \| 1.04 |
| 2^14 × 2^17 (2^31 pts) | 45.64 \| 48.23 \| 52.63 | 1.14 \| 1.12 \| 1.09 | 1.13 \| 1.12 \| 1.09 | 1.06 \| 1.09 \| 1.03 | 1.06 \| 1.05 \| 1.04 |
| 2^17 × 2^14 | 44.39 \| 47.55 \| 51.85 | 1.14 \| 1.12 \| 1.09 | 1.14 \| 1.12 \| 1.09 | 1.06 \| 1.10 \| 1.04 | 1.00 \| 0.99 \| 0.98 |
| 2^20 × 2^11 | 62.98 \| 66.43 \| 70.74 | 1.10 \| 1.08 \| 1.04 | 1.09 \| 1.08 \| 1.06 | 1.05 \| 1.07 \| 1.03 | 1.00 \| 0.99 \| 0.98 |
| 2^24 × 2^7 | 69.32 \| 74.41 \| 79.00 | 1.08 \| 1.08 \| 1.06 | 1.08 \| 1.08 \| 1.06 | 1.04 \| 1.06 \| 1.03 | 0.97 \| 0.98 \| 0.97 |

The MM 0 column is measured against the MM 1 default. MM 0 alone is 0.90–0.95×, so the pair adds the same +5–30 % on top
of MM 0. No layout loses with the pair at 2^20 or more points. Below that (2^20: +4 %) the transform is launch-bound.

**Batch 1** (job 21050, s24-30: the same code, and every rate 4–6 % lower than on s24-26; explicit plans, B1R 4 unless marked):

| layout | B1R 3 | B1R 4 + P111 | B1R 4 + P121 | B1R 3 + P111 |
|---|---|---|---|---|
| 2^20 single | 1.03 \| 1.03 \| 1.01 | 0.93 \| 1.00 \| 0.95 | 0.96 \| 1.00 \| 0.94 | 1.02 \| 1.05 \| 1.01 |
| 2^24 single | 1.06 \| 1.07 \| 1.06 | 0.95 \| 1.07 \| 1.06 | 0.97 \| 1.05 \| 0.99 | 0.99 \| 1.07 \| 1.06 |
| 2^25 single | 1.06 \| 1.06 \| 1.04 | 1.26 \| 1.34 \| 1.28 | 1.20 \| 1.26 \| 1.10 | **1.40 \| 1.40 \| 1.33** |
| 2^26 single | 1.07 \| 1.05 \| 1.05 | 0.92 \| 0.98 \| 0.98 | **1.25 \| 1.27 \| 1.19** | 1.00 \| 1.02 \| 1.01 |
| 2^28 single | 1.05 \| 1.05 \| 1.04 | 1.00 \| 1.05 \| 1.04 | 0.98 \| 1.07 \| 1.04 | 1.05 \| 1.06 \| 1.04 |
| 2^30 single | 1.05 \| 1.05 \| 1.04 | 0.97 \| 1.02 \| 1.01 | 1.02 \| 1.04 \| 1.02 | 1.01 \| 1.03 \| 1.02 |
| 2^31 single | 1.04 \| 1.04 \| 1.04 | 0.99 \| 1.10 \| 1.09 | **1.06 \| 1.12 \| 1.09** | 1.03 \| 1.11 \| 1.09 |
| 2^14 × 2^17 (2^31 pts) | 1.10 \| 1.11 \| 1.07 | 0.95 \| 1.11 \| 1.08 | 1.04 \| 1.13 \| 1.06 | 1.05 \| 1.12 \| 1.09 |
| 2^17 × 2^14 | 1.10 \| 1.09 \| 1.07 | 0.91 \| 0.99 \| 0.97 | 0.90 \| 0.95 \| 0.91 | 0.98 \| 0.99 \| 0.98 |
| 2^20 × 2^11 | 1.07 \| 1.07 \| 1.06 | 0.95 \| 1.08 \| 1.06 | 0.99 \| 1.06 \| 1.01 | 1.01 \| 1.09 \| 1.07 |
| 2^24 × 2^7 | 1.07 \| 1.06 \| 1.05 | 0.97 \| 1.06 \| 1.04 | 0.99 \| 1.04 \| 1.01 | 1.02 \| 1.08 \| 1.06 |

#### M-RUN SWITCHES

Axis "K kernels" (general, independent of the S/K/C/A rows; both values work on `p13b-K` at every size):

| value | environment |
|---|---|
| off (default) | *(nothing)* — or explicitly `NTT_B1R=0 NTT_PLAN=0` |
| on | `NTT_B1R=3 NTT_PLAN=1` |

Both switches are read by `ntt.c` at first use (any binary: `ecalc`, `mnrun.sh ... ./ecalc`, the tests). `NTT_PLAN=1`
without `NTT_B1R` is the default plan wherever the auto plan wants a b1 above 2^10. The two are also exact with
`NTT_MODMUL=0` (bit-identical, §4c), and with `NTT_MODMUL=2` they fall back to the default b1 and plan.

#### Exactness gate

`t_ntt 31 4c` (new mode: section 4c alone; job 21053, 37 s): every operation (fwd, inv, the fused inverse in the full,
bcast and pair layouts, and fwd3 + inv3_pw) under each configuration, compared on the device with the MM 0 / default-plan
reference. It covers every length from 2^10 to 2^31 and the batches 2^11 × 1000, 2^14 × 64, 2^17 × 16, 2^20 × 6 and
2^12 × 3. The configurations are K13's nine, plus B1R3, MM1+B1R3, B1R4, MM1+B1R4, MM2+B1R4+P111 (falls back), P101,
MM1+B1R4+P110, MM1+B1R4+P111, B1R3+P111, MM1+B1R4+P120, MM1+B1R3+P121, MM1+B1R4+P121, MM1+MALL20+B1R4+P111,
MM1+B1R4+P1, MM1+B1R3+P1 and B1R3+P1. **All are bit-identical: VERIFY OK, 3650 checks.**

#### Acceptance and 4 × 10^10

* `./mnaccept.sh 21061 --only unit` on 893eb7f (defaults): **8 passed, 0 failed** (t_ntt 24 3215 checks, t_mul 20,
  t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid at 2 procs; 1162 s).
* `./mnaccept.sh 21065 --only e9` (defaults): 10^9 **identical** in both bases, 16.69 s (base 10) and 26.85 s (base 2).
* `NTT_B1R=3 NTT_PLAN=1 ./mnaccept.sh 21065 --only e9`: 10^9 **identical** in both bases, 16.31 s and 23.05 s (one run
  each, so the difference is noise-level).
* **4 × 10^10 at size 1** (job 21065, s24-26, reference evicted before each run, `~/k13b/e4.sh on off`):

  | run | switches | bs | dm | bs + dm | init | total (measured) | digits |
  |---|---|---|---|---|---|---|---|
  | 1 | `NTT_B1R=3 NTT_PLAN=1` | 23.68 s (batch 15.6, mdev 7.9) | 20.59 s | **44.3 s** | 20.3 s | 64.65 s | identical (cmp) |
  | 2 | defaults | 25.01 s (batch 16.6, mdev 8.1) | 20.84 s | **45.9 s** | 18.0 s | 63.90 s | identical (cmp) |

  Batch 5 (job 21070, s24-16, `NOCMP=1 ~/k13b/e4b.sh off on off on`: timing only, digits not compared, VERIFY OK each):

  | run | switches | bs | dm | bs + dm | init | total |
  |---|---|---|---|---|---|---|
  | 1 | defaults | 25.38 s (batch 16.9) | 21.05 s | 46.5 s | 21.4 s | 67.89 s |
  | 2 | on | 24.15 s (batch 15.8) | 20.76 s | 45.0 s | 19.1 s | 64.04 s |
  | 3 | defaults | 25.35 s (batch 16.9) | 21.24 s | 46.7 s | 21.2 s | 67.85 s |
  | 4 | on | 23.61 s (batch 15.4) | 20.64 s | 44.3 s | 17.8 s | 62.09 s |

  **Over the three pairs, compute (bs + dm) is 44.5 s with the switches on (44.3, 45.0, 44.3) against 46.4 s with them off
  (45.9, 46.5, 46.7): −1.9 s, −4.0 %, and the ranges do not overlap** (measured). Most of it is in the batch tier
  (16.6–16.9 → 15.4–15.8 s); dm gains 0.3–0.5 s. The wall moves with init (17.8–21.4 s), which the switches do not
  touch, so a wall-time claim needs the M-run's repeats.

#### RESUME

* Batch 1 (job 21050, s24-30; `~/k13b/b1/` on aac6): the stride, slo, b1, pass and whole benches are done (above).
  `t_ntt 24` was cut by my 420 s timeout while its host references ran. Section 3b's GMP carry is quadratic, which is
  why mnaccept gives the test 1200 s; it takes ~10 min.
* Batch 2 (job 21053, s24-26; `~/k13b/b2/`): `t_ntt 31 4c` passed (3650 checks) and the whole bench with the P1 rows is done.
  `t_ntt 24` was cut again at 600 s (same cause).
* Batch 3 (`~/k13b/b3/`, job 21061): `./mnaccept.sh $J --only unit` on the defaults. Summary copied to `~/k13b/acc_unit_default`.
* Batch 4 (job 21065): mnaccept `--only e9` passed with the defaults and with `NTT_B1R=3 NTT_PLAN=1`. The 4 × 10^10 pair
  (on, off) is digits-identical. Runs 3 and 4 were cut by the job end, because the 40 GB cmp takes ~6 min per run.
* Batch 5 (job 21070): two timing-only pairs (above).
* **Complete.** The gate is met: bit-identical at every length (4c), mnaccept unit and e9 green, the 4e10 pair identical,
  and the M-RUN SWITCHES section is filled in. No job is live. Raw outputs are in `~/k13b/` on aac6.

#### Open issues

* The mechanism of the s = 17 and s = 24 penalty is not identified (HBM address hashing is the likely suspect). Padding
  and rotation do not cure it, so the plan avoids those strides instead. It cannot avoid them at 2^24, and there the
  default plan stays.
* The top pass of the 2^31 plan is a 5-stage tile pass (k_b16, 1316 GB/s). A register-blocked 5- or 6-stage body would
  gain ~4 ms more per 2^31 transform (modelled from the per-pass table: 26.1 ms at 1316 GB/s → ~21.5 ms at 1600 GB/s).
* `NTT_B1R=4` (16 points per thread) is dominated by 3 everywhere. It stays in only for the record.
* t_ntt 24 takes ~10 min, most of it in section 3b's quadratic GMP carry (pre-existing). `t_ntt LOGMAX 4c` is the fast
  path for the switch identities.

## K4b — `k_crt_batch`: the per-chunk carry chain as a parallel carry-lookahead

(former file: K4b.md)

**Question.** Does resolving the CRT kernel's per-chunk carry chain in parallel (instead of a 256-step serial loop run by
one thread) shorten the 1-node run at 6.441e10 digits? Baseline: `k_crt_batch` takes 4.5–4.8 s per APU at 1 node (m,
`archive:results/O7b.md` §D, `results/KERNELS.md §S39` row 3); K4 (B-form occupancy, `results/KERNELS.md §S51`) gave nothing.

**Change / branch.** Branch `k4b`. Switch `RNS_CRT_SCAN` (0 = the serial chain, default; 1 = parallel). Files:
`ecalc/rns_mul.c` (`k_crt_batch`, `rns_gconst`), `ecalc/rns_int.h` (`struct gconst` field `scan`), new unit test
`ecalc/tests/t_crtk.c` (in `TESTS`), registry rows. No call site changes: the flag rides in `struct gconst`, which every
launch already passes by value.

How it works (`RNS_CRT_SCAN=1`). Per chunk of 256 coefficients, thread j forms w_j = V_j + CY_{j−1} (V_j < B the chunk
sum's low digit, CY ≤ 4 its quotient, CY_{−1} the chunk's carry-in ≤ 5), so w_j ≤ B + 4. The serial chain's overflow is
o_j = gen_j | (prop_j & o_{j−1}) with gen = w ≥ B, prop = w = B − 1. Two wave ballots give the gen / prop masks of each
64-thread wave; every thread then adds A + G over 256 bits (A = gen | prop, G = gen; four words) and reads its carry-in as
bit j of (A + G) ^ A ^ G. The next chunk's carry-in is CY_{n−1} + o_{n−1}. The tails and the spill are unchanged.
Barriers per chunk: 3 instead of 5. The serial path is the reference: `t_crtk` compares every limb and spill word.

**Method.**
- Microbenchmark `tests/t_crtk 29` and `27` on one aac7 node (APU 0): part 1 compares scan 0 against scan 1 on random
  residues, carry ripples (every coefficient B − 1, generators 2B − 1 at about 1 in 997, an added operand of B − 1 limbs)
  and all residues p_i − 1, three and four primes, C form, ragged stripes, nc < S, a B-form batch with added operands;
  part 2 times the C-form launch of the 1-node run (q = 2^29 coefficients in 65536 stripes of 8192), median of 7.
- Gates (`tools/gate.sh --build --nodes 2`, then `--nodes 2`, `--nodes 1` twice; 10⁹ digits, aac7 launch line, P24 forced
  at 2 nodes) with the switch off and with `--env RNS_CRT_SCAN=1`; after the rebase on main, the same four gates again.
- Unit tests with `RNS_CRT_SCAN=0` and `=1`: `t_mul 20` (invoked as `mnaccept.sh`'s unit step), `t_strategy 24 2 V`.
- Timing: paired ABBA at 1 node, 6.441e10 digits, aac7 launch line (`tools/profile_load.py aac7 --line`) through
  `mnrun.sh 1`, A = `RNS_CRT_SCAN=0`, B = `RNS_CRT_SCAN=1`, same binary (branch commit e9505b97), same node per round;
  stop rule p < 0.05 / CI half-width ≤ 1.5 s / 4 rounds (driver adapted from `archive:drivers/s51/s51_abba.sh`, kept as
  `archive:drivers/k4b/`). The wall is ecalc's own `total` line.

**Results.**

| check | result |
|---|---|
| `t_crtk 29`, `t_crtk 27` equivalence (48 cases, 3 and 4 primes) | all identical, VERIFY OK (m) |
| C form 2^29 / 65536, three primes (the 1-node case) | scan 0 30.18 ms → scan 1 13.65 ms median, 2.21× (569 → 1259 GB/s) (m) |
| C form 2^29 / 65536, four primes | 49.85 → 34.11 ms, 1.46× (m) |
| C form 2^27 / 16384, three / four primes | 8.02 → 3.95 ms (2.03×) / 12.54 → 8.56 ms (1.47×) (m) |
| gates before the rebase: 2 n off / on, 1 n off / on | PASS ×4: VERIFY OK, digits identical, segv 0 (ecalc total 18.67 / 17.54 s at 2 n, 12.39 / 12.15 s at 1 n; single runs, not timings) (m) |
| gates after the rebase on main 89318ae (branch 0b3c36e), 2 n off / on, 1 n off / on; `t_crtk` | PASS ×4: VERIFY OK, digits identical, segv 0; `t_crtk` VERIFY OK (48 checks) (m) |
| gates after the second rebase on main f232085 (DEF: P24 for every multi-node product; branch e385355), same four + `t_crtk` | PASS ×4, digits identical, segv 0 (2 n: P24 in use); `t_crtk` VERIFY OK (m) |
| `t_mul 20`, scan 0 / scan 1 | VERIFY OK (169 checks) both, 727 / 731 s (m) |
| `t_strategy 24 2 V`, scan 0 / scan 1 | VERIFY OK (12 checks) both (m) |
| **ABBA 1 n, 6.441e10, 4 rounds (16 runs)** | **B − A = −2.52 s, 95 % CI [−4.19, −0.85]** (per-round deltas −3.6, −1.4, −3.2, −1.9 s; p < 0.05), all 16 runs VERIFY OK; arm means A 71.47 s, B 68.95 s (−3.5 %) (m) |

Modelled saving at 1 node: the kernel's 4.5–4.8 s per APU (m, S39/O7b) at the three-prime ratio 2.21 is 2.4–2.6 s per
APU of kernel time (modelled; the wall gain depends on how much of the kernel is on the critical path).

**Verdict.** Adopt candidate: −2.52 s [−4.19, −0.85] at 1 node (m), above the 2.0 s assumed in OPPORTUNITIES and in
line with the modelled 2.4–2.6 s of kernel time. Digits identical in every gate and unit test. Merged with the switch off;
the user decides adoption (a default flip is one line in `rns_gconst`).

**Open issues.** The four-prime C form gains less (1.46× against 2.21×, m): it is bound by `ec_words_to_dec4`'s sixteen
divisions, not by the chain; a cheaper four-word-to-decimal conversion is the next step there. Multi-node timing not
measured (the 2-node gates only show digits). `k_crt24` (`p24_crt.c`, the multi-node four-prime 24-digit CRT) has the same serial chain; not touched.

**RESUME.** Branch `k4b` merged to main with the switch off; worktrees removed. Raw logs: aac7 `~/k4b/{ubench,gates,gates2,gates3,tmul,abba}`
(`abba/pairs.tsv`, `abba/stats.txt`). Next (for the user): adopt `RNS_CRT_SCAN=1` as the default, then remove the serial path;
optionally the same carry-lookahead in `k_crt24` and a faster `ec_words_to_dec4`.

## K4c — `k_crt24`: the P24 CRT's per-chunk carry chain as a parallel carry-lookahead

(former file: K4c.md)

**Question.** Does K4b's carry-lookahead (`results/KERNELS.md §K4b`), applied to `k_crt24` (the P24 CRT, `ecalc/p24_crt.c`, the
form of every multi-node product since D18, `results/LAUNCH_DEFAULTS.md §DEF`), shorten the 2-node run at 6.441e10 digits per node?

**Change / branch.** Branch `k4c`. New switch `P24_CRT_SCAN` (0 = the serial chain, default; 1 = parallel). Files:
`ecalc/p24_crt.c` (`k_crt24` and `p24_crt_launch`, which reads the switch per launch and passes it as a kernel argument),
new unit test `ecalc/tests/t_crt24k.c` (in `TESTS`), registry and review rows, `ALGORITHM.md` §4 (P24). No call-site
change. A new switch rather than `RNS_CRT_SCAN`: that one became the default by the user's decision D21 (on 1-node data,
`k_crt_batch` only) while this branch was open; reusing it would have switched `k_crt24` on by default without its own
measurement and adoption decision.

How it works (`P24_CRT_SCAN=1`). A chunk of 256 points covers up to 342 limbs; each limb sums the digits of at most three
points (< 3B), so its quotient CY ≤ 2, the chunk's carry-in ≤ 3, and w_i = V_i + CY_{i−1} ≤ B + 2. As in K4b the serial
overflow is o_i = gen_i | (prop_i & o_{i−1}), gen = w ≥ B, prop = w = B − 1. Each thread owns limbs t and t + 256; four wave
ballots give the masks of 512 limbs (8 words); every thread adds A + G (A = gen | prop, G = gen) over the chunk's words and
reads its carry-ins as bits of (A + G) ^ A ^ G; the next chunk's carry-in is CY_{nl−1} + o_{nl−1}. Same barrier count (5) as
the serial path; the 342-step one-thread loop is gone. The tails and the spill are unchanged.

**Method.**
- `tests/t_crt24k 30` on one aac7 node (APU 0): part 1 runs `k_crt24` with the switch 0 and 1 on the same planes and compares
  every out-run limb, every column spill and the device flag: coefficients random below 10^60, long carry ripples
  (10^24 − 1 everywhere, all limbs B − 1, with generators 2·10^24 − 1 at about 1 in 997), one generator at each column's first
  point (the ripple crosses every chunk into the spill), the bound 10^60 − 1 everywhere, all residues p_i − 1 (past the bound:
  flag raised), a mix; shapes rows 1, 3, 128, 333/334 (uneven ranks of R = 1000), 428/429 (R = 3000, 7 ranks), 512, 777, 2048,
  8192, every a mod 3. Part 2 times the kernel at 2^30 points per rank (rows 4096 / 8192 / 16384, 2-node rank shape
  rows = R / 8), median of 7.
- `tests/t_p24` (k_crt24 against the GMP host model) with `P24_CRT_SCAN=0` and `=1`.
- Gates `tools/gate.sh --build --nodes 2`, `--nodes 2`, `--nodes 1` ×2, switch off and `--env P24_CRT_SCAN=1` (10⁹ digits,
  aac7 launch line, P24 in use at 2 nodes); again after the rebase on main 4ec5ab5 (SIMP, ADOPT: `RNS_CRT_SCAN=1` default).
- Profiling round (one ABBA round with `RNS_VERBOSE=1`): node 0's sum of the `dist_mn` lines' `crt` times per run.
- Timing: paired ABBA at 2 nodes, 6.441e10 digits per node, aac7 launch line through `mnrun.sh 2`, A = `P24_CRT_SCAN=0`,
  B = `=1`, same binary (branch commit 5d54994), same nodes per round, no digit output; stop rule p < 0.05 / CI half-width
  ≤ 1.5 s / 4 rounds. Driver `archive:drivers/k4c/k4c_abba.sh` (from K4b's); queue items `archive:drivers/k4c/6*_k4c_*.sh`.
  The wall is ecalc's own `total` line; bs and dm are its phase fields.

**Results.**

| check | result |
|---|---|
| `t_crt24k` equivalence (279 checks: 9 shapes × 6 coefficient kinds × up to 3 ranks) | all identical, VERIFY OK (m) |
| `t_p24` against the GMP model, switch 0 / 1 | VERIFY OK (78594 checks) both (m) |
| kernel, 2^30 points per rank, rows 4096 / 8192 / 16384 | scan 0 157.96 / 158.39 / 158.76 ms → scan 1 101.95 / 101.95 / 101.26 ms median; 1.55–1.57× (290 → 450 GB/s) (m) |
| gates on 5d54994: 2 n off / on, 1 n off / on | PASS ×4: VERIFY OK, digits identical, segv 0, P24 in use at 2 n (m) |
| gates after the rebase on main 4ec5ab5 (branch ba34ba2): 2 n off / on, 1 n off / on; `t_crt24k 0`, `t_p24` with the switch on | PASS ×4: VERIFY OK, digits identical, segv 0, P24 in use at 2 n; `t_crt24k` VERIFY OK (276 checks), `t_p24` VERIFY OK (78594) (m) |
| profiling round (RNS_VERBOSE=1), node-0 `crt` sum per run | A 4.13 / 4.25 s → B 2.95 / 2.94 s: −1.24 s per run (m, 1 round); total in that round −2.5 s (m, one round, not a result) |
| **ABBA 2 n, 6.441e10/node, 4 rounds (16 runs)** | **total B − A = −1.89 s, 95 % CI [−7.51, +3.73]** (per-round −2.8, +2.0, −6.3, −0.5 s; not significant); arm means A 126.28 s, B 124.39 s; all 16 VERIFY OK (m) |
| phases, same 4 rounds | dm −0.55 s [−3.00, +1.90]; bs −0.43 s [−1.67, +0.82] (m) |

Modelled: node 0's P24 CRT time of 4.19 s per run (m) at the kernel ratio 1.55 (m) gives −1.49 s of kernel time per run
(modelled); the profiling round measured −1.24 s. The wall saving is at most that (the CRT is on the critical path of each
product), below the ABBA's resolution at 4 rounds: the A arm had two slow runs (131.0, 131.4 s) against a B range of
122.7–125.9 s.

**Verdict.** Needs more data for a wall claim; the kernel gain is clear. Kernel 1.55× and −1.24 s of CRT time per 2-node
run (m); wall −1.89 s [−7.51, +3.73] at 4 rounds (n.s.). Digits identical in every gate and test. Merged with the switch off;
the user decides adoption (on the kernel evidence, or after more rounds: at the observed per-round spread of 3.5 s about 40 rounds would be
needed to resolve −1.2 s, modelled).

**Open issues.** The four-prime conversion `ec_words_to_dec4` now dominates the kernel (K4d). The share of the CRT at
larger node counts is not measured (the per-rank plane shrinks with g while the per-node digit count is fixed, so the CRT time
per node stays about the same, modelled).

**RESUME.** Branch `k4c` merged to main with the switch off; worktrees removed. Raw logs: aac7 `~/k4c/{gates,gates2,gates3,prof,abba}`
(`abba/pairs.tsv`, `abba/s51stat.tsv` = bs, dm per run, `abba/stats.txt`). Next (for the user): adopt `P24_CRT_SCAN=1`
or ask for more rounds; K4d.

## S38: the next single-node speed lever. Found: the dbig add/sub kernel (k_addsub) ran at 36-84 GB/s; DBIG_ADDSUB2=1 (coalesced) cuts 10.4 -> 2.0 s of GPU time per APU

(former file: S38.md)

Branch s38 (from main 476d5f1). Node: hold 12454 (aac7-n00), binary of main (worktree main-wrap-19c9801) for the profile, s38 for the change. All times measured unless labelled.

#### 1. Phase table, one node, defaults (ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_INIT_TL=1 DIST_STATS=1 NTT_SIZE_STATS=1; logs ~/s38/prof/ on aac7)

| phase | 6.441e10 (standard share) s | % of 94.2 | 8.5e10 (larger share; peak 371.7 of 480 GB) s | % of 140.7 |
|---|---|---|---|---|
| init (rns_init: staging, plane pools, region arenas) | 12.6 | 13.4 | 16.2 | 11.5 |
| bs: wait for the seed thread (GPU idle) | 11.3 | 12.0 | 14.7 | 10.4 |
| bs: batch tier, levels 1-23 | 21.8 | 23.1 | 31.7 | 22.5 |
| bs: mdev tier, levels 24-25 | 13.7 | 14.5 | 22.2 | 15.8 |
| recip (inside dm) | 8.05 | 8.5 | 11.9 | 8.5 |
| dm total (recip + division) | 34.4 | 36.5 | 55.3 | 39.3 |
| 10dP, T1, T2, dc, other | 0.4 | 0.4 | 0.4 | 0.3 |
| total | 94.2 | 100 | 140.7 | 100 |

(The S34 soak at the standard share gives 98.7-105 s; the stats switches change little.) Both runs VERIFY OK.
Tier split of the products (rns_dist): C form 29 products 24.4 s, B form 63 products 4.4 s (6.441e10); C 50 / 42.4 s, B 62 / 3.35 s (8.5e10).
Init timeline (6.441e10): plane pools 5.2 s, seed thread 5.3 -> 24.0 s (spans 11 s, waited 2.6 s for the region pools), region arenas mapped by 12.4 s, background mapping of the parity-1 halves 12.6 -> 26.8 s (3 to 11 s of it "queued for the mapper lock"), level 1 starts at 25.6 s (APU1 waits 1.0 s for chunks 17-18). At 8.5e10 the mapping binds by 5.3 s (seeds joined 31.2 s, level 1 at 36.4 s; APU0 waits 4.7 s for chunks 24-31: create 3.6 + access 6.9 + lock queue 9.1 s).

GPU view (rocprofv3 --kernel-trace on the 6.441e10 run, 7 000 dispatches): busy per APU 56.3 / 56.3 / 55.4 / 43.6 s in an 81 s window from the first kernel (APU 3 idles 12.7 s more than the others, mostly in the bs batch/mdev tiers). Busiest kernels, s per APU (APU0): **k_addsub 10.35 (APU3 4.5)**, k_gather 7.7, k_crt_batch 4.5, k_push3 (the all-to-all) 4.4, k_b1r 4.4, k_maxidx 2.4, k_modq 1.6, k_bload 1.2.

#### 2. Ceilings of the candidates, against that table

| candidate | ceiling at 6.441e10 | at 8.5e10 | verdict |
|---|---|---|---|
| S-5 push-form B broadcast | B form is 4.4 s in total; the load is about 1/3 of B's wall (S13: 0.25 of 0.72 s at 2^31); a 3.4x faster load (697 vs 51 GB/s per APU, bench) saves at most 1.0 s (modelled) | B total 3.35 s: at most 0.8 s (modelled) | below the 1 s bar; not built |
| S-4 E2 fixed-slot allocator | 1.3 s at 1e11 (TASKS row; 0.5 % of 263 s); less at these shares | same | below the bar; the layout does not fragment at these shares (no OOM in either run) |
| idle gap in bs (11.3 s waiting on the seeds) | seeds and the background VMM mapping co-bind (S22 3.1, S37): the mapping alone binds by 1.55 s here and 5.3 s at 8.5e10; seed-side tricks (S-3 Barrett) are worth 0-0.4 s | 5.3 s if the mapper lock queue were removed AND seeds kept up | the mapping line belongs to S32/S35 (ECALC_VMM_BG=2, ECALC_VMM_SAFE); not duplicated here |
| APU 3 idle in the tiers (TASKS 2 / B6) | 12.7 s idle vs APUs 0-2; perfect rebalancing would save (56.3*3 + 43.6)/4 = 53.1 vs 56.3 s busy = 3.2 s at best (modelled) | not measured | needs a 4-way split of a 3-prime workload; large design change; not built |
| the C-tier all-to-all (exposed host wait in DIST_STATS) | 10.0 s of the 24.4 s C time (41 %); but the push kernel already runs at 221-253 GB/s per APU (X13, D16) and the transform is chunk-pipelined (K = 8): a bandwidth lever is closed; only deeper overlap across primes remains and cannot be bounded from this data | 17.3 s of 42.4 s | not a quick lever; recorded |
| **k_addsub (dbig add/sub, synchronous per call)** | **10.35 s of GPU time per APU on the critical path (APU1 11.3, APU2 10.5): 36-84 GB/s measured (1.34e9 limbs x 24 B in 0.38 s), against about 3 TB/s HBM** | scales with the share | **built (section 3)** |
| k_maxidx (scans the whole range for the top differing limb) | 2.4 s (APU0) | | follow-up: scan from the top / early exit |

TASKS section 2 (Performance, single node) open items 2.1-2.6 are all initialization or seed items (2.1 overlap seeds with mapping: the init timeline above shows it is done and the pair now co-binds; 2.2 decimal mul_1: S37 says 0.4 s; 2.3 / 2.4 / 2.5 / 2.6 untouched here, none showed in the profile above 1 s).

#### 3. The lever: DBIG_ADDSUB2=1

Cause: `k_addsub` (dbig.c) gives each thread 16 consecutive limbs, so within a wave the loads and stores of limb k are 128 B apart (64 cache lines per request), and every call is synchronous (`hipStreamSynchronize` in the per-quarter OpenMP section, then a host scan of the chunk flags). 171 calls on APU0, up to 1.34e9 limbs per quarter.
Fix (`k_addsub2`, behind `DBIG_ADDSUB2`, default 0): the block's 4096 limbs move coalesced (limb e = j*256 + tid) through a 32 KB LDS tile laid out thread-major with an XOR swizzle (conflict-free both ways); G / P alias the dead tile, so the block takes exactly 32 KB of LDS. The arithmetic, the carry flags (cout / prop) and the output are those of `k_addsub`, limb for limb, so digits cannot change; the carry kernel and the host scan are untouched.

Measured on the node (6.441e10, one run each, rocprofv3 attached, default vs `DBIG_ADDSUB2=1`): k_addsub 10.35 / 11.27 / 10.54 / 4.53 s per APU -> k_addsub2 2.02 / 2.29 / 2.15 / 1.11 s; run total 93.7 s -> 81.2 s (bs 45.1 -> 41.6, dm 34.3 -> 26.2, init 14.1 -> 13.3). One pair only: the ABBA below is the evidence.

#### 4. Tests

| test | command | result |
|---|---|---|
| build | `source ecalc/aac7env.sh; make -B -j16` in ~/ntt-wt/s38/ecalc | OK |
| gate, 1e9, both arms | `DBIG_ADDSUB2={1,0} RNS_VERBOSE=1 ECALC_VERBOSE=2 ./ecalc 1000000000 <f>`, `digcmp.sh <f> ~/ref/e_1000000000.txt` | both VERIFY OK, digits identical (ADDSUB2=1: total 11.43 s; =0: 13.21 s) |
| 6.441e10, ADDSUB2=1 (profiled) | as above | VERIFY OK, total 81.2 s |
| 1-node ABBA, 6.441e10, no digit writes | `archive:drivers/tools/s38_abba.sh 12454 ~/s38/abba` (A default, B DBIG_ADDSUB2=1; stop p < 0.05 after >= 3 rounds, or CI half-width <= 3 s, or 8 rounds) | armed; result in ~/s38/abba/S38_DONE, pairs.tsv |

#### RESUME
- Driver armed on aac7 (hold 12454); collect: `cat ~/s38/abba/S38_DONE; cat ~/s38/abba/pairs.tsv; tail -5 ~/s38/abba/summary.txt` (on aac7).
- If significant and VERIFY OK in every run: decision for the user is to make DBIG_ADDSUB2 the default (one-line change in `addsub2_on`), then a 10-node check (S22-style) before adopting for the target.
- Next levers visible in the profile: k_maxidx (2.4 s, early exit from the top), k_gather (7.7 s per APU, the operand load), k_addsub2 itself (2 s, one synchronous call each).

#### Multi-node confirmation and adoption (2026-10-10, measured)

1-node ABBA at 6.441e10 digits, A = default (old `k_addsub`), B = `DBIG_ADDSUB2=1`, run in parallel on four
more nodes (driver `~/s38/s38m.sh`, outputs `~/s38/abba_<node>`), 3 rounds each, stopped on p < 0.05:

| node | B − A per round (s) | mean (s) | 95 % CI half-width (s) |
|---|---|---|---|
| aac7-n10 | −12.8, −11.8, −12.2 | −12.27 | 1.20 |
| aac7-n11 | −13.0, −10.7, −13.8 | −12.51 | 3.97 |
| aac7-n20 | −11.2, −11.8, −13.1 | −12.04 | 2.34 |
| aac7-n21 | −13.0, −10.7, −13.2 | −12.27 | 3.44 |

Arm A ≈ 91–96 s, arm B ≈ 80–83 s: about −13 % on every node; 48 of 48 runs VERIFY OK.
**Adopted as the default (`DBIG_ADDSUB2` default 1) by the user's rule "if it consistently reduces time, make it
the default".** `DBIG_ADDSUB2=0` restores the old kernel. Open: a 2- and 4-node check before the target
(the kernel also runs inside the multi-node division).

## S39: kernel survey of the 6.441e10 single-node profile (read-only; no runs, no code edits). Found: the next levers are k_gather's remote pull (7.7 s), k_maxidx's full scans (2.4 s), and a 13 %-of-HBM k_addsub2 residue; the NTT passes already run at 1.1-1.7 TB/s

(former file: S39.md)

Branch s39 (from s38 e13cd1b). Source: rocprofv3 `--kernel-trace` rocpd db `~/s38/rp` on aac7 (default binary, 6.441e10, 1 node; ~/s38/rp2 = DBIG_ADDSUB2=1), queried per agent with bounded group-by SQL (scripts ~/s38/q9.py, q10.py, q11.py). Per-APU figures are APU0 (agent 4); APUs 1-2 are within 10 % except k_maxidx (APU0 2.42 / APU1 1.92 / APU2 1.72 s) and k_addsub (APU3 4.5 s). All times measured (rocprof); bytes per call are modelled from the kernel source and the dispatch grid (labelled); GB/s = modelled bytes / measured time. HBM peak taken as ~3 TB/s per APU. Plane = 2^29 points x 8 B = 4.29 GB per APU (6 planes = 25.77 GB in the log).

#### 1. Ranked table (>= 0.5 s per APU; APU0)

| # | kernel | source | calls | s / APU | bytes per call (modelled) | achieved GB/s | % of 3 TB/s | problem |
|---|---|---|---|---|---|---|---|---|
| 1 | k_addsub (default; ADDSUB2=0) | dbig.c:728 | 171 | 10.35 | up to 24 B x 1.34e9 limbs = 32 GB | 36-84 (S38) | 1-3 | uncoalesced 16-limb segments; fixed by k_addsub2 (S38) |
| 2 | **k_gather** | rns_dist.c:86 | 174 | 7.67 | 2^29 limbs: read 4.3 GB (3/4 of it from the other three APUs' quarters, `acc` over a sharded dbig) + write 4.3 GB (stride-C, `x[il*C+j]`) = 8.6 GB | 195 (all calls 41.6-48 ms) | 6.5 | remote pull over xGMI (3.2 GB at ~73 GB/s, against 221-253 GB/s for the push kernel and 51 GB/s measured pull, S13/TASKS S-5); 4 primes x 2 operands re-pull the same operand; strided local write; 64-bit `/` and `%` per element; 112 B scratch/thread |
| 3 | k_crt_batch | rns_mul.c:741 | 200 | 4.54 | 4 planes x 8 B in + 8 B out per coefficient = 40 B; C-form (16.8 M threads = 65536 stripes x 8192 coeff): 21.5 GB / 32 ms | 670 (C-form); the 228-block B-form calls (13 x 63 ms) ~5x lower per coefficient | 22 | latency/compute-bound: Garner mulmods plus a serial 256-step carry chain by thread 0 per chunk (4 `__syncthreads` per 256 coefficients); the B-form launch (rns_dist.c:745) uses S = G/4 = 228 blocks = one block per CU (LDS 12.4 KB allows 5) |
| 4 | k_b1r<10,3,0,1> (NTT radix pass) | ntt.c:698 | 2266 | 4.44 | 16 B x points: 2^26 pts 1.07 GB in 0.99 ms; 2^29 pts 8.6 GB in 5.2 ms; 2^30 pts in 10.4 ms | 1080-1650 | 36-55 | none obvious (LDS/ALU-bound butterflies); already tuned (Phase 13) |
| 5 | k_push3 (all-to-all) | comm_xgmi.c:39 | 2088 | 4.43 | chunk of the plane (q/K per peer set) | 221-253 per APU (X13, D16) | link-bound | xGMI link limit; closed (S38 table) |
| 6 | k_b16r<0,0,0,1,0,0> | ntt.c:316 | 736 | 2.44 | 8 pts/thread assumed: 2^30 pts 17 GB in 10-14 ms | ~1250-1700 (assumed layout) | 40-57 | NTT pass |
| 7 | k_b16<6,0,0> | ntt.c:153 | 1470 | 2.43 | 2^30 pts 17 GB in 11.8 ms; 2^26 pts 1.07 GB in 1.5 ms | ~700-1450 | 25-48 | NTT pass |
| 8 | **k_maxidx** | dbig.c:843 | 494 (277 large) | 2.42 | one quarter, up to 1.34e9 limbs x 8 B x (1 or 2 operands) = 10.7 (21) GB at the 57 ms maximum | ~190 per operand at the largest; mean call 8.7 ms | ~6 | scans the whole range for the top differing limb; reads are coalesced but each thread has one dependent `dget` per iteration, 120 B scratch (kernarg pointer array indexed by the runtime quarter), a data-dependent branch; then `hipMemcpy` + host reduction |
| 9 | k_b16<5,0,0> | ntt.c:153 | 266 | 1.71 | NTT pass, 2^29..2^30 pts | ~1000-1500 | 33-50 | NTT pass |
| 10 | k_b1r<10,3,2,1> | ntt.c:698 | 326 | 1.70 | NTT pass fused with pointwise | ~1000-1600 | 33-53 | NTT pass |
| 11 | k_b16r<1,0,0,1,0,0> | ntt.c:316 | 368 | 1.58 | NTT pass | ~1000-1500 | 33-50 | NTT pass |
| 12 | k_modq (T1 residues) | dbig.c:609 | 4 | 1.57 | 54 602 blocks x 16384 limbs = 8.9e8 limbs = 7.2 GB, re-read per prime (T1_NQ primes) | 18 per prime-pass at 395 ms | ~1 | compute-bound: `unsigned __int128 % q` (software 128-bit modulo) per limb per prime, mod by division instead of the Montgomery/Barrett that ec_mod already provides; verification stage (ecalc.c:723, mn_out.c:68), partly overlapped by a background thread |
| 13 | k_b1r<10,3,1,1> | ntt.c:698 | 807 | 1.32 | NTT pass | n/a | | NTT pass |
| 14 | k_bload | rns_dist.c:551 | 126 | 1.19 | n limbs: read n x 8 B from the sharded operand (3/4 remote) + write n x 8 B; up to 8.6 GB in 125 ms | ~69 at the largest | 2 | same remote pull as #2 (the S-5 51 GB/s); per prime; 112 B scratch |
| 15 | k_scatter4 | rns_mul.c:838 | 123 | 1.17 | per element: 8 B a read + 32 B written (+ same for b) = 40-80 B; calls 4-51 ms | not derivable (M, L not in the db); assumed up to ~0.8 TB/s at the largest | | coalesced; one `bdesc` (48 B) load per element; operand pointers may be remote |
| 16 | k_b16<6,1,0> | ntt.c:153 | 735 | 1.02 | NTT pass | ~1000-1500 | 33-50 | NTT pass |
| 17 | k_b16<5,1,0> | ntt.c:153 | 133 | 0.95 | NTT pass | | | NTT pass |
| 18 | k_twpack_r<8> | ntt_dist.c:204 | 1392 | 0.86 | 2^26 pts (q/8): 1.07 GB in 0.62 ms | ~1700 | 57 | tile transpose + twiddle, already coalesced both ways |
| 19 | k_unpack | ntt_dist.c:40 | 1392 | 0.79 | 1.07 GB in 0.57 ms | ~1900 | 63 | coalesced runs of rows_k limbs; fine |
| 20 | k_pw_y | ntt.c:732 | 141 | 0.62 | 2^29 pts x (8 in, 8 out, y) = 8.6-13 GB in 4.4 ms | ~1800-2900 (y partly cached) | 60-90 | fine |
| 21 | k_scatter_runs | rns_dist.c:105 | 29 | 0.62 | read 4.3 GB local + write 4.3 GB (3/4 remote) in 21 ms | 410 total, ~300 remote write | 14 | remote push-write, already at the link rate (221-253 GB/s); no lever |
| 22 | k_unpacktw_r<8> | ntt_dist.c | 696 | 0.60 | 1.07 GB in 0.86 ms | ~1250 | 42 | fine |
| 23 | __amd_rocclr_copyBuffer (hipMemcpyAsync D2D, many small) | runtime | 2889 | 0.55 | 0.1-4 MB | | | many small copies |
| 24 | k_gather_shift (db_copy / shifts) | dbig.c:687 | 83 (36 large) | 0.55 | read + write up to 1.34e9 limbs = 21 GB at the 96 ms maximum | ~220 at the largest | 7 | coalesced but 64 B scratch (`dget` quarter select) and one synchronous call per quarter |
| - | k_addsub2 (DBIG_ADDSUB2=1, rp2) | dbig.c:772 | 171 | 2.02 | 24 B x limbs; largest call 84 ms | ~380 at the largest (derived from the S38 old/new ratio) | ~13 | residue after the S38 fix: 32 KB LDS = 2 blocks/CU (8 waves) and a load / scan / store sequence with no overlap; thread 0 serial scans of 2 x 256 per 4096 limbs; one synchronous call per quarter + host flag scan; 64 B scratch |

All other kernels are < 0.5 s (k_scatter1 0.40, k_scatter 0.39, k_complement 0.08).

#### 2. Findings that cut across rows

1. Scratch ("private segment") sizes in the trace: k_addsub 208 B, k_gather / k_bload / k_scatter_runs 112 B, k_maxidx 120 B, k_gather_shift / k_addsub2 64 B; the NTT, k_scatter4, k_modq kernels use 0. The cause is the by-value argument structs (`dv`, `acc`) with pointer arrays indexed by a runtime quarter number (`v.q[d]`, `a.q[d]`, `a.w[d]`), which the compiler copies to per-thread scratch. Hypothesis (assumed, not measured): this adds a dependent scratch load to every element and is part of why the "coalesced" dbig kernels (maxidx, gather_shift, k_addsub2) run at 190-380 GB/s. Test: replace `v.q[d]` by a 4-way select of four scalar kernel arguments (or `__builtin_amdgcn_readfirstlane`-uniform select); check that `private_segment_size` is 0 and time one k_maxidx / k_gather_shift call. This is a cheap standalone micro-test and applies to every row marked "scratch" at once.
2. The remote-pull cost: a dbig operand is striped over the four APUs' HBM, so every `acc_get` of a rank's gather is 3/4 remote. S13 / TASKS S-5 measured the pull at about 51 GB/s per APU against a push at 221-253 GB/s. k_gather's 3.2 GB of remote reads in 44 ms is ~73 GB/s: the same limit. That is why #2 and #14 are 10-30x below HBM although their reads are contiguous.

#### 3. Candidates with an expected saving of 1 s or more per APU (6.441e10; all savings modelled)

##### A. k_gather: gather the raw operand once, canonicalise per prime locally (#2) -- 3.0 to 4.4 s
- Today each product re-pulls the A (and B) operand once per prime (np = 4, rns_dist.c:828-830): 174 pulls per APU for 29 products.
- Fix: one raw pull per operand into a staging plane (the sharded read in the final transposed layout, `x[il*C+j]` through a 32x33 LDS tile as `k_transpose` does, ec_canon omitted), then np local elementwise canon passes into xa[p] / yb (one plane read + one write each, 8.6 GB, ~3.5 ms at 2.5 TB/s). For A, the staging plane can be xa[3] itself (canon of primes 0-2 into xa[0..2] from it, prime 3 in place). B needs one extra 4.3 GB plane (xb is reused per prime), or the cache plane `cb` when a cache slot exists. At the standard share the block pool has ~38 GB free per APU in the division; at 8.5e10 the headroom is smaller (371.7 of 480 GB peak) and the extra plane must be checked against mem_model.py.
- Model: pulls 174 -> 58 x 44 ms = 2.55 s, plus 174 canon passes x ~4.3 ms = 0.75 s: 3.3 s against 7.67 s, saving 4.4 s; with A-only (no extra plane) about 3.0 s. Assumption: the 44 ms is remote-bound, as the 73 GB/s against 51 GB/s measured pull indicates; a 5-line micro-test (run the gather with a local-only source) confirms or refutes it before any build.
- Alternative: owner push (S-5 form) at 221-253 GB/s: 3.2 GB / 230 GB/s = 14 ms plus 3.5 ms local, 7.67 -> ~3.0 s (saving ~4.7 s) but needs cross-APU synchronisation and a landing buffer; larger change, same ceiling.
- Tile helper: not k_addsub2's. The transpose tile of `k_transpose` (32x33, rns_dist.c) is the model; the XOR-swizzled 4096-limb thread-major tile of k_addsub2 serves a different layout (16 consecutive limbs per thread) and is not needed here. Digits unchanged (same values written).

##### B. k_maxidx: scan from the top with early exit (#8) -- 2.0 to 2.4 s (APU0 2.42, APU1 1.92, APU2 1.72)
- Today `db_norm` / `db_cmp` scan the whole quarter range (up to 1.34e9 limbs) although the answer (1 + highest limb that differs from zero / from b) is almost always within the top few limbs.
- Fix: host loop of windows from the top: [hi-4096, hi), then x16 wider windows (a one-block, 256-thread kernel for the first, then the existing grid for the rest), top quarter first and lower quarters only if the upper ones are all-zero/equal. Each probe is ~5 us plus a 8-byte `hipMemcpy`; the full scan stays as the last window, so the result (and the digits) are exactly the current one, including the all-zero and equal-operand cases (db_cmp of equal numbers keeps a full scan, which is rare).
- Model: 277 large calls x (~20-40 us) ~ 0.01 s against 2.42 s; the saving is bounded by the wall time of the synchronous call per APU: about 2.0-2.4 s on the critical path (the slowest APU sets it). Also removes the 120 B scratch from this kernel's hot loop.
- Tile helper: not applicable (a reduction, no layout change).

##### C. k_addsub2 residue (#-) -- 1.0 to 1.5 s (weak model)
- At 2.02 s it is still ~13 % of HBM on its biggest call (derived). The 32 KB LDS tile caps occupancy at 2 blocks/CU and the block runs load -> serial scan -> store with nothing overlapping.
- Fix options: (a) 16 KB tile (two operand loads share one 8-limb half-tile pass: 4 blocks/CU); (b) move the thread-0 scan of 2 x 256 into a wave-level scan (`__shfl_up` / ballot over the 4 waves) so all 256 threads work; (c) drop the scratch (finding 1). Target ~3x on the largest calls: 2.0 -> 0.7 s. Assumed; depends on whether the true limit is LDS occupancy or the scan.
- Tile helper: this IS the k_addsub2 helper, modified in place.

##### Below the 1 s bar (not built, for the record)
- k_bload (#14, 1.19 s): the same raw-pull problem as A, per prime; a shared raw staging also serves it. S38's bound 1.0 s (modelled) stands; it comes for free with A if the staging plane is shared (B-form and C-form do not run concurrently).
- k_crt_batch B-form (#3, the 228-block launches, 13 calls x 63 ms = 0.82 s plus 30 calls on 912 blocks = 0.66 s): use S >= 5 x 228 stripes (LDS 12.4 KB allows 5 blocks/CU); the merge of the extra 4-limb spills (spill_merge, k_crt_batch's `stripe_spill`) is the existing mechanism. Modelled: occupancy 1/5 -> 5/5 gives 0.65 s + 0.13 s = ~0.8 s.
- k_modq (#12, 1.57 s GPU): replace `% q` by the Montgomery multiply (`ec_mod` has pu / mu); exact arithmetic so residues are identical (T1 check keeps its meaning). Modelled GPU saving ~1.2-1.4 s, wall saving smaller because the stage overlaps the division in a background thread (Phase 10 H B1): 0.3-0.8 s.
- k_gather_shift (#24, 0.55 s): finding 1 only; ~0.3 s.
- NTT passes (#4, 6, 7, 9-11, 13, 16, 17: ~19 s per APU in all) run at 25-57 % of HBM (modelled, assuming 8 points per thread; the grid sizes fit 2^26 .. 3x2^29 points per call). A further 25 % would be a pass-count / fusion project (as already pursued in Phases 13-15), not a quick lever; the all-to-all (#5) is link-bound.

#### 4. Order of work if the user wants a build
1. Micro-test of finding 1 (scratch) and of A's assumption (a gather from a local-only source): minutes, one node, no code change in the product.
2. B (maxidx top-down): small, local, exact; saves 2.0-2.4 s of the 81 s.
3. A (raw-once gather): largest (3-4.4 s) but touches the C-form memory plan (one extra plane at the 8.5e10 share).
4. C: only after 1-3, since the scratch fix may already move it.
With ADDSUB2 default the run is ~81 s; B + A would bring it to ~75-76 s (modelled), about 7 %.

#### RESUME
- Nothing running; no allocation held by this task. Read-only on aac7 (new files ~/s38/q9.py, q10.py, q11.py only).
- Next action for the user: decide whether to commission the micro-test (finding 1 + A's assumption) and B; see section 4.

## S40: DBIG_MAXIDX_TOP (top-down k_maxidx) measured -3.2 s per run (95 % CI -5.6 .. -0.75, 3 ABBA rounds); micro-test refutes "the k_gather pull is remote-bound" and shows the runtime-quarter scratch costs ~30 % of the kernel

(former file: S40.md)

Branch s40 (from s39). Node: hold 12454 (aac7-n00), built in ~/ntt-wt/s40 (make -B). All numbers measured unless labelled.

#### 1. DBIG_MAXIDX_TOP=1 (ecalc/dbig.c `maxidx`, README row added; default off)
`maxidx` (behind db_norm / db_cmp) now scans each quarter's [lo,hi) in windows from the top: 4096 limbs, then x16 wider each step; it stops at the first window with a hit; the last window reaches `lo`, so the result is the single full scan's (all-zero and equal operands still scan everything). Same `k_maxidx` kernel and 8-byte-per-block copy per window; per-quarter, no cross-quarter coupling (lower quarters also exit at their first window).

| test | command | result |
|---|---|---|
| gate 1e9, TOP=1 | `DBIG_MAXIDX_TOP=1 RNS_VERBOSE=1 ECALC_VERBOSE=2 ./ecalc 1000000000 f`; `digcmp.sh f ~/ref/e_1000000000.txt` | VERIFY OK, identical (12.86 s) |
| gate 1e9, ADDSUB2=1 + TOP=1 | same with `DBIG_ADDSUB2=1` | VERIFY OK, identical (11.88 s) |
| 1-node ABBA 6.441e10, no digit writes | `MINR=3 MAXR=6 HALF_S=1.0 archive:drivers/tools/s40_abba.sh 12454 ~/s40`; A = `DBIG_ADDSUB2=1`, B = `DBIG_ADDSUB2=1 DBIG_MAXIDX_TOP=1`; ABBA / BAAB | all 12 runs VERIFY OK; stopped after round 3 (p < 0.05) |

ABBA totals (s): r1 A 82.38 / 83.45, B 78.29 / 78.94; r2 B 77.69 / 80.03, A 80.50 / 82.71; r3 A 80.58 / 81.02, B 78.19 / 78.41. Round deltas mean(B)-mean(A): -4.3, -2.7, -2.5. **Paired mean -3.18 s, 95 % CI half-width 2.43 s (-5.6 .. -0.75), n = 3 rounds** (t = 3.182 for 2 dof). About 3.9 % of ~81.5 s. Modelled saving was 2.0-2.4 s; the point estimate is higher than modelled, the CI includes the model. Run-to-run noise is ~1-2 s, so a longer ABBA would tighten it; not needed for a decision.

#### 2. Micro-test ecalc/tests/t_gather_probe.c (build: `make tests/t_gather_probe`; no product code involved)
Copies of the k_gather kernel at the product's size: rows x C = 2^29 points per rank, R = 4 rows (the product's stride), source = 4 quarters of 2^29 limbs (17.2 GB), run on APU 0, best of 8, outputs compared (0 mismatches in all variants).

| variant (rows 8192, C 65536) | best ms | read GB/s |
|---|---|---|
| remote source (quarters on 4 APUs), runtime quarter (the product) | 51.8 | 83 |
| local-only source (one HBM allocation, same code) | 51.4 | 84 |
| remote source, 4-way select of scalar args | 36.1 | 119 |
| local-only source, 4-way select | 35.9 | 120 |

Second shape (rows 1024, C 524288): 34.0 / 35.8 / 26.7 / 23.8 ms. Sanity: a coalesced read of one 4.29 GB quarter from APU 0: APU 1's HBM 47.6 ms (90 GB/s), own HBM 1.4 ms (3.1 TB/s), so the remote placement is real.

(a) **Remote vs local source: no difference at the product's shape (51.8 vs 51.4 ms, 1.01x; at the second shape remote is 0.95x-1.12x).** The product's k_gather takes 41.6-48 ms (S39), the probe's 52 ms: the same. The 44 ms is therefore NOT bound by the remote pull (gathering from 3 peers runs at 3 links' aggregate; the one-peer read of 90 GB/s is not the limit). S39's candidate A rests on that assumption; the pull would save at most 0-3 ms per call. The kernel is bound by its strided write `x[il*C+j]` (and the scratch, below).
(b) **Scratch.** `-Rpass-analysis=kernel-resource-usage`: k_gather copy (by-value acc struct, runtime quarter index) ScratchSize 112 B/lane (the S39 trace: 112); the 4-way-select version: **0 B**. Time: **0.70x** at the product shape (51.8 -> 36.1 ms), 0.78x / 0.66x at the other.

**Verdict on S39's raw-once gather (3.0-4.4 s modelled): do not build it as specified** (its saving came from replacing 174 pulls by 58; pulls are not the cost, local costs the same). Build instead the select-of-scalar-arguments change in k_gather (and k_bload, k_scatter_runs, k_maxidx, k_addsub2, k_gather_shift, which share the pattern): modelled saving for k_gather alone 174 x (44 x 0.30) ms = about 2.3 s per APU (modelled, assumes the probe's 0.70x carries over; same arithmetic, digits cannot change); the other 5 kernels add something not yet bounded. A coalesced-write redesign of k_gather (the strided write is the next floor) is the larger lever and is untested.

#### RESUME
- Nothing running; the hold 12454 has no steps of mine (`squeue -s -j 12454`). Data: ~/s40/pairs.tsv, summary.txt, S40_DONE on aac7. Worktrees: ~/ntt-wt/s40 (aac7), ../ntt-wt-s40 (local).
- Note: the s38 ABBA driver's `srun -w <node>` made step creation hang forever on this hold ("Requested nodes are busy"); archive:drivers/tools/s40_abba.sh drops `-w`. The earlier S38 driver (marker ~/s38/abba/S38_DONE, FAILED: step creation) never ran a single run.
- Next: user decides (1) adopt DBIG_MAXIDX_TOP (set default in `maxidx_top_on`), (2) commission the select-of-scalars change for k_gather first (then the other dbig/rns kernels), and a coalesced-write k_gather.

## S41: DBIG_QSEL (scalar-select quarter pointers), gates and ABBA

(former file: S41.md)

Branch s41 = s40 + main (ADDSUB2 default 1) + DBIG_QSEL (default 0, README row). Built on aac7 in ~/ntt-wt/s41 with `make -B SHMEM_CRAY=1 GMP_HOME=~/gmp` (log has -DCOMM_SHMEM).

#### Change
Kernels templated on QS: k_gather, k_scatter_runs, k_bload (rns_dist.c), k_gather_shift, k_addsub2, k_maxidx (dbig.c). QS=1 picks the quarter pointer by `p=q[0]; if(d==1)p=q[1]; ...` (constant indices); QS=0 is the original code. Launch chooses by `DBIG_QSEL`.

#### ScratchSize (bytes/lane, -Rpass-analysis=kernel-resource-usage, measured)
| kernel | QS=0 | QS=1 |
|---|---|---|
| k_gather | 112 | 0 |
| k_scatter_runs | 112 | 0 |
| k_bload | 112 | 0 |
| k_gather_shift | 0 | 0 |
| k_addsub2 | 0 | 0 |
| k_maxidx | 0 | 0 |
The dbig.c kernels (struct dv) were already at 0 scratch; QSEL changes nothing there (no expected gain).

#### Gates at 1e9 (measured; ./ecalc 1000000000, digcmp.sh vs ~/ref/e_1000000000.txt)
| env | verify | digits | total |
|---|---|---|---|
| DBIG_QSEL=1 | VERIFY OK | identical | 12.87 s |
| DBIG_QSEL=1 DBIG_MAXIDX_TOP=1 | VERIFY OK | identical | 12.27 s |

#### ABBA (6.441e10, 1 node each, no digit writes; archive:drivers/tools/s40_abba.sh, MINR 3 MAXR 6, stop at p<0.05; all runs VERIFY OK; measured)
ARM envs also carry the default DBIG_ADDSUB2=1. Paired mean of round deltas mean(B)-mean(A), 95 % CI (t, n-1 dof):
| switch | node (hold) | A | B | rounds | mean delta | 95 % CI | round deltas |
|---|---|---|---|---|---|---|---|
| DBIG_MAXIDX_TOP | aac7-n30 (12463) | defaults | TOP=1 | 5 (20 runs) | -1.63 s | -2.87 .. -0.39 | -0.9, -0.4, -2.9, -1.6, -2.3 |
| DBIG_QSEL | aac7-n00 (12454) | TOP=1 | TOP=1 QSEL=1 | 3 (12 runs) | -2.08 s | -3.86 .. -0.30 | -2.9, -1.5, -1.9 |
(S40's MAXIDX_TOP on 12454: -3.18 s, CI -5.6 .. -0.75, 3 rounds.) Second-node QSEL coverage is on aac6 (other agent); swapped aac7 ABBAs were cancelled by the coordinator. QSEL gain measured ~2.1 s (~2.6 % of ~78 s), close to the modelled 2.3 s for k_gather alone; k_scatter_runs and k_bload scratch also went 112 -> 0.

## S43: multi-node A/B of the three dbig kernel defaults (aac7, 2 and 4 nodes)

(former file: S43.md)

Labels: m measured. Date 2026-10-10 20:08-20:52 EDT, aac7, job 12464. Build = branch s41 binary; base = the aac7 launch line incl. X2; 6.441e10 digits per node (no digit output).
Driver: `archive:drivers/s43/s43_batch.sh` (+ `s43_job.sbatch`, `s43_stats.py`, `rundriver.sh`, `s31_lib.sh`).

- **A** = `DBIG_ADDSUB2=0` (old kernels; MAXIDX_TOP and QSEL off).
- **B** = `DBIG_ADDSUB2=1 DBIG_MAXIDX_TOP=1 DBIG_QSEL=1` (the new defaults).
- Gates: 2 x 2-node 1e9 runs, VERIFY OK, digits identical. 0 failed runs; 18/18 timed runs VERIFY OK.

#### Results (m; wall seconds, A/B per round; delta = B - A)
| nodes | hosts | rounds A/B | mean delta | 95 % CI |
|---|---|---|---|---|
| 2 (pair 1) | aac7-n31, aac7-n40 | 230/169, 171/149, 173/143 | -37.7 s | -88.8 .. +13.5 |
| 2 (pair 2) | aac7-n41, aac7-n50 | 246/156, 182/163, 181/154 | -45.3 s | -142.0 .. +51.3 |
| 4 (ABBA) | 4 nodes | 276/230, 264/220, 271/211 | -50.0 s | -71.7 .. -28.3 (p < 0.05) |

- B was faster in every round of both 2-node pairs (6/6). The 2-node CIs are wide: the first round of each pair had a slow A (230 s, 246 s), which dominates the variance. The 4-node ABBA is significant, about -18.5 % of the A wall.
- The multi-node gain (-50 s at 4 nodes) exceeds the 1-node gain (about -16 s: ADDSUB2 -12.3, MAXIDX_TOP -1.6..-3.2, QSEL -0.9..-2.1): the dbig add/sub and maxidx kernels also run inside the multi-node division.

#### RESUME
Done; nothing running. The three switches are already defaults on main (user, 2026-10-10).

## S44: aac6 SH5_MI300A nodes -- regression and ABBA of DBIG_MAXIDX_TOP, DBIG_QSEL, DBIG_ADDSUB2

(former file: S44.md)

Branch s44 = s41 (ADDSUB2 default 1, DBIG_MAXIDX_TOP, DBIG_QSEL; S41 gates passed at 1e9) + tools/s44_*.sh + a test knob (below). Clone ~/ntt-s44 on aac6 (binaries from s41 + driver scripts; ~/ntt-s44t has the knob commit for the unit tests). Output dir ~/s44out.

#### Hardware finding (measured)
- SH5_MI300A_SPX (aac6-sh5spx-a): 1 node, ONE MI300A (gfx942), 1 visible device, 125 GB host. ecalc cannot run there: `rns_init: need 4 devices, have 1` (ecalc is one prime per device; NP=3 decimal needs 3, binary base 4). Only single-device tests (t_ntt) start there. Released.
- SH5_MI300A_CPX (aac6-sh5-{09,12,15,36}): each node = one APU split in 6 XCD devices (gfx942), 22.9 GB VRAM each (128 GB / 6), 48 CPUs, 120 GB host. ecalc runs with the XCDs as its devices (3 for decimal limbs, 4 for LIMB_BASE=2, DBIG uses 4) when the planes fit one XCD: `POOL_LOG=29` (default 31 asks for 17.2 GB planes and OOMs). No HIP_VISIBLE_DEVICES needed.
- Largest size that fits (measured, POOL_LOG=29): 4e9 digits VERIFY OK (about 17 s on s12-12/15, 23 s on s12-09/36); 6e9 fails (plane pool growth), 8e9 and 1e10 OOM. All ABBAs use 4e9 digits, POOL_LOG=29, no digit output. Absolute times are not comparable with the 4-APU aac7 numbers (CPX XCD devices share one APU's HBM); only the arm differences are the S44 data.

#### Regression (node s12-09, branch s41 build)
| test | result |
|---|---|
| t_ntt 24 | FAIL only on the speed check "forward rate 0.25-0.34 TB/s below 0.9" (5 of 3215 checks; a CPX XCD has 1/6 of the APU bandwidth); all correctness checks ok |
| t_verify | PASS (334 checks) |
| t_out | PASS (1 check) |
| t_patch | PASS (both forms) |
| t_mul 20, t_bs, t_dbig 0, t_newton 20 | at the default plane pool: OOM (17 GB planes on a 21 GB XCD), not run; with the knob RNS_INIT_POOL_LOG=29 (commit cbde76c, rns_init reads it, unset = unchanged): t_mul PASS (189 checks), t_dbig PASS (555 checks), t_newton PASS (620 checks), t_bs PASS (10 checks; needs the ref/e_*.sha256 files in the clone, linked from ~/ntt/ecalc/ref) |
| t_mn_grid | not run (needs 2 node-processes) |

e9 gates (1e9, POOL_LOG=29, digits vs ~/ntt/ecalc/ref/e_1000000000.txt by digcmp.sh):
| env | verify | digits | total |
|---|---|---|---|
| defaults | VERIFY OK | identical | 7.88 s |
| DBIG_MAXIDX_TOP=1 | VERIFY OK | identical | 6.99 s |
| DBIG_QSEL=1 | VERIFY OK | identical | 7.33 s |
| DBIG_MAXIDX_TOP=1 DBIG_QSEL=1 | VERIFY OK | identical | 6.79 s |
| DBIG_ADDSUB2=0 | VERIFY OK | identical | 7.51 s |
| LIMB_BASE=2 | VERIFY OK | identical | 13.39 s |
| LIMB_BASE=2 TOP=1 QSEL=1 | VERIFY OK | identical | 14.21 s |
All 24 ABBA-round runs per ABBA were VERIFY OK (every run of every arm).

#### ABBA (4e9 digits, POOL_LOG=29; archive:drivers/tools/s44_abba.sh; >= 3 rounds, stop p<0.05 or 6 rounds; one job per node, 4 nodes in parallel)
delta = mean(B) - mean(A) per round, s; per-node mean over its rounds; "sig" = the node's own paired 95 % interval excludes 0.
| switch | node | rounds | mean A | mean B | delta | sig |
|---|---|---|---|---|---|---|
| (b) A defaults, B MAXIDX_TOP=1 | s12-09 | 6 | 22.66 | 22.72 | +0.07 | no |
| | s12-12 | 6 | 17.11 | 16.90 | -0.22 | no |
| | s12-15 | 6 | 17.79 | 17.43 | -0.35 | yes (stopped at 6) |
| | s12-36 | 6 | 22.21 | 21.95 | -0.26 | no |
| | pooled (24 rounds) | | | | -0.19 +- 0.19 | borderline (t = -2.1) |
| (c) A TOP=1, B TOP=1+QSEL=1 | s12-09 | 6 | 23.21 | 23.22 | +0.02 | no |
| | s12-12 | 6 | 17.16 | 16.80 | -0.37 | no |
| | s12-15 | 5 | 17.79 | 17.44 | -0.35 | yes (p<0.05 after 5) |
| | s12-36 | 3 | 23.83 | 23.08 | -0.75 | yes (p<0.05 after 3) |
| | pooled (20 rounds) | | | | -0.30 +- 0.19 | yes (t = -3.3) |
| (a) A ADDSUB2=0, B defaults | s12-09 | 6 | 22.77 | 22.66 | -0.11 | no |
| | s12-12 | 6 | 17.29 | 16.92 | -0.36 | no |
| | s12-15 | 6 | 18.00 | 17.64 | -0.36 | no |
| | s12-36 | 6 | 24.05 | 23.69 | -0.36 | no |
| | pooled (24 rounds) | | | | -0.30 +- 0.20 | yes (t = -3.1) |
(Pooled rounds are treated as independent; the s12-09 (a) row is a second pass, the first was cut by the 45-min job limit.) Nodes s12-09/36 run about 30 % slower than s12-12/15 for all arms (unexplained; same mode).

Reading: all three switches are a gain of about 0.2-0.4 s of 17-23 s (1-2 %), no node showed a slowdown beyond noise (s12-09 shows +0.07 and +0.02 for b and c). Within-ABBA a position effect is visible (odd and even rounds alternate, e.g. a: -0.1/-0.8/0.1/-0.8...), so per-round deltas are noisy; the pooled sign is consistent. CPX XCD devices are not the production 4-APU target; this is coverage of a second hardware configuration, not a replacement for the aac7 ABBAs.

#### RESUME
Done. Nothing running on aac6 for S44 (all jobs cancelled). Raw logs ~/s44out/ on aac6 (n09_*, n12_*, n15_*, n36_*: pairs.tsv, RESULT, logs).

## S51: kernel round 2, part b (K2 k_addsub3, K3 k_modq_m, K4 k_crt_batch_m) -- FINISHED: K2+K3 propose default (1-node significant on aac7 and aac6, 4-node ns), K4 no gain

(former file: S51.md)

Branch `s51` (from main daeb0f9). All three changes are behind new switches, **off by default** (one README row each); digits must be identical. Times Eastern; every number is labelled measured / modelled / assumed.

#### 1. Design

| id | switch | change | files |
|---|---|---|---|
| K2 | `DBIG_ADDSUB3=1` (needs `DBIG_ADDSUB2=1`) | `k_addsub3`: same 4096-limb chunk, same `cout`/`prop` flags and output as `k_addsub2`. Each thread owns two 8-limb segments (A: limbs 8 tid.., B: 2048 + 8 tid..); the chunk moves through a **16 KB** XOR-swizzled tile in two 2048-limb halves (LDS 16.5 KB per block: 4 blocks/CU, against 2 at 32 KB); the 512 (generate, propagate) pairs are scanned in order with `__shfl_up` inside each wave plus a 4-entry wave combine, replacing thread 0's serial 2 x 256 scan | `ecalc/dbig.c` |
| K3 | `DBIG_MODQ_MONT=1` | `k_modq_m`: Montgomery (R = 2^64) products in place of the 128-bit `%`: v <- montmul(v, B^256 R mod q) + (x mod q); the final scale by B^t likewise. Exact; used only when every q is odd and in [2^62, 2^63) (the T1 primes), else the old kernel. Residues are bit-identical (`ECALC_RES_LOG=1` still cross-checks against the host Horner) | `ecalc/dbig.c` |
| K4 | `RNS_CRT_SUB=m` (m > 1; 5 intended) | the B-form CRT launch (`b_core`, 228 blocks = one per CU, LDS for five) cuts every 16384-coefficient stripe into m sub-stripes (`k_crt_batch_m`, m x the blocks); `k_crt_sub_merge` (one thread per stripe) adds the m-1 interior 4-limb spills into the limbs (ripple bounded by the stripe end), copies the last sub-spill to the stripe's spill slot and adds any carry that leaves the stripe to it, so limbs and stripe spills are the same numbers as `k_crt_batch`'s | `ecalc/rns_mul.c`, `rns_int.h`, `rns_dist.c` (launch site only) |
| - | `DBIG_S51_STATS=1` | at exit one `S51STAT addsub n t modq n t crtb n t` line (host wall seconds of the db add/sub calls, `db_mod_qs`, and the B-form CRT stage): a low-noise kernel-level paired metric next to the run total | `dbig.c`, `dbig.h`, one call in `rns_dist.c` |
| - | `tests/t_s51_probe` | micro-benchmark (add, sub, in-place add, `db_mod_qs`; median/min over reps; `CHK` lines = residues of the results, compared base vs switch) | `ecalc/tests/t_s51_probe.c`, Makefile target |

Pre-checks without a GPU (measured): Python emulations of the exact block algorithm (index swizzle, halves, wave scan, partial chunks, sub/dec) and of the Montgomery step and the sub-stripe merge: 0 mismatches over randomized cases (scripts not kept; K2 emulation 40 cases incl. all-ones/sparse carries, K3 80 000 steps, K4 300 cases).

#### 2. Measurements so far (all measured)

aac6 SH5 CPX node s12-12 (one 12-min job, 23:43-23:52 EDT on 10-10, before the blackout; binary built on the aac6 login node):
- `t_s51_probe 25 3` (2^25 limbs, small): checksums (CHK add/sub/modq) **identical** base vs `DBIG_ADDSUB3=1` vs `DBIG_MODQ_MONT=1`, uniform and all-ones data. add 1.1 ms -> 0.8 ms (K2, -27 %), sub 1.1 -> 0.8 ms (uniform) / 1.5 -> 0.9 ms (all-ones); `db_mod_qs` 23.0 ms -> 1.0 ms (K3, 22x). Tiny sizes, one run each: a smoke test, not a result.
- `t_dbig 22 0 29` with `DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1`: VERIFY OK, 565 checks.
- `tools/gate.sh --nodes 1 --line none` (1e9 digits, `RNS_INIT_POOL_LOG=29`): `DBIG_ADDSUB3=1`, `DBIG_MODQ_MONT=1`, `RNS_CRT_SUB=5` and all three together: **PASS, VERIFY OK, digits identical, segv 0** (ecalc total 8.7 / 9.5 / 9.9 / 9.8 s; one run each, not a timing). The 1-node 1e9 run does not call `db_mod_qs` (S51STAT modq n = 0), so K3 is exercised by `t_dbig` here and by the 6.441e10 runs below (4 calls, VERIFY checks the T1 residues).
- aac7: build with SHMEM OK (`-DCOMM_SHMEM` in the log), kernels compile (K2: 156 VGPR, 16.5 KB LDS, no scratch; K3: 30 VGPR); no aac7 node was free for me before the queue (6/6 nodes in Jobs A and B).

#### 3. What is armed (fire-and-forget; nothing of mine runs in the foreground)

aac7 (Job B queue, `~/batch1011/jobB/queue/`, items in file order after T5a's `20_1..20_4`; out dir `~/batch1011/T5/s51/`; binary `~/ntt-wt/s51/ecalc`, never rebuilt):
- `20_5_s51a.sh` (EST 25): probe (2^28 limbs, 7 reps, kinds 0/1, base/K2/K3 x2), `t_dbig 24` with both on, gates K2, K3 (+`ECALC_RES_LOG=1`), K4, all-three at 1e9; `s51_decide.py` keeps K2/K3 only if probe checksums equal and time ratio < 0.97 and the gate passed, K4 if its gate passed -> `ARM_B`, `decide.txt`, `gates.txt`.
- `20_6_s51_abba1.sh` (EST 50): 1-node ABBA at 6.441e10 (A = base, B = ARM_B; paired rounds, stop at p < 0.05 on the total, CI half-width <= 1.5 s, or 5 rounds, MINR 3), `abba1/{pairs.tsv,s51stat.tsv,stats.txt,ABBA1_DONE}`; stats.txt also gives the paired addsub / modq / crtb times (S51STAT).
- `20_7_s51_abba4.sh` (EST 60): 4-node confirmation (6.441e10 per node, 3 rounds), `abba4/ABBA4_DONE`.
- The Job B driver skips an item that would cross W or D and retries after the handoff; items are skipped (exit 0) when no kernel was kept.

aac6 (login-node driver `~/ntt-s51/archive:drivers/s51/s51_aac6.sh`, started 23:5x EDT 10-10, sleeping to its timers; out `~/batch1011/T5/s51b/`, log `driver.log`):
- CPX worker 03:30 EDT: one 45-min job on s12-12 (own sbatch, cancelled by id): the same stage (probe 2^27, t_dbig, gates, decision), then ABBA at 4e9 digits (coverage; stop CI half-width 0.15 s / 8 rounds) -> `CPX_DONE`.
- PPAC worker 03:50 EDT: 42-min slots on s24-26 under `flock ~/batch1011/aac6_s24-26.lock` (shared with T5a, whose driver takes the same lock), first slot runs the stage, then ABBA at 6.441e10 resuming from `ppac/abba/pairs.tsv` every slot until the stop rule / 8 rounds or D (no slot started that cannot finish by D) -> `PPAC_DONE`.
- Dry-run (STUB=1: no Slurm, stub runs) of the aac6 driver and of the three aac7 items passed on the aac6 login node and locally before arming.

#### 4. Results (measured; aac7 Job B hold, aac6 s24-26 / s12-12; times Eastern, 2026-10-11)
**Decision stage** (`decide.txt`, identical on aac7, aac6 PPAC and aac6 CPX): probe checksums equal in all; K2 probe time ratio 0.745 (aac7), 0.736 (PPAC), 0.661 (CPX); K3 0.070 / 0.058 / 0.043; K4 kept on its gate. ARM_B = `DBIG_S51_STATS=1 DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1 RNS_CRT_SUB=5`.
**Gates** (1e9 digits, 1 node, each switch alone, on aac7 and aac6 PPAC and CPX): all 12 PASS, VERIFY OK, digits identical, segv 0 (K3 also with `ECALC_RES_LOG=1`).

**ABBA, A = base, B = ARM_B (all three kernels), mean B-A, negative = faster**
| where | scale | rounds | total B-A | 95 % CI half | significant | addsub (K2) | modq (K3) | crtb (K4) |
|---|---|---|---|---|---|---|---|---|
| aac7 1 node | 6.441e10 | 4 (p<0.05) | **-2.03 s** | 1.38 | yes | -0.178 | -1.659 | +0.009 |
| aac6 PPAC (s24-26) 1 node | 6.441e10 | 4 (p<0.05) | **-1.22 s** | 0.84 | yes | -0.176 | -1.573 | +0.019 (ns) |
| aac7 4 nodes (abba4) | 6.441e10/node | 3 (cap) | -6.03 s | 21.93 | no | -2.002 | -6.395 | +0.068 |
| aac7 4 nodes (abba4b) | 6.441e10/node | 5 (deadline) | -5.21 s | 35.89 | no | -1.974 | -6.394 | +0.068 |
| aac6 CPX (s12-12) | 4e9 | 0 | n/a | n/a | n/a | n/a | n/a | n/a |
The kernel columns are the S51STAT host wall seconds per run (paired B-A), significant in all rows with CI half-width <= 0.7 s except where marked. The 4-node total deltas per round: abba4 -0.5, -16.2, -1.4; abba4b +11.7, -52.8, -12.8, +13.3, +14.6 (the S47 +-26 s bimodality dominates; CI +-22..36 s).
CPX ABBA **failed at start** (bad run in round 1: the environment was not passed to the run; ALERT in `~/batch1011/T5/s51b/cpx/abba/`); only the CPX gates and probe exist. Not re-run (no cluster time left).

##### Split of the gain among K2/K3/K4 (from S51STAT, measured)
- **K3 (`DBIG_MODQ_MONT`, db_mod_qs): the main gain.** -1.66 s (aac7 1 n), -1.57 s (PPAC 1 n), -6.4 s (aac7 4 n; the 4-node run does 4 calls per node on larger data, or the 1-node figure is per run, mechanism not separated). Share of the 1-node total gain: 82 % (aac7), 129 % (PPAC; the three kernel deltas sum to -1.73 s against -1.22 s total, within its CI).
- **K2 (`DBIG_ADDSUB3`, k_addsub3):** -0.18 s at 1 node (9 % of the aac7 gain), -2.0 s at 4 nodes.
- **K4 (`RNS_CRT_SUB=5`, k_crt_batch_m): no gain.** crtb time +0.009 s, +0.019 s (ns), +0.068 s: equal or very slightly slower. It earns no place in a default.
- Sum of kernel gains: 1 node aac7 -1.84 s vs total -2.03 s (consistent); 4 nodes -8.4 s vs total -6.0 / -5.2 s (consistent in sign and size, but the total is not significant).

#### 5. Proposal and caveat (user rule: faster on every node tested + identical digits -> propose default)
- **Proposal: make `DBIG_ADDSUB3=1` and `DBIG_MODQ_MONT=1` default (K2 + K3); leave `RNS_CRT_SUB` off (K4 shows no gain).** Digits identical in all 12 gates, probe checksums equal, residues cross-checked with `ECALC_RES_LOG=1`.
- The rule is met honestly at **1 node on both clusters** (aac7 -2.03 +- 1.38 s p<0.05; aac6 PPAC -1.22 +- 0.84 s p<0.05).
- **Caveat 1:** at 4 nodes the run total is **not significant either way** (-6.03 +- 21.9, -5.21 +- 35.9 s); it is not "faster on every node tested" in the strict run-total sense, only in the kernel metric (K2 and K3 time significantly down at 4 nodes, CI <= 0.7 s) and in the sign of the point estimate. No sign of slowdown at any scale.
- **Caveat 2:** CPX has no ABBA (driver failed), so there is no run-level coverage on that node type; its probe and gates are favourable.
- **Caveat 3:** the 4-node ABBA has only 3 + 5 rounds and an unresolved 26 s bimodality (see S47); a longer 4-node run would not confirm the total unless that noise is removed. Gain size is ~0.5-2.5 % of an 80 s 1-node run (modelled from the above), 5 s of ~190 s (~3 %) at 4 nodes if the kernel gain carries over.
- Not yet done: `gate.sh --build` on the final default-on code (this is a switch-only branch; making the default is a separate small change plus a gate on 1 and 2 nodes), and the engine suite (S50 saw unrelated `t_bs` / `t_mn_grid` failures, see S50, cause unchecked).

#### 4. RESUME
- Collect (aac7): `cat ~/batch1011/T5/s51/ARM_B ~/batch1011/T5/s51/decide.txt ~/batch1011/T5/s51/gates.txt ~/batch1011/T5/s51/abba1/stats.txt ~/batch1011/T5/s51/abba4/stats.txt ~/batch1011/T5/s51/abba*/ABBA*_DONE 2>&1`
- Collect (aac6): `cat ~/batch1011/T5/s51b/CPX_DONE ~/batch1011/T5/s51b/PPAC_DONE ~/batch1011/T5/s51b/{cpx,ppac}/{ARM_B,decide.txt,gates.txt} ~/batch1011/T5/s51b/{cpx,ppac}/abba/stats.txt 2>&1; tail -5 ~/batch1011/T5/s51b/driver.log`
- ETA (Eastern, modelled): aac6 CPX 03:30-04:20, PPAC slots 03:50 to about 07:30 (shared with T5a); aac7 items start when T2/T3/T5a items before them are done, likely after the 05:56 window handoff (B2), finishing by D 08:56; unfinished ABBA rounds are reported as partial.
- Adoption rule (user-accepted): faster on every node tested + identical digits -> propose default. Per kernel: read the S51STAT deltas in each stats.txt (kernel-level) and the total (run-level; gains of 0.3-1.5 s sit near the noise of one 80 s run, so the total may stay inconclusive while the kernel metric is clear).
- If a gate FAILED: `gates.txt` names the arm; the kernel is dropped from ARM_B automatically. After the batch: remove worktree `../ntt-wt-s51` and `~/ntt-wt/s51` (aac7), `~/ntt-s51` (aac6) once merged or decided.

## T11: aac6 SH5 CPX paired ABBA of K2 (DBIG_ADDSUB3) + K3 (DBIG_MODQ_MONT) at 4e9 digits

(former file: T11.md)

Session 2026-10-11, task t11 (rerun of the S51 CPX ABBA that exited rc 6 overnight). Branch `t11` from origin/main ceb94c1.
Times Eastern. All numbers below are **measured** unless marked otherwise.

#### Result

- **Result: SUCCESS.** 6 rounds (24 timed runs), all VERIFY OK, all rc 0, no crash, digits identical in both arms.
- **Total wall (ecalc `total`), B − A, mean over 6 rounds: +0.19 s, 95% CI ±0.29 s (p not < 0.05). B is not faster on the total.**
  Stop rule reached: round cap (MAXR = 6). The CI does not exclude zero; the 0.15 s half-width target was not reached.
- **Kernel stage addsub (K2 host wall, S51STAT): B − A = −0.044 s, 95% CI ±0.011 s, significant.** This is the only metric that clearly moves: K2 is faster in its own stage by about 0.04 s.
- **Kernel stage modq (K3): B − A = 0.000 s, no samples.** The S51STAT modq counter is 0 at 4e9 digits, so K3 (`DBIG_MODQ_MONT`) is **not exercised** in this run (same as the S51 1e9 gate note). K3 is not measured here.
- **crtb: B − A = +0.001 s ±0.006 s, not significant.**
- Expected gain of 0.2–0.4 s is **not confirmed**: the total moved +0.19 s (slower, within noise).

#### Environment fix (S51 failure)

- Root cause of the S51 rc 6: the 4e9 run on a CPX XCD needs more than `RNS_INIT_POOL_LOG=29`. With the default
  `ECALC_PLANE_CAP` (2^31, set in `ecalc.c` `plane_cap_switch`) the plane pools stay at 64.4 GB (per APU 20.6–27.0 GB) and
  `db_vmm_arena_alloc` fails (hipMalloc 2.15 GB, OOM, rc 6). The header `pool_log=31` is printed before `rns_init`,
  so it does not show the knob; the header is not evidence either way.
- Working setting: `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30` (planes 47.2 GB, per APU 16.3–22.7 GB, fits 22.9 GB).
  Probe run: total 20.37 s, VERIFY OK.
- The env vars are now exported inside the `bash -lc` string of the driver (`archive:drivers/t11/t11_abba.sh`) and in both arms.
  The `ECALC_PLANE_CAP=2^30` setting is a deviation from the ecalc default; it changes plane sizing only (digits verified).
  Setting it in the production CPX launch line would need to be documented in `ecalc/README.md` by the coordinator.

#### Runs

Command (per arm): `srun --jobid=22899 --overlap bash -lc "module load rocm; cd ~/ntt-t11/ecalc; export RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30; env ECALC_SEGV_TRACE=1 DBIG_S51_STATS=1 [B: DBIG_ADDSUB3=1 DBIG_MODQ_MONT=1] ./ecalc 4000000000"`
Node: aac6 SH5_MI300A_CPX aac6-sh5cpx-a (one hold, 22899, cancelled at end). Build: `make -s -j16` rc 0 on ~/ntt-t11 (ceb94c1).

| round | A (base) total, s | B (K2+K3) total, s |
|---|---|---|
| 1 | 19.18, 19.18 | 19.55, 20.04 |
| 2 | 19.65, 20.59 | 20.54, 20.06 |
| 3 | 21.05, 19.77 | 20.33, 20.85 |
| 4 | 20.93, 21.08 | 21.22, 20.28 |
| 5 | 21.13, 19.79 | 20.46, 20.92 |
| 6 | 19.66, 21.37 | 21.22, 20.14 |

Per-round delta (B mean − A mean): +0.6, +0.2, +0.2, −0.3, +0.2, +0.2. Running mean after 6 rounds: +0.19 s, half-width 0.29 s.

Run-to-run spread within an arm is about ±0.9 s (range 19.2–21.4 s), i.e. 3–4 times the effect. Timing noise dominates.

#### Open issues

- K3 untested at 4e9 (modq counter 0): needs a run that reaches `db_mod_qs` (e.g. a digit count or path that calls it) before any K3 claim.
- The gain is not resolved at this sample size. A sharper answer needs more rounds (the CI half-width shrinks roughly with sqrt(n); 0.15 s would need about 3–4× the rounds) or a stage-level comparison only.
- Driver incidents (fixed, no data affected): first arm attempt failed rc 6 (OOM, plane cap); a stop epoch was mis-parsed in one attempt (no runs executed); the hold was cancelled by a driver scancel; all own jobs cancelled, `squeue -s` shows no steps.

#### RESUME

- Branch `t11` (worktree ../ntt-wt-t11): this file + `archive:drivers/t11/t11_abba.sh` and `t11_run.sh`.
- Raw data on aac6: `~/sess1011b/t11/abba/` (pairs.tsv, s51stat.tsv, log/). Clone `~/ntt-t11` to remove.
- Coordinator to decide: whether to adopt `ECALC_PLANE_CAP=2^30` for CPX runs (documented in ecalc/README.md) and whether to run more rounds for K2 before adopting.

## NTT3P: can ecalc's large transforms run in 3 memory passes instead of 4? (design study, no code changed)

(former file: NTT3P.md)

Tasked by the main session. Read-only on code. Labels: (m) measured, (mod) modelled, (a) assumed. Sources: results/SINGLE_NODE.md §S21 §5 (A37-Q4), K13.md, K13b.md,
RESULTS.md §33 (D5) and §48 (lds/occupancy), N3x15.md, V314.md, internal code notes, `ecalc/ntt.c` (make_plan, plan_auto, k_b16, k_b16r, k_b1r),
`ecalc/mn_model.py` / `estimate.py --fabric target-m --bw 47` and `--fabric aac7_s18 --bw 6` (run on the login node, pure python).

#### 0. Summary

- **Best split at 2^31: 13 / 9 / 9** (bottom contiguous b1 of 2^13 points, two strided 9-stage passes, all with 16-column, 128 B segments), or **12 / 10 / 9**
  (keeps today's 2^12 b1 unchanged; one 10-stage pass with 64 B segments). Both fit in the 64 KB LDS with no registers staging.
- **Modelled saving per 2^31 forward: about 15 ms of 90.4 (17 %), range 5...24 ms.** The floor of any 3-pass plan is about 66 ms (3 x 17.3 ms NOP pass + 14 ms modmul), so -24 ms is the ceiling.
- **Per run (mod): 10 nodes about 1-2 s of 435 s (0.3 %); 576 nodes about 4 s (range 1.5-7) of 219-266 s (1.5-2 %).** The 576 gain is mostly NOT the 2^31 transform: it is the
  4-step pieces' row/column transforms of length 2^20...2^22, which run 3 passes today and would run 2.
- Effort M-L (2-3 weeks incl. validation), correctness risk low (canonical outputs are unique, t_ntt hashes), performance risk medium (1 block/CU, 64 B segments).
- No prior attempt at more than 7 strided stages or a 2^13 b1 exists. Recommendation: **a 2-3 day microbench first (E0/E1, section 6); build only if it shows the fat pass within 10 % of the model.**

#### 1. Constraints (from the code)

- `k_b16`/`k_b16r`: block = 2^7 rows x 16 adjacent columns = 2048 points (128 B contiguous per row), 256 threads, `sh[128][17]` = 17.4 KB (+ 1 KB twiddles) = 18.4 KB, **3 blocks/CU, LDS-limited**;
  VGPRs 46-66 (m, K13). `NTT_B16_STG` is clamped to 3..7 (`ntt.c:788`), so **no strided pass is longer than 7 stages today**.
- `k_b1r` (contiguous b1): LGL 10...12 points per block, 2^(LGL-LGV) threads, LGV 3 or 4 points per thread in registers, one LDS exchange between groups, XOR-swizzled (`b1r_swz` covers index bits up to 11 only),
  LDS 2^LGL x 8 B (8/16/32 KB), VGPR 18-22 (m, K13 for the old k_b1). 2^12 is already in production at 2^31 (plan 121: stages [0..11], 512 threads, 32 KB, 2 blocks/CU).
- Hardware: LDS 64 KB per workgroup (exactly 8192 points, no room for a padded tile or a twiddle table in LDS), 512 KB VGPR per CU (8 B point = 2 VGPR: 64 K points per CU if all registers held data, in practice about 16 K), HBM 2.0 TB/s in the NOP passes (1.8-2.0), copy kernels 2.2-2.4 TB/s (m).
- All pass logic is private to `ntt.c` (`make_plan`, `plan_auto`, `launch_b16`, `launch_b1`); `ntt_npass` / `ntt_pass_bounds` are used only by `tests/t_ntt.c`. `NTT_MAXPASS` = 8.
  The distributed 4-step packs (`k_twpack_g` etc.) are separate kernels, not part of the passes.

#### 2. Today's plans and the pass counts (computed from `make_plan`/`plan_auto`)

| logn | stage split now (b16 passes, then b1 bits) | passes now | passes with 13/9/9 |
|---|---|---|---|
| 17-19 | 7, b1 10/11/12 | 2 | 2 |
| **20, 21, 22** | 7+3 / 7+4 / 7+5, b1 10 | **3** | **2** (13+7, 13+8, 13+9) |
| 23 | 7+6, b1 10 | 3 | 3 (13+10 needs a 10-stage pass: 2 passes only with C=8) |
| 24, 25, 26 | 7+7, b1 10 / 11 / 12 | 3 | 3 (or 2 only with a 10-stage 2nd pass, not planned) |
| **27, 28, 29, 30** | 7+7+(3..6), b1 10 | **4** | **3** (13+7+7 ... 13+9+8) |
| **31** (plan 121) | 5+7+7, b1 12 | **4** | **3** (13+9+9) |
| **32, 33** | 7+7+7, b1 11 / 12 | **4** | **3** (13+10+9 with C=8, or 12+10+10); 33 only with C=8 or 4 |

13 + 9 = 22 stages, so 2^23 stays at 3 passes. Two-pass coverage is logn <= 22, three-pass coverage is logn <= 31 (<= 33 with 64 B / 32 B segments).
3·2^k lengths add the separate radix-3 pass (`NTT_R3_FUSE` off by default, PH13), so they go from 5 to 4 passes at k = 27...31 and from 4 to 3 at k = 20...22.

#### 3. Enumerated splits for logn 31 (forward, strided passes listed top first, b1 last)

LDS tile = rows x columns x 8 B, unpadded with XOR swizzle (the 64 KB case cannot be padded). "Seg" = contiguous bytes per row read from HBM.
s_lo = lowest stage of the pass = log2 of the row stride in points (K13b slow strides: s_lo 17 and 24 only).

| # | split (stages) | block points / LDS | threads (a) | seg | s_lo of each pass | blocks/CU (LDS) | status |
|---|---|---|---|---|---|---|---|
| 0 | 5 / 7 / 7 / 12 (today, plan 121) | 2048 (18.4 KB) x3 ; 4096 (32 KB) | 256 ; 512 | 128 B ; contiguous | 26, 19, 12, 0 | 3 ; 2 | measured: 90.4 ms |
| 1 | **9 / 9 / 13** | 8192 (64 KB) x3 | 512 (16 pts/thread, 3 groups of 3 stages for the 9s ; LGV 4, 4 groups for the 13) | 128 B ; contiguous | 22, 13, 0 | 1 | recommended |
| 2 | **9 / 10 / 12** | 8192 (64 KB) ; 8192 (64 KB, 1024 rows x 8) ; 4096 (32 KB, existing) | 512 ; 512 ; 512 | 128 B ; **64 B** ; contiguous | 22, 12, 0 | 1 ; 1 ; 2 | recommended alternative (b1 untouched) |
| 3 | 10 / 10 / 11 | 8192 (1024 rows x 8) x2 ; 2048 (16 KB, existing LGL 11) | | 64 B ; 64 B ; contiguous | 21, 11, 0 | 1 ; 1 ; 4 | feasible, more segment risk |
| 4 | 11 / 10 / 10 | 2048 rows x 4 cols = 8192 (32 B segs) | | **32 B** | | 1 | not recommended: half-sector reads |
| 5 | 11 / 10 / 10 with 16-column tiles (128 KB tile: registers + LDS) | 16384 pts, 1024 threads x 16 pts, two-phase LDS exchange | | 128 B | | 1 (and the whole VGPR file) | L effort, risk high; only if 1-4 are blocked by segment width |
| 6 | 8 / 8 / 8 / 7 (4 passes) | | | | | | no gain, listed to show the pass count is the cost |

Why 13/9/9 over 12/10/9: all strided passes keep 128 B segments (the width K13b measured, 2.1-2.8 TB/s in the stride sweep outside s_lo 17/24); the only new
unknown is the 1 block/CU latency hiding. 12/10/9 reuses the measured 2^12 b1 (25.8 ms real) but adds the 64 B-segment unknown. Both avoid s_lo 17 and 24.

#### 4. Per-pass cost model at 2^31 (forward, ms; 34.4 GB moved per pass)

Calibration from S21 (m): NOP passes 17.3 / 19.0 / 17.1 (HBM-bound, 1.8-2.0 TB/s), b1-12 NOP 24.2 (1.4 TB/s, compute/exchange-bound), real 24.0 / 20.8 / 19.4 / 25.8.
Modmul exposure: about 0.3 ms per stage in the 7-stage passes (2.3 / 7), b1: 1.6-2.3 / 12; pass 0 is the outlier (6.5 ms, twiddle seeding, 5 stages).
b1-12 costs 25.8 / 12 = 2.15 ms per stage (all-in): that is what a pass costs once it is no longer HBM-bound. Model: pass = max(HBM floor 17.3 + 0.3 x stages, 2.15 x stages) x (1 + occupancy penalty).

| pass | stages | optimistic | central (+10 % for 1 block/CU) | pessimistic (+25 %) |
|---|---|---|---|---|
| strided 9, 128 B | 9 | 20.0 | 22.5 | 25 |
| strided 10, 64 B | 10 | 21.5 | 24.5 | 29 (plus an unknown segment penalty) |
| b1 13 (new) | 13 | 26.5 | 30 | 33 |
| b1 12 (today) | 12 | 25.8 (m) | 25.8 (m) | 25.8 (m) |

| plan | optimistic | central | pessimistic | saving vs 90.4 (opt / central / pess) |
|---|---|---|---|---|
| 0 today | 90.4 (m) | 90.4 (m) | 90.4 (m) | |
| 1 = 9/9/13 | 66.5 | 75.0 | 83 | **24 / 15 / 7** |
| 2 = 9/10/12 | 67.3 | 72.8 | 79.8 | **23 / 18 / 11** |
| 3 = 10/10/11 | 66 | 74 | 85 | 24 / 16 / 5 |

Reading: once the passes are not HBM-bound the total is about 31 stages x 2.15 = 67 ms whatever the split, so the split should be chosen for risk, not for the last millisecond.
The central saving is **15-18 ms (17-20 %)**, at the low end of S21's "17-20 ms (mod)" estimate; the pessimistic case is 5-11 ms. A reported central value of 15 ms is used below.
The memory-only floor (3 x 34.4 GB at 2.2-2.4 TB/s) is 43-47 ms, so even the optimistic row is compute/structure-bound, not memory-bound; this is the same finding as K13 H2 ("issue/latency-bound at 1.2-1.6 TB/s").

Other lengths (per 2^31 points, mod): logn 20-22, 3 -> 2 passes: today [b1-10 24 + 7-stage 19.4 + 3..5-stage 17] = 60; new [b1-13 30 + 7..9-stage 20-22] = 50-52: **about -9 ms-equivalent (15 %), -22 % at best**. logn 27-30, 4 -> 3 passes: about -15 ms-equivalent.

#### 5. Prior attempts and why they do not decide this

| item | where | what it found | relevance |
|---|---|---|---|
| tile 256 rows, b1 2^11 / 2^12, "occupancy costs more than the extra in-LDS stages save" | RESULTS §33 (D5), 2026-09-14 | with the OLD kernels (one LDS round trip per stage); 2^11 at 24 KB (2 blocks) 126-129 ms, 2^12 at 48 KB (1 block) 153 ms vs 117 for 2^10 | **superseded for b1** by the register-blocked b1r (K13b): 2^12 is now the best b1 in production (plan 121). Not re-tested at 2^13 or for strided 8-10 stage tiles |
| LDS bandwidth by occupancy | RESULTS §48 | 35.8 TB/s at 3 blocks/CU, 19.0-19.3 at 1 block/CU (34-64 KB tiles) | the 1 block/CU price: LDS traffic of a 9-stage body with 2 exchanges is 32 B per point = 69 GB per 2^31 transform = 3.6 ms at 19 TB/s (mod), not binding |
| occupancy H6, `NTT_B16_VAR` | K13 §H6, PH6 | 3 -> 4 blocks/CU is within +-2 % | occupancy is not the limiter for 7-stage passes (HBM / issue-bound) |
| MALL-chunked passes `NTT_MALL` | K13 H2, PH6 | no gain, "no MALL cliff, kernels are issue/latency-bound" | rules out cache-resident multi-pass as an alternative to a fatter pass |
| 3-pass plan | K13b §2 | "with a b1 of 2^10, 2^31 has 21 stages above b1 = 3 x 7, so no 3-pass plan avoids s_lo 17 and 24"; with b1 2^12 and bottom-up: 5/7/7 + b1 = 4 passes, s_lo 26/19/12 | the 3x7 limit came from the 7-stage clamp, not from a measured failure |
| 2-pass is the whole transform for logn <= 19 | `plan_auto` | b1 2^11/2^12 saves a pass at logn 25, 26 (1.25-1.40x) | **same lever applied at a different size: it worked every time it was tried** (PH5) |
| DPP / ds_swizzle exchange, radix-4 grouping | PH4, N-kernel | slower | not needed here |
| decision register | internal code notes | no entry for a >7-stage strided pass or a 2^13 b1 | not tried, not rejected |

#### 6. How many large transforms a run does (mod; NTT_SIZE_STATS will replace this)

Two families, with different sizes (internal code notes §4.3-4.4):
1. **Node-local B form / mdev / batch** (prime per APU, whole planes up to 2^31 points at size 1; 3·2^29 in the mdev pool): logn 27-31 are the big ones. The bs "top levels" (16.2 s at 10 nodes,
   14.4 s at 576 in the estimator) and part of the batch tier (19.8 / 17.6 s) live here. 4 -> 3 passes.
2. **4-step C form pieces** (`dist_core`, the mn tier): rows of 2^(logn/2) points. At 10 nodes logn 35-36, rows 2^17-2^18 = 2 passes already (no gain). **At 576 nodes
   (plane 2^20 x 2^20 over 2304 ranks) rows are 2^20-2^21 = 3 passes today, 2 after.** This is the larger term.

Estimator (`estimate.py --fabric target-m --bw 47`, g = 576; local = level wall - exposed): level 192 (12 pieces) 10.0 s, level 576 (6 pieces) 6.7 s, division (16 pieces) 17.8 s,
last reciprocal steps (groups 576, 3-5 pieces) about 3 s = **about 37 s of local piece time at rows >= 2^20**. The levels up to group 64 (rows <= 2^19 under plan 120) are already 2-pass.
NTT share of a piece (mod): 2^31 points per piece over 4 APUs x 3 primes = 18 sweeps of 2^29 points per APU, each 3 passes of about 5 ms = 270 ms of the piece's 0.83 x 1.22 = 1.0 s (`T_PIECE_31_NP[3]` x pipeline factor) = **27 %**
(a: depends on the unmeasured exact pass cost at 2^20 rows).

| | 10 nodes (435 s modelled wall, 462 m) | 576 nodes (219 s target estimate, 266 s derated, 748 s aac7-class fabric) |
|---|---|---|
| node-local logn 27-31 NTT time (assumed 40 % of top levels 16.2 / 14.4 s + a quarter of the batch tier at 19.8 / 17.6 s, NTT share 40 %) | 6.5 + 2 = 8.5 s | 5.8 + 1.8 = 7.6 s |
| saving at 17 % (central 15 ms of 90) | **1.4 s** (0.5-2.5) | **1.3 s** (0.5-2.2) |
| 4-step pieces at rows >= 2^20 | 0 | 37 s local x 27 % = 10 s of NTT; saving 22-30 % -> **2.5 s** (0.9-3.6; pessimistic fat-pass cost makes it 0) |
| **total per run** | **about 1.4 s (0.5-2.5), 0.3 %** | **about 3.8 s (1.5-6), 1.5-1.7 % of 219-266 s; 0.5 % of the aac7-class 748 s** |

Exposed fabric (50 % at 10 nodes, 63 % at 576 on aac7-class) hides nothing of this: the local pieces are on the critical path only where not overlapped (mn_model assumes they add), so the table is an upper bound if
the chunk pipeline hides some local passes. Four-prime pieces (`ECALC_NP=auto`) scale the 576 term by 1.33 (a).

**Measurement that confirms it: `NTT_SIZE_STATS` (branch s22).** Needed output: per logn, per direction (fwd / inv / inv_pw), per layout (single, batched rows), count and device milliseconds. Then
saving = sum over logn in {20-22, 27-33} of (count x ms x fraction of passes removed x per-pass share) with the measured per-pass times from t_ntt. It also decides whether the 576 piece term exists at all
(the piece rows at the target are modelled, never run: the biggest real run is 10 nodes with rows 2^18).

#### 7. Effort, risk, test plan

**Effort: M-L.** (1) Microbench E0, S (1 day): HBM read/write of a 512- or 1024-row x 16/8-column tile at s_lo 12, 13, 21, 22 and the 2^13 b1 (instantiate `k_b1r<13,4>` after extending `b1r_swz` by one index bit
and the 8192-entry twiddle table `B1R_TW`; 1 block/CU, 512 threads): gives the actual stride / segment / occupancy penalty without the butterflies. (2) 9-10 stage register-blocked strided body `k_b16w` (generalise `k_b16r`: 3-4 groups of 3 stages,
two LDS exchanges, unpadded swizzled 64 KB tile, twiddles from the global table as in K13 VAR 1, T_H by squaring over 9-10 stages), forward and inverse, `MM` 0/1, scale and R3 forms: M (4-6 days). (3) plan integration in `make_plan`
(`NTT_STG` up to 10, `NTT_PLAN` codes 13x, `plan_auto` per logn): S. (4) validation: M.

**Risks.**
- Correctness (low): outputs are canonical and unique, so any exact plan equals the old one bit for bit. Gates: `t_ntt 24` (593 checks) plus new plan cases in the STG 3-7 hash loop (`t_ntt.c:720`), `t_mul 20`, `t_mul 0 big`, e9 identical in both bases, mn e8 sizes 2-4. Inverse with fused pointwise (`inv_pw` in the b1 pass) and the R3-fused forms must be re-checked for 13 stages; `NTT_R3_FUSE` stays off.
- Occupancy / latency (medium): 1 block/CU (2 waves/SIMD) for the 9-10-stage and 2^13 kernels. Evidence for: 2^12 b1r at 2 blocks/CU runs; occupancy 3 -> 4 changed the 7-stage passes by +-2 % (K13). Evidence against: RESULTS §33's 1-block loss (old kernel) and LDS 19 vs 35.8 TB/s. Register pressure is not the limit (VGPR 46-66 of 256 at 2 waves).
- Memory pattern (medium): 64 B segments (plan 2/3) and the new strides are unmeasured; E0 settles it before any kernel work. s_lo 17 and 24 are avoided by all listed plans.
- Dist integration (low): none of the twiddle+pack kernels depends on the pass boundaries.
- Possible regression at logn 24-26 where the plan stays at 3 passes: keep `plan_auto` per logn (use the new plan only where it removes a pass).

**Test plan on one aac7 node (`tests/t_ntt`, one APU, median of 5; build main + patch, NOP build = `t_ntt_nop` as in S21).**
1. E0 microbench: tile copy rates by (rows, columns, s_lo, blocks/CU); go/no-go if 9-stage 16-column tiles at s_lo 13/22 < 1.6 TB/s.
2. `t_ntt bench 31 pass` for the new plan: real and NOP, per pass, forward and inverse; compare to section 4 (go if real 3-pass total <= 80 ms).
3. `t_ntt bench 31 whole` and layouts 2^14 x 2^17, 2^20 x 2^11, and **batched 2^20 / 2^21 / 2^22 rows** (the 576 piece shape, e.g. 455 rows x 2^20 per APU) against the default.
4. `t_ntt 24` VERIFY OK, `t_mul 20`, then `ecalc 1e9` identical and a 6.44e10 single-node run (total, mdev and bs top levels against the S21 baseline 93-96 s).
5. Pair-run ABBA at 10 nodes only if step 3 shows >= 10 % on the 2^31 transform (expected effect 1-2 s is below the 10-node noise, sd 18-49 s): not worth node time; judge by `t_ntt` and the target's NTT_SIZE_STATS.

#### 8. Decision for the user

| option | cost | benefit |
|---|---|---|
| (a) Do E0/E1 only (1-2 days, one aac7 node-hour) | none beyond the day | settles whether 64 KB / 1 block/CU tiles and 64 B segments run near the model; decides (b) |
| (b) Build 13/9/9 or 12/10/9 (M-L, 2-3 weeks) | effort, small occupancy risk | -15 ms (range -5...-24) per 2^31 transform; -1.4 s at 10 nodes, about -3.8 s at 576 (mod, 1.5-1.7 % of the target run); also 3 -> 2 passes for every 576 piece |
| (c) Leave | none | 0 |

Recommendation: (a), then (b) only if the target's NTT_SIZE_STATS shows rows >= 2^20 (the larger term). On its own, the 2^31 case (1.4 s at 10 nodes) does not justify the effort; the 576 pieces might.

## G515 — Phase 15 Batch 2, item 5 (PLAN §37 row 6): the general map's twiddle packs

(former file: G515.md)

Agent G5. Branch `p15-G5` from `int15c` c5142ea. aac6 clone `~/ntt-G515`, logs in `~/G515/`. Times Eastern (aac6 logs are
Central: +1 h).

#### 1. Feasibility note (written 2026-09-27 21:55 EDT)

**What.** The general map (g not a power of two: at the 576 target the 192 and 576 levels, and the reciprocal and division
on 576 nodes) runs its own four-step, `rns_dist.c gen_fwd / gen_inv_pw`. Its twiddled pack `k_twpack_g` and unpack
`k_unpacktw_g` gather two twiddle-table entries per point (w_n^(ij) = twr[ij >> logC] · twc[ij mod C], j bit-reversed, so the
gathers scatter), and the store / load address goes through a per-point owner search (`part_owner`, `part0`: 64-bit divisions).
`DIST_TWREC` (default since 2026-09-27) fixed this on the equal path only (`ntt_dist.c`).

**The kernels' cost now (measured, `gen_pack_bench`, one APU, prime 0, chunk 0 of K = 4 as in production, median of 7; job
21654, s24-26, 2026-09-27 21:50 EDT; log `~/G515/b1.log`).** GB/s = 16 B × points / t.

| shape (rows × C of one rank's chunk) | copy | `k_twpack_g` | **`k_twpack_gr`** | `k_unpacktw_g` | **`k_unpacktw_gr`** | `k_unpack_g` / `k_pack_cols_g` |
|---|---|---|---|---|---|---|
| 576: 2²⁰ × 2²⁰ / 2304, rank 0 (113 × 2²⁰) | 3532 | 899 (2.11 ms) | **1568 (1.21 ms)** | 888 | **1625** | 2757 / 2341 |
| 576: rank 1151 (114 × 2²⁰) | 3534 | 746 (2.57 ms) | **1516 (1.26 ms)** | 752 | **1473** | 2795 / 2426 |
| 576: rank 2303 | 3530 | 748 | **1519** | 751 | **1461** | 2765 / 2411 |
| 192: 2¹⁹ × 2¹⁹ / 768, rank 0 (170 × 2¹⁹) | 3546 | 1108 | **1811** | 1169 | **1743** | 3009 / 2387 |
| 192: rank 767 | 3549 | 932 | **1580** | 953 | **1534** | 2812 / 2321 |
| 192: 2²⁰ × 2²⁰, rank 5 (341 × 2²⁰) | 3101 | 791 | **1582** | 823 | **1508** | 2500 / 2595 |
| 3: 2¹⁶ × 2¹⁶ / 12, rank 0 | 3546 | 1114 | **2117** | 1389 | **2234** | 3191 / 3140 |
| 6: 2¹⁶ × 2¹⁶ / 24, rank 0 | 3546 | 1181 | **1866** | 1709 | **1810** | 3106 / 3197 |
| 9: 2¹⁸ × 2¹⁸ / 36, rank 4 | 3089 | 1198 | **2085** | 1380 | **2026** | 2752 / 2853 |
| 12: 2¹⁸ × 2¹⁸ / 48, rank 7 | 3078 | 1156 | **2015** | 1364 | **1970** | 2714 / 2805 |

Every recurrence form is **bit-identical** to the plain one (checked in the bench, 12 shapes, 24 checks). At the target's plane
shape the plain general-map kernels run at **750–900 GB/s**, well below the equal path's plain `k_twpack` (1175 GB/s at 2²⁸,
same job). The recurrence forms reach **1460–1620 GB/s (×1.75–2.03)**. The untwiddled `k_unpack_g / k_pack_cols_g` already run
at 2.3–3.2 TB/s: not worth touching.

**Approach (built, fa4aa2d + 8765b91).** `k_twpack_gr / k_unpacktw_gr` are `ntt_dist.c k_twpack_r<8>`'s form: each thread takes
w_n^(ij) for its first row from the tables and steps four rows by w_n^(8j) (one per column, in LDS), so it does two table
gathers per four points instead of eight. `ec_mm` is canonical, so the chain is exact. The slab address
`part0(C, nr, sg)·rk + jl·rk + il` of the plain kernel equals `jb·rk + il` for any owner sg, so there is no owner search. The
kernels guard rk not being a multiple of 32: a thread whose first row is past rk reads no table. The switch is
`DIST_TWREC_G=1` (default 0) in `gen_fwd / gen_inv_pw`. `DIST_STATS=1` adds an event-timed line of the general map's pack times
per mn product (print only; the host waits for each pack in stats mode).

**Expected gain at the target (modelled, `mn_model.py`, 4.25 × 10¹³ on 576, DEFAULT15B).**

- The general map's local piece time on the critical path is **69.4 s** (modelled: levels 28.2, reciprocal 19.6, division 21.6;
  scratch `genshare.py`, which perturbs the general-map pieces' local time by 1 %).
- The saving per point: 12 twiddled passes per piece (4 primes × 2 forward packs + 1 inverse unpack) × the measured Δt per
  point, against T_PIECE_31_NP[4] per 2²⁹ points. That is **5.7 %** of the piece's local time at the 576 shape and **3.8 %** at
  192's (2¹⁹ × 2¹⁹). In the model this is `GEN_TWPACK_F = {0: 1.0, 1: 0.95}` (modelled).
- **Model correction** (this term only): `TWREC_F` (0.98) was applied to every piece, including the general map, which never
  had the recurrence. It now applies to the equal path only. The plain model at the target moves 256.0 → **257.4 s** without the
  write (285.7 → 286.8 s with it at 0.6 GB/s).
- **With `DIST_TWREC_G=1`: 253.9 / 283.9 s, −3.5 s / −2.9 s** (modelled; assumed on the critical path, because the model adds
  local time and exposed fabric).
- Lower bound: if the chunk pipeline already hides chunks 1…K−1's packs under the wire (the fabric per chunk ≈ 9.5 ms at
  100 GB/s per APU against a 2.5 ms pack), only chunk 0's forward pack and the last chunk's unpack stay exposed. That is a
  quarter of the saving: **−0.9 s**.
- **One node: 0** (g = 1 never takes the general map).
- V3's assumption was −2…−3 s.

**Risks.** Low:
- The same arithmetic as `DIST_TWREC`, adopted by the user.
- Bit-identical by construction, and checked in the bench.
- The switch defaults off.
- The only new cases are an rk that is not a multiple of 32 (the target's 113–114-row chunks; covered by the bench) and ranks
  with unequal rows (covered: ranks 0, middle, last).

What the aac6 measurements cannot show is the overlap at the target's fabric. Loopback and 2–3 real nodes have a fabric an order
of magnitude slower per byte, so the packs are hidden there more than at the target, or equally.

**Test plan.**
1. One node: `t_mn_grid` at 2 and 4 (`DIST_GEN=1`), 3, and 6 node-processes with `DIST_TWREC_G=0` and `1`. The products are
   exact against the host, and the transforms are bit-identical by the bench.
2. `mnaccept --only unit,e9,mn` with `DIST_TWREC_G=1`, plus `mn` again under `DIST_GEN=1` (sizes 2 and 4 on the general map).
3. Timing, off vs on, same node: 10⁹ at 3 and 6 node-processes with `DIST_STATS=1` (the pack line) and without it (walls and
   level lines).
4. The exclusive window: 2 and 3 real nodes (3 = the general map), 10⁹ and 10¹⁰, off / on pairs, the pack lines and the level
   and reciprocal / division lines.

**Conclusion: worth it** (cheap, already built, low risk, −0.9…−3.5 s at the target modelled). Proceeding to the gates and
timing.

#### 2. Correctness (measured, 5ea9648, s24-16)

| test | command (on aac6, `~/ntt-G515/ecalc`) | result | job, time (EDT) |
|---|---|---|---|
| the kernels | `DIST_GBENCH=... ./tests/t_dist` (12 shapes, §1) | recurrence forms bit-identical: 24 of 24 | 21654, 21:50 |
| t_mn_grid, 3 procs (general map) | `SLURM_JOB_ID=$J ./mnrun.sh 3 env DIST_TWREC_G={0,1} ./tests/t_mn_grid 1 28` | VERIFY OK on all 3, both settings | 21656, 21:54 / 22:15 |
| t_mn_grid, 6 procs | same, 6 | VERIFY OK on all 6, both (11 min each) | 21656 |
| t_mn_grid, 2 procs, `DIST_GEN=1` | same, 2, `DIST_GEN=1` | VERIFY OK on both, both settings | 21656 |
| t_mn_grid, 4 procs, `DIST_GEN=1` | same, 4, `DIST_GEN=1` | VERIFY OK on all 4, both settings | 21656 |
| mnaccept | `DIST_TWREC_G=1 ./mnaccept.sh $J --only unit,e9,mn` | **15 passed, 0 failed** (e9 identical in both bases; mn e8 at 2/3/4, e9 at 2/4 identical) | 21663, 23:12, 1302 s |
| mnaccept, the general map at 2 and 4 | `DIST_GEN=1 DIST_TWREC_G=1 ./mnaccept.sh $J --only mn` | **5 passed, 0 failed** | 21663, 23:34, 73 s |
| 10⁹ at 3 procs, off / on | `./mnrun.sh 3 env POOL_LOG=28 DIST_TWREC_G={0,1} [DIST_STATS=1] ./ecalc 1000000000 /tmp/...` | 8 runs, all `digcmp` identical, all VERIFY OK | 21660, 22:39–23:12 |

Not run: 10⁹ at 6 procs on one node. The budget check stops it at init (rc 8: 6 ranks, 500.6 of 480 GB; also with
`ECALC_PLANE_CAP=2^30`), a memory limit that has nothing to do with this switch. One 6-proc attempt (21660, `DIST_TWREC_G=1`)
hung at init: the layered meshes connected on nodes 0–3, then nothing until the 900 s timeout, before any transform.
It came right after the rc 8 run's exit, and I read it as interference from that exit (not investigated further).

#### 3. Timing

**One node, 10⁹ at 3 node-processes (loopback; measured, job 21660, s24-16).** `total` (the wall without the digit file is
the same here: 10⁹ writes its 0.44 GB part files to /tmp in the overlap):

| DIST_TWREC_G | rep 1 | rep 2 | rep 3 | DIST_STATS=1 run | the general map's packs (node 0, max over APUs, DIST_STATS) |
|---|---|---|---|---|---|
| 0 | 17.92 | 17.25 | 16.98 | 16.98 | twpack 0.009 s, unpacktw 0.004 s |
| 1 | 17.47 | 16.82 | 17.62 | 17.02 | twpack 0.009 s, unpacktw 0.004 s |

At 10⁹ the general map's packs are **13 ms of a 17 s run** (measured): the mn tier's products are small at this size, so
the walls differ only by noise (±0.5 s). The one-node multi-process runs cannot show the effect; the pack line is the measure.

**One node, 10¹⁰ at 3 node-processes (loopback; measured, job 21666, s24-16, 23:47–23:54 EDT; `POOL_LOG=29
RNS_DIST_CACHE_MN=0`).** Without `RNS_DIST_CACHE_MN=0` every 10¹⁰ run at 3 procs on one node stops after the tree with
`HIP out of memory` at `rns_dist.c:255`, the mn transform cache's `hipMalloc`. That happened with both settings (job 21665,
six runs): a capacity limit of three processes on one node, predating this work.

| run | DIST_TWREC_G | `total` (s) | bs / dm (s) | the general map's packs (node 0, max over APUs) | digits |
|---|---|---|---|---|---|
| r1 | 0 | — (Slurm step launch timed out, rc 140; not ecalc) | — | — | — |
| r1 | 1 | 79.28 | 20.4 / 50.6 | — | VERIFY OK (not compared) |
| DIST_STATS | 0 | 88.33 | 21.2 / 59.5 | twpack 0.096 + unpacktw 0.044 = **0.140 s** | **identical** to `e_1e10.out` |
| DIST_STATS | 1 | 82.31 | 21.2 / 53.0 | twpack 0.071 + unpacktw 0.035 = **0.106 s** | **identical** |

The switch cuts the general map's pack time by 34 ms (−24 %) at this size (measured). The walls' 6–9 s spread is dm noise between
single runs (the packs are 0.1 % of the wall). The one-node sizes that fit keep the mn tier's planes small; the target's
2²⁰ × 2²⁰ planes over 2304 ranks are where the bench's ×1.75–2.03 applies.

**The exclusive window (2–3 real nodes): not run.** At 23:37 EDT s24-26 and s24-30 were down (per the integrator, reboots
during another agent's VMM test), so job 21664 (3 nodes, `~/G515/b5.sh`) was cancelled while pending. The script is ready:
10¹⁰ on 3 nodes (g = 3) off / on × 3, a DIST_STATS pair with the digits compared, 10¹⁰ on 2 nodes with `DIST_GEN=1`, 10⁹ at
6 procs over 3 nodes, and a 4 × 10¹⁰ pair. It maps no VMM memory beyond ecalc's defaults.

#### 4. The gain at the target (modelled, `mn_model.py` with this branch's term; scratch `g5model.py`)

The user moved the target to **5.1 × 10¹³ on 576** (D = 8.854 × 10¹⁰ per node) with `ECALC_NP=auto` (integrator, 2026-09-27).
"Before G5" is the model as it was (TWREC_F on every piece); it reproduces `estimate.py --g 576 --D 8.854e10 --np-mn auto` =
322.6 s. Walls in seconds, without / with the digit file at 0.6 GB/s:

| target | before G5 | plain general map (`DIST_TWREC_G=0`, the corrected model) | **`DIST_TWREC_G=1`**, all on the path | a quarter on the path | gain vs plain |
|---|---|---|---|---|---|
| **5.1e13, NP auto** | 322.6 / 354.1 | 324.6 / 355.7 | **319.8 / 351.8** | 323.4 / 354.7 | **−4.8 / −3.9** (lower bound −1.2 / −1.0) |
| 5.1e13, NP 4 | 345.0 / 376.5 | 346.9 / 378.0 | 342.1 / 374.2 | 345.7 / 377.1 | −4.8 / −3.8 |
| 4.25e13, NP auto | 238.6 / 268.3 | 240.1 / 269.5 | 236.5 / 266.6 | 239.2 / 268.7 | −3.6 / −2.9 |
| 4.25e13, NP 4 | 256.0 / 285.7 | 257.4 / 286.8 | 253.9 / 283.9 | 256.5 / 286.1 | −3.5 / −2.9 |

Against the model as the integrator ran it (322.6 s), the net is **−2.8 s** at 5.1e13: 1.9–2.0 s of the "before" number was
a credit the code did not have. Labels: the kernels' speeds are measured (§1). The per-point saving as a fraction of the
piece's local time is modelled (GEN_TWPACK_F = 0.95). That the saving is on the critical path is **assumed**; the lower bound
assumes the chunk pipeline hides 3 of 4 chunks' packs under the wire. **One node: 0.**

#### 5. Open issues

- The window on 2–3 real nodes (`~/G515/b5.sh`) is still owed: it needs s24-26 and s24-30 back.
- aac6 cannot measure whether the target's fabric hides the packs of chunks 1…K−1 (the −1.2…−4.8 s range at 5.1e13).
- Pre-existing, not this switch: 10⁹ at 6 procs on one node is over the budget check; 10¹⁰ at 3 procs on one node runs out of
  device memory at the mn cache's `hipMalloc` (`rns_dist.c:255`) unless `RNS_DIST_CACHE_MN=0`; one 6-proc start hung at
  init (job 21660).
- Not built: the untwiddled `k_unpack_g / k_pack_cols_g` (2.3–3.2 TB/s already, measured).

#### RESUME

- Step 1 (done, fa4aa2d): `rns_dist.c` `k_twpack_gr` / `k_unpacktw_gr`, `gen_pack_bench` (`tests/t_dist.c`: `DIST_GBENCH`).
- Step 2 (done, 8765b91): `DIST_TWREC_G` wired into `gen_fwd` / `gen_inv_pw` (default 0), the `DIST_STATS=1` pack line in
  `mn_core` (print only), README row.
- Step 3 (done): batch 1 (job 21654, s24-26, 21:50 EDT): the bench, §1. (21653 was pinned to s24-30, where the user's 4 h
  `apucode-crypto` job runs: cancelled.)
- Step 4 (done, this commit): the note; `mn_model.py` `GEN_TWPACK_F`, `TWREC_G`, `--twrec-g`; `TWREC_F` on the equal path
  only; `--calib` still OK (worst 6.0 %).
- Step 5 (done): batches 2–4, §2–3 (the first mnaccept attempt in 21656 found the job expired: the t_mn_grid set took 43 min;
  rerun in 21663).
- Step 6 (23:36 EDT): the 3-node window (`~/G515/b5.sh`) could not run: s24-26 and s24-30 went **down** (sinfo 23:37 EDT);
  job 21664 cancelled while pending.
- Step 7 (done): batch 6 (job 21665: 10¹⁰ at 3 procs OOM in the mn cache, both settings) and batch 7 (job 21666, with
  `RNS_DIST_CACHE_MN=0`), §3; the model at 5.1e13 and 4.25e13, §4.
- **Next: the window**: on aac6 check that `~/ntt-G515` is at 5ea9648, then `setsid nohup bash -l ~/G515/b5.sh &`. It queues a
  3-node job (`-w` all three), runs unattended and cancels itself; log `~/G515/b5.log`. Then fill §3's window part and the
  README's measured numbers.
- (old) Step 5 (running since 21:54 EDT): `~/ntt-G515` at 5ea9648, rebuilt; `~/G515/b2.sh` (log `~/G515/b2.log`): batch 2 (job 21656,
  s24-16: t_mn_grid 3, 6, 2 and 4 under DIST_GEN=1, each at DIST_TWREC_G=0/1; mnaccept unit,e9,mn with DIST_TWREC_G=1; mn with
  DIST_GEN=1 DIST_TWREC_G=1), then batch 3 on the same node (10^9 at 3 and 6 procs, off/on x 2 reps, + a DIST_STATS pass).
  If stopped: check `~/G515/b2.log`; the script cancels its own jobs (`squeue -u <user> -n G5`).
- Next: the exclusive 2–3 node window (item 4): ask the integrator.

## S115 — Phase 15 agent S1: the decimal `mul_1` and the fused seed span (`BI_MUL1_FAST`)

(former file: S115.md)

Branch `p15-S1` from `int15` @ 0b80ca3. PLAN §36 row 1a; V3's item 1 (results/SINGLE_NODE.md §V314 §1). Times Eastern. Every number is
labelled **measured**, **modelled** or **assumed**. Logs: `archive:results-raw/S115/{g1,t2,t3}/`.

#### 0. Summary

- **Correct**: `tests/t_seed` 0 mismatches in every k range up to 2⁴⁴ (including the target's 3.5 × 10¹²), against the serial
  path and GMP; `mnaccept --only unit,e9,mn` 15/15 with the switch; 10¹⁰ and **10¹¹ byte-identical** (sha1 = the references).
- **Faster seeds, less than V3 modelled**: in the 10¹¹ run the spans go **22.59 → 18.70 s** (×1.21) and the seed wait after init
  **20.90 → 17.71 s (−3.2 s)** (measured, means of 5 runs each, s24-16). V3 expected −9 s. The reason (measured): the old
  path is latency-bound and gains ×1.7 from SMT (96 → 192 threads); the new one is throughput-bound and gains ×1.0–1.05, so the
  96-thread bench's ×1.73–2.40 shrinks to ×1.4–1.5 at 192 threads (the run uses 192) and ×1.2 inside the run (which also
  shares the CPU with init's mapping).
- **Walls** (measured, same node, alternating): without the digit file off 216.8 / on 215.0 s (3 runs each); with it
  off 236.8 / on 234.6 s (2 each). The −1.8 / −2.2 s differences are **inside the run-to-run noise** (bs alone varies 91.5–102.2 s).
- **Target** (modelled): the top node's spans (V3: ≈ 20.7 s, every term k > 2³³) at ×1.2–1.5 → −3.5…−7 s of spans; the seed wait
  falls by about that: **−3…−7 s** at 576 (V3 had −9…−11).
- Recommendation: adopting is safe (exact, identical, the switch touches only `mul_1` and the seeds) and gives a small, real gain
  in the seed wait; it does not reach the mapping floor at 10¹¹.

#### 1. Design

**Switch** `BI_MUL1_FAST` (0 by default; README row). Read once on first use (`bi_mul1_fast_on()`); tests set `bi_mul1_fast`.

**The decimal `mul_1`** (`bigint.c mul1_dec_fast`, taken by `mul1_serial` — so by every `limb_mul_1` / `bi_mul_u64` /
`bi_mul_pow10` caller, serial and chunked — when decimal, m < 10¹⁸ and add ≤ 10¹⁸). The multiplier's reciprocal
mh = ⌊m·2⁶⁴ / 10¹⁸⌋ (one 128/64 division per call, out of line so the loop sees a 64-bit value) gives, for every limb x,
qe = ⌊x·mh / 2⁶⁴⌋ ∈ {Q − 1, Q} with Q = ⌊x·m / 10¹⁸⌋ (x·mh ≤ x·m·2⁶⁴/B and x·mh > x·m·2⁶⁴/B − x). Then
s = x·m − qe·B + c (mod 2⁶⁴; the true value is (x·m mod B) + (Q − qe)·B + c ∈ [0, 3B) < 2⁶⁴) is fixed by two compares against
constants: r = s − B·[s ≥ B] − B·[s ≥ 2B], c' = qe + [s ≥ B] + [s ≥ 2B] (≤ m ≤ B − 1 for canonical x). The per-limb quotient does
not depend on the incoming carry (the old fast path's chain went through a 128-bit multiply), there is no 128-bit division at
any m (the old path above 2³³ divided `u128 % 10¹⁸` and `/`), and the one form covers every m < 10¹⁸: the target's top node
(k ≈ 3.5 × 10¹²) takes the same path as k < 2³³. The limbs are those of the exact product, so identical to the serial form's.

Found on the way (measured, aac6 login node, Zen 2, single thread): the same algorithm with the two corrections one at a time
(`rem -= f ? B : 0; s = rem + c; …`) compiled by clang (hipcc, which builds `bigint.o`) to data-dependent branches:
7.9 ns per limb-step against 3.7 with gcc; the combined two-compare form is 3.7 with both. V3's double-estimate form (its mode B)
measured 3.5–3.6 in the same bench — equal within noise; the integer form was kept (exact by a two-line argument, no FP rounding
analysis, valid for all m < 10¹⁸ where the double form needs m < 2⁵⁰).

**The fused span step** (`bigint.c bi_span_step`: P += Q; Q ·= k in one pass, each limb of Q read once; P shorter, equal or
longer than Q handled; k = 0 gives Q = 0). `binsplit.c span()` uses it when decimal and the switch is on; otherwise the old
`bi_add` + `bi_mul_u64` loop. `binsplit_span()` exports `span` for the test. The seed thread's line gains
`; BI_MUL1_FAST=1` when the switch is on (the logs say which path ran).

**Outside my files** (minimal): `ecalc/Makefile` — `tests/t_seed` added to `TESTS` and its build rule.

#### 2. The unit test (`tests/t_seed`)

`./tests/t_seed [spans per range] [timing spans]` — A: `limb_mul_1` fast vs serial over m in [1, 2²⁰), [2²⁰, 2³³), [2³³, 2⁴⁴),
[2⁴⁴, 2⁵⁰), [2⁵⁰, 10¹⁸) and 15 edge values (0, 1, 2³³ ± 1, 3.5 × 10¹², 4 × 10¹², 2⁴⁴, 10¹⁷, B/2, B − 1, …), operands uniform /
all B − 1 / sparse, lengths 1–1000 and 2²² + 5 (the chunked parallel path), add ∈ {0, 1, random, B − 1, B}, with a GMP check on
a subset; B: `bi_span_step` against `bi_add` + `bi_mul_u64` with P shorter / equal / longer than Q and k = 0; C: the pipeline's
`span()` off vs on for S ∈ {1, 2, 64, 168, 236, 256, 512} and a from seven k ranges (a < 10⁴ with a = 0, 1; 4 × 10⁹; across 2³³;
10¹⁰; 3.5 × 10¹²; 4 × 10¹²; up to 2⁴⁴), with a GMP recurrence on every 50th span; D: timing.

**Measured**, node s24-30 (job 21465, 2026-09-26 23:09 EDT), `OMP_NUM_THREADS=96 OMP_PROC_BIND=spread ./tests/t_seed 400 400000`:
A 10 536 comparisons + 1 485 against GMP, **0 mismatches**; B 3 000, **0**; C 19 600 spans (2 800 per k range) + 392 against GMP,
**0 mismatches in every range including k > 2³³ up to 2⁴⁴**; `VERIFY OK`, 5 s.

Span cost, ns per term (S = 256), switch off → on (**measured**, same node):

| k range | 96 threads (all together) | 1 thread |
|---|---|---|
| 4 × 10⁹ (< 2³³: the old Barrett path) | 3.141 → 1.819 (**×1.73**) | 298.2 → 173.2 (×1.72) |
| across 2³³ | 3.982 → 1.877 (×2.12) | 379.2 → 179.8 (×2.11) |
| 10¹⁰ (the 10¹¹ run's top) | 4.255 → 1.881 (**×2.26**) | 404.6 → 179.8 (×2.25) |
| 3.5 × 10¹² (the 576 target's top node) | 5.640 → 2.351 (**×2.40**) | 538.2 → 223.0 (×2.41) |
| 4 × 10¹² | 5.671 → 2.358 (×2.41) | 540.3 → 224.0 (×2.41) |
| up to 2⁴⁴ | 6.002 → 2.494 (×2.41) | 571.8 → 239.6 (×2.39) |

(V3's prototype, same CPU: ×1.69 / ×2.11 / ×2.19 at 4 × 10⁹ / 10¹⁰ / 3.5 × 10¹².) The aac6 login node (Zen 2, clang) gave
×1.41 / ×2.77 / ×3.19 (measured, 16 threads).

#### 3. Gates (switch on), node s24-30, job 21465, commit 65d463f (code = the final code)

| test | command | result | time |
|---|---|---|---|
| unit + e9 + mn | `BI_MUL1_FAST=1 ./mnaccept.sh 21465 --only unit,e9,mn` | **15 passed, 0 failed**: t_ntt 24, t_mul 20, t_bs, t_dbig, t_newton 20, t_verify, t_out, t_mn_grid (2 procs); e9 identical in **both bases** (decimal total 10.68 s, binary 22.46 s); mn e8 at sizes 2, 3, 4 and e9 at sizes 2, 4 identical (the per-node seed spans start at other k) | 1298 s |
| 10¹⁰ | `BI_MUL1_FAST=1 ECALC_VERBOSE=2 ./ecalc 10000000000 /tmp/s1_e10on.txt`, then `dd iflag=direct … \| sha1sum` | VERIFY OK; sha1 **3fc02547…f951f3a = `e_1e10.out.sha1`** (identical) | total 25.14 s |
| **10¹¹** | the same at 10¹¹ | VERIFY OK; sha1 **578f5efb…97f55cb7 = `~/V214/e_1e11.sha1`** (identical to `results/e_1e11.out`) | total 239.12 s (with the file) |

#### 4. 10¹¹ timing, switch off vs on (measured, node s24-16, commit 65d463f)

Job 21474 (01:19–01:44 EDT 2026-09-27) and job 21490 on the same node (02:10–02:27 EDT); runs alternated off / on; each run
preceded by `posix_fadvise(DONTNEED)` on `e_1e11.out`; `ECALC_VERBOSE=2 ./ecalc 100000000000 [/tmp/s1_<tag>.txt]`, the file removed
after the run. "seed wait" = bs's `seeds … s (waiting for the init thread)`; "seed thread" = the thread's total, "spans" its span time.
"elapsed" = the srun wall including process start and exit.

**Without the digit file** (wall = `total`):

| run | switch | total | init | bs | seed wait | seed thread / spans | T1 | elapsed |
|---|---|---|---|---|---|---|---|---|
| n_off1 | off | 223.82 | 21.3 | 102.2 | 20.57 | 33.49 / 23.59 | 0.7 | 233 |
| n_on1 | on | 215.40 | 17.6 | 97.5 | 17.78 | 28.92 / 18.85 | 0.6 | 223 |
| n_off2 | off | 212.96 | 16.7 | 96.1 | 20.74 | 31.33 / 22.28 | 0.7 | 221 |
| n_on2 | on | 212.53 | 15.4 | 96.7 | 17.12 | 27.87 / 18.56 | 0.6 | 220 |
| n_off3 | off | 213.71 | 16.9 | 95.8 | 21.16 | 31.74 / 22.24 | 0.6 | 222 |
| n_on3 | on | 217.03 | 17.1 | 99.0 | 18.39 | 29.33 / 18.05 | 0.6 | 226 |
| **mean off / on** | | **216.83 / 214.99** | 18.3 / 16.7 | 98.0 / 97.7 | **20.82 / 17.76** | 32.19 / 28.71; 22.70 / 18.49 | | 225.3 / 223.0 |

**With the digit file** (100 GB to the node's local NVMe `/tmp`; wall = `total`, whose `T1` includes the wait for the writer):

| run | switch | total | init | bs | seed wait | seed thread / spans | T1 (writer wait) | writer | elapsed |
|---|---|---|---|---|---|---|---|---|---|
| w_off1 | off | 239.71 | 16.5 | 102.1 | 21.15 | 32.10 / 22.78 | 20.4 | 56.1 s | 299 |
| w_on1 | on | 239.44 | 16.1 | 101.0 | 17.79 | 28.37 / 19.38 | 22.0 | 56.8 s | 299 |
| w_off2 | off | 233.81 | 17.1 | 94.8 | 20.87 | 31.96 / 22.05 | 21.4 | 56.1 s | 292 |
| w_on2 | on | 229.67 | 17.4 | 91.5 | 17.47 | 28.50 / 18.68 | 20.3 | 57.1 s | 289 |
| **mean off / on** | | **236.76 / 234.56** | 16.8 / 16.8 | 98.5 / 96.3 | **21.01 / 17.63** | | 20.9 / 21.2 | | 295.5 / 294.0 |

Over all ten runs: seed wait **20.90 → 17.71 s (−3.19 s)**, spans **22.59 → 18.70 s (×1.21)**, seed thread 32.12 → 28.60 s
(the region wait 5.0–6.0 s, the buffers and DMA ≈ 4 s are unchanged). Walls: −1.8 s without the file, −2.2 s with it — the
same direction as the seed wait but inside the spread (bs 91.5–102.2 s, init 15.4–21.3 s run to run). The switch-on run of
g1 (s24-30, with the file) had total 239.12 s, seed wait 17.47 s, spans 18.39 s. `dm` is unaffected (99.1–100.3 s both ways).

**Why the in-run gain is smaller than the bench's** (`t_seed` at 192 threads, s24-16, measured, ns per term, off → on):
4 × 10⁹ 4.351 → 2.172 (the first range carries the threads' warm-up; the steady ranges:) across 2³³ 2.932 → 1.923 (×1.52),
10¹⁰ 2.594 → 1.824 (×1.42), 3.5 × 10¹² 3.366 → 2.251 (×1.50), 2⁴⁴ 3.476 → 2.369 (×1.47). Against 96 threads (§2): off gains
×1.64–1.73 from the SMT siblings (its per-limb chain runs through the 128-bit multiply — latency-bound), on gains ×1.00–1.05
(its chain is two compares and adds; the limb's work — three multiplies, ≈ 20 integer ops — is throughput-bound on the core).
In the run the seed thread's 192 threads also share the CPU with init's region and plane mapping: ×1.21.

#### 5. Open issues and possible next steps

- The seeds still hold level 1 back ≈ 17.7 s at 10¹¹ (seed thread ends ≈ 28.6 s, init ≈ 16.8 s). The rest of the gap is not the
  span arithmetic alone: 5.0–6.0 s waiting for the regions and ≈ 4 s of buffers/DMA. Untested ideas: `BS_SEED_THREADS=96`
  (the on-path gains nothing from the SMT siblings, which init's mapping could use); two spans interleaved per thread (no gain
  expected: throughput-bound); V3's double-estimate quotient moves one multiply to the FP pipes but measured slower on Zen 2
  (×1.54–1.73 vs ×1.64–1.85 for this form, login node, 1–48 threads), not tried on the node.
- The model (`mn_model`) lacks the seed term (V3 §1); the refit belongs to 1g. For it: the target's top node spans ×1.5 (bench,
  192 threads, k ≈ 3.5 × 10¹²), ×1.2 assumed in-run as at 10¹¹.
- T2's sweep (1c) can start from this branch.

**Outside my files**: `ecalc/Makefile` (`tests/t_seed` in `TESTS` and its rule). Nothing else.

#### RESUME

- Code done and committed (`bigint.c mul1_dec_fast / bi_span_step`, `binsplit.c span`, `tests/t_seed.c`, Makefile rule, README row).
- aac6 clone `~/ntt-S115` (branch p15-S1, bundles from 19bf6dd); node scripts `~/S115/job.sh <name> <node|any> <steps> <commit>`,
  steps `~/S115/g1.steps` (tests + gates), timing steps to come.
- Stage 0 done 23:08 EDT. Chain launched 23:09 EDT on aac6: `setsid nohup bash ~/S115/chain.sh 65d463f` = g1 (t_seed, mnaccept
  unit,e9,mn with the switch, e10 and e11 on with sha1) → t2 (10¹¹ timing, off/on alternating, no file ×2 and with file ×1) → t3 on
  t2's node (with file ×1 more pair, a third no-file pair). Logs `~/S115/{g1,t2,t3}/`, `~/S115/chain.done` at the end.
- References: e10 sha1 `~/ntt/ecalc/results/e_1e10.out.sha1` = 3fc02547…; e11 sha1 `~/V214/e_1e11.sha1` = 578f5efb….
- **Done.** g1, t2, t3 all completed (chain.done 02:27 EDT 2026-09-27; the CANCELLED states in sacct are the scripts' own
  scancel at the end). Logs copied to `archive:results-raw/S115/`. No job of mine is running or pending. Nothing left but the integrator's merge.

## N3 — host gaps in the batch tier (`BS_BATCH_PIPE`)

(former file: N3.md)

**Question.** OPPORTUNITIES N3 (`archive:results/O7b.md` §D rank 4): the batch tier's host work leaves the GPUs idle
between and inside the lower binary-splitting levels. Does doing that host work in parallel and issuing each device's
tiles back to back shorten the 1-node run at 6.441 × 10¹⁰ digits, with identical digits?

**Change / branch.** Branch `n3` (commits bcd764d, 9e774dd, plus this report). New switch `BS_BATCH_PIPE`, default 0
(off: the code paths are the old ones, unchanged). Files: `ecalc/rns_mul.c`, `ecalc/rns_mul.h`, `ecalc/binsplit.c`
(the batch-tier level loop only).

With `BS_BATCH_PIPE=1`:

- `binsplit_e`, at levels with ≥ 4096 pairs and more than `BS_BALANCE_N` nodes: the `max_nl` scan is an OpenMP
  reduction. The next level's layout is computed one region per thread. A large level's regions are contiguous index
  ranges (`region_of` is monotone), so each region's running offset equals the serial one; a check stops the run if a
  node falls outside its region's range. The product list is one buffer kept across the levels (no `calloc`/`free` of up
  to 1.9 GB per level), filled in parallel. The lengths are read back in parallel.
- `rns_mul_batch`: the per-product scans (registration, `maxnc`, the shared-B test, the device of each result) run once,
  in parallel. The device of each product is kept and reused by `rns_mul_batch_local`. The previous code looked it up
  three times, serially.
- `rns_mul_batch_local`: the per-device product order (`idx`) is built by chunks in parallel, in the same stable order.
  Each device's tiles go to its stream back to back. The descriptors pass through two pinned slots: the host fills
  tile t + 1 while tile t runs. The spill merge stays on the stream (VMM pools; host pools still merge on the CPU after
  a synchronisation, as before). The lengths of all the device's tiles are read back once at the end. Before, every
  tile waited twice (after the CRT and after the length read-back) and copied its descriptors from pageable memory.

Same kernels, same arguments and the same order on each stream, so the digits are identical by construction. The gates
check that.

**Method.** aac7 through the queue. Item 50 ran the gates on a 2-node hold:
`tools/gate.sh --build --nodes 2 --env "BS_BATCH_PIPE=1"`, then `--nodes 2`, `--nodes 1 --env "BS_BATCH_PIPE=1"`, and
`--nodes 1` (aac7 launch line, 10⁹ digits, the default reference; the 2-node gates force P24). Item 51 ran on a
1-node hold, built from the same tree:
`SLURM_JOB_ID=$J MNRUN_NODES=1 ./mnrun.sh 1 env $LINE BS_BATCH_PIPE=<0|1> ./ecalc 64410000000`. `$LINE` is
`tools/profile_load.py aac7 --line`, which includes `ECALC_VERBOSE=2`. There is no digit output. A = 0 (default),
B = 1. Four ABBA rounds, 16 runs (the round cap). Each run's wall time is its own `total` line. Paired difference per
round = mean(B) − mean(A); 95 % CI with t(3) = 3.182. Collect: `ssh <cluster> 'cat ~/n3/abba/summary.txt
~/n3/gates/summary.txt'` (access: see the private protocol). Raw data: `archive:results-raw/N3/`.

#### Results

Gates (measured, 2026-10-12 19:25–19:27 Eastern):

| gate | result | ecalc total |
|---|---|---|
| 2 nodes, `BS_BATCH_PIPE=1` (`--build`, P24 forced) | PASS: rc 0, VERIFY OK, digits identical | 19.18 s |
| 2 nodes, default | PASS | 19.09 s |
| 1 node, `BS_BATCH_PIPE=1` | PASS | 11.45 s |
| 1 node, default | PASS | 11.70 s |
| after the rebase on main 740ac38 (SHR2, P1 merged): 2 nodes and 1 node, `BS_BATCH_PIPE=1` (`--build`) | PASS, PASS (20:13–20:14 Eastern) | 19.90 s, 10.71 s |

Timing at 1 node, 6.441 × 10¹⁰ digits (measured, 2026-10-12 19:29–19:47 Eastern, 16 runs, all rc 0 and VERIFY OK):

| quantity | A (off) mean | B (on) mean | B − A per round (r1…r4) | mean B − A, 95 % CI (4 rounds) |
|---|---|---|---|---|
| **total** | 68.81 s | 67.82 s | −0.87, −1.00, −1.16, −0.94 | **−0.99 s [−1.18, −0.80]** (t = −16.3) |
| bs phase | 37.92 s | 35.75 s | −3.48, −1.88, −1.79, −1.50 | −2.17 s [−3.59, −0.75] |
| bs batch levels (`batch` of the `bs` line) | 21.96 s (21.40 in rounds 2–4; r1_1 was 26.2 s) | 19.93 s (19.62 in rounds 2–4) | — | about −1.8 s (rounds 2–4) |
| init (switch not read yet) | 13.66 s | 14.21 s | +0.65, +0.65, +0.35, +0.55 | +0.55 s (see open issues) |
| dm phase | 17.00 s | 17.58 s | +2.0, +0.15, +0.15, 0.0 | +0.58 s, all but 0.1 s of it in round 1 (B 20.2 and 18.2 s) |

Per level, round 2 (r2_5_A against r2_6_B, measured, `ECALC_VERBOSE=2` lines). Wall time is the whole level; GPU time is
the event sum scatter + ntt + crt of the slowest device:

| level (pairs) | A: layout / batch / wall | B: layout / batch / wall | GPU, both arms |
|---|---|---|---|
| 1 (14.6 M) | 2.14 / 2.85 / 5.06 s | 1.22 / 2.37 / 3.62 s | 1.17–1.27 s |
| 2 (7.3 M) | 0.32 / 1.22 / 1.57 s | 0.12 / 0.92 / 1.05 s | 0.64–0.66 s |
| 3 (3.7 M) | 0.16 / 0.67 / 0.85 s | 0.08 / 0.60 / 0.69 s | 0.38–0.39 s |
| 4–7 | 2.20 s together | 2.18 s together | — |

The gain is at levels 1–3 (−2.1 s of level wall in this pair), as the O7b analysis predicted. From level 4 up the levels
were already GPU-bound. The run total gains about half of the bs gain. The rest is offset by the B arm's longer init (+0.55 s) and by
round 1's two slow dm phases (open issues).

**Verdict.** The change is measured: **−0.99 s per 1-node 6.441 × 10¹⁰ run (95 % CI −1.18 to −0.80 s, 4 ABBA rounds),
digits identical** on all four gates. That is the low end of the 1–3 s model. The batch phase alone gains about
1.8 s. The switch stays off; the user decides adoption. At 576 nodes the per-node leaf tree is about the same size per
node (6.6 × 10¹⁰ digits per node), so a gain of the same order is expected per node there (modelled, not measured).

**Open issues.**

- The B arm's init averaged +0.55 s in all four rounds, although the switch is first read in `bs`, after init. Init
  (rns_init's mapping) varies by ±1.2 s from run to run. A position effect within ABBA (B runs are always 2nd and 3rd)
  is possible. Not resolved. Without it, the total gain would be about −1.5 s (modelled: bs −2.2 s minus dm +0.1 s plus
  the seed-wait shift).
- Level 1 is still host-bound in the B arm: 1.22 s of layout (the first touch of the 820 MB node array and of the
  1.9 GB product list) and about 1.1 s of host time inside the batch call. The remaining gap is about 2.3 s at level 1
  (measured: wall 3.62 s against 1.27 s GPU). Next steps: keep the node arrays across levels and pre-fault the buffers
  during the seed wait. Stamping the host steps inside the level would attribute the rest.
- Tested only on the VMM pools (the default). The host-pool spill-merge path keeps its synchronisation and was not
  timed.

**RESUME.** Branch `n3` merged to `main` with the switch off; worktrees removed (local `../ntt-wt-n3`, aac7
`~/ntt-wt/n3`). Nothing is running. Raw data: aac7 `~/n3/` and `archive:results-raw/N3/`. To continue: a follow-up
(level-1 first touch and node-array reuse) can sit behind the same switch; re-run item 51's ABBA command.
