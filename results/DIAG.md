# DIAG — diagnostics for the exchange engine and the init timeline
**Question.** Which exchange tags produce the rounds (165,300 per PE at a mean 14 MB per round at 576 nodes, measured on the target, with `XSHMEM_ROUND_MB=128`), why does the `XSHMEM_ROUND_STATS=2` output cost about 109 s after the program end (measured on the target), why can `gpu_ms` be negative, and what do the mapping count and the pool peak look like?
**Change / branch.** Branch `diag` (diagnostic output only; no new switch; digits untouched). Final code commit `adca36d6`, driver run `diag6`.
- I1: at `XSHMEM_ROUND_STATS>=1` a per-tag table (exchanges, rounds, bytes received and sent, largest pair) prints on stderr: node-process 0 summed over its PEs (its TOTAL row is compared with the existing totals) and the maximum over all PEs (`comm_xshmem.c`, `xtag_report`).
- I2a: the `=2` series goes to `xshmem_series_pe<rank>.txt`, and (Q139) the per-PE statistics of level >= 1 (final and every 5000 rounds) go to `xshmem_stats_pe<rank>.txt`, both in the working directory; stderr keeps the combined tables and one line each.
- I2b: each communicator has its own post and end event (destroyed with it); the last-ended exchange's event and stream are taken under a lock before the post's event is recorded; a pair on different streams or with a negative time is skipped.
- I2c: `mem.c` prints a second table `pool:pk/APU`, `pool:pk/all` (pool peak-live per phase, largest APU / all APUs).
- K5: `ECALC_INIT_TL=1` prints the `/proc/self/maps` line count at the end of init and of the run.
**Cause of the negative gpu_ms (analysis, not reproduced on the old binary).** One shared event pair per PE: the end event was re-recorded by the worker thread whenever any pending exchange finished, possibly after the next post had recorded its event (end later than post: negative) and while the caller read it. Tag 2 (result) exchanges overlap most.
**Method.** aac7, 2 nodes, layout D (path 1), driver `diag6`. Run: `ecalc 132640000000` with the aac7 launch line and `ECALC_INIT_TL=1`, no digit file, `XSHMEM_ROUND_STATS=0` then `=2`, output lines time-stamped to the second. Gates: `tools/gate.sh --nodes N --hold` at 1e9 digits after a driver build (`make -B`), path 1 (`XSHMEM=ofi`) and path 2 (`XSHMEM=cray`, `ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`, 2-node gate with `XSHMEM_ROUND_STATS=1`), then `tests/t_xshmem` at 1 and 2 nodes. The driver built because `gate.sh --build` stopped on two Python tests that failed on unmodified main at that time (fixed on main since). One pair per size, no ABBA.
**Results (all measured, aac7, 2026-11-01).**

| check | result |
|---|---|
| gate path 1, 1 node / 2 nodes | PASS / PASS (VERIFY OK, digits identical) |
| gate path 2, 1 node / 2 nodes (2 nodes with `ROUND_STATS=1`) | PASS / PASS |
| `t_xshmem` path 2, `XSHMEM_LANES=2`, 1 and 2 nodes | `XT_RESULT PASS` (printed by PE 0) |
| negative `gpu_ms`, 8 PEs x 1024 series entries | 0 of 8,192 (796 are -1: no valid pair); old rate 4 % / 50 % was measured on the target, not here |
| per-tag sums against the totals line | EQUAL: 16,783 exchanges, 27,107 rounds, 2,140,135 MB |
| stderr lines from comm_xshmem, `=2` vs `=0` | 22 vs 3 (both 2 nodes); files per `=2` run: 8 series + 8 statistics |
| teardown, total line to step end | `=0` 6 s, `=2` 4 s (earlier runs of the same code family: 5, 3, 3 and 5, 3, 4 s); at scale not remeasured |
| total time, one pair (`=0` vs `=2`) | 153.46 vs 127.74 s (init 71.0 vs 46.9 s: node-speed noise); earlier pairs 129.46 vs 126.64 and 127.54 vs 126.22 s; not a timing claim |
| `/proc/self/maps` lines (limit 65,530) | end of init 1,562; end of run 3,344 (rank 0, 2 nodes) |
| pool peak-live, largest APU / all 4 APUs, GB (final run `diag6`) | tree 54.1 / 216.3; bs 61.4 / 245.6; end 61.4 / 245.6 |

Per-tag table, node-process 0 (4 PEs summed), 2 nodes, 1.3264e11 digits, `XSHMEM_ROUND_MB` 128 (measured):

| tag | exchanges | rounds | rounds per exchange | recv MB | largest pair MB | MB per round |
|---|---|---|---|---|---|---|
| other | 103 | 103 | 1.0 | 101 | 1.05 | 0.98 |
| redist | 348 | 2,028 | 5.8 | 167,972 | 3,685 | 82.8 |
| result | 320 | 1,536 | 4.8 | 146,920 | 1,074 | 95.7 |
| spill | 320 | 320 | 1.0 | 66 | 0.67 | 0.21 |
| addsh | 776 | 2,988 | 3.9 | 218,624 | 2,147 | 73.2 |
| layeq | 14,916 | 20,132 | 1.35 | 1,606,452 | 268 | 79.8 |
| layv | 0 | 0 | | 0 | 0 | |
| total | 16,783 | 27,107 | 1.6 | 2,140,135 | | 79.0 |

Maximum over the 8 PEs (rounds): layeq 5,033, addsh 747, redist 507, result 384, spill 80, other 31.
**Verdict.** Diagnostics only; digits identical in every gate; stderr is a few lines per run. `layeq` (the layered inter-node equal-size exchange) is 89 % of the exchanges, 74 % of the rounds and 75 % of the bytes; `addsh` is 4.6 % of the exchanges and 11 % of the rounds. At 2 nodes the mean round is 79 MB: the round size follows the pair size (layeq pairs are 268 MB), not the 128 MB cap. The 576-node table (`XSHMEM_ROUND_STATS=1`) will show whether the 14 MB mean there is the same effect (hypothesis, not yet measured). This table is the input for the task "rounds sized by active pairs".
**Open issues.** The old negative-sample rate was not re-measured on the old binary. Teardown at 576 nodes not remeasured. The per-tag maximum over PEs uses offset 0 of the DATA heap at finalize (all exchanges complete, barriers around it). `gate.sh --build` was not used for the gates (see Method). Not run: 3-node checks.
**RESUME.** Branch `diag` on origin; the aac7 worktree of the branch and the driver folder `diag6` are left for the coordinator. Next: coordinator review and merge; remove both worktrees; run `XSHMEM_ROUND_STATS=1` at 576 nodes for the per-tag table.

## I3 — host timeline between exchanges (`ECALC_HOST_TL`, branch `hosttl`)

**What it is.** `ECALC_HOST_TL=1` (default 0) records, for the traced PE (`ECALC_HOST_TL_PE`, default 0, -1 = every PE), the wall time from the end of the last pending exchange to the next post ("host"), attributed to the phase tag of that next post, plus the wait inside the post (the caller's stream sync) and the number of posts made while another exchange was pending ("ovl"). It is header-only (`ecalc/hosttl.h`) with hooks in `ecalc/comm_xshmem.c` only; one file `xshmem_hosttl_pe<rank>.txt` per traced PE at finalize. Check printed per file: span (first post to last end) = busy + sum(host), exact. Unit test `tests/t_hosttl` (9 checks).

**Gates (measured, aac7 2026-11-01, path 1 unless noted).** 1e9 digits, switch unset and set (1 node; 2 nodes with `ECALC_HOST_TL_PE=-1`): VERIFY OK, digits identical, all PASS; path 2 (Cray SHMEM, `ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`, 2 nodes) on and off: PASS. After merging main (C3, `comm_xshmem.c` untouched by main) the 1-node gate with the switch unset passed again.

**Big runs (measured; 2 nodes, 1.6e10 digits, `MN_TREE_LOGN_TEST=26`, path 1, 8 PEs, 2026-11-01 19:55-20:03 ET; single runs).** All four VERIFY OK with the same digit hash (sha256 of the digit files, first 16 hex 5bc4211dbd24efd1): off, on (`ECALC_HOST_TL=1 ECALC_HOST_TL_PE=-1`), on + `XSHMEM_ROUND_STATS=1`, off again. (A first attempt passed the digit count as `1.6e10`, which ecalc reads as 1 digit; see TASKS "Proposed".)

| run | ecalc total (s) |
|---|---|
| off | 53.63 |
| on | 52.81 |
| on + ROUND_STATS | 52.89 |
| off (second) | 53.41 |

Switch on minus the mean of the two off runs: -0.71 s (-1.3 %). These are single runs, the two off runs differ by 0.22 s; this shows no measurable cost, it is not a timing claim.

**What the table shows at 2 nodes (on + ROUND_STATS run; seconds per PE over the run).**

| tag | PE 0 gaps | PE 0 host | PE 1 gaps | PE 1 host |
|---|---|---|---|---|
| other | 35 | 28.022 | 23 | 0.008 |
| redist | 112 | 4.764 | 112 | 4.794 |
| result | 80 | 0.249 | 80 | 0.252 |
| spill | 80 | 0.062 | 80 | 0.069 |
| addsh | 74 | 0.136 | 74 | 0.176 |
| layeq | 5689 | 1.604 | 5689 | 1.620 |
| total | 6070 | 34.837 | 6058 | 6.918 |

The first PE of each node (PE 0 and PE 4; 34.84 and 35.07 s) carries about 28 s of host time in the tag `other`; the other six PEs have 6.85-6.92 s with almost nothing in `other`. Common to all PEs: `redist` about 4.8 s (longest single gap 3.67 s), `layeq` about 1.6 s over 5689 gaps (0.29 ms each, max 0.29 s). Wait inside the posts is 0.09-0.10 s per PE and "ovl" is 0 everywhere (never a post with another exchange pending). Span of PE 0 is 40.8 s = busy 5.98 + host 34.84 (exact); the busy part of the PE-1 span is 5.63 s. The 28 s gap of the node-leading PEs corresponds to GPU stream time (below), so it is device compute on those PEs while the other six idle (their tail from last end to report is 28.8 s).

**Closure (measured, same run, per PE).**

| PE | table host sum (s) | run gap counter (s) | GPU stream time (s) | gap - GPU (s) |
|---|---|---|---|---|
| 0 | 34.837 | 34.837 | 29.636 | 5.20 |
| 1 | 6.918 | 6.918 | 1.800 | 5.12 |
| 2 | 6.857 | 6.859 | 1.770 | 5.09 |
| 3 | 6.865 | 6.866 | 1.755 | 5.11 |
| 4 | 35.067 | 35.067 | 29.869 | 5.20 |
| 5 | 6.893 | 6.894 | 1.771 | 5.12 |
| 6 | 6.853 | 6.853 | 1.730 | 5.12 |
| 7 | 6.848 | 6.848 | 1.729 | 5.12 |

The table's host column sum equals the run's own gap counter (`ROUND_STATS` rst[25]) to 0.002 s on all 8 PEs (the same clock interval measured in two places, so this checks the bookkeeping, not an independent quantity). Subtracting the GPU stream time (HIP events on the caller stream, counted over 5597-5603 of the 6058-6070 intervals; 462-468 intervals with a different stream are skipped, so the GPU figure is a lower bound) leaves 5.1-5.2 s per PE, the same on every PE: host-side launch and caller work not hidden behind GPU work, about 14 % of the 35 s on PE 0 and 75 % on the other PEs.

**Limits.** (1) Calls to `hipStreamSynchronize` or other syncs made by the caller inside a gap count as host time; the comm layer cannot separate them (the GPU-time subtraction above is the only split). (2) The tag is that of the next post, not the work that preceded it. (3) The GPU time skips intervals on a different stream, so gap - GPU is an upper bound of the pure host part. (4) One 2-node size, single runs; a 6-node run was not possible (queue capacity). (5) Per-call-site attribution (which function of bs, recip, dm) is not available here; `XSHMEM_ROUND_STATS=2` has the call-site series.

**RESUME.** Branch `hosttl` (origin), main merged at 99397bdc. Not merged to main; the switch stays off by default. Next: coordinator review and merge, remove the worktrees.
