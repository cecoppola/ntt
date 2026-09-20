# The eleven open decisions after Phase 10 — explained

State of the code: `main` @ 550797e. One MI300A node computes 4 × 10¹⁰ digits of e
in 83.0 ± 1.2 s with a host-memory peak of 11.7 GB and 217–262 GB of device memory;
the same code has been run to 8 × 10¹⁰ digits on the node (210 s, 393 GB of the
502 visible). The multi-node form of the same code (one process per node, four APUs
per process) is verified at sizes 2–4 on one node and on two and three real nodes at
up to 10⁹ digits. Every item below is either behind an environment switch or
untouched; nothing changes until you decide. The numbers quoted are from RESULTS
§74–§75 and the agents' write-ups `results/{G,H,M,C,T}.md`.

Each decision is written the same way: **what the thing is**, **why it is a
decision**, **the options** with what each costs and gives, and **my
recommendation** (which is only that).

---

## 1. What to do with the code paths that were measured and rejected (item E1)

### What the thing is
The rule of this project has been "implement the alternative, measure it, keep it
behind a switch, let the user decide". After Phases 8–10 the tree carries about a
dozen such switches whose alternative lost the measurement:

| switch | what it selects | why it lost |
|---|---|---|
| `NTT_B16_XCHG=1` | the transform body's last data exchange through wavefront DPP/`ds_swizzle` instructions instead of shared memory (LDS) | bit-identical but slower: 1.43 → 1.23 TB/s on the kernel; the LDS pipe was not the bottleneck |
| `DIST_R3=1` | a radix-3 variant of the distributed transform's inverse | slower than the fused radix-2 form it was compared to |
| `ECALC_POOL_GROW_GB` | manual growth of the block pool by a fixed number of GB | superseded by the sizing from a simulated layout (A-mem) and the largest-block rule (M) |
| `BS_REGION_SLACK=1` | 1/16 of slack in the region arenas of the tree | caused region-0 imbalance; the round-robin layout made it unnecessary |
| `ECALC_OVERLAP_COPY=1` | copying X to the host in the background thread while the low product runs | X no longer goes to the host at all (Phase 10 H) |
| `DIST_PLANE2=1` | a second memory plane for the distributed transform's pipeline so the unpack of chunk *k* can overlap the row pass of chunk *k+1* | 3–6 % slower at 2³⁰–2³¹ points: the extra unpacks compete with the push kernel for HBM; with one plane the exchange is already 84 % hidden |
| `BS_SEED_DIRECT=0` | the seed thread issues one DMA per 2 GiB chunk instead of writing directly into the device arenas | slower: HIP calls from a second thread queue behind the main thread's `hipMalloc`; null-stream copies queue behind the plane clears |
| `COMM_PUSH64=0`, `COMM_PUSH_BLOCKS=228` | the previous xGMI push kernel (32-bit stores, 228 blocks per peer) | the new kernel is 9 % faster on a 2³¹-point convolution |
| `RNS_DIST_CACHE=1` at size 1 | keeping the transformed pieces of Q across the reciprocal's last doubling and the X·Q product | at size 1 the planes needed for the cache cost more than the transforms saved; at size > 1 it wins and is on (`RNS_DIST_CACHE_MN=2`) |
| `MN_DM=host`, `MN_COMBINE=host` | the *host-flow stand-ins*: in the multi-node run, instead of the sharded (distributed) division, node 0 gathers P and Q, does the division alone, and scatters X back; and node 0 broadcasts X's residues instead of each node computing its own | they were the scaffolding by which the multi-node run first worked (M2–M4); the distributed division (A-div) and per-node output (A-out) replaced them, but they remain the only way to cross-check the sharded division against the single-node one at size > 1 |

### Why it is a decision
Each retained path is code that every future change must keep compiling and, in
principle, keep working. The heavy ones (`DIST_PLANE2`, the seed DMA, the host-flow
stand-ins) are hundreds of lines threaded through the most-edited files
(`ecalc.c`'s output stage, `newton_mn_divmod`, `ntt_dist.c`). The light ones are a
line or two each. Against that, the switches are the *evidence*: RESULTS cites them
by name, and a future reader (or agent) can rerun the losing measurement on a new
ROCm or a new node.

### Options
- **(a) Delete every rejected path.** About 900 lines removed, half a day plus a
  regression pass. Cleanest tree. But it also removes `MN_DM=host`, which is the
  only independent check of the sharded division at size > 1 — and decision 6 (an
  intermittent failure on exactly that verification path) is still open. Deleting
  the cross-check before that is closed is a risk.
- **(b) Keep everything as it is.** No work now. Cost is carried forward: two code
  shapes for the size-1 and size-> 1 output stage, and the rejected paths exercised
  (or silently broken) at every merge.
- **(c) Delete the heavy paths that have no measurement left to make
  (`DIST_PLANE2`, `BS_SEED_DIRECT=0`, `ECALC_OVERLAP_COPY`, and the host-flow
  stand-ins *after* decision 6 is closed); keep the one-line switches
  (`NTT_B16_XCHG`, `DIST_R3`, `COMM_PUSH*`, `RNS_DIST_CACHE`, `BS_REGION_SLACK`).**
  Half a day. The measurement evidence for the cheap ones stays runnable; the
  expensive ones are documented in RESULTS with their numbers, which is what anyone
  would consult.

### Recommendation
(c), sequenced after decision 6.

---

## 2. Whether the agents' write-ups are tracked by git (item E2)

### What the thing is
`results/` is git-ignored because it holds run logs and the 40 GB digit files. The
agents' design write-ups (`results/G.md`, `H.md`, …, `A-*.md`, `M3.md`) live in the
same folder and were each added with `git add -f`. If an agent forgets `-f`, its
write-up is not in the repository and is lost when its worktree is removed.

### Options
- **(a) Add the rule `!results/*.md` to `.gitignore`.** One line. Markdown in
  `results/` is tracked automatically; logs and digit files stay ignored.
- **(b) Keep force-adding.** Nothing to do; the risk above stays.

### Recommendation
(a). There is no downside.

---

## 3. Whether the division's memory pool is pre-grown by default (item A3, switch `ECALC_DM_POOL`)

### What the thing is
The division phase ("dm": the Newton reciprocal of Q, then the quotient X) works on
device-resident numbers drawn from a *block pool* on each APU. The pool is fed by
the region arenas that held the binary-splitting tree (donated when the tree is
done) and grows by `hipMalloc` when a request does not fit in any free hole.
Mapping device memory is slow on this node (0.054–0.075 s per GB) and, when it
happens in the middle of a phase, it also stalls the GPU.

Agent M's new logging found what the mid-phase growth actually is: not many small
blocks, but **exactly one block per APU** — the quarter of the reciprocal's
temporary *t₁* (8.9 GB at 4 × 10¹⁰, 15.6 GB at 7 × 10¹⁰, 17.8 GB at 8 × 10¹⁰), which
needs a contiguous hole and the arena's free space is in two or three smaller
holes. `ECALC_DM_POOL=1` pre-grows the pool once, at the start of the division,
with a chunk sized to hold that block, so no growth happens inside the phase.

### Why it is a decision
It is measured and harmless but small, and your rule is that defaults change only
on your call:
- 7 × 10¹⁰: 159.7 s → 157.2 s (−1.6 %; the reciprocal 35.9 → 34.2 s).
- 4 × 10¹⁰: 85.3 s → 85.9 s (within the ±1.2 s noise).
- Same peak memory either way (the same bytes get mapped, only earlier).
- 8 × 10¹⁰ was run with it on.

### Options
- **(a) Leave it off** (the present default). Nothing changes.
- **(b) On always.** One-line default change. Gains 1.7 s at 7 × 10¹⁰, nothing at
  4 × 10¹⁰.
- **(c) On above a digit threshold** (e.g. ≥ 5 × 10¹⁰). One comparison in
  `ecalc.c`. Takes the gain where it exists and leaves the 4 × 10¹⁰ series
  untouched.

### Recommendation
(c), threshold 5 × 10¹⁰. No correctness risk (VERIFY OK and identical digits in
every measured run).

---

## 4. Wiring the new `alltoallv` into the code that needs it (item B7)

### What the thing is
When the run spans *g* nodes, the big numbers are *sharded*: each node holds a
contiguous share. Several operations move data between the shares:
- `mdb_shift` — shift a sharded number by some limbs (needed by the Newton
  iteration);
- `mdb_add_shifted` — add one sharded number into another at an offset;
- the *redistribution* at the start of a distributed product — the shares are
  reorganised into the block-cyclic layout the four-step transform needs;
- `mdb_to_host_all` — bring a small number to every host.

All of these were built on the communicator's `alltoall`, which only moves
**equal-sized** slabs. Unequal transfers were made equal by **padding**: each APU
allocates a scratch area of *g* × (its largest slab) and sends mostly-empty slabs.
That is fine at g = 4 and wasteful at g = 2 048: at 4 × 10¹⁰ over two nodes
`mdb_shift`'s scratch is 17.6 GB per node; at 2 048 nodes it would be
2 048 × 2²⁶ limbs × 8 B ≈ 1.1 TB per APU — impossible.

Agent C implemented `comm_alltoallv` (unequal byte counts per peer) in every
transport — local, simulated, xGMI, TCP, and the layered node×APU communicator —
with a count check that aborts on a mismatch, and tested it. What remains is
switching the four consumers above from the padded `alltoall` to it; the exact
code is written out in `results/C.md`. The consumers live in agent G's files
(`rns_dist.c`, `newton_db.c`).

### Why it is a decision
It is needed for the target system and not at all for the single-node result; it
touches the most intricate multi-node code; and on aac6 it can only be tested to
size 4 (one node, four processes) and to 2–3 real nodes at ≤ 10⁹ digits.

### Options
- **(a) All four consumers now, on aac6.** 1–2 days. Every path re-verified through
  `t_mn_grid` (120 checks per node) and the regression script. Memory saved at
  4 × 10¹⁰ over two nodes: 17.6 GB per node from `mdb_shift`, plus g × 512 MB per APU
  from `mdb_add_shifted`. The redistribution's padding is only two rows per pair —
  nothing to save there, but the received slabs would arrive already in order and
  a binary search in the gather kernel could go.
- **(b) `mdb_shift` only, now.** Half a day. It is the one whose scratch grows
  with g; it gives most of the memory gain and the smallest change.
- **(c) Defer to the target system**, when the RDMA communicator (M8) is written
  and the code will be reworked around it anyway. No work now; the 2 048-node run
  cannot start without it.

### Recommendation
(b) now — it removes the one term that scales with g — and the rest together with
M8.

---

## 5. Laying the device arena out so the division never grows the pool (agent M's open issue)

### What the thing is
Same mechanism as decision 3, the other remedy. Instead of pre-growing the pool
by a chunk (decision 3: same bytes, mapped earlier), the arena that already exists
could be laid out from the simulated layout so that a contiguous hole the size of
*t₁*'s quarter is left at its tail. Then the division would find its block in the
arena and map **nothing** inside the phase: 62 GB less device memory mapped over
the run at 4 × 10¹⁰, 89 GB less at 8 × 10¹⁰.

### Why it is a decision
It is a memory gain, not a time gain. The time inside the mapping is at most ≈ 2 s
(decision 3 measured 1.7 s at 7 × 10¹⁰ for removing the same growth). What it buys
is headroom: at 8 × 10¹⁰ the node peaked at 393 GB of 502; with this the same run
would peak near 305 GB and the per-phase accounting puts the next size that fails
near 10¹¹ digits on one node. At 4 × 10¹⁰ it changes nothing that matters.

### Options
- **(a) Do it.** One day: the layout simulation gains one item (the reserved tail),
  and the block pool's allocator prefers the tail for the largest request.
- **(b) Leave it.** Keep decision 3's pre-grow as the remedy for the stall.

### Recommendation
(b), unless you want more than 8 × 10¹⁰ digits per node — then (a).

---

## 6. The intermittent verification failure at size > 1 (item D5)

### What the thing is
Agent T ran 18 multi-process runs of 10¹⁰ digits over four node-processes with the
pool growth deliberately forced (the situation A-mem had once seen a wrong result
in). Seventeen were right. In the one failure, the **digits were correct** (the
output file compared identical to the reference) but the Tier-1 residue check
reported six of the eight check primes BAD for both P and Q, with primes q₂ and q₆
fine. A wrong *number* cannot produce a per-prime pattern like that — every prime
would fail — so the fault is in the **checker** on the size > 1 path: either the
device residue kernel `db_mod_qs` over shares, the reduction of residues across
nodes in `mn_out.c`, or a race between the new mutex around `db_mod_qs` (Phase
10 H) and the fetch streams. A-mem had seen the identical signature once in Phase 9.
T's verdict is therefore: not the stale-pool-pointer bug that was suspected (the
binary splitting is deterministic under growth — eight kept checkpoint series were
byte-identical across runs), but an intermittent fault on the verification path.

### Why it is a decision
It has no bearing on the single-node result (the size-1 path never showed it in
hundreds of runs) and it does not corrupt digits. But at scale a run could report
BAD on a correct result, and today the residues cannot be recomputed on their own —
the whole verification would have to be rerun, and until the cause is known there
is no guarantee the fault is *only* in the checker.

### Options
- **(a) Investigate now.** 1–2 days, mostly node time: reproduce with the residue
  kernel instrumented (the per-share residues logged per prime before the
  cross-node reduction), so the failing stage identifies itself. Fix follows from
  that.
- **(b) Leave it recorded** (T.md has the log) and revisit when the multi-node
  campaign starts.

### Recommendation
(a) before any multi-node campaign; it is the only open item with a correctness
bearing. It is not needed for the single-node number, and decision 1's removal of
`MN_DM=host` should wait for it.

---

## 7. Larger transform planes for the top tree levels (item B3, and A2 which depends on it)

### What the thing is
The largest products (the top two tree levels and the reciprocal/division) run on
the *distributed tier*: a four-step transform spread over the four APUs, with the
work cut into *pieces* that fit the **transform planes** — the pre-allocated
buffers (120 GB per node today) that hold a number's residues during a product.
The planes are 2³¹ points per prime. Agent A-grid measured that 3·2³⁰-point planes
(1.5× larger) let the top levels use fewer, larger pieces: **−2.5 s**, at the cost
of +136 GB of device memory and a 15 s first-use penalty that sizing the planes at
initialisation would remove (that is the part not yet done). Item A2 — pairing the
four level-22 products so the shared operand is transformed once, as the batch
tier already does — needs the same larger planes and gives **−1.5 s** more.

### Why it is a decision
It is the largest single-node speed item left (≈ 4 s of 83, ≈ 5 %) and it is a
memory trade: at 4 × 10¹⁰ the node would go from ≈ 262 GB to ≈ 400 GB of device
memory, which fits; at 7–8 × 10¹⁰ it does not fit, so it would have to be a
size-dependent default. Mapping 136 GB more at init costs ≈ 8 s unless the planes
take part of the region arenas' allocation instead (which the simulated layout
can arrange).

### Options
- **(a) Do it**: 1 day for the planes sized at init, half a day for A2. Expected
  4 × 10¹⁰: ≈ 79–80 s.
- **(b) Leave it.** 83 s stands.

### Recommendation
(a), as a switch that is on by default below 5 × 10¹⁰ digits and off above.

---

## 8. A run over two real nodes (items C2 and D1)

### What the thing is
The multi-node code has run on two and three real aac6 nodes at 10⁸–10⁹ digits,
identical to the references. The full-size cross-node run (4 × 10¹⁰ over two
nodes) was tried and cancelled: both processes were healthy at 263 GB each, but
the aac6 nodes are joined by **1 GbE** (≈ 110 MB/s), and a single 2³¹-point
distributed product moves ≈ 100 GB across that link — 15–20 minutes per product,
dozens of products in the division, hours in all. The code is not the limit; the
cluster's fabric is. The target system's fabric is 400 GB/s per node.

### Options
- **(a) Target system only.** Nothing on aac6.
- **(b) A 5 × 10⁹-digit run over two real nodes** — the largest size the link makes
  sensible (≈ 30 min) — when two nodes happen to be idle at the same time (they
  were not, at any point in the last two days). Adds one more real-fabric data
  point on the same code path already verified at 10⁹.

### Recommendation
(b), opportunistically — it costs nothing but node time, and only if two nodes
come free.

---

## 9. The remaining small single-node items (from PLAN §22)

### What they are
- **A4** — the batch tier's *tile budget* in pair mode (15 GB today; the pools
  could allow 2²⁹ points per prime plane). A tuning knob; ≤ 1 s, possibly nothing.
- **I11** — build the transform contexts and twiddle tables once and share them
  across the four APUs, and clear the pools with kernels instead of `hipMemset`.
  Init is 16.4 s, of which ≈ 12 s is the driver mapping the device pools and
  cannot be moved; I11 attacks the other ≈ 4 s: −1…−2 s.
- **A7 / I8** — a faster decimal single-limb multiply in the seeds (two limbs per
  step or a Montgomery-style reduction). The seeds are hidden inside init today,
  so the gain is 0 until init itself shrinks below ≈ 8 s.
- **I5** — Karatsuba for products whose half-sums fit a plane. Only the binary
  (`LIMB_BASE=2`) path has such products; −2.5 s there, nothing for the decimal
  default.
- **I13** — a two-prime 62-bit engine for the batch tier alone. Rejected end to
  end in Phase 5, never measured for the batch levels only; gain unknown, likely
  none.

### Options
Each is half a day to a day. Only A4 and I11 have a plausible gain on the decimal
default, ≤ 3 s together.

### Recommendation
After decision 7, if at all.

---

## 10. Load balance when the node count is not a power of two (item C3)

### What the thing is
The distributed transform over a group of *g* nodes uses a block-cyclic layout
that is exact when *g* is a power of two. For other *g*, the nodes beyond the
largest power of two take part only in the redistribution and idle during the
product. With 2 048 = 2¹¹ nodes nothing is needed; with, say, 1 800 nodes, 776 of
them would idle in the distributed levels.

### Options
- **(a) Implement a general block-cyclic map** (one day) so any *g* balances.
- **(b) Fix the target run at a power of two** and do nothing.

### Recommendation
(b), unless the target's node count is known not to be a power of two.

---

## 11. What Phase 11 is

### What the thing is
The choice of the next block of work. Two tracks are available and they touch
disjoint files:
- **Single node**: decisions 7 + 3 + 9 → an expected ≈ 75–78 s at 4 × 10¹⁰ (from
  83.0). 2–3 days. Moves the paper's headline number.
- **Multi-node hardening**: decisions 6 + 4 + 8, then the RDMA communicator (M8)
  when the target system exists. 2–4 days. Moves the 2 048-node readiness.

### Options
- **(a) Single node only.**
- **(b) Hardening only.**
- **(c) Both at once** with two agents under the Phase 9/10 protocol (own
  worktrees, own aac6 clones, disjoint files: planes/init versus
  mdb/comm/verify), the integrator merging and running the regression.

### Recommendation
(c).

---

## Summary of recommendations (each is your call)

| # | recommendation |
|---|---|
| 1 | delete the heavy rejected paths, keep the one-line switches — after 6 |
| 2 | track `results/*.md` |
| 3 | `ECALC_DM_POOL` on above 5 × 10¹⁰ |
| 4 | `mdb_shift` on `alltoallv` now; the rest with M8 |
| 5 | leave, unless > 8 × 10¹⁰ per node matters |
| 6 | investigate before any multi-node campaign |
| 7 | do it; on by default below 5 × 10¹⁰ |
| 8 | 5 × 10⁹ over two real nodes when two are idle |
| 9 | after 7, if at all |
| 10 | fix the target at a power of two |
| 11 | both tracks with two agents |
