# TASKS — live task list (open work, decisions, target session)

Scope: open work, user decisions awaiting implementation, saved ideas and target-session items; closed items are in the
archive. | Maintained: updated in the same merge as any change that opens, closes or reschedules a task.

Shrunk 2026-10-12 (user plan rev 2). Everything closed or done, and the dated status sections, are in
`archive:TASKS_HISTORY_2026-10-12.md` (local archive) (row numbers there keep their old meaning). Measurements are in `RESULTS.md`
and `results/*.md`. Labels: (m) measured, (mod) modelled.

## Open work (2026-10-12)

| # | item | status | where |
|---|---|---|---|
| 23 | **`DIST_LOGN_TEST` 4-node correction defect** (">64 corrections", forced-grid path, A05): diagnose at 4 nodes, 1e9, `POOL_LOG=29 DIST_LOGN_TEST=23` vs 24 | open: fix approved; scheduled after all <1 h aac7 tasks | results/A05.md, G12.md |
| 27 | **P1 parameter system:** profiles, size-dependent plan values, one entry point, registry, expiry (steps 1-2 merged 2026-10-12: L5 registry, O3 profile loader) | open: six launch-line settings become built-in defaults (approved) - wave 2 lane B | results/C01.md, L5.md, O3.md |
| S | **Switch removal** (wave 2): audit approved 2026-10-12 - remove 83 settings + A5 + B4 | open: wave 2 | `ecalc/README.md` switch table |
| D | **Documentation rewrite** (wave 3): central docs, `tools/doccheck.py` (strict in the gate) done; left: regenerate the switch table after S, remove old text from print labels and registry notes (`results/` prune pass 2, `report.html` move and the `spill.c` check done: still used) | open: after S | `TESTING.md` §9 |
| P | **Performance on aac7** (wave 4): distributed division multiply (P1), `k_gather` (P2); init mapping evaluated 2026-10-12 — best candidate ≈1-1.5 s at 1 node, ≈0 at 2+ nodes (mod), not pursued | open: P1, P2 running | results/P3.md |

Done 2026-10-12 (waves 0+1): R branch clean-up + the old archive folder moved to the local archive (27 branches deleted, kept as `archive/<name>` tags); F 161 unused files removed; W compiler warnings 0, dead code and unused kernel arguments removed; C aac7 program folders tidied (raw data in `~/results_raw/`).

Closed 2026-10-12 (user decisions): 1 (mitigated by the uffd default), 16 and 29 (target size kept at 3.82e13), 22 (done by b02: engine 2 and bug C1 removed), 31 (node groups stay structured to the rack layout; uneven groups off for now). Closed 2026-10-12 (user plan rev 2), details in the archive: 2 (aac6-only hang; 0 in 2,630 runs in G2/P2), 9 (password moved to a private file, guard hooks), 10 (uffd adopted as default), 24 (not needed), 25 and 43 (soaks: not on the work list), 32 (slow-mode study: not on the work list), 35 (fixed by `DBIG_SMALL_BOUNCE`), 47 (aac6-CPX-only; `BS_SEED_PULL=0` workaround in the CPX notes), 8 (repo clean-up folded into the current plan).

## Proposed (needs the user's priority)

New work found by agents or the coordinator goes here first, one line each (what, why, size), until the user schedules,
merges or drops it. Nothing is started from this section without the user's word.

- (none)

## Decisions (user)

| # | item | status | where |
|---|---|---|---|
| 40 | **Packed output `ECALC_OUT_PACKED=2`**: -6.25 % bytes (m); verified 2026-10-12 (O4: digits identical, round trip byte-identical, RECHECK OK, m) | decided: default on - implementation in wave 2 lane A | results/C05.md, O4.md |

## Ideas (saved, not scheduled)

- 15 `DIST_COALESCE` (message aggregation, 1.0-4.5 s at 576 n, mod; results/S52.md): dropped for now.

## Target session (later)

| # | item | status | where |
|---|---|---|---|
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13), kit `nodechk` rehearsal (T14) | open | internal target task list |
| 26 | **Kit paths not rehearsed** (A06): 2-NIC a3, 8-node a4, s2chk, `--spares` STOP, plan REFUSED | open (target session) | results/A06.md |
| 28 | **P2 target bring-up:** populate `profiles/target.conf` as soon as access exists. Before access: a written runbook listing every `__FILL__` key, how it is determined, the expected range and the order (modules, build, NIC/fabric, libfabric monitor, memory edge, node screen, plan check, first small run); a script `tools/target_probe.sh` that collects the facts (modules, ROCm, APU count and memory, MemAvailable, NICs, libfabric providers/monitors, uffd, Slurm options, SHMEM library), prints a proposed target.conf and diffs it with the template; dry-run it on aac7 and aac6 | open (after the bug/optimization list) | results/C01.md |
