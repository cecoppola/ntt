# CODE_REDUCTION — shrinking the code

Scope: shrinking the code: audits, shrink passes, settings removal waves, validation of the reduction, the limb-format study and the host tier removal. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| AUD2 | removal / consolidation audit of C/HIP sources, tests and Makefile (read-only, 2026-10-12) |
| AUD3 | where ecalc's algorithms and code structure can be simpler and smaller (read-only audit, 2026-10-12) |
| SIMP | AUD3 items 1–4: one single-node division flow, DKM only, no 3·2^k distributed planes, one grid planner |
| SH3 | shrink pass 3: rns_dist.c dead items, duplicated helpers, the host product tier (2026-10-12) |
| SHR2 | shrink pass 2: top-level checkpoints only, the unreachable stand-in, SOS/OSHMEM builds, AUD2 re-scan (2026-10-12) |
| W2A | wave 2, lane A of the user-approved settings removal (2026-10-12) |
| W2B | wave 2, lane B of the settings removal (2026-10-12) |
| L2 | validation of the code reduction (B02) at full size and for speed, aac6 (2026-10-11) |
| O8 | TASKS backlog clean-up (local only, no cluster use; 2026-10-11, branch o8 off main 0298fe2) |
| H1 | PLAN.md 28.4 measurement, single-node part (decimal vs binary limbs, no code changed) |
| H2 | PLAN 28.4, multi-node part: t_mn_grid in decimal vs binary limbs on aac7 (2026-10-11) |
| H4 | PLAN 28.5 implemented on branch `h4`: the binary limb pipeline archived, decimal limbs only |
| H5 | decimal (LIMB_BASE=10) vs binary (LIMB_BASE=2) limb pipeline, paired wall clock |
| TREF | a test reference product, and the host product tier removed |

## AUD2 - removal / consolidation audit of C/HIP sources, tests and Makefile (read-only, 2026-10-12)

(former file: AUD2.md)

Base: `main` 4f7a103 (lane B of the settings removal already merged; lane A still open in worktrees). Nothing was edited.
Labels: m measured (wc / scripted scan), a assumed. Method: a scripted scan of every function definition in
`ecalc/*.c|h` (comments stripped) for callers in `ecalc/*.c|h`, `ecalc/tests/*`, `tools/*.c` (transitive: a function
whose only callers are dead functions counts as dead); a scan of `ecalc/tests/` against TESTING.md, `tools/gate.sh`,
`ecalc/mnaccept.sh`, `ecalc/accept.sh`, `results/*.md`; Makefile rules; duplicate function bodies (whitespace-normalised
hash); `#if 0`, `#ifdef` blocks and `#define`s used once; header declarations without definition. "Cited" = the name
appears in a `results/*.md`.

Not covered here (lane A / SCR): switches still being removed (W2A A2/A5/D5 etc.), scripts. Line counts exclude the comment
blocks above a function (unmeasured, small). Lines removed are counted once; all items independent unless noted.

**Total removable, measured: about 1 620 lines (items 1-9), plus up to 790 lines of long comment blocks (item 10, upper bound).**
Code in `ecalc/*.c|h` is only ~290 of these (items 4, 5, 6, 8, 9 partly); the bulk is tests / on-demand probes.

#### Ranked list (largest safe removal first)

| # | item | files / symbols | lines (m) | evidence of no use | risk | how to verify |
|---|---|---|---|---|---|---|
| 1 | Test programs with no Makefile rule, not in TESTING/gate/accept | `tests/t_nicbw.c` 259 (libfabric NIC-bandwidth probe, README of NIC16 cites it), `tests/t_a2a_sched.c` 140 (C02 schedule probe, comm_shmem peer-order switch it served is gone: B1), `tests/t_school.c` 49 (WP1 item, H4 cites), `tests/n3x_lengths.py` 28 (no citation anywhere) | 476 | no `Makefile` target, no caller in gate.sh / mnaccept.sh / accept.sh / TESTING.md; each cited at most by its own experiment's results file (n3x_lengths: none) | low | `make` (all targets still build); doccheck 0. Move to `archive:` first (results files then say "archived") |
| 2 | CPU seed-leaf microbench | `tests/b_seed64.c` 271 + Makefile rule `tests/b_seed64` 3; has its own copy of `e_terms` (same body as binsplit.c) | 274 | not in TESTS, not in TESTING/gate/accept; only S21/S22/H1/H4 results (decision made: seed leaf adopted, switch BS_SEED_PULL default 1) | low | `make`; `t_seed` unchanged |
| 3 | GPU gather micro-probes | `tests/t_gather_probe.c` 97, `t_gather_raw_probe.c` 83, `t_gather_p2_probe.c` 82 + 3 Makefile rules (~9); the three share identical `k_diff` / `k_fillsrc` bodies | 271 | "built on demand, not in TESTS"; cited only by S40/S50/O8, K1b/CAND, P2 (one-off experiments with kernel *copies*, no product code) | low | `make`; none feeds a gate |
| 4 | Pack/unpack benchmark + reference kernels (test-only code in production objects) | `ntt_dist.c`: `dist_pack_bench` 48, reference kernels `k_pack`, `k_pack_t`, `k_pack_tiled`, `k_twpack`, `k_unpack_rows`, `k_unpack_t`, `k_unpacktw`, `cmp_dev` (≈ 70, plain forms kept only "as dist_pack_bench's reference"); `rns_dist.c`: `gen_pack_bench` 53, `k_twpack_g`, `k_unpacktw_g`, `gb_cmp`, `gb_dcmp` (≈ 42); the two env blocks `DIST_PBENCH` / `DIST_GBENCH` of `tests/t_dist.c` (≈ 14) | 226 | no caller in the pipeline; only `t_dist` with `DIST_PBENCH` / `DIST_GBENCH` set, which TESTING.md / gate / accept never set (G515 cites `gen_pack_bench` as a one-off measurement; the tiled/recurrence kernels are the production ones) | low-med: touches `ntt_dist.c`, `rns_dist.c` (lane B files, merged) | `tools/gate.sh --build` 1 n + 2 n; `t_dist` default run VERIFY OK; remove together with the t_dist blocks |
| 5 | Test-only API kept alive only by tests (needs the tests edited) | `ntt.c`: `ntt_host_fwd` 19, `ntt_host_inv` 20, `ntt_pass` 10, `ntt_pass_at` 9, `ntt_pass_bounds` 6 (+5 decls; `t_ntt` timing blocks only); `rns_dist.c` `rns_mul_dist` 24; `verify.c` `vf_digits_mod` 23; `ntt_dist.c` `dist_inv_t*` 20; `modarith.h` `ec_shoup_lazy` 5; one-liners `crt_garner3/4`, `db_pool_extents`, `db_mod_q`, `db_set_base_pow`, `newton_dkm_set` (decls included) | 165 | callers only in `ecalc/tests/` (counts 1-4 each, scripted); none on the default or any switch path | med: the tests that call them must be trimmed in the same commit; per-function check needed (some are the oracle of a unit test) | `make` + `accept.sh` (t_ntt, t_mul, t_dist, t_verify, t_crt) VERIFY OK; gate 1 n |
| 6 | Functions with no caller anywhere (production) | `rns_mul.c`: `rns_ensure_staging` 5; `rns_dist.c`: `rns_mul_dist_hd` 18 (self-recursive only), `acc_flat` 1, `part_owner` 1; `dbig.c`: `db_pregrow` 9, `db_share_add_spills` 5, `db_pool_largest_free`, `db_pool_tail_bytes`, `db_pool_tail_stats`, `db_pool_vmm_stats` (1 each); `comm_layered.c`: `comm_layered_create_minor` 8 (W2A already says "left for lane B"); `bigint.c`: `bi_mul_school` 8, `bi_sub_shifted` 7, `bi_bits` 5; `binsplit.c`: `binsplit_dm_hole_bytes` 1; `verify.c`: `tier1_res` 2, `tier1_digits_res` 6; `ntt_dist.c` kernel `k_twiddle`, `k_twiddle_t` 22; `modarith.h`: `ec_add`, `ec_sub`, `ec_shoup`, `ec_shoup_pre` 10 (declarations in headers counted) | 126 | definition has no reference in any `.c|.h` outside its own body and its header declaration; no test, tool or script references (grep -w) except prose in results/M11, M, A-grid (history only) | low (compiler error if wrong) | `make` clean build (hipcc, `-Wall`), gate 1 n; DB_POOL_VERBOSE output uses none of them |
| 7 | `tests/t_bounce.c` (O2 microbenchmark) | file 39 + Makefile rule 2 | 41 | "not in TESTS"; cited only by O2/CAND | low | `make` |
| 8 | Duplicated helpers | `rns_mul.c k_store` = `dbig.c k_copy_h2d` (4); `rns_mul.c dmod128` = `p24_crt.c p24_dmod128` (4); `ntt_dist.c dev_pow_table` = `rns_dist.c pow_table` (6); `dbig.c powmod_h` = `newton_db.c mn_powmod` (1); `rns_dist.c k_canon_pass` (4); tests: `make_mdb` (t_mn_grid = cx_grid, 10), `digits_format` (t_out = t_patch, 10), `verify_done` (t_roots = harness.h, 5) | ~26 net (a: one copy kept) | identical normalised bodies (md5) in 2+ files | low-med: sharing a `static inline` helper in an existing header (modarith.h / mem.h / harness.h) changes no code path; kernels must stay `__global__` in one TU | gate 1 n; `t_mul`, `t_p24` |
| 9 | NOP-modmul bench build | Makefile `ntt_nop.o` + `tests/t_ntt_nop` rules 6; two `#ifdef NTT_NOP_MODMUL` blocks in `ntt.c` ≈ 9 | ~15 | "output wrong by design; only timing is read", on demand, S21/A37CMP result is final | low | `make`; `t_ntt` hash unchanged |
| 10 | Long history comment blocks (not counted in the total) | comment blocks of ≥ 10 lines: `rns_dist.c` 115, `binsplit.c` 82, `comm_shmem.c` 57, `ntt_dist.h` 37, `ntt.c` 33, `ecalc.c` 27, `newton_db.c` 26, `comm_layered.c` 26 ... total 787 in `ecalc/*.c|h` (m); about 990 comment lines carry "Phase NN / S-number / results/ / removed 2026-.." tags | up to ~790 (upper bound; most also explain the algorithm) | - | low per line but judgement needed (the comments are the design record; ALGORITHM.md should own it) | doccheck; no code change; compile |

#### Checked and clean (no action)

- `#if 0`: none. `#ifdef` forks still live: `COMM_HOST_ONLY` (t_comm host build), `MN_OUT_LLAPI` (Lustre, build option), `EC_PRIMES` (modarith.h:72-74 hard-wired 1; the 0 branch is the Phase 3-7 prime set, used only by `t_roots` printout - candidate for removal, size not measured, would need `t_roots`/`t_primes` edits; left to a prime-set owner), `NTT_NOP_MODMUL` (item 9).
- Header declarations without definition: none (only `comm_layered_vbytes`, defined as `extern "C"` in comm_layered.c).
- Makefile: no rule for a missing source; every header named exists. Rules "for nothing" are only the on-demand ones in items 2, 3, 7, 9. `TESTS` (built by `make all`) contains tests not named in TESTING / gate / accept / mnaccept: `t_roots`, `t_alloc`, `t_copy_order`, `t_hred_race`, `t_cap`, `t_strategy`, `t_seed`, `t_primes` (TESTING only), `t_ntt3` (TESTING only). They are cited as evidence (S13, R, A114, A11, W2B `t_strategy 24 2 V`, H1/H4) so they are NOT proposed for removal; but TESTING.md should list `t_strategy` (used as a gate in W2B) - doc gap, not code.
- `#define`s with one use and unused static globals: none found (scripted).
- Possibly stale tracked data/scripts in `ecalc/tests/` (cx_*, pc_*, plan_*, std17/ofi17/ofitune17/d16 drive scripts, ~700 lines) belong to the scripts audit (SCR); each is cited only by its own experiment result.

#### Overlaps and order

- Items 4 and 6 touch `ntt_dist.c`, `rns_dist.c`, `dbig.c`: do them after lane A's remaining batches (A5 `ECALC_OVERLAP`/`NEWTON_DEVICE` will free more callers; rerun the scan then - expect a few more orphans in `ecalc.c`, `newton.c`, `binsplit.c`).
- Items 1, 2, 3, 7, 9 touch only `tests/` and the Makefile: can go any time, no gate beyond `make` + doccheck. Result files that cite a removed program should say `archive:ecalc/tests/<name>` (move, do not delete).
- The scan is name-based: a function referenced only through a macro-pasted name or a string would be missed (I saw none); the compiler is the final check.

## AUD3 — where ecalc's algorithms and code structure can be simpler and smaller (read-only audit, 2026-10-12)

(former file: AUD3.md)

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

#### Coverage findings found on the way (not simplification; for the coordinator)

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
   that knob in `results/PRIMES_STRATEGY.md §P2415`. Item 6 fixes this. A cheaper stopgap is one gate step with
   `ECALC_NP_AUTO_TERMS=1000000` at 2 nodes.
3. The merge gate runs 1, 2 or 4 nodes (`tools/gate.sh:40`), which are all powers of two. The target's top two tree levels
   (192, 576) use the general map, which only `mnaccept`/G515-type runs at 3 and 6 processes exercise.

#### Ranked list (best five first)

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
| 10 | One transform cache | There are two slot mechanisms. Full slots (`cache_avail`, own hipMalloc, sized by the budget) serve `mul_grid` and `mn_grid` with `RNS_DIST_CACHE_PARTIAL=0`. Partial slots (`cache_pc_take`, from the block pool) are the mn default. At the target the full slots get 0 slots (`rns_dist.c` TC comment, `results/CACHE.md §TC15`). Using partial slots in `mul_grid` too leaves one mechanism. | `rns_dist.c:273-481` (cache section, ≈ 200), `cache_avail` 301-336, `cache_pc_take` 408-450 | ≈ −60 | Unknown at 1 node (the full slots serve the single-node grids at 6.44 × 10¹⁰): needs a 1-node ABBA | medium | 1-node ABBA at 6.44 × 10¹⁰; gate 1 n + 2 n | M |

#### Decisions for the user (simplifications that remove a capability)

| # | item | lines | cost / benefit |
|---|---|---|---|
| 11 | Write only the 60-bit packed form in the run (`ECALC_OUT_PACKED=2`). ASCII (=0) and 64-bit packed (=1) writers, patchers and recheck readers go; `tools/unpack_digits` makes ASCII off the clock. | ≈ −120 in `mn_out.c` (`fmt_limbs` 97, the ASCII branch of `out_run_one` 377-493, `patch_bytes` 644-668, the ASCII part of `mn_out_tail_core` 720-780, recheck readers 864-963; split estimated) | **+** one format to patch and recheck. **−** no direct ASCII file from a run, and old =1 files cannot be rechecked. Already on the fallback review list of 2026-11-11 (TASKS S). |
| 12 | Checkpoints: keep only the top-level set (needed for `ECALC_RECHECK`), drop the leaf-level and tree-level restart sets. | `binsplit.c:870-1345` (≈ 475 lines of checkpoint code; ≈ 350 of it leaf/tree restart, est.), `mn.c` tree sets (`mn_ckpt_tree_level` 331-357, the restart part of `mn_tree`), `ecalc.c` (34 lines mention them) | **+** the largest single block of optional code. **−** no restart inside a run; the target run is modelled at ~182 s, so a restart saves at most that. Off by default today; record runs leave `ECALC_CKPT_TOP` off (user decision 2026-09-27). |
| 13 | The SHMEM data path of device exchanges (used only with `COMM_OFI=0` or without a cxi NIC): `rounder`, `alltoallv_rounds`, `push_peers`, staging in the SHMEM pool. | `comm_shmem.c:521-537, 614-691` and staging (≈ 120, est.) | **+** one data path. **−** no fallback if libfabric fails on the target; SHMEM still does setup, collectives and signals. Keep at least until the target's A3 stage has run. |

#### Smaller items (each S, ≤ 20 lines)

- `mn_group`'s transform-node subset (`gt`, `tr`, `tr2`) is a leftover of the removed `MN_TOPO_GROUP`. The product path
  uses `G->all` (`lay_get`, `rns_dist.c:1084`); `tr` remains only in `mn_selftest_layered` (`mn.c:236-257`) and
  `mn_mesh2(…, sub = 1)`.
- `mn_prod_cut_x1` (`newton_db.c:841-856`) moves the division's products onto a subgroup. Its own comment says this
  never happens at the target's sizes (both products ≥ 2 n_Q points); the reciprocal's X1 steps stay.
- Two start-up self-tests, `mn_selftest` (51) and `mn_selftest_layered` (57, `mn.c`), run at every multi-node start. The
  layered test covers the same meshes plus the layered stack; check what only the first catches before merging them.

#### Considered and not recommended

- **The general map for every group size** (dropping the power-of-two `ntt_dist` path): the general map's exchange is
  2.3–2.4× slower per APU thread at 6 nodes (`results/TIME_MODEL.md §S48`, m), so the cost is far above 1 %.
- **One batch form** (dropping the striped batch for the locality-aware one): the striped form serves the few, large
  products of the upper batch levels, where per-device locality would leave APUs idle (`rns_mul.c:856-880`).
- **NTT kernels** (`k_b16` partial passes beside `k_b16r`): `plan_auto` needs the partial passes (`results/KERNELS.md §K13b`);
  the kernel set is already minimal.

#### Handed to AUD2 (dead code seen, not counted above)

- The host product tier has no production caller once A5 lands: `rns_mul`, `rns_mul_low`, `rns_mul_mdev`,
  `rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`, `mul_rec`, `mdev_gpu_crt` (`rns_mul.c`, ≈ 270 lines, measured function ranges). The
  only caller is `binsplit.c:1680-1700`, and that branch cannot run: `dev_mdev = mdev_level` (`binsplit.c:1622`) and the
  `else` sits under `!batch` ⇒ `mdev_level`. Tests (`t_mul`, `t_crt`, `t_newton`, `t_dbig`, `t_mn_grid`) still use
  `rns_mul` as a reference product, so it needs a test-side replacement (`bi_mul` or GMP). AUD2's name scan missed it
  because tests call it.
- `ckpt_hdr.mdev_host` and the "older checkpoint (host mdev tier)" reader (`binsplit.c:1203-1210`).
- `mn_out_scatter_standin` (`mn_out.c:72-87`; noted unreachable in `results/CODE_REDUCTION.md §W2A`).

#### Verdict

Do items 1–4 first. Each is S/M, has no time cost at the defaults, and all four together remove about 450 lines. Items 1
and 2 also make the 1-node gate run the production division. Item 5 removes the most code (~310 lines) but needs a 1-node
timing and memory check, so do it after item 8 (its low-risk part). Item 6 is mainly a test-coverage fix for the target's
product form: at least add the stopgap gate step now. Items 11–13 are the user's decisions.

**Open issues.** Branch line counts are approximate (hand-counted); function ranges are measured. Timing effects marked
"unknown" (items 6, 10) need ABBA rounds; no run was made. The 1.9 × 10¹⁰-digit edge of coverage finding 1 comes from the
code's tier rule, not from a log.

**RESUME.** Done: this report (read-only). Next: the coordinator schedules items (TASKS class 3) and the coverage stopgaps
(findings 1–2). Each item is its own branch from `main`, gated per TESTING.md.

## SIMP — AUD3 items 1–4: one single-node division flow, DKM only, no 3·2^k distributed planes, one grid planner

(former file: SIMP.md)

**Question.** Can the four simplifications of `results/CODE_REDUCTION.md §AUD3` (items 1–4) be made with byte-identical digits and no
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

**Final gate on the merged commit (rebased onto main 68a9d69 with the P24 default and DM_MN_WRAP; measured, aac7, 2026-10-12).**
`tools/gate.sh --build --nodes 2` (default P24, log shows "pool 0: 4 primes") PASS, 1e9 digits identical, ecalc 22.27 s;
`--nodes 1` PASS, identical, 10.63 s; `mnaccept.sh --only unit,e9,grid,mn,corr`: 18 passed, 0 failed (unit tests, 1e9 at 1 node,
2 and 4 processes, corrections at sizes 1, 2, 4); `MN_PLAN_ONLY=38200000000000:576` prints its plan; the rebase had no
conflicts, the registry regenerated unchanged, `mem_model.py --check-c` exits 0, doccheck 0.

**RESUME.** Merged to main (items 3, 4, 1, 2 and this report as separate commits); worktrees removed; nothing running.

## SH3 — shrink pass 3: rns_dist.c dead items, duplicated helpers, the host product tier (2026-10-12)

(former file: SH3.md)

**Question.** Finish the SHR2 follow-ups: remove the `rns_dist.c` items SHR2 skipped, merge duplicated helpers where safe, and
decide whether the host product tier can go. No switch added, no default changed, no digit changed.

**Branch.** `sh3` from main 5081105. Other open branches while this ran: adopt (rns_mul.c, binsplit.c, newton_db.c), k4c
(p24_crt.c), recip (newton_db.c, t_newton.c): hunks kept small.

#### What was removed (lines, measured: `git diff --numstat`)

| item | result | lines (-/+) |
|---|---|---|
| `gen_pack_bench`, `gb_cmp`, `gb_dcmp`, the plain reference kernels `k_twpack_g` / `k_unpacktw_g`, the `DIST_GBENCH` block of `t_dist`, rows `DIST_GBENCH` / `DIST_PREPS` (only read by it) | removed | rns_dist.c -99 (38 plain kernels + 61 bench), t_dist.c -8, registry -3 (README row edited) |
| `pow_table` (duplicate of ntt_dist.c `dev_pow_table`; only the bench used it), `part_owner` (only the plain kernels used it) | removed | rns_dist.c -7 and -1 |
| `rns_mul_dist_hd` (self-recursive only, no caller) | removed | rns_dist.c -18, rns_mul.h -1 |
| `acc_flat`, `rns_mul_dist` | **kept**: `rns_mul_dist` has test callers (`t_mul`, `t_primes` tier 1: the host-bigint path of the distributed tier); `acc_flat` is its helper | 0 |
| `k_store` (rns_mul.c) = `k_copy_h2d` (dbig.c) | merged: one kernel, `db_copy_kernel` (dbig.c) launches it for rns_mul.c (same grid, block, stream, so the launch is unchanged) | rns_mul.c -9/+3, dbig.c/h +2 |
| `make_mdb` (t_mn_grid.c = cx_grid.c) | merged by archiving `tests/cx_grid.c` (no Makefile rule since H4) and `cx_shapes_c25.txt`, `cx_shapes_c27.txt` to `archive:removed_2026-10-12/sh3/` (byte-identical) | tests -115 (97 + 9 + 9) |
| `mn_powmod` (newton_db.c) = `powmod_h` (dbig.c) | **skipped**: branch `recip` edits newton_db.c (11 lines); do after RECIP merges (one line each: export `db_powmod_h`, drop the copy) | 0 |
| `dmod128` / `dmod192` (rns_mul.c vs p24_crt.c) | **waits** for K4c (p24_crt.c is not to be touched) | 0 |
| host product tier (≈270 lines) | **kept**, design below | 0 |

Total: rns_dist.c 2284 -> 2155 lines (-129 net, +1 comment line edited); all files together -261 net (9 added, 270 removed, before this file) (`git diff --shortstat main..sh3`).

#### Host product tier: why it stays, and the design that would let it go

Users of `rns_mul` as a reference product: `t_mul` (also benchmarks the tier itself and tests `rns_mul_low`, `rns_mul_pair`),
`t_dbig` (grid products up to 30e6 x 7e6 limbs), `t_mn_grid` (up to ~0.9 x 2^cap limbs per operand), `t_newton`, `t_primes`
(tier 0). Replacing it by GMP is not small: the decimal limbs go to GMP through a string (`harness.h`) which is quadratic-ish
above 2^20 limbs, and `mpz_mul` of 5e8-digit operands is tens of seconds per product, times the dozens of shapes. So a
GMP reference cannot cover the large grid tests; and `t_mul`'s tier-specific tests would be deleted, not ported.

Design (proposed, not done; ~1 task, needs its own gate run):
1. **Residue check as the large-size reference.** For C = A B check `C mod q = (A mod q)(B mod q) mod q` for 3 fixed primes q
   (61-bit) using the existing `vf_digits_mod` / `db_mod_q` (O(n), no product), plus the exact low 64 limbs (schoolbook) and
   the top limbs (from a floating or GMP product of the leading 64 limbs). A wrong digit survives only with probability about
   2^-60 per prime; structured carry errors (a limb moved by B^k) change the residue unless q divides B^k - 1.
2. **GMP below 2^20 limbs** through a product-tree base conversion (O(M(n) log n)) in `harness.h`, for the small and
   boundary cases (x * 0, x * 1, all-ones, 2^k +- 1) where an exact reference is cheap.
3. Then `rns_mul`, `rns_mul_low`, `rns_mul_mdev`, `rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`, `mul_rec`,
   `mdev_gpu_crt` go; `t_mul`'s timing section is dropped (its numbers live in results/); `t_primes` tier 0 moves to the
   device path at two transform sizes (`DIST_LOGN_TEST`).
Risk: the tests lose an independent code path (the host tier shares the primes and CRT but not the distributed kernels), so
step 1's residue check should run on the device path against a second device run at another plane size before the tier goes.

#### Gates (aac7 queue, measured)

On the sh3 branch (aac7 queue item, 2 nodes, hold released by the runner; logs on aac7 under `~/sh3q/`, raw, not in git):
`tools/gate.sh --build --nodes 2` PASS (SHMEM build, P24, 1e9 digits identical, ecalc 21.91 s); `--nodes 1` PASS (identical, 10.49 s);
`mnaccept.sh --only unit,e9,mn`: 15 passed, 0 failed (unit incl. t_ntt 24, t_mul 20, t_dbig, t_newton 20, t_mn_grid 2 procs; e9;
mn 1e8 at 2/3/4, 1e9 at 2/4), digits identical. doccheck 0 problems.

#### RESUME

State: see the gates above; merge, push, TASKS row SH3, remove worktrees. Open: `mn_powmod`/`powmod_h` (after RECIP),
`dmod128`/`dmod192` (after K4c), the host tier (design above).

## SHR2 — shrink pass 2: top-level checkpoints only, the unreachable stand-in, SOS/OSHMEM builds, AUD2 re-scan (2026-10-12)

(former file: SHR2.md)

**Question.** Remove the code the user approved for removal on 2026-10-12, without changing a digit: D11 (keep only the
top-level checkpoint set: no restart inside a run), D16 (the unreachable stand-in scatter of the output stage), D14 (the
SOS / OSHMEM build branches, after mnrun.sh dropped their launch paths), and the re-scanned AUD2 items 1 (remainder), 4,
5, 6 and 8 (`results/CODE_REDUCTION.md §AUD2`, `results/CODE_REDUCTION.md §AUD3`); then the residue log `ECALC_RES_LOG*` (held for T23, which closed).

**Change / branch.** Branch `shr2`, rebased on main d1d19bf (after P1 merged); eleven commits. No switch added; no default
changed. `rns_dist.c` and `mdb.h` were not touched (on open branches during the work); `newton_db.c` only after P1 merged.

#### What was removed (lines: `git show --numstat` of each commit, measured)

| item | what | + / − lines |
|---|---|---|
| D11 | the leaf-level (level-loop) snapshots and their restart, the tree sets below the top level and their removal protocol, the restart scan and schedule comparison (`mn_ckpt_tree_level`, `bs_ckpt_restart_scan`), the abort test hooks, bs_stats' checkpoint and restart counters. Switches: `BS_CKPT_EVERY`, `BS_CKPT_MIN_LEVEL`, `BS_RESTART`, `BS_CKPT_TREE`, `BS_CKPT_TREE_EVERY`, `BS_CKPT_ABORT`, `BS_CKPT_ABORT_TREE`, `BS_CKPT_ABORT_NODE` (8). The top-level set (`ECALC_CKPT_TOP`, its background writer, budget, header v3; v2 sets still read) stays; `BS_CKPT_DIR` now only names its directory and writes nothing alone. `mnaccept.sh`: the `ckpt` step tests the top set; the `sched` step is gone | +99 / −461 |
| D16 | `out_stage`'s stand-in scatter branch and `mn_out_scatter_standin` (size > 1 always has the sharded X) | +6 / −34 |
| D14 | Makefile `SHMEM_HOME` (SOS) and `oshcc` (OSHMEM) branches; `COMM_SHMEM_SOS` (external heap) and `COMM_SHMEM_OSHMEM` code in `comm_shmem.c`; README rows `COMM_SHMEM_SERIAL`, `COMM_SHMEM_DEVHEAP`, `COMM_SHMEM_ORDER`. The Cray path is unchanged | +34 / −109 |
| AUD2 6 + AUD3 | no caller anywhere: `bi_sub_shifted`, `bi_bits`, `binsplit_dm_hole_bytes`, `comm_shmem_size`, `db_pool_tail_bytes`, `db_pool_tail_stats`, `db_pool_vmm_stats`, `db_pool_largest_free`, `db_pregrow`, `db_share_add_spills`, `ec_add`, `ec_sub`, `ec_shoup`, `ec_shoup_pre`, `k_twiddle`, `rns_ensure_staging`, `rns_pool_n_grow`; `comm_layered_create_minor` with the layered communicator's intra-minor rank order; the unreachable host `rns_mul_pair` branch of the mdev levels (`binsplit.c`) | +14 / −151 |
| AUD2 4 | `dist_pack_bench` and the kernels only it launched (`k_pack`, `k_pack_tiled`, `k_twpack`, `k_unpacktw`, `k_unpack_rows`), `t_dist`'s `DIST_PBENCH` mode (rows `DIST_PBENCH`, `DIST_PSIZE`) | +6 / −136 |
| AUD2 1 | `tests/t_nicbw.c` and rows `NICBW_HHM`, `NICBW_SHHEAP`, `NICBW_SYS` | +2 / −268 |
| AUD2 5 (part) | `t_ntt bench` (K13 / K13b microbenchmarks) with `ntt_pass`, `ntt_pass_at`, `ntt_pass_bounds` | +0 / −231 |
| AUD2 8 (part) | one `digits_format` in `tests/harness.h` (t_out, t_patch) | +13 / −22 |
| RES_LOG | `ECALC_RES_LOG`, `ECALC_RES_LOG_LEVEL`, `ECALC_RES_LOG_CPU` with every check they switched on (`bs_res_check`, the leaf hand-over check, `tree_check`, the output stage's RES lines, `db_mod_qs`'s host cross-check, `mdb_mod_qs`'s RES print) | +6 / −121 |
| (comments) | the history narration SHR1 had to leave (comm_ofi.c, ecalc.c) | +4 / −4 |
| **total** | 39 files (`git diff --shortstat` against main d1d19bf) | **+178 / −1531** |

Settings (registry rows, `tools/switch_registry.py`, measured): **186 before** (185 at the start plus P1's `DM_MN_WRAP`),
**170 after**: 16 removed (8 checkpoint, 3 residue log, 3 `NICBW_*`, `DIST_PBENCH`, `DIST_PSIZE`).

Archived (local archive, MANIFEST line): `removed_2026-10-12/ecalc/tests/t_nicbw.c` (byte-identical) and, under
`removed_2026-10-12/shr2/ecalc/`, the pre-change `ntt.c`, `tests/t_ntt.c`, `ntt_dist.c`, `tests/t_dist.c`.

#### Method

Build on the aac7 login node (worktree from the cluster clone): `make -B SHMEM_CRAY=1 GMP_HOME=~/gmp ecalc` then `make all`
(tests): rc 0, 0 warnings, `-DCOMM_SHMEM` in the log. Gates as one aac7 queue item (`81_shr2_gate.sh`, 2 nodes):

    tools/gate.sh --build --nodes 2 --hold <hold>      # aac7 line; P24 forced at 2 nodes (--p24 auto)
    tools/gate.sh --nodes 1 --hold <hold>
    MNACCEPT_TMP=~/shr2/mnaccept_tmp ecalc/mnaccept.sh <hold> --only unit,e9,ckpt

#### Results

All measured on aac7, queue items 81 (commit e181111, before the residue-log removal), 82 (90ad09c, after it, on main f08a4ea)
and 83 (0eeb624, the final tree on main d1d19bf); every build rc 0 with 0 warnings and `-DCOMM_SHMEM`.

| check | 81 | 82 | 83 (final) |
|---|---|---|---|
| gate 2 nodes, 10⁹, aac7 line, P24 forced | PASS, identical, 20.40 s | PASS, identical, 17.05 s | PASS, identical, 23.91 s |
| gate 1 node, 10⁹ | PASS, identical, 11.72 s | PASS, identical, 11.86 s | PASS, identical, 12.70 s |
| unit `t_ntt 24`, `t_mul 20`, `t_newton 20`, `t_verify`, `t_out`, `t_patch` | PASS | – | – |
| unit `t_dbig 0`, `t_bs` | PASS | PASS | PASS |
| unit `t_mn_grid` at 2 processes | cut by the item's own 1500 s cap while passing its shapes | PASS (180 checks) | PASS (180 checks) |
| `e9` size 1 | not reached | PASS, identical | PASS, identical |
| `ckpt`: top set written (2 headers, nothing else) and read by the recheck | not reached | PASS (3 RECHECK OK) | PASS (3 RECHECK OK) |
| `ckpt`: budgeted drop (`ECALC_CKPT_TOP=2`, 1 MB/s emulated) | not reached | PASS (2 ABANDONED, no header) | PASS |
| `ckpt`: `BS_CKPT_DIR` alone writes nothing | not reached | PASS | PASS |
| `recheck` e9 size 1, e8 size 2 (default `<outfile>.top`; corrupted copy fails) | – | – | PASS, PASS |

The ecalc totals are single runs on a shared machine, not timing claims. The removed code was off at the defaults and on
the launch lines (or, D11, reached only with switches that are now gone), so no timing was measured.

#### Verdict

Merged: every gate and mnaccept step PASS with digits identical; 1531 lines and 16 settings fewer.

#### Open issues (left for a later pass)

- **Host product tier** (`rns_mul`, `rns_mul_low`, `rns_mul_mdev`, `rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`,
  `mul_rec`, `mdev_gpu_crt`; ≈ 270 lines of `rns_mul.c`): no production caller any more (its last one, the unreachable mdev
  branch, is gone), but `t_mul`, `t_dbig`, `t_mn_grid`, `t_newton`, `t_primes` use `rns_mul` as their reference product.
  Removing it needs a test-side reference that is fast above 2^20 limbs (the GMP string bridge of `harness.h` is not).
- Skipped because `rns_dist.c` was on an open branch (p1, now simp): `gen_pack_bench` and its kernels (`DIST_GBENCH`),
  `rns_mul_dist_hd`, `acc_flat`, `part_owner`, `rns_mul_dist` (test-only), the duplicate `pow_table` / `dev_pow_table`.
  Not done for time: the duplicate `mn_powmod` (newton_db.c) / `powmod_h` (dbig.c); `newton_dkm_set` stays (the tests'
  override of the DKM choice).
- The open branches simp, n3 and k4b (opened during this work) edit `binsplit.c`, `ecalc.c`, `rns_mul.c`, `rns_mul.h`;
  they need a rebase over this merge.
- Kept on purpose (test oracles or checks of production code): `ntt_host_fwd/inv`, `crt_garner3/4`, `db_pool_extents`,
  `db_mod_q`, `db_set_base_pow`, `vf_digits_mod`, `tier1/tier2`, `dist_inv_t`, `ec_shoup_lazy`.
- Duplicates not merged: the device Garner of `rns_mul.c` / `p24_crt.c` (`dmod128`, `dmod192`) and `k_store` /
  `k_copy_h2d` (kernels in two translation units: a shared header would change code generation, so it needs its own check);
  `make_mdb` of `t_mn_grid.c` / `cx_grid.c` (cx_grid has no Makefile rule).
- `t_ntt bench` is cited by `results/KERNELS.md §K13`, `K13b.md`, `NTT3P.md` (history; the code is in the archive).

#### RESUME

Done: merged to main and pushed; worktrees removed (local and aac7); no job of ours left (queue items 81-83 finished, their
holds cancelled by the runner). Gate logs on aac7 under `~/shr2/Q*/` (raw, not in git). Next (a later pass): the host
product tier with a test-side reference product; the rns_dist.c items above once simp has merged.

## W2A: wave 2, lane A of the user-approved settings removal (2026-10-12)

(former file: W2A.md)

Agent LA. The lane A batches of the switch audit of 2026-10-12: A1, A3, A4, A2, D5 and A5. All six are on main. TASKS 23 was
taken over by a separate agent (T23) and is not part of this report. Every removed path was off at the defaults and on the
launch lines, or its default is now built in, so the digits are identical by construction. Each batch still ran the gates.
Times are Eastern. Line counts are `git show --stat` of the batch's commits on main (measured).

Gates for each batch ran as an aac7 queue item on 2 nodes with a SHMEM build (`-DCOMM_SHMEM` checked in the build log):
- `tools/gate.sh --build --nodes 1` and `--nodes 2`: 10^9 digits on the aac7 launch line, VERIFY OK, digits identical to
  the reference, no segfault;
- then the listed `ecalc/mnaccept.sh` steps, on 1 node.

No timing runs were made. No batch changes a kernel or a schedule at the defaults: each one only builds in today's values.

| batch | switches removed | lines (+/-, m) | gates (m) | on main |
|---|---|---|---|---|
| A1 | `ECALC_VMM_BG=2` (the value only, with `hipmm.c` and the link wraps); `ECALC_VMM_SAFE`, `ECALC_VMM_REPRO`, `ECALC_VMM_REPRO_SPIN`, `DBIG_ADDSUB2`, `DBIG_ADDSUB3`, `DBIG_QSEL`, `DBIG_MAXIDX_TOP`, `DBIG_MODQ_MONT`, `DBIG_S51_STATS`, `DBIG_BOUNCE_TS`, `DB_POOL_TRACE`, `MEM_COPY_WAIT_POLL`, `MEM_COPY_WAIT_REPORT_S` (13 switches + 1 value); also `tests/t_s51_probe.c` and the S35 drivers | +66 / -546 | gate 1 node PASS (ecalc 13.07 s), 2 nodes PASS (20.28 s); `unit` PASS (incl. t_dbig) | a77a2f1 |
| A3 | `NEWTON_ANCHOR`, `NEWTON_LOWPROD`, `NEWTON_HIGHPROD`; `NEWTON_DKM` (built in on; `newton_dkm_set` stays as the tests' override; the single-quotient division stays as the fallback for small k); `DM_MN_LEAN` (built in on; the key is gone from the launch lines and profiles); `MN_MODEL_TCP`; `COMM_HS_TS` (7); also `tools/g1_hsts.py` | +49 / -162 | gates 1 and 2 nodes PASS (18.56 s); `unit`, `corr` PASS (sizes 1, 2, 4) | 53bdfc1, 23224a9 |
| A4 | `ECALC_SPILL_DIR`, `SPILL_CHUNK_MB`, `SPILL_BUFFERED` (3): the spill primitive, which had no production caller, and `tests/t_spill.c`. The O_DIRECT file layer (`sp_file`) stays | +6 / -385 | gates 1 and 2 nodes PASS (21.49 s); `unit` PASS (incl. t_out, t_patch) | ed25de2 |
| A2 | `MN_COMBINE`, `MN_DM`, `MN_LAYERED_RAW`, `MN_LAYERED_LOCAL`, `MN_SELFTEST_GROW`, `MN_TOPO_GROUP`, `BS_SCHOOL_NL`, `LIMB_BASE`, `MN_PLAN_DEXTRA`, `ECALC_SEED_ORDER`, `BS_POOL_RULE`, `BS_CKPT_TREE_BG`, `MN_TREE_EARLY_FREE`, `ECALC_VSLOT_BUDGET`, `COMM_LAYER_VSLOT_PREALLOC`, `COMM_SHMEM_POOL_AUTO`, `COMM_INIT_EARLY` (17). `ECALC_RES_LOG*` are kept for TASKS 23 | +105 / -283 | gates 1 and 2 nodes PASS (20.62 s); `unit` PASS; `e9`, `grid`, `ckpt`, `sched` PASS (6/6, re-run in item 50, see notes). `BS_LAYOUT_ONLY` output byte-identical to the build before A2 and to main before the merge, at 6.441e10:576, 6.632e10:576, 6.441e10:16, 6.441e10:1, 4e10:4 and 1e11:4, plain and with the aac7 line; `mem_model.py --check-c` exact (0 terms off) | 4e67fde |
| D5 | default `ECALC_OUT_PACKED` 1 -> 2, the 60-bit packed form (user decision, TASKS 40); `=0` (ASCII) and `=1` stay. `t_patch` runs all three forms; its part check now reads the 60-bit form | +24 / -21 | gates 1 and 2 nodes PASS (16.05 s, 26.48 s); `unit`, `e9`, `mn`, `recheck`, `corr` PASS (20/20, item 52) | fe9f21d, d12ac2c |
| A5 | `ECALC_OVERLAP`, `NEWTON_DEVICE` (2, optional, user-approved): the sequential flow, the host reciprocal and division (`newton_recip`, `newton_divmod`, `newton_recip_seeded`) and the `par == 0` init paths. The size-1 host-P/Q path for small runs stays: it is reached at the defaults | +58 / -210 | gates 1 and 2 nodes PASS (12.04 s, 18.91 s); `unit`, `e9`, `mn` PASS 14/15. The one failure, t_patch, was the test artefact fixed in D5 (see notes) | c7942bc |

Totals (measured):
- 42 switches and 1 switch value removed, and 1 default changed.
- About 1,800 lines removed, 300 added.
- Registry rows: 257 at the start of lane A (main after B1). 185 now, with lane B's and the D1e removals in between.
- After every merge: `tools/switch_registry.py --check` 0 problems, `tools/profile_load.py --validate-all` 0 problems,
  `tools/doccheck.py` 0 problems.

#### Notes

- **Push leak guard false positive (A2).** The push hook rejects any diff whose changed or context lines contain the C text
  `src` + `.dev` (it reads it as a domain name). The commit hook checks only added lines. `ecalc.c`'s `out_stage` has such
  lines. So its stand-in scatter branch (`mn_out_scatter_standin`, the size > 1 host X) stays as unreachable code: about 25
  lines in `ecalc.c` and `mn_out.c/h`. Deleting it needs the guard to allow that token.
- **Shared mnaccept directory (aac7).** `aac7env.sh` defaults `MNACCEPT_TMP` to one shared directory. Concurrent queue
  items deleted each other's digit files, which caused "Stale file handle" and "cannot open part" failures in items 46
  and 47. Those steps passed when re-run with a private directory (`MNACCEPT_TMP=~/<item>/mnaccept_tmp`, items 50 and 52).
  Every queue item that runs mnaccept should set its own directory.
- **t_patch under the 60-bit default (D5).** `packed_part_check` read 8 bytes per limb, which gave 2399 false failures
  ("the part does not hold N limbs") under `=2`. Fixed in d12ac2c. The writer and the patch were correct: `corr` and
  `recheck` passed on the 60-bit files.
- **Not changed.** `mn_model.py PACKED_BPD` still prices the 64-bit form (8 B per 18 digits). The 60-bit form is 6.25 %
  fewer bytes. `comm_layered_create_minor` (lane B's file) has no caller since MN_TOPO_GROUP was removed.
- **A5 side effects.** The plain `t_newton` now runs the device sections (1098 checks instead of 620). The print-only
  `config:` line no longer has `overlap:1`.

#### RESUME

Lane A is complete; nothing of it is pending on aac7 (no holds, no worktrees left). Open, owned elsewhere: TASKS 23
(agent T23); removing `ECALC_RES_LOG*` after T23.

## W2B — wave 2, lane B of the settings removal (2026-10-12)

(former file: W2B.md)

User-approved removal of experiment / adopted-default switches, lane B (transport, NTT kernels, distributed
products), batches B1, B3, B2, the optional B4 (TCP transport) and D1e (TASKS 27: six launch-line settings as
built-in defaults). Every removal keeps today's value, so digits are identical by construction; each batch was
gated on aac7 through the queue (SHMEM build, `-DCOMM_SHMEM` checked). Labels: m measured, mod modelled, a assumed.
Times Eastern.

#### Batches

| batch | commit on main | switches | lines (git diff --stat, m) | gates (aac7, m) |
|---|---|---|---|---|
| B1 transport | 2646a63 | 12 + the comm_shmem.c `COMM_HS_TS` stamps | 11 files, +45 −322 | gate 2 n + 1 n PASS (1e9 identical, VERIFY OK); 2 n × 6.441e10/node VERIFY OK, 170.1 s |
| B3 NTT kernels | 3cfab5d | 12 + `ntt_b1_shoup` | 12 files, +156 −1039 | gate 2 n + 1 n PASS; `t_ntt hash` main vs B3: 2560 cases, 0 differing; `t_ntt 24`, `t_mul 20` VERIFY OK; ABBA 1 n 6.441e10, 4 pairs: B3 − main −0.40 s, 95 % CI [−3.84, +3.04] (null) |
| B2 distributed products | (this merge) | 16 | 17 files, +115 −473 | gate 2 n + 1 n PASS (twice: first base and after the A1/A3 rebase); 2 n × 6.441e10/node VERIFY OK, 167.3 s; `t_strategy 24 2 V` VERIFY OK; `mnaccept --only grid,mn` PASS; `MN_PLAN_ONLY=3.71e13:576` identical but for the P24 print |
| B4 TCP transport | (this merge) | 2 + value `tcp` | 23 files, +57 −776 | gate 2 n + 1 n PASS; 2 n × 6.441e10/node VERIFY OK, 158.0 s; `t_comm` 2 PEs (mnrun default shmem) VERIFY OK; `mnaccept --only grid` PASS |
| D1e TASKS 27 | (this merge) | 0 removed, 6 now default 1 | 16 files, +60 −53 | full stack: gate 2 n + 1 n PASS; gate 2 n with `COMM_LAYER_VSLOT_POOL=0 RNS_DIST_CACHE_PARTIAL=0 MN_OUT_DKM_HI=0` PASS; 2 n × 6.441e10/node VERIFY OK, 134.0 s; plans and BS_LAYOUT_ONLY identical, `mem_model --check-c` as on main |

Registry (`ecalc/switches.tsv`): 269 rows before wave 2 lane B → 204 on main with lane A's A1, A3, A4 (lane B removed 42 rows: B1 12, B3 12, B2 16, B4 2) after (m). README rows deleted row by row;
`tools/switch_registry.py --check` = 0 problems and `tools/profile_load.py --validate-all` = 0 problems on every branch.

##### B1 — transport experiments (12 switches)
Removed: `COMM_SHMEM_RPIPE` (two-deep rounds, W_ROFF1/W_SIG1 words), `COMM_SHMEM_PEER_ORDER` (rotated ready-first
service), `COMM_SHMEM_KEEP_STAGING`, `COMM_SHMEM_THREAD`, `COMM_SHMEM_NOSYM`, `COMM_SHMEM_PUT_MB`,
`COMM_SHMEM_STAGE_SLOTS` (regions always on when sized), `COMM_LAYER_TEST_SKEW_MB`, `COMM_PUSH64` / `COMM_PUSH_BLOCKS`
(64-bit stores, 76 blocks hard-wired), `COMM_OFI_DUMP` (dump always), `COMM_OFI_SYM_TOP` (follows
`COMM_LAYER_VSLOT_POOL`; `tests/t_ofi_alloc` sets the mode directly), and the `COMM_HS_TS` stamps in `comm_shmem.c`
(the switch itself is lane A's A3). `t_ofi_alloc 1` fails exactly as on main (its pre-fix scenario; m).

##### B3 — old NTT kernels (12 switches + the dead `ntt_b1_shoup` flag)
Removed at their defaults: `NTT_B16_BODY` (tile / radix-4 bodies of 7-stage passes), `NTT_MODMUL` (FP64 Barrett and
Shoup forms: `k_b1s`, the Shoup tables, `shoup_q`), `NTT_B1R` (the paper b1 pass `k_b1`, the 16-point form),
`NTT_PLAN` (plan_auto always), `NTT_R3_FUSE` (the fused radix-3 forms, `ntt_fwd_r3` / `ntt_inv_r3`), `NTT_B16_STG`
and `PW_FUSE` (env reads; `ntt_stg` / `ntt_pw_fuse` stay internal for the tests), `RNS_R3_MINK`, `RNS_BATCH_PAIR`,
`RNS_STRIPED_PAIR`, `RNS_PLANES_FIRST`, `RNS_POOL1_4Q`. `t_ntt`: the variant identity sections and variant benches
removed, `hash` kept for old/new diffs; `t_mul` 4b runs the paired form; `tests/n3x_node.sh` removed.

##### B2 — distributed products (16 switches)
Deleted with their paths: `MN_REDIST_FUSE`, `RNS_CRT_SUB` (`k_crt_batch_m`, `k_crt_sub_merge`), `DIST_MN_SYM_SLABS`,
`RNS_STRATEGY_FORM` and the B4 form (`k_bsplit`, `k_b4_pw`, `b_tables`; `RNS_STRATEGY=B4` refused),
`DIST_TEST_LAG_MS`. Hard-wired: `RNS_DIST_CACHE` 0, `RNS_DIST_CACHE_FIT` 1 (the sized, budget-bounded slots are
unconditional; the key left the aac7/target lines, profiles, `target_kit.sh` — its `cache_guard` is no longer
needed — and `nodescreen.sh`), `DIST_PW_FUSE` 1, `DIST_TWREC_G` 1 (the plain packs stay as `gen_pack_bench`'s
reference), `DIST_LOGR_DELTA` 0, `DIST_SYM_SLABS` 1, `RNS_AUTO_PIECE_COST` 1, `RNS_STRATEGY_GRID` 1, `DIST_RB_WAIT` 1,
`COMM_ALLTOALLV_DEPTH` 2 (the one-deep v-exchange removed), `MN_P24` 2 (the 18-digit mn path stays as the fallback
when pool 0 has fewer than four planes). Fixed: the `plan cache` line printed `MN_P24=0` when unset; it prints 2.
`MN_PLAN_ONLY=3.71e13:576` on the aac7 line: identical to main except the config hash and that print (m).

##### B4 — TCP transport (optional, user-approved; 2 switches + the value `tcp`)
`comm_tcp.c`, `COMM_HOSTS`, `COMM_PORT` and `COMM_TRANSPORT=tcp` removed. `mn.c` refuses `COMM_SIZE > 1` without
`COMM_TRANSPORT=shmem` (its TCP mesh paths removed); `mnrun.sh` sets `COMM_TRANSPORT=shmem` for more than one process
and no longer exports COMM_HOSTS/COMM_PORT; `tools/gate.sh` drops `COMM_PORT`. A build without SHMEM runs one
process only (aac6 multi-process runs need OSHMEM). `t_comm` and `t_dist`'s per-rank mode are SHMEM-only;
`wp6run.sh` and `tests/lay_host` (the host-only TCP harness of S45) removed.

##### D1e — TASKS 27: six launch-line settings as built-in defaults
`COMM_LAYER_INTER2`, `COMM_LAYER_VSLOT_POOL`, `COMM_LAYER_VSLOT_SHARE`, `COMM_LAYER_VSLOT_RELEASE`,
`RNS_DIST_CACHE_PARTIAL`, `MN_OUT_DKM_HI` default to 1, each with its `=0` fallback; INTER2 and RELEASE follow
`COMM_LAYER_VSLOT_POOL` when unset (so `COMM_LAYER_VSLOT_POOL=0` alone restores the old form). The keys left the
aac7 and target lines and both profiles. `MN_PLAN_ONLY` with the new line (no keys) against main with the old line
at 3.71e13:576, 6.441e10:2 and 1.2882e11:2: identical except the B2 print fix (m).

#### Cross-lane hunks (named for lane A)
- B2: `binsplit.c` (the `DIST_MN_SYM_SLABS` pool term, the v-slot depth), `mn_plan.c` (the P24 print).
- B4: `mn.c` (the TCP mesh paths in `mn_init`, `sub_mesh`, `sub_mesh_strided`, `mn_group_span`; `hosts_of`).
- D1e: `binsplit.c` (four one-line readers), `mn.c` and `ecalc.c` (one line each, plus `dkm_hi_explicit`).
- `rns_mul.c` hunks of B2 (CRT sub-stripes) and B3 (pairing / pool switches).
- `mem_model.py` (D1e): the X2 / S36 / M3 readers default to 1 as in the code, so `--check-c` stays as on main.
- Central docs: ALGORITHM.md (P24, B4, cache FIT, comm_tcp rows), ecalc/README.md; `tools/doccheck.py` 0 problems.

#### Open items
- B2–D1e were gated on their own stack; after the final rebase onto main (lane A's A4, the doc rewrites R1–R4) they were
  build-checked on aac7 (SHMEM build) and doccheck / registry / profile checks rerun before the push (no new gate).
- The single 2-node 6.441e10 totals above are single runs (m), not timing claims; only B3 had a paired timing (null).
- aac6 multi-process runs now need OSHMEM (`COMM_TRANSPORT=shmem`); mnaccept's non-Cray path still omits it (mnrun now sets it).
- Old experiment drivers under ecalc/tests (pc_j*, ofi17*, std17*) still set removed keys; harmless (ignored).

#### RESUME
Done: B1, B3, B2, B4, D1e merged to main. Worktrees removed (local and aac7); gate outputs on aac7 in ~/lbb1, ~/lbb3,
~/lbb2, ~/lbb2r, ~/lbb4, ~/lbd1e. Nothing running. Next lane-B work: none in this wave.

## L2 - validation of the code reduction (B02) at full size and for speed, aac6 (2026-10-11)

(former file: L2.md)

Scope: new = main a1569d5 (after B02: -36 switches, -1,354 core lines); old = tag archive/pre-reduction-2026-10-11 (bc80001).
K2+K3 (DBIG_ADDSUB3, DBIG_MODQ_MONT) became default in a5bbd08, which is an ancestor of bc80001: both builds have them, so the
comparison isolates the reduction. Node: aac6-spx-b (one node, 4 APUs), plain launch line (no env), builds from clones
`~/ntt-l2new`, `~/ntt-l2old` with `make -s -j16` (removed afterwards). aac6-spx-a was held by another user's interactive job
during the whole slot, so no second node was used (nothing of theirs was touched).

#### 1. Correctness at 1e11 digits (measured)
- `ECALC_VERBOSE=2 ./ecalc 100000000000 /tmp/l2_1e11.out` (new main, 1 node, digits written to local /tmp, packed ECPACK18 parts):
  rc 0, total 129.3 s, VmHWM 27.5 GB, **VERIFY OK**.
- `ecalc/digcmp.sh /tmp/l2_1e11.out ~/ntt/ecalc/results/e_1e11.out` (uses tools/unpack_digits --cmp): **identical**.
- 4e10: no separate 4e10 reference file exists on aac6 (`e_4e10.out` absent); the 1e11 reference was verified earlier to have the
  4e10 reference as its prefix (ref1e11 log, 2026-09-26), so the identical 1e11 digits cover the first 4e10 digits. No separate 4e10 run was made.

#### 2. Speed, paired ABBA new vs old at 64,410,000,000 digits (measured)
Command per run: `./ecalc 64410000000` (no digit write, no env), order per round new, old, old, new; reference file evicted from the
page cache (posix_fadvise DONTNEED) before each run (nothing is read in a no-write run). All 32 runs rc 0, VERIFY OK.
Per-round means of total (s), new / old: R1 73.16/72.82, R2 73.66/74.89, R3 72.96/73.25, R4 72.45/72.85, R5 72.14/73.57,
R6 73.61/73.07, R7 73.49/72.55, R8 73.39/73.32.
- Overall mean total: new 73.11 s, old 73.29 s (n = 16 each, sd 0.78 s and 1.24 s).
- Mean difference new - old over 8 rounds: **-0.18 s, 95% CI half-width 0.70 s (CI -0.88 to +0.52), t = -0.62**; stop rule: round cap 8.
- Verdict: no measurable speed difference (the CI covers 0 and excludes anything beyond about 0.9 s, 1.2% of the run).

#### Verdict
The reduction is digit-safe at 1e11 digits (VERIFY OK, identical to the reference) and speed-neutral at 6.441e10 on one aac6 node.
Not covered: multi-node (aac6 is single node), 4e10 as a separate run.

#### Commands
`tools` not changed. Driver (private, run on aac6): chained <=45-min sbatch holds on s24-26, `srun --jobid=$J`, phases: 1e11 run +
digcmp, then ABBA rounds with t-based stop rule (p<0.05 or half-width <= 0.5 s or 8 rounds, minimum 4). Jobs cancelled, clones removed.

#### RESUME
Done; nothing running on aac6.

## O8 - TASKS backlog clean-up (local only, no cluster use; 2026-10-11, branch o8 off main 0298fe2)

(former file: O8.md)

Labels: measured / modelled / assumed. Nothing was run on a cluster. No branch was deleted.

#### 1. TASKS #4 - X2 cut-group pool-offset asymmetry (S45)
Source: results/GRID_GROUPS.md §S45 (host emulation `t_lay`, up to 600 ranks over TCP; the cxi transport itself was not run).
- **What X2 needs.** `COMM_LAYER_INTER2=1` (second inter communicator, `comm_layered.c` p->i2) requires `COMM_LAYER_VSLOT_POOL=1`: the v-slots are carved from the
  symmetric comm pool, not hipMalloc'd. A symmetric block (same offset on every PE of the top group) is only valid if every member made the same allocation sequence.
- **Where the sequence differs.** In a *cut* last group (g not divisible by the group size, e.g. groups 6,6,3) the cut-group nodes run a smaller lower level.
  (a) OFI pool (`comm_ofi.c`, `COMM_OFI_SYM_TOP` top-down last fit, on with VSLOT_POOL): with `SHARE=1` the pool rule gives every group size the same slot size
  (MB_192 = MB_576), so nothing regrows differently: symmetric in every run. With `SHARE=0` the cut-group nodes hold a smaller lower slot, the top slot lands at another offset:
  asymmetric (15 nodes, groups 6,6,3: 288 mismatches). (b) SHMEM single pool (`comm_shmem.c`, first fit, control blocks in the same pool): the control blocks differ by group size,
  so the offsets differ for SHARE 0 and 1 alike (39, 150, 15 nodes; control size emulated, assumed).
  Without X2 nothing is affected (v-slots are hipMalloc'd; symmetric-offset equality is not required).
- **Consequence.** A latent hazard of the X2 path for non-dividing group schedules, not a data-corruption path on the target's 576-node schedule (levels 2..64 powers of two, 192 x 3, 576:
  no cut group; S45 modelled). Caveat added 2026-10-11 (b01, commit 2f2675c): **X2 is now on the target launch line**, so the "X2 is not on the launch line" sentence of S45 is out of date;
  the target schedule is still cut-free. The hazard becomes live if the group schedule changes (TASKS 31: node counts 572-576 with an uneven last group, or a schedule from TASKS 34 whose
  last lower group is cut) while X2 is on. A guard would be cheap (compare the top-group slot offset with an allreduce-min/max at slot creation and fail loudly); not built here (code change, needs a gate).
- **Proposed status:** "understood and documented (O8): X2 + cut last group can give unequal symmetric offsets (OFI SHARE=0; SHMEM always); target schedule has no cut group;
  re-check before any uneven-group schedule (item 31) is run with X2; optional guard = offset-equality check at slot creation".

#### 2. TASKS #5 second half - K2/K3 in the time model
Found on the way (fixed): `ecalc/estimate.py --target` crashed on main (`NameError: prof`, the `k_other=prof.k_other` of commit 65a5758 sits in `target()` and a helper, where `prof` is local to `main()`). Fix: `a.k_other = prof.k_other` in `main()`, `a.k_other` in the two helpers. Output with `--k23-s 0` is byte-identical to the fixed pre-change output.
- Model change: `mn_model.K23_S = 2.03` s per node (env `MN_MODEL_K23_S`, flag `--k23-s`; 0 = previous estimate), subtracted in `estimate.estimate()` from the wall with and without the part file.
  Basis: S51 ABBA at 1 node, 6.441e10 digits = the per-node size at 3.71e13 on 576 nodes: -2.03 +- 1.38 s (aac7), -1.22 +- 0.84 s (aac6 PPAC), measured; K3 `db_mod_qs` is -1.66 s of it. Carrying the 1-node figure to 576 is **assumed**.
  At 4 nodes the kernel metric alone is -8.4 s (K2 -2.0, K3 -6.4; mechanism not separated) but the run totals are not significant (-6.0 +- 21.9 s): not used. If it carried, the saving at 576 would be larger (upper bound only).
  It is applied after `mn_model.run()`, so `--calib*` (fitted on pre-K2/K3 runs) is unchanged. `mn_model.py --calib15c` runs and is unchanged (worst wall error 4.9 %, as before).
- Before/after, `./estimate.py --target`, profile target_l4 (default), 576 nodes, MN_GROUPS 2,4,8,16,32,64,192,576, no T2048/RELEASE (modelled, fabric assumed):

| digits | no write before | no write after | write @0.6 GB/s before | after |
|---|---|---|---|---|
| 3.71e13 | 181.7 s | 179.7 s | 216.5 s | 214.4 s |
| 3.60e13 | 179.1 s | 177.0 s | 212.5 s | 210.5 s |

  (L4's 176.3 / 181.7 s figures include T2048 + RELEASE, which `estimate.py` does not model; compare like with like: the saving is -2.0 s (about -1.1 %), modelled from a measured 1-node figure.)
- Other findings of 2026-10-11 already in the model: layv factor (target_l4 0.919, L4), level-cost fix (TREE_CRIT), k_other. K4, K7, K6 have no effect (no gain / not built). Not modelled, listed for the user: T2048, RELEASE, single-level `[6]` schedule (item 34), DIST_COALESCE (item 15).
- Remaining part of #5 (the 2/4/6-node scaling study) stays "later".

#### 3. TASKS #8 - branches (proposal only, nothing deleted)
State seen: local+remote branch list, tags `archive/*` (git), `archive:MANIFEST.md`.
| branch | state | content | proposal |
|---|---|---|---|
| s24 | no branch left; tag `archive/s24` (5 commits ahead of main, 906 lines: NTT3P E0 tile probe, 2^13 b1 pass, 13/9/9 prototype) | shelved bench, never merged; results not on main | keep the tag; nothing more |
| s25 | no branch left; tag `archive/s25` (3 commits, 335 lines: 10-node drivers) | superseded: 10-node runs impossible under QOS 6 | keep the tag |
| s37 | no branch left; tag `archive/s37` (1 commit, archive:results/S37.md only; S-3/S-6 dropped) | archive:results/S37.md is on main | tag can stay or go; content is on main |
| s35 | local + origin, worktree `ntt-wt-s35`; `git merge-base --is-ancestor s35 main` = yes | fully on main (`ECALC_VMM_BG=2` is in `dbig.c` on main, results/CRASHES_HANGS.md §S34) | on main: remove worktree + branch, no tag. TASKS row 1 wrongly says "branch s35" |
| s50 | local + origin, 10 commits ahead of main | DIST_GATHER_TILE code (`rns_dist.c`, `t_gather_probe.c`, drivers); result null at all scales, "do not adopt"; code not on main (0 hits); tag `archive/s50-gather-tile` already exists | worth keeping only as the existing tag: delete branch + worktree after the user agrees. results/EXCHANGE.md §S50 is on main |
| origin/claude/confident-mayer-yx23pt | 2026-09-24, ancestor of main | on main | delete remote branch (no tag) |
| c07, t12 | 2026-10-11 (not stale); c07 4 commits (instrumentation + archive:results/C07.md), t12 2 commits (uffd soak driver, paused) | unmerged | leave; decide at merge |
| h1 | at main | nothing | remove with its worktree when its task ends |
Tonight's h3/h4/o1-o5 not assessed. Suggested archive tags: none new (s24, s25, s37, s50 already tagged). TASKS row 8 text updated accordingly.

#### 4. Stale / superseded rows (proposed text is applied in TASKS.md on this branch)
- **#3** (S39 candidates): all built and measured: k_addsub2 -> S51 K2 `DBIG_ADDSUB3` (default), k_modq -> S51 K3 `DBIG_MODQ_MONT` (default), k_crt_batch -> S51 K4 `RNS_CRT_SUB` (no gain, not adopted); also k_maxidx S40/S41, QSEL S41, DIST_GATHER_TILE S50 (null). Closed. The stale line in the 2026-10-10 evening status block is updated too. The SH5_MI300A_SPX remark (one device, ecalc cannot run, S44) is already in the "Environment facts" paragraph (line after the table); no change needed.
- **#11 / #19 / #30 / #32 (4-node bimodality, slow mode):** A13 (4 nodes, aac7): runs with NIC MCU counter group A (oxe_mcu_meas 18-25) 10.2 vs group B (28-33) 12.1 GB/s; the group is drawn per launch. C04 reads the group per node. A14 (2 nodes, 86 launches): p(A) ~ 0.5 per launch but the 2-node rate is the same in both groups (6.59 vs 6.59 GB/s) and no env setting pins the group; 4-node group -> rate not retested (cancelled, aac7 scarce). So: the cause is a per-launch NIC group draw (A13, 4 nodes, one run each at 6 runs: weak statistics; cause of the draw unknown), not arm/node/rank/order (T05/T09), not fabric state. Open remainder = only "what picks the group" (#32) and the retry-at-init idea; #19 (concurrent traffic / memory fill) is superseded by that finding; #11's `COMM_XSTATS` investigation likewise; #30's probe is C04's `tools/envprobe.sh`, whose covariate was not adopted (corr -0.06 / -0.20) so a pre-run probe has no predictive value beyond reading the group itself.
- **#20:** superseded by `tools/initchk.py` (A03/A06); T10: no positive control. Cold probe stays off. Already marked superseded; wording tightened to "closed".
- **#38:** closed already; marked closed (C04 corr -0.06 / -0.20; C02 no build).
- **#14:** S49 cold-path probe: same fate as #20 (the S49/T10 cold probe could not be shown to catch the slow host; `initchk.py` covers it from the run log). Proposed: superseded by #20/A03/A06; reopen only if a slow host escapes `initchk`.

#### Files
- results/CODE_REDUCTION.md §O8 (this file); ecalc/mn_model.py (K23_S); ecalc/estimate.py (--k23-s, `k_other` NameError fix); TASKS.md.

#### RESUME
Branch o8 holds everything; to adopt: merge o8 after the coordinator's look; branch deletions (s35, s50, remote claude/confident-mayer-yx23pt) are the user's decision.

## H1 -- PLAN.md 28.4 measurement, single-node part (decimal vs binary limbs, no code changed)

(former file: H1.md)

Date 2026-10-11, 23:00-23:50 Eastern (aac6 logs Central). Commit under test: 0298fe2 (main). Node aac6-spx-a (PPAC_MI300A_SPX,
4 APUs, one hold, 45 min); the 2-process grid on the same node. All numbers **measured**, single run each (pass/fail and wall time
only; these are not paired timing claims).

#### Method
The library picks its base in `bi_env_base()` (bigint.c), which reads `LIMB_BASE`. Each test was run with `LIMB_BASE=2` and
`LIMB_BASE=10` in the environment; no tracked code was changed. Tests that call `bi_set_decimal(1)` themselves (t_out, t_patch,
t_seed, t_primes, b_seed64) are base-independent by construction and are listed for completeness. Command lines: those of
`ecalc/mnaccept.sh` step `unit` (`t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`, `t_newton 20`, `t_verify`, `t_out`, `t_patch`), plus
`t_crt`, `t_dec`, and `t_mn_grid 1 28` through `mnrun.sh 2` (two node-processes on the one node, `LIMB_BASE` exported). Standing
regression: `mnaccept.sh <job> --only e9` (10^9 digits, size 1, both bases).

#### Results
| test | base 2 | base 10 (decimal) | oracle | classification |
|---|---|---|---|---|
| t_ntt 24 | OK (2360 checks) 8.6 s | OK (2360 checks) 10.5 s | CPU reference NTT | pass both (base-independent) |
| t_mul 20 | OK (185 checks) 254 s | **no result in 900 s (timeout)**; with a throwaway local patch skipping the 6 largest batch cases: OK (181 checks) 783 s | GMP `mpz_mul` | (c) oracle cost, see below; not a disagreement |
| t_bs | OK (10 checks) 23.8 s | OK (10 checks) 25.7 s (first run: VERIFY FAILED 2 of 10, the SHA-256 reference files were absent in the fresh clone, `vs ref` empty; rerun with them linked) | SHA-256 of e to 10^6 / 10^7 digits | pass both |
| t_dbig 0 | OK (555 checks) 17.4 s | OK (555 checks) 19.0 s | host bigint / GMP | pass both |
| t_newton 20 | OK (620 checks) 26.6 s | OK (620 checks) 83.4 s | `mpz_tdiv_qr` (decimal exact only to 2^22) | pass both |
| t_crt | OK (12 checks) 30.1 s | OK (12 checks) 12.2 s | GMP | pass both |
| t_verify | OK (334 checks) 2.4 s | OK (334 checks) 3.0 s | GMP | pass both |
| t_out | OK (1 check) 11.3 s | OK (1 check) 11.5 s | sets decimal itself | base-independent |
| t_patch | OK (both forms) 175.5 s | OK (both forms) 176.9 s | sets decimal itself | base-independent |
| t_mn_grid (2 procs, 1 node) | OK (180 checks) 158.0 s | OK (180 checks) 143.9 s | GMP | pass both |
| t_dec | OK (26 checks) 27.9 s | FATAL rc 3 at "bigint: bi_shl by bits is a bit operation; not valid in the decimal base" (14.7 s) | GMP (radix converter) | (a) base-specific by construction: tests the binary-to-decimal converter, which exists only for binary limbs; decimal refuses bit operations on purpose |
| ecalc 10^9, size 1 (mnaccept e9) | digits identical, total 20.83 s | digits identical, total 11.40 s | reference digits e_1000000000 | pass both (decimal is the default) |

Which tests hard-code or default to binary: `bi_env_base()` is called by `rns_init` (rns_mul.c), t_dbig, t_mn_grid, t_strategy,
t_spill, cx_grid, so they follow `LIMB_BASE`; t_mul, t_newton, t_crt, t_bs, t_verify, t_ntt reach it through `rns_init`/the
library. t_crt additionally switches itself to decimal for `ECALC_NP=3`. No test needed a code edit to run in decimal.

#### The t_mul decimal case (c)
In decimal, section 4 "grpB: 3000 x 2^17, tiles" printed `running ...` and then made no progress for the whole 900 s timeout
(binary: the product takes 0.054 s and the whole `t_mul 20` run ends in 254 s). The product call (`rns_mul_batch`) is timed
before the check; the stall is the verification loop, which converts every operand and product to GMP through `mpz_from_limbs`
(a decimal-string bridge) for 3000 products of about 2.4 million digits each; `t_mul.c` itself already caps the decimal check
at 2^20 limbs in section 1-3 for the same reason ("the GMP string bridge is slow above 2^20"). It is a slow oracle in the
test, not a wrong library result, but this is inferred from the code and the progress pattern, **not** proven by a profile.
To get a decimal pass on the rest, a throwaway patch (scratch clone on aac6, **not committed**, removed) added one line
at the top of the batch loop: `if (bi_decimal && N * L > ((size_t)1 << 22)) { printf("SKIPPED ..."); continue; }`. It skipped
six cases (3000 x 2^17, 2^22 max, 2^20 CPU CRT, 2^17, 2^18, 2^14 x 40000) and every other check passed (181 of 185 counted checks;
the 4 missing are the skipped cases' VERIFY lines plus tiles). So decimal batch products at those six sizes are **not**
independently verified by this run; the e9 run (10^9 digits, decimal, identical digits) exercises large decimal products indirectly.

#### Verdict
No real disagreement (class b) in any test. One class (a) item (t_dec). One class (c) item (t_mul batch checks too slow to
run in decimal as written). Decimal runs were as fast as or faster than binary for ecalc itself (11.4 s vs 20.8 s at 10^9).
Decimal t_newton is 3.1 times slower than binary in the test (83 s vs 27 s, GMP-bridge cost in the test, not the library).

#### Open issues / for 28.5
- t_mul needs a faster decimal oracle (e.g. verify a random sample of products, or compare mod a prime) before it can be the
  rewritten decimal test; the six skipped cases should be re-verified then.
- The fresh-clone failure of t_bs without `ecalc/ref/*.sha256` is an environment step (mnaccept links them), not a defect.

#### RESUME
Raw logs (aac6): `~/h1out/`, `~/h1out2/` (and `~/h1out_all.tgz`). Scratch clones removed. Nothing merged to main; branch `h1`.

## H2 - PLAN 28.4, multi-node part: t_mn_grid in decimal vs binary limbs on aac7 (2026-10-11)

(former file: H2.md)

Measurement only, no code changed. Build: commit 0298fe2, `make -B SHMEM_CRAY=1` (log shows -DCOMM_SHMEM), a separate worktree on aac7.
Command: `COMM_TRANSPORT=shmem LIMB_BASE=<10|2> ./tests/t_mn_grid 1 28` under `mnrun.sh <p>` inside one 6-node hold (queue item 98_h2_mngrid, rc 0, 1418 s of nodes).
Driver (reusable; WT, SIZES, CAP_S, OUTD settable): `tools/h2_mn_grid_bases.sh`.

QOS limit found (measured, sacctmgr): QOS `normal` has `node=6` per user (MaxTRESPU); the partition has 13 nodes but we may hold only 6, so 6 is the maximum used.

| run | procs/nodes | base | VERIFY | time (s, measured, incl. launch) |
|---|---|---|---|---|
| t_mn_grid | 2 | 10 | OK, 204 checks, 2 of 2 nodes, 0 failed | 109 |
| t_mn_grid | 2 | 2 (control) | OK, 2 of 2 | 113 |
| t_mn_grid | 3 | 10 | OK, 3 of 3 | 105 |
| t_mn_grid | 3 | 2 | OK, 3 of 3 | 110 |
| t_mn_grid | 4 | 10 | OK, 4 of 4 | 157 |
| t_mn_grid | 4 | 2 | OK, 4 of 4 | 170 |
| t_mn_grid | 5 | 10 | OK, 5 of 5 | 156 |
| t_mn_grid | 5 | 2 | OK, 5 of 5 | 169 |
| t_mn_grid | 6 | 10 | OK, 6 of 6 | 159 |
| t_mn_grid | 6 | 2 | OK, 6 of 6 | 168 |

Result: every run passes in both bases, including the non-power-of-two sizes 3, 5, 6 (general map). No non-pass, so nothing to classify; no disagreement between bases. Decimal wall is equal to or slightly below binary (single run per cell, not a paired timing claim).
Not run: the mnaccept multi-node steps (`mn`, `ckpt`) use one node with several processes and the default (decimal) base already; the e9 step already runs both bases. Not repeated, to keep aac7 use minimal.

## H4 - PLAN 28.5 implemented on branch `h4`: the binary limb pipeline archived, decimal limbs only

(former file: H4.md)

Branch `h4` off main 0298fe2 (NOT merged; the user decides after the measured results). Readiness plan: results/H3.md (branch h3).
Labels: measured / modelled / assumed. Times Eastern.

#### 1. What changed (measured with `git diff --numstat 0298fe2..h4`)

| area | lines | note |
|---|---|---|
| core `ecalc/*.c *.h` edited in place | **-418 / +158** (17 files) | net -260 |
| `todec.c`, `todec.h` moved out of `ecalc/` | **-345** | `git mv` to `archive:binary_pipeline/` |
| core total removed from `ecalc/` | **about -605 net** | H3 estimated -520 to -650 |
| tests `ecalc/tests/` edited in place | **-191 / +117** (20 files) | net -74 |
| `tests/t_dec.c` moved out | **-86** | `git mv` to `archive:binary_pipeline/` |
| tests total removed | **about -160 net** | H3 estimated -160 to -190 |
| `newton.c` | -40 (the binary Knuth D tail only) | NOT removed: `NEWTON_DEVICE=0` and the seeds (`newton_seed_top`) use it (H3 §1) |

Per file (core): ecalc.c -121 lines of the binary T1/dc/T2 digit-string path, `pow10_big`, `LIMB_BASE` handling; newton.c -80/+? (tail);
bigint.c the binary arms of add/sub/mul_school/mul_1/add_u64/divmod_u64, `limb_shl_bits`/`limb_shr_bits`, `need_binary`, `bi_decimal`,
`bi_set_decimal`, `bi_env_base`; `bi_shl`/`bi_shr` (by bits) are gone, `bi_shl_limbs`/`bi_shr_limbs` are the shifts (12 call sites in
newton.c / newton_db.c rewritten from `64 * k` to `k`); crt.c the binary CRT window and spill ripple, `ec_np_check` / `ec_np_prod`
without the `decimal` argument (modarith.h); rns_mul.c / rns_dist.c the binary host spill merges; dbig.c / binsplit.c / verify.c /
mn.c / mn_plan.c / mn_out.c the base conditionals. Skipped on purpose (H3 step 6, out of scope): the kernels' `bool decimal`
arguments (`k_crt_batch`, `k_crt_batch_m`, `k_crt_sub_merge`, `k_spill_merge`, `k_addsub*`, `k_carry`) keep their signatures and now
receive the constant 1 at every launch, so the GPU code generation is unchanged.

**`LIMB_BASE=2` fails loudly:** `bi_check_base()` (bigint.c) is called by `ecalc` at start, by `rns_init` and by every test's
`harness_meta`; a `LIMB_BASE` other than 10 ends the program with `LIMB_BASE=<v>: only decimal limbs (10) exist since PLAN 28.5;
the binary 2^64 pipeline is archived (archive:binary_pipeline/, git tag pre-28.5-binary)` (rc 3 through `ec_fatal`). The
switch registry keeps `LIMB_BASE` as that guard (`switches_manual.tsv`, `--check` 0 problems).

#### 2. Archived (archive:binary_pipeline/, archive:MANIFEST.md section "binary_pipeline/")

`todec.c`, `todec.h`, `t_dec.c` (git mv, history follows), `t_mul_base2.c` (a copy of `tests/t_mul.c` at 0298fe2, the last base-2
form of the multiply test). Annotated tag **`pre-28.5-binary` = 0298fe2** pushed to GitHub: the binary comparison is rebuilt from it
(`git checkout -b binary pre-28.5-binary`).

#### 3. Tests (decimal only; oracle GMP through `harness.h`)

| test | change | residue checks added where the GMP string bridge is too slow |
|---|---|---|
| `t_dbig` | `bi_env_base` call and the comment dropped; body unchanged (oracle = host bigint) | - |
| `t_mul` | binary low-product arm gone; exact GMP comparison to 2^20 limbs (was 2^26 in binary) | above 2^20 (2^24..2^30, the pair cases > 2^20, the big split): residues mod 2^61-1 **and** mod 10^18+3 and 4611686018427387847 (`limbs_mod_u64`, Horner over B), plus the length |
| `t_crt` | `crt_gmp` multiplies by B; the base switch-over and prints gone; part 3 (2^30 throughput) **kept, runs in decimal** (`t_crt 0` skips it) | - (parts 1-2 exact at 2^16) |
| `t_newton` | `mpz_recip` and the band / DKM / hook parts decimal only; `check_div` exact to Q of 2^22 limbs (was 2^26) | Q > 2^22 limbs (the 2^26 point of part 3, every case incl. the perturbed seeds): A == X Q + R mod 2^61-1, 10^18+3, 4611686018427387847, 0 <= R < Q, X's length, corrections <= 4 |
| `t_mn_grid` | `bi_env_base` call dropped; oracle = host `rns_mul` | - |
| others | `t_school` decimal only; `t_strategy` default decimal, its GMP check now through `mpz_from_limbs`; `t_primes` binary bound lines gone; `t_seed`, `t_out`, `t_patch`, `b_seed64`, `t_spill`, `t_s51_probe`, `cx_grid` drop the base calls; `t_dec` deleted (Makefile, `cx_build.sh`) | |

Scripts: `mnaccept.sh` e9 step decimal only (`e9 size 1`), `accept.sh` (no BASE), `variance.sh`, `tools/node/A.sh` (no e9b2),
`tests/pc_j1.sh` / `pc_j5.sh` (no `LIMB_BASE=10`); `ecalc/README.md` switch table (`LIMB_BASE` row = the guard, `ECALC_NP`,
`todec` row gone, the "same digits by another pipeline" sentence replaced by the GMP-reference statement of H3 §4).

#### 4. Compile checks and CPU-runnable tests (measured, login nodes, 2026-10-11 23:10-23:45 ET)

- Local (no hipcc / GMP here): `gcc -fsyntax-only` of bigint.c, newton.c, crt.c, verify.c: clean.
- aac6 login node (`module load rocm; make -s -j16` in `~/ntt-h4/ecalc`, d12fee1): **rc 0, no errors**; 24 test binaries, no `t_dec`.
- aac7 login node (`make -B SHMEM_CRAY=1 GMP_HOME=~/gmp` in `~/ntt-wt/h4/ecalc`): **rc 0, no errors, no warnings**, `-DCOMM_SHMEM`
  in the log, `ecalc` links libsma; main 0298fe2 built the same way in `~/ntt-wt/h4main` (rc 0) for the ABBA baseline.
- CPU-only tests on the aac7 login node: `ECALC_NP=3 ./tests/t_crt 0` **VERIFY OK (12 checks)**, `ECALC_NP=4 ./tests/t_crt 0`
  **VERIFY OK (12 checks)** (parts 0-2, the crt3 stripe and the four-prime base-B window; part 3 skipped with 0).
  `t_modarith` needs a GPU (login node: "no ROCm-capable device"; it is in mnaccept's unit step on aac6).
- `LIMB_BASE=2 ./ecalc 1000` and `LIMB_BASE=2 ./tests/t_crt 0` on both clusters: **FATAL ... LIMB_BASE=2: only decimal limbs (10)
  exist since PLAN 28.5; the binary 2^64 pipeline is archived (archive:binary_pipeline/, git tag pre-28.5-binary), rc 3**.
  Two compile slips were caught by the cluster builds and fixed (t_crt's `ec_np_prod` calls, t_primes's `mpz_clears`): dcfbc9b, d12fee1.

#### 5. Validation drivers (armed; fire-and-forget)

##### aac6 (1 node, PPAC_MI300A_SPX; after the CI window)
`tools/h4_aac6.sh` on the login node (`setsid nohup`), clone `~/ntt-h4` at the h4 commit (delivered by bundle). Sleeps until
**03:35 ET 2026-10-12**, then three own 45-min holds, each cancelled by the script when its steps end:
A `tools/gate.sh --build --nodes 1` (1e9 identical to the reference) + `t_dbig 24`, `t_mul 20`, `t_crt 30` (ECALC_NP=3, 4),
`t_newton 26` (host flow and NEWTON_DEVICE=1), `t_mn_grid 1 28` at 2 node-processes; B `mnaccept.sh --only unit,e9,mn`;
C `mnaccept.sh --only ckpt,recheck,corr`. Output `~/h4/` (summary.txt, log/, gate1/gate.txt, mnaccept_*_summary.txt).
**Marker: `~/h4/H4_DONE`** (`SUCCESS|FAILED pass=N fail=M`). ETA about 05:30 ET (modelled: 3 x <= 45 min).

##### aac7 (queue item, 6 nodes)
`tools/h4_aac7_item.sh` copied to `~/aac7q/queue/50_h4_gates.sh` (OWNER h4, NODES 6, EST_MIN 50, APPROVED by the coordinator):
1. `tools/gate.sh --build --nodes 2` on `~/ntt-wt/h4` (SHMEM build, 1e9 over 2 nodes, digits identical);
2. `t_mn_grid 1 28` at 2, 3 and 6 node-processes (QOS `normal` allows 6 nodes per user, measured with `sacctmgr`: the user's
   13-node allowance cannot be used; 3 and 6 are the non-power-of-2 counts);
3. 4e10 digits at 1 node, no digit writes, the e16_headline aac7 launch line, h4 vs main 0298fe2 (`~/ntt-wt/h4main`, SHMEM build
   done on the login node): 5 paired ABBA rounds on the same node; `abba.tsv`, the mean difference h4 - main with its 95 % CI
   (t, 4 df) and p in summary.txt.
Output `~/h4/q/`. **Marker: `~/h4/q/H4_DONE`**; the queue's own `~/aac7q/queue/50_h4_gates.rc` / `.log`. At arming (23:50 ET) H2's 6-node item
`98_h2_mngrid` held every node (hold 12579, -t 72 min from 23:08 ET), so the daemon starts this item when that ends: start
about 00:00-00:25 ET, 35-45 min of work (modelled), done about 01:10 ET. The aac6 driver was started 23:16 ET (aac6 clock) and sleeps.

##### Collect (one bounded command each; a fresh haiku can do it)
- aac6: `ssh aac6 'cat ~/h4/H4_DONE; cat ~/h4/summary.txt; cat ~/h4/gate1/gate.txt; grep -h "^PASS\|^FAIL" ~/h4/mnaccept_*_summary.txt'`
- aac7: `ssh aac7 'cat ~/h4/q/H4_DONE ~/aac7q/queue/50_h4_gates.rc; cat ~/h4/q/summary.txt; cat ~/h4/q/gate2/gate.txt; cat ~/h4/q/abba.tsv'`
- then: fill §6 of this file, add the RESULTS.md section (coordinator), decide merge. Clean-up after the decision: `~/ntt-h4` on
  aac6, `~/ntt-wt/h4`, `~/ntt-wt/h4main` on aac7, the queue item file.

#### 5b. Makefile fix 019e652 and the re-armed ABBA (H4b, 2026-10-11 night)

- Bug (review R1): a stray recipe line after the `todec.o` removal recompiled `crt.o` as HIP (`-x hip`, no `-fopenmp`) after the
  correct plain-C compile. Fixed in 019e652 (`ecalc/Makefile`, -1 line); no other file changed.
- aac6: `~/ntt-h4` fast-forwarded d12fee1 -> 019e652 by git bundle; `tools/h4_aac6.sh` is byte-identical in both commits, so the
  running driver (pid 2926098) was not disturbed; `ecalc/*.o` removed in that clone; `gate.sh --build` uses `make -B`, so the
  03:35 ET run rebuilds everything.
- aac7: item 50 keeps running from `~/ntt-wt/h4` (d12fee1 build): its digit gates stay valid, but its 4e10 ABBA is NOT like-for-like
  (old crt.o). New worktree `~/ntt-wt/h4b` (branch h4b) at 019e652, SHMEM build on the login node: rc 0, `-DCOMM_SHMEM` present,
  `ecalc` links libsma; `crt.o` compiled once, `hipcc -O3 -fopenmp -I. -x c -c -o crt.o crt.c` (the plain C line, measured from the log).
- New queue item `tools/h4_aac7_abba2.sh` = `~/aac7q/queue/51_h4_abba2.sh` (1 node, EST 45, APPROVED): only the 4e10 ABBA h4b vs
  main 0298fe2 (`~/ntt-wt/h4main`), 5 pairs max, early stop after pair 3/4 when |t| > the 5 % critical value (df 2/3). Output
  `~/h4/q2/` (abba.tsv, summary.txt, log/). **Marker: `~/h4/q2/H4_DONE`**; rc/log `~/aac7q/queue/51_h4_abba2.rc` / `.log`.
  Collect: `ssh aac7 'cat ~/h4/q2/H4_DONE ~/aac7q/queue/51_h4_abba2.rc; cat ~/h4/q2/summary.txt ~/h4/q2/abba.tsv'`.
  Use this ABBA, not item 50's, for the h4 timing claim.

#### 5c. H4c: independent fallback correctness run on aac6 CPX (2026-10-12)

- Why: aac6 PPAC SPX is held tonight by the admin `cdash-nightly` array, so the §5 driver (pid 2926098, clone `~/ntt-h4`) may wait
  for hours. This run is independent of it (own clone, own holds) and uses idle `SH5_MI300A_CPX` nodes (not s12-12).
- Armed 00:22 ET, driver pid 3011440 on the aac6 login node (`setsid nohup`), sleeps to **03:31 ET**. Clone `~/ntt-h4c` = d84a599
  (8d12a2e + `tools/h4c_aac6.sh` only; built on the login node, `make -s -j16`, rc 0; `ecalc/ref/e_*` linked from `~/ntt/ecalc/ref`).
  Delivered by bundle `019e652..HEAD` fetched into a clone of `~/ntt-h4` (no change to `~/ntt-h4` or its driver).
- Two holds in parallel (`sbatch -p SH5_MI300A_CPX -x aac6-sh5cpx-b -N1 --gpus=4 -t 0:45:00`, pending cap 40 min, each phase
  stops starting tests after 2500 s, holds cancelled by the script). Env `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30` (as o4_aac6.sh).
  - A: t_dbig 24, t_crt 30 (ECALC_NP=3, 4), t_newton 26 (host, NEWTON_DEVICE=1), t_mn_grid 1 28 at 2 node-processes.
  - B: `LIMB_BASE=2 ./ecalc` must fail (rc != 0, "only decimal limbs"), `mnaccept.sh --only e9` (digits vs reference), then the
    mnaccept unit list one by one, timed: t_dbig 0, t_mul 20, t_newton 20, t_verify, t_out, t_patch, t_bs, t_ntt 24.
  - Each line records PASS/FAIL, seconds, and the count of log lines containing "residue" (t_mul/t_newton print "(by residues)"
    only above 2^20 / 2^22 limbs; check `grep -a residue ~/h4c/log/t_newton_26_*.log ~/h4c/log/unit_t_mul_20.log`). Compare the
    t_mul/t_newton wall time with the >900 s stall of results/CODE_REDUCTION.md §H1.
- Expected on CPX (S44): t_ntt 24 may fail only its speed check (a CPX XCD has 1/6 of the APU bandwidth); sizes above the pool
  (t_mul section 2 up to 2^30 limbs) may OOM at POOL_LOG 29; a CPX FAIL there is a hardware limit, not an h4 defect, if the log says so.
- Output `~/h4c/` (summary.txt, phase_A.txt, phase_B.txt, log/), marker `~/h4c/H4C_DONE`.
- Collect: `sshpass -p <REMOVED> ssh aac6 'cat ~/h4c/H4C_DONE; cat ~/h4c/summary.txt; grep -ac residue ~/h4c/log/*.log' < /dev/null`
  (check `squeue -u <user>` shows no `h4cA`/`h4cB`). Clean-up after reading: `rm -rf ~/ntt-h4c ~/h4c.bundle`.

#### 6. Results

##### 6.1 aac7 item 50 (measured, 2026-10-11 23:32-23:49 ET) - build had the crt.o defect; item 51 (fixed build) is the timing of record, pending
Build: h4 d12fee1 (old Makefile: `crt.o` recompiled as HIP without `-fopenmp`, see the review R1 bug above); main 0298fe2. 6-node hold, nodes 0..5.
| test | result |
|---|---|
| gate, 2 nodes, 1e9 digits | PASS: VERIFY OK, digits identical, segv 0, ecalc total 21.09 s |
| t_mn_grid 2 / 3 / 6 nodes | PASS / PASS / PASS (VERIFY OK; 103 / 109 / 157 s) |
| ABBA 4e10, 5 pairs, every run VERIFY OK | h4 mean 50.61 s, main mean 49.00 s; h4 - main **+1.61 s**, 95% CI [-2.87, +6.09], p 0.376, sd 3.61 |
Pair differences (h4 - main, s): +7.99 (pair 1, cold start: h4 57.95 vs main 49.96), +0.29, +0.79, -0.52, -0.51. Without pair 1 the mean difference is about +0.01 s (computed from the four warm pairs). Reading: no measurable rate change, but this build had the crt.o defect, so it is not like-for-like; the digit gates (valid for the code under test) pass. The timing of record is item 51 (fixed build h4b), still pending.

##### 6.2 aac7 item 51, ABBA of record (fixed build h4 019e652, 2 nodes, 4e10 digits; measured, 2026-10-12 02:24-02:34 ET)
Build h4 019e652 (crt.o recipe fixed) vs main 0298fe2; hold 2 nodes; 5 pairs, every run VERIFY OK, stop rule: case cap of 5 pairs.
| pair (order) | h4 (s) | main (s) | h4 - main (s) |
|---|---|---|---|
| 1 (AB, cold) | 57.98 | 60.67 | -2.69 |
| 2 (BA) | 49.31 | 47.92 | +1.39 |
| 3 (AB) | 49.67 | 47.90 | +1.77 |
| 4 (BA) | 48.21 | 48.07 | +0.14 |
| 5 (AB) | 47.98 | 48.12 | -0.14 |
Mean h4 50.63 s, main 50.54 s; **h4 - main +0.09 s, 95 % CI [-2.08, +2.27], p 0.91** (t 0.12, sd 1.75, n = 5, measured). Reading: no measurable rate change from the h4 code (the binary-limb code is inert when the decimal path is used), interval about +-2.2 s of ~50 s. The earlier item-50 result (+1.61 s) used the defective build and is superseded. Driver verdict: 1 PASS, 0 FAIL.

##### 6.3 Other drivers
(pending: aac6 drivers of section 5)

#### 7. H6: the aac6 single-node timeouts (2026-10-12)

##### 7.1 Diagnosis from the aac6 logs (measured, `~/h4/` SPX and `~/h4c/` CPX)
- The aac6 §5 driver: gate 1 node PASS, t_dbig 24 PASS, t_crt 30 NP 3/4 PASS, t_newton 26 host PASS (540 s); FAIL t_mul 20 (rc 124 at
  its 1500 s timeout), t_newton 26 device (rc 143: killed when the 45-min hold ended, 8 min after it started, still in section 3),
  t_mn_grid 2 procs (no time left in the hold); mnaccept unit,e9,mn 14 PASS / 1 FAIL (the same t_mul 20 timeout); phase C
  `ckpt e8 size 2` FAIL ("3 nodes exited; restart rc 0; identical") - outside this task, not investigated here.
- H4c (CPX): t_mul 20 rc 124 at 700 s; t_newton 26 device rc 124 at 700 s while printing section 6 lines (nq 2^18+5, steady
  progress, no stall); t_mn_grid 2 procs killed for memory (rc 137, a CPX XCD has 22.9 GB); everything else PASS.
- **t_mul**: the SPX log reached the last batch group (`2^14 x 40000: several tiles`, 982M limbs) before the timeout. Each product of
  sections 4/4b went through the GMP decimal string bridge (`mpz_from_limbs`, three conversions per product), the same cost as
  results/CODE_REDUCTION.md §H1's stall on main in decimal. Test cost, not a defect (the multiplies themselves took 0.001-0.39 s per group).
- **t_newton device**: no hang seen in either log (lines kept coming until the kill). h4 runs section 3 to 2^26 by residues where
  main in decimal stopped at 2^22, and the device-only sections 5-7 use the GMP bridge at 2^20-2^22 limbs; so the device run
  is longer than the 700 s (CPX) / remaining hold time (SPX) it got. To be confirmed by the 7.2 runs (h4 vs main, per-line times).

##### 7.2 Test fix (commit 0084cfc, `ecalc/tests/t_mul.c` only)
Batch groups (sections 4 and 4b) with more than 2^24 operand + product limbs: products 0, 1, N/2 and N-1 exact against `mpz_mul`
(+ x for 4b), and every product by residues c == a b (+ x) mod 2^61-1, 10^18+3 and 4611686018427387847 (modulus about 2^184) with every
limb of c < 10^18 (OpenMP over the products). Groups up to 2^24 limbs stay exact for every product (grpB 200 x 2^14, CPU CRT, staged,
N < 8, N = 8, many tiny, 2^12, 2^14, 2^18, 2^20 CPU CRT). Oracle strength: a wrong product passes only if its error is a multiple of
the 2^184 modulus (a carry or limb error e B^j is not: none of the primes divides 10); the 4b normalised-length and paired/unpaired
hash checks are unchanged. Sections 1-2 unchanged. Not run yet on a GPU (compiled on the aac6 login node, rc 0).

##### 7.3 Reruns (armed, fire-and-forget; aac6 PPAC_MI300A_SPX)
`tools/h6_aac6.sh` (commit 0c60855) on the aac6 login node, armed 04:27 ET with `PEND_MAX=21600`; clones `~/ntt-h6` (h4 0c60855) and
`~/ntt-h6m` (main a448d73, the pre-removal code), delivered by a full bundle, both built on the login node (rc 0). Three own 45-min
holds (in parallel when nodes are free; at arming both usable SPX nodes were busy with other jobs, so start time is open):
A t_mul 20 (timeout 1500 s), t_newton 26 host (1200 s), t_mn_grid 1 28 at 2 node-processes (900 s); B h4 NEWTON_DEVICE=1 t_newton 26
(2550 s); C main LIMB_BASE=10 NEWTON_DEVICE=1 t_newton 26 (2550 s). Every log line carries `[Ns]` since the test's start (section times).
Output `~/h6/` (summary.txt, log/), **marker `~/h6/H6_DONE`** (`SUCCESS|FAILED pass=N fail=M`, 5 tests).
Collect: `ssh aac6 'cat ~/h6/H6_DONE ~/h6/summary.txt; grep -a "\] --" ~/h6/log/*t_newton_26_dev.log'` (and check `squeue -u` shows no h6A/B/C).
Reading: B and C both VERIFY OK with C's time + (the 2^26 residue point) about B's = test cost; B failing while C passes, or a
log with no new line for many minutes = report as a possible defect. Clean-up after: `rm -rf ~/ntt-h6 ~/ntt-h6m ~/h6.bundle ~/ntt-h6*.build.log`.

##### 7.4 Results
Collected 2026-10-12 (the 7.3 driver ran after the 7.5 four-node CPX run had already answered the questions; all measured; marker `~/h6/H6_DONE` = `SUCCESS pass=5 fail=0`; three own holds on the 4-APU SPX nodes, started 05:42 / 06:01 / 06:09 ET, all cancelled after their tests; every test VERIFY OK).

| run | code | result | time (s) | checks |
|---|---|---|---|---|
| A1 t_mul 20 | h4 0c60855 (batch groups by residues) | VERIFY OK | 883 | 201 |
| A2 t_newton 26 host | h4 | VERIFY OK | 539 | 708 |
| A3 t_mn_grid 1 28, 2 node-processes | h4 | VERIFY OK | 147 | - |
| B t_newton 26 device (NEWTON_DEVICE=1) | h4 | VERIFY OK | 829 | 1186 |
| C t_newton 26 device (NEWTON_DEVICE=1, LIMB_BASE=10) | main a448d73 | VERIFY OK | 758 | 1136 |

- The SPX numbers agree with the CPX run of 7.5: t_mul 20 883 s (CPX 896 s), t_newton 26 host 539 s (513 s), device h4 829 s (803 s), main 758 s (731 s). B - C = +71 s (+9.4 %), the 2^26 residue point again (CPX +72 s). Section marks (s since start): h4 section 3 ends at 514 s (main 439 s), sections 5, 6, 7 and 4 end at 687, 794, 823 s (main 614, 724, 753 s): the same shape, a constant +70 to +75 s offset from section 3 on, no gap in either log.
- Conclusion (measured): the 1500 s timeout of t_mul 20 and the 45-min hold kill of t_newton 26 device were test cost on both SPX and CPX nodes, not hangs or defects. t_mn_grid 1 28 at 2 node-processes passes on one 4-APU node (147 s); the earlier miss was the hold running out.

##### 7.5 H7: four-node CPX run of the test-cost questions (aac6, `tools/h7_aac6.sh`, 2026-10-12 05:06-05:21 ET; all measured)
Four parallel own holds, one CPX node each (RNS_INIT_POOL_LOG=29, ECALC_PLANE_CAP=2^30), h4 clone a72b8ab and main a448d73. Marker `SUCCESS pass=4 fail=0`; every test VERIFY OK. Timestamps are seconds since the test's start.

| run | code | result | time (s) | checks |
|---|---|---|---|---|
| A t_mul 20 | h4 (batch groups by residues) | VERIFY OK | 896 | 201 |
| B t_newton 26, NEWTON_DEVICE=1 | h4 | VERIFY OK | 803 | 1186 |
| C t_newton 26, NEWTON_DEVICE=1, LIMB_BASE=10 | main | VERIFY OK | 731 | 1136 |
| D t_newton 26 host (control) | h4 | VERIFY OK | 513 | 708 |

- **t_newton device: no defect, the timeout was test cost.** B and C have the same shape; section 3 (divmod vs GMP) reaches 2^22 limbs at 424 s (h4) / 426 s (main), then h4 continues to the 2^26 point by residues (498 s, +74 s, +50 checks), which main in decimal does not run. Sections 5 to 7 take 108 s / 108 s for section 5 (498 -> 606 vs 426 -> 534 s) and track each other afterwards; no log gap. B - C = +72 s (+9.8 %) = the 2^26 residue point. Time at 2^26 limbs (section 4): h4 divmod 0.77 s (recip 0.39), VmHWM 31.6 GB.
- **t_mul 20** with the 7.2 fix: 896 s (was rc 124 at 1500 s on SPX and 700 s on CPX). Where the time goes: section 1 ends at 546 s, section 2 (2^31 point, 10.0 s mdev) at 690-721 s, section 4 batch 721-812 s, 4b to 894 s; VmHWM 90.1 GB. Batch rate 2.08 Gpoint/s.
- Host control (t_newton 26 host): 513 s, VERIFY OK (708 checks), divmod 1.73 s at 2^26 limbs (device 0.77 s, 2.2 x faster).
- Test-list timeouts: `ecalc/accept.sh` runs "t_mul 20" and "t_newton 26" with no timeout (nothing to change). `ecalc/mnaccept.sh` unit step runs `t_mul 20` (and `t_newton 20`, not 26) with a 1200 s limit for every test: t_mul 20 at 896 s leaves only 1.34 x margin on a CPX node (measured), so the step now gets 1800 s (1.9 x); the other tests keep 1200 s. t_newton 26 is not in the mnaccept list. If it is added, give it >= 1500 s on device (803 s + 1.9 x margin) and 1000 s on host.
- Drivers `tools/h4_aac6.sh` (t_mul 1500 s, t_newton 1500 s) and `tools/h6_aac6.sh` are history; use 1800 s for t_mul 20 and 1500 s for t_newton 26 device in any new driver.

#### RESUME
H6 (2026-10-12): collected, §7.4 filled (5 PASS, 0 FAIL on SPX; confirms §7.5: test cost). Nothing runs. Remove `~/ntt-h6 ~/ntt-h6m ~/h6.bundle` on aac6 when no longer needed.
Earlier: branch h4 = d12fee1 (+ this file). Drivers armed as in §5; nothing runs from an agent. Next: collect both markers, write §6,
coordinator adds RESULTS.md. Open: the user's merge decision (binary-vs-decimal-only); H3 step 6 (kernel `decimal` arguments)
left for a separate, timed change.

## H5 - decimal (LIMB_BASE=10) vs binary (LIMB_BASE=2) limb pipeline, paired wall clock

(former file: H5.md)

Status: ARMED in the aac7 work queue 2026-10-12 (not yet run). Labels: measured / modelled / assumed.
Prior evidence: single unpaired 1e9 runs, 11.4 s decimal vs 20.8 s binary (measured, one run each).

#### Design
- Build: the aac7 worktree `~/ntt-wt/h4main` (main before the binary removal, both bases available, SHMEM build), not modified; its `ecalc` is used for both bases, so build, node and launch line are identical. Only `LIMB_BASE=10|2` differs. Launch line = `LINE` of `e16_headline.sh` (the aac7 headline line), 1 node via `mnrun.sh`.
- Timed runs write no digits; every run must print `VERIFY OK` (T1 residue check, T2 windows, digits == X mod q).
- Order ABBA: odd pairs decimal first, even pairs binary first. diff = binary - decimal.
- 1e9: >= 4 pairs, then stop when p < 0.05 (paired t, two-sided) or the 95 % CI half-width <= 0.5 s; cap 8 pairs.
- Digits-identical check: after the 1e9 pairs, one untimed run per base writes its digits and `digcmp.sh` compares them with the 1e9 reference (`~/ref/e_1000000000.txt`); files deleted afterwards. No reference for 1e10: VERIFY OK only.
- 1e10: only if >= 12 min of the 33 min budget remain; >= 3 pairs, cap 5; a pair starts only if the budget covers the longest pair seen plus 60 s.
- Report per size: mean decimal, mean binary, mean difference with 95 % CI (t), p, ratio, pairs; the first pair (cold start) separately, plus the difference without pair 1.
- Queue item: `tools/h5_item.sh` (copy of `~/aac7q/queue/52_h5_bases.sh`; `NODES=2` so it starts early, runs use 1 node; `EST_MIN=35`; pre-approved by the coordinator).

#### Dry run (stub, local, no cluster job)
`AQ_STUB=1` (fake run times, no Slurm): 1e9 stopped after pair 4 by the CI rule, 1e10 ran to the cap of 5; summary, done-marker and PASS lines written. The statistics (mean, sd, CI, t, p from the incomplete beta function) were checked against python on two small data sets (CI and t equal; p 9.65e-06 equals the closed form for 3 df). Not exercised: the real `mnrun`/`digcmp` path (as in the other queue items).

#### Collection
- Marker: `~/h5/H5_DONE` (first word SUCCESS or FAILED). Summary: `~/h5/summary.txt`; raw pairs `~/h5/pairs_1e9.tsv`, `~/h5/pairs_1e10.tsv`; logs `~/h5/log/`.
- `ssh <cluster> 'cat ~/h5/H5_DONE; cat ~/h5/summary.txt'`

#### Results (aac7 queue item 52, 2 nodes, build main 0298fe2 + h5, 2026-10-12 02:34-02:47 ET; marker `H5_DONE`: SUCCESS, pass 4, fail 0; measured)

Binary (`LIMB_BASE=2`) minus decimal (`LIMB_BASE=10`), paired AB/BA, every run VERIFY OK.

| size | pairs | decimal mean (s) | binary mean (s) | binary - decimal (s), 95 % CI | p | ratio binary / decimal |
|---|---|---|---|---|---|---|
| 1e9 | 4 (stop rule met) | 12.73 | 24.16 | **+11.43**, [+8.57, +14.29] | 0.00105 | 1.898 |
| 1e10 | 5 | 26.06 | 58.54 | **+32.47**, [+30.37, +34.58] | 1.8e-06 | 2.246 |

- Without the first (cold start) pair: 1e9 +12.26 s [+10.24, +14.28] (3 pairs, p 0.0015); 1e10 +32.37 s [+29.28, +35.45] (4 pairs, p 5.9e-05). Per-pair differences at 1e10: +32.91, +30.01, +33.95, +33.96, +31.54 s.
- Digit gates: 1e9 with `LIMB_BASE=10` and `LIMB_BASE=2` are identical to the reference, VERIFY OK (both PASS).
- Reading: the binary-limb pipeline is correct but **1.9x (1e9) to 2.2x (1e10) slower** than decimal on this 2-node setup; the gap grows with size (+11.4 s to +32.5 s), so it is not a startup effect. No reason to adopt binary limbs on this evidence (the user decides). The run is complete: all 1e9 and 1e10 pairs finished, `H5_DONE` present.

#### RESUME
Item armed in the queue; wait for `~/h5/H5_DONE`, then add the numbers above and a short RESULTS.md section. Branch h5, not merged.

## TREF — a test reference product, and the host product tier removed

(former file: TREF.md)

**Question.** Build the test reference designed in `results/CODE_REDUCTION.md §SH3` so the host product tier (`rns_mul`: mdev, the Karatsuba /
chunked split, the CPU schoolbook, `rns_mul_low`, `rns_mul_pair`; no production caller) can go; prove the reference first
(it must catch injected faults and agree with the old reference on every existing `t_mul` case), then move the tests and
remove the tier. No switch, no default, no digit changed.

**Change / branch.** Branch `tref` (from main b80ee2e). Commits (after the final rebase on 740bfc6): d465b08 + 06978b9 (the reference and `t_mul`'s proof mode,
against the old tier), 14eb7c6 (the removal and the tests moved), then docs. Files: `ecalc/tests/tref.h` (new),
`ecalc/tests/harness.h`, `t_mul.c`, `t_dbig.c`, `t_newton.c`, `t_mn_grid.c`, `t_primes.c`; `ecalc/rns_mul.c/h`,
`ecalc/rns_dist.c`, `ecalc/ecalc.c` (three `rns_free_scratch()` calls dropped), the registry (a count).

#### The reference (`ecalc/tests/tref.h`)

- **Exact, up to 2^21 product limbs:** GMP. The decimal-limb conversions are now a product tree (value = lo + hi B^h, h = 2^k,
  the powers B^(2^k) kept; OpenMP tasks over the halves), `harness.h`'s `mpz_from_limbs` / `mpz_to_limbs`. SH3 assumed the old
  18-digit string bridge was quadratic above 2^20 limbs; it is not (GMP's `mpz_set_str` / `mpz_get_str` are subquadratic):
  single-threaded the tree and the string form take the same time (2^20 limbs: 2.2 s in, 4.5 s out on a 4-core
  workstation, m); the tree's gain is the parallel halves (1.5x on 4 cores, m). The string forms stay as its check (`t_mul` §0).
- **Residue form, any size:** (1) every limb < B and the normalised length na + nb or na + nb − 1; (2) c ≡ a b modulo three
  62-bit primes (`t1_q[0..2]`, `vf_limbs_mods`: O(n), parallel); (3) the low 64 limbs exactly (a b mod B^64 from the low 64
  limbs, GMP); (4) the top limbs: with A = Ah B^ka + Al (Ah the top 64 limbs) and B likewise, floor(c / B^(ka+kb)) must lie in
  [Ah Bh, Ah Bh + Ah + Bh] (exact bounds, not an estimate). One wrong limb changes c by d B^k, 0 < |d| < B, never a multiple of
  a prime q > B, so (2) alone always catches it.
- `tref_mul` (a reference *value*, used by `t_newton`, `t_mn_grid`, `t_dbig`): GMP up to 2^21 limbs, else the one-node device
  product (`rns_mul_dist_db`) at the default plane — the tests' grids run under `DIST_LOGN_TEST`, so the value comes from a
  different plane size — checked by the residue form (a failure is a VERIFY failure). `tref_faults` tests the reference itself.

#### Method

aac7 queue items (2 nodes; logs on aac7 under `~/tref/q1/`, `~/tref/q2/`, raw, not in git; drivers in `archive:drivers/tref/`):
- **Proof** (the old tier still present): `t_mul 20 tref` at commit 3e1b35b (06978b9 after the rebase). On every product of the old sections 1 (pool 2^20:
  tiers and boundaries, 13 shapes x 5 generators), the low products, 2 (2^16 .. 2^30 limbs per operand) and the pair section:
  `tref_mul`'s value == `rns_mul`'s; the old check (GMP / three residues) accepts `tref_mul`'s value; `tref_check` accepts
  `rns_mul`'s (exact, and also the residue form at small sizes); one limb changed at the low end, the middle and the top must
  fail the new check (both forms) and the old one.
- **Before / after** on one node of the same hold: `t_mul 20` at main b80ee2e and at the branch.
- **New tests:** `t_mul 20` (§1 device path vs GMP at two plane caps, low products; §2 to 2^30 by the residue form, plane caps
  agreeing at 2^20, 2^24, 2^26; every product of §1-§2 also fault-tests the reference), `t_dbig 24 prod`, `t_primes gpu 26`.
- **Gates:** `tools/gate.sh --build --nodes 2`, `--nodes 1` (10⁹ digits, aac7 line); `ecalc/mnaccept.sh <hold> --only unit`.

#### Results (all measured)

| item | result |
|---|---|
| proof, run 1 (sections 1, low products, 2 up to 2^24 x 2^24) | 72 products: value `tref_mul` == `rns_mul` on all 72; 0 VERIFY failures; every injected fault caught (new check, residue form, old check). The run was OOM-killed at 2^26 x 2^26 (one process holding the host tier's pinned staging and pools and the device tier's; VmHWM of the old `t_mul` alone 114.6 GB, of the new 128.1 GB) |
| proof, run 2 (sections 2 + pair, value agreement to 2^25 product limbs, above it cross-acceptance) | 17 products (11 by value: 2^16 .. 2^24 x 2^24 and the pair cases at 2^12, 2^20; 6 by cross-acceptance: 2^26, 2^27, 2^28, 2^30 and pair 2^27+5): 11 of 11 agree, the new check accepts all 17 of `rns_mul`'s products; 51 faults injected: 0 missed by the new check, 0 by the residue form (9 small cases), 0 by the old check; VERIFY OK (150 checks), 330 s |
| `t_mul 20` before (main b80ee2e) | 737 s, VERIFY OK (169 checks) |
| `t_mul 20` after (branch) | **187 s** (−550 s, −75 %), VERIFY OK (260 checks); 23 fault sets x 3 injected, 0 missed |
| `t_dbig 24 prod` (products to 2^27 against the reference) | 67 s, VERIFY OK (565 checks) |
| `t_primes gpu 26` (both plane sizes, closed form) | 45 s, VERIFY OK (89 checks) |
| gate `--build --nodes 2` / `--nodes 1` | PASS / PASS (item 2, branch a5ad2cd: 10⁹ digits identical, VERIFY OK; ecalc total 21.84 s at 2 nodes with P24, 9.76 s at 1 node) |
| `mnaccept.sh --only unit` (item 1, branch 3e8665c, 14eb7c6 after the rebase) | 9 passed, 0 failed: t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20 (2756 checks), t_verify, t_out, t_patch, t_mn_grid at 2 processes (244 checks; its reference is now `tref_mul`) |

Reference cost inside `t_mul` §2 (with the three injected faults): exact 2^20 x 2^20 8.2 s; residue form 2^24 x 2^24 0.07 s,
2^30 x 2^30 2.7 s (m). The device product at 2^30 x 2^30 took 2.9 s.

**Lines** (`git diff --numstat b80ee2e..tref`, m): production code −404 / +10 (`rns_mul.c` −341 / +3, `rns_mul.h` −32 / +5,
`rns_dist.c` −27, `ecalc.c` −4 / +2); tests +393 / −148 (`tref.h` +148 new, `harness.h` +93 / −6, `t_mul.c` +102 / −105, the
other four tests +49 / −37); docs +10 / −1. Net over code, tests and central docs: −141.

Removed (byte-identical pre-change files in `archive:removed_2026-10-13/tref/ecalc/`): `rns_mul`, `rns_mul_low`, `rns_mul_mdev`,
`rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`, `mul_rec`, `rns_free_scratch`, `mdev_gpu_crt`, `mdev_pts`,
`node_copy`, `par_copy`, `par_zero`, `x_inv_pw`, `rns_school_max`, `rns_crt_threads`, `rns_mdev_gpucrt`; `rns_mul_dist` (host
bigints), `acc_flat` and its staging in `rns_dist.c`. `t_mul`'s sections 1-3 (the host tier's tests and timings, args `big`,
`big32`) are replaced by the device-path sections; `t_primes` tier 0 (`rns_mul`) is now the device path at half the plane.

**Verdict.** The reference catches every injected fault (0 missed of all sets, both forms) and agrees with the old reference on
every case: 83 products by value, 6 large ones by cross-acceptance; the host tier is removed. `t_mul 20` is 3.9x faster.

**Open issues.**
- `struct acc`'s `flat` branches in `rns_dist.c` (the host-result spill merge, `A.flat` tests) are now dead: about 25 lines, left
  for a cleanup pass (kernel code; not touched here).
- `rns_stats` keeps the host tier's fields (`n_mdev`, `n_split`, `t_repack` ...), always 0 now; `ecalc.c` still prints
  `n_mdev` / `n_split` in three summary lines. Dropping them changes ecalc's output lines (left to the coordinator).
- The tests lost the host tier as an independent code path; the replacement independence is the residue form (no product)
  and the device path compared at two plane sizes (`t_mul` §1, §2; `t_primes`; `t_dbig` grid vs one plane).

#### RESUME

State: done; merged to main. Nothing open but the open issues above.
