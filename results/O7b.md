# O7b: fresh per-kernel GPU profile of the default code (main 0298fe2), aac6, 1 node -- ARMED, result pending

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

## RESUME
- State: job 22974 pending (begin 03:15 CDT), driver PID on the aac6 login node (`ps -u <user> | grep o7b_driver`). Nothing else runs. If the driver died, the hold can be cancelled with `scancel 22974` (our own job) and re-armed with the command in the driver header.
- Next: after O7B_DONE appears, haiku collects summary.txt, pastes the tables under a "Results" section here with the S39 comparison, then coordinator picks the kernel round. Branch o7b is not merged; remove worktree `../ntt-wt-o7b` and aac6 `~/ntt-o7b` after collection.
