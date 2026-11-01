# DR — detect and retry (C3): a faulty product repeats itself instead of ending the run

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

Cost model (modelled, 3.8e13 digits, 576 nodes): A 0.05-0.2 s, B about 60 ms, C about 5 ms, D about 12 ms, D' 0.1-0.15 s; total 0.25-0.4 s = 0.1-0.2 % of a 200 s run.

## Counting rule (step 2)

`failures` counts the PEs that failed the check of a piece, group-wide, so it is the same on every process; `injected` is local (the injections done by this process's threads); `recovered` counts those PEs once the repeat passes; the gate sums `injected` over the processes and compares: injected = failures = recovered. `checks` counts piece decisions (one `grp_max` each).

## Steps

| step | content | status | commit | gates |
|---|---|---|---|---|
| 1 | `dr.c/h`, `t_dr`, registry, this file | done | 4af77c91 | t_dr VERIFY OK (9351 checks, measured); 1-node and 2-node gates with `ECALC_DR` unset PASS, digits identical (measured) |
| 2 | A in `mn_core` (flag retry, fatal-to-flag conversions, injection `flag`) | done | 14926757 | see Measured, step 2 |
| 3 | B + C (tree residue identity) | not started | | |
| 4 | D + D' (division, cyclic) | not started | | |
| 5 | full regression with `ECALC_DR=3` | not started | | |
| 6 | timing (ABBA), docs (ALGORITHM §10, TESTING §4, TASKS) | not started | | |

## Deviations from the design

- Step 2 injection positions: `P24_CHECK_TEST_S` (the existing 0|6|12 row-class switch) positions the `flag` injection; no new switch.
- The `_PE` of `flag` is the global PE number (rho); `-1` injects on every PE of every process.
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

Branch `c3check`; steps 1 and 2 done and gated (above). Next: step 3 (B + C: `rns_mul_dist_mn_chk`, tracked residues in `mn.c`), design section 6.

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

