# FIT — the memory fit before every launch, and DM_MN_WRAP automatic

**Question.** User decision D26 (2026-10-13): "make it automatic, but we should only be scheduling jobs with efficient memory
allocations to begin with. So we need to know before we launch how well the memory fits." The planner's check (`MN_PLAN_ONLY`)
looked only at the 480 GB node budget, not at the device edge: at 3.82 × 10¹³ digits on 576 nodes, with `DM_MN_WRAP=1` (the default
since D22), the model put the device at 375.1 GB per node against the 373.44 GB edge measured on the target, and the plan still
said OK (results/NIGHT.md, section 7).

**Change / branch.** Branch `fit`. No new algorithm; digits are unchanged (the wrap on or off gives the same digits, gates of D22).

- `binsplit.c binsplit_fit` (C, the source of truth): for (digits, nodes, settings) at rank 0's layout, the device peak per node
  = max(dm phase: planes + arena + comm_ofi pools + 4 v-slots; bs phase: the same + the VMM pool's bs growth 6.0 GB + the tree
  top's v-slots), per APU = / 4, against `ECALC_DEVICE_GB`; the node = planes + arena + bs growth + host + 4 v-slots against
  `ECALC_NODE_GB`; both margins; the terms. The comm_ofi pools count as device memory (fine-grained device memory by default;
  `COMM_OFI_POOL=host` moves them out). Printed by `MN_PLAN_ONLY` (`plan fit` lines; a run that does not fit ends with
  `plan REFUSED ...: the memory does not fit`, exit 8), by every run (`memory fit:` lines on rank 0) and by `BS_LAYOUT_ONLY`
  (a `fit:` line per point).
- New switches: `ECALC_DEVICE_GB` (default 373.44: the target's edge, 93.36 GB per APU, m, results/TGTBENCH2.md) and
  `ECALC_DEVICE_MARGIN_GB` (default 5.0: the tighter of CAP17's two bars). `profiles/aac7.conf`: `ECALC_DEVICE_GB=444.0`
  (m: aac7 SPX t_edge, job 12287, VMM 444.0 GB committed, hipMalloc about 448.0; archive:RESULTS_ARCHIVE_1-114.md, section 107;
  the lower, graceful one is taken), on the aac7 launch line; `profiles/target.conf.template`: 373.44.
- `DM_MN_WRAP` automatic: unset, the run takes the wrap only when the device with it is at most `ECALC_DEVICE_GB` −
  `ECALC_DEVICE_MARGIN_GB`, else 0. Set by hand (0 / 1 / k), the set value wins, with a warning in the fit report when it does not
  fit. Every rank decides alike (rank 0's layout). The config line shows `dm_mn_wrap:<v>(fit|set|default)`; size 1 does not use
  the wrap (`default`, 1 as before). `newton_db.c dm_wrap` reads `binsplit_dm_wrap()`.
- Launchers: `tools/plan_values.py --fit <digits> <nodes>` (one entry point: the report, exit 1 = does not fit);
  `ecalc/mnrun.sh` (a block of its own after the pool block: plans the command's ecalc, prints the `plan fit` lines, refuses
  with exit 4 before srun; `MNRUN_FIT=0` skips); `tools/gate.sh` (before the allocation / the run: `fit.txt`, a FAIL when it does
  not fit; `GATE_FIT=0` skips); `ecalc/target_kit.sh` plan stage (the fit verdict and the wrap line in the summary; the refusal
  is the plan's last line, as a prime-set refusal). `plan_values.py` assumes `COMM_OFI_PLAN_CXI=1` when neither `COMM_OFI` nor
  it is set (mnrun.sh finds cxi on aac7's compute nodes at every launch; a login node has none).
- `mem_model.py`: `--check-c` ports the decision and compares the `fit:` line (wrap, device, node, fits, the device with and
  without the wrap); `mem_per_node` takes the wrap automatically by the same rule on its own `dev_max` (so `estimate.py --target`
  shows the target with the wrap off). `binsplit.c region_need` keeps its last two results (pure function; 0.18 s per call at
  the target's share): the planner with the fit takes 0.73 s against 0.58 s before (m, aac7 login node, MN_PLAN_ONLY at
  3.82 × 10¹³ on 576).

**Method.** Planner on the aac7 login node (no device). Target: `tools/plan_values.py --fit --profile target 38200000000000 576`
with `profiles/target.conf` = the template with its site keys filled by aac7 stand-ins (modules, paths; not tracked, deleted
after) -- every memory-relevant key is the template's (`COMM_SHMEM_ROUND_MB=1024`, `MN_T_CHUNK_MB=2048`, `ECALC_NP=auto`,
`COMM_OFI=1`, `ECALC_DEVICE_GB=373.44`, `ECALC_NODE_GB` = 480, the code default). aac7: `--profile aac7` at n × 6.441 × 10¹⁰
digits on n = 1, 2, 4, 6 (the S48 sizes). `mem_model.py --check-c` on `BS_LAYOUT_ONLY` output of six points. Gates: one aac7
queue item (see Results).

**Results.**

Memory fit (the planner's `plan fit` verdicts; m = computed by the planner on the aac7 login node, the figures themselves modelled
by the C layout):

| run | profile | device per node (per APU) vs limit | device margin | node vs 480 GB | node margin | DM_MN_WRAP |
|---|---|---|---|---|---|---|
| 3.82 × 10¹³ on 576 | target | 358.96 (89.74) / 373.44 | +14.48 | 395.85 | +84.15 | 0 (fit): with it 384.73 > 368.44 |
| 3.82 × 10¹³ on 576, `DM_MN_WRAP=1` set | target | 384.73 / 373.44 | −11.29 | 421.62 | +58.38 | 1 (set): WARNING, REFUSED |
| 6.441 × 10¹⁰ on 1 | aac7 | 290.09 / 444.00 | +153.91 | 316.44 | +163.56 | not used at size 1 |
| 1.2882 × 10¹¹ on 2 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit): without 368.47 |
| 2.5764 × 10¹¹ on 4 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit) |
| 3.8646 × 10¹¹ on 6 | aac7 | 374.22 / 444.00 | +69.78 | 409.24 | +70.76 | 1 (fit): without 348.45 |

Target terms at 3.82 × 10¹³ on 576 (the peak is the bs phase; the dm phase 352.96 GB): planes 120.88 + arena 214.75 + comm_ofi
pools 9.66 + bs growth 6.00 + v-slots 7.67 GB; per APU the arena 53.69 GB (the dm need 51.73: reciprocal set 38.53, division
37.45; the tree 50.93), plane pools 30.22. The wrap's term there is 7.64 GB per APU (the arena 214.75 → 240.52 GB per node).
NIGHT's 375.1 / 345.0 GB were mem_model's `dev_max`, which leaves out the comm_ofi pools (9.66 GB per node).

`mem_model.py --check-c` on BS_LAYOUT_ONLY at 4e10:1, 1e11:1, 6.632e10:576, 6.441e10:6, 1e9:2, 5e10:4 with the launch line's
settings (`COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=2048`, `ECALC_DEVICE_GB=444`): 0 terms not exact, the wrap decision equal at
every point (m).

Gates (aac7 queue items `20_fit_gates.sh`, `21_fit_gates2.sh`, commit 54f62a1 for the gates; m):

| check | result |
|---|---|
| `tools/gate.sh --build --nodes 2` | PASS: rc 0, VERIFY OK, digits identical, P24 in use; fit FITS 137.61 / 444.00 GB; config `dm_mn_wrap:0(fit)` (1e9 on 2: the wrap is not used at this size) |
| `tools/gate.sh --nodes 1` | PASS: rc 0, VERIFY OK, digits identical; fit FITS 120.44 / 444.00 GB |
| 6 nodes, 3.8646 × 10¹¹ digits, aac7 line through `mnrun.sh` (no digit output) | rc 0, VERIFY OK on all 7 lines; `mnrun.sh: plan fit FITS ... DM_MN_WRAP=1 (auto)` before srun and `memory fit:` in the run; config `dm_mn_wrap:1(fit)`; ecalc total 238.5 s (one run, not a timing claim) |

The first gate attempt (`20_`) failed at doccheck before building: the untracked stand-in `profiles/target.conf` was present in the
aac7 worktree and changed the registry's profile column. The stand-in was moved out and the two gates were run again (`21_`).

**Verdict.** Done as decided (D26): the fit is computed before every launch and a run that does not fit is refused; the wrap is
automatic. The target at 3.82 × 10¹³ on 576 nodes fits with the wrap off (+14.48 GB device margin, m planner).

**Open issues.**
- The fit's device is the layout's, not a measurement on the target: the bs growth (6.0 GB) is measured at size 1 on aac7, and
  the comm_ofi pools depend on `COMM_OFI_PLAN_CXI` being right (mnrun.sh asks a compute node; plan_values assumes 1).
- `mem_model.py --check-c` without `COMM_SHMEM_ROUND_MB=1024` differs from the C in the comm_ofi pools at 6 and 576 nodes
  (`room ofi pools` -11.6 % / -19.5 %); this predates FIT (the room rows show it) and carries into the fit rows. With the launch
  lines' `COMM_SHMEM_ROUND_MB=1024` every row is exact.
- `mem_model.py`'s own `dev_max` (used by `estimate.py`) does not count the comm_ofi pools as device memory (345.0 GB at the target
  with the wrap off against the C's 358.96 GB); the C is the source of truth.
- aac7 without its profile gets the target's 373.44 GB edge (the code default): stricter, never looser.

**RESUME.** Done: merged to main, pushed; aac7 `~/ntt-acc` updated; worktrees removed. Gate logs: `ssh <cluster> 'cat ~/fit/SUMMARY.txt ~/fit/SUMMARY1.txt'`
(access: see the private protocol).
