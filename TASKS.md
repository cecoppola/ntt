# TASKS — live task list (open work, decisions, target session)

Scope: open work, user decisions still acting, proposed items and target-session items; closed items are in the
archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.

Rewritten 2026-10-19, cleaned 2026-10-23 (closed rows, the proxy-mode decisions and the SHMEM-era target rows removed). Everything closed, and the older rows (NIC, P, N7E, N7b, N7G, the dated status paragraphs), are in
`archive:TASKS_HISTORY_2026-10-19.md` (also `archive:TASKS_HISTORY_2026-10-23.md`; older: `archive:TASKS_HISTORY_2026-10-12.md`). Measurements are in `RESULTS.md` and
`results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work, in priority order (user 2026-10-12)

Classes: 1 bugs, 2 shrink, 3 simplify, 4 performance, 5 docs (definitions in the project instructions). Within a class,
rows are in the order to do them.

- Wrap-up night (user order 2026-10-25 01:40 ET) finished 03:50, final gate a0f1 PASS (1 n + 2 n both paths, unit 9/9): gate unit-set flakes fixed (toolkit + mnaccept rc 141 rerun); mn_model DKM_HI-off start at the measured 0.369 (HI gain 7.2 s at 1e11, modelled); BS_ARENA_ROOM 0.14 (Q99); `XSHMEM_PEER=1` merged off (P at 1 n 3.3e10 51.6 -> 34.9 s, results/SINGLE_NODE.md §PEER, m; per-PE DATA heap peak 256 of 768 MiB at 1 n and 2 n); `RNS_PAIR_REUSE` (no measurable gain), `RNS_REGIONS_FIRST` (2 x slower at 1 n 1e11) and `RNS_INIT_OVERLAP` (no gain) measured, code on branches reuse / init (results/APUMULT.md §AP34-reuse, §INIT); 7.7e10 hang watch 0 in 301 runs.
- Short tasks (user order 2026-10-24 23:20 ET) merged 2026-10-25, each gated on aac7 (1 n + 2 n digits identical on both paths, unit 9/9): memfix (BS_ARENA_TRIM_GB input cap + banner flush; aac7 profile `ECALC_NODE_GB=420`; the one-node P pool rule for any transport), simpl (`bs_Pd`/`bs_Qd` plain globals, `vpe_adopt(void)`, mn_model DKM_HI on by default, dead prototype), dbgt (P at 1 n phase profile, results/SINGLE_NODE.md §P1N; `MNRUN_NICGRP=1` probe). `BS_ARENA_ROOM` default 0.14 merged 2026-10-25 (Q99); follow-ups in Proposed.
- Cleanup round 3 (user order 2026-10-24 22:10 ET): code, docs and final pass merged 2026-10-24 (gate 9zz8 PASS 1 n + 2 n both paths, unit 9/9; path 1 speed unchanged, results/CODE_REDUCTION.md §CR3S); tracked code 156 -> 143 files, 41.6k -> 39.5k lines; root docs + results/ 8.2k -> 5.2k lines (m); workspace and own aac7 directories decluttered (aac7 home 134 -> 96 GB, m). Q97 (a): Q79/Q88 code stays until the target session; Q98 (b): the user's files moved to a reference folder.

## Proposed (needs the user's priority)

One line each (what, why, size); nothing here is started without the user's word.

- Shared-operand transform reuse across a pair's two products (apumult A16, results/APUMULT.md §AP34-reuse): `RNS_PAIR_REUSE=1` (off; branch `reuse` babb434, gate PASS) keeps Q_B's forward planes for the pair's second one-node B-form product: digits identical, 1 n 7.7e10 bs -0.54 s [-0.87, -0.20], total -0.33 s [-4.04, 3.39] (m); open: grid pieces, vPE tier (path 2), tree levels (`mn.c tree_level`, `tree_level_k`), division steps 1 and 2 (share Q); M, behind a switch, both paths (Q85).
- Seed waits for the region pools in layout D (apumult A1; results/APUMULT.md §AP34, §INIT): tried 2026-10-25 on branch init (1 n 1e11, 8 runs per arm, m): region pools before the plane pools (`RNS_REGIONS_FIRST=1`) ends the seed 8 s earlier (48.4 -> 40.4 s) but starves the plane pools of 2 MiB blocks, bs 59 -> 125 s, total 140 -> 282 s: rejected; extra pinned seed buffers (`BS_SEED_EARLY`, 24 GB: total +5.7 s; 40 GB: memory-guard kills) removed again. The seed now waits on the 13.5-15 s mapping of the pools itself (see Default start-up time, L-A); payoff at the target stays ~0-1 s (mod).

- aac7 bimodal run times (3 n 1e11, both backends): about half the runs are 5-8 % slower in every compute phase (measured 2026-10-23, 9zs19-9zs29); not time-correlated (Fisher p = 1.0) and absent at 1 node without network (271 runs, unimodal, 9zv3_bm2d, results/NODE_SLOWMODE.md §BM), so it needs the fabric or more than one node; next: the 3 n ABBA with per-launch NIC counters once three nodes with working NICs exist (aac7 has two on 2026-10-24); until then pair every A/B run.
- Cray SHMEM path remaining gap (final +7.5 %, CI +3.4 to +11.5 %, 11 interleaved pairs, results/XSHMEM.md craylane): init +8 s (libsma heap NIC registration 6.2 s, hipHostRegister 1.5 s, barrier 1.4 s; aac7 4 KiB shared heap, needs THP shmem or hugetlb, admin) and a ~4.5 s first-exchange post (stream sync on every PE; not GPU page-table first touch); re-measure both on the target (Cray SHMEM 12.1) before any work here. Cray default layout D is 2.4x slower than P (four APU threads share one libsma heap at thread level 3); the target layout is P.
- Intra-node peer path for the xshmem engine: BUILT on branch `peer` (78015d4) as `XSHMEM_PEER=1` (off by default; same-node PEs exchange by hipIpc device copy; gates PASS, digits identical, 1 n + 2 n + unit + path 2): P at 1 n 3.3e10 51.6 -> 34.9 s (D 39.7, m), 2 n 2e10 26.6 -> 19.4 s, 2 n 6e10 69.7 -> 49.0 s; per-PE DATA heap peak 256 of 768 MiB at every size and node count (m); adoption as P default is the user's call, merge pending (results/SINGLE_NODE.md §PEER).
- XSHMEM target-only day-one checks (aac7 part done, results/XSHMEM.md XDIAG): T1, T2, T5 and T8 on the device heap, the T6 NIC-APU map, `XSHMEM_OFI_SIG=waw` (unbuilt) if the target needs it; at the target session.
- Default start-up time (measured 2026-10-22, results/XSHMEM.md §PPG S2d; logs of 9x5/9x0/9x4): the node-process default spends 25-31 s in init at 3 n 1e11 (region pools / VMM arenas 13.5-15 s, plane pools ~5 s, layered self-test 3.7-4.2 s, staging ~2 s) against ~10 s with one process per GPU: one process mapping memory for 4 GPUs is serialised in the HIP runtime (D ~20 GB/s per node vs P ~150 GB/s). Candidates (savings modelled): (S-A) switch to skip the layered self-test: built (branch selftest a813d8d, archive:branches_2026-10-24/, `MN_SELFTEST` 0/2) and measured 2026-10-22 (9t0 gate PASS; 9t1 3 n 1e11: `0` -1.55 s [-4.25, +1.14] over 4 pairs, init -3.2 s but bs +1.5 s; `2` no gain: the cost is building the communicator/meshes, not the test) -> not merged; (S-B) `DB_POOL_VMM=0`: measured 2026-10-22 (9z3_vmm3c, 3 n 1e11, 4 ABBA pairs) init -6.0 s but bs +4.8 s, total -1.41 s [-4.31, +1.49]: no clear gain, not adopted; (M-A) overlap plane pools / arenas with transport, staging and self-test, 4-8 s; (L-A) per-device allocator processes so D maps like P, ~20 s. Earlier (2026-10-20, 2 n warm SHMEM, init 23.65 s): overlapping `mn_init` with the plane-pool allocation was tried and dropped (starves the heap collapse of 2 MiB blocks); skipping the two self-tests gains no time (their window hides the arena's background mapping, results/DIVISION.md §DB_POOL_VMM); the remaining lever is the mapping itself (the four APUs map one at a time, about 20 s per run; the P layout cuts init about threefold). (M-A tried 2026-10-25, results/APUMULT.md §INIT: `RNS_INIT_OVERLAP=1`, the plane pools mapped in a helper thread beside the region arenas and the seed thread, 1 n 1e11, 8 runs per arm: init 35.5 -> 34.7 s, total 140.4 -> 140.1 s (m), i.e. no gain, the runtime serialises the mappings; 2 n 2e10 path 1 init -0.0/-1.5 s, path 2 none; switch kept, off. L-A (a process per device allocator, ~20 s) is the remaining lever: follow-up, not started.)
- 6 n paired ABBA of grid [2,6] against [6], at least 8 runs per arm (earlier data +23 +/- 12 s, unexplained, m); aac7, one item.
- Watch item, start-up hang at 7.7e10 (A114 E1): not reproduced on current main, 0 in 301 runs (95 % bound 1.0 %, 2026-10-25, results/CRASHES_HANGS.md §A114); any hang caught by `ECALC_INIT_TL=1` plus `eu-stack` reopens it.
- Switch audit ASK list (1 switch left; COMM_KTIME, COMM_LAYER_STATS, COMM_XGMI_STATS and NP_MN3 were removed 2026-10-19, D56; MN_OUT_WAVES, ECALC_FAST_EXIT, DM_TAIL_DEAD, ECALC_NP_AUTO_MIN and `COMM_SHMEM_PE_PER_APU=2` 2026-10-20, Q53): `COMM_XSTATS` after the parser migration; kept until the target session (D56, user 2026-10-19).

## Decisions (user) still acting

- Q99 (user 2026-10-25): `BS_ARENA_ROOM` default 0.14 (was 0.16): -8.6 GB device memory per node at the 2 n bs-phase peak, 0 remaps, time neutral (a0a2, 4 ABBA pairs, m); `BS_ARENA_ROOM=0.16` restores.

- D48 (user 2026-10-18): `COMM_SHMEM_NP_HEAP_THP=1` is the default (main 23c2c1f); `=0` restores 4 KiB pages.
- Q58 (user 2026-10-20): `MN_NONPOW2_CAP` default auto = on only from `MN_NONPOW2_MIN` (4.5e10) digits per node-process, off below (3 n 6e10 was +8.9 s with it on; a group whose size is not a power of two takes the next power-of-two plane: 6 n 3.70e11 -18.3 s with libfabric, +41.3 GB per node of plane memory; `=1` forces on, `=0` restores the power-of-two plane; results/DIVISION.md).
- Q73 (user 2026-10-21): `COMM_TRANSPORT=xshmem` is the default multi-node transport (`XSHMEM_ROUND_MB=128`, `XSHMEM_ORDER=rot`; default build `XSHMEM=ofi` where libfabric and PMI2 are found); `COMM_TRANSPORT=shmem` is the fallback and reference (results/XSHMEM.md §10).
- Q85 (user 2026-10-23): keep the fastest libfabric path (path 1) and the fastest Cray SHMEM path (path 2) both open; a change to shared code must leave path 1 digit-identical and no slower (README.md two paths, TESTING.md path 2 gate).
- Q78 (user 2026-10-22): the proxy mode (`COMM_SHMEM_PE_PER_APU`, `COMM_SHMEM_PX_*`) was removed (history in results/SHMEM.md); kept: `COMM_SHMEM_NP_HEAP_THP`, `COMM_SHMEM_STAGE_SLOT_MB` (Q79, until the Cray SHMEM 12.1 port is tested).
- Q60 (user 2026-10-20): aac7 profile `COMM_SHMEM_ROUND_MB=128` (was 1024; host staging 10.2 -> 3.1 GB per node at 2 n, no time lost at 2-6 n; lets `MN_NONPOW2_CAP` fit 3.8646e11 at 6 n for SHMEM; results/SHMEM.md).

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are in `OPPORTUNITIES.md`.

## Target session (later)

| # | item | status | where |
|---|---|---|---|
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13), kit `nodechk` rehearsal (T14) | open | internal target task list |
| 26 | **Kit paths not rehearsed** (A06): 2-NIC a3, 8-node a4, s2chk, `--spares` STOP, plan REFUSED | open (target session) | results/TOOLS_PROFILES.md §A06 |
| 28 | **Target bring-up:** populate `profiles/target.conf` as soon as access exists; runbook and probe script written 2026-10-14 (profiles/README.md "Target bring-up runbook", `tools/target_probe.sh`), login dry run OK | needs the target for the rest | results/TOOLS_PROFILES.md §C01 |
| 29 | **Hugetlb on the target:** the target has hugetlb pages and cray-shmem 12.1.0 (untested): test the libsma symmetric heap on hugetlb with `XSHMEM=cray` (aac7: init +6.2 s of NIC registration on a 4 KiB heap) and re-run the stall checks (results/SHMEM.md); aac7 has no free hugetlb pages | target session | results/SHMEM.md, results/XSHMEM.md craylane |
| 31 | **Gap at 5 or more peers and at scale** on the target fabric with cray-shmem 12.1: run the day-one checks T1-T8 (results/XSHMEM.md §8) and path 2 against path 1 at 4, 8 and 16 nodes | target session | results/XSHMEM.md |
