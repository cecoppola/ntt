# MODEL — Time model, launch defaults, the target

Scope: the time model and its fits, the 576-node estimates, the launch-line defaults and memory fit, and the target facts. Current conclusions and live numbers (labeled m measured / mod modeled / a assumed) condensed from the former per-topic files; the removed text of each is unchanged in `archive:cleanup_1103/results/<OLD>.md`. Every former section keeps its `§ID` heading.

## TIME_MODEL — the time model and the 576-node estimates

Scope: the time model and the 576-node estimates: refits to measured runs, scaling studies, the model's inputs measured on aac7. | Owner of the items below (condensed 2026-10-19 from the merged per-item reports; originals in `archive:results_merged_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers are in `archive:results_raw_2026-10-19/`.

### Topic summary

- Topic: `ecalc/mn_model.py`, `mem_model.py`, and its `estimate` subcommand (was `estimate.py`) and their fits (`ecalc/aac7_fit.json`, keys s48 and l4; the two files were merged 2026-10-25), the 576-node time estimates, and what was measured to feed them. Model only: nothing here is in the engine, and no number below is a measurement of the target (fabric 100 GB/s per APU, the part-file rate and the taper are assumed).
- **Current model and estimate (ecalc/README.md, RESULTS.md):** profile `target_l4` (§L4), 576 nodes, 3.82e13 digits **about 182 s without the write** (3.71e13: 176.3 s), modeled, fabric assumed. aac7 profiles `aac7_l4` / `aac7_l4a` fit all of 2026-10-11's multi-node runs (rms log residual 0.062; was 0.156). Run `ecalc/mn_model.py estimate --fabric target_l4` for current rows.
- **Superseded (by §L4; kept for the band structure only):** the 576 tables of Q, X, D213d, F16 and C09's "model-best" schedules (a level-costing bug), T06's layv factor 0.655 (L4: 0.92), S48's profile and its 6-node miss (-20 %), S19B's 737 s projection and B = 6 GB/s fit and C16's `AAC7` profile (both early transport).
- **Live conclusions:** (1) a tree level costs its slowest group, so explicit `MN_GROUPS` balanced tops (`2,4,8,16,64,192,576`, or `2,4,8,40,200,N`, `5,35,140,N`, `4,24,96,N` for N < 576) are 0-25 s better than the standing form and the code's default schedule is far worse at almost every N != 576 (8 s to 2000 s): any node exclusion needs an explicit `MN_GROUPS`; at 576 the standing `2,4,8,16,32,64,192,576` is within 0.2 s of the best. (2) Non-power-of-2 groups run the slower `layv` kind (rate x0.92 of layeq in the model, raw per-thread ratio 0.41). (3) The 4-node bimodality (fast 181 s, slow 208 s) is the NIC launch group (rate x0.82). (4) The 6-node default `[2,6]` is slower and more variable than the model (+23 +- 12 s vs `[6]`), unexplained. (5) Fabric at 576 is not the bottleneck if 100 GB/s per APU holds (exposed communication 13-19 % of the wall); distributed tree levels are 33-40 %; the injection bandwidth per APU is the one input that moves the wall by more than +-20 s.
- **Steps in wall time at 576** (D213d, modeled): the term layout gives the top node 1.036 x the average digits, so the wall steps at 4.29 -> 4.30e13 (+13 % for 1.2 % digits); the model matches the C plan at 401/401 sizes. The one-node default grid may be far from best above 1e11 (finding, not changed).
- **Measured inputs on aac7 (C16):** per-message 8.5 us, `T_ROUND` <= 0.015 s, `HIDE_POW2` 0.72, `MAP_RATE` 0.070 s/GB, memory edge 520 GB device / 504 GB host pool, ROCm 7.0.3 costs +22 % at 1e11 vs 7.2.4 (168.5 vs 205.9 s); the one-NIC-per-PE cap (13.5-14.8 GB/s per node for one PE vs ~32 with 4 PEs) is a design fact, not a switch. Weak scaling at 6.441e10 per node (S48 / M2a, m): 1 n 74-76 s, 2 n 139, 3 n 148 (SCALE35, earlier code), 4 n 182-188, 6 n 246-293 s.
- Memory model: Q's `flat` / `grid` forms and SHMEM staging forms (`cached` / `per_exchange` / `resident`); current device / node budgets and the `DM_MN_WRAP` fit are in ecalc/README.md and results/MODEL.md §FIT, not here. S19B: the lean division (`DM_MN_LEAN`) is built in and `DIST_CHUNKS=8` is the default (README); `MN_T_CHUNK_MB=2048` and X2 are launch-line values.
- Where this file disagrees with main: its switch statuses are as of each item's date; main's `ecalc/README.md` and `switches.tsv` win. Open: a paired ABBA `[2,6]` vs `[6]` (8+ runs per arm), probing the NIC group in every multi-node run, the target's own fabric and Lustre numbers (A3 / A4).

### target_fit — a profile fitted to the target's 2 / 64 / 576-node runs, with a per-round synchronization term (A3, 2026-10-27)

Superseded for 576-node planning on the remote-v12 engine by §v12 below (this fit gives 527.5 s for the 2e13 run that took 206.27 s); the profile stays selectable and pinned.

**Code changes.** (1) The model's `XSHMEM_ROUND_MB` follows the code: default 128 (`ecalc/xshmem_round.h`; every target run used it), not the 1024 it assumed (`--round-mb`, `Design.round_mb`); the time columns of every existing profile are unchanged (the old size only entered the memory plan: the SHMEM staging / pool columns drop, e.g. 4e10 digits at 64 nodes: host 44 -> 36 GB, pool 9 -> 2 GB; `--round-mb 1024` reproduces the previous lines exactly). (2) New profile `--fabric target_fit` (`TARGET_FIT_CONSTS`): fabric 21.0 GB/s per APU, 30 us per message, `ROUND_SYNC` 1.15 ms x peers^0.5 per exchange round (rounds = ceil(a stream's outgoing bytes / `XSHMEM_ROUND_MB`) per chunk, `round_sync_cost`; 0 = one round), `DC_FMT_MN` 1.79 s per 1e9 digits (the digit residue as the code stands), init 1.2 x plus 0.6 s per doubling of the nodes beyond 2 (`INIT_DOUBLING`), `NODE_SCALE` 1.15. The sync term, `INIT_DOUBLING` and the dc rate are reset for every other profile (`apply_profile`), `MN_MODEL_ROUND_SYNC`, `MN_MODEL_ROUND_SYNC_EXP`, `MN_MODEL_INIT_DOUBLING` override.

### v12 — the model recalibrated on the measured decomposition of the 2e13 run (D2, 2026-11-01)

**Why the standing estimate missed (mod, `estimate --g 576 --D 3.4722e10 --fabric target_fit`: 527.5 s without the write, 6.72 TB per node; measured 206.27 s, 9.33 TB).** The terms were found by switching them off one at a time in fresh processes (the schedule search re-optimizes each time, so the pieces are approximate and do not add exactly): total 527.5 s; with `ROUND_SYNC` 0, 287.4 s; additionally latency 0, 241.3 s; additionally a 10x fabric rate (210 GB/s per APU), 155.3 s. So the estimate is about 240 s round synchronization, 46 s message latency, 86 s wire, and 155 s of init (22.4 s) plus compute and the rest (about 133 s).
1. Round synchronization, about 240 s against 58.7 s measured (polls plus other engine work). `round_sync_cost` charges `ROUND_SYNC` x peers^0.5 = 1.15 ms x 575^0.5 = 27.6 ms for each round of a 575-peer exchange, and counts a round as the exchange's bytes per APU divided by 128 MiB, i.e. it assumes full rounds. Real rounds are far from full: 165,300 rounds carried 14.1 MB per PE on average (m, 9.33 TB / 4 PEs per node / 165,300), 11 % of the cap, so the real count is about 10x a full-round count (17,400 for 9.33 TB; 12,500 for the model's 6.72 TB). The old fit was thus wrong in both factors: too few rounds (about 8,700 full-width round equivalents implied by 240 s / 27.6 ms) times a per-round cost about 78x too high (27.6 ms against 0.355 ms mean engine time per round measured). The product happens to be 4x the measured bucket. The constants came from six points of the 8e9-per-node weak-scaling runs (4.6e12 digits at 576 nodes, 255.3 s then; the same size runs in 68-97 s now, m) that had one to a few rounds per exchange, so they could pin the peers dependence but not the rounds (said in §target_fit); the earlier statement that the model's round count agreed with the measured count was wrong and is withdrawn.
2. Message latency, 46 s: 30 us per message (fitted jointly with the bandwidth, not a physical latency) times 1.18 million messages per APU in the final schedule (35 s direct; the rest is schedule re-optimization).
3. Wire, about 86 s against 50.6 s measured: the fit's 21 GB/s per APU is 84 GB/s per node (x 0.655 on the non-power-of-two groups), against 184 GB/s per node measured while transferring (9.33 TB / 50.6 s copy-quiet time; a: all of the copy-quiet time is wire time). The run's report says four of eight NICs were active (about 46 GB/s per active NIC); this figure is not in a tracked document (a).
4. Local compute and the rest, about 133 s against 60.2 s measured (GPU stream 40.3 plus other host 19.9): the model also overstates the non-fabric part by about 2x at this size.
5. Init: 22.4 s modeled against 36.6 s measured. The formula has no growth with the digit count; the series grows about 0.94 s per 1e12 digits.
6. Volume: 6.72 TB modeled against 9.33 TB measured (-28 %). The model prices the planned transform exchanges, operand and result redistributions, spills and the shifts (0.15 TB per node for 134 shifts at half a limb share). The 2.6 TB gap is not attributed; the shifts are a candidate of the right size (in the engine they are a group all-to-all in 1 GiB chunks; 139 shifts took 25.57 s on rank 0, up to 4.7 TB at the wire rate), but this is not shown (a). Per-tag byte counters now exist (`results/TRANSPORT.md`); a 576-node run with them settles it.
7. The bring-up script's 90.5 s estimate has no protocol terms (not examined further).

**The decomposition (m, per PE, seconds, 2e13 digits, 576 nodes).** Init 36.6; inside exchanges: copy quiet 50.6, partner polls 37.4, other engine work 21.3; between exchanges: GPU stream 40.3, other host 19.9. Sum 206.1 of 206.27 s. Fabric 9.33 TB per node each way (the run's report says on 4 of 8 NICs; not in a tracked document, a); 165,300 rounds per PE (mean 14.1 MB per round per PE, 4 PEs per node); 139 shifts, 25.57 s on rank 0 (a share of the exchange buckets, a memo line, not an added term).

### M2fit and M3init — the round cost and the init growth measured on aac7 (campaign M2 and M3, 2026-11-01/02 ET, main d7c7f28)

**Round cost (M2; data and fits in `results/TRANSPORT.md` §M2, measured).** Run time fitted against the round count (per PE, from the per-tag counts) over a 280-fold range of rounds: 54.0 +-1.1 us per round at 2 members and 38.3 +-1.0 us at 3 members (95 % intervals, R^2 0.997 and 0.994, 136 runs); the N3 estimate (about 45 us, an upper bound) is confirmed. The model's 0.355 ms per round at 576 nodes (v12; measured as a ratio of two 10-29 numbers) is 6.6x and 9.3x these.

What it changes in the model (modeled arithmetic on measured inputs): (1) The cost per round does not grow from 2 to 3 members (54 -> 38 us; weak test: 1 vs 2 peers), so no growth detected with the peer count; the superseded `ROUND_SYNC x peers^0.5` form would predict 1.4x from 2 to 3 members and is not supported at this range. (2) At 38-54 us the 165,300 rounds of the 10-29 run would cost 6.3-8.9 s, against the 58.7 s of polls and engine buckets measured at 576 nodes: only 11-15 % of the model's round term is a per-round cost that exists at 2-3 members, the other 85-89 % (about 50 s) is attributed (inference) to per-round cost that appears only with more peers or larger rounds (14.1 MB per round against 0.3-0.5 MB); part of it may be partner skew and wire wait, not per-round cost at all. So `alpha` has two parts in the model: a floor of about 40-55 us per round (measured), and the 576-node excess of about 0.30 ms per round (inferred from the 10-29 total, not measured). (3) Gains from larger rounds (XSHMEM §N3 table: -14 % to -25 % at 2e13) assumed the whole 0.355 ms scales with the round count; the floor part does scale with rounds (measured: the line is straight), the excess part is assumed to; the table's assumption is untested, but nothing here contradicts it. (4) `mn_model.py` is not changed here; a v13 round term would be `rounds x (floor + excess)` with the floor fixed to 38-54 us and the excess to be set from the next target data.

**Not measurable on aac7:** the 575-peer cost per round (the excess); it needs a run with many peers. A rerun of the sweep on current main gives seconds per tag from the stats summary (the polls and other-engine seconds), the closest aac7 can come to the split.

**Init growth (M3, measured; 2 and 3 members from the M2 runs' `init:` lines, 1 member from the M3 item).** The model assumes 0.94 s of init per 1e12 digits at 576 nodes, which is 0.54 s per 1e9 digits per node (1e12 / 576 = 1.74e9 per node; modeled conversion; the model figure is a total-init slope across the 576-node series; the measured figures are the rns_init maximum over nodes at 1-3 nodes, not the same quantity). `rns_init` on the slowest node (seconds; digits per node in brackets):

| members | digits | digits per node | rns_init, max over nodes | runs |
|---|---|---|---|---|
| 1 | 6.6e10 | 6.6e10 | 24.6 s (init total 25.0) | 1 |
| 2 | 1e10 | 5e9 | 8.9 +-0.45 s (sd) | 48 |
| 2 | 4e10 | 2e10 | 21.4 +-2.2 s | 16 |
| 2 | 1e11 | 5e10 | 32.1 s (init total 40.4: mn_init 1.6, layered tests 5.6) | 1 |
| 3 | 1e10 | 3.3e9 | 7.8 +-0.3 s | 48 |
| 3 | 4e10 | 1.3e10 | 16.5 +-0.7 s | 24 |

Slope between sizes: 2 members 1e10 -> 4e10: 0.83 s per 1e9 digits per node; 4e10 -> 1e11: 0.36 s per 1e9 (one run); 3 members: 0.87 s per 1e9 per node. The 1-member points at the other sizes (1e9 - 3e10) did not keep init lines (only the last run of an item kept its timeline), so the 1-node slope is not available; the one point 6.6e10 gives 0.37 s per 1e9 average (24.6 s). So the growth is between 0.36 and 0.87 s per 1e9 digits per node, bracketing the model's 0.54 s, and it is concave (flatter at the largest size), not clearly a function of digits per node alone (2 members at 2e10 per node: 21.4 s; 3 members at 1.3e10: 16.5 s; 1 member at 6.6e10: 24.6 s). A straight line through the aac7 points would be wrong in either direction by about 10 s; the model's one-slope form is within that spread. Split (timeline lines, the last run of each item): at 1 member 6.6e10, the region pools take 18.7 s of the 24.6 s `rns_init` (76 %), of which the seed thread waited 13.7 s; the rest of `rns_init` is 5.9 s (staging and contexts 0.6 s first). At 2 members 1e11: region pools 17.6 / 22.8 s (two nodes) of `rns_init` 26.4 / 32.1 s, seed wait 11.2 / 16.4 s; the arena mapping takes 4.5-8.3 s for 7-10 chunks per APU (create 2.4-4.5, map 1-3.4, access 0.3-1.1 s) and the background mappers queue for the mapper lock up to 4.6 s. At 3 members 4e10: region pools 8.9 / 12.9 / 16.2 s, seed wait 2.8 / 6.9 / 13.2 s, `rns_init` 19.5-24.6 s (that run was slower than the M2 mean of 16.5 s, a node-to-node spread of 5 s). Mapping plus seed wait is therefore 70-80 % of the init growth; what remains after it (transport 1.2-1.6 s, layered tests 0.2-5.6 s) does not grow with digits. This is the S1 / `ECALC_MAPD` baseline (`results/MEMORY_IO.md` §S4); the repeat with `ECALC_MAPD=1` is not run yet.

### L4 — refit of the time model to all of 2026-10-11's multi-node aac7 runs; what it got wrong and why (wave L, agent L4)

**Answer.** One bookkeeping bug and one mis-fitted rate explain the misses; a third effect (the 6-node default's variance) is not modeled.
- **Bug (fixed, affects every uneven `MN_GROUPS` result):** a tree level was costed through its last (clipped, smaller) group; the real level ends when its slowest group ends. C09's "model-best" forms (`2,4,8,16,80,560,576`: 146.5 s) rest on this: costed at the slowest group the same form is 196.2 s, 12.5 s worse than the standing default `2,4,8,16,32,64,192,576` (183.7 s). Schedules whose levels divide the node count are unchanged (bit-identical outputs). Old behavior behind `MN_MODEL_TREE_TOPONLY=1`.
- **Rate (refit):** the layv factor 0.655 was fitted on the 6-node default `2,6` alone; fitted on all non-power-of-2 classes (3, 5, 6 nodes, 7 schedules) it is **0.92** (assumes the unknown NIC launch group is the fast one B; 0.73 / 1.0 move the 576 estimate by +-3 s). The bulk rate depends on the NIC launch group: **10.63 GB/s per APU thread in B, x0.818 in A** (S48's 10.31 was a mix).
- **Not modeled:** 6-node `[2,6]` is slower on average (256-277 s vs 243 s for `[6]`) and more variable (223-336 s, sd 22 s vs 10 s) than the model's 240 s; per-node timers show one 2-node group of three occasionally taking 2x as long (an expected maximum over groups). Unidentified with 5-7 runs per class.
- **Residuals** (rms of log ratio over 12 run classes): 0.156 -> **0.062** (group B for classes of unknown group). Power-of-2 classes -11..+5 % -> -3..+3 %; 3 nodes +18 -> -3 %; 5 nodes +7..+40 -> +5..+12 %; 6 nodes `[6]` +27 -> +2 %, `[3,6]` +17 -> -6 %, `[2,6]` +6 -> -13 % (worse).

**Verdict.** The L4 model (`aac7_l4` / `target_l4`) supersedes S48's and T06's profiles and C09's schedule advice; the standing 576 estimate is **176.3 s (3.71e13) / 181.7 s (3.82e13) no-write, modeled, fabric assumed**. Redo after more aac7 runs: add class totals to `L4_CLASSES` in `archive:scripts_2026-10-23/refit_s48.py`, `archive:scripts_2026-10-23/refit_s48.py --l4 --write-profile`, rerun `l4_search.sh <tag> "GT_PROFILE=target_l4 GT_RELEASE_GB=8.59"` (~1 min per variant on an exclusive node), then `groups_table.py plancheck`. **Open:** the 6-node `[2,6]` variance (a paired ABBA `[2,6]` vs `[6]`, >= 8 runs per arm, would settle +23 +- 12 s); layv 0.92 assumes group B for runs of unknown group (probe the NIC group in every run); the 5-node order; target fabric and part-file rate still assumed; the E[max over groups] effect would grow with groups at 576 (288 pairs at level 1), modeled nowhere (k4's +15 s is one crude bound).

### T06 — layeq / layv rate in the time model, and the 576-node estimate redone (2026-10-11)

**Change** (default-off factor, digits not involved): `mn_model.Fabric.v_factor`, `piece_cost` prices the transform exchange of a general-map group with it; `mn_model.py estimate --layv-factor F`; `archive:scripts_2026-10-23/refit_s48.py` fits it on the non-pow2 totals. **Correction of an S48 number:** the product-cost memo had no node/init scale in its key, so S48's "target + refit locals" row (169.9 / 175.3 s) read stale entries; clean values 152.4 s (3.71e13) and 157.3 s (3.82e13) no-write (mod); the fit itself and the "as is" rows are unchanged.
**Fit (mod).** layv factor **0.655** (layv at 65.5 % of the layeq rate; raw per-thread xstats ratio 0.41-0.42, m: 7.5 / 8.0 / 7.0 vs 18.3 / 19.1 / 17.3 GB/s, lower because the model's single bw also carries overlap and redist / result). Residuals 1 n 0.0 %, 2 n -1.7, 4 n +1.8, 6 n 0.0 (fitted; was -20.0). **576 nodes, no-write, 100 GB/s per APU and 2 us (a)** (3.71e13 / 3.82e13, s): layv = layeq: refit locals 152.4 / 157.3, standing locals 180.2 / 186.0; factor 0.655: 161.3 / 166.7 and 189.1 / 195.3 (+8.9 / +9.3); raw 0.41: 176.7 / 182.9 and 204.5 / 211.6 (+24 / +26). Per level at 3.71e13: 192 level +2.2 s, 576 level +6.7 s. The crude "~219 s if the whole 6 n penalty carries" is an upper bound; the per-level model prices the penalty only on the 2 of 8 layv levels (9-26 s, a). **Caveats:** 0.655 is one parameter on one point (6 n, 304 / 283 s) and absorbs everything the 6 n run has beyond the 2/4 n curve; whether it holds at 192 / 576 nodes or on the target is unmeasured. **Adopted 2026-10-11** (user, branch a02): `mn_model.TARGET_LAYV_FACTOR = 0.655` set on the `target` / `target-m` profiles; `--layv-factor 1.0` restores the old numbers; `aac7_s48` takes its json fit (0.6547). Standing 576 no-write after adoption: **189.1 / 195.3 s** (standing locals), 161.3 / 166.7 s (refit locals); the part file adds 14.3-17.3 s. **Superseded by §L4** (factor 0.919, level fix): 176.3 / 181.7 s.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/TIME_MODEL.md`.)

### M2a — time-model refit inputs: 1-, 2-, 4- and 6-node scaling points (aac7 items 77 / 78 / 7a / m2b, 2026-10-12)

Goal: refit `ecalc/mn_model.py` (then `target_l4`) to the then-current code (main 0298fe2, SHMEM build, e16 launch line + `COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1`, no digit writes, 6.441e10 digits per node, runs alternated 1 2 2 1 1 2 2 1 ...). All 24 + 12 + 12 runs rc 0 and VERIFY OK (item 77's summary overwritten by a later item and rebuilt from saved raw logs). Numbers measured.

**1 and 2 nodes** (mean, s; "excl r1" drops the cold first 1-node run: 87.4 / 80.7 / 81.4 s in items 77 / 78 / 7a):

| nodes | items | total | bs | recip | dm | init |
|---|---|---|---|---|---|---|
| 1 (excl r1, pooled 15 runs) | 77 / 78 / 7a | 74.4 (74.39 / 74.23 / 74.56) | 40.1 | 5.08 | 20.67 | 13.45 |
| 2 | 77 (n 6) | 136.41 (2.69) | 48.31 | 16.90 | 55.69 | 28.70 |
| 2 | 78 (n 6) | 137.97 (4.18) | 48.04 | 17.27 | 57.30 | 28.87 |
| 2 (excl first, 7a n 5) | 7a | 138.85 (4.95) | 48.55 | 17.20 | 57.07 | 29.48 |

### S48 — scaling study (1 / 2 / 4 / 6 nodes at 6.441e10 digits per node) and the first aac7 profile refit (2026-10-11)

Base main daeb0f9, SHMEM build, aac7 launch line + `COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1`, 2 timed runs per size, no digit output, driver `archive:drivers/s48/s48_run.sh`; 8 runs, all VERIFY OK. 6-node jobs ran in a separate Slurm job.

**Measured (m, s):**

| nodes | total (r1 / r2) | mean | init | bs | dm | recip (inside dm) |
|---|---|---|---|---|---|---|
| 1 | 76.73 / 75.87 | 76.30 | 13.5 / 13.2 | 40.5 / 40.2 | 22.6 / 22.3 | 6.1 / 5.8 |
| 2 | 144.12 / 133.85 | 138.99 | 27.3 / 26.6 | 51.0 / 47.0 | 61.3 / 55.9 | 18.4 / 16.5 |
| 4 | 181.97 / 181.62 | 181.80 | 30.1 / 28.9 | 70.9 / 72.3 | 76.9 / 76.9 | 22.0 / 22.0 |
| 6 | 304.12 / 282.82 | 293.47 | 41.6 / 31.3 | 112.6 / 112.0 | 145.9 / 135.7 | 40.3 / 38.5 |

### S19B — profile analysis of S19A (2 -> 10 nodes), the S19C switch A/B, and a 576-node projection (2026-10-07)

Read-only analysis (job 12287); code `ntt-acc` main @ 3e508e76; inputs S19A (2026-10-07 09:52-10:39 EDT) and S19C (10:40-12:11). Config: 6.441e10 digits per node (the 3.71e13 target's share), no write, `COMM_TRANSPORT=shmem` with comm_ofi device exchanges, `ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6 MEM_REPORT_DEVS=1`. `DM_MN_LEAN=1` was not set although the target's launch line has it, so "base" is the line without LEAN. All 19 runs rc 0, VERIFY OK.
**Memory (m unless marked).** 10-node top node at bs: base 389.1-389.6 GB node (top APU 92.3 GB driver-used against the target's 93.36 GB hipMalloc edge, TGTBENCH2; layout 391.49; margin -1.9 to -2.4); **LEAN (`DM_MN_LEAN=1`) 362.9-363.5 GB (-26.2 GB top node, -8.4 GB others in the tree, time neutral)**; DC8 (`DIST_CHUNKS=8`) 381.8-382.1, halves the general-map v-slots (3.44 -> 1.72 GB per APU, -7.0 GB per node, matches model); T2048 unchanged. v-slots grow with node count: 13.8 GB per node at 10, 26.8 at 576 (mod, `mem_model.vslot_resident`), so DC8 at 576 gives -13.4 GB per node (device margin 9.91 -> ~23.3 GB, mod; confirm with `BS_LAYOUT_ONLY=...:576`). LEAN + DC8 at 10 n: node_with 376.02 GB (mod).
**Scaling model.** `mn_model.py estimate --fabric aac7_c16 --bw B` (the `aac7_s18` profile was retired 2026-10-23: it was `aac7_c16` with `--bw 6`): fitted effective **B = 6 GB/s per APU thread** (raw stage 10.2 m, profile 4.25): residuals vs measured 2 / 4 / 6 / 8 / 10 n +3.9 / +3.1 / -8.6 / -1.5 / -5.8 % (profile at 4.25 +18..+30 %). **576 nodes, 3.71e13, no write (mod):** aac7-class B = 6: **737 s** (656-851 s for B = 7-5; distributed tree levels 412 s = 56 %, division 181 s, reciprocal 72 s; exposed exchange 79 %; the 192 level costs 97 s and the 576 level 66 s); target standing estimate (`target-m --bw 47`) 216.5 s; target with B derated by aac7's effective / raw ratio 0.59 about **266 s**; target at 100 GB/s per APU (a) 178 s. Fabric volume at 576: 13.7 TB per node (mod) or ~10.3 TB scaled by the 10-node measured/model byte ratio 0.75; 1.46 M messages per APU (vs 30 k at 10 n); per-node time at fixed share grows ~1.7x from 10 to 576 nodes at aac7-class fabric.
**A/B (S19C, 10 nodes, 3 rounds, fixed order base, T2048, LEAN, DC8 with base always first; m):** base 474.4 +- 49.1 s (424-522; the 6 base-like runs mean 462.0, sd 38.6): no paired delta below ~50 s means anything. T2048 414.0 +- 51.1 (mean paired -60.3 s, two fast runs below every base-like run; promising, not proven; memory unchanged); LEAN 437.5 +- 18.1 (-36.8 s, within noise; peak 362.9-363.5); DC8 432.9 +- 5.9 (-41.4 s, within base range; -7.0 GB per node). Recommendation: LEAN into every aac7 base line (already the target line), DC8 for memory subject to a `:576` layout check and a 10-node LEAN+DC8 run, T2048 only after a >= 6-round ABBA. Ranked opportunities: raise the effective fabric rate (about -40 s per +1 GB/s per APU at 10 n, -80 s at 576, mod), find the "other" time (up to 120-155 s at 10 n if waiting; needs a stats-only switch), T2048, the reciprocal chain's wait (~-10 s), DC8 memory, LEAN, init seed wait (<= 5 s), fabric noise (±12 % at 10 n: pair every A/B). Decisions raised then (DIST_CHUNKS=8, T2048 retest, instrument "other", `aac7_s18` refit to 6 GB/s) belong to the later switch sections (results/TRANSPORT.md, archive:results_merged_2026-10-24/TOOLS_PROFILES.md §S18M); the wait-for-peers instrument is `MN_WAIT_STATS`.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/TIME_MODEL.md`.)

### C16 — Phase 16 C: the model's inputs measured on aac7 (Slingshot-11, Cray OpenSHMEMX 11.8.0) and `mn_model.py` recalibrated (2026-10-04)

Agent C, branch `p16-C` (from `p16-A` 26671c6). Everything measured unless marked. Jobs: 1 node (J1 12179: toolchain, edge, mapping) and 2 nodes (J2 12180 and a rerun 12194: fabric sweeps, overlap, T_ROUND). New code: `tests/t_comm --bw / --lat` (injection sweep, per-message cost; 04d23d6), `tests/t_edge` (memory edge), `COMM_SHMEM_PUT_MB` (4a4e21c, off), the `AAC7` profile in `mn_model.py` / `mn_model.py estimate --fabric aac7` (9dbb2c7).
**2.8 Model recalibrated: the `AAC7` profile** (`mn_model.py estimate --fabric aac7`): bw_apu 3.6 GB/s, lat 8.5 us, write_bw 0.116 GB/s, HIDE_POW2 0.72, GEN_HIDE 0.011 / 0.74 (aac6, assumed), T_ROUND 0.015, MAP_RATE 0.070, NODE_SCALE 1.12, INIT_SCALE 1.20 (fitted on the 7.0.3 one-node 1e11: 205.9 = 1.12 x 183.8); the TARGET profile and all defaults unchanged; env overrides `MN_MODEL_HIDE_POW2` / `GEN_HIDE1` / `GEN_HIDE2` / `MAP_RATE` / `NODE_SCALE` / `INIT_SCALE`. Check at 1e10 per node (model at the profile's bw 3.6 / at that g's measured rate / measured wall): g = 2 50.7 / 50.7 / **35.3-36.7 s (+40 %)**; g = 4 68.0 / 77.8 (`--bw 2.9`) / 87.3 (B's e10d, **-11 %**); g = 8 81.3 / 219.2 (`--bw 1.0`) / 327.1 (B's open issue, **-33 %**); g = 1 23.9 vs 26.0-27.7 (-10 %), 207.0 vs 205.9 (fitted). The g = 2 miss is a structural byte law: the model moves 86.7 GB per APU (346.8 GB per node) where the code moved 38.7 GB (2.2x); with the code's bytes the wall is 36.6 s. **The fabric rate per APU thread falls 3.6 (2 n) -> 2.9 (4 n) -> 1.0 (8 n) GB/s** (one NIC per PE shared by g - 1 peers; cause open then; the later aac7 sections show rates of 10-19 GB/s with the current transport, so this fall-off was a property of the early transport): use `--bw` with the measured rate per node count. `T_ROUND` sensitivity at the target: the gain of `MN_T_CHUNK_MB=2048` scales with it (-10.9 s at 0.03, ~-5.5 at 0.015, ~-1 at 0.003, modeled).
**2.10 Inputs table (measured unless marked):** injection per APU thread 3.6 GB/s (13.5-14.8 per node; 19.5 at 4 MiB puts; 32 per node with 4 PEs); per-message 8.5 us per put + signal + wait; `HIDE_POW2` 0.72 (0.76 at depth 1); `GEN_HIDE_DEPTH` assumed; `T_ROUND` <= 0.015 s (fixed part <= 0.003); `MAP_RATE` 0.070 s/GB; init at 1e11 22.2 s (7.0.3) / 16.2 s (7.2.4); edge 520 GB device / 504 GB host pool; ROCm 7.2.4 vs 7.0.3 at 1e11 168.5 vs 205.9 s; part file on NFS 0.116 GB/s.
**Open (historical):** the `comm_shmem_init` segfault + PE-0 hang (A16 §4) hit 3 of 10 n2 launches (2 `shmem_init` segfaults with an `inet_recv: unexpected socket EOF` flood, 1 silent PE-0 death at init); a cancelled SHMEM step broke the node's HSN for the rest of the job (16 launches failed with `Error configuring interconnect`; a fresh allocation is the cure); digits identical across all completed runs; `NIC_POLICY=NUMA` refuses; no 3-node run for `GEN_HIDE`. Superseded by §L4 for the aac7 fit and by results/TRANSPORT.md for the transport.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/TIME_MODEL.md`.)

### SCALE35 — scaling points at 3 and 5 nodes (docs only; main 39346d03, SHMEM build, defaults)

3 runs per count, 6.441e10 digits per node, no output; all 6 VERIFY OK, no segfault (m).

| nodes | digits | totals (s) | mean +- sd | bs / recip / dm / init |
|---|---|---|---|---|
| 3 | 1.9323e11 | 147.98, 143.11, 152.06 | 147.72 +- 4.48 | 52.7 / 15.7 / 57.4 / 34.0 |
| 5 | 3.2205e11 | 175.90, 177.18, 193.25 | 182.11 +- 9.67 | 68.0 / 21.0 / 77.9 / 32.1 |

With the 1/2/4/6-node means of PLAN.md §2 (67.6 / 119.4 / 172.2 / 204.6 s) the curve reads 67.6, 119.4, 147.7, 172.2, 182.1, 204.6 s at 1-6 nodes; efficiency T1/Tn 1.00, 0.57, 0.46, 0.39, 0.37, 0.33 (m / derived). Monotone; 3 and 5 nodes show no slow mode. Data point only; nothing to adopt (3 runs per count; 5-node sd 9.7 s the largest). (These totals are from an earlier code state than §S48's 76 / 139 / 182 / 293 s series.)

## LAUNCH_DEFAULTS — launch-line decisions and defaults adopted

Owner of items B01, L1, T08, ADOPT2, DEF, FIT, S47, AAC7OPT, A2EDGE (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; originals: `archive:results_merged_2026-10-19/`. Labels: m measured, mod modeled, a assumed. Times Eastern.

### Topic summary

What is current (see `ecalc/README.md` Switches table, which wins on any difference):
- Launch lines: the build and launch lines of both paths and the sources of the aac7 and target lines are owned by `PLAN.md` section 3 (not repeated here); `ECALC_SEGV_TRACE=1` is set by `gate.sh`. The B01/L1 line (`COMM_LAYER_INTER2`, VSLOT and cache-fit switches) is history: X2 was removed 2026-10-23, the others are built in (2026-10-12). Fastest aac7 settings by size: §AAC7OPT.
- K2 + K3 (`DBIG_ADDSUB3`, `DBIG_MODQ_MONT`) default 1 (T08); `=0` is the fallback. K4 `RNS_CRT_SUB` stays off.
- `P24_CRT_SCAN=1` (D24) and `NEWTON_CHAIN_GUARD=1` (D25) are code defaults (ADOPT2); the guard costs +0.25 s [-1.00, +1.50] (n.s.) at 1 node.
- P24 (four primes, 24 digits per point) is the default for every multi-node product (D18, DEF); `ECALC_NP_MN3=1` kept the three-prime path as a fallback (removed 2026-10-19, D56). P24 costs +17.18 GB planes per node at 6.441 × 10¹⁰ digits per node (m).
- `DM_MN_WRAP` is now AUTOMATIC (D26, FIT): unset = 1 only when the device peak with the wrap is at most `ECALC_DEVICE_GB` − `ECALC_DEVICE_MARGIN_GB`; a set value wins. DEF's "DM_MN_WRAP default stays 0" and B01/L1's rank-0 figures are superseded by FIT. (D22 had made 1 the default in between; the automatic rule replaced it.)
- Memory fit before every launch (FIT): planner/runs print `plan fit` / `memory fit:`; a run that does not fit is refused (`plan REFUSED`, exit 8; `mnrun.sh` exit 4; `gate.sh` FAIL). Current target figure: 3.82 × 10¹³ digits on 576 nodes with the defaults (`MN_NONPOW2_CAP` auto = on): device 407.41 GB vs the 373.44 GB edge, margin -33.97 GB, REFUSED (planner, m, 2026-10-25; wrap off); with `MN_NONPOW2_CAP=0`: 358.96 GB, +14.48 GB (the 2026-10-19 figure; with wrap 384.73, refused). Cause: NPCAP's larger planes (see the table note). `ECALC_DEVICE_GB` default 373.44; `profiles/aac7.conf` 444.0 (m).
- S47 (4-node re-confirm of T2048 and X2) was inconclusive; both stay on the aac7 line on their 10-node evidence (`results/CODE_TOOLS.md`, S31).
- Open: P24 at non-power-of-two groups below 32 nodes not gated; DEF's 6-node `total` difference for the wrap not significant (n.s.); FIT figures are layout values, not a target measurement.

### B01 -- launch-line decisions of 2026-10-11 (branch b01 from main bc80001)

History. Put `MN_T_CHUNK_MB=2048` and the X2 exchange on the target line (X2 removed 2026-10-23); modeled 576-node memory at 3.82e13 with them was 369.1 GB rank 0 (margin 4.3 GB, mod), which L1 improved. Superseded by FIT.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/LAUNCH_DEFAULTS.md`.)

### DEF — P24 as the multi-node default (D18) and DM_MN_WRAP in the memory model + 6-node A/B (D19 b)

Question: D18 makes P24 the default for every multi-node product (three-prime path stays selectable); D19 (b): count `DM_MN_WRAP`'s extra block-pool memory in `dm_layout`/`mem_model.py` and time `DM_MN_WRAP=0` vs `=1` at 6 nodes with the new default. Branch `def` (from main d1d19bf).
Verdict: D18 (P24 default) merged. `DM_MN_WRAP=1` saves 6.25 s of the division (p 0.032, 4 rounds) but `total` is n.s.; costs +5.4 GB per APU at the division's peak. Decision went to the user (became D22, then automatic D26). Open: `BS_LAYOUT_ONLY` reads `ECALC_NP` once per process (lay out multi-node points with `ECALC_NP=auto`); P24 at non-power-of-two groups below 32 nodes not gated; 4 more rounds could settle `total`; the wrap term was checked against one peak (5.37 vs 5.73 GB, 6 nodes) and P1's 2-node growth (4.3 vs 8.59 modeled). Raw: aac7 `<cluster work dir>/def/`.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/LAUNCH_DEFAULTS.md`.)

### FIT — the memory fit before every launch, and DM_MN_WRAP automatic

Question (user decision D26, 2026-10-13): make the wrap automatic, and know before launch how well the memory fits. The planner (`MN_PLAN_ONLY`) only checked the 480 GB node budget: at 3.82e13 on 576 nodes with `DM_MN_WRAP=1` the model gave 375.1 GB device vs the 373.44 GB edge and the plan still said OK (`archive:results_merged_2026-10-23/NIGHT_RUNS.md` §NIGHT, section 7; NIGHT's 375.1/345.0 were `mem_model.dev_max`, which omits the 9.66 GB comm_ofi pools). Branch `fit`; digits unchanged.

Change:
- `binsplit.c binsplit_fit` (source of truth): device peak per node = max(dm phase: planes + arena + comm_ofi pools + 4 v-slots; bs phase: same + VMM bs growth 6.0 GB + tree-top v-slots), per APU = /4, vs `ECALC_DEVICE_GB`; node = planes + arena + bs growth + host + v-slots vs `ECALC_NODE_GB`. Printed by `MN_PLAN_ONLY` (`plan fit`; refusal `plan REFUSED ...: the memory does not fit`, exit 8), every run (`memory fit:` on rank 0), `BS_LAYOUT_ONLY` (`fit:`). comm_ofi pools count as device memory (`COMM_OFI_POOL=host` moves them out).
- Switches: `ECALC_DEVICE_GB` default 373.44 (target edge, 93.36 GB per APU, m, `results/MODEL.md §TGTBENCH2`); `ECALC_DEVICE_MARGIN_GB` default 5.0 (the tighter of CAP17's two bars). `profiles/aac7.conf`: `ECALC_DEVICE_GB=444.0` (m: aac7 SPX t_edge, VMM 444.0 GB committed, hipMalloc about 448.0; `archive:RESULTS_ARCHIVE_1-114.md` section 107; the lower, graceful one taken); `profiles/target.conf.template`: 373.44.
- `DM_MN_WRAP` automatic: unset, the wrap is taken only when device-with-wrap ≤ `ECALC_DEVICE_GB` − `ECALC_DEVICE_MARGIN_GB`; a set value (0/1/k) wins with a warning if it does not fit; all ranks decide alike. Config line `dm_mn_wrap:<v>(fit|set|default)`; size 1 does not use the wrap.
- Launchers: `tools/plan_values.py --fit <digits> <nodes>` (exit 1 = does not fit; the device comm pools are always counted since Q103, 2026-10-25); `ecalc/mnrun.sh` (refuses with exit 4 before srun; `MNRUN_FIT=0` skips); `tools/gate.sh` (`fit.txt`, FAIL if not fit; `GATE_FIT=0` skips); `target_kit.sh` plan stage. `mem_model.py --check-c` ports the decision. Planner with fit 0.73 s vs 0.58 s before (m, MN_PLAN_ONLY 3.82e13:576, login node).

Results (planner on the aac7 login node; figures from the C layout, mod). GB:

| run | profile | device per node (per APU) / limit | margin | node vs 480 | node margin | DM_MN_WRAP |
|---|---|---|---|---|---|---|
| 3.82e13 on 576, defaults (NPCAP auto on) | target | 407.41 (101.85) / 373.44 | -33.97 | 443.49 | +36.51 | 0 (fit): with it 433.18; REFUSED |
| 3.82e13 on 576, `MN_NONPOW2_CAP=0` (the 2026-10-19 figure) | target | 358.96 (89.74) / 373.44 | +14.48 | 395.04 | +84.96 | 0 (fit): with it 384.73 > 368.44 |
| same, `DM_MN_WRAP=1` set | target | 384.73 / 373.44 | −11.29 | 421.62 | +58.38 | 1 (set): WARNING, REFUSED |
| 6.441e10 on 1 | aac7 | 290.09 / 444.00 | +153.91 | 316.44 | +163.56 | not used at size 1 |
| 1.2882e11 on 2 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit): without 368.47 |
| 2.5764e11 on 4 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit) |
| 3.8646e11 on 6 | aac7 | 374.22 / 444.00 | +69.78 | 409.24 | +70.76 | 1 (fit): without 348.45 |

- Why 358.96 became 407.41 (bisect 2026-10-25, planner on the build caches and login builds, `ECALC_NP=auto MN_T_CHUNK_MB=2048 ECALC_DEVICE_GB=373.44`, xshmem; m, planner): 36a4c38 and d330ef3 (10-13, 10-19) 349.29 GB without the comm pools (planes 120.88); 463f87d (10-21) 394.52 without comm pools (planes 162.22, v-slots 11.56); from 10-21 the xshmem default counts the comm pools (Q73): +9.66 with the plain plane, +12.88 with NPCAP; today 407.41. The step is `MN_NONPOW2_CAP` auto (Q58, `results/COMPUTE.md §NPCAP`: plane memory +41.3 GB, as this planner shows: +41.34); the 10-19 figure was never re-run after it; `BS_ARENA_ROOM` 0.14 vs 0.16 changes nothing here (arena 214.75 either way). A real need, not a planner over-count. Per-term at 3.82e13 on 576 with `MN_NONPOW2_CAP=0` (peak is the bs phase; dm phase 352.96): planes 120.88 + arena 214.75 + comm_ofi pools 9.66 + bs growth 6.00 + v-slots 7.67 GB; per APU arena 53.69 (dm need 51.73: reciprocal set 38.53, division 37.45; tree 50.93), plane pools 30.22; wrap term 7.64 GB per APU.
- `--check-c` on six `BS_LAYOUT_ONLY` points with the launch-line settings (`COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=2048`, `ECALC_DEVICE_GB=444`): 0 terms inexact, wrap decision equal everywhere (m); also exact after rebasing on main (rebuilt with `make -B SHMEM_CRAY=1`, gates not rerun after the rebase).
- Gates (aac7, commit 54f62a1, m): `gate.sh --build --nodes 2` PASS (VERIFY OK, digits identical, P24; fit 137.61/444.00 GB; `dm_mn_wrap:0(fit)` at 1e9); `--nodes 1` PASS (fit 120.44/444.00); 6 nodes 3.8646e11 via `mnrun.sh` (no digit output): rc 0, VERIFY OK on all 7 lines, `dm_mn_wrap:1(fit)`, ecalc total 238.5 s (one run, not a timing claim). The first attempt failed at doccheck because an untracked stand-in `profiles/target.conf` was in the worktree.

Verdict: done as decided (D26); the fit is computed before every launch, a run that does not fit is refused, the wrap is automatic. The target at 3.82e13 on 576 fitted with the wrap off (+14.48 GB, planner) before NPCAP's default; with `MN_NONPOW2_CAP` auto it is refused (-33.97 GB, 2026-10-25). Merged.
Open: the device figure is the layout's, not a target measurement (bs growth 6.0 GB measured at size 1 on aac7; comm_ofi pools depend on `COMM_OFI_PLAN_CXI`); `mem_model.py --check-c` without `COMM_SHMEM_ROUND_MB=1024` differs in the comm_ofi pools at 6 and 576 nodes (-11.6 % / -19.5 %; exact with the launch lines' 1024); `mem_model.py dev_max` omits the comm_ofi pools (345.0 vs C's 358.96 GB); aac7 without its profile gets the 373.44 GB edge (stricter, never looser).

### S47 - exchange settings re-confirm at 4 nodes (aac7, batch 2026-10-11, track T2)

Finished 2026-10-11 08:56; **verdict INCONCLUSIVE at 4 nodes**.
Question: on the aac7 line (`e16_headline.sh`, 4 nodes, 6.441e10 digits per node = 2.5764e11 total, no digit output): (i) `MN_T_CHUNK_MB` 1024 (A) vs 2048 (B; aac7 line since 2026-10-08, S22 D2: 24 rounds, −42.3 s at 10 nodes); (ii) X2 off (A) vs on (B; since 2026-10-09, S31: −31.7 s at 10 nodes). `COMM_LAYER_VSLOT_SHARE=1` kept in both arms.
- Verdict: inconclusive; point estimates (+7.0 s, −2.7 s) well inside the noise (±17, ±10 s). Both settings stay on the aac7 line (evidence: the 10-node S22/S31 runs). To resolve at 4 nodes: remove the cause of the bimodality (per-host stage times, nodechk, fixed node set via `-w`); or about 30+ pairs for (i) (about 3.5 h per comparison, mod); or a paired design with A and B back to back on the same nodes using stage-level times. Rule (user): adopt/keep only on a CI that excludes 0 with identical digits. Raw: aac7 `<cluster work dir>/s47/run/`.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/LAUNCH_DEFAULTS.md`.)

### AAC7OPT — the fastest settings on aac7 (main 815b7a0 = 208ae18 code, 2026-10-25 13:53-14:31 ET)

Question (user 2026-10-25): the optimal ecalc settings on aac7. Items b5g_opt1n, b5h_opt1n2 (4 NIC-free nodes, 1 n), b5o_opt and
b5p_tchunk (the two networked nodes); one build each of path 1 (`XSHMEM=ofi`) and path 2 (`XSHMEM=cray`); base env = the aac7 profile line
(`ECALC_NODE_GB=420 ECALC_DEVICE_GB=444.0 MN_T_CHUNK_MB=2048 ECALC_MEM_GUARD_GB=6`); arms in Latin / ABBA order, paired by round;
`ecalc total` of PE 0; no digit file; every run VERIFY OK except the `POOL_LOG=30` arm (rc 6 at start). Labels: all m.

| question | size | pairs | result (arm - base, 95 % CI) |
|---|---|---|---|
| D (default layout) vs P (`ECALC_PE_PER_GPU=1`, path 1) | 1 n 6.6e10 | 4 | P +9.62 s [+4.98, +14.26] (D 74.2, P 83.8 s; dm 16.9 vs 28.4 s) |
| `ECALC_NP=3` / `=4` vs auto | 1 n 6.6e10, D | 8 | -0.19 [-1.23, +0.85] / +7.51 [+4.89, +10.13] (auto = 3 primes here) |
| `BS_SEED_CHUNK_MB=2048` | 1 n 6.6e10, D | 8 | +0.25 [-0.66, +1.17] |
| `ECALC_BG_THREADS=96` / `=24` (default 48) | 1 n 6.6e10, D | 8 | +1.12 [-2.42, +4.65] / -0.39 [-2.16, +1.37] |
| `POOL_LOG=30` | 1 n 6.6e10, D | - | fails at start (rc 6) |
| P vs D | 2 n 5e10 | 3 | P -14.71 s [-21.13, -8.30] (D 54.4, P 39.7 s, -27 %) |
| path 2 (`XSHMEM=cray`, P, `XSHMEM_LANES=2`) vs P (path 1) | 2 n 5e10 | 3 | +13.85 s [+10.76, +16.94] (53.5 s; vs D -0.86 [-8.78, +7.06]) |
| `MN_T_CHUNK_MB=1024` vs 2048 under P | 2 n 5e10 | 3 + 6 | +0.62 s over the 9 pairs (b5o -2.36 [-10.1, +5.4], b5p +2.11 [-1.03, +5.25]) |
| `XSHMEM_ROUND_MB=512` vs 128 under P | 2 n 5e10 | 3 | -0.65 [-11.15, +9.85] |

Capacity (planner, `plan_values.py --fit`, same base env): P fits 2 n up to 5e10 (node 412.5 GB of 420; 5.5e10 refused at 425.8 GB, the
host terms of four processes per node + Q110's staging); D fits 2 n to about 1.2e11 (1.32e11 refused by 1.7 GB); 1 n 6.6e10 fits both.

### P2JUMP — path 2's device need at 64 vs 576 nodes (2026-10-25)

Question (TASKS "Path 2 at the target's scale"): at the same per-node share 1.736e10 (1e13 on 576 n) the planner gives path 2 (`ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`) 380.23 GB per device-node at 576 n against a much smaller need at 64 n. Method (all modeled): `tools/plan_values.py --fit` of an xshmem build of main 1739cd4's planner (ECALC_DEVICE_GB 373.44, ECALC_NODE_GB 414), digits = 1.736e10 x nodes, the terms read from its `plan fit` lines; no node touched. This binary reproduces 380.23 GB at 576 n exactly but gives 276.13 GB at 64 n, not the 293.31 GB quoted in TASKS (cause of the 17 GB difference not found; the 64 n value is the same at 2-64 n and at 128, 256 and 512 n).

**Sweep, device GB per node, path 2 (planes + arena + comm_ofi pools + v-slots; wrap = DM_MN_WRAP's figure when the rule turns it on):**

| nodes | device | planes | arena | ofi pools | v-slots | note |
|---|---|---|---|---|---|---|
| 1 | 178.42 | 103.70 | 68.72 | 0 | 0 | |
| 2-64, 128, 256, 512 (powers of two) | 276.13 | 120.88 | 146.03 | 3.22 | 0 | the same at every power of two |
| 96, 192, 384 | 340.23 | 120.88 | 120.26 | 47.24 | 45.85 | non-power-of-two |
| 448 | 327.28 | 120.88 | 120.26 | 40.80 | 39.34 | |
| 288, 576 | 371.64 (380.23 with the wrap) | 120.88 | 120.26 | 63.35 | 61.15 | node 467 GB (budget 414) |
| path 1 (layout D) 64 / 576 | 232.10 / 238.97 | 120.88 | 103.08 / 96.64 | 2.15 / 9.66 | 0 / 5.79 | same share, `MN_NONPOW2_CAP` has no effect below 4.5e10 per node |

**Cause.** The jump is not the node count itself but "is the group a power of two". A power-of-two group uses `ntt_dist`'s transform, which takes its exchange scratch from the block pool: v-slots 0 (`rns_mul_dist_mn_vslot` returns 0). 576 nodes = 2304 PEs = 9 x 2^8 (and the tree's groups 192 and 576) are not powers of two, so every product uses the general map, whose v-exchange slots (two per APU, 2 x (A + max(A, B)), hipMalloc'd beside the arena, kept to the run's end) are resident: 1.92 GB per APU in layout D (K = 8 chunks) but 15.29 GB per APU under one process per GPU, because `DIST_CHUNKS` defaults to 1 under vPE (ntt_dist.c, rns_dist.c: "one exchange per transform") and the slot size is proportional to 1/K (15.29 / 1.92 = 7.97; the Python model with K = 1 gives 15.31). The v-slots set the comm_ofi pools too (`XSHMEM_HEAP_MB` 15104 = 256 MiB + 2 x 7291 MiB slot, per APU) and the planner books them twice: 61.15 + 63.35 GB. The arena shrinks by 25.8 GB at the same time (general-map grid), so the net is +95.5 GB (371.64 vs 276.13) and +104.1 with the wrap. 96 = 3 x 32 is already +64 GB; 448 = 7 x 64 is +51 GB. 576 = 9 x 64 is the worst because the top group and the 192 group both stay general-map; nothing here is the 576-node scale itself.

**Fix options (modeled, path 2 at 1.736e10 per node, 576 n):**

| setting | device | node | v-slots | ofi pools |
|---|---|---|---|---|
| default (`DIST_CHUNKS` unset = 1) | 380.23 refused | 467.17 | 61.15 | 63.35 |
| `DIST_CHUNKS=2` | 318.56 fits | 414.09 | 30.62 | 32.21 |
| `DIST_CHUNKS=4` | 288.22 | 383.76 | 15.32 | 17.18 |
| `DIST_CHUNKS=8` | 273.06 (= the 64 n figure less 3) | 368.59 | 7.67 | 9.66 |
| `DIST_CHUNKS=16` | 264.94 | 360.47 | 3.84 | 5.37 |

**A3 re-run on the built cand3 planner (2026-10-28, `ECALC_PE_PER_GPU=1 SLURM_NTASKS_PER_NODE=4 XSHMEM_LANES=2 ECALC_FIT_PER_APU=1`, target limits device 373.44 / node 487, 576 n, planner output = mod; the device / node figures reproduce the table above to the 0.01 GB).** GB per node (per busiest APU against 93.36); "unset" is `DIST_CHUNKS` not set (= 1 under vPE):

| digits | `DIST_CHUNKS` | device (per APU) | node | verdict, per-APU margin |
|---|---|---|---|---|
| 4.608e12 (8e9 per node) | unset = 1 / 2 / 4 / 8 | 328.69 (82.17) / 267.02 / 236.68 / 221.52 | 406.32 / 344.65 / 314.31 / 299.15 | fits, +11.19 / +26.61 / +34.19 / +37.98 |
| 1e13 (1.74e10 per node) | unset = 1 | 371.64 (92.91) | 467.17 | fits by +0.45 per APU (+1.80 node-mean); the wrap (380.23) is off by the auto rule, so the older "refused" figure was against the old 414 GB node budget |
| 1e13 | 2 / 4 / 8 | 318.56 (79.64) / 288.22 (72.06) / 273.06 (68.26) | 414.09 / 383.76 / 368.59 | fits, +13.72 / +21.30 / +25.10 (wrap on) |
| 3e13 (5.2e10 per node) | unset / 1 / 2 / 4 / 8 | 611.02 (152.75) / 611.02 / 519.01 / 473.58 / 450.29 (112.57) | 741.94 / 741.94 / 649.93 / 604.50 / 581.21 | REFUSED at every `DIST_CHUNKS` up to 8 (-59.39 ... -19.21 per APU; node over 487 too) |
| path 1 D reference: 4.608e12 / 1e13 | default | 135.54 (40.33 busiest) / 238.97 (74.24) | 160.54 / 268.12 | fits, +53.03 / +19.12 |

Reading: the 8e9-per-node campaign shape fits path 2 with 11 GB per APU to spare even at `DIST_CHUNKS` unset; 1e13 fits only barely unset and comfortably at 2 or more; 3e13 does not fit path 2 at any slicing (the tree term, see the 6.632e10 note below). `DIST_CHUNKS` > 1 under vPE is still not run (see Digit safety). `DIST_CHUNKS=16` gives 264.94 GB (66.23 per APU, +27.13) at 1e13 and 439.22 (109.80, -16.44, REFUSED) at 3e13. `XSHMEM_LANES` 1 / 2 / 4 does not change any planner figure (checked at 1e13 and 3e13, `DIST_CHUNKS` 1 and 16).

`MN_NONPOW2_CAP=0` changes nothing here (it is off by auto below 4.5e10 per node). `COMM_ALLTOALLV_DEPTH=1` changes nothing in the planner. `MN_GROUPS` cannot help: the top group must be all 576 nodes. A power-of-two node count (512) avoids it (276.13) but is not the target. Digit safety: `DIST_CHUNKS` only re-slices the rows of one exchange (digits were identical for 4 -> 8 on layout D, README), but under vPE K > 1 has not been run, the K = 1 default was chosen for message size (64 KiB at K = 8), and the effect on time is unmeasured: it needs the gate at 3 nodes (12 PEs, general map) with `ECALC_PE_PER_GPU=1` and an ABBA at 3 n before anything is adopted. Smallest change that fits: `DIST_CHUNKS=2` (node 414.09 against 480 GB real budget), or 4 (margin +85 GB device).

**The 6.632e10 per node share (earlier note), same binary:** device with `DIST_CHUNKS` unset, `MN_NONPOW2_CAP` auto (on from 4.5e10): 362.03 (2 n), 430.75 (8 n), 456.52 (64 n), 671.15 GB (576 n; planes 162.22, v-slots 91.69); with `MN_NONPOW2_CAP=0` 576 n is 560.62 and with also `DIST_CHUNKS=8` 453.45 GB (v-slots 7.67). The growth from 2 to 64 n at that share is the tree term of path 2 (arena 232 / 301 / 326 GB at 2 / 8 / 64 n, tree 44.95 / 63.74 / 70.02 per APU) while layout D stays at 360.95 GB: path 2's plan runs the tree over the nodes (`plan pe` line: the groups below a node are not modeled until the PE-level products exist), so each deeper level adds a bigger product per PE; the dm need (55.96 GB per APU) is the same in both. No K or cap setting removes that: path 2 needs a PE-level tree model (and then a smaller real footprint) to fit 6.6e10 per node; values here differ by 8-9 GB from the earlier note's (447.93 / 473.70 / 517.67), a planner or settings difference not traced.

### A2EDGE — record memory edge per APU at 3e13 and 3.82e13 on 576 nodes (2026-10-28, planner, mod)

**Result 1, the planner's per-APU peak is the busiest APU (region 0) unless `BS_MDEV_LOGL` balances it.** Per-APU max / margin to 93.36 (GB), `MN_NONPOW2_CAP=0`, `MN_T_CHUNK_MB` 512 or 1024 (no effect at `DIST_CHUNKS` 8 / 16), `DM_MN_WRAP` auto:

**Recommendation (mod).** For 3.82e13: `BS_MDEV_LOGL=28`, `MN_NONPOW2_CAP=0`, `DIST_CHUNKS=16` gives 85.98 GB per APU (+7.38 to the 93.36 edge; node-mean device 343.92 of 373.44; node 380.01 of 487), the only 3.82e13 setting above the 6.5 GB bar; today's S6 line (`BS_MDEV_LOGL=29`, default `DIST_CHUNKS`) has 89.74 (+3.62), below it. `BS_MDEV_LOGL=28` with the default `DIST_CHUNKS` has +5.77 (also below the bar). For 3e13: `BS_MDEV_LOGL=29` (or 28), cap 0, default `DIST_CHUNKS` is +7.92 (above the bar); `DM_MN_WRAP=0` with `DIST_CHUNKS=16` +18.12. Unproven: `DIST_CHUNKS=16` on path 1 layout D has not been timed or digit-checked (README: digits identical for 4 -> 8; the exchange messages halve against K=8's 64 KiB) and `BS_MDEV_LOGL=28` at scale has not run; both need the 1-node gate with `BS_LAYOUT_ONLY` / a 1e11 ABBA, and the 2-node gate, before they join ROUTE S6. Not adopted by this item (planner only).

### RECORD1 — the first record size and the staircase of the copy-and-paste route (2026-10-28)

Question (user, 2026-10-28): "The first record run should be a number of digits you are 99% sure will finish without crashing. We can close the memory gap with a small number of slightly larger runs." Decided on the evidence below; the route is `remote/ROUTE.md` (P3 = step 1, P4 = steps 2-4; the env words per step are in its table only).

**Planner sweep** (modeled; cand3 path 1 build on aac7, `plan_values.py --fit`, target limits device 373.44 / node 487, `ECALC_FIT_PER_APU=1`, `MN_NONPOW2_CAP=0`, 576 nodes; one run per cell, item rvp_packs3). Busiest APU in GB (margin to the 93.36 edge), by `BS_MDEV_LOGL` / `DIST_CHUNKS`:

| digits | unset | 29 | 28 | 28 + `DIST_CHUNKS=16` |
|---|---|---|---|---|
| 4.608e12 (verified on the target) | 40.33 (+53.03) | 40.33 | 36.03 (+57.33) | 36.03 |
| 1e13 | 74.24 (+19.12) | 63.50 (+29.86) | 54.91 (+38.45) | 53.77 (+39.59) |
| 1.2e13 | 80.68 (+12.68) | 63.50 (+29.86) | 57.06 (+36.30) | 55.92 (+37.44) |
| 1.5e13 | 89.74 (+3.62) | 63.97 (+29.39) | 61.82 (+31.54) | 60.21 (+33.15) |
| 2e13 | 89.74 (+3.62) | 74.71 (+18.65) | 74.71 (+18.65) | 73.10 (+20.26) |
| 2.5e13 | 87.59 (+5.77) | 79.00 (+14.36) | 79.00 (+14.36) | 77.39 (+15.97) |
| 2.8e13 | 89.74 (+3.62) | 83.30 (+10.06) | 83.30 (+10.06) | 81.69 (+11.67) |
| 3e13 | 89.74 (+3.62) | 85.44 (+7.92) | 85.44 (+7.92) | 83.83 (+9.53) |
| 3.4e13 | 89.74 (+3.62) | 89.74 (+3.62) | 89.74 (+3.62) | 88.13 (+5.23) |
| 3.7e13 | 89.74 (+3.62) | 89.74 (+3.62) | 85.44 (+7.92) | 83.83 (+9.53) |

### PIECES — piece boundaries and memory ceilings at 576 nodes (2026-11-01, planner on the aac7 login node, mod)

Question: where do the piece counts of the distributed products change with the digit count, what is the per-APU memory at each size, and where is the ceiling? Planner only (`MN_PLAN_ONLY=<digits>:576`, no compute nodes), built fresh from main (the existing build objects were older than the sources and printed no per-APU line). Settings: `MN_NONPOW2_CAP=0 DM_MN_WRAP=0 MN_LEVEL_UNIFORM=1 XSHMEM_ROUND_MB=128 ECALC_FIT_PER_APU=1 ECALC_DEVICE_GB=373.44 ECALC_NODE_GB=430 ECALC_NP=auto MN_T_CHUNK_MB=2048`. Labels: mod throughout (layout arithmetic, not a target measurement). The aac7 build counts 0 for the target's device pools (9.66 GB per node, 2.4 GB per APU), so a target run needs that much more than shown.

Pieces are the rows × columns of the largest product of node 0's groups (levels 1-6 together, then the 192-node level 7 and the 576-node level 8); they count one operand pair, so read the boundaries, not the absolute numbers.

| digits | pieces, levels 1-6 / 7 / 8 | mean per APU (GB) | busiest APU (GB) | margin of the busiest to the 93.36 GB edge (GB) | verdict |
|---|---|---|---|---|---|
| 2.6e13 | 1 / 2 / 1 | 77.66 | 85.18 | +8.18 | fits |
| 2.7e13 | 1 / 2 / 2 | 80.34 | 87.32 | +6.04 | fits |
| 2.8e13 | 1 / 2 / 2 | 80.88 | 87.32 | +6.04 | fits |
| 2.9e13 | 2 / 3 / 2 | 81.95 | 87.32 | +6.04 | fits |
| 3.4e13 | 2 / 3 / 2 | 84.10 | 87.32 | +6.04 | fits |
| 3.6e13 | 2 / 4 / 2 | 85.18 | 87.32 | +6.04 | fits |
| 3.7e13 | 2 / 4 / 2 | 86.25 | 87.32 | +6.04 | fits |
| 4.0e13 | 3 / 5 / 3 | 87.32 | 87.32 | +6.04 | fits |
| 4.4e13 | 4 / 6 / 3 | 93.77 | 93.77 | -0.41 | REFUSED (node 375.06 GB vs 373.44, -1.62) |

**Where `BS_MDEV_LOGL=28` starts to cost on one node (measured, aac7 1 node, 2026-10-28, item c06, remote-v7 d6167b2, same method as above: A = `BS_MDEV_LOGL=29`, B = `BS_MDEV_LOGL=28 DIST_CHUNKS=16`).** Means per arm, seconds; digits identical within every size (6.632e10 ...79932777543051118449; 8e10 ...52640279071809056008; 9e10 ...32983517734368241080):

| digits | rounds | total A / B | division A / B | B - A (95 % interval) |
|---|---|---|---|---|
| 6.4e10 (above) | 2 | 78.10 / 77.24 | 19.30 / 19.32 | -0.86 [-4.05, +2.34] |
| **6.632e10 (the 3.82e13 per-node share)** | 2 | 76.36 / 75.82 | 18.09 / 18.05 | -0.55 [-1.76, +0.66] (-0.7 %) |
| 8e10 | 2 | 99.28 / 98.15 | 28.97 / 28.44 | -1.13 [-13.10, +10.84] (the wide interval is one round pair of 0.2 s and one of 2.1 s) |
| 9e10 | 1 | 106.29 / 106.89 | 29.25 / 29.66 | +0.60 (1 round, no interval) |
| 1e11 (above) | 2 | 136.47 / 149.84 | 42.90 / 57.39 | +13.37 [-0.46, +27.20] |

Reading: at the record's per-node share (6.632e10) `BS_MDEV_LOGL=28` costs nothing resolved (-0.55 s, interval +-1.2 s) and the same holds to 9e10; the division jumps from about 30 s at 9e10 to 57 s at 1e11, so the cost starts between 9e10 and 1e11 per node (the cliff, not a slope; not narrowed further). So at 6.632e10 the +2.15 GB per APU of `BS_MDEV_LOGL=28` is free on one node; its behavior in the 576-node tree (different levels) is still untested. Decision on ROUTE S6 stays with the user.


**Mechanism and 576-node verdict for the `BS_MDEV_LOGL=28` cliff (2026-10-28, items c07 and c08, cand3 remote-v8 13322ef merged into c3-plan, aac7 NIC-free hold, 1 node).**

- (a) Planner diff, `MN_PLAN_ONLY`, aac7 limits, 28 vs 29 (modeled): at 9e10 and 1e11 the only change is leaf level 22, whose products (4.6e8 to 5.3e8 limbs, between 2^28 and 2^29) leave the batch tier and become device-tier dist_db products on 2^29-point planes (25.77 GB each). Leaf dist_db product counts rise by 20 at 1 node (for example 30 to 50, 34 to 54, 36 to 58 across the sizes tried). Tree top levels, reciprocal products, division products, grids and plane caps are identical. The plan therefore contains no division-side change; any division cost would come from device-tier leaf traffic interfering, not from a planned shape.
- (b) Placement (measured, 1 node, paired ABBA, digits identical in every run): 9.5e10, 1 round, B - A = +2.28 s total (dm 36.8 vs 38.2 s, not a division cost). 1e11, two new one-round pairs with the arm order swapped: +5.59 s (28 as B: 127.9 vs 122.3 s, bs +2.7 s, dm +2.8 s) and +3.87 s (28 as A: 126.6 vs 122.7 s, bs +1.6 s, dm +3.8 s), about +3 to +5 %. The earlier +13.4 / +16.3 s with division +14 s (c05) did not reproduce. Run-to-run spread within an arm is 1 to 3 s and aac7 has a documented slow mode (about +4 s on one run in three), so the earlier cliff is most likely a slow-mode artefact plus a small real cost; it is not located between 9.5e10 and 1e11 and not explained by a plan change. Doubt: 2 pairs at 1e11, no interval.
- (c) 576 nodes (planner, target limits, modeled): the same shape appears. At 3.82e13 level 22 gets 12 more dist_db leaf products (14 to 26) and at 3e13 8 more (10 to 18); the top tree levels and division products are unchanged by the switch. So the switch can reach the 1-node shape at 576 n, but it adds only leaf-tier device traffic, the same thing measured at 1 node as +3 to +5 % at 1e11 and no resolved cost at 6.632e10. Verdict: expect at most a few percent, no division cliff by mechanism. Not measured at scale; a 64-node run at 6.632e10 per node (A 29 vs B 28) is the cheap check and has not been run. The ROUTE S6 choice stays with the user.

**Settled: `BS_MDEV_LOGL=28` costs about +5.5 s per 1e11 digits on one node (measured, 2026-10-28, items c09 on s3b1 and c10 on s5b0, remote-v9 9d34130 builds, path 1, ABBA, A = `BS_MDEV_LOGL=29`, B = `BS_MDEV_LOGL=28 DIST_CHUNKS=16`, digits identical in every run).** The c05 cost (+13 to +16 s, division +14 s) did not recur.

| set | rounds | A total | B total | B - A (95 % interval) |
|---|---|---|---|---|
| c09, s3b1 | 5 | 120.9 s | 126.7 s | +5.84 [+4.63, +7.04] s, +4.8 %; rounds +7.13 +6.12 +5.94 +5.55 +4.45 |
| c10, s5b0 (4 NICs) | 3 | 121.1 s | 126.5 s | +5.46 [+2.59, +8.33] s, +4.5 %; rounds +5.66 +4.22 +6.50 |
| c08, s3b1 (one-round pairs, both orders, remote-v8) | 2 | - | - | +5.59, +3.87 s |

## TARGET — the target (digits and 576 nodes)

Owner of items TGT, TGT17, TGTBENCH2, CAP17, SC15, MS15, DOC15, L13d, G13d, S13d (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; originals of this file: `archive:results_merged_2026-10-19/`. Labels: m measured, mod modeled, a assumed (u = user-supplied). Times Eastern.

### Topic summary

- **The target now:** 3.82 × 10¹³ digits on 576 nodes (4 MI300A each, 2304 APUs), `PLAN.md` §1; standing estimate about 182-185.5 s without the write (mod, profile `target_l4`, fabric assumed: `results/MODEL.md §L4`). The target moved over time and most sections here are history at the older sizes: 4.25 × 10¹³ (Phase 14), 5.1 × 10¹³ (2026-09-27; section removed 2026-10-25, `archive:results_merged_2026-10-25/TARGET_removed.md`), 5.276 × 10¹³ (earlier headline), 4.08 × 10¹³ (TGT17, 2026-10-06), 3.76 / 3.71 × 10¹³ proposed (TGTBENCH2), then 3.82 × 10¹³ (user, current). Figures at the older sizes are superseded; keep them only as method and as the step structure.
- **Device-memory edge (live):** `hipMalloc` stops at 93.36 GB per APU = 373.44 GB per node (A6, m on the target, n = 5, 0 % spread); VMM shares the edge (aac7 444.0 vs hipMalloc about 448.0 GB, `results/MODEL.md §FIT`). This is `ECALC_DEVICE_GB` (default 373.44). The binding bound is the device layout (planes + arena + transport pools + v-slots), not the 480 GB node budget. Current fit at 3.82 × 10¹³: device 358.96 GB with the wrap off, margin +14.48 GB (planner, mod; `results/MODEL.md §FIT`); with slot release the rank-0 margin was 12.9 GB (`§L1` there).
- **Layout structure (live):** device and piece counts are tiered, not smooth: the arena steps by one 4.29 GB chunk per node at grid steps and the critical path steps by pieces. Always size by `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` (login node, no HIP) and `mem_model.py --check-c` (exact, 0.0000 % at every point tested here); `mem_model.dev_max` omits the transport pools (C `binsplit_fit` is the source of truth). Trap (measured with the removed `COMM_TRANSPORT=shmem`): without the transport variable in the environment the `room:` line silently omitted the transport pools (9.66 GB short, still printed `fits 1`); the planner now reads `COMM_TRANSPORT=xshmem` (`binsplit.c` `xsh_on`), so set it for `MN_PLAN_ONLY` / `BS_LAYOUT_ONLY` (`mnrun.sh` does).
- **Launch-environment facts (TGT, TGTBENCH2):** user-supplied hardware: 8 Slingshot NICs per node at 400 Gb/s (2 per APU, 100 GB/s injection peak per APU), Rocky 9.6, Slurm 25.11.8, ROCm 7.14.0, cray-shmem 12.1.0, CCE 21.0.0, libfabric 2.3.1, file system about 1 TB/s into one file at 32 writers per node. the NIC map (`pe_geo.c`) takes the NUMA-local cxi devices when `COMM_OFI_NICS` is unset (two NICs per APU without a setting; written for the removed comm_ofi). ROCm 7.14.0 built the then-current SHMEM build (not run on a compute node then). Proposed (not adopted then): `FI_UNIVERSE_SIZE=4096`, `FI_LOG_LEVEL=warn`, `SHMEM_SYMMETRIC_SIZE` matching our heap.
- **Rejected after review (TGTBENCH2):** hipMallocManaged host spill (host-backed memory 21 GB/s vs HBM 3.2-3.8 TB/s, 0.114 s/GB to map; the 421.5 GB layout it rested on was stale); the harness's 251.5 s estimate (contradicts the 18.6 TB per node of NIC volume: wire time alone 186 s at 100 GB/s per node); microbenchmark kernel rates as model inputs; SDMA for xGMI (our push 909-1014 GB/s per node). Adopted constants behind `--fabric target-m`: `TARGET_DEVICE_EDGE_GB = 373.44`, MAP_RATE 0.010 s/GB (no wall change: the seeds bind), host pin 256 GB per APU (info).
- **Biggest open uncertainty:** the fabric on the target (A3 injection on 8 NICs, A4 all-to-all scaling to 64+ nodes) is unmeasured; every 576-node wall is modeled with assumed fabric terms (fabric 50-200 GB/s per APU: about +70 / -35 s at 5.1 × 10¹³).
- **Batch-3 ideas of SC15/MS15 that became current:** P24 (four primes at 24 digits per point, `results/MODEL.md §DEF`, `results/COMPUTE.md`) and DKM division halves were the top-ranked items; `BS_ARENA_ROOM`, `DIST_TWREC_G`, `MN_T_CHUNK_MB=2048` followed. The "no transform cache" baseline and the slot memory discussion are history.
- **Older-hardware items (L13d, G13d, S13d):** the plan printer `MN_PLAN_ONLY` (`ecalc/mn_plan.c`) is live and validated 9 of 9 against real runs; G13d measured the grid steps on one aac6 node (division/reciprocal steps real, model's tree-top re-grids not); S13d showed Sandia OpenSHMEM works across real nodes and OpenMPI OSHMEM 4.1 hangs. aac6 is set aside; these are method and history.

### TGT — target facts (user-supplied 2026-10-12) and what they change

Question: where do the user's target facts go, which backlog rows change, what does the digit write cost against the file system rate, does any launch value change. Branch `tgt`, merged; no code or default changed.
- Change: `mn_model.py` constants `TARGET_NICS_PER_APU=2`, `TARGET_NIC_GBS=50` (so `TARGET_BW_APU` stays 100 GB/s), `TARGET_FS_SINGLE_FILE_GBS=1000` at 32 writers per node (`TARGET_WRITE_BW_FS` 1.736 GB/s per node, now printed beside the write rates); `TARGET_WRITE_BW` (1.0 GB/s per node, ecalc's writer) unchanged. `profiles/target.conf.template` keys filled; `COMM_OFI_NICS` no longer set there.
- ROCm 7.14.0 (m, aac7 login node): `ecalc` and `make all` build with `-DCOMM_SHMEM`; the `ld.lld` relocation failure of `results/TRANSPORT.md` does not reproduce. Not run on a compute node.
- ecalc's writer: one process per node, 8 pwrite threads per 256 MB chunk (`MN_OUT_THREADS=8`, `MN_OUT_CHUNK_MB=256`), O_DIRECT on Lustre, two part files per node (1152 files in all): 8 writers per node into per-node files, not 32 into one.
- Write cost at 3.82 × 10¹³ (15.92 TB packed, 27.63 GB per node; no-write wall 185.5 s, mod):

| write rate per node | raw write | wall with write | exposed |
|---|---|---|---|
| 1.736 GB/s (the file system's 1 TB/s, u) | 15.9 s | 186.5 s | 1.0 s |
| 1.0 GB/s (model prior, a) | 27.6 s | 198.3 s | 12.8 s |
| 0.6 GB/s (old single-stream prior, a) | 46.1 s | 216.7 s | 31.2 s |
| 0.434 GB/s (8 of 32 writers scaling, a) | 63.7 s | 234.3 s | 48.8 s |

- Fabric: 100 GB/s per APU is now the user-supplied peak, so the estimate is unchanged; with one effective NIC per APU (50 GB/s) 222.7 s, +37.2 s (mod).
- Plan values (m, planner): `COMM_SHMEM_POOL_MB` 2304, heap 2816 MiB, `FI_UNIVERSE_SIZE` 4096, `MN_GROUPS` 2,4,8,16,32,64,192,576, pool_log 31, plan check OK, 1236 products; with `COMM_OFI=0`: pool 10240 MiB, heap 10752 MiB, OK.
- Verdict: no default changes. Three items for target work: verify two NICs per APU (M7c, +37 s at stake), benchmark ecalc's writer shape against the file system (N8, writer design gap 11.8 s vs the 1.0 GB/s prior and up to 47.8 s vs the writer-scaled case, mod), measure the SHMEM path and 12.1.0 (N7); and ROCm 7.14.0 vs 7.2.4 at 1 node (M7d). Open: the module name behind "cray-shmem 12.1.0" is assumed `cray-openshmemx/12.1.0`. Fixed (FIX1): loader accepts `TARGET_PARTITION`, `TARGET_NODES`, `ECALC_OUT_DIR` as meta keys; `mn_model.py estimate --target` / `mem_model.TARGET_DIGITS` use 3.82 × 10¹³.

### TGTBENCH2 — the user's second set of target tests, assessed against the ecalc design (2026-10-06, branch `tgtbench2`)

Input: the user's tests on the target (576-node MI300A, 8 Cassini NICs per node) with a generic benchmark harness, not ecalc (`archive:results/TGTBENCH2_raw.md`). Base `origin/b7-vslot` c058964; login-node work only. Proposals only; no runtime change.

Summary:
1. Device edge confirmed exactly: 93.36 GB per APU = 373.44 GB per node (n = 5, 0 % spread, m target). With B7 v-slots (6.707 GB per APU = 26.83 GB per node) the then-current 4.08e13 has C-layout device 372.12 GB (m), 1.32 GB under the edge, before about 0.6 GB per APU B7ACCT could not explain.
2. The harness's "421.5 GB, 26 % oversubscribed, hipMallocManaged spill" is stale and wrong: it is the old 5.276e13 layout (before `DM_MN_LEAN`); 421.5 × 576 = 242.8 TB (not 265.4), real oversubscription 12.9 %; host-backed memory 21 GB/s for `hipMemcpy` against 3.2-3.8 TB/s HBM and 0.114 s/GB to map (ME10, m aac6). Spill rejected; cheaper levers exist (free each level's v-slots, `DIST_CHUNKS=8`: 11.5-19.5 GB, mod; `ECALC_PLANE_CAP=2^30`: -60 GB, about +80 % wall, mod).
3. The harness's "251.5 s, communication 1.0 s at 25 GB/s" contradicts the volumes: the model moves 18.6 TB per node through the NICs at 4.08e13 (2.3 TB per NIC; aac7 measured 2.47-2.63 TB per NIC at 10 nodes, m, cxi counters), so wire time at 25 GB/s per APU is 186 s; the model's wall at 25 GB/s is 369.6 s (199.6 s exposed). Their compute 250 s comes from a 64 K-point NTT microbenchmark. A3 (fabric injection) and A4 (64-node scaling) remain unmeasured: the top uncertainty (`--fall-off 0.3` costs ×1.9 at bw 47).
4. MAP_RATE 0.010 s/GB (m, target) is now profile `target-m`; init falls 1.0 s but the seed wait grows 1.0 s (CPU seeds end at 7.589 + 0.2681 × D_leaf/10⁹ ≈ 27.3 s), so the wall is unchanged (0.0 s; modeled device under `target-m` 370.5 GB).
5. Launch-line findings (Slurm-native `srun --ntasks` fixes the `fi_enable(-28)` issue for on-node multi-PE launches; our line is already Slurm-native, one PE per node; no multi-node launch reported yet).

Item verdicts (compact):
- Constants: DEVICE_MEM adopted (373.44); VMM_MAP_RATE adopted as profile; HOST 256 GB per APU recorded; PROCESS_OVERHEAD 28 GB vs our 86 GB modeled per engine process not comparable (question to user); NTT 655 GB/s (n = 65536, L2-resident) and transpose 2602 GB/s rejected (ecalc planes are 4-17 GB; 2³¹ transform 117 ms, modmul 1.31-1.34 Top/s per APU, m); INJECTION 25 GB/s is an assumption.
- C1 baseline (μ = 80.801 s at 1e10, CV 5 %) is not ecalc (ecalc 25.98-27.71 s at 7.0.3, 26.18-26.27 s at 7.2.4, m aac7); the "1.22 validates the factor" claim contradicts records (1.22 is the 7.0.3 / 7.2.4 ratio at 1e11: 205.9 vs 168.5 s; at 1e10 they match). C2 thermal: 300 s, +1 °C, no throttling, supports a 4-5 min run (partial).
- B2 device-heap vs host-staged: comm_ofi writes 4 MiB chunks from a fine-grained device pool; `COMM_OFI_POOL=host` exists (NIC16: -6 % off-NUMA on aac7); settle with `t_comm --bw` fine vs host at 2 nodes. Barriers, collectives, atomics: low relevance (one PE per node). SHMEM intra-node and SDMA: ecalc never sends intra-node bytes through SHMEM; our push 909-1014 GB/s per node (244 per APU, 93-97 % of links, m), SDMA at 50-58 GB/s per pair gives at most 174 GB/s per APU; xGMI stage already 72-76 % hidden.
- libfabric: comm_ofi sets CQ size 8192 explicitly and FI_AV_TABLE count 0; `FI_CXI_DEFAULT_CQ_SIZE` does not reach it; 100 ms HIP-init stagger does not apply (one PE per node).
