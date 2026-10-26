# target/ladder/ADJUST.md — what to change between ladder runs, and when to move up

Scope: the rules the agent on the target applies after each `ladder.sh` run: which measured number leads to which change,
and the gate to the next level. Commands: `FORMATS.md` §9-§10, the header of `ladder.sh`. Labels: m measured, mod modelled,
a assumed. The session may end at any time: never wait for a perfect level, move up as soon as the gate holds.

Loop at each level: submit (`ladder.sh --level L0`) -> the DONE markers appear -> `ladder.sh --collect L0` (one summary,
appended to `results/ladder/LADDER.md`) -> read the table below -> change at most the settings the numbers call for
(`--set KEY=VALUE`, one change per re-run, so the effect is attributable) -> re-run at the same level, or move up.
Commit `results/` after every collect (the coordinator does the git work).

## 1. Predictions (mod): `python3 ecalc/mn_model.py estimate --g 1 8 64 576 --D 8e9 --fabric target`

The first campaign runs 8e9 digits per node (user decision Q120: every code path fits at every level; raise the size
only after all levels pass on both paths).

| level | nodes (APUs) | digits | wall without write (mod) | wall with write (mod) | exposed fabric (mod) | device per node (mod, D / path 2 planner) |
|---|---|---|---|---|---|---|
| L0 | 1 (4) | 8e9 | 16.8 s | 19.9 s | 0 | 155 / 144 GB |
| L1 | 8 (32) | 6.4e10 | 22.6 s | 24.6 s | 1.5 s | 178 / 216 GB |
| L2 | 64 (256) | 5.12e11 | 25.8 s | 27.6 s | 2.5 s | 178 / 216 GB |
| L3 | 576 (2304) | 4.608e12 | 32.0 s | 33.2 s | 6.5 s | 181 / 328.69 GB |
| L3 s2 | 576 (2304) | 1e13 | 52.3 s | 55.3 s | 12.5 s | 229 / refused (380.23) |
| L3 s3 | 576 (2304) | 3.82e13 | 187.1 s | 191.2 s | 51.6 s | 390 (planner 358.96 with cap 0) / refused |

Assumptions inside the model (a): 100 GB/s per APU, 2 us per message, dragonfly group 64, part files 1 GB/s per node. The
model was calibrated on aac7/aac6 compute at larger sizes; at 8e9 per node start-up is a large share of the wall, so a
measured/model ratio away from 1 at L0 is expected; what matters is the growth from level to level: model L0 -> L1 +35 %,
L1 -> L2 +14 %, L2 -> L3 +24 % of wall. A level that grows much more than that has a fabric, launch or placement problem.
L3 runs as a staircase (user decision Q121; commands in RESTART.md §8): s1 4.608e12 on both paths, s2 1e13 and s3 the
3.82e13 headline on path 1 (layout D, `MN_NONPOW2_CAP=0`, Q119), each step only after the previous one passed the gate.
Model s1 -> s2 +63 %, s2 -> s3 3.6x of wall. For the headline at the other levels (later): L0-L2 at 6.632e10 per node model
102.7 / 118.2 / 149.6 s without write.
`collect` prints the measured / model ratio per path; the model figure of `L<n>.conf` is for the conf's digits only.
The model has no term for `ECALC_PLANE_CAP=2^30` (Q122): expect measured/model about 2.6 s lower than the default would give at
L0 (aac7 1 n 8e9, m); the growth from level to level is the check.

## 2. Gate: move up when all of these hold (a: thresholds are ours, change them on evidence)

1. At least 2 replicas (L3: the one run) with `VERIFY OK` on every run of the path that is taken up, and `collect` says
   `digit hash agree` for that path; the head of e matches (`e_prefix_check PASS`). A single `VERIFY FAILED` blocks the level:
   stop, keep the log, go to row 1 of the table.
2. Spread (max - min) / mean of `total` across replicas and runs of the path: below 5 % at L0/L1, below 8 % at L2. Above it: row 3.
3. Mean `total` within 1.5 x the model of the same level (a, wide on purpose: the model is uncalibrated on this machine). Beyond
   it: do not move up, find the phase that is slow (bs / dm / dc / init, `*_share` keys) and use rows 4-7.
4. `dev_peak_gb` (largest phase boundary of any run) at least 10 GB below `ECALC_DEVICE_GB` (373.44), and no rc 8 / rc 9.
5. No slow host left in the group (`slow_hosts=none`), or the host is in `results/ladder/exclude.txt` and the level was re-run.
6. L0 only: both paths ran (`PATH=both`); the winner is taken to L1. L1/L2: confirm the winner, the other path may drop.

Which path to take up: the one with the lower mean `total` at L1 if the gap is outside the replica spread, else path 2 (the
target's shape) for L2/L3. On aac7 at 1 node layout D (path 1) was fastest; at 2 nodes P on path 1 was fastest and path 2 was
+7.5 % (m, `results/LAUNCH_DEFAULTS.md` §AAC7OPT); the target decides. Set it with `--path`.

## 3. Rules between runs

Keys in the summary are `p<path>r<run>.<key>` (`raw/<id>.log` is the full output, `raw/<id>.pernode.tsv` the per-node `dc` / `T1`).

| # | what the numbers show | meaning | change before the next run |
|---|---|---|---|
| 1 | `verify` = FAIL or NONE; `digits_hash` differs between replicas; `e_prefix_check` FAIL | wrong digits (or the run died before the check) | Stop climbing. Save logs. Re-run once at the same level on a different group (`--exclude` the hosts of the failing replica). Same failure on another group: set `--path` to the other path; if both fail, `FI_MR_CACHE_MONITOR` must still be `userfaultfd` (never `disabled`), try `ECALC_NP=4`; report the first error line (`first_error`) to the coordinator. One failure on one group only: that group's nodes go to `exclude.txt`. |
| 2 | rc 4 from ladder.sh, or `plan REFUSED`, or rc 8 | the memory plan does not fit | ladder.sh already printed the largest fitting digits: re-run with `--set DIGITS=<that>`; or a smaller pool/chunk (`--set MN_T_CHUNK_MB=1024`, costs ~0.6 s, m aac7). At L3 the code default is refused (device 407.41 GB vs 373.44, mod planner) and `L3.conf` carries `ENV_MN_NONPOW2_CAP=0` (user decision Q119, 2026-10-25); L0-L2 are powers of two and unaffected. |
| 3 | spread across replicas above the gate, one replica slower, `slow_hosts` names a host | straggler node or a bad placement | Put the host in `results/ladder/exclude.txt` (one host per line; `ladder.sh` and `locality.py pick` read it), re-run the level. Same group slow twice: another group (`--exclude`). All groups equally slow: no node problem, rows 4-7. A host that is slow only in `dc` (the upload / write stage) = file system or its NIC. |
| 4 | `init_s` above 25 s at L0 (aac7 24 s, m) or growing with the level | VMM / pinned-memory mapping, seed upload | Compare L0 hosts (`m.*_total` per replica); a host above the median by > 3 s goes to the exclude list. Path 2 init gap (+8 s on aac7) is expected; if it is larger, prefer path 1 for the run. |
| 5 | `bs_s` high, rest as modelled | compute-bound leaf phase | `--set ECALC_NP=3` or `4` (aac7: no difference at 1 node, m; four primes cost +17 GB planes); `--set BS_SEED_CHUNK_MB=2048`: both tried on aac7, within noise — try only if bs is > 1.3 x its share in the model. |
| 6 | `dm_s` (reciprocal + division) high, or `dm_share` rises with the level | fabric time in the distributed division | `--set MN_T_CHUNK_MB=1024` (vs 2048); `--set XSHMEM_ROUND_MB=512` (aac7: no effect, m); path 2: `--set XSHMEM_LANES=1` vs 2 to see if both NICs of an APU help; `MN_GROUPS` (the tree schedule; default is the plan's pick, change only with a measurement: `tools/plan_values.py` prints it). Level step beyond the model (section 1) with a large `dm` rise = the fabric rate is below the assumed 100 GB/s per APU: record it, it feeds `mn_model.py` (`--bw`). |
| 7 | `dev_peak_gb` within 10 GB of the limit, or the plan margin below +10 GB | memory edge | `--set MN_T_CHUNK_MB=1024` (smaller exchange buffers), `--set ECALC_NP=3` if it was 4; at non-power-of-two groups `MN_NONPOW2_CAP=0` (3 x 2^k nodes; this also is what L3 does); last: fewer digits (`--set DIGITS`). |
| 8 | `write_max_s` / `write_after_checks_max_s` large (run with WRITE) | file system, not compute | `--set MN_OUT_THREADS=32` (the file system reaches its rate with 32 writers per node, user's figure); `ECALC_OUT_DIR` on another directory; do not repeat a write run to tune: it costs minutes at L2/L3. |
| 9 | mean `total` is 1.5 x the model or more and no phase explains it | placement or NIC problem | Re-run L1 with `--set` unchanged on another cabinet (`--exclude` the first cabinet's hosts) to see if locality matters; check `fi_info` / NIC counts from the probe against 8 per node. |
| 10 | `rc` 124 (timeout) | too slow or hung | Look at the last phase in `raw/*.log`; a hang in registration: confirm `FI_MR_CACHE_MONITOR=userfaultfd`; then raise `TIME_MIN` only if the log shows progress. |
| 11 | run 1 of a fresh allocation much slower than run 2 on the same host | cold start (aac7 rehearsal 2026-10-25, m: first run 154.9 s, later runs on the same node 76.9-85.5 s; init 47.4 vs 23.6-24.2 s) | Time only from run 2 on (L0 has RUNS=2 for this); compare replicas at the same run index; do not exclude a host for its first run alone |
| 12 | `ECALC_PLANE_CAP=2^30` (Q122, in L0-L3 and step s1) slower than the default at 8 nodes (bench `prm10`), or any refusal or rc 6 at start | the half-size plane pools add pieces to a level's products (aac7, m: 1 n 8e9 -2.64 s [-2.98, -2.30]; 2 n 8e9 per node, path 1, ABBA of 2-run blocks: 23.9 / 23.9 s against 27.4 / 26.0 s, about -2.8 s; unmeasured above 2 nodes) | `--set ENV_ECALC_PLANE_CAP=` for the levels where it loses; keep it where it wins; record both |

NPCAP (matters only from 4.5e10 digits per node, i.e. for the 3.82e13 headline later, not for the 8e9 campaign; bench item `prm08` measures `MN_NONPOW2_CAP` on vs off at a non-power-of-two node count): if it
shows a large time cost for NPCAP off, the fallback for L3 is a smaller digit count with NPCAP on (about 2.67e13 fits, mod
planner): `ladder.sh --level L3 --set ENV_MN_NONPOW2_CAP= --set DIGITS=26700000000000` (the empty value drops the conf's
word, so the code default applies; `ladder.sh` refuses a size that does not fit and prints the largest one).

## 4. Order of work at each level (priority order, the session may end)

- L0: `ladder.sh --level L0` (8 replicas in parallel on adjacent nodes, 2 runs per path). Fast: 15-20 min. Collect; fix rows 1-4; if the spread across the 8 nodes shows a slow host, exclude it, re-run only that replica count (`--replicas 2`).
- L1: 8 nodes x 4 replicas, one chassis (or the smallest unit that holds 8). Collect; rows 3, 6, 7. Confirm the path.
- L2: 64 nodes x 2 replicas (a cabinet if a chassis holds fewer than 64): one of them writes (`WRITE=first`): rows 6-8 matter.
- L3: all 576 nodes, one run with the write. If L2 was clean, run L3 straight. If L2 was marginal, run `--set WRITE=0` first (saves the write time of a failed run).
- Time budget (a): L0 ~20 min, L1 ~15, L2 ~25, L3 ~30 plus queue time; the Slurm limits in `levels/*.conf` are 30 / 35 / 45 / 75 min.
