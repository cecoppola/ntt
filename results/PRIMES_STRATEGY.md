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
`ec_np_planes(pool_log, g)` sizes pool 0 (4 planes when the run's largest plane 2^(min(31, pool_log) + floor(log2 g)) can exceed the
switch-over). Every one-node tier is unchanged (3 primes). `dist_core` / `mn_core` take `np` per product; the CRT kernel already had
`dgarner3/4`; the B form passes it through; the transform cache keys slots by prime count; `mn_plan.c` prints `plan primes` and counts
pieces at four; `binsplit.c` one line (pool 0 under auto); mem_model/mn_model/estimate (`--np-mn auto`) follow. Grids unchanged.

**Plan/layout (plan, login node).** `ECALC_NP=auto MN_PLAN_ONLY=4.25e13:576`: rc 0, pool 16 + 12 GiB per APU, pieces at four: tree
56 of 86, recip 26 of 66, div 28 of 28 (110 of 222), plan check OK; `=3` refused (84 over), `=4` OK. `BS_LAYOUT_ONLY`: planes 120.88 GB at
576 (= NP 4; node 387.25 GB), 103.70 GB at size 1/4 (= NP 3). `mem_model.py --check-c`: 0 terms not exact, arena diff 0.0000 %.

**Forced tests (m, lowered `ECALC_NP_AUTO_TERMS`).** `t_crt` 24: VERIFY OK (102 checks; garner3 = garner4 on 20 000 values up to the
bound, wrap at bound + 1); `t_dbig`, `t_newton`, `t_mn_grid` at sizes 2-4 (decimal and binary) VERIFY OK; 10^9 (bound 8e6) at sizes
1/2/4 and 10^8 (bound 2e6) at sizes 1-4 and 10^9 size 4 (bound 5e7): all **identical** to the reference with both prime counts occurring
in one run (B form and C, sizes 1-4). Standing regression with the switch on: mnaccept `unit,e9,mn` **15 passed, 0 failed**.

**10^11 at size 1, three pairs, order alternated (m):**

| | total | phases (bs + 10dP + dm) | init |
|---|---|---|---|
| `ECALC_NP=auto` | 202.14 / 201.16 / 197.17 s, mean **200.2** | mean **179.9 s** | 22.0 / 20.6 / 17.7 s |
| defaults (`ECALC_NP=3`) | 199.65 / 199.82 / 199.37 s, mean **199.6** | mean **180.7 s** | 18.5 / 19.0 / 18.6 s |

All six sha1 identical to the reference (578f5efb...). Neutral as designed (all products within the bound, pool 0 three planes); the
init difference was run order (first 10^11 run of a job maps slower).

**Cost of four primes vs three per product (m, 10^10, `ECALC_NP_AUTO_TERMS=1` vs real bound, same sha1):** size 1 (60 pairs) total
3.430 -> 4.478 s, **x1.306** (ntt + crt x1.314; 2^30-2^31 limbs 1.33-1.34); size 2 (45 pairs) 86.14 -> 110.96 s, **x1.288** (ntt + crt
x1.339). Matches the model's `T_PIECE_31_NP` 1.078 / 0.827 = 1.303 (S13).

**Target estimate (mod).** 4.25e13 on 576: `auto` 238.6 s without the write, 268.3 s with it at 0.6 GB/s (254.6 at 0.8) vs `ECALC_NP=4`
256.0 / 285.7 s; **-17.4 s (6.8 %)**: leaf batch 25.3 -> 19.7 and top 19.6 -> 16.4 (one-node tiers back at three primes), distributed
levels 107.0 -> 98.7, reciprocal 33.5 -> 33.2, division unchanged; node 416.0 GB (as NP 4, pool 0 at four planes = +17.2 GB); the
grid step 4.29 -> 4.30e13 does not move; ceiling 480 GB at 5.57e13 (384.6 s auto vs 411.9 s at four). Three primes is not runnable.

**Open issues at close.** Adoption by the user (done 2026-09-28: `ECALC_NP=auto` on the target line; since D18 it is the multi-node
default); four-prime products at target sizes (2^34-2^40) run the same code as the loopback runs (roots checked in §P15); a cost-aware
grid preferring pieces under the bound; pa + pb vs min(pa, pb) (-> §MPB15); `t_newton` size-1 prints no verbose line; cosmetic
`planes:` line prints `np 3` under auto.

## §P15 — roots above 2^33 and the prime bound at the 576 target (Phase 15, Batch 1 from L8's findings)

**Question/change.** L8 found three primes cannot hold the target's pieces, and `ec_root(i, logn)` = w33^(2^(33 - logn)) has a negative
shift above 33. Branch `p15-P`.

**Options for 4.25e13 on 576 (mod; mn_model DEFAULT15; part file at 0.6 GB/s; grids from `plan_sweep.sh`, plan):**

| option | wall without write | with write | node memory | code |
|---|---|---|---|---|
| three primes | 231.1 s | 349.6 s | 398.8 GB | not runnable (`ec_np_check`) |
| **(a) `ECALC_NP=4` everywhere** | **277.9 s (+46.8, +20 %)** | 396.3 s | **416.0 GB** (+17.2) | launch-line setting; `MN_PLAN_ONLY` OK; grids identical at 3 and 4 primes (step 4.29 -> 4.30e13) |
| (b) four primes only above the bound | 257.8 s (+26.7) | 376.2 s | 416.0 GB | later built as `auto` (§NP15) |
| (c) three primes, mn cap 2^35 | 3243 s (14x) | 3361 s | 384.7 GB | not viable (46 472 pieces) |

(a)'s +46.8 s: tree +21.9, reciprocal +6.6, division +6.9, one-node leaf the rest (+11.4). PLAN §36.10's 576-node numbers assumed three
primes at the 2^40 cap and are not achievable; with (a): 278 s without the write, 396 s with, 416 GB per node (mod). Recommendation made:
(a) now, (b) worth -20 s (mod) for about a day; user chose `ECALC_NP=4` (2026-09-27), then `auto` (2026-09-28), then D18.

**Fix 1 (roots; no switch; bit-identical below 2^33).** `modarith.h`: logn <= 33 as before; 33 < logn <= v2(p-1): `g^((p-1) >> logn)`
from the generator `ec_G` (`ec_root3`: `g^(((p-1) >> logk)/3)`); beyond v2(p-1) (= 48, 47, 44, 44 for the WP8 set c 2^44 + 1) `ec_fatal`
rc 3 (`ec_logn_limit` = 44). The old formula gave w = 1 at logn 34 on all four primes (m), reached by the mn tier from 8 nodes on (the
target's tree level 3 forms 2^34-point pieces, level 8 and the division 2^40; plan). Tests (m): new `t_roots` (host) VERIFY OK, 2789
checks (equal to the old formula for logn 0..33, exact order vs GMP, 33/34 chain, root3, out-of-range stops with rc 3; old prime set 1176
checks); `t_dist DIST_BIG=34` (one 2^34-point distributed transform on four APUs, 96 GiB per APU): identity and sparse square vs direct
convolution, **0 of 17 179 869 184 points differ for each of primes 0-3** (control 2^33 also 0); 2^35 does not fit one node. Transforms
27.3 + 16.0 s (prime 0). mnaccept `unit,e9,mn` 13 passed; 10^11 size 1 VERIFY OK, total 240.06 s, sha1 identical.

**Fix 2a (plan-time check).** `MN_PLAN_ONLY=D:g` checks every product of every tree level, the reciprocal chain/shards, the division and
leaf `dist_db` against the term bound of `ECALC_NP` and the roots' 2^44 limit, printing `plan check ... OK` or `plan REFUSED` (rc 3);
`BS_LAYOUT_ONLY` (g > 1) runs it silently first. Plan results at 4.25e13:576: `ECALC_NP=3` rc 3 (84 of 1244 products exceed; first at
tree level 5, piece 28 579 148 066 + 30 209 369 707 limbs = 58 788 517 773 terms, 1.01x over; the largest piece 1.077e12 limbs, 18.4x
over by pa + pb, 8.1x by min; **46 products over even by min**), `=4` rc 0, longest transform 2^40 of 2^44; layout 387.25 GB at 2^31 (np 3
370.07). Other sizes: 1e11:1 and 1e9:4 OK; binary limbs at three primes refused; `ECALC_NP=4 3e15:32768` refused by the roots (a 2^45
piece; the mn cap is valid up to groups of 8192 nodes). The grids and the step sweep (4.15-4.45e13) are identical at three and four primes.
Summary line 181 pieces (183 with each level's largest group).

**Open.** The run's own check `ec_np_check(pa + pb)` is conservative up to 2x (tightening would not rescue three primes at the target);
a prime set with 15 | p - 1 would change `ec_G`/tables only.

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
Stays C: host operands/result, offset result views, the transform cache. Exit line reports products and seconds by form.

**B4 design and verdict (m, job 21047, P = 3 decimal, `t_strategy`).** B4 splits the product mod x^n - 1 into residues mod x^(n/2) -/+ 1;
APUs 0-2 transform the longer operand (n points) and the shorter's lower residue (n/2), APU 3 forms the three primes' upper residues; planes
12 n bytes. Times (s) B / B4 / C: 2^26 0.0201 / 0.0207 / 0.0223; 2^29 0.1481 / 0.1589 / 0.1750; 2^31 0.6083 / 0.6382 / 0.8183; 2^32 1.2708 /
1.3100 / 4.3564. **B4 is 3-7 % slower than B** (B4/B 1.03-1.07, outside the 5 % rule at 2^27-2^30): the saving (about 0.05 s at 2^31) is
eaten by transferring n/2 points per prime from APU 3 (8 GiB, about 0.16 s at 51 GB/s pull; a push would still cost about 0.1 s). Its
memory advantage does not change what fits today's pools (both need one n-point plane in one pool). **B is the form `auto` uses**; B4 was
later removed (2026-10-12).

**Library (m).** Transform length is the smaller of 2^k and 3 x 2^(k-2) holding the product, at least 2^20; CRT in stripes (<= 16384
rows) with `k_crt_batch` writing result quarters and 4-limb spills added by `db_add_spills`. Library check `t_strategy <k> <reps> CV`
(six shapes per size under C, B, B4, auto): all identical at 2^22, 2^26, 2^29, 2^31 (22 checks each). n/2 x n/2 through the library
(s): 2^29 C 0.1759 / B **0.1275** (-28 %) / B4 0.1554; 2^31 C 0.8190 / B **0.5389** (-34 %) / B4 0.6368 / auto 0.8205 (= C: 2^31 does not fit
12 GiB pools). The library's B load kernel is 0.105 s vs the test's 0.227 s; the first B product at 2^31 pays a one-time 6.2 s mapping.

**Gate: 10^9 and 4 x 10^10 under each value (size 1, decimal, P = 3; one run each, all identical to the reference by `cmp` / segment
md5s).** 4 x 10^10:

| RNS_STRATEGY | wall s | phases s | bs | recip | dm | device peak GB | products by form | extra planes GiB/APU (alloc s) |
|---|---|---|---|---|---|---|---|---|
| C | 65.96 | 48.4 | 26.4 | 10.4 | 22.0 | 287.5 | all C | 0 |
| **auto** (B) | **62.05** | **42.8** | 23.9 | 7.8 | 18.9 | **287.5** | 73 B (7.39 s) + 9 C (12.16 s) | 0 |
| B4 | 68.65 | 46.7 | 29.0 | 8.4 | 17.7 | 377.7 | 82 B4 (22.36 s) | 24 (4.50 s) |
| B | 71.60 | 51.9 | 35.1 | 7.6 | 16.7 | 442.1 | 82 B (27.09 s) | 48 (11.47 s) |

auto takes 5.6 s (-12 %) off the phases at no memory cost (reciprocal -25 %, dm 22.0 -> 18.9 s); the 9 C products are 3 x 2^30-point pieces
needing 48 GiB per APU in B. B and B4 speed up recip/dm but their extra-plane mapping (11.5 s / 4.5 s inside bs) costs more than they gain
(net of mapping, mod: B about 40.4 s at 442 GB, B4 about 42.2 s at 378 GB vs auto 42.8 s at 287.5 GB; B would win only with pools sized for B at
init, +154 GB). Walls compare across two nodes; init varies +/-2 s, so phases are the fair comparison. 10^9 (s total): C 17.44, B 16.16,
B4 16.79, auto 16.25 (52 products 0.30 s in B vs 1.31 s in C, 4x per product); `RNS_STRATEGY_CHECK=1` all 52 identical.

**Bug fixed (m).** B4 hung in the reciprocal at 10^9: the four-slot twiddle-table cache always evicted the last slot, so a B4 product's two
tables evicted each other and the first was used after being freed. Fix: eight slots, round-robin (05a7730f).

**Grid under auto (m, job 21062, commit 744df439, identical by segment md5s).** `split_grid` weighs candidate grids by form (B pieces x 0.70,
C x 1, 3 x 2^k x 1.05) so every product runs B (89 B products in 17.06 s):

| run | wall | phases | bs | recip | dm | products |
|---|---|---|---|---|---|---|
| C | 69.71 | 48.1 | 26.0 | 10.46 | 22.0 | all C |
| auto, `RNS_STRATEGY_GRID=0` | 64.09 | 45.3 | 25.3 | 8.18 | 19.9 | 73 B (7.59 s) + 9 C (12.91 s) |
| **auto** (grid on) | 65.37 | **43.6** | 25.1 | 8.24 | 18.4 | **89 B (17.06 s)** |

The grid takes 3.4 s off the products and 1.7 s off the phases; vs C auto saves **4.5 s of phases (-9.4 %)** at the same 287.5 GB peak.
Regression at the default: `unit` 8 passed (1158 s), `e9` 2 passed; `RNS_STRATEGY=auto mnaccept --only mn` 5 passed (10^8 at sizes 2-4, 10^9
at 2, 4 identical).

**Open at close.** Size > 1: the mn tier's products stay C (a prime-per-APU form there would need a cross-node operand broadcast at 25-50 GB/s
per APU vs 51 over xGMI; not built; node-local products at size > 1 take the strategy); the grid cost may prefer 2^31 pieces under auto
(S13's E2); B4's transfer as an overlapped push (modelled no gain).


Sections moved to `archive:results_merged_2026-10-24/PRIMES_STRATEGY.md` (2026-10-24 cleanup): §MPB15; §S13.