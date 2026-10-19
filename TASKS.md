# TASKS — live task list (open work, decisions, target session)

Scope: open work, user decisions still acting, proposed items and target-session items; closed items are in the
archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.

Rewritten 2026-10-19. Everything closed, and the older rows (NIC, P, N7E, N7b, N7G, the dated status paragraphs), are in
`archive:TASKS_HISTORY_2026-10-19.md` (older: `archive:TASKS_HISTORY_2026-10-12.md`). Measurements are in `RESULTS.md` and
`results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work, in priority order (user 2026-10-12)

Classes: 1 bugs, 2 shrink, 3 simplify, 4 performance, 5 docs (definitions in the project instructions). Within a class,
rows are in the order to do them.

| class | item | what | where |
|---|---|---|---|
| 1 | Full acceptance suite | one run of `ecalc/mnaccept.sh` (all steps) on current main at 1, 2 and 6 nodes, VERIFY OK and digits identical | TESTING.md |
| 1 | Fallback gates | every `=0` fallback of an adopted default (list in `ecalc/README.md`, fallback rows of `ecalc/switches_review.tsv`) still gives identical digits at 1 and 2 nodes; includes `COMM_SHMEM_NP_HEAP_THP=0` and `COMM_SHMEM_PX_THP=0` | TESTING.md gate |
| 2 | Switch audit and removals | review every non-default switch against its review date; remove `COMM_SHMEM_OFFS_WITH_DATA` (fix A, no gain) and `COMM_PHASE_TS` (superseded by `COMM_TRACE`) (D49); keep `COMM_SHMEM_PX_THP` as a permanent fallback | ecalc/README.md, `ecalc/switches_review.tsv` |
| 2 | `results/` consolidation | fewer, longer files: one owner per topic, index in `RESULTS.md`, history to the archive; keep every file a doc cites | RESULTS.md |
| 2 | Scripts and models consolidation | merge overlapping scripts, makefile targets and model scripts (`tests/` holds two model scripts) | tools/, tests/, ecalc/ |
| 3 | Python unit tests for the tools | tests for `tools/*.py` (switch registry, profile loader, plan values, doccheck, digit tools), run by the gate | TESTING.md |
| 4 | SHMEM 6-node confirmation | full size (3.8646e11 digits, 6 nodes, paired runs) of SHMEM with the default `COMM_SHMEM_NP_HEAP_THP=1` against libfabric (184.5 s, m); the fix is validated only at 2 and 3 nodes (0 stalls in 16 runs at 3 n and 0 in 6 at 2 n, against 9 stalls in 24 runs without; m) | results/SHMEM.md, results/SHMEM_STALLS.md |
| 4 | Remaining 3-6 % SHMEM gap at 3-4 nodes | start-up heap collapse (2.9 s per helper) and the excess in the communication span (m); measure the breakdown, then fix behind a default-off switch | results/SHMEM.md |
| 4 | Residual GPU-queue evictions | successful compaction on the first node's NUMA node 0 still evicts the queues for 1.4-12 s summed per run (m); no visible time cost at 3 n yet | results/SHMEM.md |
| 4 | Hugetlb backing switch | the target has hugetlb pages: back the node-process and helper heaps with hugetlb instead of collapsed 4 KiB pages, removing what compaction works on; new switch, off by default, then the 4-node paired test | results/SHMEM_STALLS.md |
| 5 | Docs | central docs in line with main (this rewrite); keep them so | the central docs |

## Proposed (needs the user's priority)

One line each (what, why, size); nothing here is started without the user's word.

- N7F: source of the run-to-run spread of the proxy arm at 6 n before the NP heap fix (sd 111 s against 15 s for libfabric, m, results/N7E.md); re-check after the 6-node confirmation above; aac7 runs.
- Node drain after our jobs: the 6 nodes of a job went to `needs_reboot` right after three of our jobs ended (2026-10-13 and 2026-10-14; two killed at their timeout, one normal end), cause unknown, site-side (results/N7.md section 6, results/NIGHT2.md); size unknown.
- Gather layout: each rank holds its own rows so the 3/4 remote pull in `k_gather` disappears; `k_gather_raw` is 2.0 s per APU at 1 node (m), the floor is the remote read (results/P2.md, OPPORTUNITIES N2); algorithm change, needs a scoping pass.
- Distributed division multiply: +94 s at 6 nodes against 1 (m, before the 2026-10-12 defaults; OPPORTUNITIES N1); re-measure the split on current main before choosing a fix.

## Decisions (user) still acting

- D48 (user 2026-10-18): `COMM_SHMEM_NP_HEAP_THP=1` is the default (main 23c2c1f); `=0` restores 4 KiB pages.
- D49 (user 2026-10-18): remove `COMM_SHMEM_OFFS_WITH_DATA` and `COMM_PHASE_TS`; keep `COMM_SHMEM_PX_THP` (default 1, D36) as a permanent fallback with its review date moved to the target session. Open until the removal is merged (class 2 row above).

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are in `OPPORTUNITIES.md`.

## Target session (later)

| # | item | status | where |
|---|---|---|---|
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13), kit `nodechk` rehearsal (T14) | open | internal target task list |
| 26 | **Kit paths not rehearsed** (A06): 2-NIC a3, 8-node a4, s2chk, `--spares` STOP, plan REFUSED | open (target session) | results/A06.md |
| 28 | **Target bring-up:** populate `profiles/target.conf` as soon as access exists; runbook and probe script written 2026-10-14 (profiles/README.md "Target bring-up runbook", `tools/target_probe.sh`), login dry run OK | needs the target for the rest | results/C01.md |
| 29 | **Hugetlb and SHMEM on the target:** the target has hugetlb pages and cray-shmem 12.1.0 (untested): re-run the SHMEM stall checks and the hugetlb switch there | target session | results/SHMEM_STALLS.md |
