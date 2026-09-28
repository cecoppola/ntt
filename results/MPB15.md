# MPB (Phase 15 Batch 3): the `ECALC_NP=auto` switch-over on min(pa, pb) (`ECALC_NP_AUTO_MIN=1`)

Branch `p15-MPB` from `int15g` be2eec3 (the worktree HEAD was e107cd2, so I branched from be2eec3). aac6 clone `~/ntt-MPB15`.
Times are Eastern (aac6 logs are Central: +1 h). Every number is labelled **measured**, **modelled**, or **plan** (the C code's own
`MN_PLAN_ONLY` decisions, which are exact for the code at predicted operand sizes).

## 1. The change (off by default)

**The switch**: `ECALC_NP_AUTO_MIN=1`. It only acts with `ECALC_NP=auto` and decimal limbs. Unset, `0`, `ECALC_NP=3`/`4` or binary
limbs: nothing changes. A `MN_PLAN_ONLY=5.1e13:576` plan with `ECALC_NP=4` or `=3` gives an md5-identical output with the switch on
and off.

- **`crt.c`** reads the switch in `ec_np_init` (the `auto` branch). The new `size_t ec_np_terms(nc, na, nb)` returns min(na, nb)
  with the switch on and `nc` otherwise. **`modarith.h`** has the declaration and the argument (§2).
- **`rns_dist.c`**. These are the lines I touched (P24 edits other mn paths in this file at the same time):
  - `dist_core`: `const int np = ec_np_prod(ec_np_terms(nc, A.n, B.n), bi_decimal, "dist_core");` (the old argument was `nc`). The B form
    takes this `np` through `b_choose` / `b_core` (not changed).
  - `mn_core`: `const int np = ec_np_prod(ec_np_terms(nc, na, nb), bi_decimal, "mn_core");` (the old argument was `nc`).
  - The plan functions, so that `MN_PLAN_ONLY` counts what the run does:
    - `plan_pieces`: `p->formed4 += ec_np_for(ec_np_terms(la + lb, la, lb)) == 4`
    - `rns_dist_db_plan`: two lines, `p->formed4` for one plane and `p->np`
    - `rns_dist_mn_plan`: two lines, `p->formed4` for one plane and `p->np`
  - **Not touched**: `b_fits` (the grid's cost estimate still takes `ec_np_for(pa + pb)`). This keeps the grids unchanged. It is
    conservative: B at four planes fits wherever B or B4 at three fits, and under `auto` `b_choose` checks the placement again at the
    product's own count. It never matters at the target, where the size-1 leaf products are ≤ 2³¹ ≪ the bound.
- **`mn_plan.c`**: a helper `np_by()`. With the switch on, the header's `primes per product (…)`, the `plan primes` line and the
  `plan check … bound per product (…)` text gain ` by min(pa, pb) (ECALC_NP_AUTO_MIN=1)`. With the switch off the output is
  byte-identical to before.
- **`mn_model.py`**: `piece_np(dz, nc, na, nb)` uses min(na, nb) when `NP_AUTO_MIN` is set. `NP_AUTO_MIN` comes from the same
  environment variable `ECALC_NP_AUTO_MIN` (so `ECALC_NP_AUTO_MIN=1 python3 estimate.py --target --np-mn auto` works, and `estimate.py`
  is not edited). The two callers pass the operand lengths. The product memo key includes the flag, and `Design.env()` names it.
- **`tests/t_crt.c`** part 0: `ec_np_terms` checks with the switch on and with it off.
- **`ecalc/README.md`**: one row, `ECALC_NP_AUTO_MIN`.
- New files: `ecalc/mpb15_job.sh` (the node batches) and `tests/mpb15_lines.py`. The script checks every `RNS_VERBOSE` product line
  against the rule "four primes iff min(na, nb) > k" and counts the products the switch moved (min ≤ k < na + nb, at three).
- Pool 0's size (`ec_np_planes`) is unchanged and conservative. Under min, four planes would be needed only above 2 × the bound of
  plane points. At the target the division's pieces stay at four primes anyway, so pool 0 stays at four planes.

## 2. Exactness

Take the product C = A·B of operands of na and nb limbs, each limb in [0, 10¹⁸ − 1].

1. **The term count.** The coefficient c_k = Σ a_i b_j over i + j = k, 0 ≤ i < na, 0 ≤ j < nb. For a fixed k, each i gives at most one
   j, and each j gives at most one i. So there are at most min(na, nb) terms. Each term is ≤ (10¹⁸ − 1)², so
   c_k ≤ min(na, nb) · (10¹⁸ − 1)².
2. **No wrap.** Both tiers form the whole acyclic product in a cyclic transform of length n ≥ na + nb.
   - `dist_core`: 2^logn ≥ nc, or 3·2^(logn−1) ≥ nc for r3. The B form's `b_len(nc)` ≥ nc. B4's lo/hi split is an exact
     factorization of the same length-n cyclic product.
   - `mn_core`: `mn_shape`'s 2^logn ≥ nc.
   - Every caller passes nc = na + nb: `rns_mul_dist`, `_hd`, `mul_grid`'s one plane and pieces (pn = ai.n + bj.n), `mn_grid`'s one
     plane and pieces, `b_check`.
   - The gathers zero-fill past each operand's length.

   So each residue class mod n holds exactly one c_k (n ≥ na + nb − 1), and the bound of step 1 holds per transform point.
3. **The cuts and the added operand.**
   - The low cut, w, and the band (`rns_mul_high_db`, `rns_mul_low_db`, `rns_mul_band_db`, mn low / band) only skip whole pieces or
     truncate the operands before the product (`db_view` to w). Every formed piece is a whole product of its two views.
   - The added operand X (`mn_core`'s `cx`) enters `k_crt_batch` as `xk` after Garner, as an exact integer add with a carry. It is not
     a term of the residue sum.
   - A view normalized shorter (top zero limbs) only lowers min.
4. **Three primes.** Garner on p0, p1, p2 returns c mod M, M = p0 p1 p2. So it is exact iff c < M. `np3_max_terms` =
   ⌊(M − 1)/(10¹⁸ − 1)²⌋ = 58 424 467 928 is the largest t with t (10¹⁸ − 1)² < M (its exact 192-bit compare loop). So
   min(na, nb) ≤ bound ⇒ c_k ≤ min · (10¹⁸ − 1)² < M: exact.
   - The switch-over `ec_np_auto_terms` is ≤ the bound (the knob refuses anything above it), so every three-prime product under the
     switch is within the bound.
   - `ec_np_prod` still runs `ec_np_check(min)` on it.
   - `t_crt` part 0 checks garner3 = garner4 = v for values up to bound·(B−1)² and a wrap at bound + 1 (NP's test, run again in §3.2).
5. **The one-node tiers already used min.** `rns_mul_mdev` (`ec_np_check(min(na[0], nb))`) and `rns_mul_batch` (the largest
   min(na, nb)) have always checked min. The switch only brings the distributed tiers to the same rule.

## 3. Results

### 3.1 The plan at the target, switch off / on (plan; login node 17:49 EDT; `results/MPB15/plan_<d>_min{0,1}.txt`)

Environment: `ECALC_NP=auto COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 MN_PLAN_ONLY=<d>:576 ./ecalc`,
plus `ECALC_NP_AUTO_MIN=0` / `1`. Every run returned rc 0, `plan check … OK`, 1240 products.

| digits | pieces at four primes, off (pa + pb) | on (min) | all pieces |
|---|---|---|---|
| **5.1 × 10¹³** | tree 76 of 108, recip 35 of 74, div 40 of 40; **151 of 264** | tree **52**, recip **32**, div 40; **124 of 264** | 264 |
| 4.74 × 10¹³ | tree 68 of 100, recip 29 of 68, div 34 of 34 | tree 60, recip 26, div 34 | |
| 4.25 × 10¹³ | tree 56 of 86, recip 26 of 66, div 28 of 28 | tree 48, recip 24, div 28 | |

- **The grids are identical.** All 125 `plan` lines per size agree once the prime tags and the planes' GB are removed. The
  `plan summary` is the same: 222 / 242 pieces at 5.1 × 10¹³.
- **What moves at 5.1 × 10¹³**:
  - Tree level 5's pieces, e.g. 34 326 698 245 + 24 180 462 017 = 5.85 × 10¹⁰ (0.14 % over the bound) with min 2.42 × 10¹⁰: from four
    primes to **three**. The planes go from 68.72 to 51.54 GB per node.
  - Level 6's pieces, and 3 reciprocal pieces.
- **The division stays all at four**, as SC found: its pieces' min is ≫ the bound.

### 3.2 The node tests (measured)

(pending: `mpb15_job.sh 12` on s24-16, job 21759 submitted 17:52 EDT)

### 3.3 The estimate at 5.1 × 10¹³ on 576 (modelled; `results/MPB15/estimate_min{0,1}.txt`)

`[ECALC_NP_AUTO_MIN=1] python3 estimate.py --target --np-mn auto`. Assumptions: mn_model's code defaults, the TARGET fabric (assumed:
100 GB/s per APU, 2 µs), cache 2, the packed part file.

| digits | off: no write / @0.6 GB/s | on: no write / @0.6 GB/s | Δ |
|---|---|---|---|
| **5.1 × 10¹³** | 330.7 / 360.2 s | **324.0 / 353.4 s** | **−6.7 / −6.8 s** |
| 4.74 × 10¹³ | 306.5 / 335.8 s | 299.7 / 329.0 s | −6.8 |
| 4.25 × 10¹³ | 244.8 / 273.0 s | 242.4 / 270.6 s | −2.4 |
| 5.57 × 10¹³ (480 GB ceiling) | 395.3 / 425.7 s | 386.4 / 416.8 s | −8.9 |

- **By phase at 5.1 × 10¹³**: distributed levels 140.8 → 134.2 s, reciprocal 45.3 → 45.1 s, division 53.3 s (unchanged).
- **Memory, pieces and steps are unchanged**: 455.4 GB per node, 242 pieces.
- **This agrees with SC's model at cache 2** (−6.7 s). SC's −9.3 s is its cache-0 figure.

## 4. Open issues

- Adoption is the user's decision: `ECALC_NP_AUTO_MIN=1` on the target's launch line with `ECALC_NP=auto`.
- `b_fits` still prices the grid by pa + pb. This affects the grid only, never the digits, and is conservative (§1).
- The switch is not applied to `ECALC_NP=3`'s refusal: the plan check and `ec_np_check` still take pa + pb there.
- Pool 0's four planes could be tied to 2 × the bound under min (§1). At the target this does not matter.

## RESUME

- Steps 1–2 done (199ff38, 256b0f9): the code, the model, t_crt, the README row, the estimate, the plans at 5.1 / 4.74 / 4.25 × 10¹³.
- **Running**: `~/ntt-MPB15/ecalc/mpb15_job.sh 12 ppac-pl1-s24-16` (log `~/mpb15/job12.log`; batch logs in
  `~/ntt-MPB15/results/MPB15/`). Batch 1 is the forced tests; batch 2 is the mnaccept runs. Jobs are `-J MPB` and cancel themselves.
- Next: copy the logs back (`results/MPB15/`), fill §3.2, commit.
