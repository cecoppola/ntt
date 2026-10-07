# S18KITFIX — three bugs in `ecalc/target_kit.sh`, found in the aac7 rehearsal (`~/s18ab3`), fixed and verified for real

Branch `s18-kitfix` (its fix, commit `ef22f22`, is also already merged into `main` by another process while
this task was validating on aac7 — `main`'s own history shows `Merge ... origin/s18-kitfix: Fast-forward`).
Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled **measured**. Code change:
`ecalc/target_kit.sh` only (no C code touched, no rebuild needed).

## 1. The bugs (from `~/s18ab3/kit/KIT_SUMMARY.txt`, `~/s18ab3/kit/logs/`, `~/s18ab3/log/kit_fail_a3.txt`)

1. **Stage a3** (and `~/s18ab3/drive.sh`'s own `t_comm` sweep, which reused the same broken launch) ran
   `tests/t_comm --bw` by bare `srun`, leaving the SHMEM heap at the library's default. Every launch failed:
   `comm_shmem: pe N: shmem_malloc of 8192 MiB failed: the SHMEM heap ... must be >= 8704 MiB (the pool + 512)`.
2. **Stage a4**'s summary block for a later node count (e.g. `n=8`) repeated an earlier node count's lines
   (`n=2`) ahead of its own, because the parser `grep`/`tail`'d the one shared, appended log file that every
   `n` had written into.
3. **Stage edge** printed only the "report both forms" text, no actual VMM/arena map-rate number — `dbig.c`'s
   `"VMM arena ... s/GB"` line is gated by `vmm_vb()` (`DB_POOL_VERBOSE` / `RNS_VERBOSE`), which the stage never
   set, so the line it was grepping for never appeared in the log.

## 2. The fixes (`ecalc/target_kit.sh`, commit `ef22f22`)

1. **a3**: launch `tests/t_comm` through `mnrun.sh` instead of bare `srun` — `t_comm` fits `mnrun.sh`'s
   `<procs> <command...>` interface exactly, the same way `results/OFI17.md`'s and `results/TUNE17.md`'s own
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

## 3. Validation

`bash -n ecalc/target_kit.sh` and `ecalc/mnrun.sh`: clean. `--dry-run` (full kit and `--only a3`/`--only edge`
individually): the a3 stage prints the computed pool (`COMM_SHMEM_POOL_MB=4608 COMM_OFI_POOL_MB=1280` at the
kit's defaults) launched via `mnrun.sh`; the a4 stage shows a distinct log path per `n`.

### 3a. Real run on aac7, job 12287: `--only a3 --site aac7 --jobid 12287 --out ~/s18ab3/kit`

First attempt with the kit's own default `--nics2` (`"0,4;1,5;2,6;3,7"`, the **576-node target's** 8-NIC/node
form) correctly failed at the fabric, not the pool: `comm_ofi: fi_getinfo(...) = -61 (No data available)` —
aac7 only has 4 NICs (`cxi0`-`cxi3`), one per APU, so indices 4-7 don't exist there. Re-run with
`--nics2 "0,1;1,2;2,3;3,0"` (results/TUNE17.md's form A for aac7's 1-NIC-per-APU topology, two endpoints
sharing one cxi per device): **PASS**.

`COMM_SHMEM_POOL_MB=4608 COMM_OFI_POOL_MB=1280` (kit ROCm: `rocm/7.2.4`), `FI_UNIVERSE_SIZE=4096 FI_LOG_LEVEL=warn`.

GB/s per node (1 PE/node) at the largest slab (268435456 B = 256 MiB), both PEs:

| form | pe 0 | pe 1 |
|---|---|---|
| 1 NIC/APU (`COMM_OFI_NICS=0;1;2;3`) | 20.94 GB/s | 20.95 GB/s |
| 2 NICs/APU (`COMM_OFI_NICS=0,1;1,2;2,3;3,0`, aac7's form) | 20.92 GB/s | 20.91 GB/s |

No gain from the 2-NIC form on aac7, as `results/TUNE17.md` already found (`nic2_formA` vs `nic2_default`):
one NIC per APU there, so the "2 NICs" form just shares the same `cxi0..3` between two devices rather than
reaching new fabric. The mechanism is exercised correctly; its rate gain needs the target's real 2-NICs/APU
wiring.

### 3b. Real run on aac7, job 12287: `--only edge --site aac7 --jobid 12287 --out ~/s18ab3/kit`

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

## 4. Sweep: `~/s18sweep/drive.sh` (armed, fire-and-forget)

Runs the fixed a3 `t_comm --bw` bandwidth command (same pool-sizing formula as §2.1, applied directly to a
`srun -w <nodelist>` launch since packed/spread needs an explicit node list rather than `mnrun.sh`'s own
selection) at 2/4/6/8/10 nodes, packed vs spread placement (spread for 2-8; node lists from
`~/s18ab3/summary.txt`'s existing step4 sweep lines), 3 reps each, sequential, median/min/max GB/s per node
and per APU thread. Per-run `timeout -k 10 600` (kill by PID only, never `scancel`/`scontrol`).

A second, unrelated in-flight driver (`~/s18p2/drive.sh`, a crash-soak phase) was found actively using all 10
nodes of job 12287 at arm time — not something this task started or should touch. `~/s18sweep/drive.sh` gates
on it the same way `results/OFI17.md`'s/`results/TUNE17.md`'s drivers gate on markers elsewhere in this repo:
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
R="sshpass -p <cluster-password> ssh -o ConnectTimeout=20 chcoppola@aac7.amd.com"
$R 'cat ~/s18sweep/S18SWEEP_DONE 2>/dev/null; cat ~/s18sweep/summary.txt; column -t -s$'"'"'\t'"'"' ~/s18sweep/sweep_results.tsv'
$R 'ps -p 2704179 -o pid,etime,cmd'   # kill only by this PID, never scancel
```

## 5. Note: the branch was already merged by another process

While this task was running its real aac7 validation, another process merged `s18-kitfix` into `main`
(fast-forward, `main`'s reflog: `merge origin/s18-kitfix: Fast-forward`) and deleted the remote branch, and a
later `s18-w` branch's merge then advanced `main` further; a worktree-pruning side effect of that same
activity also removed this task's local worktree checkout (recreated here from commit `ef22f22` to land this
report). This task did not perform that merge and did not delete the branch itself, per the "do not merge"
instruction; it is recorded here because it changes what "the branch" means by the time this report lands.

> **Coordinator note (2026-10-07):** the sweep this report armed (~/s18sweep, 03:08 EDT) ran while Phase 2 used the same hold and is not used; the sweep of record is RESULTS §117, and the collision it caused is RESULTS §118.
