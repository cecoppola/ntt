# RECIP — the Newton reciprocal's error: the t_newton bound or the reciprocal?

**Question.** `t_newton` (section 5, in SIMP's run) found one divisor Q whose reciprocal μ ≈ B^(n_Q+k)/Q was −22 units off
GMP's floor at n_Q 131075, k 65538, beyond the test's ±8 bound (results on branch simp, `results/SIMP.md`, "Doubt"). Is the
bound wrong or the reciprocal? Can the error make a division wrong?

**Change / branch.** Branch `recip`. Switch `NEWTON_CHAIN_GUARD` (default 0, off): the reciprocal's anchored chain keeps one
guard limb per doubling. `t_newton`: the bound of sections 2 and 5 is the derived one (`recip_bound`), new section 8.

**Answer.** Both, in different ways.

1. **The −22 is the bound's fault (the reciprocal is as designed).** The test's ±8 units assume a normalised Q. With
   decimal limbs Q is not normalised, and one unit of μ is relative to M = B^n_Q/Q, which ranges from 1 to B. SIMP's Q has
   top limb 2.349·10¹⁶ (M = 42.6); its chain's last doubling 32769 → 65538 is exact (jn = 2j, no limb cut after it), and such
   a doubling keeps its own rounding error, at most 2M + 3 units. −22 = −0.52·M. A host GMP model of `recip_db2` with exact
   products reproduces it bit for bit (−22), and reproduces SIMP's three measured errors on the rng5 operands (+1, +0, +1).
2. **But the reciprocal has a real weakness the test never reached.** When the chain has several exact doublings in a row
   (any k with ⌈k/2^i⌉ even for several consecutive i, e.g. k = 2^m), each one squares the error left by the previous one
   (in units of the target precision, ε → −ε² + rounding). Modelled: k = 512 … 2048 at n_Q 1027 give errors up to 10¹²–10¹⁹
   units (one limb lost) for some Q with M ≈ 1, and up to 10³⁵ units (two limbs) at h = 2048. The division's corrections
   absorb errors up to ~64·B units of μ where μ's top limbs are used uncut; past that, DKM's step 1 dies with
   `DKM step 1: N corrections` (fatal; the corrections never produce wrong digits). Modelled: for a quotient of 4095 limbs
   (h = k₁ = 2048) 3 of 900 step-1 estimates are off by more than 64 (up to 1.8·10⁸).
3. **The fix is one guard limb per level** (`NEWTON_CHAIN_GUARD=1`): targets k, ⌈k/2⌉ + 1, ⌈(⌈k/2⌉ + 1)/2⌉ + 1, … so every
   doubling j → jn has jn ≤ 2j − 1 and its output is cut by ≥ 1 limb. Modelled: error 0 units in every sweep below, with
   exact or worst-case cut products; the DKM estimates exact. Cost: about two more limbs per level (≤ 2 extra tiny doublings
   at the bottom; at most 19 doublings instead of 17 up to k = 2·10⁵), negligible (modelled). Digits cannot change: the
   division corrects to the exact quotient either way.

## Derivation (one doubling, correction form, ALGORITHM.md §5.1)

r ≈ y_j = B^(n_Q+j)/Q at precision j, relative error e (r = y_j (1 + e)); M = B^n_Q/Q; one unit of r is 1/(M B^j) relative.

- t = Q_t·r, Q_t = Q's top `take` limbs (low by α < 1 unit of Q_t; α = 0 when take = n_Q); u = ⌊t/B^(take−j)⌋ − c₁ (c₁ ≤ 1:
  the low cut); d = B^(2j) − u = D + γ with D the exact value and γ = α r/B^(take−j) + β + c₁ ∈ [0, 2 + 1/B) (β the floor).
- corr = ⌊r|d|/B^j⌋ − c₂ (c₂ ≤ 1), r' = r B^j ± corr. Exact Newton gives Y(1 − e²), Y = B^(n_Q+2j)/Q. So
  **r' − Y = −Y e² + (r/B^j) γ ± (β₂ + c₂)**, with r/B^j ≈ M and Y e² = δ²/M for an input error of δ units.
- If the target jn < 2j the result is cut by 2j − jn ≥ 1 limbs: the M-scaled term (≤ 2M + 3 ≤ 2B + 3) becomes ≤ 3 units.
  If jn = 2j (the target even) nothing cuts it: **err = ⌊Y⌋ − μ ≥ −(2⌈M⌉ + 4)** after a single exact doubling, and the next
  doubling's quadratic term turns δ ≈ 2M into (2M)²/M = 4M; in normalised units ε = δ/M a run of exact doublings maps
  ε → −ε² + γ, which diverges once |ε| ≥ 2 (M ≈ 1, where the ±1 roundings are ±1 normalised). The step's convergence check
  (corr < B^(j+1)) only catches |ε| M ≥ B, so the last doubling's output can carry up to ~B²/M units.
- The division (DKM, ALGORITHM.md §5.2) uses μ's top k₁ + 1 limbs; X₀'s error is ≈ Δ/B for a μ error of Δ units when h = k₁
  (odd quotient length with the reciprocal at exactly h: the sharded flow when S has one limb more than P; size 1's prewarm
  asks k + 1 and gets h + 1, which divides Δ by B again). The correction loops stop at 64 (fatal).
- Production exposure (modelled): the run of exact doublings at the top of the chain has length ≥ 8 for ~0.4 % of k (random k
  in 10⁷ … 10¹⁰); runs of 5–7 at the top give μ errors of 10³–10⁷ units, harmless (X₀ exact). The fatal needs a long run,
  h = k₁ and an unlucky Q (1–4 % of Q at k = 2^m): roughly 10⁻⁵ per run size (modelled, order of magnitude). For 10⁹ … 10¹² digits
  (k estimated from log10 N!/18, assumed) h is odd: a top run of 0.

## Method

- Local model (no GPU): a C/GMP program reimplementing `recip_db2` step by step (seed `newton_seed_top`, take, middle/whole
  product as exact products, the low cuts as 0 / always −1 / random −1 per product, the convergence check, the overshoot
  shrink, the anchored chain), and DKM's step-1 estimate (`dkm_est_db`). It replays `t_newton 20`'s random stream of SIMP
  4003968 up to section 5 (and the rng5 stream), and sweeps uniform Q. Source kept in the local archive
  (`archive:drivers/recip/rmodel.c`).
- The SIMP operand: section 5's rng state before the draw is `0x9A4EA83C95610E7B` (top limb 23490058283511592, measured by
  the replay); `t_newton` section 8 (a) draws it directly.
- GPU (aac7, 1 node, one queue item): the build, `t_newton 20` with the switch off and on, and the 1-node gate off and on.

## Results

| check | M | k | error, guard off | error, guard on | label |
|---|---|---|---|---|---|
| SIMP's Q, n_Q 131075 (model replay) | 42.6 | 65538 | **−22** (= SIMP's GPU value) | 0 | modelled |
| rng5 Q, n_Q 131075, k = 65538 / 131075 / 262150 (model) | 1.16 | | +1 / 0 / +1 (= SIMP's GPU values) | | modelled |
| sweep n_Q 1027, k 514 (one exact doubling), 300 uniform Q | 1 … | 514 | −174 … +1 (exact products); −479 … +2 (cuts); max \|err\|/M 2.47 | 0 | modelled |
| same, top limb uniform below B/10 (large M) | up to 10¹⁷ | 514 | −1279 … 0; max \|err\|/M 1.04 (exact), 1.96 (cuts) | 0 | modelled |
| k = 512 (9 exact doublings in a row), 150 Q | | 512 | up to 3.2·10¹² (exact), 7.3·10¹⁸ (cuts) | 0 | modelled |
| k = 1024 / 2048 | | | up to 4.5·10¹³ / 1.1·10¹² (exact), ~10¹⁸ (cuts) | 0 | modelled |
| k = 513, 1027 (odd: the last doubling cut) | | | 0 | 0 | modelled |
| derived bound −(2⌈M⌉ + 6) … 8, chains with no two exact doublings in a row | | | 0 violations in 2 × 3 × 300 Q | | modelled |
| DKM step 1, n_Q 8200, quotient 4095 (h = k₁ = 2048), 300 Q × 3 A | | 2048 | \|X₁ − X₀\| up to 1.8·10⁸; 3 of 900 beyond 64 (fatal) | 0; 0 of 900 | modelled |
| same, quotient 2047 (h 1024) / 8191 (h 4096) | | | up to 3.2·10⁴ / 7.3·10¹⁵; 3 / 9 of 900 beyond 64 | 0 | modelled |
| same, quotient 3065 (h 1533, odd: top run 0, a run of 7 inside) | | | 0 | | modelled |
| same, top runs 1 … 7 (h 1522 … 1664) | | | μ error 84 … 4.5·10⁷ units; X₀ within 1 | | modelled |

GPU (aac7, 1 node, queue item `50_recip_gpu`, job 12775, commit e475e29; logs in `~/recip/` on aac7):

| check | guard off | guard on | label |
|---|---|---|---|
| `t_newton 20` (sections 1–8, 4 at 2^20) | **VERIFY OK (2206 checks)**, the new bound in sections 2 and 5 | sections 1–5 passed, then the step hit the item's 300 s timeout in section 6 (not a failure; sections 6–7 with the guard on not run) | measured |
| after the rebase onto SIMP + ADOPT (24446df, item `51_recip_rebased`): gate 1 node; `t_newton 20` | gate **PASS** (VERIFY OK, digits identical, ecalc total 13.29 s); **VERIFY OK (2756 checks)**, section 8 identical to the above | `t_newton 20` with `NEWTON_CHAIN_GUARD=1`: **VERIFY OK (2756 checks)**, all sections | measured |
| 8 (a) SIMP's Q, n_Q 131075, k 65538 (top limb 23490058283511592) | **−22 units** (bound [−92, 8]) | **0** | measured |
| 8 (b) n_Q 1027, k 1024, 100 uniform Q | **15** beyond ±8, largest \|error\| **9.9·10²⁴** units | 0 beyond ±8, largest 0 | measured |
| 8 (b) n_Q 1027, k 2048, 100 uniform Q | **9** beyond ±8, largest **2.0·10²⁷** units | 0, largest 0 | measured |
| 8 (c) DKM step 1, n_Q 8200, quotient 4095 (h = k₁ = 2048), 300 Q × 3 A, device μ | max \|X₁ − X₀\| **1.83·10⁸**, **3 of 900 beyond 64** (= the model's numbers exactly) | **0**; the 6 divisions run (3 first Q + the 3 fatal-size cases) exact against GMP, step-1 corrections ≤ 4 | measured |
| gate, 1 node, 10⁹ digits | **PASS**: VERIFY OK, digits identical, ecalc total 14.45 s | **PASS**: VERIFY OK, digits identical, ecalc total 11.82 s (one run each: not a timing comparison) | measured |

The device reciprocal behaves as the model says: the same −22 on SIMP's Q and the same DKM estimate errors (1.83·10⁸, 3
of 900) on the device μ; the larger errors at k = 2^m on the GPU (10²⁴ vs the model's 10¹³ with exact products) come
from the low cuts (the model's cut modes give up to ~10¹⁹ at n_Q 1027). The guard removes all of it.

## Verdict

(a) The −22 is a bound problem: the bound now scales with M (`recip_bound`: −(2⌈M⌉ + 6) … 8 when the last doubling is
exact, ±8 otherwise, no bound with two exact doublings in a row). (b) The reciprocal does have a real, rare failure mode
(fatal, never wrong digits) at chains with long runs of exact doublings; `NEWTON_CHAIN_GUARD=1` removes it at no measurable
cost (modelled; the 1-node gate with it passes, measured). Adoption is the user's: it changes no digits, and only the reciprocal's last few limbs and step counts.

## Open issues

- The 2-node gate with the switch on is not run (the item budget was 1 node); the sharded chain (`recip_mn_body`,
  `newton_mn_chain_start`) follows the switch. Needed before a default flip.
- `mem_model.py` does not mirror the switch's +1 limb in `dm_layout`'s jl (off by default; within the hole's slack).
- The fatal case was not provoked on the GPU through the division (it would abort the test); section 8 (c) computes the
  off-path step-1 estimates from the device μ instead (3 of 900 beyond the 64 limit, measured).
- The `newton_mn_chain_start` rewrite is identical with the switch off (all k < 2·10⁵ and 2³³ compared against the old
  loop, measured on the host); the off path's 2-node gate was not rerun.

## RESUME

Merged to main (branch `recip` deleted). Done: analysis, model, switch (off), test, docs, 1-node gates off/on, t_newton on
the GPU. Next, if the user wants the guard as the default: the 2-node gate with `--env NEWTON_CHAIN_GUARD=1`, `t_newton 20`
under the switch is done (VERIFY OK), `mnaccept.sh --only unit,e9,mn`, and a 1-node 10¹⁰ ABBA to show no cost.
