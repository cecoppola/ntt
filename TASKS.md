# TASKS — live task list (open work, decisions, target session)

Scope: open work, user decisions still acting, proposed items and target-session items; closed items are in the
archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.

Closed rows and older material are in `archive:TASKS_HISTORY_2026-10-19.md`, `archive:TASKS_HISTORY_2026-10-23.md` (older: `archive:TASKS_HISTORY_2026-10-12.md`); history is also in git. Measurements are in `RESULTS.md` and `results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work, in priority order (user 2026-10-12)

Classes: 1 bugs, 2 shrink, 3 simplify, 4 performance, 5 docs (definitions in the project instructions). Within a class,
rows are in the order to do them.

Scheduled for the next work session (user 2026-10-26):
- Class 1: every OpenMP team sized from the task's affinity mask, not `omp_get_max_threads()` (latched at launch). The target's
  login env (`SLURM_HINT=nomultithread`, no `-c`) latches 1-2 threads; BS_SEED_THREADS=auto and the mnrun hint drop cover the
  seeds and the standard launcher; still exposed: `bs_nthreads()` teams in binsplit.c and the multi-node digit residue in
  mn_out.c (13.1-13.3 s at 64 nodes on the target vs 0.24-0.37 s per 8e9 node share on aac7 at 1 node, m; cause modelled).
  Behind a switch, off by default; gate 1 node now, 2 nodes when a second networked aac7 node is free; update on new target data.
- Class 4: balance the top bs levels across the 4 APUs of a node: APU 0 holds them (aac7 8e9: 34.5 vs 23.8 GB, m; target
  2.7-6.5 GB above the planner's per-APU figure, m); it sets the per-node size edge and may explain the 3e13 P24 flag (mod).
  Calibration of `ECALC_FIT_PER_APU` against measured per-APU peaks first (aac7 item e6e).
  `ECALC_FIT_PER_APU` calibrated (aac7 2026-10-27, m): planner per-APU max 106.88 vs 105.6 GB measured on APU 0 at 9e10,
  111.18 vs 109.9 at 1e11 (APU 1-3 86-91 GB).
- Class 1: `mnrun.sh` defaults `MNRUN_CPUS_PER_TASK` to auto (hand launches got 1-2 CPUs per task on the target); time model
  (`ecalc/mn_model.py`): round size 128 MiB as in the code (it assumed 1024), a profile fitted to the target's 2/64/576-node
  phase times (8.8x low at 576 nodes, m vs mod) with a per-round synchronisation term.
- Class 1: start-up timeline lines for mesh creation and first-call handshakes (`ECALC_INIT_TL=1`): the target's 576-node init
  was 111 s against about 16 s at 64 nodes (m), cause unknown.
- Class 4: tree (dissemination) barrier, max and allgather in `comm_xshmem.c`, switch off by default: today every member
  signals every other (O(n) per collective; 124 shifts cost 15.1 s at 576 nodes, m); per-peer pipelined exchange rounds (stretch).
- Class 4: digit residue with a precomputed-quotient multiply (bit-identical) and run in the background for every run
  (13.15 s per node at 8e9 digits on the target, m).
- Then: frozen branch remote-v3 with the gated changes and the next target-session run plan (user 2026-10-27).

## Proposed (needs the user's priority)

- Path 2 at the target's scale (found 2026-10-25, planner, mod): one process per GPU (path 2, and P on path 1) does not fit the target run's per-node share (6.632e10 digits per node) from 8 nodes on: device 447.93 GB at 8 n, 473.70 at 64 n, 517.67 at 576 n even at 3e13 (limit 373.44), node 577.77-648.59 GB (budget 480); layout D fits (360.95 GB at 8/64 n; 358.96 at 576 n with `MN_NONPOW2_CAP=0`). Q120 (user, 2026-10-25): the first target campaign runs 8e9 digits per node, where every path fits at 1/8/64/576 nodes (path 2 at 576 n: device 328.69 GB, node 406.32 GB, mod); making path 2 viable at larger sizes needs a smaller per-process device and host footprint (after the campaign). `ladder.sh` drops a refused path. At 1e13 on 576 n (Q121 step s2) path 2 is 6.79 GB over the device (380.23 GB, mod). Cause and fix options (results/LAUNCH_DEFAULTS.md §P2JUMP, mod): the non-power-of-two group (576 n = 2304 PEs = 9·2^8) makes the v-exchange slots resident, and `DIST_CHUNKS` is 1 under one process per GPU; `DIST_CHUNKS=2` (device 318.56 GB) or 4 (288.22 GB) fits; never run with one process per GPU: the 3 n gate with `ECALC_PE_PER_GPU=1` and an ABBA are target question TQ84 (user 2026-10-25; aac7 has 2 NIC nodes).
- Start-up: plane pools sized to the run [deferred by the user 2026-10-25; estimates (assumed): smaller caps 2^24-2^27 for 1e8-1e9 runs about 6-7 h incl. gate, gain about 2-2.5 s per small run (mod); L-A per-device allocator processes about 3-5 days after 1-2 h of research on cross-process memory sharing under ROCm 7.14, gain up to the measured path 1 vs path 2 init gap at 8e9 per node on 2 n (14.4 vs 6.4 s)] (Q122 (b), measured 2026-10-25): the two plane pools are always mapped at the 2^31 cap (25.8 GB per GPU, a fixed 4.6-5.6 s of init at 1e8-8e9 on 1 node, m); `ECALC_PLANE_CAP=2^30` cuts total 2.64 s [-2.98, -2.30] at 8e9 and 2.05 s at 1e9 with compute unchanged and digits identical (m) and is on the 8e9 campaign's launch line (Q122 (a)); `ECALC_PLANE_CAP=run` built 2026-10-25 (off by default; results/MEMORY.md): picks 2^30 when the division's largest product fits the 2^30 cap's plane (1 node up to 9.6e9 digits, 2 nodes up to about 1.9e10 total), else 2^31; gate c8c/c8d PASS, digits identical (1e9 1 n 8.23 s against 11.55 s unset, 1.6e10 2 n 22.67 s against 26.20 s; single runs, m); conservative at non-power-of-two node counts (3 n at 8e9 per node picks 2^31, mod), and the default since 2026-10-25 (user decision; gate c9a_rundefg PASS, 1 n + 2 n both paths, digits identical); at the 8e9 campaign share it picks 2^30 at 1/8/64/576 n (mod), so the target launch line no longer sets the cap (Q122 (c)). Open: smaller caps (2^24-2^27 at 1e8-1e9) would need a planner change. Also (m): at 1 node 8e9 with the 2^30 cap, start-up switches `DB_POOL_VMM_CHUNK_GB` 1 and 4, `BS_ARENA_ROOM` 0.07 and `BS_SEED_THREADS` 48 gave no total gain (-0.23 s [-0.50, +0.03], +0.36 s [-0.00, +0.69], -0.13 s [-0.41, +0.12], +0.14 s [-0.19, +0.45] with init -0.42 s moved into compute +0.56 s; A/A +0.03 s [-0.20, +0.26]; 12 ABBA blocks each, digits identical, aac7 2026-10-25). Layout D at 3 n 1e11 spends 25-31 s in init (m, results/XSHMEM.md §PPG S2d: region pools / VMM arenas 13.5-15 s, plane pools ~5 s, layered self-test 3.7-4.2 s, staging ~2 s) against ~10 s with one process per GPU, because one process mapping memory for 4 GPUs is serialised in the HIP runtime (D ~20 GB/s per node, P ~150 GB/s). Tried, not adopted (`OPPORTUNITIES.md`: S-A / VMM0, REUSE / REGFIRST / INITOV, STARTSW): `MN_SELFTEST` skip, `DB_POOL_VMM=0`, `RNS_INIT_OVERLAP`. Remaining lever L-A: per-device allocator processes so D maps like P (~20 s, modelled), not started.
One line each (what, why, size); nothing here is started without the user's word.


- aac7 bimodal run times (3 n 1e11, both backends): about half the runs are 5-8 % slower (evidence: results/NODE_SLOWMODE.md §BM; needs the fabric); next: the 3 n ABBA with per-launch NIC counters once three nodes with working NICs exist (aac7 has two nodes with working NICs, 2026-10-25); until then pair every A/B run.
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
- Self-test cost (user 2026-10-26: keep the start-up self-tests if they really take < 15 s): read the self-test seconds
  (`ECALC_INIT_TL=1`) at 2 nodes and 576 nodes with the fixed seed default (576 n layered test 3.1-3.5 s; aac7 3 n 3.7-4.2 s, m).
