# CODE_TOOLS — Code reduction, tools and profiles, sessions

Scope: the code-reduction audits, the tools and profile system, the session record. Current conclusions and live numbers (labeled m measured / mod modeled / a assumed) condensed from the former per-topic files; the removed text of each is unchanged in `archive:cleanup_1103/results/<OLD>.md`. Every former section keeps its `§ID` heading.

## CODE_REDUCTION — shrinking the code

Scope: audits, shrink passes, settings-removal waves, validation of the reduction, the limb-format study (decimal vs binary) and the host product tier removal. Old item IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`; raw data and drivers are in `archive:results_raw_2026-10-19/`. Labels: m measured, mod modeled, a assumed.

### Topic summary

- **What this is.** The 2026-10-11..13 campaign to make ecalc smaller without changing a digit: two read-only audits (AUD2 dead code, AUD3 structure), four removal merges (SHR2, SH3, SIMP, TREF), two settings-removal waves (W2A, W2B), a speed/size validation (L2, removed 2026-10-25), a backlog clean-up (O8, removed), the fourth cleanup round (CR4) and the decimal-vs-binary limb study (H1, H2, H4, H5).
- **Current state (all merged to main unless noted).**
  - Settings: 269 registry rows before wave 2 lane B, 185 after lane A, 170 after SHR2, 145 after CR4 (m); W2A removed 42 switches + 1 value, W2B 42 rows, SHR2 16.
  - Code: SHR2 -1531 lines, SH3 -261, SIMP -418 (ecalc sources), TREF -404 production, W2A about -1800 (removed minus added about -1500). Every step gated: 1 and 2 nodes, digits identical.
  - Single-node flow is now the device DKM division at every size (SIMP): 10^10 digits on 1 node 8.3 s faster (m). The 1-node 10^9 gate now runs the production division.
  - Only the top-level checkpoint set (`ECALC_CKPT_TOP`) remains; no restart inside a run (D11).
  - TCP transport, SOS/OSHMEM build branches, spill primitive, 3·2^k distributed planes, `B4` strategy form, host product tier (`rns_mul` etc.) are gone. `ECALC_OUT_PACKED` default is 2 (60-bit packed, D5).
  - Limbs are decimal only (H4, branch `h4`, tag `pre-28.5-binary`): `LIMB_BASE` other than 10 fails loudly. Evidence: binary is 1.9x (10^9) to 2.2x (10^10) slower (H5, m). Check ecalc/README.md for the current `LIMB_BASE` row; if this summary and main disagree, main wins.
- **Live conclusions.** The reduction was digit-safe at 10^11 and speed-neutral at 6.441e10 (L2: 1e11 identical, new - old -0.18 s [-0.88, +0.52], 16 + 16 runs, m); removed paths were off at the defaults, so no timing was needed except SIMP, B3 (null) and H4/H5.
- **Still open (not done).** `k_store`/`k_copy_h2d` partly merged (SH3); AUD3 items 5-10 (size 1 through the sharded driver, P24 only, `tree_level` fold, one writer, one `mdb_add_shifted`, one transform cache); AUD3 decisions 11-13 (11 and 12 done as D5/D11; 13, the SHMEM data path: removed 2026-10-25 with the plain SHMEM transport, Q103, §CR4); `LIMB_BASE` guard row. X2 + cut last group symmetric-offset hazard (the last group, when cut, makes a different pool allocation sequence under `COMM_LAYER_VSLOT_POOL=1` with two exchanges in flight; text in `archive:results_merged_2026-10-25/CODE_REDUCTION_removed.md §O8`) is documented, no guard built.
- **Superseded.** AUD2 items 1, 4, 5 (part), 6, 8 (part) by SHR2/SH3; AUD3 items 1-4 by SIMP; the host tier open issue of SH3/SHR2 by TREF; H1's t_mul decimal stall by TREF's residue reference and H4 §7.2.

### §AUD3 — where ecalc's algorithms and code structure can be simpler and smaller (audit, 2026-10-12; open items only)

**Open items** (lines net removed, mod time/memory):
- **5** Size 1 through the sharded driver: about -310 net; memory is the risk (one-node peak laid out for the dbig flow; `mdb_shift` at g = 1 copies, 29 GB per copy of Q at 6.44e10); needs 1-node ABBA and `mem_report`.
- **6** P24 as the only multi-node product form: about -50 (partly overtaken by D18: P24 is the multi-node default); +4 GiB per APU in pool 0 at 2-16 nodes.
- **7** `tree_level` into `tree_level_k`: moot, `mn.c` has the single `tree_level` (no `tree_level_k` in the tree). **8** one output-writer machinery: partly done (the dead 8-byte packed write path, one share-source helper `mn_out_share_src`, one `x_bg` output init; about -2 net); what is left is the size-1 (`x_bg`, `x_hi`) versus multi-node (`mn_out_early`) writer flows, which are different thread and gate structures and cannot be merged without GPU gates. **9** `mdb_add_shifted`: already one body (rns_dist.c); folding it into `mdb_shift_gt` would change its round and pool pattern (memory, timing), not done. **10** one transform cache: done as dead-code removal (t10, 2026-11-08): the held full-slot path (`cache_avail`, `pl` planes, budget fit, `dist_core` slot planes) never allocated at any setting (the single-node tier has 0 slots, the mn tier takes pool partial slots); the mn partial machinery is now the only cache code. Giving `mul_grid` partial slots is a behavior change (new 1-node pool use), not done; needs an ABBA first.
- **13** (user decision) drop the SHMEM data path of device exchanges (about -120): kept until the target's A3 stage has run.
- Smaller: `mn_group`'s transform-node subset (`gt`, `tr`) is a leftover of `MN_TOPO_GROUP`; `mn_prod_cut_x1` never triggers at target sizes; two start-up self-tests (`mn_selftest`, `mn_selftest_layered`) overlap.

### §SIMP — AUD3 items 1-4: one single-node division flow, DKM only, no 3·2^k distributed planes, one grid planner

**Results (m).**
- Plan sweep (`MN_PLAN_ONLY`, `BS_LAYOUT_ONLY`, 119 points x 2 = 238 files, five settings): items 3+4 vs main d1d19bf and the merged tip vs main 89318ae: **0 differ** in 5 x 238 files.
- Merge gates on 62064f2 (aac7): gate 2 nodes P24 forced PASS (23.81 s), gate 1 node PASS (10.80 s); mnaccept unit (`t_newton 20`: 1648 checks OK), e9, mn, corr: all PASS, digits identical. The 1-node gate log now shows the `I3:` line and `divmod(dev, DKM)`: the gate runs the production division (coverage gap 1 closed). Final gate after rebase on main 68a9d69 (P24 default): gate 2 n 22.27 s, gate 1 n 10.63 s PASS; mnaccept unit,e9,grid,mn,corr 18 passed, 0 failed.
- ecalc 1 .. 10^5 digits (20 sizes) prefix-identical; `t_newton` quotient section 320 divisions identical to GMP.
- **Timing, 10^10 digits, 1 node, ABBA 4 rounds: mean A (main) 26.85 s, B (SIMP) 18.54 s; B - A = -8.31 s, 95 % CI [-9.68, -6.93], t = -19.2.** Phases: dm 5.5 -> 1.7 s, 10dP 0.9 -> 0.0, bs 8.5 -> 6.7, T1 0.0 -> 0.5. Above about 1.9e10 digits the flow was already the device flow (about 0, mod).
- One `t_newton` failure (1 of 1648 checks) after item 1: section 5 drew a different Q because section 3's RNG use changed; for that Q the reciprocal at nq 131075, k 65538 was -22 units from GMP's floor vs the +-8-unit bound. With section 5 on its own RNG (`rng5`) main and SIMP give identical errors, so SIMP does not change the reciprocal (pre-existing outlier of `recip_db2`).

**Verdict.** Adopted (merged): digits identical at every size tested, planner byte-identical, 10^10 at 1 node 8.3 s faster.

**Open.** `recip_db2` +-8-unit outlier (not investigated); `dm_layout` still sizes the size-1 division below 1.9e10 digits for the full-length set (larger than DKM needs; never short); `t_cap.c` keeps a mirror of the old 3·2^k caps; AUD3 items 5-10 not done.

### §W2A — wave 2, lane A of the user-approved settings removal (2026-10-12)

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

### §H4 — PLAN 28.5 on branch `h4`: the binary limb pipeline archived, decimal limbs only

**Open.** The user's merge decision (binary vs decimal only; H5 below is the evidence); H3 step 6 (kernel `decimal` arguments) left for a separate timed change. aac6 `ckpt e8 size 2` FAIL seen in an H4 phase C driver ("3 nodes exited; restart rc 0; identical") was not investigated there.

### §TREF — a test reference product, and the host product tier removed

**Results (m).**

**Verdict.** The reference catches every injected fault (0 missed, both forms) and agrees with the old reference on every case (83 products by value, 6 large by cross-acceptance); the host tier is removed; merged to main.

**Open.** The tests lost the host tier as an independent code path; replacement independence is the residue form and the device path at two plane sizes (`t_mul` §1, §2; `t_primes`; `t_dbig`).

## TOOLS_PROFILES — tools and the parameter system

Scope: tools and the parameter system: profiles, the switch registry, calibration, the target bring-up script (archived), mnaccept, environment probe. Condensed from the per-item reports (originals in `archive:results_merged_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`. Labels: m = measured, mod = modeled, a = assumed.

### Topic summary

What this topic is: the scripts and tables around the engine that make runs reproducible and cheap to calibrate: machine profiles (`profiles/*.conf`), a registry of every environment switch (`ecalc/switches.tsv`, generated by `tools/switch_registry.py`), a startup `config:` line, profile loading (`tools/profile_load.py`, `gate.sh --profile`; `tools/with_profile.sh` is gone, `profile_load.py NAME --run -- cmd` does its job), computed size values (`tools/plan_values.py`, `MNRUN_PRINT_ENV`), the log parser and rate-adjusted estimator (`ecalc/calib_table.py`, no longer in the tree), the environment probe (`archive:scripts_2026-10-23/envprobe.sh`), the model profile `aac7_s18`, the mnaccept fixes and the one-script target session (`target/bring-up script/target_kit.sh`).

Current state (cross-checked against ecalc/README.md and RESULTS.md; the README owns switch defaults):
- Switch registry: adopted. `switches.tsv` (266 rows when L5 landed; regenerate, do not hand-edit), `switches_gen.h` feeds the config line, `switch_registry.py --check` runs inside `gate.sh --build`. The review-date table of T27 was removed later (user decision, 2026-11); the unknown-name guard stays, launcher-only names exempt by prefix.
- Profiles: C01 templates exist (`profiles/aac7.conf`, `archive:scripts_2026-10-23/aac6.conf`, `target.conf.template`). O3's loader and validator were gated PASS on 2 nodes aac7 and 1 node aac6 (the branch was described as not merged at the time it was written; check `tools/profile_load.py` on main before relying on it). Precedence: explicit environment > profile > code default.
- Size-dependent launch values are computed, not typed: `plan_values.py` and `mnrun.sh` give pool, heap, `FI_UNIVERSE_SIZE`. The target runbook's typed pool and heap (1536 / 2048M) are stale under the X2 default (plan asks 2304 / 2816M, T27); the runbook change is the user's decision.
- Calibration: `total = a + b / r` (r = the run's own all-to-all rate) reduces the residual sd from 11.9 s to 1.6 s on 63 clean 4-node runs (B05/B07, mod on m). `calib_table.py` implements the parser, fit and rate-adjusted A/B. Caveat: where an arm changes the exchange itself, the rate covariate moves with the arm (T2048, X2 results are between raw and adjusted).
- Environment probe (C04): built; the r256 bracket correlation (-0.98, n=6) is a hypothesis, not proven. The queued 5-run test was not collected in this report; treat the probe as not adopted.
- `aac7_s18` model profile (S18M): off by default; `bw_apu` 4.25 GB/s per APU thread centers two same-digit 10-node walls (-3.1 %, +3.0 %); the 6.441e11 point stays +23.5 % high. `--target` output is unchanged.
- Target bring-up script (archived) (stages env, build, uffd, plan, edge, a3, a4, initchk, optional nodechk, s2chk). A06 added exclusion, spares, plan and uffd stages; S18KIT added ROCm candidate selection (7.2.4 first) and per-stage module loading; S18KITFIX fixed a3 pool sizing (now through `mnrun.sh`), per-n a4 logs and the edge map-rate line. a4 runs with `RNS_DIST_CACHE_FIT=1` (a switch since built in). After the removal of the plain SHMEM transport (Q103) the bring-up script's build and a3 stages still name `t_comm`, `COMM_TRANSPORT=shmem` and `COMM_OFI` (TASKS: bring-up script on hold).
- mnaccept.sh (A04): shared TMP on multi-node, reference-dir search, `refneed`, `--mem=0` on steps, `--skip-grid`; 10/10 passed on aac7 (1 and 2 nodes) and aac6.
- Open or superseded: B05's top-set restart, warm process and exchange benchmark ranks 5-7 were not built. B05 section 9 (`t_ntt 24` speed) is inconclusive. The S18KIT sweep of record is RESULTS §117 (the collision it caused is §118). Stale stand-ins: `MN_WAIT_STATS` exists in main (A06 correction).

### C01 -- machine profile templates (step one of the profile plan)

Agent c01, 2026-10-11, branch `c01` (from main bc80001). CPU only; read-only login-node commands. New files only, nothing reads them yet.
Built: `profiles/README.md` (format, precedence plan defaults < profile < overrides, evidence citation rule); `profiles/aac7.conf` (33 keys), `archive:scripts_2026-10-23/aac6.conf` (5 keys + commented CPX keys), `profiles/target.conf.template` (38 keys, 13 `__FILL__`, each with a "how to determine"); `tools/profile_check.sh` (read-only; sources a profile in `env -i`, compares to the e16 LINE and the aac7env exports, flags UNFILLED, UNKNOWN, REMOVAL-SLATED). A private draft of the target profile exists outside git.
Verdict: templates delivered with no behavior change. Next step was the loader (O3).

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/TOOLS_PROFILES.md`.)

### O3 -- P1 step 2: profile loading and validation

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/TOOLS_PROFILES.md`.)

### T27 — parameter system: computed size values and review dates for settings

Question: (A) can every setting that depends on digits or node count be computed in one place from (digits, nodes, profile), equal to today's hand values at tested sizes? (B) can every experimental, test-only or fallback switch carry an owner and review date, warning (not failing) when it passes?

Change (branch `t27`, merged; no ecalc C source changed, no digit can change):
- `ecalc/mnrun.sh`: `MNRUN_PRINT_ENV=1` (default 0) runs the whole launch computation (planner pool, heap by SHMEM implementation, `FI_UNIVERSE_SIZE`, libfabric defaults) and prints `export K=V` lines instead of calling srun; no allocation needed.
- `tools/plan_values.py [--profile P] [--line FILE] [--ecalc PATH] [--exports] DIGITS NODES`: asks `mnrun.sh` and the planner (`MN_PLAN_ONLY`); `--line` removes a typed line's size-dependent keys, recomputes and prints MATCH / DIFF / EXTRA / NO-CONSUMER (exit 1 on DIFF or `plan REFUSED`); `--exports` gives export lines for a launch without `mnrun.sh`.
- (removed later) `ecalc/switches_review.tsv`, the `owner` and `review` columns, the `WARN` lines and the README review table; the registry now has no review dates.

Part A, what was typed by hand: pool and heap on the target runbook (1536 / 2048M) and the private draft profile; `MN_GROUPS` at 576 (equals the code default schedule, so typing it changes nothing); one runbook key with no reader (NO-CONSUMER). Left as is: the bring-up script's a3 pool formula (a harness buffer size), the bring-up script's s2chk share 6.441e10 per node (to move to the 3.82e13 share in target preparation), aac6 CPX pool/plane-cap (machine-dependent, not size).

### B07 - `ecalc/calib_table.py` (step 1 of the calibration plan in archive:results_merged_2026-10-24/TOOLS_PROFILES.md §B05)

Task b07 (2026-10-11, base bc80001, branch b07). CPU only. 99 logs from aac7 (S47 48, S50 abba4_0024 16, S51 abba4 12 and abba4b 23) plus driver tsv files.
The tool: `ecalc/calib_table.py LOGDIR_OR_FILES... [--tsv F] [--fit [--oos]] [--ab [--exchange-exp EXP]] [--driver-tsv FILES] [--config REGEX=NAME] [--selftest]`. One row per log: label, exp/round/arm from the file name, nodes and digits (from `MN_PLAN_ONLY=`), total, init, bs, tree, recip, dm and sub-stages, dc, other, VmHWM, single-node chain, r (mean of the `comm_shmem ... all-to-all ... GB/s per APU thread` lines), mode (slow < 11.95, fast > 12.3 GB/s), flags (chain > 2.6 s, init >= 36 s, no VERIFY OK, no total), `clean`, per-phase memory, xstats/wait columns when those lines exist. `--fit`: OLS `stage = a + b / r` over clean runs per (nodes, digits/node, config), residual sd, optional leave-one-source-out. `--ab`: pairs with both arms clean; reports raw B - A, the rate-adjusted residual difference (CI slightly optimistic), an ANCOVA cross-check (quote this CI), per-stage values, the mean r of each arm with a WARNING when they differ (> 2 % or significant), a CAVEAT for exchange-changing experiments, and pairs needed for a target half-width (`--target-hw`, default 2 s).
Files: `ecalc/calib_table.py` (new, about 400 lines, stdlib), `ecalc/README.md` (one row). Open: `--table` output for `archive:scripts_2026-10-23/refit_s48.py` and the ABBA stop rule on the adjusted CI not wired; `config` is `-` for all current logs (drivers should echo an `env:` line, else pass `--config`); xstats/wait columns not validated against T09 logs.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/TOOLS_PROFILES.md`.)

## SESSIONS — aac7 measurement sessions of September/October

Owner of the items below (condensed 2026-10-19; full originals in `archive:results_merged_2026-10-19/`, raw data and drivers in `archive:results_raw_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Labels: (m) measured, (d) derived by arithmetic from measured numbers, (i) inferred, (mod) modeled, (a) assumed. Times Eastern.

### Topic summary

These are the aac7 10-node (6.441e10 digits per node, no write) and one/two-node sessions of 2026-10-07/09 that decided four code items and measured where the 10-node wall goes. Results that still hold:

- **`MN_T_CHUNK_MB=2048` (D2, S22) is faster on aac7**: B - A = -40.6 s +- 11.1 (95 % CI), 16 of 16 rounds negative (pooled 24 rounds -42.3 s mean, 22 of 24 negative; mean total 436 -> 396 s, dm sd 23.2 -> 10.3 s); memory unchanged. The code default is 1024 MB (pool law, needed by the target); 2048 stays an aac7 profile-line choice; no target A/B was made.
- **Three prospective items were closed NO-GO on measurement**: S-2 division/reciprocal overlap (ceiling 25-34 s at 576 (mod), realistic 6-17 s, below the 30 s bar; the `ready` counter cannot separate skew from transfer), A37-R1 GPU seed (seed and mapping co-bind on aac7 by +0.55 s; at the target the seed ends at about 16 s against a 16.5 s init floor: gain 0-1 s (mod)), and the 3-pass 2^31 NTT (13/9/9 forward 90.6 ms (m) against today's 89.9 ms; gate was <= 80 ms; the fat 9-stage passes cost 3.2-3.6 ms per stage, not the modeled 2.5). A37-R5 batch pipeline "not worth it" here (later built anyway as `BS_BATCH_PIPE`, default 1 since D20; `results/COMPUTE.md §N3`).
- **Where the 10-node wall goes (S27)**: direct exchanges cost 101 s per thread of 376 s (27 %), of which 61 % is skew (waiting for a slower node), not transport; transport variants (rot order, NIC rebinding, OFI-native signal) are dead ends (rot null on wall: +3.1 s, CI -1.0..+7.2). The one large measured, structural item is node 1 (a specific host) arriving about 20 s late at the reciprocal: S32 showed it follows the host (aac7-n01, stall in db_from_bi upload, 21-25 s), not the rank.
- **X2 (INTER2 + VSLOT_POOL, S31)** is -32 s (-8 %, clean rounds; -34 s all rounds, CI to +1.5) at 10 nodes with +0.8 GB (m) after the pool fragmentation fix; both are now built-in defaults (ecalc/README.md, since 2026-10-12). Its 576-node effect is unmeasured.
- **A rare crash exists** (segfault in libamdhip64 from the VMM background mapper, about 3.5 % of 10-node runs on 2026-10-08, 0.14 % per 2-node run (m)); `ECALC_VMM_SAFE=2` neither cured nor worsened it: not adopted (follow-up in `results/CORRECTNESS.md`).
- **Scaling data**: share ladder at 10 nodes: 410.6 s at 6.441e10, 464.4 s at 7.00e10, 528.5 s at 7.64e10 (time exponent about 1.5-1.9, above n log n; unexplained); memory linear at 28.4 GB per 1e10 digits per node (fixed about 173 GB on rank 0); the 8.1e10 share is stopped by the vslot rule (427.6 GB needed against 0.95 x the smallest MemAvailable 440.7 GB).
- **Superseded**: S22 §2 absolute wait levels (extra instruments cost 3-4 %); the S22 "S-2 GO" of S20 (flipped to NO-GO); S27's rank 3 X2 candidate (now adopted) and rank 1 (reciprocal chain lateness, diagnosed as a host defect, not a code item).

The per-item sections (tables, drivers, run lists) were moved to `archive:repo_cl6_2026-10-25/results/SESSIONS.md` (2026-10-25); section anchors that cited them now point to this summary.
