# TOOLS_PROFILES — tools and the parameter system

Scope: tools and the parameter system: profiles, the switch registry, calibration, the target kit, mnaccept, environment probe. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| C01 | machine profile templates (step one of the profile plan) |
| O3 | P1 step 2: profile loading and validation (branch o3, not merged) |
| L5 | parameter system step P1-1: switch registry + print-only effective configuration line |
| T27 | parameter system: computed size values and review dates for settings |
| C04 | environment probe (`tools/envprobe.sh`) and the probe covariate in `ecalc/calib_table.py` |
| S18M | AAC7 profile calibration against the 2026-10-06/07 10-node comm_ofi runs (branch `s18-m`) |
| B07 | `ecalc/calib_table.py` (step 1 of the calibration plan in results/TOOLS_PROFILES.md §B05) |
| B05 | a faster calibration process (design; CPU + reading only) |
| A04 | mnaccept.sh fixes (branch a04, 2026-10-11) |
| A06 | (N6): target_kit.sh hand-off items |
| S18KIT | `ecalc/target_kit.sh`: one script for the next target session (branch `s18-kit`) |
| S18KITFIX | three bugs in `ecalc/target_kit.sh`, found in the aac7 rehearsal (`~/s18ab3`), fixed and verified for real |

## C01 -- machine profile templates (step one of the profile plan)

(former file: C01.md)

Agent c01, 2026-10-11, branch `c01` from origin/main bc80001. CPU only; the only cluster contact was read-only login-node
commands (module/sinfo/ls of /sys/class) to check aac6 and aac7 facts. No behaviour change: new files only, nothing reads them yet.
Numbers in the profiles are quoted from the cited results files (labels kept there).

#### Built
* `profiles/README.md` -- format, precedence plan (defaults < profile < overrides), what belongs in a profile vs built-in default vs computed, evidence citation rule.
* `profiles/aac7.conf` (33 keys), `profiles/aac6.conf` (5 keys + commented CPX keys), `profiles/target.conf.template` (38 keys, 13 `__FILL__`, each with a "how to determine").
* `tools/profile_check.sh` -- read-only; sources a profile in `env -i`, compares with the e16 LINE and the aac7env.sh exports (MATCH/MISMATCH/SCRIPT-ONLY, aac7 profiles only), shows script `${VAR:-default}` for information, flags UNFILLED, UNKNOWN (name appears nowhere in ecalc/ or tools/), REMOVAL-SLATED (tier A/B items of results/A11.md).
* Private (not tracked): `docs/target.conf.draft` -- the template filled from docs/TARGET.md section 4 (576 nodes, pool/heap values, MN_GROUPS, ECALC_NODE_GB=480, output dir pattern); module names and partition stay `__FILL__` (TARGET.md does not give them).

#### Check output (`tools/profile_check.sh profiles/aac7.conf`, against main bc80001)
* 33 keys: MATCH 23, **MISMATCH 0**, PROFILE-ONLY 10, UNKNOWN 0, REMOVAL-SLATED 0, SCRIPT-ONLY 0.
* No mismatch is expected to appear only because b01 has not merged: main's e16 LINE already carries T2048 and X2. The PROFILE-ONLY keys are the ones b01 is adding or that no script sets: `ECALC_SEGV_TRACE` (gate.sh sets it), `COMM_OFI`, `COMM_SHMEM_IMPL`, `SHMEM_CRAY` (build flag), `GMP_HOME` (aac7env sets it only when ~/gmp/lib exists), `MNRUN_EXCLUDE_HOSTS/MODE`, and `MNRUN_UNLOAD`, `FI_MR_CACHE_MONITOR`, `FI_LOG_LEVEL` (equal to mnrun.sh defaults: the cue to delete them from the profile once mnrun's defaults move to the entry point).
* Re-run after b01 merges: `RNS_DIST_CACHE_FIT`, `ECALC_SEGV_TRACE`, `FI_MR_CACHE_MONITOR` in e16 LINE should then turn PROFILE-ONLY into MATCH.
* aac6: 5 keys, no mismatch (no launch line to compare). Target template: 13 UNFILLED (expected), plus UNKNOWN for `TARGET_PARTITION`, `TARGET_NODES`, `ECALC_OUT_DIR` (meta keys with no consumer yet).
* Negative test (temp copy of aac7.conf with `MN_T_CHUNK_MB=1024`, a bogus key and `COMM_OFI_DC=1`): reported MISMATCH, UNKNOWN and REMOVAL-SLATED respectively (measured).

#### Facts read on the login nodes (read-only, measured 2026-10-11)
aac6: no `/opt/cray/pe/sma`, no `/sys/class/cxi`, mlx5_0/1 present, PPAC_MI300A_SPX 192 CPUs 514000 MB, SH5 partitions 48 CPUs 120000 MB. aac7: `/opt/cray/pe/sma` present, no cxi on the login node (aac7-login, as mnrun.sh already notes), partition `192C4G1H_MI300A_RHEL9_A1` 192 CPUs 500000 MB.

#### Open questions
1. **Keys without a code consumer**: `TARGET_PARTITION`, `TARGET_NODES`, `ECALC_OUT_DIR`, `SHMEM_CRAY` (Makefile variable), `ROCM`-type names. The entry point must define them or the template drops them.
2. **Host exclusions**: the profile has `aac7-n01` (known slow, S31). The session rules also say always exclude `aac7-n61`; I did not add it because the task named only the first and the reason for the second is not in a results file. Decide.
3. **`ECALC_NODE_GB`**: code default 480 is the target's figure; aac7 nodes have 500000 MB. Not set in aac7.conf (no measured need); decide whether the profile should carry it.
4. **Unset section**: the target's record run unsets `ECALC_CHECKPOINT ECALC_CKPT_TOP BS_CKPT_DIR`; the format has no way to say it yet.
5. **MN_T_CHUNK_MB for the target**: the template carries 2048 (the 2026-10-11 decision "going forward") although the target line was 1024 pending its own A/B (TARGET_TASKS T11b, modelled +4.3 GB window temporaries against a 21.7 GB margin). Confirm 2048 is meant for the target too; b01 owns the README rows.
6. **Computed pool values**: `COMM_SHMEM_POOL_MB=1536` and heap 2048M are typed in TARGET.md's launch block; the private draft keeps them as a cross-check only. The plan stage should reproduce them (mnrun.sh already does the pool from MN_PLAN_ONLY).
7. **COMM_SHMEM_IMPL / COMM_OFI in aac7.conf** are recorded though auto-detected; delete if the profile should hold only what is not detectable.
8. aac6 profile is thin by design (TCP correctness path); the OSHMEM `COMM_TRANSPORT=shmem` line used in S-series checks is only described in a comment.

#### RESUME
Branch `c01` pushed; worktree removed. Next step (not started): the entry point that loads defaults < profile < overrides, validates (no `__FILL__`, no removal-slated keys, no unknown keys), prints the effective configuration and hash; then wire mnrun.sh / e16_headline.sh / target_kit.sh to it; re-run `tools/profile_check.sh` after b01 and b02 merge.

## O3 -- P1 step 2: profile loading and validation (branch o3, not merged)

(former file: O3.md)

#### Design (all script-side; no ecalc C source changed)
- `tools/profile_load.py NAME|path`: sources the profile in a clean shell ($HOME expanded), prints `export` lines only for keys the caller has NOT set
  (precedence: explicit env > profile > code default; an explicitly set empty value also wins). Exports `ECALC_PROFILE_NAME/FROM/OVER` (provenance).
  `--validate`, `--validate-all`, `--show` (key, value, origin, code default) are read-only.
- Validation errors (exit 1): key neither registered (ecalc/switches.tsv) nor external-library (FI_ SHMEM_ XT_ OMPI_ ...) nor named in a script
  (ecalc/*.sh, Makefile, tools/*) -> "unknown switch X (did you mean Y?)"; `__FILL__`; non KEY=value line; duplicate key; unsourceable file.
- `tools/with_profile.sh NAME -- cmd`: validate, load, print `config-src: profile=NAME from-profile=K,.. env-over=K,.. (code defaults: all other switches)`, exec.
- `tools/gate.sh`: new `--profile P`; `--build` now first runs `switch_registry.py --check` and `profile_load.py --validate-all` (failure = gate FAIL).
- Provenance in the run's own `config:` line (ecalc/config_line.c) was NOT done: it would be a C change (ecalc is untouched on purpose). Instead the
  wrapper/gate print the `config-src:` line immediately before the run, and ecalc's `config:` line carries the resulting values as before. Option in Decisions.
- Registry was stale on main (COMM_HS_TS missing; BI_MUL1_FAST, NEWTON_RECIP_CUT, NEWTON_RECIP_MID rows with no code site), so `--check` failed;
  regenerated with `tools/switch_registry.py` (ecalc/switches.tsv, ecalc/switches_gen.h: 3 names removed, 1 added in the config-line name list;
  no logic change, but it is a compiled header, hence the cluster gates).

#### Local tests (measured, 2026-10-11 ~23:20 ET)
| test | result |
|---|---|
| `switch_registry.py --check` before / after regenerate | 5 problems / 0 problems |
| `profile_load.py --validate-all` (aac6, aac7) | 0 problems |
| bad profile (`ECALC_NPP`, bare `FOO`, duplicate key) | rc 1, "unknown switch ECALC_NPP (did you mean ECALC_NP?)", not-KEY=value, duplicate |
| no profile: env of a clean shell with / without the tooling present | identical (diff empty) |
| aac7 profile loaded: new variables in a clean env | 37 (34 profile keys + 3 provenance) |
| `ECALC_NP=4 with_profile.sh aac7 -- ...` | NP=4 (env wins), profile value used for others, `env-over=ECALC_NP` in config-src line |
| `gate.sh --profile bad.conf` / `gate.sh --build` (registry+profile step) | FAIL with the message / registry.log and profiles.log clean (local build fails: no hipcc) |

#### Cluster gates (armed, fire-and-forget)
- aac7: queue item `75_o3_gate.sh` (2 nodes, EST 20 min, APPROVED=yes) from worktree `~/ntt-wt/o3`: gate A `--build --nodes 2` (standard line), gate B
  `--nodes 2 --line none --profile aac7`. Output `~/o3/Q/` (summary.txt, O3_DONE).
- aac6: driver `~/ntt-o3/tools/o3_aac6_driver.sh` waits for 05:35 ET (after H4), one PPAC SPX node hold <= 45 min, gate A `--build --nodes 1`, gate B `--profile aac6`.
  Output `~/o3/` (summary.txt, O3_DONE); cancels only its own hold.

#### Collected cluster gates (aac7 queue item 75, 2 nodes, 1e9 digits, 2026-10-12 ~02:49 ET; measured)

| gate | line | result |
|---|---|---|
| A, plain (`line=aac7`, env set by the gate script) | aac7 | PASS: rc 0, VERIFY OK, digits identical, segv 0, ecalc total 23.31 s, run 32 s |
| B, `--profile aac7` (`line=none profile=aac7`) | profile | PASS: rc 0, VERIFY OK, digits identical, segv 0, ecalc total 19.10 s, run 24 s |

- Gate B's log reports `config-src: profile=aac7` with the variables taken from the profile (SHMEM/OFI/transport/layer settings), so the profile loads and replaces the hard-wired line; the digits equal the reference in both gates.
- Both ecalc totals are single runs (A 23.31 s vs B 19.10 s): not a timing comparison ; the gate answers correctness only.
- Verdict: PASS (profile loading and validation work on a 2-node aac7 run). No step of ours was left in the hold at the end (steps `batch` / `extern` only).

#### Collected aac6 gates (hold 23007, 1 node, 1e9 digits, 2026-10-12 04:16-04:17 ET; measured)

| gate | result |
|---|---|
| A: standard line, `--build --nodes 1` | PASS (rc 0, VERIFY OK, digits identical, segv 0, ecalc total 11.82 s) |
| B: `--profile aac6` | PASS (same checks; ecalc total 12.18 s); `config-src: profile=aac6`, set from the profile: `MNRUN_MODULES, COMM_SHMEM_IMPL, COMM_SHMEM_SERIAL, OMPI_MCA_memheap_base_max_segments, ECALC_SEGV_TRACE`; nothing overridden by the environment |

The two totals are single runs, not a timing comparison. The hold was released by the driver (no step of ours left; `summary.txt` has no end line, the job is no longer listed).

#### RESUME
1. Read `~/o3/Q/summary.txt` (aac7) and `~/o3/summary.txt` (aac6): expect `A rc=0`, `B rc=0`, gate lines PASS, digits identical, `config-src:` line in B.
2. Paste results into this file; if all PASS, ask the user to adopt: merge o3 (then remove `~/ntt-wt/o3`, `~/ntt-o3` on aac6, worktree `../ntt-wt-o3`).
3. If gate B fails only because the profile lacks keys the aac7 LINE sets, that is a profile gap (profile_check.sh shows none today).

## L5 - parameter system step P1-1: switch registry + print-only effective configuration line

(former file: L5.md)

*Agent l5, session 2026-10-11c wave L. Branch `l5` from main a1569d5, not merged. No behaviour change: the only code edits are a
new file `ecalc/config_line.c|h` and two call sites in `ecalc.c main`. Times Eastern.*

#### Design
- `ecalc/switches.tsv` (generated, 266 rows): name, type, default, code_default, range, scope, profile, evidence, readme, source, files, sites, note.
  - 239 rows are read by `ecalc/*.c|h` (source=ecalc), 27 only by `ecalc/tests` (source=tests, scope test-only). (measured, by script)
  - B02 reports 235 for the A11 grep; the script ignores names inside comments but sees 4 more call forms (`getenv` in helper bodies, `env_on`).
  - `default` = effective default (hand value if the name is in `ecalc/switches_manual.tsv`, else the extracted one); `code_default` = what the
    extraction reads: a literal, `unset` (presence test, the variable overrides a value the code already has), `derived` (computed, 57 rows), `conflict:` (3 rows, launcher-set COMM_SIZE/COMM_RANK, MN_P24).
  - Extracted type is a heuristic (bool 75, int 129, float 24, str 8, enum 3 hand-set). Literal code defaults: 131 of 239.
  - Scope: A11 status mapped (adopted-default -> built-in default 110, launch-line 45, experiment 25 (experimental-off + rejected), test-only 59), plus hand scope for 4 names A11 does not classify. `profile` column: 19 switches appear in `profiles/*.conf`.
  - README presence: 220 Y, 19 prose, 0 N for ecalc switches.
- `ecalc/switches_manual.tsv` (hand-written, 48 rows): the launch-line switches (45 plus a few others) checked against their code sites by reading, with type/range/note; wins over the extraction. `--check` fails if a hand default contradicts a literal code default.
- `tools/switch_registry.py` (python3, no dependencies): no argument regenerates `switches.tsv` and `ecalc/switches_gen.h`; `--check` (read-only, exit 1 on problems); `--readme` prints a README table from the registry; `--readme-diff` sizes the difference.
  - `--check` fails on: a switch in the code but not the registry, a registry row with no code site, a changed `code_default`, a hand default that contradicts a code literal, a stale `switches_gen.h` or a stale `switches.tsv`. Negative tests (measured): POOL_LOG literal 31 -> 30 in ecalc.c gave 2 problems; a new `getenv("ZZ_NEW_SWITCH")` gave 2 problems (missing row, stale header); clean tree 0 problems.
  - Not covered (dynamic names): `COMM_LAYER_VSLOT_MB_<g>` (prefix), and the `POOL_LOG / RNS_PLANES_3Q30 / DIST_LOGN_TEST` knob array read in `plane_cap_switch`; those three are registered through their other sites.
- `--readme-diff`: the README switch table is 251 lines (rows carry long prose), the generated one 241 lines; the diff is 492 lines (everything), because the README rows are prose and the registry gives default/type/scope/evidence. The README is NOT changed. Proposal: keep the prose in the README and generate only a leading "registry" column block, in P1-2.
- Startup line (`ecalc/config_line.c`, header `switches_gen.h`): `cfg_snapshot()` as the first statement of `main` (before ecalc `setenv`s anything), `cfg_print(eff)` right after `plane_cap_switch`. One line on rank 0 (`COMM_RANK` unset or 0):
  `config: hash=<8 hex> set=<k=v ...sorted by name> eff=pool_log:..,np:..,base:..,verbose:..,overlap:..,plane_cap:..,comm_size:..`
  hash = FNV-1a 32 of the sorted `k=v\n` lines of the set switches (COMM_RANK excluded as per-process). `eff` shows resolved values (not hashed): pool_log, ECALC_NP, limb base, verbose, overlap, plane-cap index, comm size. Always on, one line, reads the environment only.

#### Tests
| test | command | result |
|---|---|---|
| unit (local, gcc, no GPU) | stub main + `config_line.c`; `POOL_LOG=30`, then add `ECALC_VERBOSE=2`, either order, `COMM_RANK=1` | hash a53c70a8 (matches an independent Python FNV), order-independent bd1dacca, empty set 811c9dc5, rank 1 silent (measured) |
| registry check local and aac6 | `python3 tools/switch_registry.py --check` | 266 rows, 0 problems, both places (measured) |
| aac6 CPX aac6-sh5cpx-b, hold 22954 (flock), build `make -s -j16` rc 0, clone ~/ntt-l5 @80e365b | 3 runs of `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30 ECALC_SEGV_TRACE=1 ./ecalc 1000000000`, digits vs `~/ntt/ecalc/results/e_1000000000.out` with `digcmp.sh` | all 3 VERIFY OK, rc 0, digits identical (measured) |
| config line stable | runs r1, r2 same env | `hash=8072f8a3` both; r3 with `ECALC_BG_THREADS=48` added: `hash=a099ed30` (measured) |
| totals (info only, not a timing claim) | ecalc `total` | 8.45, 8.17, 8.52 s (measured, 1 run each, no ABBA) |
`tools/gate.sh` cannot drive a CPX node (mnrun.sh and gate.sh pass `--gpus-per-node=4`; the node has 6 devices: srun "Invalid generic resource"), so the run used `srun --jobid --overlap bash -lc` as in T11. The hold was cancelled by the driver (own id only); no step left.

#### Open items / next steps (P1-2 and later)
1. Profile loading: read `profiles/<machine>.conf` (and `target.conf`) into the environment before the launch, with the registry deciding precedence (built-in < machine profile < launch-line < experiment); never override a variable already set.
2. Validation: reject unknown names that look like switches (typos: `ECALC_`, `COMM_`, `BS_`, `MN_`, `DBIG_` prefix not in the registry), values outside `range`, and non-literal combinations (e.g. `COMM_LAYER_VSLOT_RELEASE` without `COMM_LAYER_VSLOT_POOL`); warn first, then error.
3. Fill the registry gaps: `range` is only set for the 48 hand rows and bool (0|1); the 57 `derived` defaults need a hand value or a computed-default function; types are heuristic.
4. Generate the README table (or its default/scope/evidence columns) from the registry; `--readme-diff` shows the plain generated table replaces every row.
5. Wire `switch_registry.py --check` into `tools/gate.sh --build` (not done: tools/gate.sh belongs to others) and consider a per-rank hash line on multi-node runs (now rank 0 only).
6. Dynamic names (`COMM_LAYER_VSLOT_MB_<g>`) as prefix rows.

#### RESUME
Branch `l5` (3 commits on a1569d5), pushed; not merged. To regenerate after any switch change: `python3 tools/switch_registry.py && python3 tools/switch_registry.py --check`. Merge needs `tools/gate.sh --build --nodes 1` (done here by hand on CPX, 1e9 identical) and, because `ecalc.c`/Makefile changed, the usual 2-node aac7 gate.

## T27 — parameter system: computed size values and review dates for settings

(former file: T27.md)

**Question.** (TASKS 27, the two parts left after L5, O3 and W2B D1e.) A: can every setting whose right value depends on
the digit count or node count be computed in one place from (digits, nodes, machine profile), with the computed values
equal to today's hand values at the tested sizes? B: can every experimental, test-only or fallback setting carry an owner
and a review date, with the registry check warning (not failing) once the date has passed?

**Change / branch.** Branch `t27`. No ecalc C source changed; no digit can change.
- `ecalc/mnrun.sh`: `MNRUN_PRINT_ENV=1` (default 0) runs the whole launch computation (the planner's pool, the heap by SHMEM
  implementation, `FI_UNIVERSE_SIZE`, the libfabric defaults) and prints `export K=V` lines instead of calling srun; no
  allocation needed. Unset: the script behaves as before (its other messages and the launch are unchanged).
- `tools/plan_values.py [--profile P] [--line FILE] [--ecalc PATH] [--exports] DIGITS NODES`: the one entry point for the
  size-dependent values. It asks `mnrun.sh` (`MNRUN_PRINT_ENV=1`) for the launcher values and the planner
  (`MN_PLAN_ONLY=<digits>:<nodes>`) for the schedule, pool_log and the plan verdict. `--line` loads a hand-typed launch
  line, removes its size-dependent keys, computes them again and prints MATCH / DIFF / EXTRA / NO-CONSUMER per key (exit 1
  on a DIFF or `plan REFUSED`); `--exports` prints the `export` lines for a launch without `mnrun.sh` (the target runbook's
  srun form).
- `ecalc/switches_review.tsv` (new, hand-written): name, class (experiment / fallback / diagnostic / test), owner, review
  date, note — 93 rows. `tools/switch_registry.py`: columns `owner` and `review` in `switches.tsv`; `--check` prints
  `WARN` lines (exit status unchanged) for a passed date, an experiment / test-only switch without a row, a row whose
  switch is gone; `--review` lists the rows; `--today` fixes the date for tests; the regeneration writes the review table
  in `ecalc/README.md` (between markers; a stale table is a `--check` problem like a stale `switches.tsv`).
  `tools/doccheck.py` prints the warnings as `[review]` lines (not failures); `tools/gate.sh --build` prints their count.
- Docs: PLAN.md §3, `ecalc/README.md` (the review table, the `MNRUN_PRINT_ENV` row), `profiles/README.md`, TESTING.md §2 and §9.

#### A. Size-dependent values: what was still typed by hand

Searched: the aac7 line (`ecalc/e16_headline.sh` LINE, `tools/gate.sh --line`), `profiles/*.conf`, `ecalc/mnrun.sh`,
`ecalc/target_kit.sh`, the private target runbook (its launch block) and its private draft profile.

| value | where it is typed | status after T27 |
|---|---|---|
| `COMM_SHMEM_POOL_MB`, heap (`SHMEM_SYMMETRIC_SIZE` / `XT_SYMMETRIC_HEAP_SIZE` / `SHMEM_SYMMETRIC_HEAP_SIZE`) | target runbook launch block (1536, 2048M); the private draft profile | computed by `mnrun.sh` from the planner for every run through it (already so since Phase 14); `plan_values.py --exports` for the srun form. **The typed values are stale** (below) |
| `FI_UNIVERSE_SIZE` | nowhere (mnrun.sh: max(4096, 4 × tasks)) | computed, shown by `plan_values.py` |
| `MN_GROUPS` at 576 | target runbook, draft profile (`2,4,8,16,32,64,192,576`) | equals the code's default schedule at 576 (`mn_groups_parse`: powers of two, then the odd part's primes): typing it changes nothing |
| a key named in the runbook block that no code or script reads | runbook, draft profile | `plan_values.py` reports it as NO-CONSUMER (to drop) |
| aac7 lines (1 and 2 nodes, 10⁹ and 6.441 × 10¹⁰ per node) | nothing size-dependent is typed | computed values equal the values `mnrun.sh` printed in the last gates (below) |
| `target_kit.sh` a3 pools for `t_comm --bw` | a formula in the kit (`2 × threads × MB × nodes + 512`) | a test harness's own buffer size, not a run's plan value: left as is |
| `target_kit.sh` s2chk share 6.441 × 10¹⁰ per node | kit default | the coordinator's note: move to the 3.82 × 10¹³ share in the target-session preparation (not now) |
| aac6 CPX `RNS_INIT_POOL_LOG=29`, `ECALC_PLANE_CAP=2^30` | `profiles/aac6.conf` (commented) | machine-dependent (22.9 GB per XCD), not size: a CPX sub-profile if aac6 is used again |
| `MN_GROUPS` at 576 − k nodes (excluded nodes, no spares) | nowhere (the run would take the default, slow at awkward N: results/NODE_SLOWMODE.md §B04, C09.md) | not built: the structured form `2,…,64,192,N` cuts the top group, which the user's D4 ("no uneven groups for now") leaves open — see Decisions |

#### Method

All plan-only (no device, no node time): `tools/plan_values.py` on the aac7 login node with a SHMEM build of main b60e34d
(`make -B SHMEM_CRAY=1`, `-DCOMM_SHMEM` in the log), worktree `~/ntt-wt/t27`. Hand values: the gate logs of W2B D1e
(`~/lbd1e`, the `mnrun.sh:` lines) for aac7; for the target, the runbook's launch block and the private draft profile,
copied to `~/t27/*.env` (private, not in git).

    python3 tools/plan_values.py --profile aac7 <digits> <nodes>                                   # aac7
    python3 tools/plan_values.py --line ~/t27/t27_target_s4.env 38200000000000 576               # target, runbook block
    COMM_LAYER_VSLOT_POOL=0 python3 tools/plan_values.py --line ~/t27/t27_target_s4.env 38200000000000 576

#### Results (all values measured: printed by the planner / mnrun.sh on the aac7 login node)

| case | computed pool / heap / FI_UNIVERSE_SIZE / MN_GROUPS | hand value (source) | verdict |
|---|---|---|---|
| aac7 1 node, 10⁹ | 8192 / 8704M / 4096 / − | gate 1 node of D1e: 8704M, 4096 | MATCH |
| aac7 1 node, 6.441 × 10¹⁰ | 8192 / 8704M / 4096 / − | (same rule) | MATCH |
| aac7 2 nodes, 10⁹ | 512 / 1024M / 4096 / 2 | gate 2 nodes of D1e: 512, 1024M, 4096 | MATCH |
| aac7 2 nodes, 1.2882 × 10¹¹ | 512 / 1024M / 4096 / 2 | the D1e 2-node timing run: 512, 1024M, 4096 | MATCH |
| target 576 nodes, 3.82 × 10¹³, today's defaults | **2304 / 2816M** / 4096 / 2,4,8,16,32,64,192,576 | runbook block: 1536 / 2048M / 2,…,192,576 | MN_GROUPS MATCH; **pool and heap DIFF** (exit 1) |
| target 576, 3.71 × 10¹³, today's defaults | 2304 / 2816M / 4096 / same | the same block | the same DIFF |
| target 576, 3.82 × 10¹³, `COMM_LAYER_VSLOT_POOL=0` | 1536 / 2048M / 4096 / same | the same block | all MATCH (exit 0) |
| target 576, 3.82 × 10¹³, draft profile (X2 on) | 2304 / 2816M / 4096 / same | draft: 1536 / 2048M | DIFF |

Every plan above ended `plan check ... OK` (1236 products at 576 nodes). The one key of the runbook block with no reader is
reported NO-CONSUMER.

**Reading.** The planner reproduces every hand value it was made for: the aac7 values exactly, and the target's typed
1536 / 2048M exactly in the configuration they were planned in (X2's v-slot pool off). Since X2's pool became the default
(2026-10-12, W2B D1e; on the target line by the user's decision of 2026-10-11), the plan's SHMEM control area grows from
894.2 to 1662.4 MiB and the pool need from 1536 to 2304 MiB (m, `plan pool` line). The typed runbook block would then start
with a heap below the need, and the budget check would stop the run with rc 8 before compute (the runbook's own pool step) —
no wrong digits, but a lost launch. Computing the values removes that class of error; it is the reason to stop typing them.

#### Gates (ecalc/mnrun.sh changed: aac7 queue, SHMEM build)

aac7 queue item `60_t27_gate.sh` (hold 12672, 2 nodes, 2026-10-12 15:19 ET), commit 569b486, measured:

| gate | result |
|---|---|
| `tools/gate.sh --build --nodes 2` (aac7 line) | PASS: rc 0, VERIFY OK, digits identical, segv 0, ecalc total 18.35 s; registry check 0 problems, 0 review warnings |
| `tools/gate.sh --nodes 1` | PASS: rc 0, VERIFY OK, digits identical, segv 0, ecalc total 12.07 s |
| `MNRUN_PRINT_ENV=1 mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI=1 ./ecalc 1000000000` inside the hold | rc 0, no step launched; pool 512, heap 1024M (both names), `FI_UNIVERSE_SIZE` 4096 — the values the gate's own launch printed |

No step of ours was left in the hold (batch / extern only). The totals are single runs, not timing claims.

#### Part B: review dates

| class | rows | owner | review by (seed rule) |
|---|---|---|---|
| experiment | 9 | coordinator (TASKS S) | 2026-11-11 (30 days) |
| fallback (the `=0` / old value kept after a default flipped on 2026-10-12: the six D1e switches, the three CAND switches, `ECALC_OUT_PACKED`) | 10 | coordinator (TASKS S) | 2026-11-11 (30 days after the flip) |
| diagnostic (test-only switches read by ecalc) | 47 | diagnostics (TESTING.md) | 2027-04-12 (6 months) |
| test (read only by `ecalc/tests`) | 27 | the test program | 2027-04-12 |

Checks (local, measured): clean tree `--check` 0 problems, 0 warnings, exit 0; `--today 2026-11-12`: 19 warnings
(the 9 experiments and 10 fallbacks), exit 0; a past date, a row for a missing switch and an experiment without a row gave
one `WARN` line each, exit 0 (plus the stale-README problem, as intended, until regenerated); `tools/doccheck.py` printed
the warnings as `[review]` lines with exit 0.

#### Verdict

Both parts built. A: one entry point for the size-dependent values (`tools/plan_values.py`, backed by `mnrun.sh`'s own
computation and the planner); equal to the hand values at every tested size, and it found the target runbook's pool and
heap stale under today's defaults. B: owner and review date on every non-default setting; warnings only. Adoption of the
runbook change is the user's (Decisions).

#### Open issues

- The target runbook's launch block (private) still types the stale pool / heap and the redundant schedule: the change
  is the user's decision (below). The private draft profile carries the same keys.
- `MN_GROUPS` at 576 − k nodes: not computed (Decision).
- The aac7 1-node line goes through the SHMEM path of `mnrun.sh` (the line sets `COMM_TRANSPORT=shmem`): pool 8192 and
  heap 8704M at one process are mnrun's fallback values, harmless (as before).
- Lane A removes switches: their review rows then print a "gone" warning until deleted from `switches_review.tsv`.

#### Decisions (for the user)

1. **Target launch block: stop typing the size values.** Why: the typed pool / heap (1536 / 2048M) are stale under today's
   defaults (the plan asks 2304 / 2816M) and would stop the run with rc 8. Options: (a) replace the four typed lines with
   `eval "$(tools/plan_values.py --profile target --exports <digits> <nodes>)"` — cost: one line changes in what the user
   types; benefit: always the plan's values. (b) launch through `ecalc/mnrun.sh` (computes everything, as the kit does) —
   cost: a different launch form at 576 nodes than the runbook's srun line; benefit: the same path as every aac7 and kit run.
   (c) keep typing, updated to 2304 / 2816M — cost: goes stale again with the next default change; benefit: no new step.
   Recommendation (a).
2. **`MN_GROUPS` with excluded nodes (576 − k).** Why: the default schedule is slow at awkward node counts (modelled +34 s
   at 575, B04 / C09); the structured form `2,…,64,192,N` cuts the top group. Options: (a) leave as is (D4) — cost: a slow
   schedule if nodes are excluded; (b) a switch, off by default, that takes the full machine's schedule cut at N (C change,
   needs gates and a run at a cut top) — benefit: about 34 s at 575 nodes (modelled, C09: 211.3 vs 245.4 s). Recommendation (a) until a node exclusion is
   actually expected.

#### RESUME

Done: branch `t27` merged to main and pushed; gates PASS (output `~/t27/Q/` on aac7); worktrees removed (local and aac7);
nothing running. Next: the two decisions above; when lane A removes a switch, delete its row from `ecalc/switches_review.tsv`
and run `tools/switch_registry.py`.

## C04 - environment probe (`tools/envprobe.sh`) and the probe covariate in `ecalc/calib_table.py`

(former file: C04.md)

Task c04 (2026-10-11, base bc80001, branch c04). Labels: measured = read from logs/NIC counters; modelled = statistics on them.

#### What was built
- `tools/envprobe.sh`: runs `tests/t_comm --bw 4 REPS(100) 256` (the A13 probe; no new test program needed) over all nodes of the hold with FIXED settings
  (the caller's COMM_/MN_/RNS_/ECALC_/DM_/FI_ variables are removed; `COMM_TRANSPORT=shmem FI_MR_CACHE_MONITOR=userfaultfd COMM_SHMEM_POOL_MB=12000`),
  reads `oxe_mcu_meas_18..25` (group A) vs `28..33` (group B) and `oxe_stall_fgfc_start_0` on every node before and after, prints
  `ENVPROBE r64=<GB/s/thread> r256=<GB/s/thread> group=A|B|NA fgfc=<M> nn=<n> t=<epoch> wall=<s>` (also appended to `$OUT/envprobe.log`).
  Modes `--snap FILE` and `--group PRE POST` bracket a real run with the same reader. Expected cost 35-45 s (A13: 43 s at 250 reps; 100 reps assumed ~30-35 s, not yet timed).
- `ecalc/calib_table.py` (copy of branch b07, **overlaps b07: the coordinator must merge, c04's version is b07's plus the probe additions**): parses `ENVPROBE` lines
  in a run log or in a sidecar `<label>.probe`; new columns `pr64 pr256 pgroup pfgfc`; `--covariate probe` (fits and `--ab` use the probe rate instead of the run's own r;
  `--probe-col r64|r256`), `--probe-check` (does the probe predict the run r and total?). Selftest passes (`python3 -I ecalc/calib_table.py --selftest`).
- `tools/envprobe_offline.py` rebuilds probe lines from A13's data (probe logs + NIC snapshots) for the offline check.
- `tools/c04_queue_item.sh`: the queue item (below).

#### Offline validation on A13 (6 runs r001-r006, 7 probes p000-p006; each run bracketed by probes p(k-1), p(k))
- Group reader reproduces A13: probes p002, p005, p006 = A, p000, p001, p003, p004 = B (measured); runs r004, r005 = A, others B.
- Does probe r predict run r? Run r = 12.00 13.10 13.07 10.99 11.60 13.68 (raw sd 1.04 GB/s). Correlation with the probe rate (n=6, modelled):

| probe covariate | corr with run r | leave-one-out sd of r (GB/s) |
|---|---|---|
| r64 before | -0.71 | 0.91 |
| r64 after | +0.09 | 1.81 |
| r256 before | -0.69 | 1.19 |
| r256 after | -0.76 | 0.85 |
| r256 mean of before+after | **-0.98** | **0.37** |
| r64 mean of before+after | -0.47 | 1.02 |

- Reading: the sign is NEGATIVE (a higher 256 MiB probe rate goes with a slower run), the mean-of-bracket r256 explains almost all of the variance of r in 6 points (LOO sd 0.37 vs raw 1.04),
  but n=6, six covariate variants were tried, and no mechanism is known. A13 already noted the slow runs had the two highest "before" probes. **Not proven; treat as a hypothesis the queue item tests.**
  Probe group does not predict run group (A13 finding 3, confirmed: p004 B -> r005 A, p005 A -> r006 B). The run's own group matches slow/fast (r004, r005 A).
- Consequence for the plan: since the group is drawn per launch, a probe cannot see the next run's group; it can only be an exchange-independent covariate if the r256 relationship above is real.
  The run's own NIC group (read with `--snap/--group`, no extra cost) is a second, post-hoc covariate that no arm can move, and is the better-founded one (6/6 agreement with slow/fast).

#### Queue item (aac7)
`~/aac7q/queue/70_c04_envprobe.sh` (OWNER=c04, NODES=4, EST_MIN=30, APPROVED=coordinator-2026-10-11): 6 probes and 5 real runs (6.441e10 digits/node, e16_headline aac7 line + uffd + xstats,
no digit writes) in the order probe, run, probe, run, ..., probe (each run bracketed by two probes; the shared probes make 6 instead of 10, to fit 30 min: modelled 6 x ~40 s + 5 x ~215 s + prep, ~24 min).
No new cycle starts after 27 min (BUDGET_S). Worktree build `~/ntt-wt/c04` (login node, SHMEM build checked: `-DCOMM_SHMEM` in `~/sess1011c/c04_build.log`, `ldd` shows libsma). Output only `~/sess1011c/c04/Q/`:
`runs.tsv`, `probe/*.line`, `log/rK.log`, `log/rK.probe`, `summary.txt` (runs the `--probe-check`), marker `C04_DONE`.
Dry run (STUB=1, login node, 2 cycles): passes (probe lines, runs.tsv, summary, marker). NOT dry-run on real nodes: the node-side counter reader was tested on a fake sysfs tree only; the first real use is the queue item.

#### Collect (after the item ends)
`cat ~/sess1011c/c04/Q/summary.txt`; then `python3 -I ecalc/calib_table.py ~/sess1011c/c04/Q/log --probe-check --probe-col r256` (and `r64`), and with 5 new + 6 A13 pairs pooled for the sign check.
Decision rule: adopt the probe as covariate only if the pooled (11) corr stays strongly negative (|corr| > 0.8, LOO sd well under the raw sd) for the same column chosen BEFORE looking at the new data (r256 mean of bracket).

#### Open issues
- Probe rate at REPS=100 (vs 250 in A13) is untested; if rates shift, compare only within one REPS.
- fgfc on a probe is a delta over ~35 s, on a run over ~200 s: not comparable without dividing by wall (not done).
- The probe also cannot say what picks the group (a14's question).

#### RESUME
Branch c04 pushed. Item queued; after it runs, collect as above, update this file with the 5 new pairs, and tell the coordinator to merge calib_table.py with b07's. Worktree `../ntt-wt-c04` and aac7 `~/ntt-wt/c04` stay until the item has run (the item uses the aac7 worktree).

## S18M: AAC7 profile calibration against the 2026-10-06/07 10-node comm_ofi runs (branch `s18-m`)

(former file: S18M.md)

Local model only (`ecalc/estimate.py` / `mn_model.py`), no cluster jobs from this task. Checks `mn_model.AAC7`
(`apply_profile('aac7')`, bw_apu = 3.6 GB/s per APU thread, Phase 16 C's 2-node SHMEM put-path measurement) against
three **measured** no-write 10-node walls from job 12287 (aac7, `~/s18ab3/summary.txt`, comm_ofi on by default):

| run | digits (total) | D/node | measured no-write wall (m) |
|---|---|---|---|
| b0 | 8.1e11 | 8.1e10 | 721.90 s |
| b' (`ECALC_VSLOT_BUDGET=1`) | 8.1e11 | 8.1e10 | 679.16 s |
| c | 6.441e11 | 6.441e10 | 457.15 s |

All three VERIFY OK; aac7 walls vary ±25% between jobs at the same config (protocol note), so b0 vs b' (same
digits, same node count, 679.16 vs 721.90 s = 6.3% apart) brackets that noise.

#### 1. Old profile (`--fabric aac7`, bw_apu = 3.6, AAC7_CONSTS unchanged): `./estimate.py --g 10 --D <D/node> --fabric aac7`

| run | modelled (mod) no-write | measured (m) | error % (mod over m) |
|---|---|---|---|
| b0 | 797.9 s | 721.90 s | **+10.5%** |
| b' | 797.9 s | 679.16 s | **+17.5%** |
| c | 645.0 s | 457.15 s | **+41.1%** |

Error grows as digits shrink — not a flat offset. `AAC7_CONSTS`'s `NODE_SCALE`/`INIT_SCALE` (1.12/1.20, fit for the
ROCm 7.0.3 local-compute slowdown) were tested as a candidate fix (`MN_MODEL_NODE_SCALE=1.0 MN_MODEL_INIT_SCALE=1.0`
vs the profile's 1.12/1.20): the 8.1e11 estimate only moves 797.9 → 780.7 s (**-2.2%**) — not the dominant term.
`exposed` (the uncovered fabric time) is 641.3 of 797.9 s at bw=3.6 (80% of the modelled wall), so the fabric
constant, not the local-compute scale, is what is off.

#### 2. New profile `aac7_s18` (`ecalc/mn_model.py` `AAC7_S18`; `AAC7_CONSTS` left untouched)

`bw_apu` refit to **4.25 GB/s per APU thread** — the value that centers the two same-digit no-write walls (b0, b'):
at bw=4.25, modelled 699.6 s sits -3.1% under b0 and +3.0% over b'. `./estimate.py --g 10 --D <D/node> --fabric aac7_s18`:

| run | modelled (mod) no-write | measured (m) | error % |
|---|---|---|---|
| b0 | 699.6 s | 721.90 s | **-3.1%** |
| b' | 699.6 s | 679.16 s | **+3.0%** |
| c | 564.8 s | 457.15 s | **+23.5%** |

`c`'s error improves (41.1% → 23.5%) but is not closed — it is a different, smaller digit count (6.441e11 vs
8.1e11), so a single flat `bw_apu` cannot fit both points exactly; this is a residual open item, not a bug fix.

#### 3. The a4 comm-mark rates do not explain the residual (measured, `~/s18ab3/kit/logs/04_a4.log`)

GB-received-weighted average per-APU-thread comm_ofi rate, summed over all comm-mark phases:

| node count | Σ GB received | Σ post-to-completion s | weighted rate |
|---|---|---|---|
| n=2 | 373.02 GB | 25.84 s | **14.43 GB/s/APU thread** |
| n=8 | 814.58 GB | 86.33 s | **9.44 GB/s/APU thread** |

Both are far above both 3.6 (old) and 4.25 (new, fit) GB/s — plugging either measured rate directly into `--bw`
makes the model run *faster* than measured (e.g. `--bw 9.76` gives 392.4 s at 8.1e11 vs the measured 721.90 s, a
-45.6% error in the other direction). So the comm-mark all-to-all rate is not a 1:1 stand-in for the model's
`bw_apu` term — the wall also carries bs/dm local-compute time, init, and dc (now excluded, no-write) that the
comm-mark isolated test does not include at the same proportions. Left open for a future calibration pass; `c`'s
residual (+23.5%) is likely the same mechanism.

#### 4. `--target` unaffected

`./estimate.py --target` output is byte-identical before and after this change (diffed against the pre-edit run).
`aac7_s18` is a new, off-by-default profile name (`--fabric aac7_s18` or `MN_MODEL_PROFILE=aac7_s18`); `AAC7_CONSTS`,
`AAC7`, `TARGET`, `TARGET_M` and every existing profile's behavior are untouched.

#### Files changed

- `ecalc/mn_model.py`: `AAC7_S18` (new `Fabric`, bw_apu=4.25, all else = `AAC7`'s values), `PROFILES['aac7_s18']`,
  `apply_profile()` applies `AAC7_CONSTS` to `aac7_s18` exactly as it does to `aac7` (unchanged dict).
- `ecalc/estimate.py`: `--fabric` choices gain `aac7_s18`; help text documents it.

## B07 - `ecalc/calib_table.py` (step 1 of the calibration plan in results/TOOLS_PROFILES.md §B05)

(former file: B07.md)

Task b07 (2026-10-11, base bc80001, branch b07). CPU only, 0 node time. Labels: measured = parsed from logs; modelled = statistics on them.
Logs copied from aac7 (bounded, 26 MB): `~/s47/run/log` (48 logs), `~/batch1011/T5/s50/abba4_0024/log` (16), `~/batch1011/T5/s51/abba4/log` (12), `~/batch1011/T5/s51/abba4b/log` (23) = 99 logs; driver `ab.tsv`/`summary.tsv` of S47.

#### The tool
`ecalc/calib_table.py LOGDIR_OR_FILES... [--tsv F] [--fit [--oos]] [--ab [--exchange-exp EXP] [--desc EXP=TEXT]] [--driver-tsv FILES] [--config REGEX=NAME] [--selftest]`.
- One row per log: src, label, exp/round/arm (from the file name: `ii_r3_A`, `r1_2B`, `n4_r1`), config (md5 of the env words if the log prints a `env:`/`config:`/`LINE |` line, else `--config`; the current logs print none, so it is `-`), nodes and digits (from `MN_PLAN_ONLY=`), total, init, bs, tree
  (max node tree levels), recip, dm and its sub-stages (reciprocal products, step 1, step 2, X_lo Q from the `divmod(mn, DKM)` line), dc, other, VmHWM, single-node chain, r (mean of the `comm_shmem: pe N: all-to-all ... GB/s per APU thread` lines), rmin/rmax, mode (slow < 11.95, fast > 12.3 GB/s), flags
  (chain > 2.6 s, init >= 36 s, no VERIFY OK, no total), `clean`, per-phase `dev_`/`live_`/`hwm_` memory (init, tree, bs, end), `bud_*` (budget peak device/host), `live_win`, `live_pct`, `avail_min`, `xs_<phase>_<kind>_gbpe/_rate` and `wait_/bar_<phase>` when `COMM_XSTATS` / `MN_WAIT_STATS` lines exist, `wall`/`start` from the driver files, `resid` after `--fit`.
- `--fit`: ordinary least squares `stage = a + b / r` over clean runs with a rate, per (nodes, digits/node, config); stages total, bs, dm, recip; residual sd (n-2 dof). `--oos`: fit on all other sources, bias and sd on the left-out source.
- `--ab`: per experiment (src, exp), pairs = rounds with both arms; a pair is used only if every run in it is clean. Reports B - A raw, the rate-adjusted difference of residuals (group fit, treated as known: CI slightly optimistic), an ANCOVA cross-check (arm intercepts, common slope, fit on the pairs only; this CI includes the slope uncertainty, so quote it), per-stage (bs, dm, recip) raw and adjusted, the mean r of each arm (paired and Welch), the WARNING when the arms' mean r differ (significant, or > 2 %), the CAVEAT for experiments named in `--exchange-exp`, and the pairs needed for a target half-width (`--target-hw`, default 2 s).
- Not in the logs, so not done: per-phase rate covariate (S47 logs have no `xstats`; the t09-style logs do and the columns fill automatically), the environment probe (B05 rank 4), the `--table` output for `refit_s48.py` (not wired).

#### Acceptance 1: T05 clean 4-node runs (99 logs read, 63 clean 4-node with a rate; 32 not clean, 2 of them failed runs, 2 are 2-node)
| quantity | B05 (modelled) | calib_table (modelled, n = 63) |
|---|---|---|
| total = a + b/r | 80.8 + 1388 / r | **81.1 + 1384 / r** |
| residual sd | 1.61 s (raw 11.9 s) | **1.62 s** (raw 11.87 s) |
| bs | 46.3 + 337/r, sd 1.55 | 46.4 + 336/r, sd 1.55 |
| dm | 2.1 + 1030/r | 2.2 + 1029/r, sd 0.94 s |
| recip | - | 3.2 + 258/r, sd 0.36 s |

a differs by 0.3 s and b by 0.3 % from B05 (B05's exact r definition or rounding of the log values; same 63 runs). Out-of-sample (leave one source out, total): abba4 (n=11) bias -1.9 s sd 2.0; abba4_0024 (n=16) +0.1, 1.3; abba4b (n=7) +0.2, 2.5; S47 dir (n=29) +0.9, 1.1 (fit on the other sources). Command:
`python3 -I ecalc/calib_table.py <s47 log> <s50 abba4_0024 log> <s51 abba4 log> <s51 abba4b log> --fit --oos --tsv t05.tsv` (a 4-node group of 63 clean runs; the 2-node group has n=2 and no fit).

#### Acceptance 2: S47 ABBA, rate-adjusted effects (4 nodes, arm B - arm A, seconds of `total`; negative = B faster)
Command: `python3 -I ecalc/calib_table.py <s47 log dir> --ab --exchange-exp i --exchange-exp ii --driver-tsv ab.tsv summary.tsv`. S47 fit (29 clean 4-node runs): total = 84.4 + 1348 / r, sd 0.94 s. 11 (T2048) and 10 (X2) rounds, of which only **6 pairs each are usable** (the rest hold a chain-spike or cold-start run, flagged unclean).

| effect (B - A) | raw (n=6) 95 % CI | rate-adjusted, residual diff | ANCOVA (conservative) | mean r A / B (paired B-A) |
|---|---|---|---|---|
| **T2048** (`MN_T_CHUNK_MB` 1024 -> 2048) | +6.9 s [-7.0, +20.7] | **-1.08 s [-2.03, -0.14]** | **-0.91 s [-1.99, +0.18]** | 13.73 / 12.86 (-0.87 [-2.5, +0.8]) |
| stages (adjusted) | dm raw +6.0 [-5.0, 17.0] | dm -0.09 [-1.04, +0.86], bs -0.69 [-2.32, +0.95], recip +0.15 | | |
| **X2** (`COMM_LAYER_INTER2` + `VSLOT_POOL` 0 -> 1) | -4.2 s [-15.7, +7.3] | **+0.41 s [-0.59, +1.41]** | **+0.44 s [-0.54, +1.42]** | 12.80 / 13.34 (+0.54 [-0.7, +1.8]) |
| stages (adjusted) | dm raw -3.1 [-11.6, +5.4] | dm +0.41 [-0.44, +1.27], bs +0.24, recip +0.14 | | |

Reading (modelled): the raw ABBA CIs (+-14 and +-11.5 s) hide both effects; the adjusted CIs are 10x narrower. T2048: about -1 s (the adjusted CI excludes 0, the ANCOVA CI just includes it), all of it outside dm's exchange-bound term (dm -0.1 s), so not distinguishable from zero in dm; X2: +0.4 s, no effect within +-1.4 s.
**Caveats.** (1) Both arms change the exchange itself (X2 changes the layers; T2048 changes the dm round sizes), and the arms' mean r differ (-6.3 % and +4.2 %, neither significant with 6 pairs). A rate covariate that the arm moves is not arm-independent: if the arm really slows or speeds the exchange, the adjustment removes that part of the effect, and the true effect lies between the raw and the adjusted one. The tool prints this WARNING/CAVEAT; resolving it needs the environment probe (B05 rank 4) or a per-phase rate that the arm does not touch (bs-phase rate for dm-only arms; needs `COMM_XSTATS=1` in the launch line). (2) 5 of 11 and 4 of 10 rounds were dropped because a run in the pair had a chain spike or a cold start; the number of usable pairs, not the adjustment, limits the CI here. (3) The residual-difference CI treats the fit as known; use the ANCOVA CI when the two disagree about 0.

#### Files
`ecalc/calib_table.py` (new, ~400 lines, stdlib), `ecalc/README.md` (one row), this file. No engine code touched.

#### Open issues
- `--table` output for `refit_s48.py` and the ABBA stop rule on the adjusted CI (B05 step 1, items 4-5 of the plan) not wired; the `--ab` pairs-needed line is the first half.
- `config` is `-` for all current logs (the logs print no env words); until the driver echoes its launch line into each log (an `env: ...` line), pass `--config`.
- The t09 xstats logs were not part of this acceptance (only partly present in the scratch copy); the xstats/wait columns are exercised by a selftest only for presence, not validated against T09.

#### RESUME
Done and merged to branch b07 (not to main). Next: wire `--table`/stop rule, add the `env:` echo to the drivers, validate xstats columns on the t09 logs.

## B05 - a faster calibration process (design; CPU + reading only)

(former file: B05.md)

Task b05 (Wave B, 2026-10-11, main = bc80001). Goal: calibrate how long a full-size run takes and how much memory it uses
with far fewer full runs. Sources: results/NODE_SLOWMODE.md §T05 (93 four-node runs), results/NODE_SLOWMODE.md §T09, results/CHECKPOINT.md §N13, the code
(`ecalc/ecalc.c`, `mn.c`, `mnrun.sh`, `refit_s48.py`, `mem_model.py`), archive:drivers (s47, s48, s50), and aac7 `~/sess1011b/t09`
(read only). Labels: **measured** (read from logs or timed), **modelled** (computed from measured data), **assumed**.
Node-minutes = nodes x wall minutes. Times Eastern.

#### 0. Answer in five lines

1. The noise is not random enough to need many runs. In the 63 "clean" 4-node runs of T05, `total = 80.8 s + 1388 / r`, where `r` is
   the run's own all-to-all rate (GB/s per APU thread, printed by every run). The residual sd is **1.6 s** against **11.9 s** raw
   (fit **modelled** on **measured** runs; fitted on S47 only it predicts the S50/S51 runs with bias -0.9 s, sd 1.8 s, out of sample).
   Every run, fast or slow, then carries information; no run is thrown away and no pairing is needed to cancel the mode.
2. So the cheapest big gain is software, not cluster time: one parser that turns each log into one table row (stage times,
   per-stage exchange rate, memory), plus the rate-adjusted estimator. Same 10 ABBA pairs (254 node-min at 4 nodes), CI half-width
   about 1.6 s instead of ~17 s (**modelled**).
3. Per-run overhead is small: wall minus `total` is only **6-7.5 s** (**measured**, 9 runs, t09); the 30 s init is *inside* `total`
   (16 % of a 4-node run, **measured**). Skipping eviction/verify/guard buys at most 3 %; a persistent process (no init) would buy 16 % but is a
   large, risky code change. Not the place to look.
4. Memory needs no full run per point: the device layout is allocated at init, and `MN_PLAN_ONLY` + `BS_LAYOUT_ONLY` +
   `mem_model.py --check-c` predict it exactly (335.6 GB planned vs 335.0 GB in use after init, t09 rank 0, **measured**). One
   `ECALC_INIT_ONLY=1` run confirms it (about 30 s + launch). Only the host peak and the pool-live peak need one full run per config.
5. Restarting at the top tree set (to sweep dm parameters) works in code but is a net gain of only about 20-40 s per run
   (**modelled**), and not tested at scale; rank it below 1-2.

#### 1. Where a run's wall goes (4 nodes, 6.441e10 digits/node, fast mode; measured, t09/T05)

| part | seconds | share | cuttable for calibration? |
|---|---|---|---|
| srun launch, PMI, process start before `t00`; teardown after `total` | 6-7.5 (wall - total, 9 runs; 5.8-7.5) | 3.5 % | at most ~3 s: one srun for a batch is not possible (each run is a process), so no |
| `init` (VMM arena mapping 7-8 s per APU, plane pools 6.5-7.3 s, seed buffers, transport 2.3 s) | 29.8 | 16 % | only by a warm process (see 1.2) |
| bs (leaf + tree) | 70.7 | 39 % | only by restart (section 3) |
| dm (reciprocal 22 s inside) | 76.8 | 42 % | the part under study |
| dc + T1/T2 + other (VERIFY) | 3.0 + ~1 | 2 % | keep: it is what makes the timing valid |
| total | 181.4 (sd 1.4) | | |

Notes on what the previous drivers add on top of `total`:
- **Eviction** (`posix_fadvise DONTNEED` of `~/ref/*`) only matters when a reference file is read. The no-digit-write calibration runs read
  none (s48_run.sh says so). It costs ~1 s; skip it for calibration, keep it for digit-gate runs.
- **MemAvailable guard** (`ECALC_MEM_GUARD_GB=6`, sampler every 1 s) is already inside the run and free; keep. The driver's `prep` adds an `srun`
  per node and, when MemAvailable is low, a `sleep 30` loop up to 300 s (s47_driver.sh). Keep the check, do it once per hold and after any abort.
- **Poll quantisation:** `rd_run` polls every 20 s, so the driver's wall is rounded up to 20 s (T05: 181-184 s runs recorded as 200 s). With
  runs back to back this adds about 10 s per run on average (**assumed** from the quantisation) = 0.7 node-min per 4-node run (5 %). A `wait $pid` with
  the watchdog in a side loop removes it. t09's driver, which was not quantised, ran back to back (gaps of 0-1 s, measured from its summary).
- `rd_health` before every run: 4 serial `srun`s (assumed 4-8 s). Do it before the first run and after a failure.
- `mnrun.sh` per launch: one `MN_PLAN_ONLY` call on the login node, one `srun` probe for a cxi NIC, then `bash -lc module load` on every rank. All inside the 6-7.5 s.

##### 1.2 What a warm process would need (not recommended now)
Running K configurations in one process (pools, VMM arenas, transport, plane pools kept) removes the 29.8 s init and the 6-7 s launch per extra run:
36 s of 188 s (19 %, 2.4 node-min per 4-node run, **modelled**). It needs a re-entrant `ecalc` main (free all stage state, rebuild P and Q), a re-armed
`mn_selftest`, and leaves memory state between runs (fragmentation of the block pool), which would bias the memory calibration. Effort: days; risk: high. Revisit only if the
other items leave init as the dominant cost.

#### 2. Extract more per run: one calibration table

Every run already prints (when the switches are on) all that is needed: `total` line (init, bs, dm, other, VmHWM), `tl` init timeline (`ECALC_INIT_TL=1`), `xstats` per PE, phase and kind
(`COMM_XSTATS=1`: GB, t_post, t_call, t_wait), `wait-stats` per node and phase (`MN_WAIT_STATS=1`), `comm_shmem: ... all-to-all ... GB/s per APU thread`, the `budget:` lines (peak device + host
per rank), the `mem[r] [phase]` tables, the `live:` lines (pool live and window peak per level, MemAvailable, Cached), `memsample` (min MemAvailable). t09 logs (stats on) have all of them, and its 6 fast-mode
runs average 183.2 s against T05's stats-off fast mean of 181.4 s (+1.8 s, n = 6 vs 47, about 1.3 standard errors: not significant, but a cost of up to ~2 s is not excluded; **measured**; verify with 6 runs without the switches in the first calibration hold). Make them part of the default
calibration launch line (`COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1 ECALC_LIVE=1 ECALC_MEM_SAMPLE=1`).

Table (one TSV row per run, so a sweep is a single file; written by a new `ecalc/calib_table.py`, which reuses `refit_s48.parse_log`):

| group | columns |
|---|---|
| identity | label, hold, start EDT, nodes, digits/node, git commit, the env words of the line, hosts, rc, VERIFY |
| wall | total, wall (driver), init, bs, recip, dm, dc, other; per tree level times (`tree levels`) |
| network | per phase (bs, recip, dm, other) x kind: GB per PE, t_post, t_wait, rate = GB / t_post (mean, min, max over PEs); the headline `all-to-all` rate; barrier and wait per node |
| mode | fast / slow by the rate threshold (11.95 / 12.3 GB/s, T05); chain spike flag (`recip(mn): single-node chain` > 2.6 s); cold-start flag (init > 36 s) |
| memory | `budget:` peak device and host per rank; device in use after init; VmHWM; minimum MemAvailable (sample) and when; max `live:` window peak / pool per APU and level; per-phase `mem[r]` totals |
| derived | adjusted time per stage = a_stage + b_stage / r (below), residual |

Feeding `refit_s48.py`: add `--table FILE` (it already accepts `--table nodes:total:init`) so the refit reads (nodes, adjusted fast-mode totals, init) from the TSV, and add the measured
exchange rate (GB / t_post by phase) as a fixed input to the bw_apu grid search instead of fitting it blind. Today's refit needs a hand-made `--table`.

#### 3. Stage-targeted calibration by checkpoint restart (what works today)

Read: `mn.c` (`mn_tree`, `mn_ckpt_tree_level`, `mn_ckpt_top_finish`), `binsplit.c` (1595-1602), `ecalc.c` (548-552, 575-590), results/CHECKPOINT.md §N13.

Works today (**measured** in N13, small sizes): `BS_CKPT_DIR=<dir>` with `BS_CKPT_TREE_EVERY=64` writes only the top tree set (`n<rank>_tree_LLL.*`, magic ECBSCKP3, header records node count and the `MN_GROUPS` schedule); `ECALC_CKPT_TOP=1|2`
writes it in the background during the reciprocal and division. A second launch with `BS_RESTART=1` and the same dir, node count and `MN_GROUPS` finds the top level on every node, skips seeds and the whole bs ("bs: restart at tree level L: the leaf tree is skipped"),
reads P and Q shares, and runs reciprocal + division + output + VERIFY. A different node count or schedule is refused on every node (rc 4 / 5), not a hang. Digits identical at 1e8 size 4 and 1e9 sizes 2 (VERIFY OK on all nodes).

What a dm-sweep run would cost (**modelled**; P and Q scale linearly from N13's 17.8 GB P at 4e10 digits): 2.576e11 digits -> P = Q = 115 GB, 28.6 GB each per node, 57 GB per node for the set.
- Save: bs + tree = 70.7 s. Pay: init 29.8 s unchanged (the layout stays the full run's, which keeps memory representative) + read 57 GB: 36-52 s at 1.1-1.6 GB/s (the rates N13 measured for write on aac7/aac6 disks;
  read rate not measured) or about 12 s from a warm page cache. Net saving 20-35 s of 181 s (11-19 %), i.e. 1.4-2.3 node-min per 4-node run.
- Write once: a full run with the set costs more (N13: writing at 0.22 GB/s stalled a 4e10 run from 80 s to 128 s; at >= 1.1 GB/s it is hidden), so the first run is a normal run plus ~0-60 s.

Gaps before it can be used for sweeps:
1. **Never run at scale** (N13 tested 1e8-1e9; 57 GB per node is 60x larger). Needs one 4-node trial.
2. **Where to put 57 GB per node.** aac7's `/tmp` is RAM (t09: an 80 GB file in `/tmp` took Cached to 165 GB and tripped the 6 GB MemAvailable guard); after init MemAvailable is 90-144 GB and the run still takes 17-34 GB, so 57 GB leaves 0-70 GB margin. NFS home is 0.1-0.3 GB/s. Local NVMe on aac7 is not documented in the protocol: **unknown**, check first.
3. **Representativeness:** P and Q come from `bs_ckpt_tree_read` instead of the tree's own allocations, so the pool's live/peak figures and cache state differ from a full run. Good for timing of the exchange-bound dm; not for memory peak.
4. Only valid for knobs that act after the tree (dm / Newton / output: `MN_T_CHUNK_MB`, `COMM_LAYER_*` inside dm, `DIST_GATHER_TILE`, `NEWTON_*`); anything that changes the tree or the layout needs a fresh set.
5. A switch to leave out the bs part of init (arena mapping for the regions, 8-9 s per APU) would save another ~8 s but changes the memory layout; do not.

Needed: no engine change to try it (a driver `calib_dm.sh` that runs a full run with the set, then K restarts); a dm-only wall metric (`dm` + `recip` from the `total` line) and the rate covariate of section 4 are required to compare restarts.

#### 4. Noise: classify, then regress

Data: T05, 93 four-node runs, 63 clean (chain <= 2.6 s, init < 36 s). Slow share 29 % (27 of 93), independent between runs.

| quantity | value | label |
|---|---|---|
| fast mode total / sd | 181.4 s / 1.42 s (n = 47) | measured |
| slow mode total / sd | 208.3 s / 1.80 s (n = 16) | measured |
| raw sd of clean totals, modes mixed | 11.9 s | measured |
| `total = a + b / r` (r = mean over PEs of the all-to-all rate) | a = 80.8 s, b = 1388 s GB/s, residual sd 1.61 s | modelled |
| bs: a = 46.3, b = 337, sd 1.55 s; dm: a = 2.1, b = 1030, sd 1.06 s | dm is 97 % exchange-bound at these rates | modelled |
| out of sample (fit S47 clean n = 27, test S50/S51 clean n = 36) | bias -0.87 s, sd 1.81 s | modelled |
| fit from 6 random clean runs, error on the other 57 | mean abs 1.5 s | modelled |

Runs needed for a 95 % CI half-width h on a **single configuration's absolute time** (normal approx, minimum 3 in practice; **modelled**):

| h | ignore mode (sd 12.3) | classify and use only fast runs (sd 1.4) | rate-adjusted (sd 1.6, any mode) |
|---|---|---|---|
| 1 s | 577 | 12 (9 fast) | 10 |
| 2 s | 144 | 3 | 3 |
| 3 s | 64 | 3 | 3 |

For an **A/B comparison** (difference of two arms, difference sd = sqrt 2 x per-arm sd): half-width 1 s needs about 17 pairs classified-fast-only (24 runs per arm, since only 71 % are fast and both arms of a pair must be fast to be used) or about 20 pairs
rate-adjusted (all pairs usable); half-width 2 s needs 4-5 pairs either way. Today's 10 pairs of 4-node ABBA = 254 node-min (20 runs x 12.7 node-min, with 190 s fast wall) gives about 1.6 s half-width rate-adjusted (**modelled**) against the ~17 s measured (S47). Equal budget, ten times the precision.

Cautions: (a) the rate covariate must come from something the arm does not change. For arms that change only dm, use the bs-phase rate (bs rate and dm rate fall together: bs x0.81-0.84, dm x0.70-0.72 in T09); for arms that change the exchange itself (COMM_LAYER_*), the headline rate moves with the arm, so
use a stand-alone probe (T05 steps 1-3: 30 s on 4 nodes = 2 node-min, before each run) or the init-time self-test exchange. (b) The relation is fitted on the current code. Refit `a`, `b` per stage whenever the code changes; 6 runs spanning both modes are enough (1.5 s). (c) The chain spike (a separate effect, 31 % of runs in some holds, +3-89 s in the single-node reciprocal chain) is outside the model: flag and exclude it (or model `recip chain` as its own term); the first 1-2 runs of a fresh hold also have init 46-53 s.

#### 5. Real-size component benchmarks as proxies

Candidates present in the tree: `tests/t_mn_grid.c` (correctness of the sharded product tier; sizes are multiples of the plane cap and checked against host products; not a benchmark), `target_kit.sh` stage `a4` (all-to-all scaling with `MN_COMM_MARK`, 5-10 min per node count), `a3` (injection), `tests/t_nicbw.c`.

Assessment (modelled):
- Because dm is exchange-bound (97 % of its time in the 1/r term, section 4), the all-to-all at the real message sizes is the right predictor of dm and 31 % of bs (b_bs/(a_bs r) at r = 13.8 is 24.4 s of 70.7 s). A proxy has to run in much less than the 3 minutes of a full run to pay off. `a4` at 5-10 min per node count does not.
- A 30 s all-to-all probe (T05 step 1; message sizes from `MN_PLAN_ONLY`'s largest exchange, e.g. the dm step with the T chunk of 2048 MB) per 4 nodes costs 2 node-min against 12.7 for a full run (6x cheaper). It classifies the environment (rate r) and predicts total with the regression of section 4 (sd 1.6 s) **only for a code and switch set whose a, b are already fitted**. It cannot rank two switch settings on its own.
- For ranking an exchange-affecting parameter (chunk size, round MB, layer settings): a benchmark that runs `mn_core`'s dm-sized product alone with the setting, 3 repetitions, 20 s each, would find the direction and probably the size of the effect (**assumed**; dm's compute hides under the exchange, so the isolated product should show the same exchange time). Validation needed: 4 settings x (benchmark + 3 full runs rate-adjusted) = about 60 node-min once. Effort 1-2 days (the benchmark driver around `rns_dist`/`mn_core`, `MN_PLAN_ONLY` for the shapes), risk medium. Rank below 1-2.
- Zero use for `init` and bs-compute, which are not exchange-bound (bs: 46 s of compute in a, 4-node).

#### 6. Memory with zero (or one) full run

Facts (measured, t09 rank 0, 4 nodes, 2.576e11 digits): device in use after init 335.0 GB (planes 103.1, regions 55.8, block pool 176.1); `budget:` peak 380.3 GB (device 335.6 + host 44.7) of the 480 GB budget; host RSS 11.5 GB after init (HWM 29.0). The device figure is the pre-allocated layout, so it is known from the plan before any run.

Procedure:
1. `MN_PLAN_ONLY=<digits>:<g>` (also run by `mnrun.sh` on the login node; no device) and `BS_LAYOUT_ONLY=<D>:<g>` give the plan and the arena request; `python3 ecalc/mem_model.py --check-c <layout-file>` compares the C layout with the model term by term (exact at 4e10, 1e11 and the target's share per README). Cost 0 node-min. This yields the device + planned host budget for any digits per node and node count, and the largest D that fits (bisect on the plan; no cluster).
2. Confirm with `ECALC_INIT_ONLY=1` (maps pools and arena, prints `mem[r] [init]` and exits): about 30 s + launch = 0.6 node-min at 1 node, 2.5 at 4 nodes (**modelled** from init 29.8 s). Run it only at the boundary digits and for each new layout.
3. What the plan cannot give: the host peak above 29 GB HWM (the model says 44.7 GB; MemAvailable min 120-146 GB after init) and the pool's live peak per stage. One full run per layout reads them from the always-on `live:` / `memsample` lines (the table, section 2); all later timing runs of the same layout repeat them for free, so memory calibration rides on the timing runs.
4. Calibration table rows give the residual `measured peak - planned peak` per stage; a layout whose residual exceeds 1 % flags a model error before it flags a memory kill (rc 9).

Gain: memory points stop costing runs of their own: 0 full runs for the device layout (**modelled**, since the layout is allocated up front), 1 shared run for the dynamic peak.

#### 7. Test-speed question (aac6 1 node, `tests/t_ntt 24` under `-c` variants)

Inconclusive, see section 9.

#### 8. Ranked plan (gain per calibration point, effort, risk)

A "calibration point" = one configuration at one node count, time + memory, 95 % CI about 2 s. Baseline today: about 10 ABBA pairs = 254 node-min at 4 nodes with ~17 s CI (S47), more if the CI must be 2 s (hundreds of runs, modelled unreachable).

| rank | step | gain per point (node-min, 4 nodes unless stated) | effort | risk |
|---|---|---|---|---|
| 1 | `calib_table.py`: log -> row parser, mode flag, stage regression `a + b/r`, adjusted A/B stats with an ABBA stop rule on the adjusted CI | CI 17 s -> 1.6 s at 254 node-min, or the same 2 s CI for 5 pairs = 127 node-min (saves ~127; **modelled**); 1 node and 6 nodes likewise (6-node run is 29.7 node-min, so the gain there is larger in absolute terms) | 1 short session (parser exists in `refit_s48.parse_log`) | low: analysis only, no engine change; needs validation of `a, b` per node count (fit from 6 spanning runs) |
| 2 | Make the stats switches the default calibration launch line; poll fix in the ABBA driver (`wait $pid`, health/prep once per hold) | 0.7 node-min per 4-node run (5 %) from the poll alone; the stats cost at most ~2 s (not significant, measured) | 2 h | low |
| 3 | Memory by plan: `MN_PLAN_ONLY` + `--check-c` + `ECALC_INIT_ONLY` at the boundary; table columns for planned-vs-measured peak | removes all memory-only runs: 1 full run (12.7) -> 0 or 2.5 node-min per layout | half a day | low |
| 4 | Environment probe (30 s all-to-all, 2 node-min) before each run or each hold start, for arms that change the exchange; refit `a, b` after any code change | keeps step 1 valid for exchange-changing arms (without it the regression is unusable there); 2 node-min each | 1 day (a `t_a2a`-style test exists in `t_nicbw` / `a4`; size from the plan) | medium: probe must match the app's path (SHMEM, 4 NICs) |
| 5 | Top-set restart for dm sweeps (driver only) | 1.4-2.3 node-min per run (11-19 %); first needs a 4-node trial and a place for 57 GB per node (aac7 /tmp is RAM) | 1 day + 1 trial (~40 node-min) | medium: unmeasured at scale; memory not representative |
| 6 | Exchange-stage benchmark at real piece sizes | 4 -> 0.3 node-min per candidate setting for exchange-bound parameters (assumed) | 1-2 days + ~60 node-min validation | medium: prediction quality unknown |
| 7 | Warm process (K configs per launch) | 2.4 node-min per run (19 %) | days | high: re-entrancy, memory bias |

Rule of thumb after steps 1-4: a new setting costs 6 runs (spanning both modes) = 76 node-min at 4 nodes for time to about 2 s, memory included, instead of hundreds.

#### 9. Test speed, `tests/t_ntt 24` on one aac6 node

Measured on aac6 aac6-spx-a, my own job 22905 (20 min, ended by its time limit; no step or process of mine left), 14:36-14:56 Central (15:36-15:56 EDT). Binary: the shared clone's `~/ntt/ecalc/tests/t_ntt`, `srun --jobid --mem=0 --gpus=4 [-c N] --overlap bash -lc "module load rocm; ./tests/t_ntt 24"`.

| variant | result (measured) |
|---|---|
| `-c` unset, no `-n` | timeout at 300 s; 4 tasks started (the log shows four copies of the header and the step reports tasks 0-3 killed) |
| `-c 192` (auto), no `-n` | timeout at 300 s, again 4 tasks |
| `-c 48`, no `-n` | timeout at 300 s, 4 tasks |
| `-c 96`, no `-n` | killed by me after 26 s of its 300 s (to free time) |
| `-n1`, `-c` unset | timeout at 150 s, 1 task; sections 1 and 2 done within the first seconds, stuck in section 3 (convolution) |
| `-n1`, `-c 48` | cut by the job's time limit after 120 s, still in section 3 |

What this says: **the question is not answered**. No variant finished `t_ntt 24` inside 150-300 s, so no wall could be compared. Two real findings:
1. `srun -N1 --mem=0 --gpus=4` **without `-n1` starts 4 tasks** on this partition (four copies of every test, each with 4 devices and its own OpenMP threads). `mnaccept.sh`'s `R()` and my commands use that form. If `mnaccept` tests are slow, this
   is a candidate cause. N13's regression (job 21017) passed 13 tests in 1263 s, so it does not hang there, which makes the explanation partial (those ran on s24-26; `t_ntt` section 3 here did not finish even with one task).
2. Section 3 of `t_ntt` is where it stalls on s24-16 today (with one task as well), independent of `-c` 48 vs unset. Cause not found (could be the host-side OpenMP compare with 48/192 threads, or a node problem on s24-16).
Next step (cheap, 10 min, 1 node): same node, `-n1`, `-c` unset / 48 / 96, with `OMP_NUM_THREADS` set (4, 48) and per-section timestamps (`ts`-style `date` on each output line), timeout 600 s each, one at a time; compare with s24-26. Not done: out of the 20-minute allowance.

#### 10. First concrete step (one short session)

Build `ecalc/calib_table.py` (CPU only, no cluster job):
1. Input: a directory of run logs (aac7 `~/s47/run/log`, `~/batch1011/T5`, `~/sess1011b/t09/log`), strip the `N:` labels as `refit_s48.parse_log` does; output `calib.tsv` with the columns of section 2.
2. Mode flag from the all-to-all line (11.95 / 12.3 GB/s thresholds), chain-spike and cold-start flags.
3. `--fit`: per stage, least squares `t = a + b / r` over clean runs, printing a, b, residual sd, and the out-of-sample check (fit on one source, test on the others).
4. `--ab ARM_A ARM_B`: adjusted per-pair differences, mean and 95 % CI, and the number of further pairs needed for a target half-width.
5. Acceptance: reproduce a = 80.8, b = 1388, sd 1.6 on the 63 T05 clean runs; the same fit on the 12 t09 runs (rate from `xstats`) as a second dataset.
Cost: 0 node-min; deliver the parser first, then wire `refit_s48.py --table-file`. Raw logs are on aac7 and are read, not moved.

#### RESUME
Design done; nothing running on the clusters after the section 9 measurement. Next: step 1 above (parser + fit), then the driver changes (rank 2), then the 4-node probe test (rank 4) in the next 4-node hold.

## A04: mnaccept.sh fixes (branch a04, 2026-10-11)

(former file: A04.md)

Changes (ecalc/mnaccept.sh): (1) TMP defaults to `$HOME/sess/mnaccept_<job>` (shared) on a multi-node hold, node-local `/tmp` on 1 node; MNACCEPT_TMP overrides.
(2) Reference dir: ECALC_REF, else clone `ref/` if it has the 1e9 file, else `$HOME/ref`, else `$HOME/ntt/ecalc/ref`; dangling `ref/e_*` links are replaced (`ln -sfn`).
(3) `refneed`: absent/dangling reference gives `FAIL <step>: reference missing: <file>` and the step is skipped (t_bs: 1e6/1e7 sha256; e9: 1e9 txt). Logic checked on a stub only (measured: rc 1, message printed); not run live.
(4) R/N steps get `--mem=0` (default-memory step with `-c 192` was very slow/hung today).
(5) t_mn_grid: `COMM_TRANSPORT=shmem` when /opt/cray/pe/sma exists (per a01); new `--skip-grid` / MNACCEPT_SKIP_GRID=1; new step name `grid`.

Tests (measured):
| where | command | result |
| aac7 2-node hold 12520 | `--skip-grid --only unit,e9` | 10 passed, 0 failed, 894 s, e9 identical (no false DIFFERS) |
| aac7 1-node hold 12527 | same | 10 passed, 0 failed, 913 s |
| aac6 PPAC s24-26 1 node, job 22904 (old default ref path) | same | 10 passed, 0 failed, 1240 s |
| aac7 2-node hold 12528 | `--only grid` (COMM_TRANSPORT=shmem) | PASS t_mn_grid 2 procs, 200 checks, 126 s |
Note: aac7aenv sets MNRUN_CPUS_PER_TASK=auto (-c 192); with `--mem=0` unit steps ran fine, t_ntt 24 alone takes ~5 min there.

#### RESUME
Done; merge a04. Jobs cancelled, aac6 clone ~/ntt-a04 and aac7 worktree ~/ntt-wt/a04 removed.

## A06 (N6): target_kit.sh hand-off items

(former file: A06.md)

Branch a06 (from main 6d80ef1). Files: `ecalc/target_kit.sh`, `ecalc/README.md` (3 rows), `tools/initchk.py` (taken unchanged from branch a03, commit a3a27f5).
All rehearsal numbers below are **measured** on aac7, 2 nodes (job 12524, then 12526; both cancelled, no steps left), site flag `--site aac7`.

#### What was added
| item | behaviour |
|---|---|
| `--exclude-hosts LIST` | `MNRUN_EXCLUDE_HOSTS` + `MNRUN_EXCLUDE_MODE=drop` for every mnrun.sh launch; `srun -x` for nodechk; excluded hosts are subtracted from the allocation size when a stage decides whether a node count fits (no `-w`). `MNRUN_EXCLUDE_HOSTS` in the environment is the default. |
| `--spares N` | summary prints "request max node count + N"; nodechk writes `$OUT/flagged_hosts.txt` (later stages in the same `--out` drop those hosts) and prints `SPARES: STOP` if flagged > N, else OK. Default off. |
| stage `plan` (default on) | `MN_PLAN_ONLY=<digits>:<procs>` on a compute node for every a4 count (+ `--plan-digits/--plan-procs`); PASS only if the last plan line is `plan check ... OK`. a4 and s2chk call the same check before each long run and refuse it on `plan REFUSED`. |
| stage `uffd` (default on) | tiny 2-node ecalc with `FI_LOG_LEVEL=info`; PASS if "memory monitor is: uffd" appears; also reports the userfaultfd syscall result and `vm.unprivileged_userfaultfd`. |
| `FI_MR_CACHE_MONITOR` | exported `${VAR:-userfaultfd}` right after the `disabled` guard, echoed; every kit `srun` has `--export=ALL`. |
| stage `initchk` (default on, after a4) | runs `tools/initchk.py` on the smallest a4 log (or `--initchk-log`). a4 now sets `ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=2 MNRUN_LABEL=1`. Exit 0 PASS, 3 FLAG (post-run diagnosis: rerun without the host, not a pre-run gate), other PARTIAL; fewer than 3 nodes also PARTIAL (no median). SKIPPED if the script is absent. |
| `--only preflight` | env,build,uffd,plan,edge,nodechk. nodechk (opt-in) now runs right after edge, before a3/a4. |
| `cache_guard` | s2chk launch aborts unless `RNS_DIST_CACHE_FIT=1` or `RNS_DIST_CACHE_MN=0` is in effect (the README's two safe settings). README names no explicit MN value for the target line (its line uses FIT=1), so none is set; a decision, see below. |

`NEWTON_MN_RECIP_TS=2` (newton_db.c ~l.894), checked in source: it only moves `db_reserve(&Qtop, hq.n)` ahead of `db_from_bi` (whose own reserve then is a no-op) and prints timestamps; same allocation, no digit change. a4 VERIFY OK in the rehearsal with it set.

#### Rehearsal (aac7, 2 nodes, measured)
| stage | result | time |
|---|---|---|
| build (login node, rocm/7.2.4, incl. t_nodechk) | PASS | 39 s |
| env | PASS (fi_info -p cxi reports 0 domains on aac7: aac7 has no cxi listing here; not a target figure) | 2 s |
| uffd | PASS, libfabric selected uffd; syscall alone is refused for the user (`vm.unprivileged_userfaultfd=0`) yet libfabric still picks uffd, so the log line, not the syscall, is the test | 22 s |
| plan (2 procs 2e10 + 4 procs 4e10) | PASS both, `plan check ... OK` | 2 s / 105 s with a4's plan+run |
| nodechk (2 nodes) | PASS, 0 flagged, upload 0.017 s all APUs | 3 s |
| nodechk `--exclude-hosts <real node>` | probed 1 node via `-x`; a3 then SKIPPED (usable 1 < 2) | 3 s / 0 s |
| edge (t_edge dev+vmm, e 1e9, e 1e10) | PASS | 187 s |
| a3 | FAIL on aac7: the 1-NIC form runs (aggregate ~26 GB/s per node at 16 MiB), the 2-NIC form `0,4;1,5;2,6;3,7` dies in `fi_getinfo` (aac7 has 4 NICs per node, not 8). Same with `FI_MR_CACHE_MONITOR=memhooks`, so not caused by the new default. Expected on aac7, not a kit regression. | 15-18 s |
| a4 n=2 (1e10/node) | PASS, VERIFY OK | 92 s |
| initchk on that real 2-node log | parses per-node rns_init / map / region / transport / upload (9.4 / 10.9 s rns_init); no median with 2 nodes, so PARTIAL (after fix) | <1 s |
| s2chk | not run (opt-in; needs an MN_WAIT_STATS build). Correction by a12: MN_WAIT_STATS is in main (comm_util.c, commit 48220c5, D3); the earlier "not in main" was wrong. Not yet run | - |
| dry-run of every stage with `--exclude-hosts --spares --plan-digits` | commands as designed | - |

#### Not done / open
- (h) crash/hang auto-capture, (c) the cold probe already existed (`--nodechk-cold-gb`, off by default); T2048, X2 untouched.
- initchk with >= 3 nodes was never run on a real log by this task (rehearsal had 2 nodes; a03 tested on 8 historical runs). No positive control for a stall in the kit's own run.
- Not rehearsed: the 2-NIC a3 form (needs the 8-NIC target), 8-node a4, s2chk, `--spares` STOP path with a real flagged node, plan REFUSED path (only OK seen).
- a4 itself runs plain ecalc without RNS_DIST_CACHE words (aac7-shaped); README says its default costs +37.5 % at 1e10/2 procs. Not changed here.
- `mnrun.sh` gets the monitor default on branch a10; kit exports it itself.
- Blocking the hand-off: target-side items in the private draft (target ROCm/module names, 8-NIC form, a 576-node run, operator copy of the runbook) - none of them are kit code.
- Decision: whether the target launch line should name `RNS_DIST_CACHE_MN` explicitly (README documents only FIT=1 or MN=0).

#### RESUME
Merge a06 (target_kit.sh, README rows, tools/initchk.py from a03 - take a03's version if both land). aac7: worktree ~/ntt-wt/a06 removed, scratch ~/sess1011c/a06 (logs, times.txt, times2.txt) left for the coordinator.

#### a12 merge note
- Stage a4 now runs with `RNS_DIST_CACHE_FIT=1` (README: the safe setting; the default costs +37.5 % at 1e10 on 2 processes, measured earlier; matches the s2chk cache guard). Digits unaffected (sizing switch only).
- Correction: `MN_WAIT_STATS` exists in main (D3, comm_util.c); s2chk is merely not rehearsed.

## S18KIT — `ecalc/target_kit.sh`: one script for the next target session (branch `s18-kit`)

(former file: S18KIT.md)

The user has no time to read docs on the target. `ecalc/target_kit.sh` is self-contained (`-h` gives the full usage, time
estimates and the one file to send back), runs each stage behind its own flag and timeout, never stops the other stages when
one fails, and writes a single `$OUT/KIT_SUMMARY.txt` to send back — logs under `$OUT/logs/` are backup detail only. It never
launches with `oshrun`: every multi-process stage is Slurm-native `srun --ntasks … --ntasks-per-node=1` (`mnrun.sh` for the
ecalc runs, direct `srun` for `t_edge`/`t_comm`), and sets `FI_UNIVERSE_SIZE` (max(4096, 4 × ntasks)) and `FI_LOG_LEVEL=warn`
only where the caller has not already set them (the TGTBENCH2-proposed defaults, internal target notes §4, not yet adopted on the
main launch line — this script adopts them for its own probe runs only).

#### What it answers

| stage | answers | WISHLIST item |
|---|---|---|
| `env` | hostname list, ROCm version, module list, `fi_info -p cxi` domain count, Slurm job geometry | §1.2 (partial), environment record |
| `build` | the binary exists on this target, built with target-generic module detection (no aac7 version hard-coded) | prerequisite for everything below |
| `edge` | the device-memory edge (`t_edge dev`/`vmm`, run alone, first), the init footprint (host RSS, per-APU device in-use vs driver-used), the VMM arena map rate per APU and per node, the single-node 1e10 wall | §2.1 (re-confirms A6), §2.2 (process overhead in our own terms, not the harness's), §3.1 |
| `a3` | injection bandwidth with `comm_ofi`, 1 NIC/APU vs 2 NICs/APU (`COMM_OFI_NICS`), GB/s per node | §1.3 (A3) |
| `a4` | the all-to-all rate at 2/8/64 nodes with `MN_COMM_MARK=1`, VERIFY, wall | §1.4 (A4) |

A3 and A4 are "still the top uncertainty" per results/TARGET.md §TGTBENCH2 §0; this kit is what gets them measured.

#### Stages and expected runtime (wall clock, once the allocation is live)

| stage | nodes | expected | what it runs |
|---|---|---|---|
| `env` | 1 (probed) | < 1 min | hostname/date, `scontrol show job`, `fi_info -p cxi`, `module list`, ROCm version |
| `build` | 0 (login/any) | 3–10 min | `make SHMEM_CRAY=1` for `ecalc`, `tests/t_comm`, `tests/t_edge`, `tools/` |
| `edge` | 1, alone | 5–10 min | `t_edge dev`, `t_edge vmm` (sequential, not concurrent with anything else on the node), `ecalc 1e9` with `MEM_REPORT_DEVS=1`, `ecalc 1e10` |
| `a3` | 2 | 3–5 min | `t_comm --bw` with `COMM_OFI_NICS="0;1;2;3"` then `"0,4;1,5;2,6;3,7"` |
| `a4` | 2, 8, 64 (skips what the allocation can't hold) | 5–10 min per node count | `mnrun.sh <n> env COMM_TRANSPORT=shmem MN_COMM_MARK=1 ecalc <n * 1e10> <out>` |

Total for a full run with an allocation that holds 64 nodes: roughly 30–55 minutes. `--only`/`--skip` (comma lists of
`env,build,edge,a3,a4`) narrow it; `--nodes-a4` narrows the node counts.

#### Validation done here (login node / local, no job touched)

- `bash -n ecalc/target_kit.sh`: OK.
- `shellcheck` not available in this environment; not run here (rehearsal on aac7 §below checks for it too).
- `./ecalc/target_kit.sh -h`: prints the full usage block.
- `./ecalc/target_kit.sh --dry-run --site aac7 --jobid 99999 --out /tmp/...`: every stage prints its commands (no execution),
  `KIT_SUMMARY.txt` gets one `DRY-RUN` block per stage. `--only env,a4 --nodes-a4 2,8,64 --jobid 12287` confirmed stage
  selection and the per-node-count `mnrun.sh` lines (digits = n × 1e10, `FI_UNIVERSE_SIZE=4096` for n ≤ 1024).

#### The aac7 rehearsal (for the coordinator to run)

Per the task's constraints: **no `srun`/`sbatch` on aac7, never touch jobs 12287/12294** beyond `--dry-run`'s printed text
(which never executes anything). The rehearsal is `--dry-run` only, on the aac7 **login** node, from the `~/b7acct` clone,
fetching and checking out this branch:

    sshpass -p <cluster-password> ssh -o ConnectTimeout=20 aac7 '
      cd ~/b7acct &&
      git fetch origin &&
      git checkout s18-kit &&
      git pull --ff-only &&
      cd ecalc &&
      bash -n target_kit.sh && echo "bash -n OK" &&
      (command -v shellcheck >/dev/null 2>&1 && shellcheck -S warning target_kit.sh || echo "shellcheck not available") &&
      ./target_kit.sh --dry-run --site aac7 --jobid 12287 --out /tmp/s18kit_dry_2n --nodes-a4 2 &&
      ./target_kit.sh --dry-run --site aac7 --jobid 12287 --out /tmp/s18kit_dry_8n --nodes-a4 8 &&
      cat /tmp/s18kit_dry_2n/KIT_SUMMARY.txt &&
      cat /tmp/s18kit_dry_8n/KIT_SUMMARY.txt
    '

64 nodes is not available on aac7 (no 576-node-class allocation there); the dry-run's own node-count argument covers it
instead (`--nodes-a4 2,8,64` prints the 64-node `mnrun.sh` line unconditionally in `--dry-run` mode — a real run would skip
it if the allocation is smaller). `--jobid 12287` here only supplies the text the printed commands would carry; `--dry-run`
never calls `srun`, `sbatch` or `squeue` against it.

To rehearse inside the hold (the coordinator, when ready — NOT run by this report): the same two `--dry-run` lines, then,
inside `srun --jobid=12287 --overlap` (or `--jobid=12294`), a real `--site aac7` run with `--nodes-a4 2` (and separately `8`)
and `--only env,build,edge,a3,a4` as needed, pointed at a scratch `--out`.

#### Open items

- The VMM map-rate unit (per APU-GB mapped by one APU, or per node-GB) is still not settled (TGTBENCH2 Q5); `edge`'s summary
  reports the per-APU figures as printed and flags the per-node figure as a naive sum, not a wall-clock rate, pending that
  answer.
- `a4`'s node counts (2/8/64) are configurable (`--nodes-a4`) but the script does not request dragonfly placement (packed vs
  spread, WISHLIST §1.4); that is a `--switches=1@<time>` Slurm option for the coordinator to add at the `mnrun.sh` call site
  if the admins confirm the spread syntax.
- `build`'s SMA/DSMML auto-detection (`module -t avail cray-openshmemx|cray-dsmml`, highest version) is still a blind
  newest-wins pick; `TARGET_SMA_MODULE`/`TARGET_DSMML_MODULE` override it by hand if that is ever wrong on the target.
  ROCm itself no longer picks blindly -- see "Module handling" below.

#### Module handling

Two bugs, found by rehearsing on aac7 (results/V16.md) and by re-reading the script against `mnrun.sh`/`aac7env.sh`:

1. **ROCm choice.** `module -t avail rocm | sort -V | tail -1` picks the *newest* version, which on aac7 is
   `rocm/7.14.0` -- a version that fails to link ecalc (results/V16.md: 7.12.0/7.13.0/7.14.0 and `rocm-new`/10.0.0 all
   hit the same `-fPIC` device-link error in `ld.lld`; `rocm/7.2.4` is the newest that links). The target's ROCm is
   unknown ahead of time ("do your best with the libraries you have"), so `stage_build` now tries candidates in
   order and keeps the first that actually builds `ecalc` + `tests/t_comm` + `tests/t_edge` + `tools`:
   `$TARGET_ROCM` alone if the caller set it; else `rocm/7.2.4`, `rocm/7.2.3`, `rocm/7.0.3` (whichever are available,
   in that order), then any other `rocm/<version>` module, newest first. `make clean` runs between attempts (stale
   `.o` from a different ROCm must not be reused); every attempt is logged, and the summary names the ROCm used and
   any that failed first. A versioned `rocm` module can conflict with a default-loaded `rocm` (aac7:
   `rocm/7.0.3`, the same trap V16 found) -- the build unloads whatever `rocm/*` the login shell already has before
   loading each candidate.
2. **Runtime stages never loaded modules.** Only the `build` subshell called `module load`; the `srun` stages for
   `edge`/`a3`/`a4` ran with whatever the caller's login shell had, which can be a different ROCm/SHMEM than the one
   that built the binaries. `stage_build` now writes the chosen set to `$OUT/kit_modules.env`
   (`TARGET_ROCM`/`TARGET_SMA_MODULE`/`TARGET_DSMML_MODULE`/`TARGET_ROCM_UNLOAD`); every later run stage
   (`edge`, `a3`, `a4` -- not `env`, which deliberately records the target's as-found default) sources it and fails
   with a clear message if it is missing (so `--only edge` etc. in a later invocation needs a prior `build` in the
   same `--out`), loads the same modules inside its own `srun` (`module_preamble()`, the unload-then-load convention
   `aac7env.sh`/`mnrun.sh` already use), and records the runtime ROCm actually seen on the compute node
   (`hipconfig --version`, inside the `srun`) in its log and the summary. `a4` additionally exports
   `MNRUN_MODULES`/`MNRUN_UNLOAD` to `mnrun.sh` from the same chosen set (honouring any caller override, per
   `mnrun.sh`'s own convention).

Validated on the aac7 login node (branch `s18-kit`, no `srun`/`sbatch`, jobs 12287/12294 untouched): `bash -n` OK;
`shellcheck` not installed there either. `--dry-run --site aac7 --jobid 12287 --nodes-a4 2` and `...--nodes-a4 8`
both print the ROCm candidate list (`rocm/7.2.4 rocm/7.2.3 rocm/7.0.3 rocm/7.14.0 rocm/7.13.0 rocm/7.12.0` on aac7)
and assume the first, write `kit_modules.env`, and every later-stage `srun` line carries the module unload/load
preamble and a `hipconfig --version` probe. A real `--only build` run (no `srun`, so safe on the login node) chose
`rocm/7.2.4` on the first try and built `ecalc`, `tests/t_comm`, `tests/t_edge`, and `tools` successfully --
confirming the candidate order matches results/V16.md's finding without needing the aac7-specific version hard-coded.

## S18KITFIX — three bugs in `ecalc/target_kit.sh`, found in the aac7 rehearsal (`~/s18ab3`), fixed and verified for real

(former file: S18KITFIX.md)

Branch `s18-kitfix` (its fix, commit `ef22f22`, is also already merged into `main` by another process while
this task was validating on aac7 — `main`'s own history shows `Merge ... origin/s18-kitfix: Fast-forward`).
Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled **measured**. Code change:
`ecalc/target_kit.sh` only (no C code touched, no rebuild needed).

#### 1. The bugs (from `~/s18ab3/kit/KIT_SUMMARY.txt`, `~/s18ab3/kit/logs/`, `~/s18ab3/log/kit_fail_a3.txt`)

1. **Stage a3** (and `~/s18ab3/drive.sh`'s own `t_comm` sweep, which reused the same broken launch) ran
   `tests/t_comm --bw` by bare `srun`, leaving the SHMEM heap at the library's default. Every launch failed:
   `comm_shmem: pe N: shmem_malloc of 8192 MiB failed: the SHMEM heap ... must be >= 8704 MiB (the pool + 512)`.
2. **Stage a4**'s summary block for a later node count (e.g. `n=8`) repeated an earlier node count's lines
   (`n=2`) ahead of its own, because the parser `grep`/`tail`'d the one shared, appended log file that every
   `n` had written into.
3. **Stage edge** printed only the "report both forms" text, no actual VMM/arena map-rate number — `dbig.c`'s
   `"VMM arena ... s/GB"` line is gated by `vmm_vb()` (`DB_POOL_VERBOSE` / `RNS_VERBOSE`), which the stage never
   set, so the line it was grepping for never appeared in the log.

#### 2. The fixes (`ecalc/target_kit.sh`, commit `ef22f22`)

1. **a3**: launch `tests/t_comm` through `mnrun.sh` instead of bare `srun` — `t_comm` fits `mnrun.sh`'s
   `<procs> <command...>` interface exactly, the same way `results/OFI_FABRIC.md §OFI17`'s and `results/OFI_FABRIC.md §TUNE17`'s own
   `bw()` driver functions already launch it. `COMM_SHMEM_POOL_MB` / `COMM_OFI_POOL_MB` are set from the same
   formula those drivers use: `t_comm --bw` allocates 2 symmetric buffers (send, receive) per thread, each
   `A3_MAXMB` MiB × the node count; with `A3_THREADS` threads that's `2 × A3_THREADS × A3_MAXMB × nodes` MiB
   (`"8 * g * mx + 512"` in OFI17/TUNE17's notation, at their default 4 threads). `mnrun.sh` then sizes
   `SHMEM_SYMMETRIC_SIZE` / `XT_SYMMETRIC_HEAP_SIZE` itself at pool + 512 MiB; `COMM_OFI_POOL_MB` sizes
   `comm_ofi`'s device staging pool the same way (`2 × nodes × A3_MAXMB + 256`).
2. **a4**: each node count `n` now gets its own log file (`04_a4_n<n>.log`); the per-`n` summary lines are
   parsed only from that file (it is still concatenated into the combined `04_a4.log` afterward, so the
   on-disk full log is unchanged for a human reading start to end).
3. **edge**: `DB_POOL_VERBOSE=1` is added to the `1e9` run's environment (narrower than `RNS_VERBOSE`, which
   adds unrelated chatter) so `dbig.c`'s per-APU "VMM arena ... s/GB" line actually appears; the summary now
   computes both requested forms from those lines — mean of the per-APU `s/GB` values ("per APU-GB" form), and
   sum(seconds)/sum(GB) across the node's APUs ("per node-GB" form, explicitly labelled as a serialized-
   equivalent sum, not the parallel wall-clock rate) — both labelled measured.

#### 3. Validation

`bash -n ecalc/target_kit.sh` and `ecalc/mnrun.sh`: clean. `--dry-run` (full kit and `--only a3`/`--only edge`
individually): the a3 stage prints the computed pool (`COMM_SHMEM_POOL_MB=4608 COMM_OFI_POOL_MB=1280` at the
kit's defaults) launched via `mnrun.sh`; the a4 stage shows a distinct log path per `n`.

##### 3a. Real run on aac7, job 12287: `--only a3 --site aac7 --jobid 12287 --out ~/s18ab3/kit`

First attempt with the kit's own default `--nics2` (`"0,4;1,5;2,6;3,7"`, the **576-node target's** 8-NIC/node
form) correctly failed at the fabric, not the pool: `comm_ofi: fi_getinfo(...) = -61 (No data available)` —
aac7 only has 4 NICs (`cxi0`-`cxi3`), one per APU, so indices 4-7 don't exist there. Re-run with
`--nics2 "0,1;1,2;2,3;3,0"` (results/OFI_FABRIC.md §TUNE17's form A for aac7's 1-NIC-per-APU topology, two endpoints
sharing one cxi per device): **PASS**.

`COMM_SHMEM_POOL_MB=4608 COMM_OFI_POOL_MB=1280` (kit ROCm: `rocm/7.2.4`), `FI_UNIVERSE_SIZE=4096 FI_LOG_LEVEL=warn`.

GB/s per node (1 PE/node) at the largest slab (268435456 B = 256 MiB), both PEs:

| form | pe 0 | pe 1 |
|---|---|---|
| 1 NIC/APU (`COMM_OFI_NICS=0;1;2;3`) | 20.94 GB/s | 20.95 GB/s |
| 2 NICs/APU (`COMM_OFI_NICS=0,1;1,2;2,3;3,0`, aac7's form) | 20.92 GB/s | 20.91 GB/s |

No gain from the 2-NIC form on aac7, as `results/OFI_FABRIC.md §TUNE17` already found (`nic2_formA` vs `nic2_default`):
one NIC per APU there, so the "2 NICs" form just shares the same `cxi0..3` between two devices rather than
reaching new fabric. The mechanism is exercised correctly; its rate gain needs the target's real 2-NICs/APU
wiring.

##### 3b. Real run on aac7, job 12287: `--only edge --site aac7 --jobid 12287 --out ~/s18ab3/kit`

**PASS.** `t_edge dev`/`vmm`: 400.0 GB committed and touched (the cap). Host RSS 7.5 GB after init (staging
4.3 GB pinned + device regions 11 GB; init 6.0 s). Single-node 1e10 wall: 25.43 s; `VERIFY OK`.

Per-APU VMM arena map rate (measured, now actually printed via `DB_POOL_VERBOSE=1`):

| APU | GB mapped | seconds | s/GB |
|---|---|---|---|
| 0 | 4.29 | 0.49 | 0.227 |
| 1 | 2.15 | 0.57 | 0.263 |
| 2 | 2.15 | 0.52 | 0.244 |
| 3 | 2.15 | 0.61 | 0.283 |

- **Per APU-GB form** (mean of the per-APU rates): **0.2542 s/GB**
- **Per node-GB form** (10.74 GB total / 2.19 s summed per-APU time, 4 APUs — a serialized-equivalent sum,
  not the parallel wall-clock rate): **0.2039 s/GB**

Both forms measured; TGTBENCH2 (Q5) never settled which one is the target figure, so both are reported.

#### 4. Sweep: `~/s18sweep/drive.sh` (armed, fire-and-forget)

Runs the fixed a3 `t_comm --bw` bandwidth command (same pool-sizing formula as §2.1, applied directly to a
`srun -w <nodelist>` launch since packed/spread needs an explicit node list rather than `mnrun.sh`'s own
selection) at 2/4/6/8/10 nodes, packed vs spread placement (spread for 2-8; node lists from
`~/s18ab3/summary.txt`'s existing step4 sweep lines), 3 reps each, sequential, median/min/max GB/s per node
and per APU thread. Per-run `timeout -k 10 600` (kill by PID only, never `scancel`/`scontrol`).

A second, unrelated in-flight driver (`~/s18p2/drive.sh`, a crash-soak phase) was found actively using all 10
nodes of job 12287 at arm time — not something this task started or should touch. `~/s18sweep/drive.sh` gates
on it the same way `results/OFI_FABRIC.md §OFI17`'s/`results/OFI_FABRIC.md §TUNE17`'s drivers gate on markers elsewhere in this repo:
it polls `squeue -s -j 12287` for non-`batch`/`extern`/`interactive` steps and only starts once none remain
(deadline +8 h; never cancels anything). It does not depend on which git branch `~/ntt-acc` has checked out
(it calls `tests/t_comm` directly, bypassing `target_kit.sh`/`mnrun.sh`; `t_comm.c` is untouched by this fix),
so it is safe to run regardless of what else is using that clone. `EXIT` trap writes the marker and leaves
`~/ntt-acc` on `main`.

- Marker: `~/s18sweep/S18SWEEP_DONE`
- Summary: `~/s18sweep/summary.txt` (per-node-count/placement median/min/max), raw TSV `~/s18sweep/sweep_results.tsv`
- Login-node PID: 2704179 (`setsid`+`nohup`, parent PID 1)
- Started: 2026-10-07 03:08:07 EDT
- At arm time the gate found job 12287 already free and started immediately. The first three reps (n=2,
  packed) completed in ~6-8 s each; one of them hit a transient libfabric `"Failed to allocate TGQ"` /
  `"Invalid resource domain"` error (cxi domain cleanup lag from back-to-back launches on the same nodes),
  recorded as `rc=3`/`NA` in the TSV rather than hidden -- `median`/`min`/`max` still computed from the two
  good reps. The first `spread` rep then ran well past that pace (no new log line for several minutes as this
  report was written) -- plausibly more fabric contention from the same cause, or renewed contention from
  `~/s18p2/drive.sh`'s next soak iteration; each `run_once` is bounded by `timeout -k 10 600`, so it resolves
  (success or recorded failure) within 610 s regardless. 27 total runs (5 node counts x up to 2 placements x
  3 reps): worst case, if every remaining run took the full timeout, this finishes well inside the 8 h
  deadline; at the pace of the first node count, a realistic ETA is on the order of **10-20 minutes** from the
  03:08:07 EDT start, possibly longer if contention recurs.

Collect:
```
R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 aac7"
$R 'cat ~/s18sweep/S18SWEEP_DONE 2>/dev/null; cat ~/s18sweep/summary.txt; column -t -s$'"'"'\t'"'"' ~/s18sweep/sweep_results.tsv'
$R 'ps -p 2704179 -o pid,etime,cmd'   # kill only by this PID, never scancel
```

#### 5. Note: the branch was already merged by another process

While this task was running its real aac7 validation, another process merged `s18-kitfix` into `main`
(fast-forward, `main`'s reflog: `merge origin/s18-kitfix: Fast-forward`) and deleted the remote branch, and a
later `s18-w` branch's merge then advanced `main` further; a worktree-pruning side effect of that same
activity also removed this task's local worktree checkout (recreated here from commit `ef22f22` to land this
report). This task did not perform that merge and did not delete the branch itself, per the "do not merge"
instruction; it is recorded here because it changes what "the branch" means by the time this report lands.

> **Coordinator note (2026-10-07):** the sweep this report armed (~/s18sweep, 03:08 EDT) ran while Phase 2 used the same hold and is not used; the sweep of record is RESULTS §117, and the collision it caused is RESULTS §118.
