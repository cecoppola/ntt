# CODE_REDUCTION — shrinking the code

Scope: audits, shrink passes, settings-removal waves, validation of the reduction, the limb-format study (decimal vs binary) and the host product tier removal. Old item IDs resolve through `results/INDEX.md`; raw data and drivers are in `archive:results_raw_2026-10-19/`. Labels: m measured, mod modelled, a assumed.

## Topic summary

- **What this is.** The 2026-10-11..13 campaign to make ecalc smaller without changing a digit: two read-only audits (AUD2 dead code, AUD3 structure), four removal merges (SHR2, SH3, SIMP, TREF), two settings-removal waves (W2A, W2B), a speed/size validation (L2), a backlog clean-up (O8) and the decimal-vs-binary limb study (H1, H2, H4, H5).
- **Current state (all merged to main unless noted).**
  - Settings: 269 registry rows before wave 2 lane B, 185 after lane A, 170 after SHR2 (m); W2A removed 42 switches + 1 value, W2B 42 rows, SHR2 16.
  - Code: SHR2 -1531 lines, SH3 -261, SIMP -418 (ecalc sources), TREF -404 production, W2A about -1800 (removed minus added about -1500). Every step gated: 1 and 2 nodes, digits identical.
  - Single-node flow is now the device DKM division at every size (SIMP): 10^10 digits on 1 node 8.3 s faster (m). The 1-node 10^9 gate now runs the production division.
  - Only the top-level checkpoint set (`ECALC_CKPT_TOP`) remains; no restart inside a run (D11).
  - TCP transport, SOS/OSHMEM build branches, spill primitive, 3·2^k distributed planes, `B4` strategy form, host product tier (`rns_mul` etc.) are gone. `ECALC_OUT_PACKED` default is 2 (60-bit packed, D5).
  - Limbs are decimal only (H4, branch `h4`, tag `pre-28.5-binary`): `LIMB_BASE` other than 10 fails loudly. Evidence: binary is 1.9x (10^9) to 2.2x (10^10) slower (H5, m). Check ecalc/README.md for the current `LIMB_BASE` row; if this summary and main disagree, main wins.
- **Live conclusions.** The reduction was digit-safe at 10^11 and speed-neutral at 6.441e10 (L2); removed paths were off at the defaults, so no timing was needed except SIMP, B3 (null) and H4/H5.
- **Still open (not done).** Dead `struct acc` `flat` branches in `rns_dist.c` (about 25 lines); `rns_stats` host-tier fields and ecalc's `n_mdev` / `n_split` print lines; duplicate Garner `dmod128/192`; `mn_powmod` / `powmod_h`; `k_store`/`k_copy_h2d` partly merged (SH3); AUD3 items 5-10 (size 1 through the sharded driver, P24 only, `tree_level` fold, one writer, one `mdb_add_shifted`, one transform cache); AUD3 decisions 11-13 (11 and 12 done as D5/D11; 13, the SHMEM data path, kept until the target's A3 stage); `LIMB_BASE` guard row. X2 + cut last group symmetric-offset hazard (O8) is documented, no guard built.
- **Superseded.** AUD2 items 1, 4, 5 (part), 6, 8 (part) by SHR2/SH3; AUD3 items 1-4 by SIMP; the host tier open issue of SH3/SHR2 by TREF; H1's t_mul decimal stall by TREF's residue reference and H4 §7.2.

## §AUD2 — removal / consolidation audit of C/HIP sources, tests and Makefile (read-only, 2026-10-12)

Base main 4f7a103; nothing edited. Method: scripted scan of every function definition in `ecalc/*.c|h` for callers (transitive), tests vs TESTING.md / gate / accept scripts, Makefile rules, duplicate bodies (hash), `#if 0` / single-use defines, header declarations without definition.

**Result (m): about 1620 removable lines (items 1-9) plus up to 790 lines of history comment blocks (item 10, upper bound).** Only about 290 are in `ecalc/*.c|h`; the bulk is tests and on-demand probes.

| # | item | lines (m) | risk |
|---|---|---|---|
| 1 | tests with no Makefile rule: `t_nicbw.c` 259, `t_a2a_sched.c` 140, `t_school.c` 49, `n3x_lengths.py` 28 | 476 | low |
| 2 | CPU seed-leaf microbench `b_seed64.c` + rule | 274 | low |
| 3 | GPU gather micro-probes (3 files + rules; shared `k_diff` / `k_fillsrc`) | 271 | low |
| 4 | pack/unpack benchmark and reference kernels (`dist_pack_bench`, `gen_pack_bench`, `DIST_PBENCH`/`DIST_GBENCH` blocks of `t_dist`) | 226 | low-med |
| 5 | test-only API: `ntt_host_fwd/inv`, `ntt_pass*`, `rns_mul_dist`, `vf_digits_mod`, `dist_inv_t*`, `ec_shoup_lazy`, one-liners | 165 | med (tests edited) |
| 6 | functions with no caller anywhere (`rns_ensure_staging`, `rns_mul_dist_hd`, `db_pregrow`, `db_pool_*`, `comm_layered_create_minor`, `bi_mul_school`, `bi_sub_shifted`, `bi_bits`, `k_twiddle*`, `ec_add/sub/shoup*`, ...) | 126 | low |
| 7 | `t_bounce.c` (O2 microbenchmark) + rule | 41 | low |
| 8 | duplicated helpers (`k_store`=`k_copy_h2d`, `dmod128`=`p24_dmod128`, `dev_pow_table`=`pow_table`, `powmod_h`=`mn_powmod`, `make_mdb`, `digits_format`, `verify_done`) | about 26 net | low-med |
| 9 | NOP-modmul bench build (`ntt_nop`, `NTT_NOP_MODMUL` blocks) | about 15 | low |
| 10 | long history comment blocks (787 lines in blocks of >= 10 lines; about 990 lines carry phase/result tags) | up to about 790 | judgement |

Checked and clean: no `#if 0`; no header declaration without definition; no single-use defines or unused statics. Live `#ifdef` forks: `COMM_HOST_ONLY`, `MN_OUT_LLAPI`, `EC_PRIMES` (hard-wired 1; the 0 branch only for `t_roots`), `NTT_NOP_MODMUL`. Tests in `make all` not named in TESTING/gate/accept (`t_roots`, `t_alloc`, `t_copy_order`, `t_hred_race`, `t_cap`, `t_strategy`, `t_seed`, `t_primes`, `t_ntt3`) are cited as evidence and kept; doc gap: TESTING.md should list `t_strategy`. Order: items 4 and 6 after lane A (more orphans expected after A5); 1, 2, 3, 7, 9 any time (archive, do not delete). The scan is name-based (macro-pasted names would be missed; none seen).

## §AUD3 — where ecalc's algorithms and code structure can be simpler and smaller (read-only audit, 2026-10-12)

Question: which duplicate paths, tiers and layers can be merged or removed and at what time/memory cost. Base main 4f7a103 (18 885 lines of `ecalc/*.c|h`, m). Time and memory effects are **mod**, no runs.

**Coverage findings.**
1. The 1-node 10^9 gate did not run the production single-node flow: P, Q went to the device flow only if the top level ran on the device tier (below about 1.9e10 digits, mod, it ran the batch tier), so the gate ran the host flow with the full-length-reciprocal division, not DKM. Fixed by SIMP item 1.
2. No aac7 gate at <= 16 nodes ran the target's P24 product form (four primes): pool 0 holds four planes automatically only for g >= 32. A gate step with `ECALC_NP_AUTO_TERMS=1000000` at 2 nodes is the stopgap. (Since D18, P24 is the multi-node default; see `results/PRIMES_STRATEGY.md §P2415`.)
3. Gates run only power-of-two node counts; the target's top levels (192, 576) use the general map, exercised only by 3- and 6-process runs.

**Ranked items** (lines net removed, mod time/memory):
- **1** One single-node division flow, drop the host-P/Q flow: about -120; ~0 time at >= 1.9e10, small sizes get faster (DKM -25.7 s at 10^11, `results/DIVISION.md §DKM15`). Low risk. Done in SIMP.
- **2** DKM only: about -130; 0 cost (DKM runs above about 72 digits). Done in SIMP.
- **3** Drop the distributed tier's 3·2^k planes (`dist3`, `DIST_R3`, `RNS_PLANES_3Q30`, two plane-cap values): about -140; best case for the removed form 80.8 +- 1.3 vs 82.0 +- 0.3 s at 4e10 on one node (`results/MEMORY.md §I` item 4, m), within noise. Done in SIMP.
- **4** One grid planner and loop (six shape functions to one; `mul_grid`/`mn_grid` loops): about -60; identical by construction. Done in SIMP.
- **5** Size 1 through the sharded driver: about -310 net; memory is the risk (one-node peak laid out for the dbig flow; `mdb_shift` at g = 1 copies, 29 GB per copy of Q at 6.44e10); medium-high, needs 1-node ABBA and `mem_report`. **Open.**
- **6** P24 as the only multi-node product form: about -50; time about equal per product (mod, unmeasured at aac7 sizes), +4 GiB per APU in pool 0 at 2-16 nodes. **Open** (partly overtaken by D18).
- **7** `tree_level` into `tree_level_k` (about -25), **8** one output-writer machinery (about -70), **9** `mdb_add_shifted` one body (about -50), **10** one transform cache (about -60; 1-node ABBA needed). **Open.**

**Decisions for the user.** 11: write only the 60-bit packed form (about -120 in `mn_out.c`): the default became 2 in W2A D5, ASCII/64-bit kept. 12: keep only the top checkpoint set (about -350 lines of leaf/tree restart): done in SHR2 D11. 13: drop the SHMEM data path of device exchanges (about -120): no fallback if libfabric fails; kept until the target's A3 stage has run.

Smaller items: `mn_group`'s transform-node subset (`gt`, `tr`) is a leftover of `MN_TOPO_GROUP`; `mn_prod_cut_x1` never triggers at target sizes; two start-up self-tests (`mn_selftest`, `mn_selftest_layered`) overlap.

Considered and not recommended: general map for every group size (its exchange is 2.3-2.4x slower per APU thread at 6 nodes, `results/TIME_MODEL.md §S48`, m); one batch form (striped form serves the few large upper-level products); fewer NTT kernels (`plan_auto` needs the partial passes, `results/KERNELS.md §K13b`).

Handed to AUD2: the host product tier (about 270 lines of `rns_mul.c`, only caller an unreachable branch; tests use it as reference), `ckpt_hdr.mdev_host`, `mn_out_scatter_standin`.

Verdict: do items 1-4 first (about -450 lines, no time cost at the defaults); 5 later after 8; 6 mainly as a coverage fix; 11-13 are user decisions.

## §SIMP — AUD3 items 1-4: one single-node division flow, DKM only, no 3·2^k distributed planes, one grid planner

**Question.** Can AUD3 items 1-4 be done with identical digits, no slowdown, and does item 1 close coverage gap 1?

**Change / branch.** Branch `simp`, commits in the order 3, 4, 1, 2. No new switch. Removed settings: `DIST_R3`, `RNS_PLANES_3Q30`, the `3*2^29` / `3*2^30` values of `ECALC_PLANE_CAP` (2^30, 2^31, `fit` stay; `off` = `POOL_LOG` and `DIST_LOGN_TEST` as given).

| item | what went | lines (m) |
|---|---|---|
| 3 | `dist_r3`, `plan3`, `k3_*` kernels, `dist3_fwd/inv`, r3 branches; the 3q30 size rule; 3·2^k plane-cap values | +78 -202, net -124 |
| 4 | six shape functions to one `grid_search`/`grid_shape`; three piece loops to one `grid_loop` | +152 -154, net -2 |
| 1 | size-1 host flow in `ecalc.c` (host residues, host A, full-k host reciprocal, `newton_db_divmod`, host top-set write, `out_ctx.X`), host pool of A; `binsplit.c` hands the batch-tier top level over as device numbers at size 1 | +60 -203, net -143; tests +75 -56 |
| 2 | full-length-reciprocal bodies of `newton_db_divmod_shifted` / `newton_mn_divmod`, `newton_dkm_on/set`; DKM covers every k; size > 1 refuses k_mu < 5 | +16 -165, net -149; tests +15 -18 |
| total | 21 files, +407 -806 (m); ecalc sources net **-418** | |

**Results (m).**
- Plan sweep (`MN_PLAN_ONLY`, `BS_LAYOUT_ONLY`, 119 points x 2 = 238 files, five settings): items 3+4 vs main d1d19bf and the merged tip vs main 89318ae: **0 differ** in 5 x 238 files.
- Merge gates on 62064f2 (aac7): gate 2 nodes P24 forced PASS (23.81 s), gate 1 node PASS (10.80 s); mnaccept unit (`t_newton 20`: 1648 checks OK), e9, mn, corr: all PASS, digits identical. The 1-node gate log now shows the `I3:` line and `divmod(dev, DKM)`: the gate runs the production division (coverage gap 1 closed). Final gate after rebase on main 68a9d69 (P24 default): gate 2 n 22.27 s, gate 1 n 10.63 s PASS; mnaccept unit,e9,grid,mn,corr 18 passed, 0 failed.
- ecalc 1 .. 10^5 digits (20 sizes) prefix-identical; `t_newton` quotient section 320 divisions identical to GMP.
- **Timing, 10^10 digits, 1 node, ABBA 4 rounds: mean A (main) 26.85 s, B (SIMP) 18.54 s; B - A = -8.31 s, 95 % CI [-9.68, -6.93], t = -19.2.** Phases: dm 5.5 -> 1.7 s, 10dP 0.9 -> 0.0, bs 8.5 -> 6.7, T1 0.0 -> 0.5. Above about 1.9e10 digits the flow was already the device flow (about 0, mod).
- One `t_newton` failure (1 of 1648 checks) after item 1: section 5 drew a different Q because section 3's RNG use changed; for that Q the reciprocal at nq 131075, k 65538 was -22 units from GMP's floor vs the +-8-unit bound. With section 5 on its own RNG (`rng5`) main and SIMP give identical errors, so SIMP does not change the reciprocal (pre-existing outlier of `recip_db2`).

**Verdict.** Adopted (merged): digits identical at every size tested, planner byte-identical, 10^10 at 1 node 8.3 s faster.

**Open.** `recip_db2` +-8-unit outlier (not investigated); `dm_layout` still sizes the size-1 division below 1.9e10 digits for the full-length set (larger than DKM needs; never short); `t_cap.c` keeps a mirror of the old 3·2^k caps; AUD3 items 5-10 not done.

## §SH3 — shrink pass 3: rns_dist.c dead items, duplicated helpers, the host product tier (2026-10-12)

**Question.** Finish the SHR2 follow-ups. No switch or default changed. Branch `sh3` from main 5081105.

**Removed (lines, m).** `gen_pack_bench`, `gb_cmp`, `gb_dcmp`, plain kernels `k_twpack_g`/`k_unpacktw_g`, `DIST_GBENCH` block of `t_dist`, rows `DIST_GBENCH`/`DIST_PREPS`: rns_dist.c -99, t_dist.c -8, registry -3. `pow_table` -7, `part_owner` -1, `rns_mul_dist_hd` -18. `k_store`=`k_copy_h2d` merged into one kernel (`db_copy_kernel`; rns_mul.c -9/+3). `tests/cx_grid.c` and two shapes files archived to `archive:removed_2026-10-12/sh3/` (tests -115). Total: rns_dist.c 2284 -> 2155 lines; all files -261 net. **Kept:** `acc_flat`, `rns_mul_dist` (test callers). **Skipped:** `mn_powmod`/`powmod_h` (wait for RECIP), `dmod128/192` (wait for K4c).

**Host product tier (about 270 lines) kept; design for removal** (executed in §TREF): a residue-form reference (three 61-bit primes, exact low 64 limbs, top limbs from the leading 64) for large sizes; GMP below 2^20 limbs via a product-tree base conversion; then drop `rns_mul`, `rns_mul_low`, `rns_mul_mdev`, `rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`, `mul_rec`, `mdev_gpu_crt`. Risk: tests lose an independent code path; run the residue check on the device path at a second plane size first.

**Gates (aac7, m).** gate 2 n PASS (P24, 21.91 s), gate 1 n PASS (10.49 s), `mnaccept --only unit,e9,mn`: 15 passed, 0 failed, digits identical; doccheck 0.

## §SHR2 — shrink pass 2: top-level checkpoints only, the unreachable stand-in, SOS/OSHMEM builds, AUD2 re-scan (2026-10-12)

**Question.** Remove the code the user approved on 2026-10-12 (D11, D16, D14) plus re-scanned AUD2 items and the residue log, without changing a digit. Branch `shr2` on main d1d19bf, eleven commits. No switch added, no default changed.

**Removed (lines +/-, m).**

| item | what | +/- |
|---|---|---|
| D11 | leaf-level snapshots and restart, tree sets, restart scan, abort hooks, checkpoint counters; 8 switches (`BS_CKPT_EVERY`, `BS_CKPT_MIN_LEVEL`, `BS_RESTART`, `BS_CKPT_TREE`, `BS_CKPT_TREE_EVERY`, `BS_CKPT_ABORT`, `BS_CKPT_ABORT_TREE`, `BS_CKPT_ABORT_NODE`). Top-level set (`ECALC_CKPT_TOP`, background writer, header v3, v2 still read) stays; `BS_CKPT_DIR` only names its directory. mnaccept `ckpt` step tests the top set, `sched` step gone | +99 / -461 |
| D16 | `out_stage`'s stand-in scatter branch, `mn_out_scatter_standin` | +6 / -34 |
| D14 | Makefile SOS and OSHMEM branches, `COMM_SHMEM_SOS`/`OSHMEM` code, README rows `COMM_SHMEM_SERIAL`, `COMM_SHMEM_DEVHEAP`, `COMM_SHMEM_ORDER`. Cray path unchanged | +34 / -109 |
| AUD2 6 + AUD3 | about 17 callerless functions, `comm_layered_create_minor`, the unreachable host `rns_mul_pair` branch of the mdev levels | +14 / -151 |
| AUD2 4 | `dist_pack_bench` and its kernels, `DIST_PBENCH`, `DIST_PSIZE` | +6 / -136 |
| AUD2 1 | `tests/t_nicbw.c`, rows `NICBW_*` | +2 / -268 |
| AUD2 5 (part) | `t_ntt bench` (K13/K13b microbenchmarks), `ntt_pass*` | +0 / -231 |
| AUD2 8 (part) | one `digits_format` in `harness.h` | +13 / -22 |
| RES_LOG | `ECALC_RES_LOG`, `_LEVEL`, `_CPU` and every check they enabled | +6 / -121 |
| total | 39 files | **+178 / -1531** |

Registry rows (m): **186 before, 170 after** (16 removed: 8 checkpoint, 3 residue log, 3 `NICBW_*`, `DIST_PBENCH`, `DIST_PSIZE`). Archived: `removed_2026-10-12/ecalc/tests/t_nicbw.c` and, under `removed_2026-10-12/shr2/ecalc/`, the pre-change `ntt.c`, `tests/t_ntt.c`, `ntt_dist.c`, `tests/t_dist.c`.

**Gates (aac7, m; queue items 81, 82, 83, SHMEM build rc 0, 0 warnings).** Gate 2 nodes P24 forced: PASS, identical (20.40 / 17.05 / 23.91 s); gate 1 node PASS (11.72 / 11.86 / 12.70 s); unit tests PASS (`t_mn_grid` 180 checks); e9 size 1 PASS; `ckpt`: top set written and read by the recheck (3 RECHECK OK), budgeted drop (`ECALC_CKPT_TOP=2`, 1 MB/s emulated: 2 ABANDONED, no header), `BS_CKPT_DIR` alone writes nothing; `recheck` e9 size 1 and e8 size 2 PASS (corrupted copy fails). Totals are single runs on a shared machine, not timing claims; removed code was off at the defaults.

**Verdict.** Merged: all gates and mnaccept steps PASS with digits identical; 1531 lines and 16 settings fewer.

**Open (left for later).** Host product tier (done in §TREF); `rns_dist.c` items (done in §SH3); `mn_powmod`/`powmod_h`; `newton_dkm_set` stays (tests' override); duplicate device Garner (`dmod128/192`) and `k_store`/`k_copy_h2d` need their own codegen check; `make_mdb`; kept as test oracles: `ntt_host_fwd/inv`, `crt_garner3/4`, `db_pool_extents`, `db_mod_q`, `db_set_base_pow`, `vf_digits_mod`, `tier1/tier2`, `dist_inv_t`, `ec_shoup_lazy`. `t_ntt bench` is cited by `results/KERNELS.md §K13`, `§K13b`, `results/KERNELS.md §NTT3P` (history; code in the archive).

## §W2A — wave 2, lane A of the user-approved settings removal (2026-10-12)

Batches A1, A3, A4, A2, D5, A5, all on main; every removed path was off at the defaults (or its default is now built in), so digits are identical by construction. Each batch gated on aac7 (1 and 2 nodes, 10^9 digits identical, VERIFY OK, then listed mnaccept steps); no timing runs.

| batch | removed | lines (m) | gates (m) | commit |
|---|---|---|---|---|
| A1 | `ECALC_VMM_BG=2` (value only), `ECALC_VMM_SAFE`, `_REPRO`, `_REPRO_SPIN`, `DBIG_ADDSUB2/3`, `DBIG_QSEL`, `DBIG_MAXIDX_TOP`, `DBIG_MODQ_MONT`, `DBIG_S51_STATS`, `DBIG_BOUNCE_TS`, `DB_POOL_TRACE`, `MEM_COPY_WAIT_POLL`, `MEM_COPY_WAIT_REPORT_S` (13 + 1 value); `t_s51_probe.c` | +66 / -546 | 1 n PASS (13.07 s), 2 n PASS (20.28 s); unit PASS | a77a2f1 |
| A3 | `NEWTON_ANCHOR`, `NEWTON_LOWPROD`, `NEWTON_HIGHPROD`, `NEWTON_DKM` (built in on), `DM_MN_LEAN` (built in on), `MN_MODEL_TCP`, `COMM_HS_TS` (7) | +49 / -162 | 1 n, 2 n PASS (18.56 s); unit, corr (1, 2, 4) PASS | 53bdfc1, 23224a9 |
| A4 | `ECALC_SPILL_DIR`, `SPILL_CHUNK_MB`, `SPILL_BUFFERED` (spill primitive, `t_spill.c`; `sp_file` stays) | +6 / -385 | PASS (21.49 s) | ed25de2 |
| A2 | 17: `MN_COMBINE`, `MN_DM`, `MN_LAYERED_RAW`, `MN_LAYERED_LOCAL`, `MN_SELFTEST_GROW`, `MN_TOPO_GROUP`, `BS_SCHOOL_NL`, `LIMB_BASE` (later re-added as guard, §H4), `MN_PLAN_DEXTRA`, `ECALC_SEED_ORDER`, `BS_POOL_RULE`, `BS_CKPT_TREE_BG`, `MN_TREE_EARLY_FREE`, `ECALC_VSLOT_BUDGET`, `COMM_LAYER_VSLOT_PREALLOC`, `COMM_SHMEM_POOL_AUTO`, `COMM_INIT_EARLY` | +105 / -283 | PASS (20.62 s); unit, e9, grid, ckpt, sched 6/6; `BS_LAYOUT_ONLY` byte-identical at 6 shapes; `mem_model.py --check-c` exact | 4e67fde |
| D5 | default `ECALC_OUT_PACKED` 1 -> 2 (60-bit packed; user decision, TASKS 40); `=0` and `=1` stay | +24 / -21 | PASS (16.05 s, 26.48 s); unit, e9, mn, recheck, corr 20/20 | fe9f21d, d12ac2c |
| A5 | `ECALC_OVERLAP`, `NEWTON_DEVICE`: sequential flow, host reciprocal and division; size-1 host-P/Q path for small runs kept (reached at the defaults then) | +58 / -210 | PASS (12.04 s, 18.91 s); unit, e9, mn 14/15 (t_patch artefact fixed in D5) | c7942bc |

Totals (m): 42 switches + 1 value removed, 1 default changed; about 1800 lines removed, 300 added; registry 257 at lane A start, 185 after; `switch_registry --check`, `profile_load --validate-all`, doccheck 0 problems after every merge.

**Notes.**
- **Push leak guard false positive (A2).** The push hook rejects diffs whose lines contain a C token it reads as a domain name; `out_stage` has such lines, so the stand-in scatter branch stayed until SHR2 D16 removed it.
- **Shared mnaccept directory.** `aac7env.sh` defaulted `MNACCEPT_TMP` to one shared directory; concurrent items deleted each other's digit files ("Stale file handle", "cannot open part"; items 46, 47). Passed with a private directory (`MNACCEPT_TMP=<cluster work dir>/<item>/mnaccept_tmp`, items 50, 52). Every item running mnaccept sets its own.
- **t_patch under the 60-bit default (D5).** `packed_part_check` read 8 bytes per limb: 2399 false failures under `=2`; fixed in d12ac2c, the writer and patch were correct (`corr`, `recheck` passed).
- **Not changed.** `mn_model.py PACKED_BPD` still prices the 64-bit form (the 60-bit form is 6.25 % fewer bytes). A5: plain `t_newton` now runs the device sections (1098 checks instead of 620).

## §W2B — wave 2, lane B of the settings removal (2026-10-12)

User-approved removal of experiment / adopted-default switches (transport, NTT kernels, distributed products); each removal keeps today's value, digits identical by construction, each batch gated on aac7 (SHMEM build).

| batch | commit | switches | lines (m) | gates (m) |
|---|---|---|---|---|
| B1 transport | 2646a63 | 12 + `COMM_HS_TS` stamps | 11 files, +45 -322 | gate 2 n + 1 n PASS; 2 n x 6.441e10/node VERIFY OK, 170.1 s |
| B3 NTT kernels | 3cfab5d | 12 + `ntt_b1_shoup` | 12 files, +156 -1039 | gates PASS; `t_ntt hash` main vs B3: 2560 cases, 0 differing; ABBA 1 n 6.441e10, 4 pairs: **B3 - main -0.40 s, 95 % CI [-3.84, +3.04]** (null) |
| B2 distributed products | merged | 16 | 17 files, +115 -473 | gates PASS (twice, after A1/A3 rebase); 2 n x 6.441e10/node 167.3 s; `t_strategy 24 2 V`, `mnaccept --only grid,mn` PASS; `MN_PLAN_ONLY=3.71e13:576` identical but for the P24 print |
| B4 TCP transport | merged | 2 + value `tcp` | 23 files, +57 -776 | gates PASS; 2 n x 6.441e10/node 158.0 s; `t_comm` 2 PEs PASS |
| D1e (TASKS 27) | merged | 0 removed, 6 now default 1 | 16 files, +60 -53 | gates PASS incl. all `=0` fallbacks; 2 n x 6.441e10/node 134.0 s; plans identical |

Registry (m): 269 rows before lane B -> 204 on main with lane A's A1, A3, A4 (lane B removed 42: B1 12, B3 12, B2 16, B4 2); `switch_registry --check` and `profile_load --validate-all` 0 problems.

- **B1:** removed `COMM_SHMEM_RPIPE`, `_PEER_ORDER`, `_KEEP_STAGING`, `_THREAD`, `_NOSYM`, `_PUT_MB`, `_STAGE_SLOTS` (regions always on when sized), `COMM_LAYER_TEST_SKEW_MB`, `COMM_PUSH64`/`COMM_PUSH_BLOCKS` (64-bit stores, 76 blocks hard-wired), `COMM_OFI_DUMP` (always), `COMM_OFI_SYM_TOP` (follows `COMM_LAYER_VSLOT_POOL`). `t_ofi_alloc 1` fails exactly as on main (m).
- **B3:** removed `NTT_B16_BODY`, `NTT_MODMUL` (FP64 Barrett/Shoup forms), `NTT_B1R`, `NTT_PLAN` (plan_auto always), `NTT_R3_FUSE`, `NTT_B16_STG`, `PW_FUSE`, `RNS_R3_MINK`, `RNS_BATCH_PAIR`, `RNS_STRIPED_PAIR`, `RNS_PLANES_FIRST`, `RNS_POOL1_4Q`; `t_ntt` variant sections removed, `hash` kept for old/new diffs.
- **B2:** deleted `MN_REDIST_FUSE`, `RNS_CRT_SUB`, `DIST_MN_SYM_SLABS`, `RNS_STRATEGY_FORM` and the B4 strategy form (`RNS_STRATEGY=B4` refused), `DIST_TEST_LAG_MS`. Hard-wired: `RNS_DIST_CACHE` 0, `RNS_DIST_CACHE_FIT` 1, `DIST_PW_FUSE` 1, `DIST_TWREC_G` 1, `DIST_LOGR_DELTA` 0, `DIST_SYM_SLABS` 1, `RNS_AUTO_PIECE_COST` 1, `RNS_STRATEGY_GRID` 1, `DIST_RB_WAIT` 1, `COMM_ALLTOALLV_DEPTH` 2, `MN_P24` 2 (18-digit mn path stays as fallback when pool 0 has fewer than four planes). The plan-cache line printed `MN_P24=0` when unset; now 2.
- **B4:** `comm_tcp.c`, `COMM_HOSTS`, `COMM_PORT`, `COMM_TRANSPORT=tcp` removed; `mn.c` refuses `COMM_SIZE > 1` without `COMM_TRANSPORT=shmem` (mnrun.sh sets it); a non-SHMEM build runs one process only; `wp6run.sh`, `tests/lay_host` removed.
- **D1e:** `COMM_LAYER_INTER2`, `COMM_LAYER_VSLOT_POOL`, `COMM_LAYER_VSLOT_SHARE`, `COMM_LAYER_VSLOT_RELEASE`, `RNS_DIST_CACHE_PARTIAL`, `MN_OUT_DKM_HI` default to 1 with `=0` fallbacks (INTER2 and RELEASE follow VSLOT_POOL when unset); keys left the launch lines and profiles. `MN_PLAN_ONLY` identical at 3.71e13:576, 6.441e10:2, 1.2882e11:2 (m).

Open: after the final rebase B2..D1e were only build-checked; single 2-node totals are single runs, only B3 had a paired timing (null); aac6 multi-process runs need OSHMEM; old experiment drivers (`pc_j*`, `ofi17*`, `std17*`) set removed keys (ignored).

## §L2 — validation of the code reduction (B02) at full size and for speed, aac6 (2026-10-11)

New = main a1569d5 (B02: -36 switches, -1354 core lines) vs old = tag `archive/pre-reduction-2026-10-11` (bc80001); both have K2+K3, so the comparison isolates the reduction. One aac6 node (4 APUs), plain launch line.

- **Correctness at 1e11 (m):** `./ecalc 100000000000` rc 0, total 129.3 s, VmHWM 27.5 GB, **VERIFY OK**; digits identical to the 1e11 reference (`digcmp.sh`), which also covers the first 4e10 digits (no separate 4e10 run).
- **Speed, paired ABBA at 64 410 000 000 digits, 8 rounds, 32 runs, all VERIFY OK (m):** mean total new 73.11 s, old 73.29 s (n = 16 each, sd 0.78 s and 1.24 s); **new - old = -0.18 s, 95 % CI [-0.88, +0.52], t = -0.62**; stop rule: round cap 8.

**Verdict.** Digit-safe at 1e11 and speed-neutral at 6.441e10 on one node (CI excludes anything beyond about 0.9 s, 1.2 % of the run). Not covered: multi-node, a separate 4e10 run.

## §O8 — TASKS backlog clean-up (local only, no cluster use; 2026-10-11, branch o8 off main 0298fe2)

1. **X2 cut-group pool-offset asymmetry (TASKS #4, from `results/GRID_GROUPS.md §S45`; host emulation, cxi not run).** `COMM_LAYER_INTER2=1` needs `COMM_LAYER_VSLOT_POOL=1` (v-slots carved from the symmetric comm pool); a symmetric block is valid only if every member of the top group made the same allocation sequence. In a cut last group (g not divisible by the group size, e.g. 6,6,3): OFI pool with `SHARE=1` is symmetric (same slot size), with `SHARE=0` asymmetric (15 nodes, groups 6,6,3: 288 mismatches, m emulated); SHMEM single pool is asymmetric for SHARE 0 and 1 (39, 150, 15 nodes; control size emulated, a). Without X2 nothing is affected. **Consequence:** latent hazard for non-dividing group schedules, not a corruption path on the target's 576-node schedule (no cut group; S45 mod). X2 is now on the target launch line (b01, 2f2675c), so the S45 sentence "X2 is not on the launch line" is out of date. Re-check before any uneven-group schedule (TASKS 31: 572-576 nodes; TASKS 34); an offset-equality allreduce at slot creation would be a cheap guard, not built.
2. **K2/K3 in the time model (TASKS #5).** Fixed on the way: `ecalc/mn_model.py estimate --target` crashed (`NameError: prof`); fix `a.k_other = prof.k_other` in `main()`. Model change: `mn_model.K23_S = 2.03` s per node (env `MN_MODEL_K23_S`, flag `--k23-s`, 0 = old), applied after `mn_model.run()` so `--calib*` is unchanged. Basis: S51 ABBA at 1 node, 6.441e10 digits: -2.03 +- 1.38 s (aac7), -1.22 +- 0.84 s (aac6) (m); carrying it to 576 nodes is **a**. At 4 nodes the kernel metric is -8.4 s (K2 -2.0, K3 -6.4) but run totals not significant (-6.0 +- 21.9 s): not used.

   | digits (576 nodes, `mn_model.py estimate --target`, mod) | no write before / after | write @0.6 GB/s before / after |
   |---|---|---|
   | 3.71e13 | 181.7 / 179.7 s | 216.5 / 214.4 s |
   | 3.60e13 | 179.1 / 177.0 s | 212.5 / 210.5 s |

   Saving -2.0 s (about -1.1 %), mod from a measured 1-node figure. Not modelled: T2048, RELEASE, single-level `[6]` schedule (item 34), DIST_COALESCE (item 15). The 2/4/6-node scaling study stays "later".
3. **Branches (TASKS #8; proposal only, nothing deleted).** Tags `archive/s24`, `s25`, `s37`, `s50-gather-tile` kept; s35 fully on main (remove worktree and branch); s50 (DIST_GATHER_TILE, null at all scales, do not adopt) branch deletable after user agrees; remote `claude/confident-mayer-yx23pt` ancestor of main (delete); c07, t12 unmerged (leave); h1 at main.
4. **Stale rows.** #3 (S39 candidates) all built and measured: k_addsub2 -> K2 `DBIG_ADDSUB3`, k_modq -> K3 `DBIG_MODQ_MONT` (both default), k_crt_batch -> K4 `RNS_CRT_SUB` (no gain); closed. #11/#19/#30/#32 (4-node bimodality): A13 (4 nodes, aac7): NIC MCU counter group A (oxe_mcu_meas 18-25) 10.2 vs group B (28-33) 12.1 GB/s, group drawn per launch; A14 (2 nodes, 86 launches): p(A) about 0.5 but the 2-node rate is the same in both groups (6.59 vs 6.59 GB/s); cause of the draw unknown, not arm/node/rank/order; a pre-run probe has no predictive value (C04 corr -0.06 / -0.20). #20/#14 superseded by `tools/initchk.py` (A03/A06); #38 closed.

## §H1 — PLAN.md 28.4 measurement, single-node part (decimal vs binary limbs, no code changed)

2026-10-11, commit 0298fe2, one aac6 node (4 APUs); each test run with `LIMB_BASE=2` and `LIMB_BASE=10`; single runs (pass/fail and wall time only, not timing claims; m).

| test | base 2 | base 10 | class |
|---|---|---|---|
| t_ntt 24 | OK (2360) 8.6 s | OK 10.5 s | pass both |
| t_mul 20 | OK (185) 254 s | **no result in 900 s**; with a throwaway patch skipping the 6 largest batch cases: OK (181) 783 s | (c) oracle cost |
| t_bs | OK (10) 23.8 s | OK 25.7 s (first run failed only for missing SHA-256 reference files in a fresh clone) | pass both |
| t_dbig 0 | OK (555) 17.4 s | OK 19.0 s | pass both |
| t_newton 20 | OK (620) 26.6 s | OK 83.4 s | pass both |
| t_crt | OK (12) 30.1 s | OK 12.2 s | pass both |
| t_verify | OK (334) 2.4 s | OK 3.0 s | pass both |
| t_out, t_patch | OK 11.3 s, 175.5 s | OK 11.5 s, 176.9 s | base-independent |
| t_mn_grid (2 procs, 1 node) | OK (180) 158.0 s | OK 143.9 s | pass both |
| t_dec | OK (26) 27.9 s | FATAL rc 3 ("bi_shl by bits ... not valid in the decimal base") | (a) base-specific by construction |
| ecalc 10^9, size 1 (e9) | identical, 20.83 s | identical, 11.40 s | pass both |

**t_mul decimal stall (c).** Section 4 (3000 x 2^17, tiles) made no progress for 900 s: the verification loop converts every operand and product through the GMP decimal-string bridge. Inferred from code and progress, not profiled. The six skipped cases (3000 x 2^17, 2^22 max, 2^20 CPU CRT, 2^17, 2^18, 2^14 x 40000) were not verified in decimal by this run; e9 exercises large decimal products indirectly.

**Verdict.** No real disagreement (class b). One class (a) (t_dec), one class (c) (t_mul checks too slow). Decimal ecalc is faster (11.4 vs 20.8 s at 10^9); decimal `t_newton` 3.1x slower in the test (GMP-bridge cost). Open: a faster decimal oracle for t_mul (done: §TREF, §H4 7.2). Branch `h1`, nothing merged.

## §H2 — PLAN 28.4, multi-node part: t_mn_grid in decimal vs binary limbs on aac7 (2026-10-11)

Measurement only. Build 0298fe2 (SHMEM). `COMM_TRANSPORT=shmem LIMB_BASE=<10|2> ./tests/t_mn_grid 1 28` under `mnrun.sh <p>`, p = 2..6, one 6-node hold (queue item 98_h2_mngrid). QOS `normal` allows 6 nodes per user (`sacctmgr`, m), so 6 is the maximum used.

Result (m): every run passes in both bases at 2, 3, 4, 5, 6 processes (including the non-power-of-two general map); p = 2 base 10: 204 checks, 0 failed. Wall times (s, single runs incl. launch), base 10 / base 2: p2 109 / 113, p3 105 / 110, p4 157 / 170, p5 156 / 169, p6 159 / 168; decimal equal or slightly faster. Not repeated: mnaccept `mn`, `ckpt` steps (one node, default decimal) and e9 (both bases run elsewhere). Driver `tools/h2_mn_grid_bases.sh`.

## §H4 — PLAN 28.5 on branch `h4`: the binary limb pipeline archived, decimal limbs only

Branch `h4` off main 0298fe2 (the user decides the merge after measured results). Readiness plan `archive:results/H3.md`.

**Change (m, `git diff --numstat`).**
- Core `ecalc/*.c|h` -418 / +158 (17 files, net -260); `todec.c/.h` moved out (-345); core total about **-605 net** (H3 estimated -520..-650). Tests -191 / +117 (net -74), `t_dec.c` moved out (-86): about -160 net. `newton.c` -40 (binary Knuth D tail only; not removed otherwise: `NEWTON_DEVICE=0` and the seeds used it, H3 §1).
- Removed: binary arms of bigint ops, `bi_shl/bi_shr` by bits (`bi_shl_limbs/bi_shr_limbs` remain), `bi_decimal`, `bi_set_decimal`, `bi_env_base`; binary T1/dc/T2 digit path in `ecalc.c`, `pow10_big`; the binary CRT window and spill ripple in `crt.c`; `ec_np_check` / `ec_np_prod` lose their `decimal` argument. Kernels' `bool decimal` arguments kept (constant 1 at every launch, GPU code unchanged; H3 step 6 left).
- **`LIMB_BASE=2` fails loudly:** `bi_check_base()` (called by `ecalc`, `rns_init`, every test) ends the program with "only decimal limbs (10) exist since PLAN 28.5; the binary 2^64 pipeline is archived (archive:binary_pipeline/, git tag pre-28.5-binary)", rc 3. The registry keeps `LIMB_BASE` as that guard.
- Archived: `todec.c/.h`, `t_dec.c`, `t_mul_base2.c` (copy of `t_mul.c` at 0298fe2) in `archive:binary_pipeline/`; annotated tag **`pre-28.5-binary` = 0298fe2** pushed to GitHub (`git checkout -b binary pre-28.5-binary` rebuilds the comparison).
- Tests are decimal only with GMP oracle through `harness.h`: `t_mul` exact to 2^20 limbs (was 2^26), above it residues mod 2^61-1, 10^18+3 and 4611686018427387847 plus length; `t_newton` exact to Q of 2^22 limbs (was 2^26), above it residues, 0 <= R < Q, corrections <= 4; `t_crt` part 3 (2^30 throughput) kept in decimal; `t_mn_grid` oracle host `rns_mul`; `mnaccept` e9 decimal only.

**Compile checks and CPU tests (m).** aac6 and aac7 login builds rc 0 (aac7: no warnings, `-DCOMM_SHMEM`); `ECALC_NP=3` and `4 ./tests/t_crt 0` VERIFY OK (12 checks each); `LIMB_BASE=2 ./ecalc 1000` and `./tests/t_crt 0` fail with the FATAL message, rc 3 on both clusters. Two compile slips fixed (dcfbc9b, d12fee1). Makefile bug found in review R1 (stray recipe line recompiled `crt.o` as HIP without `-fopenmp`), fixed in 019e652 (-1 line); builds before it are not like-for-like for timing.

**Validation results (all m).**
- aac7 item 50 (build with the crt.o defect): gate 2 n PASS (21.09 s); `t_mn_grid` 2/3/6 nodes PASS (103/109/157 s); ABBA 4e10, 5 pairs: h4 50.61 s, main 49.00 s, +1.61 s, CI [-2.87, +6.09], p 0.376. Superseded by item 51.
- **aac7 item 51, ABBA of record (fixed build 019e652, 2 nodes, 4e10, 5 pairs, all VERIFY OK):** h4 50.63 s, main 50.54 s; **h4 - main +0.09 s, 95 % CI [-2.08, +2.27], p 0.91**, t 0.12, sd 1.75, n = 5. No measurable change (binary code inert on the decimal path).

**H6: the aac6 single-node timeouts (2026-10-12).**
- Diagnosis: on aac6 `t_mul 20` hit its 1500 s timeout (SPX) and 700 s (CPX), `t_newton 26` device was killed by the 45-min hold end (SPX) and 700 s (CPX), `t_mn_grid` 2 procs ran out of hold or memory (CPX XCD has 22.9 GB, rc 137). `t_mul`: each batch product went through the GMP decimal bridge (same cost as H1's stall). `t_newton` device: no hang, lines kept coming; h4 runs section 3 to 2^26 by residues where main in decimal stopped at 2^22.
- Fix (commit 0084cfc, `t_mul.c` only): batch groups above 2^24 operand + product limbs check products 0, 1, N/2, N-1 exactly against `mpz_mul` and every product by residues mod 2^61-1, 10^18+3, 4611686018427387847 (about 2^184) with every limb < 10^18; groups up to 2^24 limbs stay exact. A wrong product passes only if its error is a multiple of the 2^184 modulus (a carry or limb error e B^j is not, none of the primes divides 10).
- **Results (m, all VERIFY OK; H7 on four CPX nodes 05:06-05:21 ET and H6 on SPX, marker `SUCCESS pass=5 fail=0`):** t_mul 20 h4 **883 s SPX (201 checks) / 896 s CPX**; t_newton 26 host 539 s SPX / 513 s CPX (708 checks); t_newton 26 device (`NEWTON_DEVICE=1`) h4 829 s SPX / 803 s CPX (1186 checks) vs main (`LIMB_BASE=10`) 758 s SPX / 731 s CPX (1136 checks): **+71 s (+9.4 %) SPX, +72 s (+9.8 %) CPX = the 2^26 residue point** (+50 checks); t_mn_grid 1 28, 2 node-processes, 147 s SPX. Section marks track each other with a constant +70..75 s offset from section 3 on, no log gap. At 2^26 limbs: device divmod 0.77 s (recip 0.39), host 1.73 s (device 2.2x faster); t_mul VmHWM 90.1 GB, batch rate 2.08 Gpoint/s.
- Conclusion (m): the timeouts were test cost, not hangs or defects. `mnaccept.sh` unit step limit for `t_mul 20` raised to 1800 s (1.9x margin; 1200 s left 1.34x on CPX); new drivers should use 1800 s for t_mul 20 and 1500 s for `t_newton 26` device (1000 s host); `accept.sh` has no timeout.

**Open.** The user's merge decision (binary vs decimal only; H5 below is the evidence); H3 step 6 (kernel `decimal` arguments) left for a separate timed change. aac6 `ckpt e8 size 2` FAIL seen in an H4 phase C driver ("3 nodes exited; restart rc 0; identical") was not investigated there.

## §H5 — decimal (LIMB_BASE=10) vs binary (LIMB_BASE=2) limb pipeline, paired wall clock

**Question.** How much slower is the binary limb pipeline? Prior evidence: single unpaired 1e9 runs, 11.4 s decimal vs 20.8 s binary (m).

**Design.** aac7 queue item 52 (`tools/h5_item.sh`, 2 nodes, runs 1 node): the `h4main` worktree (main before binary removal, both bases in one binary, SHMEM build), launch line `LINE` of `e16_headline.sh`, no digit write, every run VERIFY OK; ABBA order; 1e9 >= 4 pairs stopping at p < 0.05 or CI half-width <= 0.5 s (cap 8); untimed digit-identity run per base against the 1e9 reference; 1e10 >= 3 pairs (cap 5) within the budget. Stub dry run checked the statistics against python.

**Results (m; item 52, 2026-10-12, marker SUCCESS, pass 4, fail 0). Binary minus decimal, paired:**

| size | pairs | decimal mean | binary mean | binary - decimal, 95 % CI | p | ratio |
|---|---|---|---|---|---|---|
| 1e9 | 4 | 12.73 s | 24.16 s | **+11.43 s**, [+8.57, +14.29] | 0.00105 | 1.898 |
| 1e10 | 5 | 26.06 s | 58.54 s | **+32.47 s**, [+30.37, +34.58] | 1.8e-06 | 2.246 |

Without the cold-start pair: 1e9 +12.26 s [+10.24, +14.28] (3 pairs, p 0.0015); 1e10 +32.37 s [+29.28, +35.45] (4 pairs, p 5.9e-05); per-pair differences at 1e10: +32.91, +30.01, +33.95, +33.96, +31.54 s. Digits identical to the reference in both bases at 1e9.

**Verdict.** The binary pipeline is correct but **1.9x (1e9) to 2.2x (1e10) slower**; the gap grows with size (not startup). No reason to adopt binary limbs; the user decides. Branch `h5`, not merged.

## §TREF — a test reference product, and the host product tier removed

**Question.** Build the reference designed in §SH3 so the host product tier (`rns_mul`: mdev, Karatsuba/chunked split, CPU schoolbook, `rns_mul_low`, `rns_mul_pair`; no production caller) can go; prove the reference first (it must catch injected faults and agree with the old reference on every `t_mul` case), then move the tests and remove the tier. No switch, default or digit changed.

**Change / branch.** Branch `tref` (from main b80ee2e): d465b08 + 06978b9 (reference and `t_mul` proof mode, against the old tier), 14eb7c6 (removal, tests moved), then docs. Files: `ecalc/tests/tref.h` (new), `harness.h`, `t_mul.c`, `t_dbig.c`, `t_newton.c`, `t_mn_grid.c`, `t_primes.c`; `rns_mul.c/h`, `rns_dist.c`, `ecalc.c` (three `rns_free_scratch()` calls dropped).

**The reference (`ecalc/tests/tref.h`).**
- Exact up to 2^21 product limbs: GMP, with decimal-limb conversions as a product tree (parallel halves). SH3 assumed the old 18-digit string bridge was quadratic above 2^20 limbs; it is not (GMP's `mpz_set_str`/`mpz_get_str` are subquadratic): single-threaded tree and string form take the same time (2^20 limbs: 2.2 s in, 4.5 s out on a 4-core workstation, m); the tree's gain is the parallel halves (1.5x on 4 cores, m). The string forms stay as its check (`t_mul` §0).
- Residue form, any size: (1) every limb < B and the normalised length na + nb or na + nb - 1; (2) c = a b modulo three 62-bit primes (`t1_q[0..2]`, `vf_limbs_mods`, O(n), parallel); (3) the low 64 limbs exactly; (4) the top limbs: floor(c / B^(ka+kb)) in [Ah Bh, Ah Bh + Ah + Bh] (exact bounds). One wrong limb changes c by d B^k, 0 < |d| < B, never a multiple of a prime > B, so (2) alone catches it.
- `tref_mul` (a reference value for `t_newton`, `t_mn_grid`, `t_dbig`): GMP up to 2^21 limbs, else the one-node device product (`rns_mul_dist_db`) at the default plane (the tests' grids run under `DIST_LOGN_TEST`, so a different plane size), checked by the residue form. `tref_faults` tests the reference itself.

**Method.** aac7 queue items (2 nodes): proof with the old tier present (`t_mul 20 tref`: `tref_mul` value == `rns_mul` value, old and new checks accept both, one-limb faults at low end, middle and top must fail both); before/after `t_mul 20` on one node at main b80ee2e and the branch; new tests (`t_mul 20`, `t_dbig 24 prod`, `t_primes gpu 26`); gates (`tools/gate.sh --build --nodes 2`, `--nodes 1`, `mnaccept --only unit`).

**Results (m).**

| item | result |
|---|---|
| proof run 1 (sections 1, low products, 2 up to 2^24 x 2^24) | 72 products: `tref_mul` == `rns_mul` on all 72; 0 VERIFY failures; every injected fault caught (new check, residue form, old check). OOM-killed at 2^26 x 2^26 (one process holding both tiers' staging and pools; VmHWM old `t_mul` alone 114.6 GB, new 128.1 GB) |
| proof run 2 (sections 2 + pair) | 17 products (11 by value to 2^25 product limbs, 6 by cross-acceptance at 2^26..2^30 and pair 2^27+5): 11 of 11 agree; new check accepts all 17; 51 faults injected: 0 missed by new check, 0 by residue form (9 small cases), 0 by old check; VERIFY OK (150 checks), 330 s |
| `t_mul 20` before (main b80ee2e) | 737 s, VERIFY OK (169 checks) |
| `t_mul 20` after | **187 s** (-550 s, -75 %, 3.9x faster), VERIFY OK (260 checks); 23 fault sets x 3 injected, 0 missed |
| `t_dbig 24 prod` (products to 2^27) | 67 s, VERIFY OK (565 checks) |
| `t_primes gpu 26` | 45 s, VERIFY OK (89 checks) |
| gate `--nodes 2` / `--nodes 1` | PASS / PASS (10^9 identical; ecalc 21.84 s at 2 nodes with P24, 9.76 s at 1 node) |
| `mnaccept --only unit` | 9 passed, 0 failed (t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20 (2756 checks), t_verify, t_out, t_patch, t_mn_grid 2 procs (244 checks)) |

Reference cost in `t_mul` §2: exact 2^20 x 2^20 8.2 s; residue form 2^24 x 2^24 0.07 s, 2^30 x 2^30 2.7 s; device product at 2^30 x 2^30 2.9 s (m).

**Lines (m, `git diff --numstat b80ee2e..tref`).** Production code -404 / +10 (`rns_mul.c` -341, `rns_mul.h` -32 / +5, `rns_dist.c` -27, `ecalc.c` -4 / +2); tests +393 / -148 (`tref.h` +148, `harness.h` +93 / -6, `t_mul.c` +102 / -105, four other tests +49 / -37); docs +10 / -1. Net -141.

Removed (pre-change files in `archive:removed_2026-10-13/tref/ecalc/`): `rns_mul`, `rns_mul_low`, `rns_mul_mdev`, `rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`, `mul_rec`, `rns_free_scratch`, `mdev_gpu_crt`, `mdev_pts`, `node_copy`, `par_copy`, `par_zero`, `x_inv_pw`, `rns_school_max`, `rns_crt_threads`, `rns_mdev_gpucrt`, `rns_mul_dist` (host bigints), `acc_flat` and its staging. `t_mul` sections 1-3 (host-tier tests, args `big`, `big32`) replaced by device-path sections; `t_primes` tier 0 is now the device path at half the plane.

**Verdict.** The reference catches every injected fault (0 missed, both forms) and agrees with the old reference on every case (83 products by value, 6 large by cross-acceptance); the host tier is removed; merged to main.

**Open.** `struct acc`'s `flat` branches in `rns_dist.c` are dead (about 25 lines, kernel code, not touched); `rns_stats` keeps host-tier fields (always 0) and `ecalc.c` prints `n_mdev` / `n_split` in three summary lines (dropping them changes output lines; left to the coordinator). The tests lost the host tier as an independent code path; replacement independence is the residue form and the device path at two plane sizes (`t_mul` §1, §2; `t_primes`; `t_dbig`).
