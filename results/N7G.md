# N7G — COMM_SHMEM_PX_THP and COMM_SHMEM_PX_PIN on the proxy mode: pairs and verdict

**Question.** Do huge pages (`COMM_SHMEM_PX_THP=1`) and pinned proxy pools (`COMM_SHMEM_PX_PIN=1`) shorten the 6-node SHMEM
proxy run against the default `COMM_OFI=0` path? Both switches default to 0.

**Change / branch.** Branch `n7g`, commit 006a80f (switches `COMM_SHMEM_PX_THP`, `COMM_SHMEM_PX_PIN`, default 0). Gates on
`n7g` passed before the pairs (m).

**Method.** Three interleaved pairs at 6 nodes, 3.8646e11 digits. Arm A: `COMM_OFI=0` (defaults). Arm B: A plus
`COMM_SHMEM_PX_THP=1 COMM_SHMEM_PX_PIN=1`. Difference is B − A, paired, 95 % CI from the t distribution with n = 3.
Driver and tables on aac7 (`n7g_pairs/tables.txt`); not copied here.

**Re-gate of item 36 on `n7f` (commit 76c8742).** Gates pass 11 of 11 at 1 and 2 nodes, each switch alone and all together;
2-node 1e10 compare identical (m). Summary on aac7 (`n7g_regate/summary.txt`).

**Results (m).**

| arm | runs | mean (s) | sd (s) | pair roffmax (s) |
|---|---|---|---|---|
| A: defaults (`COMM_OFI=0`) | 3 | 432.1 | 83.6 | 652 / 979 / 670 |
| B: + THP + PIN | 3 | 307.2 | 93.7 | 252 / 540 / 347 |

B − A = −124.9 s, 95 % CI [−460.6, +210.8], n = 3 pairs (m). Not significant.

**Page size (m).** The helper pools are 4 KB pages. Shared-memory huge pages are unavailable on this system.
`MADV_COLLAPSE` works on the pool and costs 0.29 s per GiB.

**Verdict.** Not significant. The default stays 0 for both switches. THP and PIN need more pairs before any adoption
decision (the three pairs have a CI wider than the effect).

**Open issues.** The run-to-run spread is large (sd 84 to 94 s on 3 runs); the open N7F item covers the same kind of spread. Four more
pairs of THP-only against the default are not run (aac7 budget, needs the user's word). The effect of PIN alone is not
separated from THP.

**RESUME.** Branch `n7g`, commit 006a80f plus the N7G docs commit. Done: results, RESULTS row, OPPORTUNITIES note, TASKS
rows. Next (not run): 4 more pairs, THP only against the default, at 6 nodes (queue via the aac7 runner, paired ABBA,
≤ 4 paired rounds per the project rule). Decision D35: merge defaults? No; both switches stay off.

## Item 37 (2026-10-15, gated): THP only, 4 pairs at 3.8646e11

Four interleaved pairs at 3.8646e11 digits. Arm A: default SHMEM. Arm B: `COMM_SHMEM_PX_THP=1` only (PIN left at 0).
Measured wall times: A mean 576.5 s (sd 155.5), B mean 288.8 s (sd 63.0). Paired B − A = −287.8 s, 95 % CI [−507, −68],
n = 4; B faster in 4 of 4 pairs (m).

Gates PASS: 1 and 2 nodes with THP, kill default on and off, comm_ofi 2-node gate, 6-node 1e10 compare identical (m).
Switch stays default 0 pending user decision D36.

## Item 38 (2026-10-15, gated): default flip, D36 (a)

Build aedc13c, no PX_* variables set. Ran 20:52 to 21:18 ET (27 min, measured).

- 1 node, COMM_OFI=0: gate PASS, digits identical (m).
- 2 nodes, COMM_OFI=0: gate PASS, digits identical (m).
- 6 nodes, 1e10 digits: VERIFY OK, compare identical (m).
- 6 nodes, 3.8646e11 digits, 2 interleaved pairs: A = default (THP on), B = COMM_SHMEM_PX_THP=0.

| pair | A (default) | B (THP=0) | B - A |
|---|---|---|---|
| 1 | 237.4 s | 337.3 s | +99.9 s |
| 2 | 228.8 s | 544.3 s | +315.5 s |

Mean B - A +207.7 s (sd 152.5, n = 2, 95 % CI [-1162, +1578]; not significant alone). Direction matches item 37 (4 of 4 pairs). THP was active in A (huge helper pools 2562 MiB per node) and absent in B (m).

Verdict: PASS. D36 (a) applied: COMM_SHMEM_PX_THP defaults to 1 in the COMM_OFI=0 proxy mode.

## Items 39 and 41 (2026-10-15/16): huge-page pairs, 6 nodes, 3.8646e11 (measured)

- Item 39: 4 pairs, default on vs COMM_SHMEM_PX_THP=0: B - A +28.4 s (95 % CI -27 to +84).
- Item 41: 4 pairs, same design: B - A +163.5 s (95 % CI -45 to +372). Default on: 290 s mean; off: 475 s.
- Both drivers hit their timeouts after the last run; tables are from the summary files, not DONE markers.
- Pooled over the 14 pairs of items 37, 38, 39 and 41 (the mean of the pair means, weighted by pairs): about 167 s saved by the default (modelled; the runs mix conditions).
- Item 40 (heap switch, COMM_SHMEM_HEAP_THP=1): gates PASS at 1 and 2 nodes and 6-node 1e10 identical; 2 pairs B - A +24.8 s (CI -179 to +229): no gain, stays 0.
- Item 40 task-3 arm (COMM_SHMEM_OFFS_WITH_DATA=64 COMM_SHMEM_MESH_PAR=1): 2 pairs, +49 s: no gain, stays 0.
- Verdict: huge-page default stays on; heap and task-3 switches stay off.

## Items 45-48 (2026-10-16/17): libfabric baseline and default-run spread (measured)

- Libfabric (COMM_OFI unset), 6 nodes, 3.8646e11, 4 runs (item 45): 178, 181, 186, 193 s; mean about 184 s.
- Default SHMEM (huge pages on), item 46, 3 runs: 254, 256, 289 s; THP off, 1 run: 446 s.
- Default SHMEM, items 47 and 48, 8 runs: 221-397 s (sd about 60 s). The spread is larger than the gap between the default and libfabric means.
- Compaction-stall increase per node during each run (items 48, four default runs): the largest per run was 6770, 4170, 708 and 501. The slowest run (339 s) had the largest value, but the second-fastest (240 s) had the second largest, so no link is shown (n = 4).
- Straggler (item 43 analysis, n = 8): one node finishes late and the other five wait in layv; cause not identified.
- Drivers 45-48 hung at exit after their runs (drivers without a done-marker); fixed for 48 by writing a done-marker before the exit path.
- Verdict: the remaining SHMEM gap is mostly run-to-run variance; the cause of the lagging node is open. Task 5 (layered exchange) is not started.

## Items 49-51 (2026-10-17): layered-exchange switch and per-PE timestamps (measured)

- Layered-exchange switch COMM_SHMEM_LAYV_ANY (item 49-50, 4 paired ABBA runs at 3.8646e11): B - A per pair -19.6, -1.3, -101.6, -57.5 s; mean -45.0 s, 95 % CI about [-116, +26]; switch-on faster in 4 of 4 pairs; not significant. Gates PASS at 1 and 2 nodes; 1e10 at 6 nodes identical.
- Timestamp runs (item 51, 4 default runs): wall 223, 264, 186, 235 s.
- No stable straggler: the last PE to enter layv changes by run (pe1 26 %, pe4 22 %, pe3 21 % of exchanges); the largest layv enter spreads are rare single events (0.4 to 6.9 s).
- The slow run's extra time is mostly in the first bs step (+55 s of the +78 s): two PEs (pe4, pe5) start bs about 48 s late; the late pair varies by run. The cause of that startup lag is not identified.
- Clock sync error inferred about 0.4 ms (not measured).
