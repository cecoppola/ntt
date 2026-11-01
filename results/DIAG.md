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
