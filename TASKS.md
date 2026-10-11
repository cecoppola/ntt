# Outstanding tasks and opportunities

> **HISTORICAL.** Covers Phase 12–13 (through 2026-09-25), before Phases 14–17 and the target work closed or
> superseded most of these items. Superseded by `docs/code/06_EVALUATION.md` §4 (current ranked recommendations) for
> aac6/aac7 work and `docs/TARGET_TASKS.md` for target-only work. Kept below as the historical task log; the body
> is not edited.

Consolidated from PLAN §23, §26–§28, archive/docs/DECISIONS3.md, archive/docs/CODE_REDUCTION.md and the open-issue
sections of `results/{R,G12,I,S12,Q,W,M11}.md`. State (Phase 12, superseded by the status section below): `main` @ a73fb1d; one node computes
4 × 10¹⁰ digits in 80.7 ± 1.2 s and 10¹¹ digits in 263 s; the regression is 21/21; the
576-node estimate is ≈ 3.9 × 10¹³ digits in ≈ 4.0 min (modelled).

## The work plan after Phase 13 (2026-09-23) — read this first

State: `main` @ 73b6c85. The chosen design is the default (three primes, `NTT_MODMUL=1`, `RNS_STRATEGY=auto`,
`ECALC_PLANE_CAP=2^31`, `MDB_SHIFT_CHUNK_MB=1024`, `COMM_ALLTOALLV_DEPTH=2`, `NTT_B1R=3`, `NTT_PLAN=1`). One node: 4 × 10¹⁰
digits in 63.5 ± 1.5 s; the target's top-node share, 7.64 × 10¹⁰, in 133.3 s at 354 GB. **Target: 4.25 × 10¹³ digits on
576 nodes, ≈ 3.9 min, 452 GB per node (modelled)**, below both grid steps (RESULTS §78–§82). Regression 21/21.

This section supersedes the status tables below, which stay as the record. Every item names where its evidence is.
The order puts first what the target run cannot succeed without, then per-node speed (≈ 80 % of the modelled 576-node
wall is per-node compute), then verification and documents, then code reduction. Target-machine work is in
`docs/TARGET_TASKS.md` (T0–T9, another agent); it runs whenever target access comes, but T4 (the headline run) waits on
Phase A.

### Phase A — blockers for the target run (do first) — **DONE 2026-09-25 (RESULTS §84)**: A1 race fixed (hang unproven), A2 pool law (the target needs MN_T_CHUNK_MB=1024 to fit), A3 clean exits, A4 real-node grid + mnrun detection, A5 RNS_PLANES_FIRST

| # | item | why first | evidence | size |
|---|---|---|---|---|
| A1 | **The rare hang after init: find and fix the cause.** Soak runs at the target's share size (7.64 × 10¹⁰) and at small sizes (fast repeats, e.g. 10⁹ × 200) with stacks captured by `archive/drivers/ecalc/g13d_hang.sh` (gdb launch mode; `ptrace_scope` blocks attaching); then fix; then a soak with zero hangs | 1 hang in 31 one-node runs. A 576-node job runs 576 processes: at 1/31 each it almost never finishes; even at 1/3000 it fails 17 % of the time. A watchdog does not rescue a 576-node run | G13d §0 (197 threads in futex, 2 in `kfd_wait_on_events`, just after init — the seed thread's join) | 1–2 sessions |
| A2 | **The SHMEM pool: model it, size it, fix the staging** — measure the pool's high-water at 2 real nodes and 4 processes at 10⁸–10¹⁰, fit its growth, put it in `mem_model`/`estimate.py`, re-check the 4.25 × 10¹³ node total; include 2.4 (`rns_dist`'s slabs still staged: the one-line `comm_sym_alloc` change) | 8479 MiB in use at 10¹⁰ on 2 nodes, above the 8192 default; the model's column is flat. Decides whether 452 GB holds (T0 on the target) | S13d open 3; TARGET_TASKS T0 | ½ session |
| A3 | **Clean failure from worker threads**: the in-phase pool guard calls `exit(1)` from four worker threads at once and the process segfaults; route it through one error path with a clear message and exit code | at 576 nodes a clean, attributable failure matters | G13d (c) | small |
| A4 | **`t_mn_grid` on real nodes** (SOS, 2 and 3 nodes, small size) and **`mnrun.sh`'s SHMEM detection** under wrappers (`stdbuf`, `timeout`, `numactl`) | the any-size map is the target's path; only loopback-verified | S13d open 2, 5 | small |
| A5 | **The dist tier's operand load: 10 × slower per call on s24-16 than on s24-30** (0.89 against 0.094 s) — find whether it is the node or the memory state | if it is memory state, it is a large speedup available everywhere, and a hazard at scale (it made a 1.42 × 10¹¹ run 2 × slower) | G13d (b), P13b | ½ session |

### Phase B — per-node speed and memory (in payoff order at the target's share, 133 s = init 25 + tree 53 + dm 56)

| # | item | expected | evidence |
|---|---|---|---|
| B1 | **Initialization** (20–27 s, the most variable phase): 2.1 overlap the seeds with the plane mapping; 2.2 the decimal `mul_1`; E8 / 6.4 the seeds as a GPU kernel | init 22 → 13–15 s: −7…−10 % of the per-node wall | TASKS 2.1, 2.2, 6.4 |
| B2 | **The reciprocal**: E7 / 6.3 middle and short products; 2.3 piece loading at large sizes (with A5) | −25…−35 % of the reciprocal (22 s at the share) | TASKS 6.3, 2.3 |
| B3 | **`auto`'s grid ignores the per-piece cost** (≈ 0.08 s per piece per 2³¹ limbs, D213d): add it to the choice; with it, the 1.245 × 10¹¹ fragmentation OOM (largest free block 27.50 GB for 27.67 GB) and the small-piece grids above 10¹¹ | one node above 10¹¹: dm 162–171 s against C's 90 s; also moves steps | D213d, G13d (a) |
| B4 | **Transform lengths 5·2ᵏ and 7·2ᵏ** (E6 / 6.2) | −3…−5 % of transform time; finer lengths soften the grid steps | TASKS 6.2 |
| B5 | **Kernels**: the 2³¹ plan's 5-stage top pass (≈ 4 ms per transform); the stride penalty's cause (s_lo 17 / 24, ≈ 12 % of a 2³¹ transform); `ntt3.c` on the reduced-correction modmul; the general-map twiddle packs in `rns_dist.c` (`k_twpack_g`, `k_unpacktw_g`); `DIST_TWREC` measured multi-node | a few % each | K13b, K13, X13b |
| B6 | **Tier balance at three primes**: APU 3 idles in the mdev and striped batch tiers (≤ 0.3 s at 4 × 10¹⁰); B4's transfer as a push overlapped with the transform; B at size > 1 (a cross-node broadcast) | small; B at size > 1 is a design question | P3, B13b |
| B7 | **Hardware**: H1 CPX mode for the batch tier (needs an administrator); H5 SDMA for the X fetches and the checkpoint writes | unknown | TASKS 6.6, PLAN §29 |

Recorded and not recommended: 2.5 (seeds in host memory: `hipMemcpy` drops to 21 GB/s), 2.6 (pairwise combine: ~0
for the default schedule), 6.8 (truncated FFT). Closed by measurement: 6.7 / H2 (no MALL cliff), H6, H7.

### Phase C — the model, verification, documents

| # | item | evidence |
|---|---|---|
| C1 | **The model**: the size-1 tree-top re-grids it predicts mostly do not appear (only S4); one size-4 step 1.7–3.8 % late; `cap_factor` at the small caps unexplained; the host term at size > 1 (3 GB above the model over TCP); the size-1 leaf products (`plan leaf`) not validated; the r·d operand's size estimate | G13d (a), (c); D13b; M13 open 1; L13d |
| C2 | **Checkpoints at size > 1 at scale** (the background top set tested only at 10⁸–10⁹; old v2 tree sets restart with a warning); 4.2 the recheck's cold-disk floor | N13, TASKS 4.2 |
| C3 | **The papers** (`~/xetex/digits_as_limbs.tex`, `digit_cost.tex`): three primes, the design table, the target 4.25 × 10¹³, the term-share steps, `MN_PLAN_ONLY`, SHMEM on real nodes; 4.3 (four inferred cells of the complexity ladder: three short runs at tagged commits) | RESULTS §78–§82 |
| C4 | **Documents in step**: TARGET.md §6 item 2 (depth 2 now measured: 74 % at 2 real nodes, 73–78 % at 3), TASKS §3 rows closed by Phase 13 (3.4 superseded, 3.5 measured), the README switch table against the source | — |

### Phase D — code reduction (PLAN §28, last)

| # | item |
|---|---|
| D1 | archive the ≈ 320 lines of instruments; remove the ≈ 1,400 lines of obsolete code (engine 2, host Newton, stand-ins, rejected layouts); **archive the binary pipeline** (the user's request) and rewrite what depends on it — ≈ 2,166 core lines |
| D2 | the Phase 13 switches the defaults superseded, **the user's decision per switch**: forced `B`, `B4`, `ECALC_PLANE_CAP=off`, `COMM_ALLTOALLV_DEPTH=1`, `NTT_MODMUL=0/2`, `NTT_MALL`, `NTT_B16_VAR`, the non-temporal pack, `DIST_TWREC`, `DIST_TPACK`, `NTT_B1R=0/4`, `NTT_PLAN=0` |
| D3 | the regression and a five-run series after each step (the reduction must not move a digit or a second) |

### Decisions for the user

1. The GMP baseline at 4 × 10¹⁰ (TASKS 4.4): state that it does not fit (recommended) or spend ≈ 6 h proving it.
2. D2: which superseded switches to delete.
3. H1: whether to ask the administrator for a CPX-mode node.

### Suggested sessions

1. **A1 + A2 + A3 + A4** (the hang soak and fix, the pool, the clean exit, `t_mn_grid`): one multi-agent session; A1
   and A2 need nodes, A3 and A4 are small.
2. **A5 + B1 + B2** (the load speed, initialization, the reciprocal): the largest per-node gains.
3. **B3 + B4 + B5** (auto's cost, new lengths, kernels), then **C1** (refit the model to the faster code) and
   `design_table.py` regenerated, so the target estimate is restated.
4. **C2–C4** (checkpoints at scale, papers, documents).
5. **D1–D3** (code reduction), then a final regression and series.

The target agent's T0–T9 can start any time; its T4 (the 4.25 × 10¹³ run) waits for session 1.

---

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

Per-item reasoning in `archive/docs/CODE_REDUCTION.md`. Approved groups: (a) archive, (b) remove,
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
reciprocal, `batch_local` vs striped-pair. Reasoning in `archive/docs/CODE_REDUCTION.md` §(d).


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

---

## Idea sweep of past sessions (2026-10-07)

Two bounded sweeps of past transcripts (the main session and its subagents, and six
home-directory sessions) produced the ideas below. Only ideas **not** already in §§1–7,
PLAN, TARGET_TASKS, TARGET_WISHLIST, the decision register or S19B §5 are listed. Ideas
that were already listed or rejected are not repeated here. Confidence in the sweep is
low to medium: the search used greps and targeted reads, not full reads. Labels: measured
(m), modelled (mod), assumed (assumed).

| # | idea | what | claimed saving (label) | source | effort | risk | overlap / where it goes |
|---|---|---|---|---|---|---|---|
| S-1 | **Release v-slots after each distributed tree level** | The general-map v-exchange slots stay resident for the whole tree. Free each level's slots once its exchange is consumed (lifetime and order must keep digits identical) | −11.5 GB per node device at 576 nodes (mod, from a transcript table); not quantified at 10 nodes. Probably smaller once DC8 is in, since DC8 halves the slot size | 2026-10-06/07 (docs session, "Find the opportunities") | M | medium: needs a switch and a digits check | Overlaps **DIST_CHUNKS=8** (now default, commit 0adf205; S19B §5 #5, −13.4 GB per node, and ME26). Overlaps the tree-level tight reservations in **PLAN L1 / E2** (−51.7 GB mod at 10¹¹, 50 GB m). Size it after DC8 is measured at 10 nodes |
| S-2 | **Division-specific overlap** (not only the reciprocal) — **SAVED as a target-gated option (user 2026-10-08, TARGET_TASKS T12)** | The division waits entirely on exchanges: 232 of 432 s at 10 nodes. S19B §5 #4 covers only the reciprocal's wait; §5 #2 covers general "other" time. The division's own overlap is not listed | up to the 232 s wait (upper bound, assumed); realistic share unknown | 2026-10-07 (docs session, candidate table B) | M (assumed) | medium | Partly listed (S19B §5 #2, #4). Sizing: the D3 `MN_WAIT_STATS` run S20 (main session's note; see the caveat under the table). Candidate A of the E1 work in that note, not to be confused with §7 E1 (`t_primes`). **Status 2026-10-07 (S20, results/S20.md §3, RESULTS §120): sized.** comm_wait about 200 s per thread (a: 4 threads) = 43 % of bs wall and 55 % of dm+recip wall; skew bound 13-30 s per thread in dm, about 0 in recip; recommendation GO on a prototype (user's decision); a ready counter is not needed first. **Status 2026-10-08 (S22, results/S22.md §2, RESULTS §122): re-sized, GO revised to NO-GO for now.** Scaling n = 2..10: wait per thread 30 % -> 53 % of wall; the fabric is busy 84 % of the division, so the ceiling of a perfect division + reciprocal overlap is 43 s of 453 at 10 nodes (mod), 25-34 s at 576 on the target fabric (mod), realistic 6-17 s (a), below the 30 s threshold. The `ready` counter counts data-arrival signals too (skew + transfer). Decision is the user's; one target `MN_WAIT_STATS` run would decide it |
| S-3 | **Barrett constant division for decimal seeds** (further tuning) | Replace the compiler's constant division in the decimal seed `mul_1` path with a Barrett constant. The "further tuning still on the table" item | ≈ −10 s on one node (assumed; no measurement in the transcript) | 2026-09-17 (main session) | S | low | Child of **§2.2** and PLAN WP4 (§58, Barrett `mul_1` done, 2.6 ns/limb). Measure first with the seed-thread timers; it moves the seed wall, which 2.1 caps **Measured, dropped (S37, results/S37.md).** |
| S-4 | **E2 fixed-slot allocator table** | If the tight-reservation layout (E2, L1) still fragments, replace the allocator with an explicit table of fixed slots | removes one 22 GB mid-phase allocation: ≈ 1.3 s at 10¹¹ (m; cost of that allocation in batch 4); memory-neutral; removes the fragmentation risk | 2026-09-24 (main session) | M | medium: layout correctness | A contingency **inside PLAN L1 / E2**, not a new line. Do not start unless the L1/E2 layout fragments. Note: the sweep's "E2" is the allocator layout, not §7 E2 (`t_cap`) |
| S-5 | **Push-form broadcast for B** | Push instead of pull for the B tier's plane broadcast. Each APU writes its quarters to the other three; the Phase-2 bench gave 697 GB/s node-wide for the push, and B's pull ran at ≈ 51 GB/s per APU | not quantified (no model or run) | 2026-09 (S13, "not built"; date not resolved) | M | low | Overlaps **B6** (B4's transfer as a push overlapped with the transform) and **§7 H4** (xGMI push saturation). Treat as a question to answer with H4, not an item |
| S-6 | **HIP graphs for small-dispatch tiers** | Capture repeated small launches (batch and LEAF tiers) as a graph to cut per-dispatch cost. Graphs were never tested; the note says this may lower the "fuse kernels under 30 µs" threshold | upper bound ≈ 4 µs × dispatches per run (assumed; the dispatch count is not measured). Per-dispatch cost itself is measured at 3.5–4.0 µs on the MI300A microbench (m) | undated (2fe672bd, MI300A microbench, "worth adding" table) | M | low: needs a switch; re-capture for variable sizes | Overlaps **§7 H6** (occupancy and launch configuration). Count the dispatches first (one bounded run); if the count times 4 µs is under 1 s, drop it **Measured, dropped (S37, results/S37.md).** |

**Caveat on the cross-references.** The E1 "candidate A" and the `MN_WAIT_STATS` run "S20"
named for S-2 were not found in the repo's docs at this commit. Before S-2 is sized, confirm
that run exists. The ID E1 in §7 means `t_primes`, and E2 in §7 means `t_cap`, so the
sweep's E-labels must be read in their own context.

Not added (already listed or rejected; see the sweep reports): the squaring path, the
reciprocal warm start, the lazy reduction, matrix cores, the RCCL all-to-all, the
per-device remap lock and the other items already in the lists.

## a37v1 comparison (A37CMP, 2026-10-07) — accepted by the user

Source: `results/A37CMP.md` (the a37v1 spec read item by item against ecalc). The user accepted the
recommendations of its §2 shortlist on 2026-10-07 (~20:35 EDT) and rejected the §3 items (decision register §2.4).
Labels: measured (m), modelled (mod), assumed (assumed). Run order: A37-R2 first, then A37-Q1 / Q4 / Q9 as the
measurements their items need; A37-R1 only after both its gates.

| id | what | benefit (label) | effort | gate / dependency | source |
|---|---|---|---|---|---|
| **A37-R2** (FIRST) | CPU microbench of a37v1's base-2^64 sub-range seed (with convert and combine) against ecalc's decimal seed loop (`bi_span_step`, 2.6 ns/limb), on 229-term spans of 128 limbs, one thread, no node | decides whether the seed can be fixed on the CPU; modelled seed 22 → 14–18 s at 6.44e10 (mod, low confidence); a37v1's leaf cost (201 us / 352 terms) suggests it may not beat ecalc (assumed) | S (½ day) | none; gates A37-R1 | A37CMP §2 R2 |
| **A37-R1** (SHELVED by the user 2026-10-08; was gated) | Revive the GPU seed kernel (TASKS 6.4 / E8 / B1; register PH9, S24): one thread or wave per 229-term span, written straight into the region arenas; CPU path kept as fallback; first a bench-only kernel reporting spans/s; behind an off-by-default switch | −7…−12 s at 576 (mod; init 20.6 s + seed wait 9.3 s today); about 0…−4 s at 10 nodes on aac7 (mod, the mapping binds there). Evidence: a37v1 N13 does 4.35e9 terms in 6 s (m, its spec §14), so 7.5e9 terms ≈ 10 s (mod, linear), against an ecalc CPU seed of ≈ 22 s (mod) | L (2–4 d) | **GATED** on A37-R2 (CPU rewrite must not already close the gap) and on A37-Q1 (one-node init timeline at 6.44e10 confirming the seed binds init). Risks: ordering against the VMM mapping; HIP calls from a second thread queue behind allocations (TASKS 6.4). Digits: exact integers, byte-identity checkable per span | A37CMP §2 R1 |
| **A37-R4** | Profile `dc` (2.8–4.1 s, S19B) to split D2H fetch, formatting and T1 residues (Q7); if formatting, a two-digit-table `fmt18` (a37v1 formats 4e10 digits in 0.5 s, m) | ≤ −3 s per node (mod; 0 if `dc` is T1 or D2H); also speeds `tools/unpack_digits` (off the clock) | S (2 h) | none; bit-identical | A37CMP §2 R4, §4 Q7 |
| **A37-R6** | Time the reciprocal chain per doubling for j < 34126 (`ECALC_LOG_CLOCKS`, S19B §5 #4; floor 2.28 s, m), then, if the timings show it, a one-APU (or CPU) small-j path in place of the four-APU distributed products | ≤ −2 s (mod; the 10 s mean excess is the wait already in S19B §5 #4) | S (measure), then M | pairs with D3 / S-2 (division overlap); measurement is A37-Q8 | A37CMP §2 R6, §4 Q8 |
| **A37-R5** (LOW PRIORITY) | Batch tile pipeline: two streams, pinned descriptors, fold `k_norm` and stitch (A27_BATCH_PIPE analogue: overlap tile i's CRT/merge with tile i+1's scatter/NTT) | ≤ −2 s per node (mod: merge 0.84 s + gaps at 4e10, ×1.6) | M (1–2 d) | **needs A37-Q9** (by-phase batch-tier run at 6.44e10) before any gain is trusted | A37CMP §2 R5, §4 Q9 |
| **A37-R9** | Ops hygiene: `archive/drivers/ecalc/a14_soak.sh` and `archive/drivers/ecalc/g13d_hang.sh` send SIGTERM to ecalc, wait up to 60 s, and SIGKILL only if still alive (KFD poisoning risk on a `kill -9`) | avoids a poisoned node (qualitative) | S (done 2026-10-07) | none | A37CMP §2 R9 |

**Measurements** (no code change; each is a bounded run on the target's share, one job at a time):

- **A37-Q1** One-node init timeline at 6.44e10 with `ECALC_INIT_TL=1` on the target. Settles whether the seed
  thread (≈ 22 s mod) binds init at 576 nodes, or the 16.5 s reference init does. Gates A37-R1. (On aac7 it cannot
  answer this: the mapping is slow there.)
- **A37-Q4** NOP-modmul build of the 2^31 forward NTT (a37v1 HBM floor 51 ms; ecalc 99 ms, m) to split the remaining
  gap into structure against modmul; also whether a WbPowTab-style twiddle seed (a37v1 #58) matters. No gate; informs
  the NTT work only.
- **A37-Q9** By-phase batch-tier breakdown at 6.44e10 on 10 nodes (`BS_LAYOUT` / phase timers). a37v1's batch tier is
  21.0 s at 4e10 (m); ecalc's last full by-phase breakdown is at 9.5e10 (25.1 s, m). Gates A37-R5 (and the R7/R8
  gains, which are scaled from 4e10 N-kernel data).

**Status 2026-10-07 (S21, results/S21.md, RESULTS §121; recommendations only, decisions are the user's):**

- **A37-R2: DONE.** The b64k32 method gives no one-thread gain (ratio 0.86-1.07, identical digits); all-core 6-8 s vs 9-15 s (dec18). The R1 gate stays open.
- **A37-Q1: DONE (aac7 single node).** Seed thread about 15 s without the pool wait (spans 10.7 s), not 22 s; seed and mapping finish together on aac7. Modelled for the target: seed ends about 16-17 s vs the 16.5 s init floor.
- **A37-R1: recommended NO-GO now** (gain about 0-6 s, mod, not 7-12); first run `ECALC_INIT_TL=1` once on the target's share.
- **A37-R4: DONE.** `dc` loop 4.1 s = fetch 26 %, reversal 12 %, T1 residues 55 %, T2 8 %; packed output means no formatting on the clock: fmt18 not worth it.
- **A37-R6: measured on one node.** j < 34126 totals about 0.02 s; no one-APU path justified; the 10-node floor (2.28 s) needs A37-Q8 (per-doubling clocks at 10 nodes). Odd: chain start-up 4.14 s vs 0.43 s between two runs.
- **A37-Q4: DONE.** 2^31 forward real 90.4 ms, NOP 76.7 ms (modmul 13.7 ms, 15 %); 4 passes, the extra pass (about 17-20 ms, mod) is the remaining lever. **A37-Q9** and **A37-R5**: not run.

**Status 2026-10-08 (S27 / S25 / S26, results/S27.md, RESULTS §123; recommendations only):**

- **XEFF X1 (rot): NULL** (ABBA B-A +3.1 s, CI -1.0..+7.2). **X3 not built.** **X2 (INTER2 + VSLOT_POOL): in S28.** COMM_XSTATS done: direct exchanges 101 s per thread, 61 % skew; node 1 enters the reciprocal ~20 s late (new item below).
- **NEW XEFF-6a: reciprocal-chain lateness of node 1 (~19 s idle on nine nodes, m).** Diagnose with one `ECALC_LOG_CLOCKS=1 COMM_XSTATS=1` 10-node run, then replicate/overlap the chain (switch, off by default). Top recommendation for the 10-node hold.
- **A37-Q9: DONE** (batch tier 21.3 s at 10 n, 20.6 s at 1 n); **A37-R5: not worth it** (realistic -1.5..-3 s). S25 ladder 6.441/7.0/7.64e10: 410.6/464.4/528.5 s VERIFY OK; memory 28.4 GB per 1e10 + 173 GB fixed; 8.1e10 blocked by the vslot rule. A1 soaks: 975 of 975 ok.

**Status 2026-10-08 (S22 / S23 / S24, results/S22.md, RESULTS §122; recommendations only, decisions are the user's):**

- **D2 / MN_T_CHUNK_MB=2048: DONE on aac7.** S22's 16 new ABBA rounds: 16 of 16 negative, mean -40.6 s (CI -51.7 .. -29.5); pooled with S20 24 rounds, mean -42.3, median -35.1, 22 of 24 negative. Recommended: adopt on aac7 profile lines, target line unchanged (pool law 1024) until a target A/B.
- **A37-Q1: DONE x 6 (aac7).** Seed ends 0.55 s after the last mapping (co-bind); seed threads 96: total unchanged; BS_SEED_FILL=0 +7.6 s. **A37-R1: NO-GO** (target seed ends about 15.8 s vs 16.5 s init floor, mod; gain 0-1 s). One `ECALC_INIT_TL=1` run on the target is the open check (build only if the seed ends after about 18 s). **A37-R2** b_seed64 x 3: digits identical, all-core b64k32 1.15-3x shorter than dec18, no 1-thread gain.
- **A37-Q4 sweep and NTT_SIZE_STATS: DONE.** L24-L31 real / NOP table in S22.md §4.1; no whole 2^31 transform at 6.441e10 per node; whole >= 2^27 transforms 2.5 s per APU at 10 nodes.
- **NTT3P / 3-pass 2^31 NTT: E0 done, NO-GO.** 13/9/9 forward 90.6 ms vs today 89.9 (gate <= 80 ms); 9-stage passes 32.4 / 28.5 ms vs model 22.5; correctness of b1r<13> swizzle OK (468 checks, plan equals production at 2^22, 2^31). Stop unless the user wants a 2-3 day tuning try of the 9-stage body.
- **A37-R6 / dm variance:** S21's 4.14 s Newton start-up did not recur in 6 runs (0.44-0.61 s; dm 33.4-34.3 s); 10-node per-doubling clocks (A37-Q8) still not run.


**The user's decisions of 2026-10-08 (on the S22 / S23 / S24 analysis):**

- **`MN_T_CHUNK_MB=2048`: ADOPTED on the aac7 base line** (`DM_MN_LEAN=1`, `DIST_CHUNKS` 8, `MN_T_CHUNK_MB=2048`; `ecalc/e16_headline.sh`, README, 00_OVERVIEW updated). Evidence: S22 D2, 24 paired ABBA rounds, mean −42.3 s, median −35.1 s, p≈0.04 (m). **The target launch line stays 1024**; optional target A/B "2048 vs 1024, 2 runs each" is `docs/TARGET_TASKS.md` T11b.
- **S-2 division overlap: SAVED as an option** (not built). Gate: build only if the target wait-stats run shows the exposed division/reciprocal wait above about 30 s per APU thread (modelled at 576: ceiling 25–34 s, realistic gain 6–17 s, below the bar). Recorded in `docs/TARGET_TASKS.md` T12.
- **A37-R1 GPU seed: SHELVED** (user 2026-10-08). S22 §3: n = 6 timelines, seed ends 0.55 s after the last mapping on aac7 (co-bind), seed threads halved changes the total by 0; on the target the seed is modelled to end at about 15.8 s against the 16.5 s init floor (gain 0–1 s). Reopen only if the target check (T12) shows the seed ending after about 18 s and more than 2 s after the last mapping.
- **NTT3P / 3-pass 2^31 NTT: SHELVED** (user 2026-10-08). E0: 13/9/9 forward 90.6 ms vs 89.9 ms today (gate ≤ 80 ms); 9-stage passes 32.4 / 28.5 ms vs 22.5 modelled.
- **Target check stage:** the user wants the target to check S-2 and A37-R1 anyway: `ecalc/target_kit.sh --only build,s2chk` (opt-in; 2 and 8 nodes, 6.441e10 digits/node, `ECALC_INIT_TL=1 MN_WAIT_STATS=1`, verdict lines in `KIT_SUMMARY.txt`). Needs an `ecalc` with `MN_WAIT_STATS` (branch s22 / `d3-wait-stats`, not yet in `main`).


**Not in this list.** Items rejected on 2026-10-07 (A37-R3, R7, R8, and the six a37v1 choices) are in the
decision register §2.4, with the reason for each. A37-R3 stays a caution: the a37v1 pitfall (1 corrupt run in 18
with fresh anonymous pages and concurrent GPU writes, m) is not shown to apply to ecalc, whose runs use
`COMM_SHMEM_DEVHEAP=1` and whose target uses `comm_ofi`.

## Status 2026-10-08 (S30/S31 analysis, RESULTS 124)
- S30 (chain broadcast): done, null; cause is a ~20 s stall of node 1 before the chain (results/S30.md). Open: host-vs-rank test + db_from_bi timestamps.
- Segfaults d30_r4s2_A / x2_r3s2_B: open; set ECALC_SEGV_TRACE=1 in all 10-node drivers; candidate race in dbig.c vmm_bg_map tail.

## Status 2026-10-09 (S31/S32/S33, RESULTS 125)
- S31 (X2): done: -34.1 s all rounds / -31.7 s clean at 10 nodes, +0.8 GB; adoption is the user's decision (results/S31.md).
- S32 (host vs rank): done: the 20-25 s stall follows host x9000c1s0b1n0; eviction untested (dd did not drop the cache). Parallel soak still running at 08:20 EDT.
- S33 (2-node VMM_SAFE=2 soak): running; so far base 1 of 142 crashes, VMM_SAFE=2 0 of 141; with S32, VMM_SAFE=2 shows no benefit (3 of 731 vs 1 of 732).
- Open: addr2line of the SEGV traces, synchronous-mapping soak, exclude x9000c1s0b1n0.

## Status 2026-10-10 (S27-S34 wrap-up, RESULTS 126)

- `main` contains s34 and s29 (--no-ff). X2 is adopted on the aac7 line (`ecalc/e16_headline.sh`); optional target try = TARGET_TASKS T13. Rejected: X1 rot, X3 DC off, CHAIN_BCAST, VMM_SAFE=1/2 (docs/code/05_DECISION_REGISTER.md 2.6).
- `ECALC_VMM_BG=0`: final soak 4/422 vs 0/420 segfaults (Fisher p 0.063 one-sided, 0.124 two-sided, suggestive), +20 s; off by default, undecided (results/S34.md).
- Slow host x9000c1s0b1n0: opt-in `MNRUN_EXCLUDE_HOSTS` (mnrun.sh, rundriver.sh); `docs/AAC7_ADMIN_NOTE.md` for the admins (the user sends it). Low MemAvailable is not unique to that host (7 of 13 nodes ~441 GB, 6 ~520 GB).
- Kit: opt-in stage `nodechk` (TARGET_TASKS T14), not yet rehearsed on aac7.
- aac7 QOS is now 6 nodes per user: no 10-node runs.


## Deferred to a future session: the multi-node segfault (user, 2026-10-10)

The current focus is optimising for the node counts we can get on aac7 at once (QOS: 6 per user). The crash work
is parked here:
- **Symptom:** segfault in `hipMemMap` called from the background mapper `vmm_bg_map` (`ecalc/dbig.c`), about 1 % of
  2-node runs (4/422) and about 3.5 % of 10-node runs (3/~85) (m); about 0.4–0.5 % per node per run, so at 576 nodes a
  run would almost always crash (≥ 90 %, mod, if the per-node rate holds). **A fix is required before the target.**
- **Candidates:** `ECALC_VMM_BG=0` (synchronous mapping, branch s34, on main): 0/420 vs 4/422 (Fisher p 0.063 one-sided),
  +20 s per 2-node run (m). `ECALC_VMM_BG=2` (branch s35 015994a: one process-wide lock around the VMM calls and 24
  wrapped HIP calls): gate passed, 0 crashes in 18 A / 17 C runs so far (~/s35_part1, ~/s35_part2; no evidence yet).
- **Next step:** a fast reproducer (e.g. smaller map chunks → many more `hipMemMap` calls per run) so a fix can be judged
  in hours; a plain soak needs ≈ 600 runs per arm (≈ 55 h on 4 nodes) to tell a ≥ 90 % reduction from none.


## Status 2026-10-10 evening (RESULTS 127)

**Done:**
- S35 crash-fix build (`ECALC_VMM_BG=2`, gate passed), S36 VSLOT_SHARE (adopted), S37 (S-3/S-6 dropped), S38/S40/S41 kernel defaults (ADDSUB2, MAXIDX_TOP, QSEL on), S39 survey, S43 multi-node check (2 and 4 nodes: -50.0 s at 4 nodes), S44 aac6 CPX coverage, S45 576-node plan and host harness.
- Single node at 6.441e10 digits: 94 s -> about 78 s (m).

**Open:**
- 10-node runs are impossible under the aac7 QOS (6 nodes per user).
- QSEL confirmation on aac6 with 4 APUs: pending in branch s42 (not merged).
- Next kernel candidates from S39: `k_addsub2` tile / wave-scan (1.0-1.5 s, mod), `k_modq` 128-bit `%` (about 1.4 s GPU, mod), `k_crt_batch` small grid (about 0.8 s, mod).
- X2 cut-group pool-offset asymmetry found by S45 (not a target risk): understand and document.
- Crash work (hipMemMap in `vmm_bg_map`) deferred; the fix is still required before the target (see the deferred section above).
- Scaling study at 2 / 4 / 6 nodes: later. Handoff document: later.
- 2026-10-10 late: aac6 S42 (results/S42.md) confirms ADDSUB2 (−11.6/−12.5 s), MAXIDX (−2.8/−2.6 s), QSEL (−1.49/−1.09 s) on two 4-APU nodes. The one QSEL=1 hang (n1_r6_4B, after init) did not recur in 44 repeat runs (0/44 QSEL=1, 0/14 QSEL=0): 1 hang in 64 QSEL=1 runs on aac6, cause unknown; watch for start-up stalls in future soaks.
