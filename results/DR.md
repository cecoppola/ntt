# DR — detect and retry (C3): a faulty product repeats itself instead of ending the run

## Summary (final, 2026-11-01; branch `c3check`, tested commit 9129ac52; all numbers measured on aac7 unless marked modeled)

`ECALC_DR` (default 0 = off, the unchanged code paths) selects what is checked inside the run; a failed check repeats only the faulty piece or product, once, inside the product call (no extra memory beyond the first attempt's peak, no change of digits):

| level | adds | on a fault |
|---|---|---|
| 1 | A: the CRT flag of each P24 piece becomes a collective decision | piece repeats |
| 2 | B: tree product residue identity R(C) = R(A) R(B) + R(X) with tracked residues; C: leaf and division-start residues vs the recurrence | product repeats; operand corrupted at rest, leaf or division-start mismatch: fatal naming node and APU |
| 3 | D: division low product X Q mod B^w (xql, recomputed once); wrap check; D': cyclic product modulo B+1 and B-1; independent check of R(R1) | xql and cyclic product repeat once; a wrap or step-1 mismatch is fatal naming the step |

`ECALC_DR_TOTAL` (default 8) caps the failures per run; a second failure at the same place is fatal with the location. Exit line per process: `DR: <checks> checks, <injected> injected, <failures> failures, <recovered> recovered (...), <fatal> fatal`. At size 1 (one PE group) the checks are inactive (no DR lines).

Results at 9129ac52 (step 5, below): `mnaccept --only unit,e9,mn,grid,corr` with `ECALC_DR=3`: 28 passed, 0 failed; 1, 2 nodes and path 2: gates and 1e11 digits identical, all injected faults of the kinds above recovered or fatal as designed. Cost (step 6, 2 nodes, 128.82e9 digits, ABBA): B (`ECALC_DR=3`) minus A (unset) = +0.48 s on 121.18 s (+0.40 %), 95 % CI -2.53 ... +3.49 s: not distinguishable from zero at 2 nodes. The design model for 576 nodes is 0.1-0.2 % (modeled; not extrapolated from the 2-node figure).

Known limitations and deviations: the wrap check is exact and fatal (inputs are consumed in place); the persistent case prints the exit summary twice; the D' modulus B-1 is weak for operands divisible by 10^18-1; retries free the bad result and re-allocate instead of zeroing in place.

Not tested: different per-PE failure histories within one group (counts equal on all PEs in every test); a launcher without kill-on-bad-exit (local-only fatals rely on `srun --kill-on-bad-exit=1`; the bench launcher change was checked with `bash -n` only); D' injection before normalisation (the injection flips a limb after `mdb_norm`); peak memory of a retry at the memory edge (runs here are far below it); the fatal path of the step-1 remainder check R(R1) (added in the step-4 review fixes; exercised only on clean data, as the third division check) and the "D NOT ACTIVE" notice with its counter (no injection or test triggers either); DR at the target's node counts (6 nodes and above) and the retry rate there.

**Question.** Target runs failed with CRT flags (a wrong transform-domain value, kind 1) and T1/VERIFY failures (wrong in-range limbs, kind 2), more often the larger the run. User decision Q142: full detect-and-retry. Can the product tiers detect a transient fault inside the product, before it reaches C, and repeat only the faulty piece, with no extra memory and no change of digits?

**Change / branch.** Branch `c3check` (from main c86e5fcc). Switch `ECALC_DR` (0 = off, default, the unchanged code paths; 1 = A; 2 = A + B + C; 3 = A + B + C + D + D'), `ECALC_DR_TOTAL` (default 8), fault injection `ECALC_DR_TEST` (+ `_K`, `_PE`, `_N`, `_M`). Registry rows with review dates; `ecalc/README.md` generated table. Files: `ecalc/dr.c/h` (switches, injection parser, counters, context, residue helpers), `ecalc/tests/t_dr.c`, `ecalc/rns_dist.c` (`mn_core`).

## Design summary (planning agent, 2026-11-01; code read at main c86e5fcc)

Kind (1), the CRT flag, already detects the fault; missing is recovery: the flag becomes a collective, per-piece decision inside `mn_core`, taken before any result limb leaves the PE (the scatter and the chunked rounds write C), and the piece repeats. Kind (2) needs an independent expected value (a residue computed at consumption from corrupted storage proves nothing): the 62-bit prime `t1_q[0]` (`DR_Q`) and tracked residues with exact end-to-end identities: tree products (tracked residues seeded by the T1 recurrence), the leaf (per-node recurrence), each division step (A' = X Q + R), and the flagless cyclic product (modulo B+1 and B-1; B^2-1 divides B^P-1 for even P). Rejected: coefficient sums at x = +-1 (misses most kind-1 faults, whole-limb input faults and delivery faults).

| product | check | kind 1 | kind 2 |
|---|---|---|---|
| leaf, node-local | C: residues of P_r, Q_r vs the per-node recurrence | n/a | detected; fatal naming the node |
| tree, full P24 +- X | A flag -> piece retry; B R(C) = R(A) R(B) + R(X) from tracked residues -> product retry | recovered | production / redistribution / delivery faults recovered; operand corrupted at rest -> fatal naming node and APU |
| reciprocal, division estimate | A only | recovered | cannot make wrong digits (corrections after the exact remainder fix any error) |
| division low product X Q mod B^w | A + D before `mn_dkm_corr` | recovered | recovered (xql recomputed once) |
| wrap: L (low_basis), U (cyclic) | D on Rc; D': U = X~ Q'' mod (B+1), (B-1) | L: flag; U: D' | U recovered; L or assembly -> fatal naming the step |
| backstop | existing T1 at the end | | |

Retry rules: every retry runs inside the product call (inputs are freed right after it returns); no extra memory (planes reused, C's share re-zeroed in place); the decision is collective (A by `grp_max` on mesh 0, B and D by an allgather summed identically on every node); the repeat runs with the transform cache off (`cache_drop()`); a second failure at the same place is fatal with the location; `ECALC_DR_TOTAL` caps the failures per run. Localization: the log line carries product number, size, piece i,j of ka x kb, cause, attempt, first flagged point and, from an allgather of per-node masks, the PEs. Exit summary per process: `DR: <checks> checks, <injected> injected, <failures> failures, <recovered> recovered (pieces, products, division), <fatal> fatal`.

Injection: `ECALC_DR_TEST=<kind>`, `_K` (k-th P24 product of the process), `_PE` (-1 = all), `_N` (consecutive attempts; 2 = persistent). Kinds: flag (step 2), deliver, store, xql, cyc, leaf (steps 3-4).

Cost model (modeled, 3.8e13 digits, 576 nodes): A 0.05-0.2 s, B about 60 ms, C about 5 ms, D about 12 ms, D' 0.1-0.15 s; total 0.25-0.4 s = 0.1-0.2 % of a 200 s run.

## Counting rule (step 2)

`failures` counts the PEs that failed the check of a piece, group-wide, so it is the same on every process; `injected` is local (the injections done by this process's threads); `recovered` counts those PEs once the repeat passes; the gate sums `injected` over the processes and compares: injected = failures = recovered. `checks` counts piece decisions (one `grp_max` each).

## Steps

| step | content | status | commit | gates |
|---|---|---|---|---|
| 1 | `dr.c/h`, `t_dr`, registry, this file | done | 4af77c91 | t_dr VERIFY OK (9351 checks, measured); 1-node and 2-node gates with `ECALC_DR` unset PASS, digits identical (measured) |
| 2 | A in `mn_core` (flag retry, fatal-to-flag conversions, injection `flag`) | done | 14926757 | see Measured, step 2 |
| 3 | B + C (tree residue identity, leaf check, division-start compare): `rns_mul_dist_mn_chk`, tracked residues in `mn.c`, injection `deliver`, `store`, `leaf` | done | 489e38ee, 583b6479, 1d266195 | see Measured, step 3 |
| 4 | D + D' (division `mn_dkm_low` steps 1-2, the wrap, the cyclic product; injection `xql`, `cyc`, test modifier `leafslow`) | done | df4caf7e, e7274f10, (this commit) | see Measured, step 4 |
| 5 | full regression with `ECALC_DR=3` | done | 9129ac52 | see Measured, step 5 |
| 6 | timing (ABBA), docs (ALGORITHM §10, TESTING §4, TASKS) | done | this commit | see Measured, step 6 |

## Deviations from the design

- Step 2 injection positions: `P24_CHECK_TEST_S` (the existing 0|6|12 row-class switch) positions the `flag` injection; no new switch.
- The `_PE` of `flag` is the global PE number (rho); `-1` injects on every PE of every process.
- Step 3, retry: the design re-zeroed C's share in place (`db_zero_fill`); the code frees the bad result and re-runs `mn_grid`, which allocates C again. Peak memory: the bad C is freed before the repeat allocates its replacement, so the repeat's peak equals the first attempt's (one C share plus the planes; the planes are reused, the transform cache is dropped); an in-place zero fill would have saved only the allocator round trip. Unmeasured at the memory edge (the runs here are far below it).
- Step 3, leaf: the tracked residues are seeded from the leaf by `db_mod_qs` (one extra pass over the node's P_r, Q_r at the start of the tree) and the independent check is the node's recurrence (`pq_bg_run`, the T1 values of its term range), compared as soon as the recurrence is done (non-blocking after every level and at the division's start; a recurrence still running at the division's start is not waited for, so it keeps overlapping the division: the compare is deferred to the join in `out_stage` (`mn_dr_leaf_final`, before T1), runs exactly once, and is counted in the summary line as `leaf checks deferred`; a recurrence that never started is logged and counted as `skipped`; `pq_bg.started` is stored with release), not the other way round (the design seeded from the recurrence). `ends` counts the leaf compare and the division-start compare of the tree's final P, Q (2 per node).
- Step 3, local-only fatals (leaf check, division-start compare) call `ec_fatal` on the failing node only; the other nodes are ended by the launcher's `srun --kill-on-bad-exit=1` (`mnrun.sh`, D34), the same path as every other multi-node fatal. The bench launcher `ecalc_srun` (`target/bench/items/_ecalc.sh`) lacked the option (peers would have waited for the item timeout); it now passes it (`ECALC_SRUN_KILL=0` switches it off for the independent per-node probes of `mem02.sh`). Other multi-node srun launches checked: `target_kit.sh` node check (independent probes, no collective), `remote/run/lib.sh` snapshot and monitor (independent, monitor deliberately `=0`). Not tested with a launcher without that option; the `ecalc_srun` change was checked with `bash -n` only (no bench item run).
- The persistent case (`_N=2`) prints the exit summary twice per process (the repeat's failure and the fatal path); harmless.

## Open issues

- Step 2 covers only the flag (kind 1) of the P24 piece; kind 2 and delivery faults are steps 3-4.
- Not exercised: PEs of one group with different failure counts (the cap decision is collective, so it cannot diverge, but no test builds such a history; the counts here are equal on all PEs).

## Measured, steps 1-2 (aac7, 2026-11-01; items 60_dra_x1, 63_dra_x2b, 62_dra_x3 of the DR-A series; all counts measured)

Setup names: 2p = `t_mn_grid 1 28` on 2 processes; 4p = 1 node, 4 PEs (`ECALC_PE_PER_GPU=1 COMM_TRANSPORT=xshmem XSHMEM_LOOPBACK_PES=1 ECALC_NP=4`); 4 APU threads per process, so `_PE=-1` injects 8 (2p) or 4 (4p) at once. Counts are the DR summary lines (injected summed over processes, failures and recovered as the group-wide value).

| case | result |
|---|---|
| step 1 gate, `ECALC_DR` unset, 1 node and 2 nodes, 1e9 | PASS, digits identical |
| `t_mn_grid`, `ECALC_DR=1`, no injection: 2p, 3 processes, 4p; each also with `RNS_DIST_CACHE_PARTIAL=1` and `DIST_GEN=1` | 0 failures, VERIFY OK in every process |
| legacy `P24_CHECK_TEST` runs, DR off | as before (fatal with the CRT flag; `P24_CRT_ANY=0` s=0/6 pass the flag and fail VERIFY) |
| `flag`, K in {1,3}, PE=0, S in {0,6,12}, 2p and 4p (12 runs) | injected = failures = recovered = 1 in all 12; VERIFY OK; the log line names "PE 0 (node 0, APU thread 0: CRT flag)" |
| same with PE=-1 (12 runs) | injected = failures = recovered = 8 (2p) and 4 (4p) in all 12; VERIFY OK; the line lists the PEs |
| `flag` K=2 PE=-1, 3 processes: 12 injected = failures = recovered, VERIFY OK; K=3 PE=-1 with `RNS_DIST_CACHE_PARTIAL=1` (2p); K=3 PE=0 with `DIST_GEN=1` (4p) | 8 / 1 injected = failures = recovered; VERIFY OK |
| `_N=2`, K=1 PE=0, 2p and 4p | rc 3 in both; "attempt 2 failed at 1 PE(s): PE 0 ...; failed again after the repeat (persistent), stopping" plus node, PE, APU thread, plane, first flagged point; the summary shows fatal 2 (2p, one per process) and 4 (4p, one per APU thread) |
| 2 nodes, 1e9, `ECALC_DR=1`: no injection / K=1 PE=0 / K=5 PE=-1 | digits identical; injected = failures = recovered = 0 / 1 / 8 |
| 2 nodes, 1.6e10, `MN_TREE_LOGN_TEST=26`: off / `ECALC_DR=1` / K=1 PE=0 / K=3 PE=-1 | digits identical in all four (total 58.6 / 55.9 / 56.1 / 55.5 s, single runs, not a timing comparison); 0 / 0 / 1 / 8 injected = failures = recovered; K=1 is a 5 x 5-grid piece (429282525 x 459606368 limbs) |
| 2 nodes, 1.6e10, K=2 PE=0 `_N=2` | rc 3, location printed (product #2, piece of the 5 x 5 grid), no digits |

Items 61_dra_x2 failed in 3 s per run (NIC-free node, libfabric rc -61; ALLOW_SLOW was wrong for the multi-process runs); repeated as 63_dra_x2b on a normal node.

## RESUME

Branch `c3check`; steps 1 and 2 done and gated (above). Step 3 done and gated (Measured, step 3). Step 4 done and gated (Measured, step 4). Steps 5 and 6 done (Measured, steps 5 and 6). Open: the follow-ups in TASKS "Proposed".

## Review fixes and the cap test (aac7, 2026-11-01, commit e22efde; item 70_drfix; all counts measured)

Fixes in `dr_piece_decide`: (1) the `who[640]` text is clamped after every append (a long PE list used to push `sizeof who - k` below zero); (2) the `ECALC_DR_TOTAL` decision is collective: the first `grp_max` now carries the verdict (bit 32) and the process's count of failed pieces (low 32 bits), so every PE of the group adopts the group's maximum count and takes the same branch; no collective is added, and with `ECALC_DR` unset nothing changes. New injection setting `ECALC_DR_TEST_M` (default 1): corrupt M consecutive P24 products from the K-th (each product has its own latch), so two distinct products fail in one run.

| case (`t_mn_grid 1 28`, `ECALC_DR=1`, `flag`) | result |
|---|---|
| gate, 1 node, `ECALC_DR` unset | PASS, digits identical (1e9) |
| `t_dr` | VERIFY OK, 9353 checks |
| K in {1,3}, PE in {0,-1}, 2 processes (regression) | injected = failures = recovered = 1 (PE 0) / 8 (PE -1); VERIFY OK in both processes |
| cap: `ECALC_DR_TOTAL=1`, K=1, M=2, PE=0 / PE=-1, 2 processes; PE=-1, 3 processes; PE=-1, 4 PEs on 1 node | no hang (9 s, 9 s, 10 s, 4 s); rc 3 (the fatal return code) in all four; product #2 stops with "the run's limit ECALC_DR_TOTAL of failed pieces is reached" and the location, printed by every process (2 / 2 / 3 / 4 lines) |
| under the cap: `ECALC_DR_TOTAL=2`, same injections, PE=0 (2 processes), PE=-1 (3 processes, 4 PEs) | both products recovered (failures = recovered = 2 PE / 24 / 8), VERIFY OK in every process, rc 0 |
| persistent, `_N=2`, 2 processes | rc 3, location printed, as before |


## Measured, step 3 (aac7, 2026-11-01, 16:17-16:39 ET; items 81_drb_t1, 82_drb_n2, 83_drb_n1; commits 583b647 and 1d26619; all counts measured)

`t_mn_grid 1 28`, `ECALC_DR=2` (shapes: product, product + x, subgroups, mixed). Setup 4p = 1 node, 4 PEs (`ECALC_PE_PER_GPU=1 COMM_TRANSPORT=xshmem XSHMEM_LOOPBACK_PES=1 ECALC_NP=4`).

| case | result |
|---|---|
| no injection: 2, 3 processes, 4p; 2p with `RNS_DIST_CACHE_PARTIAL=1`; 4p with `DIST_GEN=1` | rc 0, VERIFY OK in every process, 0 failures; checks per process 236 pieces + 30 products (2 and 3 processes), 314 + 66 (4p) |
| `deliver` K=1 PE=0 (2p), K=3 PE=-1 (2p), K=2 PE=1 (3 processes), K=1 PE=-1 (4p) | rc 0, VERIFY OK, 1 failure, 1 recovered (a product), products 31 / 31 / 31 / 67 (one more than without injection = the repeat) |
| `deliver` K=1 PE=0 `_N=2` (2p) | rc 3 after 8 s; "checked product #1 (10066329 x 8388608 limbs), group [0, 2): attempt 2: R(C) = ..., expected R(A) R(B) = ...; failed again after the repeat (persistent), stopping" |
| `store` K=1 PE=0 (2p), K=2 PE=1 (2p), K=1 PE=2 (4p) | rc 3 in 9 / 9 / 3 s; the message names the corrupted operand's holder: "node 0 APU 1 (operand B)", "node 1 APU 1 (operand B)", "node 2 APU 0 (operand B)", with product number, size and group (the FATAL header's APU is the reporting thread's device, not the corrupted one) |
| stage-2 `flag` regression, K=1 PE=-1 and K=3 PE=0 (2p) | VERIFY OK; 8 / 8 and 1 / 1 injected / failures / recovered |
| `t_dbig 0` (parts, `db_set_limb`), `t_dr` | VERIFY OK (705 checks), VERIFY OK (9353 checks) |
| `mnaccept --only mn` (e8 at sizes 2, 3, 4; e9 at sizes 2, 4), `ECALC_DR=2`, also with `MN_GROUPS=3` and `MN_GROUPS=4` | 5 PASS in each of the three runs, digits identical, all nodes VERIFY OK, 0 failures |
| gates, `ECALC_DR` unset, 1 node and 2 nodes, 1e9 | PASS, digits identical (the 2-node gate: P24 pool of 4 primes) |

2 nodes (ecalc, aac7 line, reference sha comparison):

| case | result |
|---|---|
| 1e9 `ECALC_DR=2`, no injection | digits identical, VERIFY OK, total 12.6 s; per node 28 checks (24 pieces, 2 products, 2 ends) |
| 1e9 `deliver` K=1 PE=0; K=2 PE=1 | digits identical, VERIFY OK (12.2, 11.8 s); 1 failure, 1 recovered; products 3 per node |
| 1e9 `leaf` PE=0; PE=1 | rc 143 (the peer ended by srun) / FATAL rc 3 "leaf check failed on node 0" / "on node 1" with both residue pairs; the job ended 10.6 s and 11.1 s after launch (a full run takes about 20 s from launch); log time resolution 0.5 s |
| 1.6e10 (`MN_TREE_LOGN_TEST=26`) `ECALC_DR=2`, no injection | digits identical, VERIFY OK, total 57.0 s (the step-2 `ECALC_DR=1` run: 55.9 s; single runs, not a timing comparison); 84 checks per node (80 pieces, 2 products, 2 ends) |
| 1.6e10 `deliver` K=1 PE=0 | digits identical, VERIFY OK, total 54.9 s; 1 failure, 1 recovered; 110 checks per node (105 pieces, 3 products) |
| 1.6e10 `leaf` PE=0 | FATAL "leaf check failed on node 0", the job ended 20.8 s after launch (a full run 123 s from launch) |

Checks per level per node (no injection, 1e9, `products` column of the summary line; the design expects 2 per binary level, 2 (nch - 1) per k-way level, plus 2 ends): 2 nodes, one binary level: 2 products. 1 node 4 PEs, default schedule (2 binary levels): 4. `MN_GROUPS=4` (one 4-way level, 3 combines): 6. `MN_GROUPS=3,4` (a 3-way level of PEs 0-2, then a binary level): 6 on PEs 0-2 (4 for the 3-way level + 2), 2 on PE 3 (it only takes part in the second level). Sizes 2, 3, 4 at 1e8 (`mnaccept`): size 2: 2; size 3 (one 3-way level, default schedule `3`): 4; size 4: 4; size 4 with `MN_GROUPS=3`: 6 / 6 / 2. All as the design expects; no difference to explain. The pieces count varies by node (the grid of pieces is not the same on every node).

Guards (read, ECALC_DR unset, 0 or 1): `mn.c` `mn_tree` seeds and sets `g_dr.on` only under `dr_level() >= 2 && mn_size() > 1` (otherwise `memset` of the state, `chk = 0`: the descriptor all-gather stays at 8 words, `use = 0`, the unchecked `rns_mul_dist_mn`); `dr_leaf_check` and `mn_dr_division_check` return at `!g_dr.on` (the latter is also called only under `dr_level() >= 2` in `newton_db.c`); `ecalc.c` registers the leaf poll only under `dr_level() >= 2`; the pass-through `pq_bg_run` adds one atomic store. `rns_dist.c`: `g_chk_on` is 0 outside `rns_mul_dist_mn_chk`, so the three delivery fatals are unchanged and `chk_deliver_inject` is not called; `trk_share`, `mdb_trk_make`, `chk_at_rest` and the residue all-gather run only inside the checked entry. `dbig.c`: `db_mod_qs` passes `parts = 0`. No collective, allocation or pass is added; the gates above with `ECALC_DR` unset are digit-identical.

Review fixes (2026-11-01, commit f750eaa; aac7 item 84_drb_fix, hold 14595, 2 nodes, measured): `ecalc_srun` with `--kill-on-bad-exit=1`; leaf check deferred instead of spinning; `mdb_dr_store_inject` called on every node of a k-way level (the occurrence count agrees on all nodes; a node outside the holders of Qr has an empty share and skips). Results (single runs, measured):

| case | result |
|---|---|
| gates, `ECALC_DR` unset, 2 nodes and 1 node, 1e9 | PASS, digits identical (ecalc total 12.06 s and 8.30 s) |
| `t_verify` new section 4: `vf_pq_range_mod` vs the leaf P, Q (`binsplit_ref`) of 11 term ranges (single terms, [1,3), [100,101), mid ranges, the last terms, the whole range) mod all T1 primes | VERIFY OK, 422 checks. `db_mod_qs` itself needs a GPU; it is checked against the host residue in `t_dbig`, so the chain is covered without a GPU-side leaf test |
| `t_mn_grid` `ECALC_DR=2` at 2 and 4 processes, no injection | VERIFY OK on all nodes, 0 failures |
| `deliver` K=1 PE=0, 2 processes | recovered (1 failure, 1 recovered), VERIFY OK |
| `store` K=1 and K=2, 3 processes; K=1 and K=2, 4 processes; K=1 with `MN_GROUPS=4` | all FATAL rc 3 naming node and APU (for example "node 0 APU 1 (operand B)", "node 3 APU 1 (operand B)"), the job ended 9-14 s after launch |
| ecalc 1e9, 1 node 4 PEs, `MN_GROUPS=4` (the k-way path) `ECALC_DR=2`, no injection | digits identical, VERIFY OK, total 8.56 s; 36 checks per node (28 pieces, 6 products = 2 (nch - 1), 2 ends); leaf checks deferred 0, skipped 0 |
| same, `store` K=2 PE=1; K=1 PE=3; K=3 `MN_GROUPS=3,4` PE=-1 | FATAL naming node 1 APU 0; node 3 APU 0; nodes 0-2 APU 0 (operand A); ended 7.6, 8.6, 7.6 s after launch. K=1 PE=2 injects nothing (occurrence 1 is the running Q of the top child, held by node 3 only: the count agrees now on every node) |
| same, `leaf` PE=2 | FATAL "leaf check failed on pe 2", ended 8.1 s after launch |
| 2 nodes, 1e9 `ECALC_DR=2`, no injection | digits identical, VERIFY OK, total 12.16 s; 28 checks per node (24 pieces, 2 products, 2 ends); leaf checks deferred 0 (the recurrence finishes during the tree at this size, so the deferral path ran in no measured case) |
| 2 nodes, `deliver` K=1 PE=0 | digits identical, VERIFY OK (11.88 s), 1 failure, 1 recovered |
| 2 nodes, `leaf` PE=0; PE=1 | FATAL "leaf check failed on node 0" / "on node 1"; the job ended 12.2 s and 12.7 s after launch (0.5 s after the FATAL line) |

Trace (read only): in `mn_grid`'s `kc > 1` path a corrupted delivery cannot index out of bounds before the residue check: `share_carry_fix` and `db_share_add_one` are bounded by the share length `cn`, and the carry flags only select a +1 on the node's own share; the kc value enters `node_carry_in` as a flag byte only (the byte is then wrong, which can only change values, and the fail bit `chk_flag(4)` is already set). The gather tables (`spill_cols`, `piece_window`) are computed from the geometry, not from delivered data. No guard added.

Checks not done: the division-start compare (`mn_dr_division_check`) had no injection of its own (a `store` corrupts an operand earlier and is found in the next product); the local-only fatals were checked with the srun launcher only.

## Measured, step 4 (aac7, 2026-11-01, 16:00-17:30 ET; items 90_drc_x1 (commit df4caf7), 91_drc_n1 (df4caf7), 92_drc_x2 (e7274f10); all counts measured, single runs)

Code (`ECALC_DR >= 3`): D around `mn_dkm_low` (`newton_db.c` `mn_dkm_low_d`, division steps 1 and 2): E = (R(S) B^e - R(X) R(Q)) - (R(Aw) - R(xql)) must be 0 or +-B^w; a mismatch frees xql, drops the transform cache and recomputes it once; a second failure, or the `ECALC_DR_TOTAL` cap, is fatal with the location. D in the wrap (`mn_dd_wrap`): exact E = 0, fatal on a mismatch (no stored input to recompute from; U faults are caught earlier by D'). D' in `rns_mul_dist_mn_cyc_v`: U == A B modulo B+1 and B-1 (the plane has an even size, so B^2 - 1 divides B^P - 1), one 6-word host all-gather, retry once. Each residue is an all-gather (`mdb_trk_make`) summed alike on every node: the verdict is collective. Cost: 3 gathers per low product, 2 per wrap step plus one for R(R1), 1 per cyclic product. Test injections: `xql` (K = the process's low product number: 1 step 1, 2 step 2), `cyc` (K = cyclic product number); `leafslow` is a modifier in the `ECALC_DR_TEST` comma list (`leaf,leafslow`): the recurrence waits for the division's start (at most 120 s), so the deferred leaf compare runs.

2 nodes, 1e9 (digits against the reference sha, VERIFY OK where rc 0), `ECALC_DR=3`: (before the step-4 review fixes: the division-check counts below are 2 per clean run; with the R(R1) check added they are 3)

| case | result |
|---|---|
| clean | digits identical, 12.1 s; division checks 2 per node (28 checks: 24 pieces, 2 products, 2 ends) |
| `xql` K=1 PE=0; K=2 PE=0, PE=1 | digits identical, 1 injected = 1 failure = 1 recovered (division checks 3) |
| `xql` `_N=2` K=1, K=2 | rc 3, FATAL names the step ("low product X Q mod B^w (...), division X Q mod B^w, group [0, 2), step 1" / "step 2"); the job ended 0.5 s after the FATAL line (12.7 s after launch) |
| `DM_MN_WRAP=24` (one cyclic plane of 2^24), clean | division checks 4 (2 D' + 2 D), digits identical |
| `cyc` K=1 PE=0, K=2 PE=1 (wrap 24) | digits identical, 1 injected = failure = recovered |
| `cyc` `_N=2` K=1, K=2 | rc 3, FATAL "cyclic product #1 ... division X Q mod B^w, group [0, 2), step 1" (and "#2 ... step 2"); ended within 0.5 s of the FATAL line |
| `ECALC_TEST_CORR=7`, `NEWTON_DKM_TEST_HI=-9`, each without and with wrap 24 | no false failures, digits identical |
| `leafslow` alone | rc 0, "leaf checks deferred 1"; `leaf,leafslow` PE=0: FATAL "leaf check failed on node 0" |
| 1.6e10 (`MN_TREE_LOGN_TEST=26`) | clean `ECALC_DR=3`: digits identical, VERIFY OK, total 54.4 s, 84 checks per node, division checks 2; `xql` K=2 PE=0: digits identical, VERIFY OK, 52.3 s, 1 injected = failure = recovered (division checks 3); `ECALC_DR` unset: digits identical, 52.8 s (single runs, not a timing comparison) |

1 node, 4 PEs (`ECALC_PE_PER_GPU=1 COMM_TRANSPORT=xshmem XSHMEM_LOOPBACK_PES=1 ECALC_NP=4`), 1e9, `ECALC_DR=3`: clean; `xql` K=1, K=2 recovered; `xql` `_N=2` fatal; wrap 24 clean; `cyc` recovered; `cyc` `_N=2` fatal (in the df4caf7 build the first cyclic product's message lacked the division text; fixed in e7274f10); `leafslow` rc 0; `leaf,leafslow` PE=2 FATAL "leaf check failed on pe 2", ended 0.5 s after the FATAL line. Digits identical, VERIFY OK where rc 0.

Acceptance: `t_dbig 0` VERIFY OK (705), `t_dr` VERIFY OK (9357 checks); `t_mn_grid 1 28` `ECALC_DR=3` at 2 and 3 processes clean, `flag` and `deliver` at 2 processes recovered (df4caf7); `mnaccept.sh --only mn,corr` `ECALC_DR=3`: 8 PASS (5 mn + 3 corr), identical, all nodes VERIFY OK; `--only mn` with `ECALC_TEST_CORR=7` and with `NEWTON_DKM_TEST_HI=-9`: all PASS; every run showed 0 failures and "division checks 2". Gates with `ECALC_DR` unset on e7274f10 (full rebuild, registry and python checks included): 2 nodes PASS (digits identical, ecalc 12.60 s, P24 pool of 4 primes), 1 node PASS (digits identical, ecalc 7.15 s).

Guards (read; `ECALC_DR` < 3 adds no collective, allocation or pass): `newton_db.c` `mn_dkm_low_d` calls plain `mn_dkm_low` when `!g_dd.on`; `g_dd.on` is set only under `dr_level() >= 3` (`mn_divmod_dkm`); `dd_step`, `dd_res` and `mn_dd_wrap` run only under `g_dd.on`. `rns_dist.c` `rns_mul_dist_mn_cyc_v`: `chk = dr_level() >= 3`; with `chk` 0 the loop runs once with the old body (no `cyc_part`, no `malloc`, no gather), then `*Cm = Cn; return`. `rns_dr_event` and `mdb_dr_inject` are called only from checked paths. `dr.c`: the "division checks" text only at level 3; `leafslow` is read in `pq_bg_run` only under `dr_level() >= 2` and does nothing unless set. `mn.c` `mn_dr_division_seen` is one atomic store at the division start (no collective).

Deviations and notes (step 4):
- The wrap check is exact (E = 0 with the branch actually taken), not +-B^w; it is fatal, not a retry (Ah and T1 are consumed in place).
- D' modulus B-1 is weak for operands divisible by 10^18-1 (Q is, since its small prime factors occur in the product: expected residue 0): it still changes by a nonzero amount for every single-limb error, and the B+1 modulus is not degenerate.
- The D' injection flips a limb after `mdb_norm`, so it does not test the pre-norm path. The persistent case prints the exit summary twice per process (as in steps 2-3).
- `leafslow` is test-only; on a run whose division never starts it would wait up to 120 s.
- Not done in this step: the full regression, ABBA timing, ALGORITHM/TESTING text (steps 5-6, below). The step-1/2 regression subset (flag, deliver at 2 processes) ran on df4caf7, not e7274f10 (the later commit changes only the failure location text).

## Measured, step 5 (aac7, 2026-11-01, fixed commit 9129ac52; all measured, single runs)

Fixes of the review (all `ECALC_DR >= 2` only): the division flag `g_dr_div_seen` is stored before the `!g_dr.on` return; at level 3 a D that cannot be enabled (no `DR_Q` residue) prints one line on rank 0 and counts `DR_C_DIV_OFF` ("D NOT ACTIVE" in the summary); an independent check of R(R1) right after division step 1 (fatal naming the step; clean 2-node runs now show 3 division checks, not 2); `dd_step(1)` runs before the step-1 A mu product; the context is cleared at both ends of the division. Local: unittest 57 OK, switch registry 0 problems, doccheck 0.

| case | result |
|---|---|
| `mnaccept --only unit,e9,mn,grid,corr`, `ECALC_DR=3` (1 node allocation) | 28 passed, 0 failed (1056 s); DR lines: 0 failures, 0 fatal. (A "steps left: 2" note in the item's done line was a leftover count at note time; the runner cancelled the hold.) |
| 1 node, 4 PEs, quick checks | clean passes; `xql` K=1 and `cyc` (wrap 24) recovered; `leafslow` clean (deferred 1, no 120 s stall) |
| 1 node gate, `ECALC_DR` unset | PASS, digits identical (8.61 s) |
| 1 node 4e10, `ECALC_DR=3` | digits identical, no DR lines (DR inactive at size 1) |
| 2 nodes 1e9, `ECALC_DR=3` | clean: digits identical, 12.45 s, division checks 3; `xql` K=1 recovered (1 injected = 1 failure = 1 recovered); wrap-24 `cyc` K=1 recovered |
| 2 nodes gate, `ECALC_DR` unset | PASS, digits identical (12.69 s) |
| path 2 (`ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`, cray build), 2 nodes: gates with `ECALC_DR=3`; `ECALC_DR=3` + `xql` K=1; `ECALC_DR` unset | all three pass, digits identical |
| 2 nodes 1e11, `ECALC_DR=3` (rerun, 19:17-19:35 ET) | rc 0, VERIFY OK on both nodes, first 16 hex of the sha1 of the unpacked ASCII digits = 578f5efb0ff2b9af (the reference, `remote/FACTS.md`); total 266.85 s; per node "DR: 42 checks (pieces 38, products 2, ends 2), 0 injected, 0 failures", division checks 5; the wrap path (`DM_MN_WRAP`) was taken |

The first 1e11 attempt ended "No space left on device" (home file system full; not a code failure). The next attempt hashed the packed part files with sha256 (a method mismatch with the reference, not a digit error: VERIFY OK, "digits == X mod q ok", the e prefix and the stored tail all matched); the rerun used the reference method.

## Measured, step 6 (aac7, 2026-11-01; 2 nodes, 128.82e9 digits, no digit output)

ABBA: one warm-up pair discarded, then 4 rounds A B B A; A = `ECALC_DR` unset, B = `ECALC_DR=3`; all runs rc 0, VERIFY OK.

| quantity | value |
|---|---|
| mean A / mean B (8 runs each) | 121.18 s / 121.66 s |
| B - A over the 4 round differences | +0.48 s, 95 % CI -2.53 ... +3.49 s (t = 3.182), +0.40 % of A |
| B - A over the 8 pairs | +0.48 s, 95 % CI -1.35 ... +2.31 s |
| injected `flag` K=1 PE=0 with `ECALC_DR=3`, one run | 120.43 s; 1 injected = 1 failure = 1 recovered (pieces) |

The cost is not distinguishable from zero at 2 nodes (the CI includes zero and is wider than the modeled 0.1-0.2 %). The injected-retry run is a single run and shows no retry cost above the run-to-run spread. The model figure for 576 nodes (0.1-0.2 %) stays modeled.

M1 soak (aac7 campaign, 2026-11-02, main d7c7f28; measured): DR was active in 16 clean runs of 36 (10 layout P at 1 node 6.6e10, 6 at 2 nodes 1e11; `ECALC_DR=3`, forced remaps, diagnostic words, tiny rounds): 0 fatal, 0 rc != 0, 0 digit differences; the false-positive count (failures without injection) is not known because the harness deletes the logs that hold the `DR:` exit lines (results/STRESS.md §M1).

## Measured, step 6 repeat: paired timing in the aac7 campaign (M6, 2026-11-02 00:33-00:48 ET, main d7c7f28, item 64_m6_dr; 2 nodes, 6.441e10 digits, measured)

ABBA, 6 pairs, A = `ECALC_DR` unset, B = `ECALC_DR=3`, all runs rc 0 and VERIFY OK. Mean A 65.18 s, mean B 64.62 s: **B - A = -0.56 s, 95 % interval [-3.52, +2.40] s** (-0.9 %, interval -5.4 % to +3.7 %). Together with the step-6 table above (+0.48 s, 4 round differences, 128.82e9 digits), the overhead of `ECALC_DR=3` is not distinguishable from zero at 2 nodes at either size; the interval is about 3 s wide, so a cost of 0.1-0.2 % (modeled) is far below what aac7 can resolve, and the 576-node figure stays modeled.
