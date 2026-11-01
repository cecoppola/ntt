# DIAG — diagnostics for the exchange engine and the init timeline
**Question.** Which exchange tags produce the rounds (165,300 per PE at 576 nodes although `XSHMEM_ROUND_MB=128`), why does the `XSHMEM_ROUND_STATS=2` series cost 109 s at finalize, why can `gpu_ms` be negative, and what do the mapping count and the pool peak look like?
**Change / branch.** Branch `diag` (diagnostic output only, no new switch, digits untouched). I1: per-tag table at `XSHMEM_ROUND_STATS>=1` (`comm_xshmem.c`, `xtag_report`). I2a: the `=2` series goes to `xshmem_series_pe<rank>.txt` in the working directory (written at finalize, then freed); stderr keeps one line. I2b: every exchange has its own end and post event; the end event of the last-ended exchange is taken (under a lock, with its stream) before the post's event is recorded; a pair on different streams or with a negative time is skipped. I2c: `mem.c` prints a second table, `pool:pk/APU` and `pool:pk/all` (pool peak-live per phase, largest APU / all APUs). K5: `ECALC_INIT_TL=1` prints the line count of `/proc/self/maps` at the end of init and at the end of the run (`ecalc.c`).
**Cause of the negative gpu_ms (analysis).** The old code had one shared start/end event pair per PE: the end event was re-recorded by the worker thread when ANY pending exchange finished, possibly after the next post had recorded its event (a later end event, an earlier post event: negative), and while the caller read it. Tag 2 (result) exchanges overlap most, hence the half. Not reproduced on the old binary (not run).
**Method.** aac7, 2 nodes, layout D (path 1), queue items `diag1`..`diag5` (the last one is the final code, commit a1f34624). `ecalc 132640000000` with the aac7 launch line + `ECALC_INIT_TL=1`, no digit file, `XSHMEM_ROUND_STATS=0` then `=2`; output lines time-stamped to the second. Gates: `tools/gate.sh --nodes N --hold` (1e9 digits). The gate's own `--build` stops on 2 Python tests (`test_site`: pmi path) that also fail on unmodified main `ca6d3665` in this environment, so the driver built (`make -B`) and ran the gate without `--build`; registry check and doccheck were run by the driver.
**Results (all measured).**

| check | result |
|---|---|
| gate path 1, 1 node / 2 nodes, 1e9 | PASS / PASS (digits identical, VERIFY OK) |
| gate path 2 (`ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`), 1 node / 2 nodes (2 nodes with `XSHMEM_ROUND_STATS=1`) | PASS / PASS |
| `tests/t_xshmem correct`, `XSHMEM_LANES=2`, path 2, 1 and 2 nodes | `XT_RESULT PASS` (the test prints it on PE 0 only) |
| negative `gpu_ms` samples, 2 n 1.3264e11, 8 PEs x 1024 series entries | 0 of 8192 (796 are -1 = no pair: first exchange, other stream) |
| per-tag sums vs totals line | EQUAL (16,783 exchanges, 27,107 rounds, 2,140,135 MB received on node-process 0) |
| stderr, `=2` vs `=0` | 85 lines / 125 kB vs 3 lines / 88 kB (the remaining lines are the per-PE final statistics, 8 PEs); series: 8 files of ~178 kB |
| teardown (total line to step end) | `=0`: 5, 3, 3 s; `=2`: 5, 3, 4 s (three runs each; at 2 nodes the old stderr volume was small too, the 109 s was 2304 PEs x 1024 lines; not remeasured at scale) |
| total time, one pair per run set (`=0` vs `=2`) | 153.35 vs 134.74 s (init 68.3 vs 54.1: node-speed noise), 129.46 vs 126.64 s, 127.54 vs 126.22 s: three single pairs, no cost visible from the series; not a timing claim (TESTING.md section 6) |
| mapping lines of `/proc/self/maps` (limit 65,530) | end of init 1,571 to 1,595; end of run 1,953 to 4,079 (rank 0, 2 nodes) |
| pool peak-live (run `diag3`, per phase, largest APU / all 4 APUs, GB) | tree 54.1 / 216.3; bs 61.4 / 245.6; end 61.4 / 245.6 |

Per-tag table, node-process 0 (4 PEs summed), 2 nodes, 1.3264e11, `XSHMEM_ROUND_MB` default 128 (columns: exchanges, rounds, MB received, largest pair MB, MB per round):

| tag | exchanges | rounds | rounds/exchange | recv MB | max pair MB | MB/round |
|---|---|---|---|---|---|---|
| other | 103 | 103 | 1.0 | 101 | 1.05 | 0.98 |
| redist | 348 | 2028 | 5.8 | 167,972 | 3,685 | 82.8 |
| result | 320 | 1536 | 4.8 | 146,920 | 1,074 | 95.7 |
| spill | 320 | 320 | 1.0 | 66 | 0.67 | 0.21 |
| addsh | 776 | 2988 | 3.9 | 218,624 | 2,147 | 73.2 |
| layeq | 14,916 | 20,132 | 1.35 | 1,606,452 | 268 | 79.8 |
| layv | 0 | 0 | | 0 | 0 | |
| total | 16,783 | 27,107 | 1.6 | 2,140,135 | | 79.0 |

Max over the 8 PEs per column (rounds): layeq 5,033, addsh 747, redist 507, result 384.

**Finding.** `layeq` (the layered inter-node equal-size exchange) has 55 % of the exchanges and 74 % of the rounds and 75 % of the bytes; at 2 nodes the mean round is 79 MB, below 128 MB, because many layeq exchanges are one round of 268 MB / 4 = about 80 MB pairs. The shifts (`addsh`) are 4.6 % of the exchanges, 11 % of the rounds. `spill` and `other` exchanges move under 1 MB per round, a round each. The per-round size is thus set by the pair size (largest pair 268 MB for layeq), not by the 128 MB cap; at 576 nodes the pairs shrink and a round carries few active pairs, which is the 14 MB mean. The 576-node per-tag table will show this directly (run with `XSHMEM_ROUND_STATS=1`).
**Verdict.** Diagnostics only; merge-ready after the coordinator's review. Digits identical in all gates.
**Open issues.** The per-PE final statistics (5 lines per PE) and the 5000-round periodic prints still go to stderr at `>=1`; at 2304 PEs that is thousands of lines. The old negative-sample rate was not re-measured on the old binary. Teardown at scale not remeasured. The per-tag max over PEs uses offset 0 of the dead DATA heap at finalize (all exchanges complete, a barrier before).
**RESUME.** Branch `diag` on origin, last driver `diag5` in `~/day/diag5` on aac7 (worktree `~/ntt-wt/diag`). Next: coordinator review, merge, remove both worktrees; then the 576-node per-tag table for the next task.
