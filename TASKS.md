# TASKS — live task list (open work, decisions, target session)

Scope: open work, user decisions still acting, proposed items and target-session items; closed items are in the
archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.

Rewritten 2026-10-19, cleaned 2026-10-23 and 2026-10-25 (finished rows and settled decisions removed: history is in git, `RESULTS.md` and `results/`). Closed rows and older material are in
`archive:TASKS_HISTORY_2026-10-19.md`, `archive:TASKS_HISTORY_2026-10-23.md` (older: `archive:TASKS_HISTORY_2026-10-12.md`). Measurements are in `RESULTS.md` and
`results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work, in priority order (user 2026-10-12)

Classes: 1 bugs, 2 shrink, 3 simplify, 4 performance, 5 docs (definitions in the project instructions). Within a class,
rows are in the order to do them.

- `ecalc/mn_model.py estimate --D 3.71e13 --g 576` crashes in `mem_model.p24_grid_shape` (no grid found, `best` is None; class 1, found 2026-10-25; same on main 889f86a, so older than the cleanup).
- Target memory fit (class 1, found 2026-10-25): the planner now refuses 3.82e13 on 576 (device 407.41 GB per node vs 373.44, -33.97 GB; m, planner) where it reported +14.48 GB on 2026-10-19; same on main 0d052a9 and after cleanup round 4, so a change between 10-19 and 10-25 (or a different configuration then) moved it; find which, then fix the plan or the docs.
- Cleanup round 4 (user order 2026-10-25, Q103-Q111 all (a)): done, merged 2026-10-25 (gate b4i_cl4g3 (208ae18: tools, path 1 1 n + 2 n, unit 18/18, path 2 1 n PASS) + b4j_cl4p2 (d64e00e: path 2 1 n + 2 n PASS), digits identical). 185 -> 162 files, 46508 -> 39817 lines, switches 194 -> 145 (m); path 1 no slower (-0.91 s [-3.20, +1.39], m); `results/CODE_REDUCTION.md §CR4`. Left for later: `vpe_on` collapse, Garner `dmod128/192` dedupe, comment shortening, `t_pe_geo_live` merge; `target/kit/target_kit.sh` stage `a3` (one vs two NICs per APU) is disabled: it ran the removed `t_comm --bw`; a `tests/t_xshmem bw` replacement is open (kit on hold); `ecalc/mem_model.py` / `mn_model.py` still model the pools under the old names (`COMM_OFI`, `COMM_SHMEM_ROUND_MB`, a dead `transport='shmem'` branch; outputs unchanged); `binsplit.c` still prints `comm_shmem pool: COMM_OFI:` / `comm_ofi pools` in its pool lines (text only).

## Proposed (needs the user's priority)

One line each (what, why, size); nothing here is started without the user's word.

- Shared-operand transform reuse (apumult A16) and seed waits for the region pools (A1): measured 2026-10-25, no gain, not merged (Q101, Q102; `OPPORTUNITIES.md`, `results/APUMULT.md §AP34-reuse`, `§INIT`); left as ideas: reuse across a grid's pieces, the multi-node tree levels, division steps 1 and 2; only with a new payoff estimate.

- aac7 bimodal run times (3 n 1e11, both backends): about half the runs are 5-8 % slower in every compute phase (measured 2026-10-23, 9zs19-9zs29); not time-correlated (Fisher p = 1.0) and absent at 1 node without network (271 runs, unimodal, 9zv3_bm2d, results/NODE_SLOWMODE.md §BM), so it needs the fabric or more than one node; next: the 3 n ABBA with per-launch NIC counters once three nodes with working NICs exist (aac7 has two on 2026-10-24); until then pair every A/B run.
- Cray SHMEM path remaining gap (final +7.5 %, CI +3.4 to +11.5 %, 11 interleaved pairs, results/XSHMEM.md craylane): init +8 s (libsma heap NIC registration 6.2 s, hipHostRegister 1.5 s, barrier 1.4 s; aac7 4 KiB shared heap, needs THP shmem or hugetlb, admin) and a ~4.5 s first-exchange post (stream sync on every PE; not GPU page-table first touch); re-measure both on the target (Cray SHMEM 12.1) before any work here. Cray default layout D is 2.4x slower than P (four APU threads share one libsma heap at thread level 3); the target layout is P.
- Same-node peer path follow-ups (`XSHMEM_PEER`, default on under P since Q100, results/SINGLE_NODE.md §PEER): a 3 n run for the gain and the per-PE heap peak (needs a third NIC node); signals and control words by direct writes and copies spread over several SDMA engines (dm still +3.5 s behind D at 1 n 3.3e10, m); the Cray path through `shmem_ptr` at the target session.
- XSHMEM target-only day-one checks (aac7 part done, results/XSHMEM.md XDIAG): T1, T2, T5 and T8 on the device heap, the T6 NIC-APU map, `XSHMEM_OFI_SIG=waw` (unbuilt) if the target needs it; at the target session.
- Default start-up time (measured 2026-10-22, results/XSHMEM.md §PPG S2d): layout D spends 25-31 s in init at 3 n 1e11 (region pools / VMM arenas 13.5-15 s, plane pools ~5 s, layered self-test 3.7-4.2 s, staging ~2 s) against ~10 s with one process per GPU: one process mapping memory for 4 GPUs is serialised in the HIP runtime (D ~20 GB/s per node vs P ~150 GB/s). Tried and not adopted: skip the layered self-test (`MN_SELFTEST`, since removed, 3 n 1e11 -1.55 s [-4.25, +1.14], the cost is building the meshes), `DB_POOL_VMM=0` (init -6.0 s but bs +4.8 s), plane pools mapped in a helper thread (`RNS_INIT_OVERLAP`, no gain: the runtime serialises the mappings; results/APUMULT.md §INIT). Remaining lever: per-device allocator processes so D maps like P (L-A, ~20 s, modelled), not started.
- 6 n paired ABBA of grid [2,6] against [6], at least 8 runs per arm (earlier data +23 +/- 12 s, unexplained, m); aac7, one item.
- Watch item, start-up hang at 7.7e10 (A114 E1): not reproduced on current main, 0 in 301 runs (95 % bound 1.0 %, 2026-10-25, results/CRASHES_HANGS.md §A114); any hang caught by `ECALC_INIT_TL=1` plus `eu-stack` reopens it.

## Decisions (user) still acting

Only decisions that no central doc states: the others are in `README.md` (two paths, Q85), `RESULTS.md` (defaults in force: Q58, Q73, Q96, Q99, Q100, Q103) and `OPPORTUNITIES.md` (not adopted: Q101, Q102).

- Q97 (user 2026-10-24): Q79/Q88 code stays until the target session (Q103 then removed the plain SHMEM transport).
- Q103-Q111 (user 2026-10-25): all (a), executed in cleanup round 4 (row above).

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are in `OPPORTUNITIES.md`.

## Target session (later)

| # | item | status | where |
|---|---|---|---|
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13; X2 is now built in, no switch), kit `nodechk` rehearsal (T14) | open | internal target task list |
| 26 | **Kit paths not rehearsed** (A06): 2-NIC a3, 8-node a4, s2chk, `--spares` STOP, plan REFUSED | open (target session) | results/TOOLS_PROFILES.md §A06 |
| 28 | **Target bring-up:** populate `profiles/target.conf` as soon as access exists; runbook and probe script written 2026-10-14 (profiles/README.md "Target bring-up runbook", `target/probe/probe.sh`), login dry run OK | needs the target for the rest | results/TOOLS_PROFILES.md §C01 |
| 29 | **Hugetlb on the target:** the target has hugetlb pages and cray-shmem 12.1.0 (untested): test the libsma symmetric heap on hugetlb with `XSHMEM=cray` (aac7: init +6.2 s of NIC registration on a 4 KiB heap) and re-run the stall checks (results/SHMEM.md); aac7 has no free hugetlb pages | target session | results/SHMEM.md, results/XSHMEM.md craylane |
| 31 | **Gap at 5 or more peers and at scale** on the target fabric with cray-shmem 12.1: run the day-one checks T1-T8 (results/XSHMEM.md §8) and path 2 against path 1 at 4, 8 and 16 nodes | target session | results/XSHMEM.md |
