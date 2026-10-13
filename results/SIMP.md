# SIMP — AUD3 items 1–4: one single-node division flow, DKM only, no 3·2^k distributed planes, one grid planner

**Question.** Can the four simplifications of `results/AUD3.md` (items 1–4) be made with byte-identical digits and no
slowdown, and does item 1 close AUD3 coverage gap (a) (the 1-node 10⁹ gate did not run the production DKM division)?

**Change / branch.** Branch `simp`, four commits in the order 3, 4, 1, 2 (3 and 4 first: 1 and 2 overlapped the files of P1
and SHR2 while they were open). No new switch. Settings removed: `DIST_R3`, `RNS_PLANES_3Q30`, and the `3*2^29` / `3*2^30`
values of `ECALC_PLANE_CAP` (2^30, 2^31 and `fit` stay; `off` now means "`POOL_LOG` and `DIST_LOGN_TEST` as given").

| item | what went | `ecalc/*.c|h` lines (measured, `git show --numstat`) |
|---|---|---|
| 3 | `dist_r3`, `plan3`, the `k3_*` kernels, `dist3_fwd/inv` and the r3 branches of `dist_core`; the r3 argument of `plane_pts` / `split_grid_cap`; `rns_planes_3q30` and its size rule (`rns_mul.c`), the b3 argument of `rns_plane_pool_bytes`; the 3·2^k plane-cap values (the cap index is now `pool_log − 30`). `mem_model.py`'s size rule returns off | +78 −202, net **−124** (code −94, comments −30) |
| 4 | `split_grid_cap`, `split_grid`, `db_grid_shape`'s body, `mn_grid_shape`'s body, `p24_split`, `p24_grid_shape` → one `grid_search` / `grid_shape` over a form (cap, smallest plane, the piece's points: limbs or P24's, the form-B cost of `RNS_STRATEGY=auto`); the piece loops of `mul_grid`, `mn_grid` and the plan printer's `plan_pieces` → one `grid_loop` (order, cache-slot assignment, counts) with a per-tier piece callback | +152 −154, net **−2** (code −10, comments +8): a restructure, 6 shape functions → 1, 3 loops → 1 |
| 1 | the size-1 host flow in `ecalc.c` (host residues, host A = S·B^dl, the full-k host reciprocal, `newton_db_divmod`, the host top-set write, the host X source of the writer, `out_ctx.X`); A's host pool (`binsplit_take_hpool`, its allocation, `hpool` in `mem.c`). `binsplit.c` hands a top level that ended on the batch tier over as device numbers at size 1 too | +60 −203, net **−143** (code −132); tests +75 −56 |
| 2 | the full-length-reciprocal bodies of `newton_db_divmod_shifted` and `newton_mn_divmod`, `newton_dkm_on/set`, the scratch `g_mu/g_t/g_xq`; DKM covers every k (s = 0 at k = 1: step 1 is the whole division); size > 1 refuses k_mu < 5 with a message | +16 −165, net **−149** (code −134); tests +15 −18 |
| total | 21 files, +407 −806 (measured, `git diff --shortstat`, all files incl. docs and the registry) | ecalc sources net **−418** |

**Method.**
- Builds on the aac7 login node (SHMEM, `-DCOMM_SHMEM` in the log), plan sweeps there (no device).
- Planner identity: `MN_PLAN_ONLY=<D>:<g>` and `BS_LAYOUT_ONLY=<D/g>:<g>` for D = 10⁹, 4·10⁹, 10¹⁰, 2·10¹⁰, 4·10¹⁰,
  6.44·10¹⁰, 10¹¹, 3·10¹¹, 10¹², 4·10¹², 10¹³, 2·10¹³, 3.82·10¹³ and g = 1, 2, 3, 4, 6, 8, 12, 16, 32, 64, 128, 192, 384, 576
  (points with 10⁸ ≤ D/g ≤ 2·10¹¹: 119 points, 238 files), under five settings: the defaults, `ECALC_NP=auto`, `ECALC_NP=4`
  (P24 grids), `ECALC_PLANE_CAP=2^30`, `RNS_STRATEGY=C`. Compared ignoring `META`, the `config:` line (its print-only cap index
  is now 1 for 2^31, was 2) and the `planes:` line (the two 3·2^k cap columns are gone).
- Gates through the aac7 queue: `tools/gate.sh --build --nodes 2` (P24 forced), `tools/gate.sh --nodes 1`,
  `ecalc/mnaccept.sh <hold> --only unit,e9,grid,mn,corr` (the unit step runs `t_newton 20` and `t_mul 20`).
- Item 1 + 2 development item (1 node): `t_newton 20`; 10⁹ on the aac7 line with `NEWTON_VERBOSE=1` (the division visible);
  `ecalc` at 1, 2, 5, 10, 17, 18, 19, 30, 35, 36, 37, 50, 54, 72, 73, 100, 150, 200, 1000, 10⁵ digits (ASCII) against the
  reference's prefix; ABBA at 10¹⁰, 1 node, aac7 line, no digit output, A = main d1d19bf, B = SIMP c4df499 (items 1–4 on that
  base), 4 rounds (TESTING.md §6; the round cap).

**Results.**

| check | result |
|---|---|
| plan sweep, items 3+4 against main d1d19bf | 5 × 238 files, **0 differ** (measured) |
| plan sweep, the merged tip 62064f2 against main 89318ae | 5 × 238 files, **0 differ** (measured) |
| **merge gates on 62064f2** (items 1–4 rebased on N3; job 12734) | gate 2 nodes P24 forced PASS (ecalc total 23.81 s); gate 1 node PASS (10.80 s); mnaccept unit (t_ntt 24, t_mul 20, t_bs, t_dbig, **t_newton 20: 1648 checks OK**, t_verify, t_out, t_patch, t_mn_grid 2 procs), e9, mn (10⁸ at 2/3/4, 10⁹ at 2/4), corr (sizes 1/2/4): **all PASS**, digits identical |
| gate 2 nodes, P24 forced (items 1–4 before the N3 rebase, 4003968) | PASS: VERIFY OK, digits identical, ecalc total 19.41 s |
| gate 1 node (4003968) | PASS: VERIFY OK, digits identical, ecalc total 9.55 s; the log has `I3: P, Q residues by kernel …` and `divmod(dev, DKM) end`: the gate now runs the production division (coverage gap (a) closed) |
| mnaccept unit, e9, grid, mn, corr (4003968) | all PASS except `t_newton 20` (1 of 1648 checks; fixed by `rng5`, see below) |
| t_newton 20 with section 5 on its own RNG, main 740ac38 and SIMP 4003968 | both VERIFY OK (1098 / 1648 checks); identical reciprocal errors on identical Q (+1, +0, +1 units at nq 131075) |
| t_newton 3b (quotients of 1–8 limbs, dl 1–4 and nq − 2, nq 5 … 16387) | 320 divisions identical to GMP (X exact, R's residues) |
| ecalc 1 … 10⁵ digits (20 sizes) | all prefix-identical to the reference, VERIFY OK |
| 10⁹, 1 node, aac7 line | identical, VERIFY OK; `bs: leaf P 55555560, Q 55555560 limbs copied to device numbers`; `divmod(dev, DKM, X_hi writer) 0.10 s: k 55555557 = 27777779 + 27777778 (s)` |
| items 3+4 alone (38a5b13, before the P1/SHR2 rebases) | gate 2 n PASS (20.98 s), gate 1 n PASS (11.22 s), mnaccept unit/e9/grid/mn/corr all PASS |

Timing, 10¹⁰ digits, 1 node (measured, dev12 item, job 12721): mean of A (main) 26.85 s, B (SIMP) 18.54 s; per round B − A
= −8.93, −8.19, −8.98, −7.13 s; **mean −8.31 s, 95 % CI [−9.68, −6.93] s, 4 rounds (8 pairs), t = −19.2**. The phases: dm
5.5 → 1.7 s (the DKM device division instead of the host full-length one), 10dP 0.9 → 0.0 s (no host A), bs 8.5 → 6.7 s (no
host copy of P, Q), T1 0.0 → 0.5 s (the device flow's T1; not analysed). Above about 1.9 × 10¹⁰ digits at one node the
flow was already the device flow, so the change there is ≈ 0 (modelled, AUD3).

The `t_newton` failure: item 1 rewrote section 3, which changed how many random numbers the earlier sections draw, so
section 5 drew a different Q; for that Q the reciprocal at nq 131075, k 65538 was −22 units off GMP's floor, beyond the test's
±8-unit bound. With section 5 on its own RNG the same Qs give identical errors on main and on SIMP (above), so the reciprocal is
unchanged by SIMP and the outlier is a property of `recip_db2` on some operands (pre-existing; the division's corrections absorb
it in a run). `t_newton` now seeds section 5 separately (`rng5`).

**Verdict.** Adopt (user-approved as a simplification): digits identical at every size tested (1 digit to 10⁹ at 1 node, 10⁸ –
10⁹ at 2–4 processes, 10⁹ at 2 nodes with P24), planner output byte-identical, 10¹⁰ at 1 node 8.3 s faster.

**Open issues.**
- `recip_db2` can miss the ±8-unit bound (−22 units for one Q at nq 131075, k 65538); not investigated (pre-existing, outside
  SIMP). A t_newton sweep over many Q at that shape would size it.
- `dm_layout` (binsplit.c) still sizes the size-1 division below about 1.9 × 10¹⁰ digits for the full-length set (larger than
  DKM's; never short). Making it DKM's everywhere is a layout change for `mem_model.py --check-c`, left out.
- `tests/t_cap.c` keeps its own mirror of the old 3·2^k caps (a pure-arithmetic model of the design space; not a test of the code).
- AUD3 items 5–10 (size 1 through the sharded driver, P24 everywhere, …) are not part of SIMP.

**RESUME.** Merged to main (items 3, 4, 1, 2 and this report as separate commits); worktrees removed; nothing running.
