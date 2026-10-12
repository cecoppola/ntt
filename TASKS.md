# TASKS — live task list (open work, decisions, target session)

Shrunk 2026-10-12 (user plan rev 2). Everything closed or done, and the dated status sections, are in
`archive/TASKS_HISTORY_2026-10-12.md` (row numbers there keep their old meaning). Measurements are in `RESULTS.md`
and `results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work (2026-10-12)

| # | item | status | where |
|---|---|---|---|
| 1 | **Multi-node segfault** in `hipMemMap` from `vmm_bg_map` (about 1 % of 2-node runs, 3.5 % of 10-node runs, m); a fix is required before the target | **deferred** by the user to a later session; candidates `ECALC_VMM_BG=0` (on main, +20 s, 0/420 vs 4/422) and `ECALC_VMM_BG=2` (branch s35, no evidence yet) | "Deferred" section below |
| 22 | **Tier A code reduction** (A11: engine 2 + bug C1, about 1 290 lines, 24 switches; assumed) | open (user approval) | results/A11.md |
| 23 | **`DIST_LOGN_TEST` 4-node correction defect** (">64 corrections", forced-grid path, A05): diagnose at 4 nodes, 1e9, `POOL_LOG=29 DIST_LOGN_TEST=23` vs 24 | open | results/A05.md, G12.md |
| 27 | **P1 parameter system:** adopted settings become built-in defaults; machine profiles (`profiles/*.conf`, templates from C01); size-dependent values computed in the plan stage; one entry point with precedence defaults < profile < overrides, validation, an effective-config print + hash; a switch registry that generates the README table; experiments expire | step 1 (L5 registry) and step 2 (O3 profile loader, `gate.sh --profile`) merged 2026-10-12; remaining: size-dependent plan values, one entry point, expiry | results/C01.md, L5.md, O3.md |
| R | **Repo branch clean-up + `archive/` out of the repo** (plan rev 2, wave 1) | open | plan rev 2 |
| F | **Unused files** (find and remove; plan rev 2, wave 1) | open | plan rev 2 |
| S | **Switch audit, then removal** (wave 1 audit; wave 2 removal of switches not on the target line) | open | `ecalc/README.md` switch table |
| W | **Warnings + dead code** (compiler warnings, unreachable code; plan rev 2) | open | plan rev 2 |
| C | **aac7 program folders** (clean-up of the program folders on aac7; plan rev 2) | open | plan rev 2 |
| D | **Documentation rewrite** (wave 3) | open | plan rev 2 |
| P | **Performance on aac7** (wave 4): distributed division multiply, `k_gather`, init mapping | open | plan rev 2 |

Closed 2026-10-12 (user plan rev 2), details in the archive: 2 (aac6-only hang; 0 in 2,630 runs in G2/P2), 9 (password moved to a private file, guard hooks), 10 (uffd adopted as default), 24 (not needed), 25 and 43 (soaks: not on the work list), 32 (slow-mode study: not on the work list), 35 (fixed by `DBIG_SMALL_BOUNCE`), 47 (aac6-CPX-only; `BS_SEED_PULL=0` workaround in the CPX notes), 8 (repo clean-up folded into the current plan).

## Decisions (user)

| # | item | status | where |
|---|---|---|---|
| 15 | **`DIST_COALESCE` decision** (message aggregation, 1.0-4.5 s at 576 n, mod; S52 design) | open (user) | results/S52.md |
| 16 | **Target size decision:** 3.82e13 digits (margin 13.4 GB, mod) vs 3.71e13; do not exceed 3.82e13 | open (user) | results/S52.md |
| 29 | **Size question (with RELEASE on the line, L1: 3.82e13 is back to 12.9 GB rank-0 margin, mod; below is the T2048 + X2 only picture):** with T2048 + X2 the 576-node rank-0 device margin is 12.9 GB at 3.71e13 digits and 4.3 GB at 3.82e13 (below the 10 GB rule); largest size keeping 10 GB is about 3.79e13 (mod; finer than 0.01e13 not tried) | open: user decision | results/B01.md |
| 31 | **Uneven-groups policy:** any node count 572-576 passes the plan check, but the default group schedule depends on the factors of g; an uneven last group is plan-checked only, no run exists. Decide the policy and run a small test of a non-dividing `MN_GROUPS` (3-7 nodes) | open: user decision + small test | results/B04.md, B06.md |
| 40 | **Packed output `ECALC_OUT_PACKED=2` adoption path** (user task list 2026-10-11): -6.25 % bytes (m); 2.5-4.2 s at 576 (mod); needs the target write rate | verified 2026-10-12 (O4): fmt 2 digits identical at 1e9 and 1e10 (4 nodes), unpack round trip byte-identical, RECHECK OK (m); write time follows the shared file system more than the bytes. Still off by default; adoption waits for the target write rate | results/C05.md, O4.md |

## Target session (later)

| # | item | status | where |
|---|---|---|---|
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13), kit `nodechk` rehearsal (T14) | open | internal target task list |
| 26 | **Kit paths not rehearsed** (A06): 2-NIC a3, 8-node a4, s2chk, `--spares` STOP, plan REFUSED | open (target session) | results/A06.md |
| 28 | **P2 target bring-up:** populate `profiles/target.conf` as soon as access exists. Before access: a written runbook listing every `__FILL__` key, how it is determined, the expected range and the order (modules, build, NIC/fabric, libfabric monitor, memory edge, node screen, plan check, first small run); a script `tools/target_probe.sh` that collects the facts (modules, ROCm, APU count and memory, MemAvailable, NICs, libfabric providers/monitors, uffd, Slurm options, SHMEM library), prints a proposed target.conf and diffs it with the template; dry-run it on aac7 and aac6 | open (after the bug/optimization list) | results/C01.md |
