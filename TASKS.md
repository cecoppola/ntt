# TASKS — live task list (open work, decisions, target session)

Scope: open work, user decisions still acting, proposed items and target-session items; closed items are in the
archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.

Closed rows and older material are in `archive:TASKS_HISTORY_2026-10-19.md`, `archive:TASKS_HISTORY_2026-10-23.md` (older: `archive:TASKS_HISTORY_2026-10-12.md`); history is also in git. Measurements are in `RESULTS.md` and `results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work, in priority order (user 2026-10-12)

Classes: 1 bugs, 2 shrink, 3 simplify, 4 performance, 5 docs (definitions in the project instructions). Within a class,
rows are in the order to do them.

Built in the 2026-10-27 work sessions on branch `cand3` (= `cand` + the failures session + Q130 + the overnight session Q131; frozen copy remote-v8; every new behaviour behind a switch, off by
default except the launcher `-c` auto), 1-node gates passed on aac7 (m: builds and gates both paths, switches off = remote-v4 planner
lines in 4 cases, all on = identical digits, ABBA no slower). Still open, in order:
- Class 1: 2-node gates (both paths) of `cand3` (= remote-v8; item e8b ready, also the overnight cases: the CRT-flag injection with `P24_CHECK=0` and `t_vpe idcoll` across nodes, 1e11 A/B of `BS_MDEV_LOGL=28 DIST_CHUNKS=16`; also the full-run P24 cases at 2 nodes, 1e11 with natural 1 x 2 pieces and 1.6e10 with `MN_TREE_LOGN_TEST=26`, under `P24_CHECK` 1 / 2), then merge to main (needs a second networked aac7 node, free from 10-31).
- Done 2026-10-27 (failures session, `remote/ROUTE.md` steps 4b, 5b, 7): `P24_CHECK` (3e13 CRT flag: input limbs checked as stored and as
  received, CRT flag at any shift; =2 no transform cache; `P24_CHECK_TEST` fault injection), `COMM_SEQ_CHECK`, `XSHMEM_HANG_S`,
  `XSHMEM_HS_BATCH` (2e13 handshake hang), `XSHMEM_SPARSE` (zero-byte peers skipped: the shift cost), `ECALC_INIT_TL` process age and
  init split (576-node init). Evidence (m, aac7 1 node): `tests/t_mn_grid 1 28` 158 P24 products per PE, 1088 operands checked, 0 flags;
  both fault injections fatal as designed; 6.4e10 ABBA all four new switches on vs off: one process per device 80.20 -> 80.10 s
  (-0.1 %, -2.80 .. +2.61 s), one per node 74.33 -> 74.26 s (-0.1 %, -3.07 .. +2.93 s). Audit verdicts (code reading): the P24 bound
  holds for every product kind (`results/PRIMES_STRATEGY.md` §P24 audit); the hang is most likely a straggler past the fixed 900 s
  (`results/XSHMEM.md` COMM2). Target benches dry-run on aac7: 8/8 run.
- Done 2026-10-27/28 (overnight session Q131, 1-node gates on aac7, m): VPE sub-communicator ids for non-power-of-two groups of 64+ PEs (the old rule was fatal: layout P / path 2 at 576 nodes would have died at the 768-PE groups; `results/XSHMEM.md`; host audit 2..2304 PEs, 0 collisions); the default all-shift CRT flag proven with `P24_CHECK_TEST=2` at `P24_CHECK=0` (`P24_CHECK_TEST_S`); route scripts: per-run summary lines, `s5_run.sh --hosts`, `arms.sh` (concurrent arms in one allocation, `remote/ROUTE.md`); record per-APU edge (`results/LAUNCH_DEFAULTS.md` on `cand3`, record-edge section: 3.82e13 has +3.62 GB per APU with today's S6 line, +7.38 GB with `BS_MDEV_LOGL=28 DIST_CHUNKS=16`, but `BS_MDEV_LOGL=28` costs +13.4 s at 1e11 on 1 node, -0.9 s at 6.4e10); path 2 at 576 n (§P2JUMP: 1e13 fits, 3e13 never); peer-path measurement and design (`results/SINGLE_NODE.md` §PEER); aac6 (re-authorized): unit tests pass under ROCm 7.14.1 and 10.1 on a healthy node; four of its five SPX nodes are degraded (`t_ntt` 0.05-0.11 TB/s against 2.1, 1.6e10 108 s against 24.6 s), so aac6 timing needs a node that passes the `t_ntt` rate check; `BS_MDEV_LOGL=28 DIST_CHUNKS=16` costs nothing resolved at 6.632e10-9e10 digits on 1 node (the record's per-node share), +13 s at 1e11; peer-path round counters `XSHMEM_ROUND_STATS` and `XSHMEM_PEER_STREAMS` (the exchange wait is mostly the post's stream sync on earlier GPU work, `results/SINGLE_NODE.md` §PEER).
- Done 2026-10-27 (Q130, user): the P24 CRT flag covers every shift class by default (`P24_CRT_ANY`, fallback 0 = the old s = 12 flag); record runs from 1e13 set `XSHMEM_HANG_S=3600` (`remote/ROUTE.md` S6; default 900); the 3e13 failure inference (no env.txt: `MN_NONPOW2_CAP=0` most likely; only 3e13 mixes one- and two-piece products across groups at tree levels 1-6, and every flagged node lies in nodes 192-383, mod) in `remote/FACTS.md` and ROUTE 7c. Evidence (m, aac7 1 node): builds and gates both paths, `t_p24crt`, `t_mn_grid` with mixed grid shapes on 4 and 8 PEs (176 P24 products per PE, 0 flags), both injections fatal, 1.6e10 digits identical; remote-v6 archive check (manifest 274/0/0).
- Done 2026-10-27 (user decisions): `XSHMEM_ROUND_PIPE` removed from `cand` (it was +10.5 % slower on path 2 at 1 node, m);
  `BS_TOP_SPREAD` (B4) dropped, `BS_MDEV_LOGL=29` adopted for target runs from 1e13 digits (`remote/ROUTE.md` S6).
  1-node evidence for the remaining switches (`ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup XSHMEM_COLL=tree`):
  6.4e10 ABBA (m), off vs mask+shoup+tree: one process per device 89.68 -> 87.79 s (-2.1 %, interval -2.33 .. -1.44 s);
  one per node 76.95 -> 77.50 s (+0.7 %, -0.08 .. +1.18 s, not resolved); split, 4 rounds each (m): mask alone -0.62 s (-2.14 .. +0.90), shoup + tree +0.39 s (-1.80 .. +2.58), both within noise; path 2 at 6.4e10: 131.43 -> 127.91 s (-2.7 %, -8.09 .. +1.06 s, not resolved).
  Launcher `-c` auto confirmed (m): `-c 192` for one task per node, `-c 48` for 4, none when `MNRUN_CPUS_PER_TASK=` is empty.
- Target session: the gains (team sizing, residue, tree collectives, round size, init timeline) are measured on the target
  (remote/ROUTE.md "Next target session").

## Proposed (needs the user's priority)

- Communicator ids of cut runs (found 2026-10-27, code): non-power-of-two sub-communicator ids are `256 + 64*slot + size`, so a cut last run of size k that shares a PE with a full run of size k collides (`communicator id used twice`, seen in `t_vpe hs` at 8 PEs, fixed in the test); check whether any engine partition can form such a pair at the target's node counts (code reading, small).
- Path 2 at the target's scale (found 2026-10-25, planner, mod; updated 2026-10-27 in `results/LAUNCH_DEFAULTS.md` §P2JUMP: 1e13 fits by +0.45 GB per APU under the 487 GB node budget, 3e13 never fits): one process per GPU (path 2, and P on path 1) does not fit the target run's per-node share (6.632e10 digits per node) from 8 nodes on: device 447.93 GB at 8 n, 473.70 at 64 n, 517.67 at 576 n even at 3e13 (limit 373.44), node 577.77-648.59 GB (budget 480); layout D fits (360.95 GB at 8/64 n; 358.96 at 576 n with `MN_NONPOW2_CAP=0`). Q120 (user, 2026-10-25): the first target campaign runs 8e9 digits per node, where every path fits at 1/8/64/576 nodes (path 2 at 576 n: device 328.69 GB, node 406.32 GB, mod); making path 2 viable at larger sizes needs a smaller per-process device and host footprint (after the campaign). `ladder.sh` drops a refused path. At 1e13 on 576 n (Q121 step s2) path 2 is 6.79 GB over the device (380.23 GB, mod). Cause and fix options (results/LAUNCH_DEFAULTS.md §P2JUMP, mod): the non-power-of-two group (576 n = 2304 PEs = 9·2^8) makes the v-exchange slots resident, and `DIST_CHUNKS` is 1 under one process per GPU; `DIST_CHUNKS=2` (device 318.56 GB) or 4 (288.22 GB) fits; never run with one process per GPU: the 3 n gate with `ECALC_PE_PER_GPU=1` and an ABBA are target question TQ84 (user 2026-10-25; aac7 has 2 NIC nodes).
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
