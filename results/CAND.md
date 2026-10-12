# CAND - the three off-by-default speed switches combined (interaction test)

Branch `cand` (off `main` a448d73). Labels: measured / modelled / assumed. Times Eastern.

## Design
- Code only, squashed from the three branches (one commit each; their results files and tools stay on their branches):
  - `o2`: `DBIG_SMALL_BOUNCE` (+ print-only `DBIG_BOUNCE_TS`, `tests/t_bounce.c`) - results/O2.md on o2.
  - `k1b`: `DIST_GATHER_RAW` (+ `DIST_GATHER_RAW_MARGIN_GB`, `tests/t_gather_raw_probe.c`) - results/K1b.md on k1b.
  - `k8b` at 99eee11 (includes the hang fix b1ff573): `BS_SEED_PULL` - results/K8b.md on k8b.
- No textual conflicts. Switch registry regenerated with `tools/switch_registry.py`; the three new switches given
  `experiment` rows in `ecalc/switches_manual.tsv`; `--check`: 269 rows, 0 problems (measured). README rows from each branch kept.
- All switches off by default; with them unset the cand binary takes the main code paths.
- Local build not possible (no hipcc on the workstation); built on aac7 (SHMEM) and aac6 by the gates below.

## Measurement (armed 2026-10-12 ~07:10 ET)
Arms: A = main binary, switches unset; B = cand binary with `DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1`.
- (a) aac7, 2 nodes, queue item `tools/cand_items/7e_cand_two.sh` (as `~/aac7q/queue/7e_cand_two.sh`): `tools/gate.sh --build
  --nodes 2` all on, then paired AB/BA at 6.441e10 digits per node (1.2882e11 total), aac7 LINE, no digit writes; A = the main
  build in the h4main worktree (copied to `~/cand/ecalc_main`). Stop: >= 4 pairs and p < 0.05 on the total, or 6 pairs, or no
  run after 65 min. NIC launch group and rank-0 memory per run. Output `~/cand/` (summary.txt, runs.tsv, stats.txt, groups.tsv,
  log/), marker `~/cand/DONE`.
- (b) aac6, 1 PPAC_MI300A_SPX node, `tools/cand_items/cand6_driver.sh`: own clones `~/ntt-cand` (cand) and `~/ntt-cand-main`
  (main a448d73) from a full bundle; `gate.sh --build --nodes 1 --line none` all on; paired AB/BA at 6.441e10, stop >= 4 pairs
  and p < 0.05 on the total, or 8 pairs, or no run after 40 min of the 45-min hold. Output `~/cand6/` (summary.txt, pairs.tsv,
  runs.tsv, log/), marker `~/cand6/DONE`.
- Stats: `tools/cand_items/cand_stats.py` (copy of o2's `o2_stats.py`, stop rule on the total; paired B-A, Student t, 95 % CI).

## Early results (measured, partial)
- aac7 gate 2 nodes all on: PASS (1e9, VERIFY OK, digits identical, -DCOMM_SHMEM), 07:11 ET.

## RESUME
- Collect aac7: `ssh <aac7> 'cat ~/cand/DONE; cat ~/cand/summary.txt'` (ETA ~08:00 ET, latest 08:20).
- Collect aac6: `ssh <aac6> 'cat ~/cand6/DONE; cat ~/cand6/summary.txt'` (ETA ~08:00 ET; the hold started 07:13).
- Then: fill a Results section here (paired mean B-A with 95 % CI, p, pairs, VERIFY, memory per arm, NIC groups), remove
  `~/ntt-wt/cand` (aac7) and `~/ntt-cand`, `~/ntt-cand-main` (aac6) once done.
