# O7b: fresh per-kernel GPU profile of the default code (main 0298fe2), aac6, 1 node -- DONE; analysis and wall-time budget below

Branch o7b (from main 0298fe2). Purpose: S39's table (results/S39.md) predates S40/S41/S50/S51 (k_addsub2, DBIG_ADDSUB3, DBIG_MODQ_MONT now default); this run re-ranks the kernels so the next kernel round can be chosen.

## Method (same as S39 where possible)
- Tool: `rocprofv3 --kernel-trace` (S39/S38 used it with the rocpd db; here `--output-format csv`, one `*_kernel_trace.csv`, aggregated by `tools/o7b_table.py`). Kernel names have their argument lists stripped, so template instances (k_b1r<10,3,0,1>, ...) stay separate as in S39.
- Sizes: 6.441e10 digits (the S39/S38 size, 1 node, code defaults, no digit writes) and 1e10 digits (asked for). Per size: profiled run, then an unprofiled run (overhead = difference of the two `total` lines; one pair, indicative only).
- Differences from S39 (comparability): S39 ran on aac7 (Cray SHMEM, the e16 launch line); this runs on aac6 PPAC_MI300A_SPX, plain launch (no launch line, single node, no SHMEM), same 4-APU MI300A node type. Absolute times may differ from aac7 by the line/fabric; kernel ranking and per-kernel seconds are comparable. S39 listed APU0 (agent 4) seconds: the first per-agent column of the table is the equivalent.
- Table columns: kernel, calls, total ms (sum over the 4 APUs), % of GPU time (share of summed kernel durations), % of wall (total ms / (4 x run wall)), seconds per APU. Top 25 plus "other" and "all kernels" rows. All times measured (rocprofv3 timestamps; run wall from the ecalc `total` line).
- Each run must print VERIFY OK (counted in summary.txt).

## Arm (done 2026-10-11 23:34 Eastern)
- aac6 clone `~/ntt-o7b` (clone of `~/ntt`, which was already at main 0298fe2, so no bundle needed), built with `module load rocm; make -s -j16`; driver `~/ntt-o7b/tools/o7b_driver.sh` (same file as tools/o7b_driver.sh on this branch), output dir `~/o7b`.
- Hold: job 22974, 1 node, 40 min, `--begin` 04:15 Eastern (03:15 Central), queued behind our other jobs; the driver waits for RUNNING (gives up and cancels only job 22974 at 09:00 Eastern), runs the four steps (P/U for each size, each skipped if too little hold time is left), writes the table, cancels its own hold, checks no step is left, writes the marker.
- Dry-run of the driver logic with stub sbatch/srun/rocprofv3 passed locally (summary, table, marker, scancel of own job only).
- Expected: start ~04:15-05:00 Eastern, ~10-15 min of node time.

## Collect
- Marker: `ssh aac6 'cat ~/o7b/O7B_DONE'` (SUCCESS / FAILED: reason).
- Table: `ssh aac6 'cat ~/o7b/summary.txt'` (copy it into this file, compare with S39 section 1, rank the next candidates).
- Raw: `~/o7b/rp_<digits>/`, run logs `~/o7b/log/`.
- Caveat to check on collection: rocprofv3 7.2.4 csv column names (`Agent_Id`, `Kernel_Name`, `Start_Timestamp`, `End_Timestamp`) were not verified on the cluster; if the table says "no kernel_trace.csv" or is empty, re-run `python3 ~/ntt-o7b/tools/o7b_table.py ~/o7b/rp_<digits> <total_s> 25` after fixing names (raw csv is kept).

## Results (collected 2026-10-12 02:17 Eastern; job 22974 ran 02:13-02:17, cancelled by the driver; all four runs VERIFY OK)
Labels: **m** measured, **mod** modelled, **as** assumed. Raw tables: `~/o7b/summary.txt` on aac6 (top 25 per size), csv under `~/o7b/rp_<digits>/`; timeline script `tools/o7b_timeline.py` (busy seconds per 5 s bin, union of kernel intervals).

| size | profiled total (m) | unprofiled total (m) | profiler overhead (one pair, indicative) | kernel busy per APU 0/1/2/3 (m, sum of durations) | busy / (wall) on APU 0-2 |
|---|---|---|---|---|---|
| 1e10 | 26.20 s | 24.60 s | +1.6 s (6.5 %) | 4.4 / 4.5 / 4.5 / 2.2 s | 17 % |
| 6.441e10 | 74.09 s | 73.32 s | +0.77 s (1.0 %) | 43.7 / 44.7 / 44.4 / 38.1 s | 60 % |

Top kernels at 6.441e10 (share of summed GPU time, m): k_gather 15.0 %, k_crt_batch 10.9 %, k_push3 10.0 %, k_b1r<10,3,0,1> 9.6 %, k_b16<6,0,0> 5.4 %, k_b16r<0,0,1,0> 5.0 %. At 1e10: k_crt_batch 20.6 %, k_b16r<0,0,1,0> 12.5 %, k_b1r<10,3,0,1> 9.7 %, k_b16r<1,0,1,0> 7.8 %, k_bload<1> 7.4 %. (At 1e10 every dist product is the B form (60 products, 0 C form) and there is no k_gather / k_push3; the profile there is the B-form + batch-tier mix, not the production shape.)

### A. Why the GPUs look idle: the first 25 s of the 6.441e10 run have no GPU work (m)
Phase lines of the unprofiled run: `total 73.32 = init 15.5 + bs 38.2 (seeds 10.0 + batch 19.3 + mdev 8.7) + dm 19.5 (recip 4.8) + other 0.1`. The seeds are binary-splitting leaves computed on the host during init; `bs` then "waits 10.0 s for the init thread" (log: seeds 17.76 s of host work, spans 11.93 s, 2.00 s waiting for the regions, 1.63 s issuing the DMA). The kernel timeline agrees: with t = first dispatch (a fill kernel during init, run time about 13 s, inferred), APU busy seconds per 5 s bin are ~0 until trace time 10 s (run time about 23-25 s, mod), then 1.1, 3.0, 4.8, 4.8, 4.4 ... 4.4 s per 5 s bin (88-98 % busy). So:

| window of the 73.3 s run (unprofiled phase lines, m) | seconds | GPU state |
|---|---|---|
| init (arenas 7.3 s, region pools 7.5 s, plane pools, contexts) | 15.5 | idle (only fills / copies) |
| bs: waiting for the seed thread | 10.0 | idle |
| **pre-GPU subtotal** | **25.5 (34.8 %)** | idle |
| GPU-active window (bs batch + mdev + recip + division) | 47.8 | busy 43.9-44.7 s per slowest APU (profiled 44.7 s minus about 0.8 s profiler overhead, mod); union of kernel intervals 41.6 s, so ~3 s of the sum is kernels overlapping on two queues (m) |
| non-kernel gap in the active window | 3.5-6 s (5-8 % of wall, mod) | host layout (bs batch levels 1-23: layout 1.06 s + add+norm 0.31 s, m), sync points, launch gaps, ramp bins |

Inside the active window the slowest APU is 92 % kernel-busy (m, 44 s of 47.8 s). At 1e10 the same arithmetic gives init 11.2 + seed wait 3.3 = 14.5 s of 26.2 s (55 %) before any kernel, and `other` 1.6 s after; kernel busy 4.5 s of ~11.7 s of active window (38 %). That is the "17 % busy" of the first look: it is a small-size effect plus the fixed init cost; at the 6.441e10 reference the GPU is 60 % of wall and 92 % of the active window.

### B. What changed since S39 (6.441e10, APU0 seconds; S39 = aac7 Cray, one rocprof db; now = aac6 plain launch; platform differs, so only large moves are real)
| kernel | S39 (APU0) | now O7b (APU0 / APU1 / APU2 / APU3) | change | reason |
|---|---|---|---|---|
| k_addsub (S39 #1) -> k_addsub3<1> | 10.35 (ADDSUB2 off) | 1.35 / 1.76 / 1.91 / 0.87 | **-9.0** | S38 addsub2, S40/S41/S51 addsub3 (K2) default |
| k_maxidx | 2.42 | 0.01 (k_maxidx<1>, 970 calls in all) | **-2.4** | DBIG_MAXIDX_TOP default (S42) |
| k_modq (T1 residues) | 1.57 | 0.08 (k_modq_m) | **-1.5** | K3 DBIG_MODQ_MONT default (S51) |
| k_gather | 7.67 (44 ms / call) | 6.34 / 6.37 / 6.40 / 6.47 (36.5 ms / call, 174 calls per APU) | -1.3 | not a code change we know of; platform / fabric (aac6 plain launch), assumed |
| k_gather_shift | 0.55 | 0.27 / 0.44 / 0.55 / 0.34 | -0.3 | follows the addsub3 / maxidx change |
| k_crt_batch | 4.54 | 4.46 / 4.73 / 4.77 / 4.76 | 0 | unchanged (candidate untouched) |
| k_b1r<10,3,0,1> | 4.44 | 4.25 | -0.2 | NTT unchanged |
| k_push3 | 4.43 | 4.32 | -0.1 | link-bound, unchanged |
| k_b16r<0,0,1,0> / k_b16<6,0,0> / k_b16<5,0,0> / k_b1r<10,3,2,1> | 2.44 / 2.43 / 1.71 / 1.70 | 2.41 / 2.34 / 1.66 / 1.68 | ~ -0.05 each | unchanged |
| k_bload | 1.19 | 1.19 (0.98-1.28; APU3 0) | 0 | unchanged |
The three S39 candidates: B (maxidx) and the addsub / modq part of C are done; **A (raw-once gather) is still open** and k_gather is now the single largest kernel (15 % of GPU time, 6.4 s per APU). Summed APU0 kernel busy dropped by roughly 14 s of the listed rows (m, kernel by kernel), against about 8 s of wall (S39 ~81 s on aac7 vs 73.3 s now on aac6): the wall gain is smaller than the busy gain (partly overlap / platform; not separable here). New in the list: k_addsub3 is uneven (APU0 1.35 s, APU2 1.91 s, APU3 0.87 s).

### C. Busy time by class, per APU (m, 6.441e10; seconds, mean of 4 APUs; APU3 in brackets)
NTT and pointwise compute (k_b1r, k_b16, k_b16r, k_pw_y, k_r3) 19.0 (16.4) | k_gather + k_bload remote pulls 7.3 (6.5) | k_crt_batch 4.7 (4.8) | k_push3 all-to-all 4.3 (4.3) | pack / unpack / transpose / twiddle 2.6 | scatter 2.2 (1.6) | dbig add-sub, shift, modq, norm 2.0 (1.3) | copies / fills 0.7 | sum APU0-2 43.7-44.7, APU3 38.1. APU3 is 6.6 s below the others, all of it in the B-form work (it has no k_scatter / k_bload / k_r3 and about half the b16r passes).

### D. Ranked levers at 6.441e10, 1 node (savings are per run, modelled; none built)
| rank | lever | kind | saving (mod) | basis |
|---|---|---|---|---|
| 1 | **Start the seed spans earlier / more of them in parallel** so `bs` does not wait 10.0 s; the seed thread starts only when the first arena half is mapped (A09: ~13 s at 1 node), the compute itself is 11.9 s | host | **5-8 s** | upper bound is the 10.0 s wait; floor: spans 11.9 s cannot start before staging (~1 s) unless buffers are separate from the regions; host cores shared with the mapper |
| 2 | **Init mapping (K8, A09/N9):** arenas 7.3 s + region pools 7.5 s synchronous mapping of 180 GB; only the part not hidden behind the seeds counts, so it adds to rank 1 only if both are done | host / alloc | **2-4 s** beyond rank 1 | m: init 15.5 s of which arena 7.29 s; assumed 25-50 % removable (async mapping, larger pages) |
| 3 | **k_gather / k_bload raw-once pull (S39 A):** 6.4 + 1.2 s per APU of remote pull at ~73 GB/s; pull the operand once, canonicalise per prime locally | GPU | **2.5-3.6 s** | S39 3.0-4.4 s scaled by today's 6.34 / 7.67; needs one extra 4.3 GB plane; the 5-line local-source micro-test of S39 is still untried |
| 4 | **Non-kernel gaps in the active window:** host layout and add+norm in the batch tier (1.4 s, m), sync points, ramp bins (kernel-busy only 62-88 % in trace bins 10-20 s) | host | **1-3 s** | m: gap 3.5-6 s; assumed half removable by overlapping layout with the previous level; needs a per-level GPU-idle timeline (not in this data) |
| 5 | **k_crt_batch (4.5-4.8 s per APU, 22 % of HBM in the C form):** serial carry chain per chunk; B-form S = 5 x 228 stripes | GPU | **0.8-2.0 s** | S39 B-form occupancy 0.8 s; 2.0 s needs the C-form chain restructured (as) |
| 6 | k_addsub3 residue (1.35-1.91 s per APU, uneven) | GPU | ~1 s | S39 C scaled; slowest APU sets the critical path |
| 7 | K7/O5 APU3 rebalance (branch o5) | GPU | **0.5-0.9 s** | C07: APU3 idle only in the B form (4.4 s of 94 s); bound if all four APUs were equal: 44.7 -> 42.7 s = 2.0 s, not reachable (APU3 lacks the pull / scatter stages) |
| 8 | NTT passes (19 s per APU, 40-55 % of HBM): any 10 % gain | GPU | 1.9 s per 10 % | a fusion project, no quick lever |
| - | k_push3 4.3 s | link | 0 | link-bound, closed (S38) |
GPU-kernel levers (3, 5, 6, 7, 8) total about 5-8 s (ranks 3 + 5 + 6 + 7); host / alloc levers (1, 2, 4) total 8-15 s (overlapping, so not additive: ranks 1 and 2 share the same 25.5 s window, floor about 12-14 s of host compute + staging). The first lever by size is therefore a host lever, not a kernel.

### E. Notes and caveats
- Profiled vs unprofiled is one pair per size; no timing claim is made from it. `other` 1.6 s at 1e10 (dc and the small tail) is outside the kernel trace window.
- t = first dispatch is inferred to fall at run time ~13 s from the phase lines (init 15.5 s, seeds wait to 25.5 s, GPU ramp starting inside the 10-20 s trace bins); the clocks of the log and the trace were not cross-stamped (`ECALC_LOG_CLOCKS` was off), so the 25.5 s idle window is from the log (m) and the trace shape (m) agreeing, not from a shared timestamp.
- This profile has one APU-level view only (kernel start / end); host-side waits inside the active window need `C07_TS`-style stamps (branch c07, not merged) to attribute.

## RESUME
- State: run complete (job 22974 cancelled by its driver, no step left, marker SUCCESS). Analysis written above. Nothing runs on aac6 for this task; clone `~/ntt-o7b` on aac6 and worktree `../ntt-wt-o7b` still exist (remove after merge / decision).
- Next (coordinator): pick from section D; a seeds-start experiment (rank 1) and the S39-A local-source micro-test (rank 3) are the cheapest checks; a per-level GPU-idle timeline would size rank 4.
