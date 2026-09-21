# X — cross-fabric traffic and the 576-node model (Phase 11, PLAN.md §26 row X)

Branch `x11` (from `main` @ 72aa2e9). Files: `ecalc/mn_model.py` (new), `ecalc/newton_db.c` (`recip_mn`, `newton_mn_divmod`,
`mdb_shift_g`), nothing else touched. Clone `~/ntt-x` on aac6; batches `~/x_b1.sh` (calibration, job 20802, s24-26),
`~/x_b2.sh` (the X1 gates); logs `~/x11/b1/`, `~/x11/b2/`.

## X2 — the fabric model (`mn_model.py`)

`./mn_model.py` prints, for D ∈ {4e10, 8e10, 1e11} digits per node and g ∈ {4, 64, 576} nodes, the per-node wall by phase,
the bytes on the fabric (per node, per NIC, on the dragonfly's global links), the exposed communication, the memory per
node and the total digits; `--calib` reproduces the aac6 multi-process runs; `--group G --layers 3 --taper t --lat s
--bw GB/s --write-bw GB/s --rule full` vary the target's parameters. The script is the document: every constant is in
the table at its top with its RESULTS section.

### Inputs (measured)

| item | value | where |
|---|---|---|
| single-node phases at 1e9, 1e10 (this session), 4e10, 7e10, 8e10: init / batch / top levels / reciprocal / division | 9.3/1.5/0.1/2.8/0.4 · 10.8/7.1/1.1/4.8/3.0 · 16.4/23.0/13.1/15.2/14.9 · 22.2/39.5/25.7/35.9/36.6 · 25.1/46.6/38.8/49.6/49.5 s | job 20802; RESULTS §75, §74, results/M.md |
| the 2^31-point piece product (4 primes, 2^29 points per APU) | 1.11 s (load 0.33, ntt 0.67, crt/out 0.11); 0.72 s with the B operand's transform cached | results/G.md |
| the four-step transform's parts at 2^31 over the four APUs (per three transforms of one prime) | rows 0.0624, cols 0.0429, packs 0.0385, xGMI exchange 0.0571 s, exposed 0.0089 (84 % hidden) | results/C.md (DIST_STATS) |
| memory per node (size 1) | planes 120.3 GB; regions 2.3 GB and the dm pool's peak live 2.9 GB per 10⁹ digits; host 3.1 + 0.215 GB per 10⁹ | results/M.md, RESULTS §75 |
| the target | 576 nodes, 100 GB/s per APU (two 400 Gb/s NICs), dragonfly groups of `MN_TOPO_GROUP` = 64 nodes, ~2 µs per message, the global tier limited by the group's aggregate injection to each peer group (taper 1.0) | PLAN §25 |

### The model

Phases per node at size g and D digits per node: init(D), batch(D), top levels(D) from the single-node table
(log-log interpolation; above 8e10 extrapolated on the 7e10–8e10 slope); then every distributed product is costed the
way the code forms it — the tree's levels over groups 2, 4, …, 64 and the two 3-way steps to 192 and 576 (L's
C3-576 schedule; a 3-way level = 4 products: Q₂Q₃, Q₁(Q₂Q₃), P₂Q₃+P₃, P₁(Q₂Q₃)+…), the reciprocal's anchored chain
(k, ⌈k/2⌉, … to 2¹⁶; per step Q_t·r and r·d, six shifts, two adds, the small collectives) and the division's A_h·μ
(low cut) and X·Q (high cut), each product as `split_grid_cap`'s grid at the group's plane cap 2^(31+log₂ gt) with the
2-slot transform cache over shares (B's piece held across A's pieces, A's piece 0 across B's).

A piece of `pts` points over g' nodes (q = pts/(4 g') points per APU): local = 1.11 s × q/2²⁹ (the measured piece, its
xGMI exchanges included) plus, per transform (EC_NP × (fwd + inv) = 12 layered all-to-alls of 8q bytes per APU),
the fabric stage's excess over the xGMI stage it runs beside (`comm_layered` keeps two exchanges in flight, so
exposed = max(0, t_fabric − t_xGMI); t_xGMI = 0.019 s × q/2²⁹); the operand redistributions and the result exchange
(plain all-to-alls of the operands' bytes, exposed); three small collectives. An all-to-all over g' nodes with B bytes
per APU: two-layer (APU × node) — B (g'−1)/g' on the NIC, K (g'−1) messages per APU (K = 4 slab chunks);
three-layer (APU × node-in-group × group, g' > G) — B ((G−1) + 3 (g'−G))/g' on the NIC (the cross-group data is
relayed in and out of the group), K (2 (G−1) + g'/G − 1) messages; the bytes that cross a global link are
B (g'−G)/g' either way, timed against the group's aggregate injection to each peer group × taper.

Memory per node at the dm peak = planes 120.3 + max(regions, dm pool)(D) + the distributed scratch (4 q per APU at the
cap plane = 69 GB, M3) + the shift buffers (2 × share/4 per APU with `alltoallv` — L's B7; g × that padded, as today)
+ host. The transform cache over shares (2 × 16 GiB per APU) is counted as 0 (the group takes what the pool allows).

### Calibration on aac6 (`--calib`; g node-processes sharing one node over loopback TCP, job 20802, s24-26)

The substitutions: the loopback transport at 3.3 GB/s per node-process (g = 2; its single stream per mesh is slower
below 1 GiB per APU, exponent 0.3) / 3.7 GB/s (g = 4, exponent 0.1) — fitted on the products' `dist_mn` lines
(2³¹ over 2 × 4 ranks 15.7–16.6 s, over 4 × 4 ranks 9.4–11.2 s; 2²⁶ 1.2 / 0.55 s) — 1 ms fixed per chunk exchange, the
local passes g × slower (the processes share the four APUs), the leaves at 0.5 × the single-node bs of the total digits
(measured: four 2.5 × 10⁹-digit leaves take 3.9 s beside bs(10¹⁰) = 8.2 s), init as measured (a shared-node artefact).

| digits | g | measured wall | modelled | error | measured bs+tree / dm | modelled leaf+tree / dm |
|---|---|---|---|---|---|---|
| 10⁹ | 2 | 27.7 s | 28.1 | +2 % | 3.8 / 18.0 | 3.7 / 18.5 |
| 10⁹ | 4 | 19.7 s | 19.3 | −2 % | 3.8 / 10.6 | 3.1 / 11.0 |
| 10¹⁰ | 4 (POOL_LOG 29) | 110.1 s | 98.5 | −11 % | 32.6 / 68.9 | 27.4 / 62.5 |
| 10¹⁰ | 2 (POOL_LOG 30) | 146.4 s | 131.1 | −10 % | 26.8 / 105.2 | 23.1 / 93.7 |

(size 1 for reference: 10⁹ 14.4 s, 10¹⁰ 29.0 s, identical to the references.) Within the 20 % gate; the model is
10 % optimistic at 10¹⁰ — the TCP transport's mid-size exchanges (2²⁸–2³⁰) run at 60–80 % of its large-exchange rate.

### The target tables (`./mn_model.py`, defaults: G = 64, two layers, taper 1, 2 µs, part files at 2 GB/s per node)

Per-node wall (s) = init + batch + top levels + distributed levels + reciprocal + division + the part file's exposed
tail; "exposed" = the communication not hidden by the pipeline; NIC = bytes through the node's eight NICs (per NIC =
1/8); global = bytes over the dragonfly's global links per node.

| D per node | g | total digits | wall | init | batch | top | dist. levels | recip | division | output | exposed | NIC / global per node | node GB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4e10 | 4 | 1.6e11 | 99.2 s (1.7 min) | 16.4 | 23.0 | 13.1 | 8.4 | 11.6 | 13.3 | 13.3 | 5.6 (6 %) | 4.3 TB / 0 | 352 |
| 4e10 | 64 | 2.56e12 | 120.8 (2.0) | 16.4 | 23.0 | 13.1 | 27.7 | 13.2 | 14.9 | 12.6 | 13.1 (11 %) | 8.4 TB / 0 | 352 |
| 4e10 | 576 | 2.30e13 | 138.9 (2.3) | 16.4 | 23.0 | 13.1 | 47.6 | 13.3 | 11.0 | 14.5 | 18.4 (13 %) | 10.7 TB / 5.5 TB | 352 |
| 8e10 | 4 | 3.2e11 | 218.8 (3.6) | 25.1 | 46.6 | 38.8 | 21.6 | 26.7 | 39.8 | 20.1 | — | — | 512 ✗ |
| 8e10 | 64 | 5.12e12 | 274.4 (4.6) | 25.1 | 46.6 | 38.8 | 71.7 | 30.0 | 44.4 | 17.8 | 34.6 (13 %) | 22.6 TB / 0 | 512 ✗ |
| 8e10 | 576 | 4.61e13 | 317.6 (5.3) | 25.1 | 46.6 | 38.8 | 127.4 | 24.3 | 30.8 | 24.6 | 46.3 (15 %) | 28.1 TB / 14.4 TB | 512 ✗ |
| 1e11 | 4 | 4.0e11 | 311.2 (5.2) | 30.8 | 61.4 | 77.2 | 28.5 | 34.0 | 58.4 | 20.8 | 20.5 (7 %) | 16.4 TB / 0 | 593 ✗ |
| 1e11 | 64 | 6.4e12 | 384.7 (6.4) | 30.8 | 61.4 | 77.2 | 94.5 | 38.2 | 65.1 | 17.5 | 47.8 (12 %) | 31.1 TB / 0 | 593 ✗ |
| 1e11 | 576 | 5.76e13 | 446.1 (7.4) | 30.8 | 61.4 | 77.2 | 168.0 | 36.1 | 45.1 | 27.5 | 63.7 (14 %) | 38.7 TB / 20.2 TB | 593 ✗ |

(✗: does not fit 502 GB by the memory model — the size-1 profile plus the distributed scratch and the shift buffers;
the size-1 run fits 8e10 at 393 GB because it has neither.)

**Where the time is.** The fabric is not the bottleneck: at 576 nodes the exposed communication is 13–15 % of the wall
at every D (the transforms' exchanges are 84 % hidden behind the row passes on xGMI and the fabric stage at 100 GB/s
per APU costs about what the xGMI stage costs, as PLAN §25 anticipated; what stays exposed is the operand
redistributions and the result exchange of every piece, ≈ 0.1 s per 2³¹-point piece, and the latency term of the
small products). The distributed levels are: at 576 nodes there are 8 of them (2, 4, …, 64, then 3-way to 192 and
576) and each costs every node the same local work — two products of a full plane per APU — so they are 34 % of the
wall at 4e10 and 40 % at 8e10, against the fabric's 13 %. Their cost steps with the grid: at D per node just below a
power of two of limbs (n_Q ≤ 2³¹: D ≤ 3.86e10) each level product is ONE piece; above it two, then four, then six:

| D per node | total digits (576) | wall | dist. levels | pieces per level product | node GB |
|---|---|---|---|---|---|
| 2.0e10 | 1.15e13 | 1.3 min | 27 s | 1 | 272 |
| 3.0e10 | 1.73e13 | 1.7 | 31 | 1 | 312 |
| **3.8e10** | 2.19e13 | **2.0** | 33 | 1 | 344 |
| 4.0e10 | 2.30e13 | 2.3 | 48 | 2 | 352 |
| 5.0e10 | 2.88e13 | 2.8 | 50 | 2 | 392 |
| 6.0e10 | 3.46e13 | 3.6 | 84 | 4 | 432 |
| 7.0e10 | 4.03e13 | 4.2 | 94 | 4 | 472 |
| **7.7e10** | **4.44e13** | **4.6** | 96 | 4 | 500 |
| 8.0e10 | 4.61e13 | 5.3 | 127 | 6 | 512 (does not fit) |
| 1.0e11 | 5.76e13 | 7.4 | 168 | 8 | 593 (does not fit) |

**The global tier.** With the layered (two-layer) all-to-all, every exchange over a group larger than the dragonfly
group sends (g'−G)/g' of its bytes over global links: 5.5 TB per node at 4e10 (of 10.7 TB on the NICs), 14.4 of
28.1 TB at 8e10 — the top two tree levels, the reciprocal's last steps and the division. Under the uniform all-to-all
the group's aggregate injection to each peer group is spent evenly, so with taper 1 the global tier is never the
limit; at taper 0.5 with 10 µs messages and 1 GB/s part files (`--taper 0.5 --lat 1e-5 --write-bw 1`) the 576-node
walls become 2.9 / 6.6 min at 4e10 / 8e10 (exposed 36 / 89 s) — still the levels' local work first.

**What the third layer buys** (`--layers 3`, G = 64): messages per APU at 4e10 × 576 fall from 925 k to 322 k, but the
cross-group data is relayed twice inside the groups, so the NIC bytes double (10.7 → 21.6 TB) and the exposed
communication goes 18.4 → 44.7 s (wall 139 → 161 s). The third layer pays only for exchanges below ≈ 200 MB per APU
(latency-bound: the reciprocal's early steps), which X1 sends to small groups instead — so at 576 nodes the two-layer
form with X1 is the better pair; the third layer is worth having only if the message rate, not the bandwidth, turns
out to be the limit of the real fabric (a per-message cost above ≈ 15 µs makes it win at 4e10).

**X1 in the model**: the reciprocal at 4e10 × 576 on the model's groups 13.3 s vs 16.0 s with every product on the
full group (messages per APU 548 k vs 1.53 M; exposed 4.5 vs 6.8 s); at 64 nodes 13.2 vs 13.3 s (nothing: 64 nodes'
latency term is small). A product on the full 576 costs at least a 2³² plane (`logmin = 2 (7 + log₂ gt)`) and 15
exchanges of 575 messages per APU ≈ 0.2 s however small it is; on two nodes the same step costs 10 ms.

### HEADLINE (576 nodes; modelled from the measured per-node profile; extrapolated where marked)

* **The largest digit count: 576 × 7.7 × 10¹⁰ = 4.4 × 10¹³ digits in ≈ 4.6 min per-node wall** (4.2 min without the
  part file's exposed tail) — the largest D per node that fits the memory model (500 of 502 GB: planes 120 + the dm
  pool 223 + distributed scratch 69 + shift buffers 68 + host 20) and just under 2³² limbs of Q, where the levels'
  products are 2 × 2 grids (at 8e10 they are 2 × 3: 4.61 × 10¹³ digits in 5.3 min, but 512 GB — does not fit
  without M's memory work or a smaller distributed scratch). Assumptions: the single-node phases as measured (7e10 /
  8e10 interpolated: measured), the fabric at 100 GB/s per APU and 2 µs per message with the two-layer all-to-all
  (modelled), L's block-cyclic map for the 3-way levels (modelled as 4 products each), B7's `alltoallv` shifts
  (required: the padded buffers are 39 TB per node at 576), the part files at 2 GB/s per node (assumed).
* The safe per-node size: **576 × 3.8 × 10¹⁰ = 2.2 × 10¹³ digits in 2.0 min** (344 GB per node; every level product
  one piece) — measured single-node profile, modelled fabric.
* 576 × 4 × 10¹⁰ = 2.3 × 10¹³ digits in 2.3 min; 576 × 8 × 10¹⁰ = 4.6 × 10¹³ in 5.3 min (memory-limited, see above).

## X1 — the group per product size in the reciprocal and the division (`newton_db.c`)

`recip_mn`: each sharded step j → jn runs on the smallest prefix group [0, 2^L) of the full group G whose modelled
cost is least (`x1_level`: the pieces at the group's cap × (local 1.11 s × q/2²⁹ × the APU sharing + 15 exchanges of
8q (g'−1)/g' bytes at `NEWTON_MN_BW` GB/s per APU with 4 (g'−1) messages of `NEWTON_MN_LAT` s and `NEWTON_MN_FIXED`);
defaults = the target fabric (100 GB/s, 2 µs, 0); `MN_MODEL_TCP=1` = aac6's loopback (0.8 GB/s per APU thread, 1 ms
per exchange, the APUs shared size-way); `NEWTON_MN_GROUPS=0` = every product on G as before). The groups are the
tree's (`mn_group_at(L)`, created by `mn_tree` on every node — no new meshes; node 0's group at level L is [0, 2^L)).
Mechanics: r lives on the current step's group; Q stays on G and Q_t is cut out of it by one exchange over G into the
step's group (`mdb_shift_g`: `mdb_shift` with the target group's sharding — every node of G takes part, the pieces of
nodes outside the target are empty) once per step instead of once per repeat; the step (both products, the shifts, the
adds, the compares, the overshoot path) runs on the group's members only, the other nodes go to the next step's
Q_t exchange and wait there; when the next step's group is larger, r is re-sharded onto it (`x1_regroup`: r's
descriptor from node 0 by an all-gather over the new group, one exchange); after the last step μ is re-sharded onto G.
`newton_mn_divmod`: the two products go through the same rule with the re-sharding of A, B and C counted
(`mn_prod_cut_x1`) — on the target's sizes they always take G (they are ≥ 2 n_Q points); on aac6 they take a subgroup
only at 10⁸ with the TCP constants.

The digits cannot depend on the choice (every sharded operation is exact; the products are the same integers over any
group); size 1 does not enter this code.

### Measurements (batch 3 = `~/x_b2.sh`, job 20815, s24-26, one node, the node-processes sharing it; every run's part files
concatenated and `cmp`'d against `ref/e_<digits>.txt` / `results/e_1e10.out`, VERIFY OK on every node)

| run | env | groups taken by the reciprocal | digits | wall |
|---|---|---|---|---|
| 10⁹ size 1 | — | (size 1: none of this code) | identical | 13.7 s |
| 10⁸ size 2 | `POOL_LOG=27` | full (g = 2 has no subgroup) | identical | 8.5 |
| 10⁸ size 3 | `POOL_LOG=27 MN_MODEL_TCP=1` | full | identical | 9.0 |
| **10⁸ size 4** | `POOL_LOG=27` (target constants) | **[0, 2) for j ≤ 3.5 × 10⁵, then the four** | identical | 8.4 |
| 10⁸ size 4 | `MN_MODEL_TCP=1` | full | identical | 8.4 |
| 10⁸ size 2 | `NEWTON_MN_GROUPS=0` | full (the switch off) | identical | 8.1 |
| 10⁹ size 2 | `POOL_LOG=29` | full | identical | 27.3 |
| **10⁹ size 4** | `POOL_LOG=29` (target constants) | **[0, 2) for j ≤ 4.3 × 10⁵ (4 steps), then the four** | identical | 20.6 |
| 10⁹ size 4 | `MN_MODEL_TCP=1` | full | identical | 20.2 |
| 10¹⁰ size 4 | `POOL_LOG=29 NEWTON_MN_GROUPS=0` (X1 off) | full | identical | **114.6** (dm 73.6, recip 45.1) |
| **10¹⁰ size 4** | `POOL_LOG=29` (target constants) | **[0, 2) for j ≤ 5.4 × 10⁵ (4 steps, 0.55 s together), then the four** | identical | **123.0** (dm 78.4, recip 48.2) |
| **10¹⁰ size 4** | `POOL_LOG=29 MN_MODEL_TCP=1` (aac6's constants: the rule keeps the full group, as the model says it should on a shared node over loopback) | full | identical | **100.6** (dm 60.1, recip 33.4) |

The same 10¹⁰/4 configuration without X1 ran 110.1 s (job 20802) and 105.7 s (job 20809): the node's spread is
± 5 s, and the four subgroup steps cost 0.55 s in the X1 run against 0.57 s for the same steps on the full group (the
loopback transport has no latency term to save, and the local passes on two node-processes instead of four are no
cheaper on a shared node). So on aac6 X1 with the target's constants is neutral on the steps it changes and the wall
difference is noise; with aac6's own constants (`MN_MODEL_TCP=1`, which the calibration says is the rule for this
machine) the rule leaves every product on the full group and the wall is 100.6 s. The first version of the batch
(job 20809) found the bug the size-4 runs exist for: a non-member's `mn_group_at(L)` is its own group [2, 4), so its
pieces of Q_t went to the wrong nodes and the step repeated without end — the target group is now named as
[0, 2^L) on every node (`mdb_shift_g(…, tg0, tg)`).

## X3 — the transform cache's reach

Not started (the time went to the model's calibration and the X1 gate); the mechanism (results/G.md: `rns_dist_cache_hold`
pins Q's pieces from the reciprocal's top step to the division's X Q; `RNS_DIST_CACHE_MN` slots as the pool allows) is in
place and off by default — what X3 would add is a per-product slot budget from the pool's free bytes (M's accounting)
so that A_h μ's pieces do not evict Q's.

## Open issues

* The model's distributed levels assume L's block-cyclic map spreads a 3-way level over all 3m nodes with the
  per-rank points of a binary level; the 9-way top as two 3-way levels costs 4 products each (Q₂Q₃ shared) — 8
  products against 4 for two padded binary levels, at 2/3 the points per rank each; the schedule "…, 64, 576" with
  one 9-way step would be different again. The tables use 192, 576.
* The memory at size > 1 is the size-1 profile plus 69 GB of distributed scratch (M3's 4 q per APU) and the shift
  buffers; M's per-node accounting at size > 1 should replace these two lines (they decide 7.7e10 vs 8e10 per node).
* The per-message cost of a SHMEM put with signal from a host thread is not 2 µs on every implementation; `--lat`
  moves it, and at ≥ 15 µs the third layer starts to pay at 576 (the reciprocal's early steps are X1's already).
* The part file: at size > 1 the `dc` tail is exposed on aac6 (2.4 s at 10¹⁰/4, 5.0 s at 10¹⁰/2: the writer to /tmp),
  hidden at size 1; the model charges max(0, D/write_bw − half the division) per node — the target's file system
  decides.
* X3 (the transform cache's reach across the division's products) is not started.
