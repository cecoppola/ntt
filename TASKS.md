# TASKS — live task list (open work, decisions, target session)

Scope: open work, user decisions awaiting implementation, saved ideas and target-session items; closed items are in the
archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.

Shrunk 2026-10-12 (user plan rev 2). Everything closed or done, and the dated status sections, are in
`archive:TASKS_HISTORY_2026-10-12.md` (local archive) (row numbers there keep their old meaning). Measurements are in `RESULTS.md`
and `results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work, in priority order (user 2026-10-12)

The class column is the user's priority class (1 bugs, 2 shrink, 3 simplify, 4 performance, 5 docs; defined in the
project instructions). Within a class, rows are in the order to do them.

| class | # | item | status | where |
|---|---|---|---|---|
| 1 | NIC | **Node screen checks the NIC count each node's runtime sees** (3 SHMEM runs aborted 2026-10-13: 1 NIC visible, 4 requested) | done 2026-10-13 (7de81c7): `--want-nics`, verified on 4 nodes; not yet seen on a real 1-NIC node | results/NODE_SLOWMODE.md §NIC |
| 4 | P | **Performance on aac7:** distributed division multiply (+94 s at 6 nodes vs 1, m); `k_gather` and init mapping evaluated, not pursued | adopted 2026-10-12 (D20-D22): `BS_BATCH_PIPE=1` (N3, −0.99 s at 1 n, m), `RNS_CRT_SCAN=1` (K4b, −2.52 s at 1 n, m) and `DM_MN_WRAP=1` (P1, D22; +5.4 GB per APU, mod) are code defaults, `=0` fallbacks; nothing running | results/EXCHANGE.md §P2, P3.md, P1.md, DEF.md, N3.md, K4b.md |
| 4 | N7E | **Paired repeat of the N7D proxy switches at 6 n** (`COMM_SHMEM_PX_SEND=2`, `COMM_SHMEM_PX_INIT=1`) against comm_ofi | done 2026-10-14 (commit 2f34f01 run, 83/83 ok): B − A +129.3 s [+92.9, +165.7], 1.68 × (28 pairs, m); the N7D 1.43 × gap is not reproduced; PX_SEND 2 vs 1 and PX_INIT 1 vs 0 n.s.; both stay off, no default change | results/SHMEM_ITEMS.md §N7E |
| 4 | N7b | **One SHMEM PE per APU (D29 (a)); target: 2 NICs per APU → PEs per APU parameter 1 or 2** (one PE per node uses one of four NICs; Fix A `COMM_SHMEM_PEER_ORDER=shift` is measured, off by default) | B3 done 2026-10-14: digits identical at 1/2/6 n; 6-n full run 656.7 s vs comm_ofi 224.0 s (+432.7 s, CI [+174.3, +691.1], 4 pairs, m): mode stays off; N7C 2026-10-14: diagnosed (pool on a random NUMA node, SDMA copies); `COMM_SHMEM_PX_NUMA=1` + `HSA_ENABLE_SDMA=0` merged off by default, digits identical, 6-n +242.4 s vs comm_ofi (CI [+127.1, +357.7], m), still 2.1 x slower; N7D 2026-10-14: `COMM_SHMEM_PX_KCOPY`, `COMM_SHMEM_PX_INIT`, `COMM_SHMEM_PX_SEND` merged off by default, gates 1/2/6 n PASS, 6-n +83.2 s vs comm_ofi (CI [+37.7, +128.7], 3 pairs, m), 1.43 x | results/SHMEM_ITEMS.md §N7B, results/SHMEM_ITEMS.md §N7C, results/SHMEM_ITEMS.md §N7D |
| 4 | N7G | **THP and PIN on the proxy mode** (`COMM_SHMEM_PX_THP`, `COMM_SHMEM_PX_PIN`, default 0): 3 pairs at 6 n, B − A −124.9 s [−460.6, +210.8], not significant (m); both stay off (PX_PIN removed 2026-10-18) | done 2026-10-15 (results/SHMEM_ITEMS.md §N7G); next: 4 more pairs, THP only against the default, not run (needs the user's word) |

Closed 2026-10-13: 23, RECIP, SHM, S, SH3, SIMP, 27, SH4, D, FIT (pre-launch memory fit, automatic DM_MN_WRAP; results/LAUNCH_DEFAULTS.md §FIT), STAT (static-analysis fixes; results/NIGHT_RUNS.md §STAT) — details in archive:TASKS_HISTORY_2026-10-12.md.

SHMEM 2026-10-17 (summary in results/SHMEM.md): libfabric 184.5 s vs SHMEM default 286.0 s mean over 24 runs, best 4 220.5 s (m); huge-page default kept (D36), heap switch dropped (D39), fixes A/C no gain, COMM_SHMEM_LAYV_ANY and COMM_PHASE_TS merged (c3a469d, default 0, gates PASS item 55). Items 56-67 recorded in results/SHMEM.md (LAYV_ANY no effect; no stalls or gap at 1 and 2 n; scaling model); items 70-89 recorded too (stall reproduced as host-memory fragmentation, memory-preparation fix partial; proxy path loses about 20 % at 4 n, SS candidate, workers not adopted); open: fix #2 for the stalls, SS confirmation (items 72j, 72k).


Closed 2026-10-18: SHMEM stall work. Cause: kernel proactive compaction plus the 4 KiB pinned node-process SHMEM heap, which evicted the GPU queues (KFD). Fix: `COMM_SHMEM_NP_HEAP_THP` default 1 (D48); 0 stalls in 12 runs at 3 n vs 7 in 18 (m, results/SHMEM.md). `COMM_SHMEM_PX_PIN` removed (no significant gain). Remaining items are under Proposed.

Closed 2026-10-12 and earlier (waves 0+1, user decisions, plan rev 2): details in `archive:TASKS_HISTORY_2026-10-12.md`.

## Proposed (needs the user's priority)

- N7G next: done 2026-10-16 (item 41, 4 more pairs: B − A +163.5 s, CI [−45, +372]; results/SHMEM_ITEMS.md §N7G, D36 default kept). Closed.
- N7F: find the source of the 111 s run-to-run spread of the proxy arm at 6 n (N7E, sd 111.1 s vs 15.1 s comm_ofi, m) and more pairs for PX_SEND=2 / PX_INIT=1 (CI half-widths 288 s and 157 s, m); aac7 runs, needs the user's word.

New work found by agents or the coordinator goes here first, one line each (what, why, size), until the user schedules,
merges or drops it. Nothing is started from this section without the user's word.

- SHMEM 6-node confirmation: full-size run (3.8646e11 digits) of SHMEM with `COMM_SHMEM_NP_HEAP_THP=1` against libfabric 184.5 s (m, results/SHMEM.md); 6 nodes, paired runs.
- Hugetlb backing switch for the proxy and node-process heaps (the target has hugetlb): removes the 4 KiB pinned pages the compaction works on; new switch, off by default; size medium.
- The remaining 3-6 % SHMEM gap at 3 n: comm-span excess and the init heap collapse (2.9 s per helper) (m, results/SHMEM.md); measurement then fix.
- Residual GPU-queue evictions from successful compaction on NUMA node 0 of the job's first node (1.4-12 s summed per run, m, results/SHMEM.md); likely solved by the hugetlb switch above.

- Node drain after our jobs: the 6 nodes of a job went to `needs_reboot` ("reboot timed out") right after three of our jobs ended: 2026-10-13 21:07 ET and 2026-10-14 02:05 ET (both runs killed at their timeout) and 2026-10-14 09:06 ET (a job that ended normally); other normal ends had no drain; cause unknown, site-side; investigate (results/SHMEM_ITEMS.md §N7 §6, results/NIGHT_RUNS.md §NIGHT2). Size unknown.
- Gather layout: each rank holds its own rows so the 3/4 remote pull in `k_gather` disappears; `k_gather_raw` is 2.0 s/APU at 1 node (m), floor is the remote read (results/EXCHANGE.md §P2). Algorithm change, size unknown — needs a scoping pass.

## Decisions (user)

- D35 (answered NO, user 2026-10-15): `COMM_SHMEM_PX_THP` and `COMM_SHMEM_PX_PIN` stay default 0 (3 pairs not significant, results/SHMEM_ITEMS.md §N7G). Closed.
- D36 (answered (a), user 2026-10-15): `COMM_SHMEM_PX_THP=1` is the default of the COMM_OFI=0 proxy mode (4 pairs at 3.8646e11, B − A −287.8 s, CI [−507, −68]); `COMM_SHMEM_PX_THP=0` restores 4 KiB pages; PX_PIN stays 0. Applied on branch `thp`, gate item 38_thp_gate. Closed.

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are kept in `OPPORTUNITIES.md` (old item 15 `DIST_COALESCE` is there as S52).

## Target session (later)

| # | item | status | where |
|---|---|---|---|
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13), kit `nodechk` rehearsal (T14) | open | internal target task list |
| 26 | **Kit paths not rehearsed** (A06): 2-NIC a3, 8-node a4, s2chk, `--spares` STOP, plan REFUSED | open (target session) | results/TOOLS_PROFILES.md §A06 |
| 28 | **P2 target bring-up:** populate `profiles/target.conf` as soon as access exists. Before access: a written runbook listing every `__FILL__` key, how it is determined, the expected range and the order (modules, build, NIC/fabric, libfabric monitor, memory edge, node screen, plan check, first small run); a script `tools/target_probe.sh` that collects the facts (modules, ROCm, APU count and memory, MemAvailable, NICs, libfabric providers/monitors, uffd, Slurm options, SHMEM library), prints a proposed target.conf and diffs it with the template; dry-run it on aac7 and aac6 | probe + runbook written 2026-10-14 (profiles/README.md "Target bring-up runbook"); login dry run OK, compute-node dry run queued (aac7); needs the target for the rest | results/TOOLS_PROFILES.md §C01 |
