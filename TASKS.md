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
| 4 | SHMEM exchange gap against libfabric | closed, superseded 2026-10-21: the aac7 Cray SHMEM tuning phase ended by user order; the project builds target-style communication (cray-shmem 12.1, 8 NICs per node, GPU-memory buffers) emulated with libfabric; record: results/SHMEM.md closing section | results/SHMEM.md |
| 4 | Residual GPU-queue evictions | closed, superseded 2026-10-21 with the aac7 SHMEM phase (no measurable wall-clock cost at 4 n, m) | results/SHMEM_STALLS.md §6.5 |
| 5 | Docs | central docs in line with main (this rewrite); keep them so | the central docs |

## Proposed (needs the user's priority)

One line each (what, why, size); nothing here is started without the user's word.

- BS_ARENA_TRIM_GB input hardening (review nits of the atrim merge): cap the parsed value as a double before the size_t cast ("inf"/"nan"/huge values are undefined behaviour, binsplit.c ~591) and fflush the rank-0 banner; tiny.
- Node drain after our jobs: the 6 nodes of a job went to `needs_reboot` right after three of our jobs ended (2026-10-13 and 2026-10-14; two killed at their timeout, one normal end), cause unknown, site-side (results/SHMEM_ITEMS.md §N7 section 6, results/NIGHT_RUNS.md §NIGHT2). Same symptom again on 2026-10-19 at about 11:05 ET: 7 nodes went to `needs_reboot` ("reboot timed out") right at the end of our jobs, 4 nodes of a 6-node job after a normal end and 3 nodes of two 2-node jobs cancelled mid-run (m). No admin request (D43); size unknown. 2026-10-20: five nodes lost their cxi NICs during the day (`fi_info -p cxi` lists none; the node screen excludes them) and one more stopped responding; each had run our items 15 min to 3 h before (one 23 min after a 6 n SHMEM + libfabric item ended); cause unknown, aac7 runs paused at 13:50 ET.
- Gather layout: scoping done 2026-10-19; recommendation drop: best achievable saving 1.7 s of about 73 s at 1 node and at most 0.4 s of 184.5 s at 6 nodes (mod, upper bounds); optional cheap probe: owner-push gather, small; close OPPORTUNITIES N2 layout as not worth it.
- Start-up shortening (init 23.65 s at 2 n warm SHMEM, m): overlapping `mn_init` with the plane-pool allocation was tried and dropped (starves the heap collapse of 2 MiB blocks); next-best: register the four proxy pools from four threads (about 1.0 -> 0.3 s, mod); skipping the two self-tests gains no time (2026-10-20: their window hides the arena's background mapping, which then slows bs instead; results/DIVISION.md §DB_POOL_VMM); the remaining lever is the mapping itself: a smaller dm extra in the arena plan or a faster mapper (the four APUs map one at a time, about 20 s per run); size medium.
- 6 n paired ABBA of grid [2,6] against [6], at least 8 runs per arm (earlier data +23 +/- 12 s, unexplained, m); aac7, one item.
- NIC launch-group probe in every multi-node run, to separate the layv factor 0.92 from the rate; small.
- G2 soak results were never recorded and the start-up hang at 7.7e10 (A114 E1) is still open (results/CRASHES_HANGS.md); size unknown.
- Switch audit ASK list (1 switch left; COMM_KTIME, COMM_LAYER_STATS, COMM_XGMI_STATS and NP_MN3 were removed 2026-10-19, D56; MN_OUT_WAVES, ECALC_FAST_EXIT, DM_TAIL_DEAD, ECALC_NP_AUTO_MIN and `COMM_SHMEM_PE_PER_APU=2` 2026-10-20, Q53): `COMM_XSTATS` after the parser migration; kept until the target session (D56, user 2026-10-19).
- `BS_ARENA_ROOM` default 0.16 -> 0.14: one arena chunk less per APU at the 2 n and 4 n standard sizes (-8.6 GB per node, 0 remaps, time neutral; items 9jz, 9ka), no change at 6 n (9kc); needs a pool replay over sizes (`tests/as_pool_replay.py`) before a default change; small.

## Decisions (user) still acting

- D48 (user 2026-10-18): `COMM_SHMEM_NP_HEAP_THP=1` is the default (main 23c2c1f); `=0` restores 4 KiB pages.
- D53 (user 2026-10-19): `COMM_SHMEM_PX_SEND` unset is 3 (PX3) under `PE_PER_APU=1` with an OpenSHMEM 1.5 build, else 1 (main a6cc1b3); `=1` is the fallback.
- D54 (user 2026-10-19): `COMM_SHMEM_PEER_ORDER` and `COMM_SHMEM_PX_BLOCK_MB` removed (main 66e34d0; gates 7/7 and acceptance 23/23, items 99l and 99m): peer loops walk in rank order, the PX3 chunk is the fixed 4 MiB.
- Q58 (user 2026-10-20): `MN_NONPOW2_CAP` default auto = on only from `MN_NONPOW2_MIN` (4.5e10) digits per node-process, off below (3 n 6e10 was +8.9 s with it on; a group whose size is not a power of two takes the next power-of-two plane: 6 n 3.70e11 -18.3 s with libfabric, +41.3 GB per node of plane memory; `=1` forces on, `=0` restores the power-of-two plane; results/DIVISION.md).
- Q59 (user 2026-10-20): `COMM_SHMEM_PX_SCHED` unset = `auto` (99 up to 4 nodes, else PX3; 4 n -14 s; `=0` restores PX3; results/SHMEM.md).
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
| 29 | **Hugetlb and SHMEM on the target:** the target has hugetlb pages and cray-shmem 12.1.0 (untested): re-run the SHMEM stall checks and the paired test of `COMM_SHMEM_HUGETLB` (on against off, 4 nodes) there; aac7 has no free hugetlb pages, so the merged switch (default off) only exercises its fallback here | target session | results/SHMEM_STALLS.md |
| 30 | **Early offset publish (option A'):** publish the receive offset before the data is needed; needs a separate intra-receive buffer (extra device memory per slot, memory cost); deferred to the target session by the user (D55) | target session | results/SHMEM.md |
| 31 | **SHMEM gap at 5 or more peers and at scale** on the target fabric with cray-shmem 12.1: the aac7 gap grows with the off-node peers (+0.8 % at 1 peer, about +7 % at 3, about +9 % at 5); measure it first at 4, 8 and 16 nodes against libfabric | target session | results/SHMEM.md |
| 32 | **`PX_SEND=4` with `PEER_ORDER=shift` at 3 peers only:** superseded 2026-10-21 (aac7 SHMEM tuning ended; results/SHMEM.md closing section) | closed | results/SHMEM.md |
