# DR — detect and retry (C3): a faulty product repeats itself instead of ending the run

**Question.** Target runs failed with CRT flags (a wrong transform-domain value, kind 1) and T1/VERIFY failures (wrong in-range limbs, kind 2), more often the larger the run. User decision Q142: full detect-and-retry. Can the product tiers detect a transient fault inside the product, before it reaches C, and repeat only the faulty piece, with no extra memory and no change of digits?

**Change / branch.** Branch `c3check` (from main c86e5fcc). Switch `ECALC_DR` (0 = off, default, the unchanged code paths; 1 = A; 2 = A + B + C; 3 = A + B + C + D + D'), `ECALC_DR_TOTAL` (default 8), fault injection `ECALC_DR_TEST` (+ `_K`, `_PE`, `_N`). Registry rows with review dates; `ecalc/README.md` generated table. Files: `ecalc/dr.c/h` (switches, injection parser, counters, context, residue helpers), `ecalc/tests/t_dr.c`, `ecalc/rns_dist.c` (`mn_core`).

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
| 1 | `dr.c/h`, `t_dr`, registry, this file | see RESUME | | |
| 2 | A in `mn_core` (flag retry, fatal-to-flag conversions, injection `flag`) | see RESUME | | |
| 3 | B + C (tree residue identity) | not started | | |
| 4 | D + D' (division, cyclic) | not started | | |
| 5 | full regression with `ECALC_DR=3` | not started | | |
| 6 | timing (ABBA), docs (ALGORITHM §10, TESTING §4, TASKS) | not started | | |

## Deviations from the design

(none yet)

## Open issues

(see RESUME)

## RESUME

Branch `c3check`; steps 1 and 2 by agent DR-A.
