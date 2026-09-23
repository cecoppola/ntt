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

## 6. New opportunities found by review and research (2026-09-22)

A code review, a literature search and a hardware search produced the following. Each
carries an estimate and a confidence; the first is the largest single opportunity
remaining in the project.

### 6.1 Three primes instead of four — **the big one**

The four-prime engine is inherited from the *binary* base. The requirement is
$n B^2 < \prod p_i$. With $B = 2^{64}$ and $n = 2^{31}$ the coefficients reach $2^{159}$
and three 52-bit primes ($2^{155.4}$) are genuinely too small — hence four. With the
decimal base now in production, $B = 10^{18} < 2^{60}$ and the coefficients reach only
$2^{150.6}$: **three primes fit with a 27x margin**, and the margin holds across the whole
operating range (54x at $2^{30}$, 18x at the $3\cdot2^{30}$ planes we use today, 14x at
$2^{32}$, 7x at the engine's $2^{33}$ maximum).

What it is worth, if the margin is real in practice:

| | today (4 primes) | with 3 | change |
|---|---|---|---|
| transforms per product | 4 forward + 4 inverse | 3 + 3 | **−25 %** |
| plane memory at $4\times10^{10}$ | 180.4 GB | 135.3 GB | **−45 GB** |
| CRT input, per-prime exchange traffic | 4 planes | 3 planes | −25 % |
| per-node digit ceiling at 576 nodes | $6.7\times10^{10}$ | $\approx 7.7\times10^{10}$ | machine $3.9 \to 4.4\times10^{13}$ |

Independent support: y-cruncher's NTT uses "anywhere from 3 to 9 primes" depending on
size, so three is a normal design point, not an edge case.

*Why the 25 % is real and not cancelled by the node's structure.* The four-prime choice
also maps one prime to each APU, which is what `rns_mul_mdev` does — and there the fourth
prime is free, because the four run in parallel. But that tier is not on the critical
path. In the two tiers that are, the primes run **sequentially**: the batch tier gives
each APU its own *products* (subtree ownership, for the 40x locality cliff) and runs all
four primes on them one after another; the distributed tier puts all four APUs on **one
prime at a time**, because a single prime's plane already needs all four memories. So in
58.6 s of the 58.8 s of GPU phases the fourth prime costs a full 25 % of wall clock.
Prime-per-APU buys *latency*, not throughput, and the batch tier always has many
independent products — which is why locality won there, correctly.

What it costs: `EC_NP = 4` is baked into the prime tables, the CRT (a 4-word
reconstruction window becomes 3), and — the one real obstacle — the `mdev` tier's
one-prime-per-device mapping, which assumes primes and APUs are equinumerous. Three
primes over four APUs needs a different assignment there (or that tier keeps four).
**Effort 2–3 d; expect −6…−10 s of the 58.8 s of GPU phases and −45 GB.** Gate: a
hard assertion at init that $nB^2 < \prod p_i$ for the chosen $n$, `t_crt` and `t_mul`
extended to three primes, then $10^9$ digits byte-identical.

### 6.2 More transform lengths: $5\cdot2^k$ and $7\cdot2^k$
We support $2^k$ and $3\cdot2^k$, which leaves an average padding waste of about 15 %.
y-cruncher supports $2^k$, $3\cdot2^k$, $5\cdot2^k$ and $7\cdot2^k$; adding the last two
brings the average waste to about 6 %. The primes already admit the radix-3 factor; a
radix-5 and radix-7 stage would be needed, on the model of `ntt3.c`.
**Effort 2 d; expect −3…−5 % of transform time.** Confidence: medium-high.

### 6.3 Middle and short products in the reciprocal
The Newton iteration computes `rns_mul(t1, qt, r)` — a full $2j \times j$ product — and
then uses only the middle $j+1$ limbs; the correction computes a full $j \times j$
product and keeps the top half. The classical remedy is the **middle product** (a cyclic
convolution of length $2j$ instead of an acyclic one of length $3j$) and a **short/high
product** for the correction; both are standard in Newton-based division.
**Effort 2–3 d; expect −25…−35 % of the reciprocal** — that is −3…−5 s at
$4\times10^{10}$ and −13…−18 s at $10^{11}$, where the reciprocal is 52.5 s of 263 s.
Confidence: medium — the saving is clear for schoolbook and Karatsuba, and for
transform-based products it comes from the shorter cyclic convolution, which must be
measured rather than assumed.

### 6.4 Compute the seed spans on the GPU
During initialization the GPUs are idle (they are being mapped) while the seed thread
spends 21 s of the 22 s doing schoolbook arithmetic on the CPU. Two measurements make
this attractive: the CPU stores into device memory at 2.6 GB/s against 30 GB/s into host
memory (so the CPU is a poor writer of the arenas), and the spans are embarrassingly
parallel — 17 M independent spans of 256 terms. Moving the span computation into a kernel
removes both the arithmetic and the slow stores, at the cost of ordering it against the
pool mapping (pool 0 first, seeds into it, regions after).
**Effort 2 d; expect init 22 → 13–15 s, i.e. −7…−9 s of the wall.** Confidence: medium —
the ordering against `hipMalloc` is the risk, and I's measurements show that HIP calls
from a second thread queue behind the main thread's allocations.

### 6.5 Two endpoints per APU — the doubled NICs
The target gives each APU **two** 400 Gb/s NICs; the SHMEM transport creates **one
context per APU thread**, which binds to one NIC. The standard practice on Slingshot is
one endpoint per physical NIC with GPU-to-NIC affinity, and striping across them.
Without this the node injects at about 200 GB/s of its 400.
**Effort 1–2 d; expect up to 2x the per-node injection bandwidth**, which matters for the
13–16 % of the distributed tier that is exposed and for the operand redistributions.
Confidence: high on the mechanism, unverifiable until the target.

### 6.6 Investigate CPX mode for the batch tier
MI300A runs SPX (all six XCDs as one partition) by default; CPX exposes each XCD
separately. The batch tier's products are independent and subtree-owned, so CPX might
improve cache locality and scheduling; the distributed tier, which wants one large
partition per APU, would not. Since the mode is set at boot on this machine it is a
question for the target's administrators, not a code change.
**Effort: one measurement if a CPX-mode node can be obtained.** Confidence: low —
speculative, but cheap to test and it costs nothing to ask.

### 6.7 MALL (Infinity Cache) awareness in the transform tiles
MI300A has a 256 MB memory-attached last-level cache. The transform's tile sizes were
tuned against LDS and HBM, not against the MALL. Sizing a pass's working set to stay
resident in 256 MB (per APU: the plane is far larger, but a *tile column* need not be)
could lift the 1.0–1.4 TB/s the kernels achieve against a 3.0 TB/s copy ceiling.
**Effort 1–2 d of experiment; expect 0–10 %.** Confidence: low-medium.

### 6.8 Truncated Fourier transform (recorded, not recommended yet)
The TFT computes exactly the $n$ coefficients needed instead of rounding to the next
admissible length, removing the padding waste entirely. It subsumes 6.2 but is a
substantial rewrite of the transform and interacts awkwardly with the four-step
factorization and the distributed exchange. Recorded for completeness; 6.2 captures most
of the benefit for a fraction of the work.

### 6.9 Prime-per-APU for the reciprocal's fitting doublings — the unexploited K4 case
The node's K4 topology pays where a product is **latency-bound and fits one APU's plane**:
each APU takes one prime and does full-length transforms locally, with **no exchange at
all**. Derived from the measured constants (a $2^{31}$ convolution = 1.11 s, one all-to-all
= 0.158 s), for one product of $n = 2^{31}$ points over four primes:

| strategy | wall | exchanges | plane per APU |
|---|---|---|---|
| prime-per-APU | **2.54 s** | 0 | 51.6 GB |
| four-step | 4.44 s | 12 | 12.9 GB |

Prime-per-APU is **1.75x faster for 4x the plane memory** — so it fits only below about
$2^{30}$ points on today's 45 GB per-APU budget. The reciprocal's doublings are a
*dependent chain* (no batch to fill the machine with), and the ones below that size
currently go through the four-step and pay 12 exchanges each. **Effort 2 d; expect ≈ 1 s
at $4\times10^{10}$ and ≈ 4 s at $10^{11}$** (those doublings carry ~16 % of the
reciprocal's work). Composes with 6.1: in this regime the prime count does not change the
wall at all (each APU does its own prime's three transforms in parallel), so three primes
costs nothing here and saves 25 % everywhere else.

**The three-regime rule this implies** — the optimal placement of one product on four APUs:

| regime | condition | strategy | status |
|---|---|---|---|
| A | many independent products, each fits a plane | product-per-APU, all primes local | the batch tier |
| B | single product in a dependent chain, fits the per-APU budget | **prime-per-APU** | **the gap (6.9)** |
| C | single product too large for one plane | four-step | top levels, reciprocal, division |

Note for the record: the K4 structure gives **faster runtime, not less memory**. Memory
efficiency comes from splitting one plane across four APUs — which is exactly what forces
the exchange. Prime-per-APU is the memory-hungry, communication-free extreme.

### Suggested priority among these
**6.1** (largest, and it improves time *and* memory *and* the machine ceiling),
then **6.4** (largest single-node wall item after the seeds work already in §2),
then **6.3**, then **6.5** before any target campaign, then **6.2**; treat 6.6–6.8 as
experiments.

---

## 7. The design-space campaign — PLAN §29 (Phase 13)

The items in §6 interact: the prime count, the distribution strategy, the plane cap and
the transform-length set cannot be chosen independently, and several hardware features
have never been measured on the paths that matter. PLAN §29 is the campaign that settles
them by measurement, with the decision rule fixed in advance — **digits per node-second
subject to fitting the node**, Pareto table of (wall, node memory), ties broken by the
modelled 576-node ceiling.

| # | experiment | decides |
|---|---|---|
| E0 | `t_strategy`: one product under all three distributions at $2^{26}$–$2^{31}$, $P$ = 3 and 4 | where regime B begins; whether the grid cap should drop (decides E4, E5 before either is written) |
| E1 | `t_primes`: 3-prime CRT and convolution against GMP to $2^{33}$, worst-case limbs | that the margin is real |
| E2 | `t_cap`: transform points per cap for the shapes actually formed (no node time) | what E0's answer would cost |
| E3 | three primes end to end | −25 % transform work, planes 180.4 → 135.3 GB |
| E4 | prime-per-APU for the reciprocal's fitting doublings (6.9) | ≈ 1 s at 4e10, ≈ 4 s at 1e11 |
| E5 | **the exchange-free grid**: cap set so every piece fits prime-per-APU | whether the all-to-all can leave the node entirely |
| E6 | $5\cdot2^k$, $7\cdot2^k$ lengths | −3…−5 % of transform time |
| E7 | middle and short products in the reciprocal | −25…−35 % of the reciprocal |
| E8 | seeds as a kernel | init 22 → 13–15 s |
| **E9** | **xGMI and the fabric at once** — measure the present overlap, then deepen the pipeline, then xGMI as a relief valve for NIC imbalance | ceiling **27 % of exchange time** at 576; part 1 needs only two real nodes |
| H1 | CPX vs SPX partitioning | needs an administrator |
| H2 | the MALL (256 MB): find the cliff, size tiles under it | kernels run 1.0–1.4 of 3.0 TB/s |
| H3 | modmul engine on the *full* transform (FP64 Barrett vs Shoup vs reduced-correction) | only ever compared on the first pass |
| H4 | xGMI concurrency: does the push saturate all three links | |
| H5 | SDMA offload of the $X$ fetches and checkpoint writes | frees CUs during the division |
| H6 | occupancy and launch configuration, revisited under H2 | |
| H7 | non-temporal stores in pack/unpack | they pollute the cache the transform wants |
| H8 | two endpoints per APU for the doubled NICs | built now, measured on the target |

Order: E1, E2 (no node time) → E0 → H2, H3, H4, H7, **E9 part 1** (kernel- and
link-level constants, before any pipeline work) → E3 → E4, E5 → E6, E7, E8 → H1, H5, H6
→ H8 on the target. Full statement, gates and discipline in PLAN §29.

---

## Suggested order

1. **1.1, 1.7, 4.1** — cheap, and two of them are traps that would bite at scale.
2. **2.1 + 2.2** — the largest single-node item left (≈ −4 s), and they unblock each other.
3. **1.2, 1.3, 1.4, 2.4** — the remaining g-dependent memory terms, before any large run.
4. **3.1–3.4** on the target, in that order; **1.5** opportunistically before then.
5. **6.1** — three primes: the largest remaining opportunity in the project, and it
   moves time, memory and the machine ceiling together.
6. **6.4, 6.3** — the seeds on the GPU, then the middle product in the reciprocal.
7. **6.5** before any target campaign; **6.2** when convenient.
   All of §6 is settled by the **PLAN §29 campaign** (§7 above), which should run as one
   multi-agent session: E1/E2/E0 and the H-series parallelise across disjoint files.
8. **4.3, 4.4** — document hygiene, one allocation.
9. **PLAN §28** — the code reduction, last.
