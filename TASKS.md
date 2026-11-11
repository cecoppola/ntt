# TASKS — live task list (open work, proposed items, decisions)

Scope: open work, user decisions still acting and proposed items; closed items are in the archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.
Closed rows and the earlier done-lists: `archive:TASKS_HISTORY_2026-11-10.md` (older: `archive:cleanup_1107/TASKS_before_1107.md`, `archive:TASKS_HISTORY_2026-10-23.md`); also git. Measurements: `RESULTS.md`, `results/*.md`. Labels: (m) measured, (mod) modeled, (a) assumed.

No further target session of ours follows (user 2026-10-31): the code goes to the target one way and an agent there reads and tests it, so each item is proven on aac7 / aac6 and every target-only gain gets a self-contained test in `target/`. Checkpoints and anything that needs target data are off the list (user 2026-11-08), including the mn_model v13 round term (aac7 floor 38-54 us per round against 0.355 ms at 576 nodes, m) and `ECALC_MAPD=1` repeats of M3/M5/M6: dropped.

## Open work

Order: bugs, shrink, simplify, performance, docs. Only performance rows are open.

1. `XSHMEM_ROUND_ACTIVE=1` (rounds sized by the active pairs, `results/TRANSPORT.md` N1, M6): built, off by default (user 2026-11-08). Gates PASS, digits identical; 3 n 1e10 ABBA +0.33 s [-0.11, +0.77] (8 pairs, m), not distinguishable from zero. Adopt only if a target A/B at 576 n shows a gain with identical digits.
2. `NEWTON_MN_LAZY=1` (reciprocal Q_t as a window view, `results/COMPUTE.md` §LAZY): merged, off by default; adopt only on target time. Parked behind a target measurement: shifts point-to-point instead of a group all-to-all, and F-out (design).
3. Two exchanges in flight per communicator: design only. The same feature (`COMM_LAYER_INTER2`) was built and removed 2026-10-21 because it was slower (3 n 1.9323e11: SHMEM 149.9 against 143.2 s, libfabric 140.2 against 137.2 s, m). Modeled gain at 576 n, 2e13: 10.5-17.5 s at 128 MiB rounds, 1.3-4.4 s at 512-1024 MiB, model error about 13 s (mod). Build (a second inter-node mesh behind `COMM_LAY_INTER2=1`, default off, about 120-180 lines, a) only if the target round-size A/B leaves at least 5 s; round size: keep 128, the A/B decides 512/1024 (`results/TRANSPORT.md` §R10).
4. Pool fragmentation at 576 n (54 remaps of 3.9 GB in the 10-29 target data): PARKED (user 2026-11-10). A reproduction needs a pool really near full, about 1.5e11 digits (more than 20 times the usual 2-node size); two aac7 attempts at a 99 % planner edge gave 0 remaps and 0 in-phase hipMalloc in 4 verified runs (2 n 6.441e10, with and without `ECALC_MAPD`, m; `results/MEMORY_IO.md` §S4). Reopens with a larger node count.
5. Campaign harness gaps (aac7 M1/M4): the 1-node M4 rerun filled the planner-against-memory table (`results/MEMORY_IO.md` §M4, 2026-11-10). Pending: the 2-node part and the clock-logging arm. The 2-node rerun stopped early on node memory (root file system growth on s5b0 and s6b0, not ours); screen MemAvailable before each run. Closed once both are done.

## Proposed (needs the user's priority)

One line each; nothing here is started without the user's word.

- Root cause of the 10-26 target failures at mixed piece layouts (2e13 handshake hang, 3e13 `P24 CRT flag` on nodes 283-376; both had a mixed level 7). Worked around by `MN_LEVEL_UNIFORM=1` on every record step. Ruled out at one node (2026-10-29, m): mixed 2-way shapes, partial transform-cache slots k=1..4, 32-bit size arithmetic. Leads: the block pool nearly full (373 of 373.44 GB per node at 3e13), fabric errors at 576 n, a per-product plane checksum to localize the corruption.
- Path 2 at the target's scale (mod, `results/MODEL.md` §P2JUMP): one process per GPU does not fit the target's per-node share from 8 nodes on (device 447.93 GB at 8 n, 517.67 at 576 n against 373.44); layout D fits. Making path 2 viable needs a smaller per-process footprint (`DIST_CHUNKS=2` or 4 fits, mod); `ladder.sh` drops a refused path. No run with one process per GPU at the target before the 3 n gate with `ECALC_PE_PER_GPU=1` and an ABBA (target question TQ84).
- Detect and retry (DR), `ECALC_DR` stays off (user 2026-11-09). DR untested paths pass in small tests (2026-11-08/09): per-PE failure histories, retry at the memory edge, and the cyclic wrap product with `DM_MN_WRAP=28` at 1e10 digits (recovery and persistent fatal); the retry added no pool peak beyond the 0.2 GB noise in 3 of 4 paired comparisons at 6.441e10, 2 nodes (`DM_MN_WRAP=31`, the 4th at the tolerance). Open: ABBA of `ECALC_DR=3` against unset at 6 nodes when nodes allow (2-node result +0.40 %, CI includes 0; model 0.1-0.2 % at 576 nodes untested; about 8 runs per arm). The retry peak memory at the memory edge stays unmeasured (both base runs at the tighter planner limit died on a start-up hipMalloc out of memory, `results/CORRECTNESS.md`). A crash seen only with the second `DM_MN_WRAP` setting is recorded; chase it only if it repeats on a clean node (user 2026-11-10).
- `BS_ARENA_TRIM_GB` stays opt-in (user 2026-11-10; saves 34.4 GB per node at 2 n 1.2882e11, time neutral, m, `results/MEMORY_IO.md` §ATRIM).
- aac7 bimodal run times (3 n 1e11, both backends): about half the runs are 5-8 % slower (`results/TRANSPORT.md` §BM). Next: the 3 n ABBA with per-launch NIC counters once three nodes with working NICs exist (aac7 has two); until then pair every A/B run.
- Cray SHMEM path remaining gap (final +7.5 %, CI +3.4 to +11.5 %, 11 interleaved pairs, `results/TRANSPORT.md` craylane): init +8 s (libsma heap NIC registration 6.2 s, hipHostRegister 1.5 s, barrier 1.4 s; aac7 4 KiB shared heap, needs THP shmem or hugetlb, admin). The ~4.5 s first-exchange post is queued GPU work, not an overhead (code read 2026-11-09). Re-measure on the target (Cray SHMEM 12.1) before any work here. Cray default layout D is 2.4x slower than P (four APU threads share one libsma heap at thread level 3); the target layout is P.
- Same-node peer path (`XSHMEM_PEER`, `results/COMPUTE.md` §PEER): open are a 3 n run for the gain and the per-PE heap peak (needs a third NIC node) and the Cray path through `shmem_ptr` at the target.
- Transient fabric error watch (2026-10-28, m): `t_vpe hs` with `XSHMEM_COLL=tree` at 4 PEs once ended with a cxi `PCIE_UNSUCCESS_CMPL` completion error, 6 of 6 reruns pass; note any repeat.
- 6 n paired ABBA of grid [2,6] against [6], at least 8 runs per arm (earlier data +23 +/- 12 s, unexplained, m); aac7.

## Decisions (user) still acting

Others are stated in `README.md` (two paths, Q85), `RESULTS.md` (defaults in force) and `OPPORTUNITIES.md` (not adopted).

- Checkpoints after the tree and after the reciprocal are not built (user 2026-11-01, 2026-11-08).
- `ECALC_MAPD` is on by default above one process (user 2026-11-08).
- `ECALC_NP=3` above one node is refused (rc 3); `ECALC_NP=auto` plans.
- Items whose gain only the target can show are built behind off switches and digits-gated on aac7: two NICs per APU on path 1 (`XSHMEM_LANES=2`, layout D), the launch-time switch-group check (`ecalc/topo_check.py`, `MNRUN_TOPO_CHECK`), the hierarchical exchange (`OPPORTUNITIES.md` N9).

Performance ideas, open or tried, are in `OPPORTUNITIES.md`.
