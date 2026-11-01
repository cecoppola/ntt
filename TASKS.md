# TASKS — live task list (open work, decisions, target session)

Scope: open work, user decisions still acting, proposed items and target-session items; closed items are in the
archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.

Closed rows and older material are in `archive:TASKS_HISTORY_2026-10-19.md`, `archive:TASKS_HISTORY_2026-10-23.md` (older: `archive:TASKS_HISTORY_2026-10-12.md`); history is also in git. Measurements are in `RESULTS.md` and `results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work, in priority order (user 2026-10-12)

Classes: 1 bugs, 2 shrink, 3 simplify, 4 performance, 5 docs (definitions in the project instructions). Within a class,
rows are in the order to do them.

Plan from the 10-29 target reports (user approval 2026-10-31). No target session of ours follows: the code is imported to the target one way and an agent there reads and tests it, so each item is proven on aac7 / aac6 and every target-only gain gets a self-contained test in PLAN.md (row 14):

Class 1
1. Launch-kit fixes: alloc.sh without SLURM_HINT, one REMOTE_DEVICE_GB -> ECALC_DEVICE_GB rule in lib.sh, NIC form 0,1;2,3;4,5;6,7, PLAN hugetlb row (branch kitfix).
2. Block-pool audit: frees and VMM remaps against GPU work still in flight (q_free / vmm_make_room have no stream sync); a sync switch to test it (branch c1pool).
3. aac7 overnight stress test for the size-dependent corruption (3.7e13+ failed 6 of 6 on the target): pool pressure, tiny rounds, P24_CHECK=2, diagnostic words, the row-2 switch on/off. After it the user decides the scope of row 4.
4. Detect and retry: per-prime check of every distributed product (coefficient sums), retry instead of a fatal on a mismatch or CRT flag, location logged; fault-injection test.
5. Node health gate: EDAC ue/ce counts and boot time per node in the node screen; per-run NIC error counter deltas.
6. Per-APU memory guard and true per-APU high-water (per-NUMA MemFree), stair rule moved to it.
7. Diagnostics: per-tag round and byte counters, round series to a per-node file (teardown), gpu_ms event pairing, mapping count in ECALC_INIT_TL (branch diag); host timeline between exchanges.

Class 4
8. Exchange rounds sized by active pairs (zero peers skipped).
9. Shifts point-to-point instead of a group all-to-all; lazy shift (design).
10. Larger rounds for large all-to-alls, re-evaluated with the recalibrated model.
11. Two exchanges in flight per communicator (design, after rows 8-10).
12. Start-up: per-device allocator processes, seed-thread pool wait, self-test time visible, pool fragmentation.
13. Checkpoints after the tree and after the reciprocal.
Items whose gain only the target can show (user 2026-10-31: build them): two NICs per APU on path 1 (`XSHMEM_LANES=2`, layout D) and a launch-time switch-group check, behind switches, digits-gated on aac7; hierarchical exchange as an OPPORTUNITIES entry.

Class 5
14. Piece-boundary table and memory ceilings in results/LAUNCH_DEFAULTS.md; time model recalibrated on the 2e13 decomposition; write-time model; target facts corrected where they live; PLAN.md target section rewritten as the target agent's test list (setting, A/B with stop rule, adoption rule).

The 2026-10-27/29 work sessions (branch `cand3` = `cand` + the failures session + Q130 + the
overnight session Q131 + the target-copy readiness work; every new behaviour behind a switch, off by default except the
launcher `-c` auto and the all-shift CRT flag `P24_CRT_ANY=1`) were merged into main on 2026-10-31. The frozen target copy is
branch remote-v12 (= cand3 0c2da14; the target session follows `remote/ROUTE.md`). The Done lines below record what the merge brought.
- Done 2026-10-31: 2-node gate of `cand3` = remote-v12 (item e8b inside e31n, aac7 2 n, m; `results/XSHMEM.md` §G2N): build and gates
  at 1 and 2 nodes on both paths PASS; every full run VERIFY OK, the 1.6e10 and 1e11 runs with the expected last digits (1.6e10 on path 1 D / P and path 2,
  the new switches off and on; 1e11 and 1.6e10 under `P24_CHECK` 0 / 1 / 2; 1.3264e11 layout D with `XSHMEM_ROUND_STATS=2`); 0 P24 flags
  in every run; the five fault injections (`P24_CHECK_TEST` 1 / 2 and 2 at `P24_CHECK=0`, `t_vpe seqdiv`, `t_vpe idcoll`) end in their
  designed fatal; `t_vpe` all / hs PASS 8/8; ABBA 2 n D off vs on -0.59 s [-2.58, +1.41] (not resolved); cand3 merged into main.
- Done 2026-10-27 (failures session, `remote/ROUTE.md` steps 4b, 5b, 7): `P24_CHECK` (3e13 CRT flag: input limbs checked as stored and as
  received, CRT flag at any shift; =2 no transform cache; `P24_CHECK_TEST` fault injection), `COMM_SEQ_CHECK`, `XSHMEM_HANG_S`,
  `XSHMEM_HS_BATCH` (2e13 handshake hang), `XSHMEM_SPARSE` (zero-byte peers skipped: the shift cost), `ECALC_INIT_TL` process age and
  init split (576-node init). Evidence (m, aac7 1 node): `tests/t_mn_grid 1 28` 158 P24 products per PE, 1088 operands checked, 0 flags;
  both fault injections fatal as designed; 6.4e10 ABBA all four new switches on vs off: one process per device 80.20 -> 80.10 s
  (-0.1 %, -2.80 .. +2.61 s), one per node 74.33 -> 74.26 s (-0.1 %, -3.07 .. +2.93 s). Audit verdicts (code reading): the P24 bound
  holds for every product kind (`results/PRIMES_STRATEGY.md` §P24 audit); the hang is most likely a straggler past the fixed 900 s
  (`results/XSHMEM.md` COMM2). Target benches dry-run on aac7: 8/8 run.
- Done 2026-10-29 (user): remote-v12 = cand3 0c2da14 frozen: the user's staircase 1.8e13 / 3.6e13 / 5.4e13 + the user's step 4 (no automatic size choice), `DM_MN_WRAP=0 MN_LEVEL_UNIFORM=1 COMM_HANG_DETAIL=1` on every step, failure -> command table; archive check on aac7 PASS (manifest 0/0, s2/s3 both, 1.6e10 VERIFY OK, 12-process mixed shapes switch off/on identical digits).
- Done 2026-10-28 (target-copy readiness, user): three reviews (remote/ kit, ecalc build/runtime, tree hygiene and security) and fixes: copy-and-paste route in 5 pastes (`remote/ROUTE.md`: check+build+gates, screen+4.608e12 at 576 n, first record run 1e13, staircase 2e13 -> 2.8e13 -> 3.82e13 stopping at the first failure with the measured-excess memory rule, report; `remote/run/stair.sh`, `report.sh`, `chain.sh --brief`); the 99 % first-run argument (`results/LAUNCH_DEFAULTS.md` on `cand3`, first-record section RECORD1, Q132); branch name remote-v11 in the kit; build fallbacks for libfabric and the SHMEM library directory; Python 3.6-safe tools; `target/governance/` left out of the copy (Q133). aac7 1 node (m): gate 27 PASS, remote-v11 archive check PASS (manifest 278/0/0, 1.6e10 24.1 s); aac6: remote-v10 unit tests and 1.6e10 VERIFY OK under ROCm 7.14.1 and 10.1.
- Done 2026-10-27/28 (overnight session Q131, 1-node gates on aac7, m): VPE sub-communicator ids for non-power-of-two groups of 64+ PEs (the old rule was fatal: layout P / path 2 at 576 nodes would have died at the 768-PE groups; `results/XSHMEM.md`; host audit 2..2304 PEs, 0 collisions); the default all-shift CRT flag proven with `P24_CHECK_TEST=2` at `P24_CHECK=0` (`P24_CHECK_TEST_S`); route scripts: per-run summary lines, `s5_run.sh --hosts`, `arms.sh` (concurrent arms in one allocation, `remote/ROUTE.md`); record per-APU edge (`results/LAUNCH_DEFAULTS.md` on `cand3`, record-edge section: 3.82e13 has +3.62 GB per APU with today's S6 line, +7.38 GB with `BS_MDEV_LOGL=28 DIST_CHUNKS=16`, time cost settled below); path 2 at 576 n (§P2JUMP: 1e13 fits, 3e13 never); peer-path measurement and design (`results/SINGLE_NODE.md` §PEER); aac6 (re-authorized): unit tests pass under ROCm 7.14.1 and 10.1 on a healthy node; four of its five SPX nodes are degraded (`t_ntt` 0.05-0.11 TB/s against 2.1, 1.6e10 108 s against 24.6 s), so aac6 timing needs a node that passes the `t_ntt` rate check; `BS_MDEV_LOGL=28 DIST_CHUNKS=16` against 29 on 1 node (paired ABBA, LAUNCH_DEFAULTS record-edge section): +0.06 s [-1.16, +1.27] at the record's per-node share 6.632e10 (6 rounds), no resolved cost up to 9.7e10, a step between 9.7e10 and 9.8e10 (no plan switch; division device need against the arena), +5.5 s [+4.75, +6.26] (+4.5 %) at 1e11 (10 rounds); peer-path counters `XSHMEM_ROUND_STATS` (=2 per-exchange series) and `XSHMEM_PEER_STREAMS`: on layout P the exchange wait (4.9-7.7 s per PE at 3.3e10) is the PE's own GPU compute still queued at the forward-transform posts, not communication; no fixed slow GPU; per-slab posts (`DIST_CHUNKS` 4 / 8) give no gain (`results/SINGLE_NODE.md` §PEER). Integration gates and archive checks of remote-v9 / remote-v10 PASS (aac7 1 node).
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
- Root cause of the 10-26 target failures at mixed piece layouts (2e13 handshake hang, 3e13 `P24 CRT flag` on nodes 283-376; both had a mixed level 7). Worked around in remote-v12 by `MN_LEVEL_UNIFORM=1` on every record step. Ruled out at one node (2026-10-29, m): mixed 2-way shapes, partial transform-cache slots k=1..4 (`RNS_PC_TEST_P`, branch shape6), 32-bit size arithmetic. Leads: the block pool nearly full (373 of 373.44 GB per node at 3e13), fabric errors at 576 n, a per-product plane checksum to localise the corruption; the shape6 run 5e9:6:25 with `RNS_PC_TEST_P=2` timed out before its first product (not rerun).

- Path 2 at the target's scale (found 2026-10-25, planner, mod; updated 2026-10-27 in `results/LAUNCH_DEFAULTS.md` §P2JUMP: 1e13 fits by +0.45 GB per APU under the 487 GB node budget, 3e13 never fits): one process per GPU (path 2, and P on path 1) does not fit the target run's per-node share (6.632e10 digits per node) from 8 nodes on: device 447.93 GB at 8 n, 473.70 at 64 n, 517.67 at 576 n even at 3e13 (limit 373.44), node 577.77-648.59 GB (budget 480); layout D fits (360.95 GB at 8/64 n; 358.96 at 576 n with `MN_NONPOW2_CAP=0`). Q120 (user, 2026-10-25): the first target campaign runs 8e9 digits per node, where every path fits at 1/8/64/576 nodes (path 2 at 576 n: device 328.69 GB, node 406.32 GB, mod); making path 2 viable at larger sizes needs a smaller per-process device and host footprint (after the campaign). `ladder.sh` drops a refused path. At 1e13 on 576 n (Q121 step s2) path 2 is 6.79 GB over the device (380.23 GB, mod). Cause and fix options (results/LAUNCH_DEFAULTS.md §P2JUMP, mod): the non-power-of-two group (576 n = 2304 PEs = 9·2^8) makes the v-exchange slots resident, and `DIST_CHUNKS` is 1 under one process per GPU; `DIST_CHUNKS=2` (device 318.56 GB) or 4 (288.22 GB) fits; never run with one process per GPU: the 3 n gate with `ECALC_PE_PER_GPU=1` and an ABBA are target question TQ84 (user 2026-10-25; aac7 has 2 NIC nodes).
- Start-up: plane pools sized to the run [deferred by the user 2026-10-25; estimates (assumed): smaller caps 2^24-2^27 for 1e8-1e9 runs about 6-7 h incl. gate, gain about 2-2.5 s per small run (mod); L-A per-device allocator processes about 3-5 days after 1-2 h of research on cross-process memory sharing under ROCm 7.14, gain up to the measured path 1 vs path 2 init gap at 8e9 per node on 2 n (14.4 vs 6.4 s)] (Q122 (b), measured 2026-10-25): the two plane pools are always mapped at the 2^31 cap (25.8 GB per GPU, a fixed 4.6-5.6 s of init at 1e8-8e9 on 1 node, m); `ECALC_PLANE_CAP=2^30` cuts total 2.64 s [-2.98, -2.30] at 8e9 and 2.05 s at 1e9 with compute unchanged and digits identical (m) and is on the 8e9 campaign's launch line (Q122 (a)); `ECALC_PLANE_CAP=run` built 2026-10-25 (off by default; results/MEMORY.md): picks 2^30 when the division's largest product fits the 2^30 cap's plane (1 node up to 9.6e9 digits, 2 nodes up to about 1.9e10 total), else 2^31; gate c8c/c8d PASS, digits identical (1e9 1 n 8.23 s against 11.55 s unset, 1.6e10 2 n 22.67 s against 26.20 s; single runs, m); conservative at non-power-of-two node counts (3 n at 8e9 per node picks 2^31, mod), and the default since 2026-10-25 (user decision; gate c9a_rundefg PASS, 1 n + 2 n both paths, digits identical); at the 8e9 campaign share it picks 2^30 at 1/8/64/576 n (mod), so the target launch line no longer sets the cap (Q122 (c)). Open: smaller caps (2^24-2^27 at 1e8-1e9) would need a planner change. Also (m): at 1 node 8e9 with the 2^30 cap, start-up switches `DB_POOL_VMM_CHUNK_GB` 1 and 4, `BS_ARENA_ROOM` 0.07 and `BS_SEED_THREADS` 48 gave no total gain (-0.23 s [-0.50, +0.03], +0.36 s [-0.00, +0.69], -0.13 s [-0.41, +0.12], +0.14 s [-0.19, +0.45] with init -0.42 s moved into compute +0.56 s; A/A +0.03 s [-0.20, +0.26]; 12 ABBA blocks each, digits identical, aac7 2026-10-25). Layout D at 3 n 1e11 spends 25-31 s in init (m, results/XSHMEM.md §PPG S2d: region pools / VMM arenas 13.5-15 s, plane pools ~5 s, layered self-test 3.7-4.2 s, staging ~2 s) against ~10 s with one process per GPU, because one process mapping memory for 4 GPUs is serialised in the HIP runtime (D ~20 GB/s per node, P ~150 GB/s). Tried, not adopted (`OPPORTUNITIES.md`: S-A / VMM0, REUSE / REGFIRST / INITOV, STARTSW): `MN_SELFTEST` skip, `DB_POOL_VMM=0`, `RNS_INIT_OVERLAP`. Remaining lever L-A: per-device allocator processes so D maps like P (~20 s, modelled), not started.
One line each (what, why, size); nothing here is started without the user's word.

- aac7 bimodal run times (3 n 1e11, both backends): about half the runs are 5-8 % slower (evidence: results/NODE_SLOWMODE.md §BM; needs the fabric); next: the 3 n ABBA with per-launch NIC counters once three nodes with working NICs exist (aac7 has two nodes with working NICs, 2026-10-25); until then pair every A/B run.
- Cray SHMEM path remaining gap (final +7.5 %, CI +3.4 to +11.5 %, 11 interleaved pairs, results/XSHMEM.md craylane): init +8 s (libsma heap NIC registration 6.2 s, hipHostRegister 1.5 s, barrier 1.4 s; aac7 4 KiB shared heap, needs THP shmem or hugetlb, admin) and a ~4.5 s first-exchange post (stream sync on every PE; not GPU page-table first touch); re-measure both on the target (Cray SHMEM 12.1) before any work here. Cray default layout D is 2.4x slower than P (four APU threads share one libsma heap at thread level 3); the target layout is P.
- Same-node peer path follow-ups (`XSHMEM_PEER`, default on under P since Q100, results/SINGLE_NODE.md §PEER): a 3 n run for the gain and the per-PE heap peak (needs a third NIC node); signals and control words by direct writes and copies spread over several SDMA engines (dm still +3.5 s behind D at 1 n 3.3e10, m); the Cray path through `shmem_ptr` at the target session.
- GPU work before the exchanges on layout P (found 2026-10-28, m, `results/SINGLE_NODE.md` §PEER): kernel-launch fusion of operand gather, NTT passes and pack (bound 1.9-2.8 s of 34 s at 3.3e10 on P, 1.2-1.5 % on layout D at 1 node; upper bounds); `k_gather_mn` 2.3 s per PE before the forward transforms as its own item; whether the record's multi-node layout D has busy posts: the e8b series arm ran on 2026-10-31 (2 n 1.3264e11 layout D, `XSHMEM_ROUND_STATS=2`, VERIFY OK, 128.2 s, m); its per-exchange series lines are kept on aac7 and not yet analysed.
- Transient fabric error watch (2026-10-28, m): `t_vpe hs` with `XSHMEM_COLL=tree` at 4 PEs on s5b0 once ended with a cxi `PCIE_UNSUCCESS_CMPL` completion error (gate e10m), 6 of 6 reruns pass, NICs healthy before and after; note any repeat.
- 6 n paired ABBA of grid [2,6] against [6], at least 8 runs per arm (earlier data +23 +/- 12 s, unexplained, m); aac7, one item.

## Decisions (user) still acting

Only decisions that no central doc states: the others are in `README.md` (two paths, Q85), `RESULTS.md` (defaults in force: Q58, Q73, Q96, Q99, Q100, Q103) and `OPPORTUNITIES.md` (not adopted: Q101, Q102).

- Q97 (user 2026-10-24): Q79/Q88 code stays until the target session (Q103 then removed the plain SHMEM transport).
- Q103-Q111 (user 2026-10-25): all (a), executed in cleanup round 4 (row above).

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are in `OPPORTUNITIES.md`.

## Target session

No target session of ours follows (user 2026-10-31): the target agent's test list goes into PLAN.md (row 14); the lines below are history.

Target work: `target/RESTART.md` and `target/bench/QUESTIONS.md`.
- Self-test cost (user 2026-10-26: keep the start-up self-tests if they really take < 15 s): read the self-test seconds
  (`ECALC_INIT_TL=1`) at 2 nodes and 576 nodes with the fixed seed default (576 n layered test 3.1-3.5 s; aac7 3 n 3.7-4.2 s, m).
