# PRIMES_STRATEGY — prime count and strategy

Condensed 2026-10-19 (task 8b) from the merged per-item reports; originals in `archive:results_merged_2026-10-19/`, raw data in
`archive:results_raw_2026-10-19/` (and `archive:results-raw/`). Old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`. Labels: **m** measured,
**mod** modelled, **a** assumed, **plan** = the C code's own `MN_PLAN_ONLY` decision (exact for the code at predicted sizes).
Times Eastern.

## Topic summary

- **Why more than three primes.** Three primes (p0 p1 p2) hold a product piece of at most 58 424 467 928 terms (pa + pb, 18-digit
  limbs). The 576-node target forms pieces up to 1.08e12 limbs, so three primes cannot run it (`MN_PLAN_ONLY=4.25e13:576` refused: 84
  of 1244 products over the bound, the first at tree level 5, 1.01x over; even a min(pa, pb) test leaves 46). §P15.
- **Current state (see ecalc/README.md; D18, 2026-10-12, results/LAUNCH_DEFAULTS.md).** At size 1 `ECALC_NP` is 3; unset at size > 1
  it becomes `auto`, plane pool 0 holds four planes in every multi-node run, and **every multi-node product runs four primes in the
  24-digit P24 form** (`mn_p24_of`; the former `MN_P24=1/2` switch is built in). The pre-D18 three-prime multi-node path (`ECALC_NP_MN3`) was removed 2026-10-19 (D56). `ECALC_NP_AUTO_MIN` (min(pa, pb) switch-over, §MPB15) was removed 2026-10-20 (Q53); `ECALC_NP_AUTO_TERMS` (test knob) remains; the
  `auto` per-product switch-over of §NP15 now matters only at one node-process (the `ECALC_NP_MN3=1` path is removed, D56).
- **Digits.** Every variant is exact by construction and was byte-identical in every test (mnaccept, 1e8/1e9, 4e10, 1e11, 2^34
  transforms). Roots above 2^33 were broken (w = 1) and fixed with no switch (§P15).
- **P24 measured value.** 10^10 at 2 processes 158.1 -> 97.0 s (m, -38 %, §P2415); 2 x 6.441e10 digits on 2 nodes on the aac7 line
  -8.87 s [-13.48, -4.26], p 0.009 (m, §MEAS-P24); at +8.6 GB per APU (+34.4 GB per node) of pool memory. Per-point surcharge x1.025
  (m, n = 2). Modelled target gain at cache 0 -88 to -113 s (mod).
- **Four vs three primes per product** costs x1.29-1.31 (m, §NP15); the grids and plan steps do not depend on the prime count (the cap
  is in points).
- **Strategy (Phase 13).** For one dependent product prime-per-APU (B) is the fastest at 2^20..2^32 points; the boundary is memory not
  time (§S13). `RNS_STRATEGY=auto` (B where its planes fit the pools, else C; the grid prefers B-fitting pieces) is the default since
  Phase 13c; B4 was slower and was removed 2026-10-12 (§B13b). The plane cap default is `run` (`results/MEMORY.md`).
- **Superseded:** the planning tables in §P2415 §1.4/§4.2 and §NP15 §4 (modelled targets of 2026-09-28; later target figures are in
  results/TARGET.md); §P15's option (a)/(b) discussion (resolved: `auto`, then D18); the 2^35-cap option (c).

## §P2415 — four primes at 24 digits per transform point in the mn tier (Phase 15 Batch 3, agent P24)

**Question/change.** SC's §3.1 (archive:results_merged_2026-10-24/TARGET.md §SC15, user-approved 2026-09-28): with four primes (p0p1p2p3 = 2^206.55) a transform
point carries 24 digits instead of 18, so 4 limbs (72 digits) become 3 points and a plane of pa + pb limbs holds P(pa) + P(pb) =
ceil(3n/4) points each (3/4 of today's). Branch `p15-P24`; the switch `MN_P24` (default 0) is now built in (see topic summary).

**Bound.** Coefficient <= min(Pa, Pb) (10^24 - 1)^2. Garner needs min < 1.51e14 (2^47.1); the binding check is the CRT's four-limb
output c 10^s < 10^72 (s <= 12), i.e. c < 10^60, min <= 10^12 = 2^39.86 (x1.82 headroom at the 2^40 cap). So a P24 plane is capped at
2^40 points (`P24_CAP_LOG`; the target's 576-node cap is exactly 2^40), `mn_core` refuses min(Pa, Pb) > 10^12, and the CRT kernel raises a
device flag on a fifth nonzero limb. Digits are byte-identical by construction (exact product, no rounding).

**Design.** Rank rho's rows [a, a + r) of the R x C plane give per column j the points [Rj + a, ...); their limbs form a contiguous run
(in-run [L(x0), L(x1)+1) operand side, out-run [L(x0), L(x1)) result side, tiling the limb line exactly), L(x) = x + floor(x/3); closed
forms `p24_seq_start` / `p24_seq_limb` in O(1). New kernels: `k_pack_mn24`, `k_gather_mn24` (point = two limbs combined and reduced mod
p), `k_crt24` (Garner -> 4 limbs shifted by s digits, carry over the out-run, a 4-limb spill at L(x1)), `k_scatter_mn24`; spill decode
in dbig.c (`db_share_add_spills_x24`); pieces need no alignment (each view regroups from its own limb 0). The operand-side peak is
max(A, B) (A's sequence freed after its gathers), below today's. A P24 product adds X afterwards with `mdb_add_shifted`; the transform
cache is not used by P24 pieces. The decision is one function `mn_p24_of(na, nb, g)` used by the run, plan, scratch sizing and tests;
`dist_core` (one-node tier) is untouched. Mode 1 ran P24 where the 18-digit piece would run four primes; mode 2 ran every mn product at
P24 (SC's model). New files `p24.h`, `p24_mn.h`, `p24_crt.c`, `tests/t_p24.c`, `tests/p24_model.py`, mn_model P24 term.

**Tests (m).** `t_p24` host: VERIFY OK (78541 checks, 1.7 s); seven one-token mutants each fail; device: VERIFY OK (78594 checks).
Behind `MN_P24`: `t_mn_grid` at 2/3/4 processes, chunked rounds, auto with both counts: all VERIFY OK; mnaccept `unit,mn` with
`MN_P24=1 ECALC_NP=4`: 13 passed, 0 failed (1253 s); `MN_P24=2 ECALC_NP=auto`: 5 passed (78 s); switch off: `unit,e9,mn` 15 passed,
4e10 size 1 identical (65.03 s); 1e8 at size 2 identical (72 P24 lines).

**Timing pair (m), 10^10 at size 2, `ECALC_NP=4`, POOL_LOG 29, interleaved 0,1,0,1 over two jobs, digits identical:**

| run | MN_P24=0 total (bs, dm) | MN_P24=1 total (bs, dm) |
|---|---|---|
| job 1 rep 1 / rep 2 | 154.25 (24.6, 119.9) / 158.96 (25.6, 124.8) | 99.69 (19.3, 72.0) / 97.02 (18.9, 69.2) |
| job 2 (after `k_gather_mn24_np` fusion) rep 1 / rep 2 | 156.22 (24.7, 122.0) / 162.98 (26.2, 127.2) | 95.64 (18.2, 68.3) / 98.29 (18.7, 70.7) |
| **mean** | **158.1** | **96.97** after the fusion (97.7 all four): **-38 %** |

The gain is quadratic-plane effect: node 0 forms 34 pieces over 10.0 x 2^30 plane points at 18 digits vs 32 pieces over 6.0 x 2^30 under
P24. **Per-point price at the same plane (surcharge): x1.025** (10.36 s per 2^30 points at 18 digits vs 10.62 s P24 after the fusion;
n = 2 runs x 3 pieces vs 4 x 8): redistribution 0.47-0.53 -> 0.74-0.77 s, transforms 8.66-9.07 -> 8.98-9.16, CRT 0.05 -> 0.05.

**Plan and model at the target (plan; mod).** C plan (`MN_PLAN_ONLY`, all checks OK; largest P24 piece 2^40 points, min 3.6e11 < 1e12):
5.1e13: pieces node 0 today 222 (108/74/40), P24 **158** (72/63/23), auto + MN_P24=1 166; 5.55e13: 259 -> **180**; 5.7e13: 266 -> 182.
The model gives the same counts. Layout memory: +34 MB (dm) / +36 MB (tree), arena and node unchanged. Modelled walls (no write / with
write at 0.6 GB/s; fabric assumed 100 GB/s per APU, 2 us), 5.1e13: ECALC_NP=4 cache 0 430.2 / 449.3 s today vs 317.2 / 352.3 (P24=1);
auto cache 0 406.8 / 426.0 vs P24=2 311.0 / 346.2; with the measured x1.025: 314.6-321.8 s without the write. P24 at cache 0:
-88...-113 s without the write (SC had -94.5); with two cache slots -32 s. 5.55e13 under mode 2 runs 343.9 / 378.7 s (faster than today's
5.1e13), memory ceiling 480 GB at about 5.57e13. A sweep 5.0-5.8e13 is in `archive:results_raw_2026-10-19/P2415/model_sweep.txt`.

**Open issues at close.** Cache off for P24 products (later cache work: results/CACHE.md); mem_model had no P24 term; the layout did not
see mode 2 (harmless, <= +0.04 GB); surcharge measured at one configuration; B's gathers run per prime; 1e11 size-1 and ckpt/recheck with
P24 on not run then. **Decision/origin:** adoption later by D18 (2026-10-12).

## §NP15 — four primes only where needed (`ECALC_NP=auto`; Phase 15 Batch 2, PLAN §37 row 1)

**Question/change.** Run four primes only for products over the three-prime bound, three otherwise (P15's option (b)). Branch `p15-NP`.
`ECALC_NP=auto` (off by default then): decimal limbs -> `ec_np` 3 and `ec_np_auto`; `ec_np_for(nterms)` = 4 when nterms >
`ec_np_auto_terms` (= `ec_np3_max_terms` 58 424 467 928, or the test knob `ECALC_NP_AUTO_TERMS`), `ec_np_prod` adds the old check;
grid preferring pieces under the bound; pa + pb vs min(pa, pb) (-> §MPB15); `t_newton` size-1 prints no verbose line; cosmetic
`planes:` line prints `np 3` under auto.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/PRIMES_STRATEGY.md`.)

## §P15 — roots above 2^33 and the prime bound at the 576 target (Phase 15, Batch 1 from L8's findings)

**Question/change.** L8 found three primes cannot hold the target's pieces, and `ec_root(i, logn)` = w33^(2^(33 - logn)) has a negative
shift above 33. Branch `p15-P`.
**Open.** The run's own check `ec_np_check(pa + pb)` is conservative up to 2x (tightening would not rescue three primes at the target);
a prime set with 15 | p - 1 would change `ec_G`/tables only.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/PRIMES_STRATEGY.md`.)

## §MEAS-P24 — four-prime product form (P24) for all multi-node products against the default, 2 nodes

**Question.** Time and device memory of P24 for every multi-node product (what `tools/gate.sh --p24` forces) at the standard 2-node size
(OPPORTUNITIES AUD3-6). Measurement only; main a6ba51d (SHMEM); A unset, B `ECALC_NP_AUTO_TERMS=1000000`; queue item `83_meas_p24`
(user-approved 2026-10-12), 4 rounds of ABBA, 2 x 6.441e10 = 1.2882e11 digits, no digit writes, aac7 launch line + `COMM_XSTATS=1
MN_WAIT_STATS=1`; VERIFY OK and rc 0 on all 16 runs (B logs `plane pool 0 for 4 primes`).

**Results (m).** A totals mean 133.7 s (131.68-138.71); B mean 124.8 s (122.81-127.26); round differences -12.47, -7.89, -5.58, -9.55;
**mean B - A = -8.87 s, 95 % CI [-13.48, -4.26], 4 pairs, p 0.009** (stop rule met). Stages (A / B, s): binary splitting 43.1 / 40.3
(-2.7), dm 52.9 / 44.4 (-8.5), init about 34 / 36.5. Device memory pool 0 at `[init]` per node (per APU): in use 317.8 (79.5) -> 352.2 (88.1)
GB = **+34.4 (+8.6)**: planes +17.2 (+4.3), block pool +17.2 (+4.3); host RSS unchanged (HWM 28.9 GB); the peak was not extracted.

**Verdict.** P24 is faster at 2 nodes (-6.6 %), at twice the earlier modelled +4 GiB per APU. The target uses P24 from 32 nodes; whether
the target is memory-bound decides the trade. D18 later made P24 the multi-node default and kept `ECALC_NP_MN3=1` as the fallback (removed 2026-10-19, D56); the
gate compares digits at 10^9 (results/CODE_REDUCTION.md §AUD3).

## §B13b — Phase 13b, agent B: strategy B in the library, and B4 (PLAN §31, axis S)

**Question/change.** Put strategy B into `dist_core` (`rns_dist.c`) behind `RNS_STRATEGY` (default C): `C` four-step; `B` prime-per-APU
(planes that do not fit the pools from an extra per-APU hipMalloc buffer kept to the end of the dm phase); `B4` B spread over all four APUs
(P = 3, 1.5 n points per APU); `auto` B where its planes fit the pools as sized at init, else C, never allocates; the grid prefers B-fitting
pieces (`RNS_STRATEGY_GRID=1`, default under auto). `RNS_STRATEGY_CHECK=1` re-forms every B-form product with C and aborts on a difference.
**Open at close.** Size > 1: the mn tier's products stay C (a prime-per-APU form there would need a cross-node operand broadcast at 25-50 GB/s
per APU vs 51 over xGMI; not built; node-local products at size > 1 take the strategy); the grid cost may prefer 2^31 pieces under auto
(S13's E2); B4's transfer as an overlapped push (modelled no gain).
Sections moved to `archive:results_merged_2026-10-24/PRIMES_STRATEGY.md` (2026-10-24 cleanup): §MPB15; §S13.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/PRIMES_STRATEGY.md`.)

## §P24 audit — the P24 CRT bound end to end, and the P24_CHECK diagnostic (lane L2, 2026-10-27)

Trigger: a 3e13-digit run on the target (576 nodes, layout D) died with `P24 CRT flag` (`rns_dist.c` mn_core, after `p24_crt_err`); 4.608e12 completed. No logs.

**Verdict (code reading; nothing run).** The bound holds for every product kind **if every input limb is below 10^18**. A fired flag therefore cannot be a marginal bound violation.
- Bound: a coefficient sums at most min(Pa, Pb) products of two points below 10^24, so c < min(Pa, Pb) (10^24 - 1)^2. Plane cap **at most 2^40** points (`P24_CAP_LOG`; the actual cap is `mn_logn_cap(g)` = min(`dist_logn_max`, pool_log) + floor(log2 g) - 2 under VPE, so 31 + 9 = 40 at 576 nodes in layout D, 38 under VPE, lower with pool_log 30; so the bound only gets better with a smaller cap; 576 nodes: 40 with or without `MN_NONPOW2_CAP`: the next power of two, 2^41, does not fit q at 576 nodes) gives Pa + Pb <= 2^40, so min <= 2^39 = 5.50e11 and **c < 5.50e59 (code: 1.82x below 10^60); c 10^12 < 5.50e71 < 10^72 (same margin)**. The primes' product M = 1.511e62 (code), 276x above the worst c, so Garner is exact. `P24_MIN_MAX` (10^12) and `chk24` in the planner are looser than the cap (1.82x) and never fire; the cap is the real limit.
- By product kind: full / low product (pieces skipped, shift only moves the output window) / low cut (whole piece still formed; the cut skips pieces) / `mdb_view` operands (limbs clipped to the view; limbs past the view read as 0 in `k_gather_mn24`, `tend`) / grid pieces (each piece its own plane, `pa = ceil(na / ka)`, accumulated across pieces only after its CRT: the sum is never inside a coefficient) / NONPOW2-capped grids (cap and plane shape from `mn_shape`, same 2^40 limit; rows 455 / 456 per PE at 576 nodes, p24_run periodic in j mod 3 for any R, a): the inputs are the stored limbs, so the premise is only "limbs normalised". Nothing in the P24 path shifts or truncates an operand. At 3e13: operands about 1.67e12 limbs = 1.25e12 points each (code), so a grid of pieces with Pa + Pb <= 2^40 (census: 72 P24 pieces at 2e13 - 3.82e13); 3.82e13: 2.12e12 limbs = 1.59e12 points each, same cap, same bound. The bound is independent of the digits count.
- The flag's own semantics (found, code; the all-shift flag below became the default 2027-10-27, Q130, switch `P24_CRT_ANY`, =0 = this old flag): the old always-on flag is bit 1 = the carry out of c 10^s past four limbs, which at s = 0 and 6 needs c >= 10^72 / 10^66, above M: only s = 12 points (a third of them) flag at c >= 10^60, and the spill carry is bit 2. A **valid-range** error (stale or wrong limbs below 10^18) gives a correct-looking product with wrong digits and raises no flag. A flag needs a residue set that is no convolution of in-bound inputs: then c is a random value below M, which is >= 10^60 with probability 99.3 %, so an s = 12 point flags with near certainty. So the flag means (1) an input limb >= 10^18, (2) a corrupted or misplaced plane in the transform (a wrong exchange, twiddle or layout, in any of the four primes), (3) a stale or mismatched transform-cache plane, or (4) an index or kernel defect in a range no completed run reached.
- Cache (grid products only; P24 caches only inside `cache_pc_take`'s partial slots, keys reset at every grid product and at `cache_pc_give`): key = (mdb, offset, length, q, np, P24). `q` fixes the plane shape (`mn_shape` is a function of logn and q differs between logn), so an equal key means the same limbs and the same layout; no stale reuse found by reading.
- Over-commit: pool allocations come from the pre-mapped arena; growth inside a phase is `hipMalloc` and a failure is `mem_oom` (fatal, clean). An over-committed APU is documented in this tree to slow down (8x on aac7 once) or lose a process, not to return wrong data; nothing in the code path turns an allocation failure into a silent wrong value. Not testable on aac7; `P24_CHECK` does not see it.
- No defect found (T1d: nothing to fix). Open: (4) can only be seen at the target scale.

**P24_CHECK (default 0, changes no digit; zero cost off).** 1: every P24 operand's limbs are checked < 10^18 as packed here ("the stored share") and as received ("the exchange"), and the CRT also flags c >= 10^60 at s = 0 and 6 (bit 4), with the first flagged point (since Q130 this all-shift flag is on by default, `P24_CRT_ANY=1`, also at `P24_CHECK=0`, where the fatal line says the inputs are "not checked"; `P24_CRT_ANY=0` restores the s = 12 + spill flag unless `P24_CHECK` >= 1); a received-data hit and a CRT flag are fatal and print the product number and kind, piece, node, PE, sizes (plane R x C, first row, rows, shift) and the first bad position. 2: also no transform cache on P24 products. Only cache misses are checked: an operand served from the transform cache skips the exchange and the check (`P24_CHECK=2` turns the cache off so every operand is checked). The stored-share check is a **note** on stderr ("non-normalised share (may be legal)"), not fatal: stored shares come from carry-propagating producers (`mdb_addsub` scan, CRT output), but normalisation is not proven for every one. Reading a failure: "note" lines then a fatal "as received" = those limbs travelling, a producer question (P24 needs them < 10^18); "as received" fatal with no note = corruption in the redistribution; CRT flag with the operands checked (cache hits excepted) = transform / index suspect, and a flag that disappears at `P24_CHECK=2` = the cache.

**Coverage and fault injection.** Under `P24_CHECK` >= 1 each PE prints at exit `P24_CHECK: <products> P24 products (<pieces> pieces), <n> operands checked (stored a, received b), <h> cache hits unchecked, <f> flags` so a gate can prove the check ran (a product counts when it takes P24; a piece is one CRT; a hit is an operand served from the cache). `P24_CHECK_TEST=1|2` (with `P24_CHECK_TEST_K`, default 1) corrupts one received limb (to 10^18) or one plane residue of the k-th P24 product: the received-data check or the CRT flag must then be fatal, which proves the checks can fire. `t_mn_grid` prints `pe N: M P24 products formed`.

**t_p24crt.** The CRT kernel against GMP: digits equal for in-bound coefficients (random, all 10^60 - 1, edge mixes, zeros; three rank shapes incl. 342 and 205 rows); one coefficient from 10^60 to M - 1 at each s class: bit 4 exactly when c >= 10^60 in the default (all-shift) form, bit 1 only at s = 12 in the `P24_CRT_ANY=0` form. Digits are checked for both check settings and `p24_crt_err_pos` equals the planted point. Result: VERIFY OK, 171 checks (measured, aac7 gate of 179df51; the rerun after this change is pending).
