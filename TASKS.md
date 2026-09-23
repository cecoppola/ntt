# Outstanding tasks and opportunities

Consolidated from PLAN §23, §26–§28, DECISIONS3.md, CODE_REDUCTION.md and the open-issue
sections of `results/{R,G12,I,S12,Q,W,M11}.md`. State: `main` @ a73fb1d; one node computes
4 × 10¹⁰ digits in 80.7 ± 1.2 s and 10¹¹ digits in 263 s; the regression is 21/21; the
576-node estimate is ≈ 3.9 × 10¹³ digits in ≈ 4.0 min (modelled).

Nothing here is a known defect. The one open correctness item from earlier phases — the
leaf-transition race — was closed in Phase 12 with a named cause.

---

## 1. Correctness and robustness

| # | item | why | effort |
|---|---|---|---|
| 1.1 | **The two memory models disagree** at 576 nodes: `mem_model.py` says 6.7 × 10¹⁰ digits/node, the tree's own C accounting says 7.1 × 10¹⁰ | a run planned on the wrong one fails at init on 576 nodes simultaneously; 4 × 10⁹ digits/node of difference | 0.5 d |
| 1.2 | **`mdb_shift` exchange scratch** — 71 GB per node at 8 × 10¹⁰, the last term above the single-node profile at 576 | the g-dependence the gridded tree otherwise removed | 1 d |
| 1.3 | **The window temporary `T`** of an accumulating grid piece is O(share): 35 GB per node at 8 × 10¹⁰ | largest remaining O(share) term in the tree's scratch; a rounds form bounds it to a constant | 1 d |
| 1.4 | **Top tree set written synchronously at size > 1** (35 GB / size per node); the size-1 path writes it in the background | only tested at 10⁸/2; at scale it is on the critical path | 0.5 d |
| 1.5 | **Two real nodes over SHMEM** at 10⁹ | never had two idle nodes at once; the only transport path never exercised across a real fabric | opportunistic |
| 1.6 | **`DIST_LOGN_TEST` below the division's own need** fails "65 corrections" — *pre-existing, also on the binary pipeline* | a forced-grid test configuration, not a default; worth understanding before it is hit for real | 0.5 d |
| 1.7 | Tree checkpoint sets are indexed by the schedule's level number: **a restart must use the same `MN_GROUPS`** | undocumented trap; add the check and the error message | 1 h |

## 2. Performance — single node

The floor has moved: initialization is 22 s of which the seed thread is 21 s, so **the
seeds are now the critical path**, where driver page-mapping used to be.

| # | item | evidence | expected |
|---|---|---|---|
| 2.1 | **Overlap the seeds with the plane mapping** (planes are mapped first; the seed thread currently waits on the region arenas) | I's measurement: moving the seeds out of the window in either direction costs 7 s; overlapping them further has not been tried | −2…−4 s |
| 2.2 | **The decimal `mul_1` itself** (two limbs per step, or a Montgomery-style reduction) | 2.6 ns/limb, 8.5 s of the seed thread's 14 s of CPU work | −2…−3 s, and it uncaps 2.1 |
| 2.3 | **The reciprocal at 10¹¹ digits**: 52.5 s of the 121 s dm phase, dominated by piece loading | measured this session on the 10¹¹ run | −10 s at 10¹¹, ~0 at 4 × 10¹⁰ |
| 2.4 | **`rns_dist`'s slabs still staged** in `ecalc` | S left a one-line change (`comm_sym_alloc` for `sl`/`tmp`) | removes the last staging term at scale |
| 2.5 | Seeds in host-memory regions (CPU stores at 30 GB/s against 2.6) | would make the seed stores nearly free, but any `hipMemcpy` on that memory drops to 21 GB/s | untried; probably a loss |
| 2.6 | Pairwise (not Horner) combine inside a k-way tree level | Q: ≈ 25 % fewer plane points at a 9-way step; the default 3·3 schedule has no step above 3-way | ~0 for the default schedule |

## 3. Scaling and target readiness

| # | item | note |
|---|---|---|
| 3.1 | **Measure the two fabric assumptions first** on the target: SHMEM per-message cost (2 µs assumed; at 20 µs the 576-node wall goes 4.0 → 4.3 min) and part-file bandwidth (2 GB/s assumed) — then re-run `estimate.py` | 1 h; converts the estimate into a plan |
| 3.2 | **Bring up the SHMEM forms in order** per `docs/TARGET.md` (2 → 4 → 64 → 576 nodes), enabling thread-multiple contexts, device heap and pool-resident slabs at each step | the runbook exists for exactly this |
| 3.3 | **Decide the top schedule by measurement** (3·3 default vs 9-way; the model says 3·3 by 5 %) | one environment variable |
| 3.4 | **Run safe size before ceiling**: 6.1 × 10¹⁰ digits/node (3.5 × 10¹³ total) before 6.7 × 10¹⁰ (3.9 × 10¹³) | at the ceiling one node's allocation failure ends the run |
| 3.5 | The general map's exchange overlap (`GEN_HIDE` = ½) is an assumption, and at 576 **every** full-group product uses the general map | 20–30 % of the modelled exposed time rests on it |
| 3.6 | The transform cache over shares allocates 16 GiB/slot/APU by `hipMalloc`, not from the block pool and not sized to the group | works, but is not what the design says; tidy when convenient |
| 3.7 | rocSHMEM's host-API put-with-signal unverified (one switch if absent) | target-dependent |

## 4. Verification and documentation

| # | item |
|---|---|
| 4.1 | **Checkpoint write vs the division**: release Q after the output stage, then size the decision from the measured disk rate (at 0.31 GB/s the 35.6 GB set costs 113 s the division waits for; at ≥ 1 GB/s it is free) |
| 4.2 | The recheck's cold-disk floor (≈ 50–70 s at 4 × 10¹⁰) is not separately measured from the set's read |
| 4.3 | Four cells of the complexity study's ladder are inferred, not measured: GPU phase time at rungs 2, 4, 5, 8 and device peak before rung 6 — three short runs at tagged commits, one allocation |
| 4.4 | Decide the GMP baseline at 4 × 10¹⁰: state that it does not fit (recommended) or spend ~6 h proving it |
| 4.5 | `<outfile>.top` (35.6 GB at 4 × 10¹⁰, ≈ 20 TB at the target) is left on disk for the operator — the runbook says to delete it after the recheck |

## 5. Code reduction — PLAN §28, scheduled after everything above

Per-item reasoning in `CODE_REDUCTION.md`. Approved groups: (a) archive, (b) remove,
(e) the binary pipeline. Group (c) (merging) is **not** scheduled; group (d) is retention.

| step | content | core lines |
|---|---|---|
| 28.1 | **(a)** archive the instruments (`ECALC_RES_LOG`+, `LEAF_DUMP`, `COPY_PROBE`, `B_SNAPSHOT`, `MEM_DPOOL_FILL`, `MEM_COPY_NOWAIT`, `DBIG_SERIAL`, `DBIG_WARM`, `MEM_NO_DEV_MEMSET`, five trace switches); `t_alloc` and `t_copy_order` stay in `tests/` | −320 |
| 28.2 | **(b1)** engine 2: `ntt2.c`, `ntt2.h`, `modarith2.h`, `crt2.c`, 5 dispatch sites | −757 |
| 28.3 | **(b2)** `newton.c`, the host-flow stand-ins, the rejected layout/placement switches, `MEM_ALLOC`'s losing forms, the DPP body, legacy flow guards | −646 |
| 28.4 | **(e0)** flip the library default to decimal and run the full suite — the measurement that decides whether the binary pipeline still has anything to say | 0 |
| 28.5 | **(e)** archive the binary pipeline: `todec.c`/`.h`, the bit operations, ~45 branch sites, `t_dec.c`; **rewrite `t_dbig`, `t_mul`, `t_crt`, `t_newton`, `t_mn_grid` to decimal**, each re-validated against GMP | −446 (+86 test) |
| 28.6 | README switch list regenerated, `archive/MANIFEST.md`, both papers' LOC figures, and the paper's §9 amended (it loses the independent-pipeline claim) | — |

**Totals**: core 13 016 → **≈ 10 850 (−2 166, 17 %)**; switches 116 → ≈ 55.
Gate at every step: `mnaccept.sh --full --stress` green either side, plus a five-run
4 × 10¹⁰ series after 28.3 and 28.5.

### Not scheduled — group (c), merging (≈ 130 lines)
- **Piece-selection policy** (~60): `split_grid_cap` vs `mul_karatsuba`/`mul_chunked` — two
  policies for one decision; merge the policy, keep the two execution paths. Low risk.
- **Grid execution** (~70): `mul_grid` (dbig) vs `mn_grid` (mdb) share the loop shape;
  factor the piece iteration and cut predicates. Medium risk — live at every size.

### Retained deliberately — group (d)
Three big-integer layers (different carry topologies, already factored through one core),
six product tiers (all reachable, each bound by a different resource), five communicator
implementations (each earns its place), `ntt3.c` (adopted), single-node vs sharded
reciprocal, `batch_local` vs striped-pair. Reasoning in `CODE_REDUCTION.md` §(d).

---

## Suggested order

1. **1.1, 1.7, 4.1** — cheap, and two of them are traps that would bite at scale.
2. **2.1 + 2.2** — the largest single-node item left (≈ −4 s), and they unblock each other.
3. **1.2, 1.3, 1.4, 2.4** — the remaining g-dependent memory terms, before any large run.
4. **3.1–3.4** on the target, in that order; **1.5** opportunistically before then.
5. **4.3, 4.4** — document hygiene, one allocation.
6. **PLAN §28** — the code reduction, last.
