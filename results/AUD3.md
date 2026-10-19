# AUD3 — where ecalc's algorithms and code structure can be simpler and smaller (read-only audit, 2026-10-12)

**Question.** Which duplicate code paths, special-case tiers, constant parameters and needless layers in `ecalc/` can be
merged or removed, and what each would cost in time and memory? This covers structure only. Scripts are SCR's audit, dead
code is AUD2's (`results/CODE_REDUCTION.md §AUD2`), and settings removal is TASKS S.

**Change / branch.** None: read-only. Base `main` 4f7a103; line numbers are on that commit. Lane A's `la-a5` (A5: removes
`ECALC_OVERLAP`, `NEWTON_DEVICE`, `newton_recip`/`newton_divmod`) shifts lines in `ecalc.c`, `newton_db.c` and
`binsplit.c` but leaves every item below in place; its commit message confirms item 1's host flow is kept.

**Method.** I read ALGORITHM.md, PLAN.md and the code (18 885 lines of `ecalc/*.c|h`, measured `wc -l`). Function line
ranges were found by a script that scans to the closing brace, so they are measured. Branch line counts inside functions
were counted by hand and are approximate. Time and memory effects are **modelled** from existing measurements in `results/`;
I made no runs. "0" means the removed path does not run at the defaults or launch lines (I traced this in the code).

## Coverage findings found on the way (not simplification; for the coordinator)

1. **The 1-node merge gate (10⁹) does not run the production single-node flow.** At size 1, P and Q go to the device
   flow only when the top binary-splitting level ran on the device tier: `ovl3 = ovl && bs_Pd.n` (`ecalc.c:610`), and
   `bs_Pd` is set only when 2·max_nl + 1 > 2³⁰ (`binsplit.c:1621`). Below about 1.9 × 10¹⁰ digits (n_Q ≈ 1.07 × 10⁹ limbs,
   modelled) the top level is batch tier. The run then takes the host flow: host residues, a host `A = S·B^dl`,
   `newton_db_recip` to the full k (not h ≈ k/2), and `newton_db_divmod` (`newton_db.c:241-313`), which is the
   **full-length-reciprocal division, not DKM**. Production at 6.44 × 10¹⁰ uses `newton_db_divmod_shifted` → DKM.
   `tools/gate.sh` runs 10⁹ (line 30). Only `mnaccept.sh`'s 10¹¹ standard step runs the production path. Item 1 fixes this.
2. **No aac7 run at ≤ 16 nodes uses the target's multi-node product form (P24, four primes).** `ec_np_planes`
   (`crt.c:88-94`) sizes pool 0 for four planes only when 2^(31 + ⌊log₂ g⌋) exceeds the three-prime bound 5.84 × 10¹⁰
   terms. That needs g ≥ 32; at the target (g = 576) it holds. `mn_p24_of` (`rns_dist.c:1786`) turns P24 on exactly when
   pool 0 holds four planes. So aac7 gates at 1–6 nodes run the 18-digit three-prime `mn_core` path, and the target runs the
   P24 path (`p24.h`, `p24_mn.h`, `p24_crt.c`, 32 `p24` branches in `mn_core`). No gate sets `ECALC_NP=4` or
   `ECALC_NP_AUTO_TERMS` (grep of `tools/gate.sh`, `ecalc/mnaccept.sh`, TESTING.md: none). P24 was last run on aac7 under
   that knob in `results/P2415.md`. Item 6 fixes this. A cheaper stopgap is one gate step with
   `ECALC_NP_AUTO_TERMS=1000000` at 2 nodes.
3. The merge gate runs 1, 2 or 4 nodes (`tools/gate.sh:40`), which are all powers of two. The target's top two tree levels
   (192, 576) use the general map, which only `mnaccept`/G515-type runs at 3 and 6 processes exercise.

## Ranked list (best five first)

Size: S ≤ 1 day, M 1–3 days, L > 3 days (assumed). "Lines" = lines removed net, from the measured ranges.

| # | item | what becomes simpler | files and lines (main 4f7a103) | lines | time / memory (modelled) | risk | how to verify | size |
|---|---|---|---|---|---|---|---|---|
| **1** | **One single-node division flow: drop the host-P/Q flow** | At size 1, the batch-tier top level hands P, Q to device numbers, as size > 1 already does (`binsplit.c:1749-1766`, condition `mn_size() > 1`). The host flow then goes, and every size-1 run takes the device DKM flow. This also fixes coverage finding 1. | `binsplit.c:1749` (condition), 1767-1770 (host copy), 196-204 + 869 + 1789 (`g_hpool`, `binsplit_take_hpool`); `newton_db.c:241-313` (`newton_db_divmod`), 226-238 (host-Q branches of `newton_db_recip`); `ecalc.c` non-`ovl3` branches: 619-626 (host top set), 633-636 (hpool as A), 641 (host residues), 676, 689-697 (host 10dP), 712, 728; `out_ctx.X` and the host source of `x_bg_src` | ≈ −120 | ≈ 0 at ≥ 1.9 × 10¹⁰ (that path already runs). Below that: one device-to-device copy of P, Q (≤ 17 GB at the edge, < 0.05 s at ~1 TB/s, mod); the division becomes DKM (−25.7 s at 10¹¹, `results/DIVISION.md §DKM15`, m), so small sizes get faster | low: the hand-over code exists and is gated at size > 1 | gate 1 n 10⁹ (the log must show the `I3:` line), `mnaccept` unit, e9, corr, recheck, full; `ecalc/tests/t_newton.c` sections that call `newton_db_divmod` move to `newton_db_divmod_shifted`; ecalc at 50, 100, 1000 digits | S |
| **2** | **One division algorithm (DKM only)** | The full-length-reciprocal division (one doubling more) survives as the non-DKM body of the device and sharded divisions. They are reached only for k < 4 (about d < 72 digits) or when tests call `newton_dkm_set(0)`. Tiny d can use the existing host `bi_divmod_school`. | `newton_db.c:454-516` (non-DKM part of `newton_db_divmod_shifted`, ≈ 60), 1146-1214 (non-DKM body of `newton_mn_divmod`, ≈ 68), `newton_dkm_on/set` and the `kr` choice (≈ 6); with item 1 `newton_db_divmod` is already gone | ≈ −130 | 0: DKM runs at every size above ~72 digits (`newton_db.c:454`, `:1145`) | low | `t_newton` (DKM against `bi_divmod_school` at small sizes), gate 1 n + 2 n, `mnaccept` e9, mn, corr; ecalc 1–200 digits against the reference prefix | S |
| **3** | **Drop the distributed tier's 3·2^k planes** (`dist3`, the 3·2³⁰ pool sizing, the 3·2^k values of the plane cap) | The default plane cap is `2^31` (`ecalc.c:401`), which sets `RNS_PLANES_3Q30=0`. The 3·2^k form-C transform therefore runs only when set by hand (README: `RNS_PLANES_3Q30` default 0, "the mapping costs more than the products gain"). The mn tier never uses it (`rns_dist.c:883`). The batch tier and form B keep their own 3·2^k lengths (`ntt3.c`, `RNS_R3`), which are untouched. | `rns_dist.c:157-239` (`dist_r3`, `plan3_get`, `k3_*` kernels, `dist3_fwd/inv`, ≈ 75), r3 branches of `dist_core` 711-771 (≈ 10), the `r3` parameter of `plane_pts` / `split_grid_cap`; `rns_mul.c:52-77` and the 3q30 sizing in `rns_init` (≈ 20); `ecalc.c:398-440` (the 3·2²⁹ / 3·2³⁰ rows of `ECALC_PLANE_CAP` and its `RNS_PLANES_3Q30` knob, ≈ 10); `binsplit.c:502, 730, 791`; `mn_plan.c:306, 353`. Switches: `DIST_R3`, `RNS_PLANES_3Q30`, two `ECALC_PLANE_CAP` values (`2^30`, `2^31`, `fit` stay) | ≈ −140 | 0 at the defaults and launch lines. The best case for the removed form was 80.8 ± 1.3 against 82.0 ± 0.3 s at 4 × 10¹⁰ on one node with the old size rule (`results/MEMORY.md §I` item 4, m), within one run's noise | low | gate 1 n + 2 n; `MN_PLAN_ONLY` and `BS_LAYOUT_ONLY` output identical at the defaults over a sweep; `ecalc/tests/t_cap.c`, `t_strategy.c` | M |
| **4** | **One grid planner and one grid loop** | There are six shape functions: `split_grid_cap`, `split_grid` (the B-form cost model), `db_grid_shape`, `mn_grid_shape`, `p24_split`, `p24_grid_shape`. All are the same i, j ≤ 32 search with a different cap, point function and cost, so they become one search with a (cap, points, cost) descriptor. `mul_grid` and `mn_grid` repeat the same piece loop, skip rule and cache-slot assignment (nA/nB), so they can share one loop over a "form piece" callback. `mn_plan.c` then asks one function. | `rns_dist.c:883-956` and 1775-1814 (shape functions, 66 lines); `mul_grid` 957-1013 (57); `mn_grid` 1815-1890 (76) | ≈ −60 | 0: same shapes and pieces, so digits and times are identical by construction | low (planner output is checkable without a device) | `MN_PLAN_ONLY` and `BS_LAYOUT_ONLY` byte-identical before and after on a sweep (10⁹ … 5 × 10¹³ × 1 … 576 nodes, `RNS_STRATEGY=auto`); `t_mn_grid`, `t_strategy`; gate 1 n + 2 n | M |
| **5** | **Size 1 through the sharded driver** (one reciprocal/division/output driver) | Size 1 keeps a parallel set of drivers: its own branch in `main`, its own writer machinery (`x_bg`, `x_hi`, the `newton_db_x*_hook`s), and its own division (`divmod_shifted_dkm`, `dkm_*_db`). With a one-node `mn_group` (comm_local) and `rns_mul_dist_mn*` at g = 1 dispatching to the dbig tier (`mul_grid`, so form B is kept), size 1 runs `newton_mn_divmod` and `mn_out_early_*`. `recip_db2` stays (the sharded chain starts with it). | `ecalc.c:58-131` (x_bg/x_hi, ≈ 70 net of `pq_bg`), 610-726 (size-1 dm branch, ≈ 117); `newton_db.c:194-205` (hooks), 322-516 (`dkm_*_db`, `divmod_shifted_dkm`, `newton_db_divmod_shifted`, ≈ 195), 226-238 | ≈ −390, +80 (g = 1 group and dispatch, views at g = 1) ≈ −310 | ≈ 0 if `mdb_shift` at g = 1 is a view, not a copy (it copies today: 29 GB per copy of Q at 6.44 × 10¹⁰, ~0.03 s each at ~1 TB/s, mod). **Memory is the risk:** the one-node peak at 6.44 × 10¹⁰ is laid out for the dbig flow (`DM_TIGHT`, the arena hole), and a copy of Q does not fit without the view | medium–high | 1-node ABBA at 6.44 × 10¹⁰ (stop rule per TESTING.md), `mem_report` peaks against the plan, gate 1 n + 2 n, `mnaccept` (corr, recheck, patch), `t_patch` | L |
| 6 | P24 as the only multi-node product form | At size > 1 pool 0 always holds four planes, so every mn product runs P24. This removes the 18-digit side of `mn_core` (the added operand in the CRT, `seq_start` paths), `mn_grid_shape`, and the full-slot cache branch of `mn_grid` (`!p24 && !pc`). aac7 then gates the target's form (coverage finding 2). | `rns_dist.c` `mn_core` 1514-1745 (non-P24 sides, ≈ 35), 1775-1781, 1830-1834; `crt.c:88-94` (auto for g > 1) | ≈ −50 | Time ≈ equal per product: 4 primes × ¾ points against 3 primes × 1 point, with the same exchange bytes and a costlier four-prime Garner (mod; **unmeasured at aac7 sizes**). Memory +4 GiB per APU in pool 0 (one 2²⁹-limb plane, mod) at 2–16 nodes; check the fit at 6.44 × 10¹⁰/node with `BS_LAYOUT_ONLY` | medium | `BS_LAYOUT_ONLY` fit; 2-node ABBA at 6.44 × 10¹⁰/node; gate 2 n + 4 n | M |
| 7 | `tree_level` folded into `tree_level_k` | `tree_level_k`'s own comment says that for nch = 2 it is `tree_level`'s pair of products with the same operands in the same order. The frees match too (P_i after the first product, the shares after the second). | `mn.c:453-474` (22), the dispatch at 412 | ≈ −25 | 0 | low (the log line format changes: check the parsers, e.g. `calib_table.py`) | gate 2 n + 4 n digits; `mem_live` lines unchanged | S |
| 8 | One output-writer machinery (a sub-step of 5, doable alone) | Size 1 starts its writer through `x_bg`/`x_hi`. Size > 1 does the same job with `mn_out_early_start` / `mn_out_early_hi_start` / `mn_out_early_lo_release`. Size 1 can call the latter on a g = 1 view of its dbig X. | `ecalc.c:58-131`, the hook calls in `newton_db.c:385-437` | ≈ −70 | 0 (same writer underneath: `mn_out_run`) | medium (patch/defer paths) | `t_patch`, `mnaccept` corr, recheck; gate 1 n | M |
| 9 | `mdb_add_shifted`: one implementation, no shift parameter | Two bodies exist: the single-shot body and `mdb_add_shifted_rounds`. Both callers pass k = 0 (`rns_dist.c:1838, 1879`, measured). The rounds body can serve every size (K = 1 when the window fits), and k can go. | `rns_dist.c:2196-2338` (121 + kernels) | ≈ −50 | 0 (the single-shot is the K = 1 case) | low | `t_mn_grid`, gate 2 n | S |
| 10 | One transform cache | There are two slot mechanisms. Full slots (`cache_avail`, own hipMalloc, sized by the budget) serve `mul_grid` and `mn_grid` with `RNS_DIST_CACHE_PARTIAL=0`. Partial slots (`cache_pc_take`, from the block pool) are the mn default. At the target the full slots get 0 slots (`rns_dist.c` TC comment, `results/TC15.md`). Using partial slots in `mul_grid` too leaves one mechanism. | `rns_dist.c:273-481` (cache section, ≈ 200), `cache_avail` 301-336, `cache_pc_take` 408-450 | ≈ −60 | Unknown at 1 node (the full slots serve the single-node grids at 6.44 × 10¹⁰): needs a 1-node ABBA | medium | 1-node ABBA at 6.44 × 10¹⁰; gate 1 n + 2 n | M |

## Decisions for the user (simplifications that remove a capability)

| # | item | lines | cost / benefit |
|---|---|---|---|
| 11 | Write only the 60-bit packed form in the run (`ECALC_OUT_PACKED=2`). ASCII (=0) and 64-bit packed (=1) writers, patchers and recheck readers go; `tools/unpack_digits` makes ASCII off the clock. | ≈ −120 in `mn_out.c` (`fmt_limbs` 97, the ASCII branch of `out_run_one` 377-493, `patch_bytes` 644-668, the ASCII part of `mn_out_tail_core` 720-780, recheck readers 864-963; split estimated) | **+** one format to patch and recheck. **−** no direct ASCII file from a run, and old =1 files cannot be rechecked. Already on the fallback review list of 2026-11-11 (TASKS S). |
| 12 | Checkpoints: keep only the top-level set (needed for `ECALC_RECHECK`), drop the leaf-level and tree-level restart sets. | `binsplit.c:870-1345` (≈ 475 lines of checkpoint code; ≈ 350 of it leaf/tree restart, est.), `mn.c` tree sets (`mn_ckpt_tree_level` 331-357, the restart part of `mn_tree`), `ecalc.c` (34 lines mention them) | **+** the largest single block of optional code. **−** no restart inside a run; the target run is modelled at ~182 s, so a restart saves at most that. Off by default today; record runs leave `ECALC_CKPT_TOP` off (user decision 2026-09-27). |
| 13 | The SHMEM data path of device exchanges (used only with `COMM_OFI=0` or without a cxi NIC): `rounder`, `alltoallv_rounds`, `push_peers`, staging in the SHMEM pool. | `comm_shmem.c:521-537, 614-691` and staging (≈ 120, est.) | **+** one data path. **−** no fallback if libfabric fails on the target; SHMEM still does setup, collectives and signals. Keep at least until the target's A3 stage has run. |

## Smaller items (each S, ≤ 20 lines)

- `mn_group`'s transform-node subset (`gt`, `tr`, `tr2`) is a leftover of the removed `MN_TOPO_GROUP`. The product path
  uses `G->all` (`lay_get`, `rns_dist.c:1084`); `tr` remains only in `mn_selftest_layered` (`mn.c:236-257`) and
  `mn_mesh2(…, sub = 1)`.
- `mn_prod_cut_x1` (`newton_db.c:841-856`) moves the division's products onto a subgroup. Its own comment says this
  never happens at the target's sizes (both products ≥ 2 n_Q points); the reciprocal's X1 steps stay.
- Two start-up self-tests, `mn_selftest` (51) and `mn_selftest_layered` (57, `mn.c`), run at every multi-node start. The
  layered test covers the same meshes plus the layered stack; check what only the first catches before merging them.

## Considered and not recommended

- **The general map for every group size** (dropping the power-of-two `ntt_dist` path): the general map's exchange is
  2.3–2.4× slower per APU thread at 6 nodes (`results/TIME_MODEL.md §S48`, m), so the cost is far above 1 %.
- **One batch form** (dropping the striped batch for the locality-aware one): the striped form serves the few, large
  products of the upper batch levels, where per-device locality would leave APUs idle (`rns_mul.c:856-880`).
- **NTT kernels** (`k_b16` partial passes beside `k_b16r`): `plan_auto` needs the partial passes (`results/K13b.md`);
  the kernel set is already minimal.

## Handed to AUD2 (dead code seen, not counted above)

- The host product tier has no production caller once A5 lands: `rns_mul`, `rns_mul_low`, `rns_mul_mdev`,
  `rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`, `mul_rec`, `mdev_gpu_crt` (`rns_mul.c`, ≈ 270 lines, measured function ranges). The
  only caller is `binsplit.c:1680-1700`, and that branch cannot run: `dev_mdev = mdev_level` (`binsplit.c:1622`) and the
  `else` sits under `!batch` ⇒ `mdev_level`. Tests (`t_mul`, `t_crt`, `t_newton`, `t_dbig`, `t_mn_grid`) still use
  `rns_mul` as a reference product, so it needs a test-side replacement (`bi_mul` or GMP). AUD2's name scan missed it
  because tests call it.
- `ckpt_hdr.mdev_host` and the "older checkpoint (host mdev tier)" reader (`binsplit.c:1203-1210`).
- `mn_out_scatter_standin` (`mn_out.c:72-87`; noted unreachable in `results/CODE_REDUCTION.md §W2A`).

## Verdict

Do items 1–4 first. Each is S/M, has no time cost at the defaults, and all four together remove about 450 lines. Items 1
and 2 also make the 1-node gate run the production division. Item 5 removes the most code (~310 lines) but needs a 1-node
timing and memory check, so do it after item 8 (its low-risk part). Item 6 is mainly a test-coverage fix for the target's
product form: at least add the stopgap gate step now. Items 11–13 are the user's decisions.

**Open issues.** Branch line counts are approximate (hand-counted); function ranges are measured. Timing effects marked
"unknown" (items 6, 10) need ABBA rounds; no run was made. The 1.9 × 10¹⁰-digit edge of coverage finding 1 comes from the
code's tier rule, not from a log.

**RESUME.** Done: this report (read-only). Next: the coordinator schedules items (TASKS class 3) and the coverage stopgaps
(findings 1–2). Each item is its own branch from `main`, gated per TESTING.md.
