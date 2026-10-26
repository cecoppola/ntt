# TASKS — live task list (open work, decisions, target session)

Scope: open work, user decisions still acting, proposed items and target-session items; closed items are in the
archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.

Rewritten 2026-10-19, cleaned 2026-10-23 and 2026-10-25 (finished rows and settled decisions removed: history is in git, `RESULTS.md` and `results/`). Closed rows and older material are in
`archive:TASKS_HISTORY_2026-10-19.md`, `archive:TASKS_HISTORY_2026-10-23.md` (older: `archive:TASKS_HISTORY_2026-10-12.md`). Measurements are in `RESULTS.md` and
`results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work, in priority order (user 2026-10-12)

Classes: 1 bugs, 2 shrink, 3 simplify, 4 performance, 5 docs (definitions in the project instructions). Within a class,
rows are in the order to do them.

None open.

## Proposed (needs the user's priority)

- Path 2 at the target's scale (found 2026-10-25, planner, mod): one process per GPU (path 2, and P on path 1) does not fit the target run's per-node share (6.632e10 digits per node) from 8 nodes on: device 447.93 GB at 8 n, 473.70 at 64 n, 517.67 at 576 n even at 3e13 (limit 373.44), node 577.77-648.59 GB (budget 480); layout D fits (360.95 GB at 8/64 n; 358.96 at 576 n with `MN_NONPOW2_CAP=0`). Q120 (user, 2026-10-25): the first target campaign runs 8e9 digits per node, where every path fits at 1/8/64/576 nodes (path 2 at 576 n: device 328.69 GB, node 406.32 GB, mod); making path 2 viable at larger sizes needs a smaller per-process device and host footprint (after the campaign). `ladder.sh` drops a refused path. At 1e13 on 576 n (Q121 step s2) path 2 is 6.79 GB over the device (380.23 GB, mod). Cause (results/LAUNCH_DEFAULTS.md §P2JUMP, mod): not the node count but a non-power-of-two group (576 n = 2304 PEs = 9·2^8): the general-map transform's v-exchange slots are resident and scale as 1/`DIST_CHUNKS`, which is 1 under one process per GPU (8 in layout D), so 15.29 GB per APU instead of 1.92, booked twice (slots + pools); every power-of-two count gives 276.13 GB. Fix candidate: `DIST_CHUNKS=2` (device 318.56 GB, node 414.09) or 4 (288.22, node 383.76), mod; never run with one process per GPU: needs the 3 n gate with `ECALC_PE_PER_GPU=1` and an ABBA; needs 3 NIC nodes (aac7 has 2) or the target.
- Start-up: plane pools sized to the run (Q122 (b), measured 2026-10-25): the two plane pools are always mapped at the 2^31 cap (25.8 GB per GPU, a fixed 4.6-5.6 s of init at 1e8-8e9 on 1 node, m); `ECALC_PLANE_CAP=2^30` cuts total 2.64 s [-2.98, -2.30] at 8e9 and 2.05 s at 1e9 with compute unchanged and digits identical (m) and is on the 8e9 campaign's launch line (Q122 (a)); `ECALC_PLANE_CAP=run` built 2026-10-25 (off by default; results/MEMORY.md): picks 2^30 when the division's largest product fits the 2^30 cap's plane (1 node up to 9.6e9 digits, 2 nodes up to about 1.9e10 total), else 2^31; gate c8c/c8d PASS, digits identical (1e9 1 n 8.23 s against 11.55 s unset, 1.6e10 2 n 22.67 s against 26.20 s; single runs, m); conservative at non-power-of-two node counts (3 n at 8e9 per node picks 2^31, mod), so it does not replace the fixed 2^30 on the target launch line. Open: make it the default (user decision); smaller caps (2^24-2^27 at 1e8-1e9) would need a planner change. Also (m): at 1 node 8e9 with the 2^30 cap, start-up switches `DB_POOL_VMM_CHUNK_GB` 1 and 4, `BS_ARENA_ROOM` 0.07 and `BS_SEED_THREADS` 48 gave no total gain (-0.23 s [-0.50, +0.03], +0.36 s [-0.00, +0.69], -0.13 s [-0.41, +0.12], +0.14 s [-0.19, +0.45] with init -0.42 s moved into compute +0.56 s; A/A +0.03 s [-0.20, +0.26]; 12 ABBA blocks each, digits identical, aac7 2026-10-25). Layout D at 3 n 1e11 spends 25-31 s in init (m, results/XSHMEM.md §PPG S2d: region pools / VMM arenas 13.5-15 s, plane pools ~5 s, layered self-test 3.7-4.2 s, staging ~2 s) against ~10 s with one process per GPU, because one process mapping memory for 4 GPUs is serialised in the HIP runtime (D ~20 GB/s per node, P ~150 GB/s). Tried, not adopted (`OPPORTUNITIES.md`: S-A / VMM0, REUSE / REGFIRST / INITOV, STARTSW): `MN_SELFTEST` skip, `DB_POOL_VMM=0`, `RNS_INIT_OVERLAP`. Remaining lever L-A: per-device allocator processes so D maps like P (~20 s, modelled), not started.
One line each (what, why, size); nothing here is started without the user's word.


- aac7 bimodal run times (3 n 1e11, both backends): about half the runs are 5-8 % slower in every compute phase (measured 2026-10-23, 9zs19-9zs29); not time-correlated (Fisher p = 1.0) and absent at 1 node without network (271 runs, unimodal, 9zv3_bm2d, results/NODE_SLOWMODE.md §BM), so it needs the fabric or more than one node; next: the 3 n ABBA with per-launch NIC counters once three nodes with working NICs exist (aac7 has two on 2026-10-24); until then pair every A/B run.
- Cray SHMEM path remaining gap (final +7.5 %, CI +3.4 to +11.5 %, 11 interleaved pairs, results/XSHMEM.md craylane): init +8 s (libsma heap NIC registration 6.2 s, hipHostRegister 1.5 s, barrier 1.4 s; aac7 4 KiB shared heap, needs THP shmem or hugetlb, admin) and a ~4.5 s first-exchange post (stream sync on every PE; not GPU page-table first touch); re-measure both on the target (Cray SHMEM 12.1) before any work here. Cray default layout D is 2.4x slower than P (four APU threads share one libsma heap at thread level 3); the target layout is P.
- Same-node peer path follow-ups (`XSHMEM_PEER`, default on under P since Q100, results/SINGLE_NODE.md §PEER): a 3 n run for the gain and the per-PE heap peak (needs a third NIC node); signals and control words by direct writes and copies spread over several SDMA engines (dm still +3.5 s behind D at 1 n 3.3e10, m); the Cray path through `shmem_ptr` at the target session.
- 6 n paired ABBA of grid [2,6] against [6], at least 8 runs per arm (earlier data +23 +/- 12 s, unexplained, m); aac7, one item.

## Decisions (user) still acting

Only decisions that no central doc states: the others are in `README.md` (two paths, Q85), `RESULTS.md` (defaults in force: Q58, Q73, Q96, Q99, Q100, Q103) and `OPPORTUNITIES.md` (not adopted: Q101, Q102).

- Q97 (user 2026-10-24): Q79/Q88 code stays until the target session (Q103 then removed the plain SHMEM transport).
- Q103-Q111 (user 2026-10-25): all (a), executed in cleanup round 4 (row above).

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are in `OPPORTUNITIES.md`.

## Target session

Target work: `target/RESTART.md` and `target/bench/QUESTIONS.md`.
