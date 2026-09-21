# The design choices open after Phase 11 — explained, with costs and benefits

State: `main` @ ef4c717 (RESULTS §76). One node: 4 × 10¹⁰ in 81.5 ± 1.4 s / 11.7 GB
host; 7 × 10¹⁰ in 153.5 s; 10¹¹ in 262.9 s at 445 GB of 502, no memory mapped inside
any phase. Multi-node: SHMEM transport, transform balanced at any group size, models
calibrated. Target: PLAN §25 (576 nodes, Slingshot-2 dragonfly, 8 NICs/node, SHMEM).
Nothing below has been acted on; every item is a switch or untouched. Numbers are
from results/{S,L,X,P,V,M11}.md. The decisions from DECISIONS.md that were settled
by measurement in Phase 11 (planes, DM pool, arena tail, alltoallv, load balance,
D5's cause) are not repeated; what remains of them is folded in below.

Same format as before: what it is, why it is a decision, the options with cost and
benefit, my recommendation.

---

## 1. The timing-dependent fault at the batch tier's level transition

### What it is
Agent V found that when a transform plane pool is *forced* to grow in the middle of
the binary-splitting phase (T's recipe: `POOL_LOG=29 RNS_POOL1_GB=3.2213` at 10¹⁰
over four processes), node 0's or node 1's leaf results P_r, Q_r sometimes come out
wrong from one limb upward — 5 of 26 runs — while any per-level probe (a residue
kernel or a CPU read of the regions between levels) makes it vanish (31 of 31). That
is the signature of a race: something at the end of one level is not complete when
the next level starts; the first suspect is `spill_merge`, where the CPU writes into
the device regions before the next level's scatter kernel reads them. It never occurs
at the defaults: since Phase 9 the pools are sized at initialisation from a simulated
layout and never grow inside a phase (every regression, every 4 × 10¹⁰, the 10¹¹ run
had zero growth). Its owners are the batch-tier files (`rns_mul.c`, `binsplit.c`),
which V did not have.

### Why it is a decision
The digits are wrong when it strikes and the (now correct) verification catches it,
so it is a rerun, not a silent error. But a race that exists is a race that can
appear under a different timing — a new ROCm, the target's fabric — and a 576-node
run that fails verification costs the whole run.

### Options
- **(a) Find and fix it now.** 1–2 days, mostly node time: V's reproducer
  (`v11_d5.sh`, `ECALC_RES_LOG`, `ECALC_LEAF_DUMP`) localises the level at which the
  leaf goes wrong; the fix is a stream synchronisation or an ordering at the
  transition. Benefit: the fault is gone rather than avoided.
- **(b) Make "no growth inside a phase" an enforced invariant**: abort with the
  accounting instead of growing (M's `mem_oom` already prints it). Half a day. The
  race becomes unreachable at the defaults *and* under any misconfiguration; but it
  is still there.
- **(c) Both**, (b) first as a guard, (a) when node time allows.

### Recommendation
(c). (b) is cheap insurance for the target; (a) is the only real closure.

---

## 2. The top-level product gridded over the 2³¹-point planes at large g

### What it is
At the top of the tree over g nodes the final product is one distributed
transform whose planes grow with n/(4g) — at 576 nodes M's accounting puts them at
214–856 GB per node plus spill buffers, which caps the machine at ≈ 1.2 × 10¹³
digits (≈ 2 × 10¹⁰ per node). The single-node reciprocal already avoids exactly this
by cutting its products into *pieces* over fixed 2³¹-point planes (A-grid's grid over
shares does the same for the sharded division). Applying that form to the tree's top
levels keeps the per-node profile equal to the single node's, and the 576-node limit
becomes the single-node ceiling × 576: ≈ 4.4 × 10¹³ digits.

### Why it is a decision
It is the single item that decides whether the machine computes 1.2 × 10¹³ or
4.4 × 10¹³ digits; it is 2–3 days in L's and G's files (`rns_dist.c`, `mn_tree`); it
can be tested on aac6 only by forcing the grid at small sizes (`DIST_LOGN_TEST`), as
the division's grid was.

### Options
- **(a) Implement now** on aac6, forced-grid tests at sizes 2–9 and 10⁹–10¹⁰.
- **(b) Defer to the target**, where the size that needs it can be run. Risk: the
  first large run on the target is also the first test of the code path.

### Recommendation
(a). It is the largest remaining scaling item and it is testable now.

---

## 3. What to do about the larger transform planes (`RNS_PLANES_3Q30`)

### What it is
P built the 3·2³⁰-point planes sized at initialisation: the GPU phases lose 3.6 s
(top levels 13.2 → 10.4 s) but mapping 60 GB more at init costs 4.7–6.3 s, because
the driver maps at only 0.08–0.12 s/GB while the seed thread streams into the arenas
at the same time. Net: slower on one node; off by default; the fastest *phases* ever
measured (62.4 s) are with it on.

### Options
- **(a) Leave it off.** Nothing to do.
- **(b) Move the seeds out of the mapping window** (seeds after the pools are mapped,
  or the pools mapped before the seed thread starts): the mapping returns to
  0.057 s/GB and the net becomes ≈ −1.5…−2 s. One day; the risk is that the seeds
  then show on the wall clock (they are 15 s of CPU work hidden today).
- **(c) On for multi-node runs only**: over 576 nodes init is per node and the
  distributed levels are 34–40 % of the wall, so the planes' gain in the top levels
  is worth more there and the mapping cost is the same 5 s. A switch by size, no code.

### Recommendation
(c) now (a one-line default), (b) if a single-node number matters more than 2 %.

---

## 4. The SHMEM transport's form on the target

### What it is
The transport works on aac6's OSHMEM in its most conservative form: one global lock
serialising the four APU threads' calls (`COMM_SHMEM_SERIAL=1`, because OSHMEM 4.1.6
crashes under concurrent waits), the symmetric heap in host memory registered with
HIP, and the callers' slabs staged through the pool by a helper thread. Cray SHMEM on
the target supports `SHMEM_THREAD_MULTIPLE`, contexts per thread that map to
separate NICs, and device-memory symmetric heaps — the forms that use the eight NICs
in parallel and avoid the staging copy.

### Why it is a decision
None of the faster forms can be tested on aac6; each is a switch (`COMM_SHMEM_SERIAL=0`,
`COMM_SHMEM_DEVHEAP=1`) plus one addition (an allocator `comm_sym_alloc` so the
callers' slabs live in the pool, no staging). Writing them now means untested code;
not writing them means the first day on the target is spent on the transport.

### Options
- **(a) Write the three forms now behind their switches**, tested only for
  compile and for the serial path. Half a day. First day on the target: flip switches.
- **(b) Leave the target forms to the target.**

### Recommendation
(a): cheap, and the target's first hours are the scarce resource.

---

## 5. The dragonfly third layer (`MN_TOPO_GROUP`)

### What it is
The all-to-all in three layers (APU × node-in-group × group) aggregates cross-group
traffic into one message per peer group. Built (S) and modelled (X): at 576 nodes it
cuts messages per APU from 925 k to 322 k but doubles the bytes on the NICs (relayed
data), and the modelled wall goes 139 → 161 s. It would pay only for exchanges below
≈ 200 MB per APU — which X1 now sends to small groups instead.

### Options
- **(a) Keep it, off by default**, and measure once on the target (the model's
  message cost is an assumption; if the real per-message cost is 10× higher the
  balance flips). No work.
- **(b) Delete it.** Half a day; loses the only remedy if the target's message rate
  is the problem.

### Recommendation
(a).

---

## 6. The level → group schedule at 576

### What it is
576 = 9 · 2⁶: after the six doubling levels (groups of 2 … 64) the top step is either
one 9-way level (`2,4,…,64,576`, the default) or two 3-way levels
(`…,64,192,576`). L's map handles both; X's model costs them as 4 products per
3-way level pending L's real schedule; the choice changes the top levels' product
count and their group sizes.

### Options
- **(a) Decide by the model now** (an afternoon: L's real schedule in X's model).
- **(b) Decide on the target by measurement** (both are one environment variable).

### Recommendation
(a) now for the paper's number, (b) confirms it.

---

## 7. Digits per node on the target: the safe size or the ceiling

### What it is
Per node the profile is measured to 10¹¹ digits (445 GB) on one node. Over 576
nodes the model adds the cross-node scratch (after L's `alltoallv`: 17–71 GB per
node) and puts the ceiling at 7.7 × 10¹⁰ per node at 500 of 502 GB — no margin —
against 3.8 × 10¹⁰ per node at 344 GB.

### Options
- **(a) Plan the target run at the ceiling** (4.4 × 10¹³ digits, ≈ 4.6 min): the
  headline; one allocation failure on one node ends the run.
- **(b) Plan at the safe size** (2.2 × 10¹³, 2.0 min) and step up.
- **(c) Both**, safe first.

### Recommendation
(c); the run at the ceiling only after M's tail layout is verified at size > 1 on the
target (it is verified at size 1 to 10¹¹ and at size 4 to 10¹⁰).

---

## 8. Housekeeping settled by Phase 11's measurements

- **`ECALC_DM_POOL`**: with the tail layout the pre-grown pool is a no-op at every
  size (zero growth anyway). Delete the switch and the C3 block (an hour) or keep as
  a fallback if `ECALC_TAIL=0`. *Recommend delete; `ECALC_TAIL=0` is the fallback.*
- **The host-flow stand-ins (`MN_DM=host`, `MN_COMBINE=host`)**: kept by V as the
  only sharded-vs-host cross-check while item 1 is open. *Recommend: delete when 1
  is closed.*
- **`ECALC_CKPT_TOP=1` at size 1**: the recheck mode needs the top-level P, Q set on
  disk; today it is written only when asked (2 × 17.8 GB at 4 × 10¹⁰, ≈ 20 s of write
  hidden or not depending on the file system). *Recommend: default on for runs
  above 10¹⁰ — a re-verification without a rerun is worth 20 s.*
- **The transform cache's reach (X3)**, **A4's tile knob**, **`RNS_STRIPED_PAIR`**:
  measured or unmeasured small items; leave as they are.

---

## 9. What cannot be settled on aac6

- **Two real nodes over SHMEM** (never two idle nodes together in the session; the
  code path is the same as TCP's, which passed on 2–3 nodes). Opportunistic.
- **Part-file bandwidth at scale** (C7): the model assumes 1 GB/s per node; the
  target's file system decides whether the 40 GB per node hides under the low
  product.
- **The real per-message cost of SHMEM on Slingshot** (`--lat`): the assumption
  behind items 5 and 6.
- **s24-30 runs the large sizes 50 % slower than the other nodes** (M): a
  measurement-hygiene note — series are taken on one node — not a design choice.

---

## Summary

| # | choice | recommendation |
|---|---|---|
| 1 | leaf-transition race under forced growth | guard now (abort instead of grow), fix when node time allows |
| 2 | top-level product gridded over 2³¹ planes | implement now; it decides 1.2 vs 4.4 × 10¹³ digits |
| 3 | larger planes | on for multi-node sizes only; seeds out of the mapping window if 2 % on one node matters |
| 4 | SHMEM target forms (thread-multiple, device heap, pool-resident slabs) | write behind switches now |
| 5 | dragonfly third layer | keep off, measure once on the target |
| 6 | 9-way vs 3·3 top schedule | model now, measure on the target |
| 7 | digits per node on the target | safe size first, ceiling after the tail layout is verified at size > 1 |
| 8 | housekeeping | delete `ECALC_DM_POOL`; stand-ins after 1; `ECALC_CKPT_TOP` on above 10¹⁰ |
| 9 | aac6 cannot settle | two-node SHMEM run when idle; the rest on the target |
