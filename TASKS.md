# TASKS — live task list (open work, proposed items, decisions)

Scope: open work, user decisions still acting and proposed items; closed items are in the archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.
Closed rows and the 10-29 to 11-03 done-lists are in `archive:cleanup_1107/TASKS_before_1107.md` (older: `archive:TASKS_HISTORY_2026-10-12.md`, `archive:TASKS_HISTORY_2026-10-19.md`, `archive:TASKS_HISTORY_2026-10-23.md`); history is also in git. Measurements are in `RESULTS.md` and `results/*.md`. Labels: (m) measured, (mod) modeled.

## Open work, in priority order (user 2026-10-12)

Classes: 1 bugs, 2 shrink, 3 simplify, 4 performance, 5 docs (definitions in the project instructions). Within a class, rows are in the order to do them. No target session of ours follows (user 2026-10-31): the code is imported to the target one way and an agent there reads and tests it, so each item is proven on aac7 / aac6 and every target-only gain gets a self-contained test in PLAN.md.

Class 1
1. DONE (branch c1): launch script fixes (alloc.sh and the target scripts do not inherit SLURM_HINT, one REMOTE_DEVICE_GB rule in lib.sh, NIC form 0,1;2,3;4,5;6,7, PLAN hugetlb row).

Class 4
8. Exchange rounds sized by active pairs (zero peers skipped).
9. Shifts point-to-point instead of a group all-to-all; lazy shift: F-in (reciprocal Q_t as a view, `NEWTON_MN_LAZY`) built on branch lazy, not merged, gates PASS (`results/COMPUTE.md` §LAZY); F-out (design) parked behind the target measurement.
10. Larger rounds for large all-to-alls, re-evaluated with the recalibrated model.
11. Two exchanges in flight per communicator (design, after rows 8-10).
12. Start-up: per-device allocator processes (arenas and the per-prime plane pools, `ECALC_MAPD`, user decision 2026-11-01), seed-thread pool wait, self-test time visible, pool fragmentation (S4, 2026-11-02: the 2-node 6e10 gather fallback is not the cache slots, `results/MEMORY_IO.md` §S4; the 576-node remaps are not reproduced, open).
13. Checkpoints after the tree and after the reciprocal: not built for now (user decision 2026-11-01).
Items whose gain only the target can show are built behind switches and digits-gated on aac7: two NICs per APU on path 1 (`XSHMEM_LANES=2`, layout D), the launch-time switch-group check (`ecalc/topo_check.py`, `MNRUN_TOPO_CHECK`), the hierarchical exchange (OPPORTUNITIES N9).

## Proposed (needs the user's priority)
- Root cause of the 10-26 target failures at mixed piece layouts (2e13 handshake hang, 3e13 `P24 CRT flag` on nodes 283-376; both had a mixed level 7). Worked around in remote-v12 by `MN_LEVEL_UNIFORM=1` on every record step. Ruled out at one node (2026-10-29, m): mixed 2-way shapes, partial transform-cache slots k=1..4 (test setting `RNS_PC_TEST_P`, since removed; branch shape6), 32-bit size arithmetic. Leads: the block pool nearly full (373 of 373.44 GB per node at 3e13), fabric errors at 576 n, a per-product plane checksum to localise the corruption; the shape6 run 5e9:6:25 with `RNS_PC_TEST_P=2` timed out before its first product (not rerun).

- Path 2 at the target's scale (mod, `results/MODEL.md` §P2JUMP): one process per GPU (path 2, and P on path 1) does not fit the target run's per-node share from 8 nodes on (device 447.93 GB at 8 n, 517.67 at 576 n against the 373.44 GB limit); layout D fits. Making path 2 viable at larger sizes needs a smaller per-process device and host footprint (`DIST_CHUNKS=2` or 4 fits, mod); `ladder.sh` drops a refused path. Never run with one process per GPU at the target before the 3 n gate with `ECALC_PE_PER_GPU=1` and an ABBA (target question TQ84).
- Start-up (`results/MEMORY_IO.md` §MAPBENCH, `results/TRANSPORT.md` §PPG): layout D spends 25-31 s in init at 3 n 1e11 against about 10 s with one process per GPU (m): one process mapping memory for 4 GPUs is serialized in the HIP runtime. Remaining lever L-A: per-device allocator processes (`ECALC_MAPD`), not started further. Smaller plane caps (2^24-2^27 at 1e8-1e9) would need a planner change (`ECALC_PLANE_CAP=run` is the default). Tried, not adopted: `OPPORTUNITIES.md` S-A / VMM0, REUSE / REGFIRST / INITOV, STARTSW.
One line each (what, why, size); nothing here is started without the user's word.

- DR (C3) timing at scale: ABBA of `ECALC_DR=3` against unset at 6 nodes when nodes allow (2-node result +0.40 %, CI includes 0; model 0.1-0.2 % at 576 nodes is untested); one item, about 8 runs per arm.
- DR launcher check: confirm that every multi-node launcher path the target launch scripts use ends all nodes when one calls `ec_fatal` (kill-on-bad-exit); only the `mnrun.sh` srun path and the bench launcher were checked, the latter by `bash -n`.
- DR untested paths (`results/CORRECTNESS.md`): different per-PE failure histories, retry peak memory at the memory edge, D' injection before normalization; a small test each if the user wants `ECALC_DR` adopted.

- aac7 bimodal run times (3 n 1e11, both backends): about half the runs are 5-8 % slower (evidence: results/TRANSPORT.md §BM; needs the fabric); next: the 3 n ABBA with per-launch NIC counters once three nodes with working NICs exist (aac7 has two nodes with working NICs, 2026-10-25); until then pair every A/B run.
- Cray SHMEM path remaining gap (final +7.5 %, CI +3.4 to +11.5 %, 11 interleaved pairs, results/TRANSPORT.md craylane): init +8 s (libsma heap NIC registration 6.2 s, hipHostRegister 1.5 s, barrier 1.4 s; aac7 4 KiB shared heap, needs THP shmem or hugetlb, admin) and a ~4.5 s first-exchange post (stream sync on every PE; not GPU page-table first touch); re-measure both on the target (Cray SHMEM 12.1) before any work here. Cray default layout D is 2.4x slower than P (four APU threads share one libsma heap at thread level 3); the target layout is P.
- Same-node peer path follow-ups (`XSHMEM_PEER`, default on under P since Q100, results/COMPUTE.md §PEER): a 3 n run for the gain and the per-PE heap peak (needs a third NIC node); signals and control words by direct writes and copies spread over several SDMA engines (dm still +3.5 s behind D at 1 n 3.3e10, m); the Cray path through `shmem_ptr` at the target session.
- GPU work before the exchanges on layout P (found 2026-10-28, m, `results/COMPUTE.md` §PEER): kernel-launch fusion of operand gather, NTT passes and pack (bound 1.9-2.8 s of 34 s at 3.3e10 on P, 1.2-1.5 % on layout D at 1 node; upper bounds); `k_gather_mn` 2.3 s per PE before the forward transforms as its own item; whether the record's multi-node layout D has busy posts: the e8b series arm ran on 2026-10-31 (2 n 1.3264e11 layout D, `XSHMEM_ROUND_STATS=2`, VERIFY OK, 128.2 s, m); its per-exchange series lines are kept on aac7 and not yet analysed.
- Transient fabric error watch (2026-10-28, m): `t_vpe hs` with `XSHMEM_COLL=tree` at 4 PEs on s5b0 once ended with a cxi `PCIE_UNSUCCESS_CMPL` completion error (gate e10m), 6 of 6 reruns pass, NICs healthy before and after; note any repeat.
- 6 n paired ABBA of grid [2,6] against [6], at least 8 runs per arm (earlier data +23 +/- 12 s, unexplained, m); aac7, one item.
- Next steps after the aac7 campaigns (offered 2026-11-03, saved by the user for later): (a) analyse the per-exchange series of the 2-node e8b run (logs on aac7; does time per round grow within a run, the nearest aac7 proxy for the 575-peer excess) and fix the campaign harness gaps (row below); (b) DR launcher check and DR untested paths (rows above), before `ECALC_DR` is used on the target; (c) kernel fusion before the exchanges (row above; bound 1.9-2.8 s of 34 s at 1 node, mod); (d) target data: mn_model v13 round term (aac7 floor 38-54 us per round, m, against 0.355 ms at 576 nodes, m) and `ECALC_MAPD=1` repeats of M3/M5/M6.
- Campaign harness gaps (aac7 campaigns M1/M4, 2026-11-03): `tools/stress/stress.sh` deletes clean-run logs, so the `DR:` exit lines are lost and the DR false-positive count of a soak cannot be read (results/MEMORY_IO.md M1); keep those lines per run. The M4 driver overwrote memory files per size, so planner vs measured exists for 3 points only (results/MEMORY_IO.md M4); keep them per size. Small each.

## Decisions (user) still acting

Only decisions that no central doc states: the others are in `README.md` (two paths, Q85), `RESULTS.md` (defaults in force: Q58, Q73, Q96, Q99, Q100, Q103) and `OPPORTUNITIES.md` (not adopted: Q101, Q102).

- Q97 (user 2026-10-24): Q79/Q88 code stays until the target session (Q103 then removed the plain SHMEM transport).

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are in `OPPORTUNITIES.md`.
