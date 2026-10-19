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
| 1 | 6-node acceptance gate | the 6-node step of the acceptance suite (`ecalc/mnaccept.sh`) on current main; the 1-2 node steps passed 23/23 (2026-10-19); runs inside the pending 6-node confirmation item | TESTING.md |
| 4 | SHMEM 6-node confirmation | full size (3.8646e11 digits, 6 nodes, paired runs) of SHMEM with the default `COMM_SHMEM_NP_HEAP_THP=1` against libfabric (184.5 s, m); the fix is validated at 2, 3 and 4 nodes (0 stalls in 16 runs at 3 n and 0 in 6 at 2 n, against 9 stalls in 24 runs without; m); the item is queued | results/SHMEM.md, results/SHMEM_STALLS.md |
| 4 | Remaining 1-2 % SHMEM gap at 4 nodes | 4 n 6.4e10, pooled over runs without the first run of each item: libfabric 53.18 s (n 7), SHMEM DT 54.34 s (n 12, +2.2 %), DTIX 53.54 s (n 4, +0.7 %) (m); trace compare: exchange in flight equal (21.9 vs 22.1 s), DT has +2.5 s in copy-in and +1.0 s in quiet (m); 98n ABBA: DT 53.8 s = DTIX 53.8 s (n 8 each), S 55.8 s (n 2) (m), so INTER2=0 brings nothing; next: paired L against DT at 4 n and 2 n (items queued), then a fix for the copy-in behind a default-off switch | results/SHMEM.md §Four-node breakdown |
| 4 | Residual GPU-queue evictions | corrected: the summed counter counts each eviction once per GPU (4 times); real eviction time per node-process per run is 0.86 s SHMEM and 1.21 s libfabric, 93 % of it (SHMEM) in the first 30 s, no measurable wall-clock cost at 4 n (m); likely source: 4 KiB pages in GPU-registered host buffers; an A/B of `MEM_HSTAGE_COLLAPSE=1` (default-off switch, not merged) at 4 nodes is queued | results/SHMEM_STALLS.md §6.5 |
| 4 | Hugetlb paired test | `COMM_SHMEM_HUGETLB` is merged (default off, main b1468ae); the 4-node paired test (on against off) is not done; fold into target item 29 if the target is the only place with hugetlb | results/SHMEM_STALLS.md |
| 5 | Docs | central docs in line with main (this rewrite); keep them so | the central docs |

## Proposed (needs the user's priority)

One line each (what, why, size); nothing here is started without the user's word.

- N7F: source of the run-to-run spread of the proxy arm at 6 n before the NP heap fix (sd 111 s against 15 s for libfabric, m, results/SHMEM_ITEMS.md §N7E); re-check after the 6-node confirmation above; aac7 runs.
- Node drain after our jobs: the 6 nodes of a job went to `needs_reboot` right after three of our jobs ended (2026-10-13 and 2026-10-14; two killed at their timeout, one normal end), cause unknown, site-side (results/SHMEM_ITEMS.md §N7 section 6, results/NIGHT_RUNS.md §NIGHT2); size unknown.
- Gather layout: scoping done 2026-10-19; recommendation drop: best achievable saving 1.7 s of about 73 s at 1 node and at most 0.4 s of 184.5 s at 6 nodes (mod, upper bounds); optional cheap probe: owner-push gather, small; close OPPORTUNITIES N2 layout as not worth it.
- Distributed division multiply: +94 s at 6 nodes against 1 (m, before the 2026-10-12 defaults; OPPORTUNITIES N1); re-measure the split on current main before choosing a fix.
- First-run init artifact: the first run of each queue item has init 33-35 s against 23-25 s in later runs (4 items), coinciding with the lowest MemFree before the run (slow device-memory commit) (m); production is always a first run on its allocation; fix: memory preparation or page-cache eviction before the first run; size small, needs a cold-run probe with MemFree before and after.
- Start-up shortening (init 23.65 s at 2 n warm SHMEM, m): overlapping `mn_init` with the plane-pool allocation was tried and dropped (starves the heap collapse of 2 MiB blocks); next-best: register the four proxy pools from four threads (about 1.0 -> 0.3 s, mod); restructure the two self-tests (6.3 s for work that costs milliseconds, m); size small to medium.
- 6 n paired ABBA of grid [2,6] against [6], at least 8 runs per arm (earlier data +23 +/- 12 s, unexplained, m); aac7, one item.
- 2 n `ECALC_LIVE=1` measurement of the multi-node dm peak at 1e10-4e10 per node (never measured above 1 node); about 1 node-hour (a run is queued).
- NIC launch-group probe in every multi-node run, to separate the layv factor 0.92 from the rate; small.
- One-node default grid may be far from best above 1e11 digits (code not changed); measurement item, small.
- Part-file count is 2*size at size > 1: check the target scripts that sort `.part*` files; small.
- G2 soak results were never recorded and the start-up hang at 7.7e10 (A114 E1) is still open (results/CRASHES_HANGS.md); size unknown.
- README gaps: confirm whether `RNS_AUTO_PIECE_COST`, `ECALC_VSLOT_BUDGET` and `DIST_R3` still exist (removed or built in); doc-only, small.
- Switch audit ASK list (9 switches): MN_OUT_WAVES, FAST_EXIT, DM_TAIL_DEAD, NP_AUTO_MIN, COMM_KTIME, COMM_LAYER_STATS, COMM_XGMI_STATS, PE_PER_APU=2, NP_MN3; `COMM_XSTATS` after the parser migration; needs the user's keep-or-remove call per switch; small each.

## Decisions (user) still acting

- D48 (user 2026-10-18): `COMM_SHMEM_NP_HEAP_THP=1` is the default (main 23c2c1f); `=0` restores 4 KiB pages.

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are in `OPPORTUNITIES.md`.

## Target session (later)

| # | item | status | where |
|---|---|---|---|
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13), kit `nodechk` rehearsal (T14) | open | internal target task list |
| 26 | **Kit paths not rehearsed** (A06): 2-NIC a3, 8-node a4, s2chk, `--spares` STOP, plan REFUSED | open (target session) | results/TOOLS_PROFILES.md §A06 |
| 28 | **Target bring-up:** populate `profiles/target.conf` as soon as access exists; runbook and probe script written 2026-10-14 (profiles/README.md "Target bring-up runbook", `tools/target_probe.sh`), login dry run OK | needs the target for the rest | results/TOOLS_PROFILES.md §C01 |
| 29 | **Hugetlb and SHMEM on the target:** the target has hugetlb pages and cray-shmem 12.1.0 (untested): re-run the SHMEM stall checks and the hugetlb switch there | target session | results/SHMEM_STALLS.md |
