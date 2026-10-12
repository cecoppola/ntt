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

## Results (collected 2026-10-12; all measured unless marked)
Arms: A = main binary (85d9d73f), switches unset; B = cand binary (7e7cd7da) with `DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1`. 6.441e10 digits per node, aac7 line, no digit writes. Every run VERIFY OK (aac7 24/24, aac6 8/8). Gates: aac7 2 nodes all on PASS (1e9 digits identical, -DCOMM_SHMEM) twice (rounds 1 and 2); aac6 1 node PASS.

### aac7, 2 nodes (item 7e round 1, rerun as round 2), paired AB/BA, B-A
| round | pairs | mean total A -> B | paired B-A total (95 % CI, p) | paired B-A chain (95 % CI, p) |
|---|---|---|---|---|
| 1 (`~/cand`) | 6 | pair 1 was cold (A 170.0, B 145.5) | -4.95 s [-15.45, +5.55], p 0.280 | -4.51 s [-8.39, -0.64], p 0.030 |
| 2 (`~/cand2`) | 6 | - | -6.22 s [-12.20, -0.24], p 0.044 | -4.62 s [-7.61, -1.64], p 0.011 |
| pooled (computed from the 12 per-run totals) | 12 | A 140.10 s, B 134.51 s | **-5.58 s [-10.54, -0.63], p 0.031** | **-4.57 s [-6.57, -2.57], p 0.0004** (A chain mean 4.57 s vs B 0.06 s in all 12) |
| pooled without round-1 pair 1 (cold, A 170.0 s) | 11 | A 137.38 s, B 133.52 s | -3.86 s [-7.39, -0.33], p 0.035 | - |

- B was faster in 11 of 12 pairs (pair differences, s: -24.5, -0.2, -5.5, -1.1, +3.3, -1.7, -2.1, -1.2, -9.6, -9.6, -0.6, -14.3).
- "Chain" is the db_to_bi conversion chain: arm B 0.05-0.06 s in every run; arm A 2.0-2.4 s normally, but 6.1-10.8 s in 5 of 12 A runs (late PE up to +6.8 s on one node). Those A-arm spikes explain the large spread; the B arm has none. A single-switch effect cannot be separated here (all three on).
- The stop rule of the item (p < 0.05 on the total) was met in round 2 alone (p 0.044) and by the pooled 12 pairs (p 0.031); the p values are two-sided Student t on few pairs, round-1 total alone is not significant (CI crosses 0).
- NIC launch groups (A/B label of each run; fgfc_M 0.0 in all): both groups occur in both arms and rounds (round 1: arm A 3 x A / 3 x B, arm B 5 x A / 1 x B; round 2: arm A 5 x A / 1 x B, arm B 5 x B / 1 x A). The A-arm chain spikes occurred in 3 group-A runs and 2 group-B runs (5 spikes in 12 A runs, of which 8 were group A and 4 group B): no group pattern visible with this n ( a group effect cannot be excluded).
- Memory, rank 0, max over 6 runs per round and arm: device in use 317.8 GB, pool peak live 201.80 GB, hipMalloc 0.00 GB, APU driver-used 82.2 GB, identical in A and B: no memory cost (m).

### aac6, 1 node (`~/cand6`), 4 pairs, stop rule met after pair 4
- Totals A: 74.10, 73.47, 72.86, 74.54 (mean 73.74 s); B: 66.08, 66.55, 66.10, 67.61 (mean 66.59 s).
- Paired B-A total **-7.16 s [-8.08, -6.23], p 0.0001** (n 4; all four pairs negative). Gate 1 node PASS (1e9). (Memory columns NA: no per-rank memory lines at one node.)

### aac6 profile of cand all-on (`~/p2a`), 6.441e10, 1 node, one profiled + one unprofiled run (indicative)
- Profiled total 66.45 s, unprofiled total 66.91 s (both VERIFY OK). Unprofiled phases: init 13.74 s (region pools 7.24 s inside rns_init; arenas 6.98 s), bs 36.56 s (seeds 8.2, batch 21.1, mdev 7.1; the bs thread waited 8.24 s for the init thread), dm about 16.6 s (derived: total - init - bs).
- Against main (results/O7b.md on branch o7b, same size, 1 node, main 0298fe2, unprofiled): total 73.32 s -> 66.91 s (-6.4 s, one pair vs one run, indicative), init 15.5 -> 13.7 s, bs 38.2 -> 36.6 s (seed wait 10.0 -> 8.2 s), dm 19.5 -> about 16.6 s. This matches the paired aac6 result (-7.16 s).
- Kernel profile: GPU busy per APU 47.8 / 49.0 / 48.7 / 41.9 s (O7b main: 43.7 / 44.7 / 44.4 / 38.1 s), i.e. more GPU time but a shorter wall. New in the top list: `k_pull` 8.6 s per APU (18.5 % of GPU time, 4 calls; the BS_SEED_PULL kernel moving seeds on the device) and `k_gather_raw<1>` 2.0 s per APU (4.3 %). GPU timeline busier early (first 5 s bin 2.4 s busy, bin 5-10 s fully busy).

### Verdict
- All three switches on together: faster at both 1 node (-7.16 s, aac6) and 2 nodes (-5.58 s pooled, aac7), digits identical, no memory change. The 2-node gain is about 4.6 s from the chain (db_to_bi) plus a smaller remainder from the other two switches; per-switch shares not separated (not tested alone here; see o2, k1b, k8b results). Interaction vs the sum of the single results is not evaluated in this file.
- Adoption is the user's decision.

### Pending
- Item 7g (4 nodes, `~/cand4/`) is running on aac7, expected about 10:00 ET; its result goes here when collected.

## RESUME
- Collect 4-node round: `ssh <aac7> 'cat ~/cand4/DONE; cat ~/cand4/summary.txt'`, add as "aac7, 4 nodes" with the same columns (paired total and chain, groups, memory).
- Cleanup once done: `~/ntt-wt/cand` (aac7), `~/ntt-cand`, `~/ntt-cand-main` (aac6).
