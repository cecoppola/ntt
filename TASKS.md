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
| 1 | NIC | **Node screen checks the NIC count each node's runtime sees** (3 SHMEM runs aborted 2026-10-13: 1 NIC visible, 4 requested) | done 2026-10-13 (7de81c7): `--want-nics`, verified on 4 nodes; not yet seen on a real 1-NIC node | results/NIC.md |
| 4 | P | **Performance on aac7:** distributed division multiply (+94 s at 6 nodes vs 1, m); `k_gather` and init mapping evaluated, not pursued | adopted 2026-10-12 (D20-D22): `BS_BATCH_PIPE=1` (N3, −0.99 s at 1 n, m), `RNS_CRT_SCAN=1` (K4b, −2.52 s at 1 n, m) and `DM_MN_WRAP=1` (P1, D22; +5.4 GB per APU, mod) are code defaults, `=0` fallbacks; nothing running | results/P2.md, P3.md, P1.md, DEF.md, N3.md, K4b.md |
| 4 | N7b | **One SHMEM PE per APU (D29 (a)); target: 2 NICs per APU → PEs per APU parameter 1 or 2** (one PE per node uses one of four NICs; Fix A `COMM_SHMEM_PEER_ORDER=shift` is measured, off by default) | B3 done 2026-10-14: digits identical at 1/2/6 n; 6-n full run 656.7 s vs comm_ofi 224.0 s (+432.7 s, CI [+174.3, +691.1], 4 pairs, m): mode stays off; exchange rate in ecalc undiagnosed | results/N7B.md |

Closed 2026-10-13: 23, RECIP, SHM, S, SH3, SIMP, 27, SH4, D, FIT (pre-launch memory fit, automatic DM_MN_WRAP; results/FIT.md), STAT (static-analysis fixes; results/STAT.md) — details in archive:TASKS_HISTORY_2026-10-12.md.

Closed 2026-10-12 and earlier (waves 0+1, user decisions, plan rev 2): details in `archive:TASKS_HISTORY_2026-10-12.md`.

## Proposed (needs the user's priority)

New work found by agents or the coordinator goes here first, one line each (what, why, size), until the user schedules,
merges or drops it. Nothing is started from this section without the user's word.

- Node drain after killed runs: nodes went to `needs_reboot` straight after our SHMEM runs were killed at their timeout (2026-10-13 21:07 ET and 2026-10-14 02:05 ET, twice); cause unknown; investigate (results/N7.md §6). Size unknown.
- Gather layout: each rank holds its own rows so the 3/4 remote pull in `k_gather` disappears; `k_gather_raw` is 2.0 s/APU at 1 node (m), floor is the remote read (results/P2.md). Algorithm change, size unknown — needs a scoping pass.

## Decisions (user)

(none open)

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are kept in `OPPORTUNITIES.md` (old item 15 `DIST_COALESCE` is there as S52).

## Target session (later)

| # | item | status | where |
|---|---|---|---|
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13), kit `nodechk` rehearsal (T14) | open | internal target task list |
| 26 | **Kit paths not rehearsed** (A06): 2-NIC a3, 8-node a4, s2chk, `--spares` STOP, plan REFUSED | open (target session) | results/A06.md |
| 28 | **P2 target bring-up:** populate `profiles/target.conf` as soon as access exists. Before access: a written runbook listing every `__FILL__` key, how it is determined, the expected range and the order (modules, build, NIC/fabric, libfabric monitor, memory edge, node screen, plan check, first small run); a script `tools/target_probe.sh` that collects the facts (modules, ROCm, APU count and memory, MemAvailable, NICs, libfabric providers/monitors, uffd, Slurm options, SHMEM library), prints a proposed target.conf and diffs it with the template; dry-run it on aac7 and aac6 | open (after the bug/optimization list) | results/C01.md |
