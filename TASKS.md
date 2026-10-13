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
| 1 | 23 | **`DIST_LOGN_TEST` 4-node correction defect** (">64 corrections", forced-grid path): 4 nodes, 1e9, `POOL_LOG=29 DIST_LOGN_TEST=23` failed, 24 OK | **done 2026-10-12: already fixed**: the cause was `mdb_add_shifted` on a group with g0 > 0 (the tree's gridded P), fixed by 9a746c8 (2026-09-22); bracketed 06638dd fails / 9a746c8 passes; main passes at 4 aac7 nodes (caps 2^24-2^26, 1e10 at 2^29); no code change | results/T23.md |
| 2 | S | **Settings removal:** 83 settings + A5 (overlap/device flow) + B4 (TCP) removed or built in; registry 269 → 185 rows (m) | done 2026-10-12: lane A (42 settings and 1 value removed, ~1,800 lines out, m) and lane B (42 rows, D1e six built-in defaults) on main, gates PASS | results/W2A.md, W2B.md |
| 2 | SH3 | **Shrink pass 3** (approved AUD2/AUD3 leftovers, results/SHR2.md): `rns_dist.c` dead items (gen_pack_bench, rns_mul_dist_hd, acc_flat, part_owner, rns_mul_dist, duplicate pow_table) after SIMP merges; merge duplicated helpers (dmod128/192, k_store/k_copy_h2d, mn_powmod/powmod_h, make_mdb); host product tier (≈270 lines) needs a fast test reference first | waits for SIMP | results/SHR2.md, AUD2.md |
| 3 | 27 | **Parameter system:** registry, profiles, six built-in defaults, size-dependent plan values (`tools/plan_values.py`) and review dates for settings (`ecalc/switches_review.tsv`) done 2026-10-12 | done except one user decision (results/T27.md): `MN_GROUPS` at 576 - k nodes. D9 resolved (a), 2026-10-12: the target launch block no longer types pool/heap (a typed 1536/2048 MiB was stale under the X2 default: plan 2304/2816); it takes them from `tools/plan_values.py --exports`. SCR2 2026-10-12: aac7 launch line moved into `profiles/aac7.conf` (`e16_headline.sh` archived, D15), SOS/OSHMEM launch paths removed from `mnrun.sh` (D14), the `run` wrapper removed (D13), and the 2-node gate forces the target's four-prime form P24 (`--p24`; since D18 P24 is the multi-node default and the gate checks it, `--mn-primes`) | results/C01.md, L5.md, O3.md, T27.md |
| 4 | P | **Performance on aac7:** distributed division multiply (+94 s at 6 nodes vs 1, m); `k_gather` and init mapping evaluated, not pursued | running: division multiply | results/P2.md, P3.md |
| 5 | D | **Documentation:** central docs and the strict doc check done; final pass after S done 2026-10-12 (settings table regenerated, stale settings and scripts removed from the docs, `results/` prune 3); each merge updates its docs | done (keep: each merge updates its docs) | `TESTING.md` §9 |

Done 2026-10-12 (waves 0+1): R branch clean-up + the old archive folder moved to the local archive (27 branches deleted, kept as `archive/<name>` tags); F 161 unused files removed; W compiler warnings 0, dead code and unused kernel arguments removed; C aac7 program folders tidied (raw data in `~/results_raw/`).

Closed 2026-10-12 (user decisions): 1 (mitigated by the uffd default), 16 and 29 (target size kept at 3.82e13), 22 (done by b02: engine 2 and bug C1 removed), 31 (node groups stay structured to the rack layout; uneven groups off for now). Closed 2026-10-12 (user plan rev 2), details in the archive: 2 (aac6-only hang; 0 in 2,630 runs in G2/P2), 9 (password moved to a private file, guard hooks), 10 (uffd adopted as default), 24 (not needed), 25 and 43 (soaks: not on the work list), 32 (slow-mode study: not on the work list), 35 (fixed by `DBIG_SMALL_BOUNCE`), 47 (aac6-CPX-only; `BS_SEED_PULL=0` workaround in the CPX notes), 8 (repo clean-up folded into the current plan).

## Proposed (needs the user's priority)

New work found by agents or the coordinator goes here first, one line each (what, why, size), until the user schedules,
merges or drops it. Nothing is started from this section without the user's word.

- (class 1) Newton reciprocal error bound: t_newton found one quotient whose reciprocal is −22 units off, beyond the test's ±8 bound; identical on main (not caused by SIMP). Either the bound or the reciprocal is wrong. Local analysis first (S–M); results/SIMP.md.
- Gather layout: each rank holds its own rows so the 3/4 remote pull in `k_gather` disappears; `k_gather_raw` is 2.0 s/APU at 1 node (m), floor is the remote read (results/P2.md). Algorithm change, size unknown — needs a scoping pass.

## Decisions (user)

| # | item | status | where |
|---|---|---|---|
| 40 | **Packed output `ECALC_OUT_PACKED=2`**: -6.25 % bytes (m); verified 2026-10-12 (O4: digits identical, round trip byte-identical, RECHECK OK, m) | done 2026-10-12: default `ECALC_OUT_PACKED=2` (batch D5, gates PASS) | results/C05.md, O4.md, W2A.md |

## Ideas (saved, not scheduled)

Performance ideas, open or tried, are kept in `OPPORTUNITIES.md` (old item 15 `DIST_COALESCE` is there as S52).

## Target session (later)

| # | item | status | where |
|---|---|---|---|
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13), kit `nodechk` rehearsal (T14) | open | internal target task list |
| 26 | **Kit paths not rehearsed** (A06): 2-NIC a3, 8-node a4, s2chk, `--spares` STOP, plan REFUSED | open (target session) | results/A06.md |
| 28 | **P2 target bring-up:** populate `profiles/target.conf` as soon as access exists. Before access: a written runbook listing every `__FILL__` key, how it is determined, the expected range and the order (modules, build, NIC/fabric, libfabric monitor, memory edge, node screen, plan check, first small run); a script `tools/target_probe.sh` that collects the facts (modules, ROCm, APU count and memory, MemAvailable, NICs, libfabric providers/monitors, uffd, Slurm options, SHMEM library), prints a proposed target.conf and diffs it with the template; dry-run it on aac7 and aac6 | open (after the bug/optimization list) | results/C01.md |
