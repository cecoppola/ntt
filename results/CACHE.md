# CACHE — the multi-node transform cache

Scope: the multi-node transform cache: what it is worth, its allocation, the partial cache inside 480 GB. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| CX15 | Phase 15 agent CX: what the multi-node transform cache is worth |
| TC15 | Phase 15 agent TC: the multi-node transform cache's allocation (internal target task list T7) |
| PC15 | Phase 15 Batch 3 agent PC: a partial transform cache inside 480 GB (`RNS_DIST_CACHE_PARTIAL`) |

## CX15 — Phase 15 agent CX: what the multi-node transform cache is worth

(former file: CX15.md)

Agent CX. Branch `p15-CX` from `int15f` 2788e1d (TC's `RNS_DIST_CACHE_FIT` on top of main B1 + the Batch 2 switches). aac6 clone
`~/ntt-CX15`, logs in `~/CX15/` (copies of the analysed ones in `archive:results-raw/CX15/`). Node s24-16 only. Times Eastern (aac6 logs are
Central: +1 h). Numbers labelled **measured** / **modelled** / **assumed** / **fitted**.

#### Summary

- **Per hit, the model is about right** (fitted on aac6): a piece whose operand transform is cached saves 1/3 of its transforms'
  time (`ntt` 0.859 → 0.574 s at 4 node-processes: 2 of the 3 transforms per prime remain) and ~45 % of its redistribution — **24–33 %
  of the piece** measured (cx_grid on the target's 9 grid shapes, and ecalc 10¹⁰ end to end at 2 and 4 procs) against **31–32 %**
  modelled (`mn_model.piece_cost`, fwd 2 vs fwd 1, on the aac6 fabric, the same pieces): measured / modelled **1.043, 0.976** (cx_grid,
  2 / 4 procs), **0.965, 0.840** (ecalc, 2 / 4 procs, 4 runs each) → `CACHE_HIT_F = 0.96` (fitted). A miss into a slot costs
  nothing measurable (−0.2..+0.1 s per product).
- **The hits** the code takes at the target's grids (the slot designation, the cuts' skips, simulated exactly by the new
  `mn_model.cache_pieces`; the simulation matches the code's hit counts on all 9 target shapes at 2 and 4 procs): **162** at 2 slots
  where the old term credited **168** (the low-cut division / reciprocal grids lose A's piece 0: A_h mu 5 × 6 gets 15, not 19), and
  **107 at 1 slot**, which the old term credited **none**.
- **At 5.1 × 10¹³ (modelled)**: 2 slots −80.8 s (the old term: −87.5), **1 slot −53.8 s** (the old term: 0); but **neither fits
  480 GB** (one full slot is 68.7 GB per node; the rooms are 44.5 GB in the tree levels incl. the arena's 20.0 GB slack, 34.2 GB in
  the division).
  **With 0 slots — what the budget allows and what `RNS_DIST_CACHE_FIT=1` gives — the target is 434.1 s (7.23 min) without the write,
  453.3 s (7.55 min) with it @ 0.6 GB/s at `ECALC_NP=4`; 406.8 s (6.78 min) / 426.0 s (7.10 min) at `ECALC_NP=auto`** (the standing
  346.6 s assumed 137.4 GB per node that do not exist).
- **What fits (modelled, §3)**: slots holding only some of a piece's primes. A 1-prime slot (17.2 GB per node) fits everywhere: −13.0 s;
  with the grid's loop along its longer axis −16.4 s; a 2-prime slot in the tree (drawn from the arena's slack + the room: 34.4 of
  44.5 GB) and a 1-prime slot in the division: −20.6 s, **−26.7 s with the longer axis** (407.4 s, 6.79 min). Each needs code (§3).
- **End to end on aac6 (measured, §2)**: 10¹⁰ with target-like grids, cap-sized slots (FIT): **−14.4 % (1 slot) / −20.4 % (2 slots)** at
  2 procs, **−10.8 % / −17.7 %** at 4 procs; the default 16 GiB slots at 2 procs **+37.5 %** (memory pressure). Digits identical in
  all 14 runs.
- **TC's contradiction** (§1.1): TC's "wall" column included the digit comparison, which read the 10 GB reference from /shared (NFS)
  when it was not in the node's page cache (93 s after the first 10¹⁰ run, 13 s after the second); the processes' own walls were
  115 s (off) and 140 s (on), in line with `total`. The cache's +26 s in `dm` was the allocation of the default (oversized) slots —
  2 × 12 GiB per APU per process, 4× what POOL_LOG 29 needs — inside the first grid product (8.9 s and 16.8 s on the two processes
  of one node), in a run with only two 1 × 2 grids (one hit each), and the node's memory pressure afterwards (§2: with the default
  16 GiB slots at 2 procs the host-staged shifts take 60–96 s instead of 5; 10¹⁰ is then 37 % slower than without the cache, while
  FIT's cap-sized slots make it 20 % faster).

#### 1. What was measured and how

##### 1.1 TC's runs re-read (measured, TC's logs `~/TC15/m1/`, `~/TC15/m2/`)

TC's `m1.sh` times `wall` from before `mnrun.sh` to after `digcmp.sh` (the packed parts against `results/e_1e10.out` on /shared). The
logs' own start (`META date`) and end (the log's mtime) give the processes' walls and the gaps between runs:

| run (job 21669 / 21687, s24-16) | ecalc process: start → end (EDT) | process wall | `total` | the gap to the next run (digcmp + launch) | TC's `wall` |
|---|---|---|---|---|---|
| e10p2pl29_off | 04:46:54 → 04:48:49 | 115 s | 113.8 s | 93 s (the reference read from /shared, first 10¹⁰ of the job) | 207 s |
| e10p2pl29_on | 04:50:22 → 04:52:42 | 140 s | 139.6 s | 13 s (the reference in the page cache) | 154 s |
| e10p4pl31_def | 04:52:55 → 04:53:42 | (killed: OOM) | — | — | 48 s |
| e10p4pl28_off | 04:53:44 → 04:55:07 | 83 s | 82.4 s | 92 s (the page cache emptied by the OOM kill) | 176 s |
| e10p2pl29_fit (m2) | 05:44:49 → 05:46:41 | 112 s | 111.7 s | 92 s (first 10¹⁰ of the job) | 205 s |
| e10p4pl28_fit (m2) | 05:48:13 → 05:49:31 | 78 s | 76.4 s | ~8 s | 86 s |

So **the walls' 207 vs 154 s is the digit check's reference read (≈ 107 MB/s from NFS), not the run**; the runs themselves agree with
`total`: on is 25.8 s slower than off (measured). Where that comes from (TC's `e10p2pl29_on.log`): `transform cache: 2 of 2 slots of
12.0 GiB per APU allocated in 8.89 s` on one process and **16.84 s** on the other (both hipMalloc 96 GiB at once on one node), inside
the first mn grid product (A_h mu, 1 × 2 pieces: `A mu + shift 21.08 → 48.86 s`); the one hit of each grid saves ~3.6 s (`X Q +
window 18.18 → 14.54`). The default slot is `rns_pool0_np() × 2^(dist_logn_max − 2)` limbs = 12 GiB at three primes, 4× what the
run's POOL_LOG 29 planes need (TC15 §1). With FIT (m2: 3 GiB slots, allocated in 2.04 s) the same run is 111.7 s against 113.8 off.
The rest of the +25.8 s (A mu + shift +27.8 s against the 16.8 s allocation and the hit's −3.6 s; the shifts 4.58 → 7.79 s) is the
node's memory pressure after 192 GB of slots: §2's `c2` (the default 16 GiB slots at four primes, 275 GB on the node) shows it at
scale — the host-staged exchanges (the shifts, the result exchange) slow 12–19× while the transforms do not.

##### 1.2 The target's grids, at a lowered cap (tests/cx_grid; measured, job 21701, s24-16, 08:21–09:03 EDT)

`tests/cx_shapes.py` lists the 5.1 × 10¹³ run's grid products (mn_model's product list, which equals the code's plan) and emits the
**9 distinct grid shapes**, scaled to a cap of 2^27 (`tests/cx_shapes_c27.txt`: tree 2 × 3, 2 × 6, 2 × 2 (the tree's layout: A over
the group's lower half, B over the upper); reciprocal 2 × 2 low+high cut, 1 × 2 low cut, 5 × 3 low+high cut, 2 × 4 low cut; division
5 × 6 low cut (A_h mu), 5 × 6 high cut (X Q)). `tests/cx_grid` forms each over the node-processes of `mnrun.sh` with the cache at 0,
1 and 2 slots **interleaved per shape** (the same operands, one process, `rns_dist_cache_test_slots`; the slots allocated once, at
2, `RNS_DIST_CACHE_FIT=1`), a warm-up and 2 timed repetitions; `RNS_DIST_CACHE_TRACE=1` prints every piece (its cache state, wall,
`redistribute / ntt / crt / out / carry`). Every product's share of C hashed: **identical over the slot counts and repetitions**
(`VERIFY OK` on every process; 0 DIFFERS). `ECALC_NP=4`, decimal limbs. Command:

    SLURM_JOB_ID=$J ./mnrun.sh 2 env LIMB_BASE=10 ECALC_NP=4 RNS_DIST_CACHE_FIT=1 RNS_POOL_GROW=1 RNS_DIST_CACHE_TRACE=1 \
        ECALC_VERBOSE=1 ./tests/cx_grid 26 2 0,1,2 tests/cx_shapes_c27.txt        (4 procs: pool_log 25, the same cap 2^27)

**Products** (node 0, mean of 2, seconds; `archive:results-raw/CX15/fit_p2.txt`, `fit_p4.txt`; measured):

| shape (target's) | 2 procs: 0 / 1 / 2 slots | saved 1 / 2 slots | 4 procs: 0 / 1 / 2 slots | saved 1 / 2 slots | hits 1 / 2 slots |
|---|---|---|---|---|---|
| tree 2 × 3 | 15.00 / 12.70 / 11.12 | −15 / −26 % | 6.16 / 5.30 / 4.58 | −14 / −26 % | 3 / 5 |
| tree 2 × 6 | 30.74 / 25.49 / 22.11 | −17 / −28 % | 12.66 / 10.88 / 9.22 | −14 / −27 % | 6 / 11 |
| tree 2 × 2 | 10.48 / 8.46 / 7.46 | −19 / −29 % | 4.00 / 3.30 / 3.01 | −17 / −25 % | 2 / 3 |
| recip 2 × 2 low+high | 9.95 / 8.54 / 7.89 | −14 / −21 % | 3.96 / 3.33 / 2.97 | −16 / −25 % | 2 / 3 |
| recip 1 × 2 low | 5.29 / 5.46 / 4.49 | +3 / −15 % | 1.95 / 1.95 / 1.62 | 0 / −17 % | 0 / 1 |
| recip 5 × 3 low+high | 34.00 / 26.32 / 25.27 | −23 / −26 % | 13.26 / 10.14 / 9.69 | −23 / −27 % | 10 / 11 |
| recip 2 × 4 low | 17.57 / 15.50 / 13.54 | −12 / −23 % | 7.28 / 6.33 / 5.79 | −13 / −20 % | 3 / 5 |
| div A_h mu 5 × 6 low | 57.18 / 41.11 / 38.79 | −28 / −32 % | 21.40 / 17.14 / 16.66 | −20 / −22 % | 14 / 15 |
| div X Q 5 × 6 high | 51.05 / 43.24 / 35.84 | −15 / −30 % | 20.23 / 15.89 / 14.26 | −21 / −29 % | 14 / 19 |
| **all 9** | **231.3 / 186.8 / 166.5** | **−19.2 / −28.0 %** | **90.9 / 74.3 / 67.8** | **−18.3 / −25.4 %** | 54 / 73 |

The hits per product equal `mn_model.cache_pieces`' simulation of the code on every shape at both process counts (and the target's own
shapes give the same counts: `archive:results-raw/CX15/shapes_51e13.txt`).

**Pieces** (node 0; every piece against itself at 0 slots in the same repetition; mean seconds; measured):

| | n | piece wall | redistribute | ntt |
|---|---|---|---|---|
| 2 procs, hit (1 slot) | 108 | 2.641 → 1.783 | 0.106 → 0.063 | 2.423 → 1.605 |
| 2 procs, hit (2 slots) | 146 | 2.635 → 1.743 | 0.105 → 0.059 | 2.413 → 1.565 |
| 2 procs, cached miss (1 / 2 slots) | 68 / 30 | 2.605 → 2.657 / 2.587 → 2.612 | ≈ | 2.383 → 2.437 / 2.379 → 2.407 |
| 4 procs, hit (1 slot) | 108 | 1.031 → 0.719 | 0.068 → 0.039 | 0.859 → 0.574 |
| 4 procs, hit (2 slots) | 127 | 1.035 → 0.718 | 0.068 → 0.037 | 0.860 → 0.573 |
| 4 procs, cached miss (1 / 2 slots) | 68 / 29 | 1.034 → 1.037 / 1.037 → 1.036 | ≈ | ≈ |

A hit takes one third off the piece's transforms (the operand's forward transform is 1 of the 3 per prime) and ~45 % off its
redistribution (one of the two operands). **The fit** (`tests/cx_fit.py grid`): the hit pieces' measured saving against
mn_model's (piece_cost at fwd 2 − fwd 1, on `aac6_fabric('tcp', g)`, the same pieces): 0.878 s vs 0.792 s (2 procs, n 254), 0.315 vs
0.349 s (4 procs, n 235) — in seconds 1.108 / 0.903, but the aac6 fabric's constants were fitted on whole runs, not pieces, so the fit
takes the saving **as a fraction of the piece: 33.3 % measured vs 31.9 % modelled → 1.043 (2 procs), 30.5 % vs 31.2 % → 0.976 (4 procs)**.
With §2's end-to-end ratios (0.965 at 2 procs, 0.840 at 4, means of 4 runs) the mean is **0.956 → `CACHE_HIT_F = 0.96`** (fitted;
assumed to carry to the target's fabric: the saving's structure — one of three transforms per prime, one redistribution — is the
same there; the aac6 loopback only weights it differently, its pieces being ~90 % exchange, the target's ~80 % local, archive:results-raw/CX15/shapes_51e13.txt).

##### 1.3 The transforms' parts (DIST_STATS=1; measured, job 21733, 12:44–12:50 EDT)

cx_grid at 4 procs, 0 and 2 slots, `DIST_STATS=1` (the transforms' kernels event-timed per APU, the exchanges host-timed; `archive:results-raw/CX15/j4/
fit_p4stats.txt`). The products and the hits repeat §1.2 (all 9: 89.4 → 67.3 s, −24.8 %; a hit 30.3 % of its piece vs 31.2 % modelled →
0.971). Per hit piece (n 73, against itself at 0 slots): rows 0.005 → 0.003 s, cols 0.004 → 0.003, packs 0.002 → 0.002, the exchange
stage (event) 0.656 → 0.438, the all-to-alls (host) 0.826 → 0.552 — **both parts drop by a third**, but on aac6 the transforms are
~99 % exchange (the loopback TCP), so the local kernels' third is 3 ms per piece here: the target's local share of the saving (~80 %,
modelled) rests on the single-node measurement behind `T_PIECE_31_BHIT` (results/GRID_GROUPS.md §G: 1.11 → 0.72 s at 2^31, the operand's load and
forward transform), not on these runs.

#### 2. ecalc end to end with target-like grids (measured)

`~/CX15/j2.sh <tag> <procs> 10000000000 <POOL_LOG> 2 <cfgs>`: ecalc 10¹⁰ over node-processes on s24-16, `ECALC_NP=4 POOL_LOG=<pl>
RNS_POOL_GROW=1 RNS_DIST_CACHE_TRACE=1 ECALC_VERBOSE=1 MEM_REPORT_DEVS=1` + the configuration, interleaved (c0 f1 f2 c2, c0 f1 f2 c2);
the wall is `mnrun.sh` alone, the digits compared after it (`digcmp.sh` against `results/e_1e10.out`). Configurations: **c0**
`RNS_DIST_CACHE_MN=0`; **f1 / f2** `RNS_DIST_CACHE_MN=1 / 2 RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_FIT_RESERVE_GB=0` (slots sized to the
cap: 1 GiB per APU here); **c2** `RNS_DIST_CACHE_MN=2` (the default slot size, 16 GiB per APU at four primes).

**10¹⁰ at 2 node-processes, POOL_LOG 27** (job 21703, 10:32–11:13 EDT; the grids: tree 2 × 3 twice, reciprocal 3 × 1, 1 × 2, 5 × 2,
2 × 3, division 4 × 5 twice — 58 grid pieces of 82; `archive:results-raw/CX15/j2/`). Every run **identical** to the reference, 3 × VERIFY OK:

| config | `total` run 1 / run 2 (s) | mean | vs c0 | bs / dm (mean) | grid pieces: sum (node 0), hits | shifts (68) | cache allocation |
|---|---|---|---|---|---|---|---|
| c0 | 278.89 / 274.28 | 276.6 | — | 57.5 / 211.4 | 235.6 s, 0 | 5.0 s | — |
| f1 | 232.26 / 241.11 | 236.7 | **−14.4 %** | 50.4 / 178.4 | 193.9 s, 34 | 4.9 s | 1 × 1 GiB/APU, 0.25–0.39 s |
| f2 | 221.24 / 218.89 | 220.1 | **−20.4 %** | 44.1 / 168.7 | 177.0 s, 45 | 5.1 s | 2 × 1 GiB/APU, 0.62–0.70 s |
| c2 | 350.86 / 409.69 | 380.3 | **+37.5 %** | 104.9 / 267.5 | 202.4 s, 45 | **60.0 / 95.7 s** | 2 × 16 GiB/APU, 10.4–16.9 s |

(the walls are `total` + 1.4–2.0 s in every run.) Against the same piece in c0 run 1, the hit pieces of f1 save 45.5 s (34 hits) and of
f2 52.7 s (45 hits); mn_model on the aac6 fabric gives 44.0 / 58.1 s — **ratios 1.035 / 0.907**, the cx_grid fit again.

**c2 — the default slot size — is slower than no cache**, and that is TC's contradiction at scale: the two processes' 2 × 16 GiB per
APU (275 GB on the node) are allocated in 10–17 s inside the first tree product, and then everything that goes through host memory
slows: the 68 `mdb_shift`s take 60–96 s instead of 5 (S = P + Q 6.0 s instead of 0.8; the grid pieces' result exchange `out` 0.44–0.86 s
instead of 0.15), while the products' transforms are as fast as f2's (hit pieces 2.99–3.07 vs 2.71–2.84 s). The node's free memory
is what `cache_avail` checks (per process, margin 24 GB); the host side (the TCP transport's buffers, the shifts' staging) is then
starved. TC's `on` run (2 × 12 GiB at three primes, 192 GB on the node) showed the same, smaller: allocation 16.8 s + shifts 4.6 → 7.8 s.

**10¹⁰ at 4 node-processes, POOL_LOG 26** (job 21725, 11:39–11:55 EDT; the grids: tree level 1 2 × 2 over groups of 2, level 2 2 × 3,
reciprocal 3 × 1, 1 × 2, 5 × 2, 2 × 3, division 4 × 5 twice — 66 grid pieces on node 0; `archive:results-raw/CX15/j3/`). All **identical**, 5 × VERIFY OK:

| config | `total` run 1 / run 2 (s) | mean | vs c0 | bs / dm (mean) | grid pieces: sum (node 0), hits | shifts (69) |
|---|---|---|---|---|---|---|
| c0 | 146.66 / 146.32 | 146.5 | — | 54.6 / 86.1 | 111.8 s, 0 | 2.4 s |
| f1 | 133.11 / 128.39 | 130.8 | **−10.8 %** | 48.5 / 76.2 | 97.7 s, 38 | 2.3 s |
| f2 | 120.58 / 120.66 | 120.6 | **−17.7 %** | 43.9 / 70.7 | 89.1 s, 51 | 2.3 s |

(the walls are `total` + 1.3–1.4 s.) (c2 at 4 procs was not run: 4 × 128 GiB of default slots exceed the node; TC's `e10p4pl28_on` died of it, OOM in `cache_avail`.)

The hit pieces against themselves in c0 run 1 (`tests/cx_fit.py e2e`, `archive:results-raw/CX15/j2/e2e.txt`, `j3/e2e.txt`): **as a fraction of the
piece, 29.2–32.9 % measured vs 32.2 % modelled at 2 procs (ratios 1.021, 0.907, 0.920, 1.012), 24.1–28.6 % vs 31.8 % at 4 procs (0.757,
0.837, 0.864, 0.901)**; c2's hits 23.4–25.5 % (0.79, 0.73: the memory pressure again).

#### 3. The model re-fit, the 5.1 × 10¹³ estimate, and ways to hold a slot

##### 3.1 mn_model.py's cache term

- `MN_MODEL_CACHE=code` (the default now): `_product_cost` takes the hits from `cache_pieces` (the code's `mn_grid` / `cache_plan`
  rules: A's pieces i < min(ka, N − 1) in their slots, B's piece j in slot nA + j % nB, lookups at the piece's own q, the cuts'
  skips), at `CACHE_MN_SLOTS` slots (env `MN_MODEL_CACHE_SLOTS`, default 2 as the code's `RNS_DIST_CACHE_MN`); a hit saves
  `CACHE_HIT_F` (0.96, fitted) of piece_cost's fwd 2 − fwd 1. At the target a hit saves 0.40–0.58 s of a 1.25–1.81 s piece (modelled
  × 0.96; ~80 % of it the local forward transform, ~20 % the transform's exchanges, ~3 % the redistribution: `archive:results-raw/CX15/shapes_51e13.txt`). `MN_MODEL_CACHE=old` gives the term before (every piece after the
  first cached at ≥ 2 slots, nothing at 1).
- The proposals' knobs (off unless set): `CACHE_PRIMES` (a slot of k of the piece's primes: k / np of the transforms' saving, no
  redistribution saved), `CACHE_LOOP='long'` (the loop along the longer axis), `CACHE_SLOTS_PHASE` (slots per phase);
  `./mn_model.py --cache-proposals` prints §3.3.

##### 3.2 The 5.1 × 10¹³ estimate (modelled; `estimate.py --target`, fabric assumed as in TARGET.md; `archive:results-raw/CX15/estimate_target_*.txt`)

| cache | ECALC_NP | 5.1e13 without the write | with the write @2.0 / 0.8 / **0.6** GB/s | 4.74e13 without / @0.6 | distributed levels / recip / div |
|---|---|---|---|---|---|
| old term, 2 slots (the standing number) | 4 | 346.6 s (5.78 min) | 341.2 / 361.4 / 377.8 s | 322.0 / 352.9 s | 150.9 / 44.0 / 50.3 |
| code's hits × 0.96, 2 slots (137.4 GB/node: does not fit) | 4 | 353.3 s (5.89 min) | 347.9 / 366.3 / 382.7 s | 328.3 / 357.6 s | 153.1 / 45.5 / 53.3 |
| code's hits × 0.96, 1 slot (68.7 GB/node: does not fit) | 4 | 380.3 s (6.34 min) | — / — / 407.9 s | | 174.4 / 48.1 / 56.4 |
| **0 slots (what 480 GB allows; FIT gives 0)** | **4** | **434.1 s (7.23 min)** | 428.7 / 436.9 / **453.3 s (7.55 min)** | 401.2 / 422.1 s | 205.6 / 56.0 / 71.1 |
| **0 slots** | **auto** | **406.8 s (6.78 min)** | 401.4 / 409.6 / **426.0 s (7.10 min)** | 374.6 / 395.5 s | 188.6 / 55.7 / 71.1 |

##### 3.3 Holding a slot inside 480 GB (modelled; `archive:results-raw/CX15/proposals.txt`)

The rooms per node at 5.1 × 10¹³ (mem_model, the standing figures): the tree levels 480 − (bs device 411.0 + host 44.4) + the
arena's slack at the tree (arena 284.1 − tree need 264.2 = 20.0) = **44.5 GB**; the division 480 − (device 417.0 + host 28.8) =
**34.2 GB** (the arena is sized to the division's need: no slack). One prime of a slot at the cap (q = 2^29 at every target grid) is
2^29 × 8 B × 4 APUs = **17.2 GB per node**; a four-prime slot 68.7 GB.

| configuration (ECALC_NP=4; under auto, 406.8 s at 0 slots, the full slots gain 5–6 % less, the partial ones within 0.2 s the same: `archive:results-raw/CX15/proposals.txt`) | slot GB per node, tree / division | fits | without the write | with the write @0.6 | gain |
|---|---|---|---|---|---|
| the old term, 2 slots (the standing number) | 137.4 / 137.4 | no | 346.6 s | 377.8 s | −87.5 s |
| 2 full slots (the code's default) | 137.4 / 137.4 | no | 353.3 s | 382.7 s | −80.8 s |
| 1 full slot | 68.7 / 68.7 | no | 380.3 s | 407.9 s | −53.8 s |
| 1 full slot, the loop along the longer axis | 68.7 / 68.7 | no | 366.4 s | 394.7 s | −67.7 s |
| 1 slot of three primes ("three-prime slots under auto": auto forms three-prime pieces only at tree levels 1–4, g ≤ 16 — pieces below the three-prime bound 5.84e10 limbs; every other grid piece is four-prime) | 51.5 / 51.5 | no | 395.0 s | 420.3 s | −39.1 s |
| 1 slot of 2 primes, both phases | 34.4 / 34.4 | no (the division by 0.2 GB) | 408.0 s | 431.3 s | −26.1 s |
| **1 slot of 1 prime** | 17.2 / 17.2 | **yes** | 421.1 s | 442.3 s | **−13.0 s** |
| **1 slot of 1 prime, the longer axis** | 17.2 / 17.2 | **yes** | 417.7 s | 439.1 s | **−16.4 s** |
| **tree: 1 slot of 2 primes (arena slack + room); division: 1 of 1 prime** | 34.4 / 17.2 | **yes** | 413.5 s | 436.7 s | **−20.6 s** |
| **the same, the longer axis** | 34.4 / 17.2 | **yes** | **407.4 s (6.79 min)** | **430.9 s (7.18 min)** | **−26.7 s** |
| tree: 2 slots of 1 prime; division 1 of 1 | 34.4 / 17.2 | yes | 415.9 s | 437.1 s | −18.2 s |

What each needs (none is built; all are proposals):
- **k-prime slots** (`RNS_DIST_CACHE_PRIMES=k`, say): `cache_plan` / `mn_core` keep the slot's planes for primes < k only; a hit
  skips those primes' gathers and forward transforms; the operand's redistribution and its other primes stay (so the saving is
  k / np of the transform part, no redistribution saved — the model prices it so). Memory: k × 4.3 GB per APU.
- **the loop along the longer axis**: `mn_grid` loops j outer / i inner, so B's piece j is reused across the ka − 1 other pieces; when
  kb > ka (every target tree grid: 2 × 3, 2 × 6), looping i outer and caching A's piece i gives ka (kb − 1) reuses instead of kb (ka − 1)
  (1 slot at 2 × 6: 10 hits instead of 6). The digits do not depend on the order (exact adds); no memory.
- **a slot from the arena's slack** (the tree only): the block pool has 20.0 GB unused at the tree levels; drawing the slot there per
  grid product (instead of one hipMalloc held to the division's end) makes the tree's room 44.5 GB; the division has none.
  `RNS_DIST_CACHE_FIT`'s budget bound (TC) then needs the per-phase room instead of one figure.
- **the allocation**: hipMalloc at ~11 GB/s (measured: 96 GiB in 8.9 s, twice that with two processes at once) — 17.2 GB per node
  costs ~1.6 s once (modelled at 0.055 s/GB, results/GRID_GROUPS.md §G's mapping rate); not in the gains above.

#### 4. Tests run (all on s24-16, `~/ntt-CX15` at 75cdb2f: the code of this branch; later commits change only the model, tests' scripts and the report)

| test | command | result | job, time (EDT) |
|---|---|---|---|
| cx_grid 2 procs | `./mnrun.sh 2 env LIMB_BASE=10 ECALC_NP=4 RNS_DIST_CACHE_FIT=1 RNS_POOL_GROW=1 RNS_DIST_CACHE_TRACE=1 ECALC_VERBOSE=1 ./tests/cx_grid 26 2 0,1,2 tests/cx_shapes_c27.txt` | 54 timed products + 9 warm-up; every share identical over 0/1/2 slots and repetitions; VERIFY OK on both | 21701, 08:21–08:51 |
| cx_grid 4 procs | the same with 4, pool_log 25 | 53 of 54 timed products (the job's time ran out on the last); all identical; 0 DIFFERS | 21701, 08:51–09:03 |
| ecalc 10¹⁰, 2 procs, c0 f1 f2 c2 × 2 | `~/CX15/j2.sh j2 2 10000000000 27 2 c0,f1,f2,c2` (= `./mnrun.sh 2 env ECALC_NP=4 POOL_LOG=27 RNS_POOL_GROW=1 RNS_DIST_CACHE_TRACE=1 ECALC_VERBOSE=1 MEM_REPORT_DEVS=1 <cfg> ./ecalc 10000000000 /tmp/cx15/<tag>.txt`, then `digcmp.sh` against `results/e_1e10.out`) | 8 runs identical, 3 × VERIFY OK each; §2 | 21703, 10:32–11:13 |
| ecalc 10¹⁰, 4 procs, c0 f1 f2 × 2 | `~/CX15/j2.sh j3 4 10000000000 26 2 c0,f1,f2` | 6 runs identical, 5 × VERIFY OK each; §2 | 21725, 11:39–11:55 |
| gates: mnaccept | `./mnaccept.sh 21733 --only unit,e9,mn` (unit = t_ntt, t_mul, t_bs, t_dbig, t_newton, t_verify, t_out, t_mn_grid; e9 both bases; mn e8 at 2/3/4, e9 at 2/4) | **15 PASS, 0 FAIL** (1307 s) | 21733, 12:15–12:37 |
| gates: 4 × 10¹⁰ | `ecalc 40000000000` at size 1 (the reference evicted from the page cache), `digcmp.sh` against `results/e_4e10.out` | **identical**, `total` 61.69 s | 21733, 12:37–12:44 |
| cx_grid 4 procs, DIST_STATS | `./mnrun.sh 4 env ... DIST_STATS=1 ./tests/cx_grid 25 1 0,2 tests/cx_shapes_c27.txt` | 18 timed products, all identical, VERIFY OK; §1.3 | 21733, 12:44–12:50 |

#### 5. Open issues

- The fit is on aac6's loopback fabric (pieces ~99 % exchange, 2 and 4 processes sharing 4 APUs); `CACHE_HIT_F = 0.96` is assumed to
  carry to the target's fabric, where the saving is mostly the local forward transform (35 % of the piece's local time, results/GRID_GROUPS.md §G's
  single-node measurement) — the aac6 runs confirm the third of the transforms, not the local share's size.
- The standing estimate: `estimate.py --target` still prices `CACHE_MN_SLOTS = 2` by default (the code's `RNS_DIST_CACHE_MN` default),
  now **353.3 s** with the code's hits × 0.96 (was 346.6 s with the old term); **the budget allows 0 slots → 434.1 s (7.23 min), 453.3 s
  (7.55 min) with the write** (`MN_MODEL_CACHE_SLOTS=0`). Whether the default should be 0 (or FIT's figure) in the model is the user's call.
- The code's default (`RNS_DIST_CACHE_MN=2`, slots at 2^(dist_logn_max − 2)) is harmful wherever the slots are oversized or the node is
  shared: at 10¹⁰ on 2 processes it made the run 37 % slower (§2), and at the target it would take 137.4 GB per node that 480 GB does not
  have (TC). `RNS_DIST_CACHE_FIT=1` fixes both (0 slots at the target; cap-sized slots on aac6: −18..−20 %). Adoption is the user's.
- The proposals of §3.3 are modelled only; none is built. The best that fits is −26.7 s (a 2-prime slot in the tree from the arena's
  slack, a 1-prime slot in the division, the loop along the longer axis).

#### RESUME

- Step 0 (done, 75cdb2f): `RNS_DIST_CACHE_TRACE=1` (rns_dist.c `mn_grid`: one line per grid piece; off by default) and
  `rns_dist_cache_test_slots` (tests only); `tests/cx_grid.c` + `tests/cx_build.sh`, `tests/cx_shapes.py` (+ `cx_shapes_c27.txt`),
  `mn_model.cache_pieces`; README row (1043c3c).
- Step 1 (done): job 21701 (cx_grid p2, p4) → `archive:results-raw/CX15/j1_p*.log`, `fit_p*.txt`.
- Step 2 (done): jobs 21703 (10¹⁰ p2) and 21725 (10¹⁰ p4) → `archive:results-raw/CX15/j2/`, `j3/`; the model: the 'code' term, `CACHE_HIT_F = 0.96`,
  `--cache-proposals`, the estimates (`archive:results-raw/CX15/`).
- Step 3 (done): job 21733 (gates: mnaccept unit,e9,mn 15/15, 4e10 identical; the DIST_STATS pass) → `archive:results-raw/CX15/j4/`.
- Done. No job of CX is queued or running. The aac6 clone `~/ntt-CX15` is at 75cdb2f (the code; later commits are model / scripts / report).

## TC15 — Phase 15 agent TC: the multi-node transform cache's allocation (internal target task list T7)

(former file: TC15.md)

Agent TC. Branch `p15-TC` from `int15e` 6990778. aac6 clone `~/ntt-TC15` (bundle from `~/ntt` f184d51), logs in `~/TC15/`.
Times Eastern (aac6 logs are Central: +1 h). Numbers labelled **measured** / **modelled** / **assumed**.

#### 1. What the code allocates (read from `rns_dist.c` at 6990778)

**The allocation** (`cache_avail`, `rns_dist.c` ≈ 239–262): at the first product that wants the cache, once per process
(`g_cache.tried`), `n` slots, each **one `hipMalloc` per APU of `cache_slot_bytes()` = `rns_pool0_np()` × 2^(`dist_logn_max()` − 2)
× 8 B**, the four APUs in parallel. `dist_logn_max()` is the single-node tier's cap (31 unless `DIST_LOGN_TEST` / a tree cap test
lowers it): **16 GiB per slot per APU at four primes, 12 GiB at three**, whatever the run's `POOL_LOG`, group or product sizes.
`n` = `RNS_DIST_CACHE_MN` (default 2) for the mn tier, cut to what every APU's `hipMemGetInfo` free memory minus
`RNS_DIST_CACHE_MARGIN_GB` (24 GB) allows, then (in `mn_grid`) to the group's minimum over its nodes.

**When**: `mn_grid` asks for the slots only for a grid product (`nc > cap`: more than one piece) or a pinned hold
(`RNS_DIST_CACHE_HOLD=1`, off). The first mn grid product of a run triggers the allocation. The mn tier's slot use is
q limbs per prime per rank, q ≤ 2^(c − 2) with c = min(`dist_logn_max()`, `rns_pool_log()`) (`mn_logn_cap`); so the slot is
**oversized by 2^(31 − pool_log)** whenever `POOL_LOG` < 31 (e.g. 64× at `POOL_LOG=25`), and exact at the target (pool_log 31).

**Freed**: only by `rns_dist_cache_release()` — `newton_db.c` 265 / 344 (single-node dm) and **858 (the mn division's end)** —
and by `rns_dist_cap_test`. There is no release after the tree: an allocation at a tree level stays through the reciprocal
and the division.

**Not counted anywhere** (checked):
- the memory report (`mem_report`): the cache registers no `mem_acct` provider, so `device … in use` omits it (only
  `MEM_REPORT_DEVS=1`'s `(driver: used of total)` sees it);
- `ECALC_BUDGET_CHECK` (`ecalc.c budget_check`): runs at init, before any cache exists, and its peak
  (`binsplit_node_bytes`: plane pools + arena + tables + host) has no cache term;
- `BS_LAYOUT_ONLY` / `mem_model.py` (`--p15`, `--check-c`): no cache term (`grep -i cache mem_model.py` finds only `lru_cache`);
- `mn_model.py` counts it **only as time**: `CACHE_MN_SLOTS = 2` — the modelled walls assume the two slots' hits.

**At the target** (`MN_PLAN_ONLY`, archive:results_raw_2026-10-19/TGT15/plan576_51e13.txt, modelled from the code): every tree level from 1 (group 2)
to 8 (576) forms grid products at the cap (`2^32` at g 2 … `2^40` at g 576), so **the cache is requested at tree level 1**,
in the bs phase, and would stay to the division's end. The slot there: 4 primes × 2^29 × 8 = **16 GiB per APU (17.18 GB)**;
at g = 576, q = 456 × 2^20 limbs uses 89 % of it. Configured 2 slots = **128 GiB = 137.4 GB per node** (modelled).
The same at 4.74 × 10¹³ (tree level 1 is a 2 × 2 grid at 2^32 there too).

**Against the 455.4 GB node figure** (internal target notes §1, `mem_model.py --p15`): the figure **does not include the cache**.
With the configured two slots the node would be 455.4 + 137.4 = **592.8 GB**; with one slot **524.1 GB** — both over 480
(and 2 slots over the node's 512 GiB = 549.8 GB) (modelled). At 4.74 × 10¹³: 439.2 + 137.4 = 576.6 GB, one slot 507.9 GB.
Whether the code actually takes them depends only on what `hipMemGetInfo` reports free at tree level 1 (the only guard):
§2 measures that relation on s24-16.

**What the cache is worth at the target (modelled; `mn_model.py` prices it, `CACHE_MN_SLOTS = 2`).** `estimate.py --target` at this
branch, as is and with `mn_model.CACHE_MN_SLOTS = 0` patched in (archive:results_raw_2026-10-19/TC15/estimate_target_cache{2,0}.txt; the fabric assumed as in
TARGET.md):

| total digits | without the write, cache 2 slots (as modelled) | no cache | difference | @ 0.6 GB/s: 2 slots / none |
|---|---|---|---|---|
| **5.1 × 10¹³** | 346.6 s (5.78 min) | **434.1 s (7.23 min)** | **+87.5 s (+25 %)** | 377.8 / 453.3 s |
| 4.74 × 10¹³ | 322.0 s | 401.2 s | +79.2 s | 352.9 / 422.1 s |
| 4.25 × 10¹³ (the old target) | 257.4 s | 313.8 s | +56.4 s | 286.8 / 335.0 s |

(346.6 s here vs TARGET.md's 344.7 s: the model at int15e 6990778, with the Batch 2 branches merged.) By phase at 5.1 × 10¹³: the
distributed levels 150.9 → 205.6 s, the reciprocal 44.0 → 56.0 s, the division 50.3 → 71.1 s. Without the cache every piece of a
grid redistributes and forward-transforms both operands; with two slots B's piece j is kept across i and A's piece 0 across j. So
**the standing 5.1 × 10¹³ time assumes 137.4 GB per node that the 455.4 GB node figure does not hold** — one of the two numbers is
wrong: either the run takes the slots (the node is over 480 GB) or it does not (the wall is ≈ 87 s longer than modelled).

#### 2. Measured on s24-16 (batch m1: job 21669, 04:46–04:59 EDT; batch m2: job 21687, 05:44–06:18 EDT, 2026-09-28)

Logs are on aac6 in `~/TC15/m1/` and `~/TC15/m2/`; the summaries are in archive:results_raw_2026-10-19/TC15/m1.out and m2.out. Every run used `MEM_REPORT_DEVS=1 ECALC_VERBOSE=1`,
TCP loopback through `mnrun.sh`, and three primes. At the default `POOL_LOG=31`, 10⁹ and 10¹⁰ at 2 and 4 processes have **no mn grid product**, so the cache is
never requested (`plan cache … not wanted`). To exercise it, the pools were lowered as agent G5 did. Every figure below is measured, from single runs.

| run (procs, POOL_LOG) | cache | slots per APU (alloc time) | `total` (s) | dm (s) | division products (s) | hits / lookups | digits |
|---|---|---|---|---|---|---|---|
| 10¹⁰, 2, 29 | off | – | **113.78** | 83.56 | 37.72 | – | identical |
| 10¹⁰, 2, 29 | default | 2 × 12.0 GiB (8.89 s; 16.84 s on the second process) | **139.58** | 109.81 | 58.01 | 3 / 12 | identical |
| 10¹⁰, 2, 29 | FIT | 2 × 3.0 GiB (2.04 s) | **111.67** | 82.29 | 32.77 | 3 / 12 | identical |
| 10¹⁰, 4, 28 | off | – | 82.41 | 48.95 | 24.94 | – | identical |
| 10¹⁰, 4, 28 | default | 2 × 12.0 GiB (12.03 s) | **rc 6: `HIP out of memory at rns_dist.c:255`** on rank 2, after the tree | – | – | – | – |
| 10¹⁰, 4, 28 | FIT | 1 × 1.5 GiB (0.5–0.8 s; the budget bound: room 18.8 − 8.0 GB → 1) | 76.37 (bs 20.8 vs 26.8 off: bs noise) | 48.31 | 20.78 | 0 / 6 | identical |

**Which figure is right: `total`, not the wall.** In the e10p2pl29 pair, `total` is 113.8 s with the cache off and 139.6 s with it on. The script's walls read the opposite,
207 s and 154 s. The wall is not an ecalc time. It covers `mnrun.sh` (its `MN_PLAN_ONLY` call, the launch) plus `digcmp.sh`, which reads the 10 GB reference
`e_1e10.out` from Lustre. The first 10¹⁰ run of each batch reads that reference cold. The off run was m1's first 10¹⁰ run (wall 207 s), and FIT was m2's first
(wall 205 s against `total` 111.7 s). Later runs found it in the page cache. ecalc's own `total` and its phase lines are the measurement.

**What the default cache does at these sizes: it costs time.** With the default rule, 10¹⁰ at 2 processes is **+25.8 s** (+23 %) slower than with the cache off:
- The allocation sits inside the division's first grid product (`A mu + shift` 48.86 s vs 21.08 s). It is 2 slots × 12 GiB × 4 APUs per process, 103 GB per
  process and 206 GB for the node, where the products need 3 GiB per slot. It took 8.9 s on one process and 16.8 s on the other, which waited.
- The division products then ran slower as well (58.0 s vs 37.7 s; shifts 7.79 s vs 4.58 s).
- At 4 processes the same rule ran out of memory. Every process read `hipMemGetInfo` before the others had allocated, and 4 × 2 × 12 GiB = 96 GiB per APU came on
  top of the pools. This is G5's failure, reproduced.

**What the cache buys when it is sized (FIT).** At 2 processes, 3 hits in 12 lookups saved **2.1 s in `total`**. The division products gained 4.9 s
(37.7 → 32.8 s), but the reciprocal read +3.5 s, which is noise. At 4 processes FIT gave one slot and **0 hits**: `mn_grid` puts one slot on B's pieces only when
there are ≥ 2 slots (nA = min(ka, nf − 1) = 0), so a 1 × 2 grid gets nothing from it, and dm was the same (48.3 vs 48.9 s). These are 1 × 2 grids over 2 or 4
loopback nodes, one sample each, within the ±3 s spread of dm.

**The other failures are not the cache.**
- **rc 6 at POOL_LOG 24 and 25** (all 10⁹ runs: on, off and FIT alike): `rns_dpool: plane pool 0 … would grow inside a phase, 0.20 -> 0.27 GB: the pools are sized
  at init and must not grow (RNS_POOL_GROW=1 allows it)`. At pools this small, the mn tier's planes exceed pool 0. This limit existed before this work (mnaccept's
  stress recipe sets `RNS_POOL_GROW=1` for the same reason), and it hit the cache-off runs identically. These points give no data.
- **rc 6 in e10p4pl28_on**: the cache's out-of-memory, above.
- **rc 137 in e10p4pl31_def** (4 processes at `POOL_LOG=31`): `srun: task 1: Killed`, the kernel's OOM killer during init. Each process maps a 195.8 GB layout
  (the plan's node figure; modelled), 783 GB for four on one node. That configuration cannot fit one node, and the run reached no mn product. It was a baseline
  probe for the driver's figures, not a cache run.

**What `hipMemGetInfo` sees (the default rule's only guard).**
- It is node-wide per APU: total **137.4 GB (128 GiB) per APU**, and the processes' allocations add up. Two processes of 20.7 GB each show 41.2 GB used.
- It **does not count the arena's region bytes** under `DB_POOL_VMM`. In the `ECALC_INIT_ONLY` probes, the driver's figure equals planes + pool + tables exactly
  (1 × 10¹¹: APU0 in use 108.1 GB, driver 68.9 GB = 108.1 − regions 39.2; APU1 89.7 vs 58.2 = 89.7 − 31.6).
- At the top node's rehearsal layout (9.17 × 10¹⁰, `ECALC_NP=4`, one process, device 379.7 GB), the driver shows 56–71 GB used per APU. That leaves 66–81 GB free,
  which is 42–57 GB after the 24 GB margin: room for two 17.2 GB slots on every APU.
- If the target's node looks the same at tree level 1, the default takes both slots and the node goes to ≈ 590 GB. That is over the 480 GB budget and over the
  node's physical 549.8 GB, so the run would be OOM-killed or fail at `rns_dist.c:255`. This is inferred from the measured driver behavior. The target's region
  bytes at tree level 1 were not measured.

#### 3. The fix: `RNS_DIST_CACHE_FIT` (built; off by default)

**Where the memory could come from (modelled, `mem_model.py`, 5.1 × 10¹³):** the budget room is 480 − 455.4 = 24.6 GB per node, below
one slot (68.7 GB per node). The arena during the tree has 284.1 − 264.2 = 19.9 GB of slack at its largest level (tree need 264.2 of
the arena 284.1), the division none (the arena is sized to the dm need). The plane pools (120.9 GB) are the products' own planes. So
**no source inside 480 GB holds even one slot at the target** (nor at 4.74 × 10¹³: room 40.8 GB); drawing the slots from the block
pool per grid would only work at levels with ≥ 68.7 GB of slack, which the peak levels do not have.

**The switch** (`rns_dist.c`; functions touched: `cache_slot_bytes`, `cache_avail`, `cache_plan`, `rns_dist_cache_release`; new
`cache_fit_on`, `cache_slot_log`, `cache_fit_reserve`, `cache_acct`, `rns_dist_cache_budget`, `rns_dist_cache_plan`):
- **sized to the cap**: a slot is `rns_pool0_np()` × 2^(c − 2) limbs per APU, c = min(`dist_logn_max`, `POOL_LOG`) — the largest
  rank plane either tier can form (`mn_logn_cap`: q ≤ 2^(c − 2)). A product whose q exceeds the slot is not cached (a guard in
  `cache_plan`). At the target the slot is unchanged (16 GiB); at `POOL_LOG=25` it is 64× smaller (0.19 GiB, three primes).
- **bounded by the budget**: `budget_check` (`ecalc.c`, one added line) hands each rank its room (its share of `ECALC_NODE_GB` less
  its modelled init peak) and its node's rank count; the slots are at most (room − `RNS_DIST_CACHE_FIT_RESERVE_GB` / ranks) / (4 ×
  slot). The reserve (32 GB per node) covers what the init peak does not hold (at the target `mem_model.py`'s node is 28.8 GB above
  the layout's: the SHMEM pool, the VMM seed buffers, the phases' growth). **At 5.1 × 10¹³ and 4.74 × 10¹³: 0 slots.**
- **shared free memory**: the `hipMemGetInfo` rule divides the free memory by the node's node-processes (every process of a node
  reads the same free memory; G5's 3-process OOM).
- **reported** (whether or not the switch is on): the slots' bytes are in the memory report as `other` (a `mem_acct` provider;
  before, only `MEM_REPORT_DEVS`'s driver figure saw them); the verbose `transform cache:` line adds the per-process GB and FIT's
  bound; `MN_PLAN_ONLY` and `BS_LAYOUT_ONLY` (through the plan check) print a `plan cache` line. `mem_model.py` gains
  `cache_mn_bytes`, `node_peak_cache`, `cache_fit_slots` and `--cache` (node_peak itself unchanged).

`plan cache` at the target (measured on the login node, archive:results_raw_2026-10-19/TC15/plan_cache.txt, launch line env):
`wanted first in the tree (mn grids: tree 20, recip 4, div 2), held to the division's end | default: 2 slots x 16.00 GiB per APU
(2^29 limbs x 4 primes) = 137.44 GB per node … node 426.61 -> 564.05 GB | FIT: 16.00 GiB per APU (2^29 x 4), room 21.39 GB (ECALC_NODE_GB
480 less the node and the reserve 32) -> 0 slots`. At 4.74 × 10¹³: node 410.42 → 547.86 GB by default, FIT 0 slots.
`mem_model.py --cache` (modelled): 5.1 × 10¹³ node 455.4, with the default's 2 slots 592.9 (1 slot 524.2), FIT 0 slots → 455.4;
4.74 × 10¹³ 439.2 / 576.7 / 508.0 → FIT 0.

#### 4. Gates (batch m2, job 21687, s24-16, at 7837ba5)

- `RNS_DIST_CACHE_FIT=1 ./mnaccept.sh 21687 --only unit,e9,mn`: **15 PASS, 0 FAIL** (1303 s; archive:results_raw_2026-10-19/TC15/mnaccept_fit.txt).
  - unit: `t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`, `t_newton 20`, `t_verify`, `t_out`, and `t_mn_grid` at 2 processes (200 checks; its grids use the cache).
  - e9 at size 1, identical in both bases (`LIMB_BASE=10` 11.70 s, `LIMB_BASE=2` 21.39 s).
  - mn e8 at sizes 2, 3 and 4, and e9 at sizes 2 and 4: identical.
- FIT with the cache in use: 10¹⁰ at 2 processes (`POOL_LOG=29`, 3 hits) and at 4 processes (`POOL_LOG=28`) are identical to `e_1e10.out`.
- The default build (the switch off): **4 × 10¹⁰ at size 1 identical** to `e_4e10.out`, `total` 61.65 s. With the switch off, the code changes only the reporting,
  and the guard is inactive (`slot_q` = 0).
- `mem_model.py --check-c`: not rerun. No layout formula changed; the new model terms sit outside `node_peak`.
- Not run: `t_mn_grid` 3, 4 and 6 with FIT (only mnaccept's 2-process case ran), and the `ckpt`, `recheck`, `full` and `stress` steps. None of them touches the cache.

#### 5. Reconciling with `mn_model.py` (`CACHE_MN_SLOTS = 2`)

**Is the +87.5 s "no cache" penalty at 5.1 × 10¹³ supported? It is unmeasured.** aac6's data cannot test it:
- The only runs with the cache in use are 1 × 2 grids at 2³⁰ over 2 to 4 loopback processes.
- There, a sized cache gains about 5 s on the division's products (13 %) and 2 s on `total`, and one slot gains nothing. The direction agrees with the model, and so
  does the 1-slot result: a hit needs ≥ 2 slots.
- The target's saving comes from 2 × 2 to 5 × 6 grids at 2³²–2⁴⁰ over the real fabric: 26 grid products, where one hit skips a redistribution over up to 576
  nodes plus a forward transform. aac6 has no such product. The +87.5 s is purely the model's price.

**What the data does settle.** The target cannot hold both the cache and its 455.4 GB node:
- one slot is 68.7 GB per node, and the room is 24.6 GB (modelled);
- no memory inside 480 GB is free for it: the arena has at most 19.9 GB of slack in the tree and none in the division.

So the model's 346.6 s (5.78 min) wall and its 455.4 GB node cannot both hold. The default code then does one of two things:
- it takes the slots, because the measured `hipMemGetInfo` does not see the VMM regions, and the run fails near 590 GB; or
- it takes none, and runs without the cache.

**Recommendation for the target estimate.**
1. **Assume no mn cache: 434.1 s (7.23 min) without the write, 453.3 s (7.55 min) at 0.6 GB/s; 401.2 s at 4.74 × 10¹³** (modelled, `CACHE_MN_SLOTS = 0`,
   archive:results_raw_2026-10-19/TC15/estimate_target_cache0.txt). Label the 87.5 s as the model's unmeasured cache credit: it is the estimate's largest single item that no measurement
   backs.
2. **Put `RNS_DIST_CACHE_FIT=1` (or `RNS_DIST_CACHE_MN=0`) on the target's launch line** so the run is deterministic at 0 slots and cannot reach the ≈ 590 GB node.
   FIT is the better of the two: it still sizes a cache on smaller runs and in multi-process tests.
3. Getting the credit back needs memory. Each 16 GiB-per-APU slot costs 68.7 GB per node. A per-prime cache (one prime's plane, a quarter of a slot) could fit the
   24.6 GB room. It is not built; it would be a design question for the user.

#### RESUME

- Step 1 (done, d1bff78): the code analysis (§1).
- Step 3 code (done, 3c17073 + 7837ba5): `RNS_DIST_CACHE_FIT`, the plan line, mem_model `--cache`, README rows. Built on aac6 in
  `~/TC15/wt` (a worktree of `~/ntt-TC15`, detached at 7837ba5); `~/ntt-TC15` itself stays at 6990778 for batch m1.
- Step 2 (done): batch m1 (job 21669, 04:46–04:59 EDT) → §2. Batch m2 (job 21687, 05:44–06:18 EDT) → §2 FIT rows and §4. Both jobs
  ended and cancelled themselves; nothing is queued.
- Step 4 (done): §5, the reconciliation with `mn_model.py` and the recommendation.
- Open: the target's region bytes at tree level 1 (which decide what the default does there) are not measured. `t_mn_grid` 3, 4 and 6 have
  not run with FIT. A per-prime cache is not built (§5.3).

## PC15 — Phase 15 Batch 3 agent PC: a partial transform cache inside 480 GB (`RNS_DIST_CACHE_PARTIAL`)

(former file: PC15.md)

Agent PC. Branch `p15-PC` from `int15i` d2d05b3 (= main 99c7de4 + P24 + MPB + DKM, composed; the worktree was at 99c7de4, so I branched
from d2d05b3). aac6 clone `~/ntt-PC15`, logs in `~/PC15/`. Times Eastern (aac6 logs are Central: +1 h). Every number is labelled
**measured**, **modelled**, **assumed** or **fitted**. Agent MS's report on the node memory with P24 + DKM had not landed (p15-MS = d2d05b3
at 22:40 EDT), so the rooms below are the model's (`mem_model.py` at d2d05b3).

#### Summary

- **Built** (`RNS_DIST_CACHE_PARTIAL=1`, off by default, README row): the mn tier's transform-cache slots are taken **per grid product from the
  block pool** (the arena: **no byte added to the node**), as many primes as the pool's free bytes allow after the product's own need and a
  0.5 GB margin per APU (never more than the free extents hold: no remap, no growth); a slot of k < np primes when a full one does not fit;
  **P24 products cached too** (keyed apart); the grid's loop along its longer axis. Functions: `rns_dist.c` `cache_pc_take`, `cache_pc_give`,
  `cache_pc_report`, `pc_on`, `cache_slot_k`, `cache_pl` (new), `cache_lookup`, `cache_find`, `cache_plan`, `rns_dist_cache_release`, `mn_core`,
  `mn_grid`, `rns_dist_cache_plan`; `dbig.c` `db_pool_fit_count` (new, outside my files: a read-only query, 3 lines); `mn_model.py` (the P24
  hits, per-phase primes, `hit_cost`, `CACHE_PART_F`, `cache_partial`).
- **Digits identical everywhere** (measured): cx_grid (18- and 24-digit, forced k = 1 / 2 / full and the pool rule, 2 processes; every share
  hash equal to the switch-off run's), ecalc 10¹⁰ at 2 processes (8 runs, with and without `MN_P24=2 NEWTON_DKM=1`), mnaccept
  `unit,e9,mn,corr` 19/19 with the switch and 19/19 with the switch + `MN_P24=2 NEWTON_DKM=1`, 4 × 10¹⁰ identical.
- **The hits are the model's** on every forced product; a partial hit saves **7 % (1 prime) / 14 % (2 primes)** of its piece, a full one 31 %
  (measured, aac6); the model's partial term was 7–9 % low → `CACHE_PART_F = 1.08` (fitted). End to end at 10¹⁰ (full slots at that size):
  **−21.8 %** (18-digit), **−16.3 %** (P24 + DKM), memory in use unchanged.
- **At 5.1 × 10¹³ with `MN_P24=2 NEWTON_DKM=1` (+ MPB; modelled)**: the pool holds 2 primes in the tree (the arena's slack) and 1 in the
  division (DKM's unused hole + the room's bytes): **288.2 → 274.0 s without the write, 308.2 → 294.9 s with it at 1.0 GB/s (−14.1 / −13.3 s)**;
  −12.1 s if the room's bytes are left alone in the division; **≈ −6 s with `BS_ARENA_ROOM=0`** (MS's route to 5.55 × 10¹³: one prime in
  the tree only). Without the P24 path the cache would gain nothing under `MN_P24=2`.

#### 1. Design (stage 1)

##### 1.1 What the numbers allow at the target (modelled, 5.1 × 10¹³ on 576, `ECALC_NP=auto`, B2's defaults)

- One prime of a slot at the target's plane (q = 2²⁹ limbs per APU at every target grid, CX15 §3.3) is **4.29 GB per APU, 17.2 GB per
  node**; a four-prime slot 68.7 GB per node.
- The rooms outside the arena (CX's, re-run at d2d05b3): the tree levels 480 − (bs device 427.5 + host 44.4) = **8.1 GB** without the
  arena's slack; the division 480 − (dm device 429.8 + host 28.8) = **21.5 GB**. FIT's budget room (the init check less the reserve) is
  **4.9 GB** (`mem_per_node`'s `cache_fit_room`). So a hipMalloc'd slot, held from the tree's first grid product to the division's end
  as the code holds it today, gets **no prime** at the target: the tree's 8.1 GB is below one prime.
- **The block pool has the room** (`mem_model.mem_per_node`, the arena mapped at init, inside the node figure):
  - the tree: arena 300.6 − tree need 264.2 = **36.5 GB per node free at the tree's peak** (9.1 GB per APU): two primes;
  - the division: the arena is laid for the **default** division (binsplit.c `dm_layout` does not follow `NEWTON_DKM`, DKM15 §1.4, §3), so
    under `NEWTON_DKM=1` the need falls to 285.4 GB (`dm_layout(dkm=True)`) against 296.9 → **21.7 GB free** counting the
    `BS_ARENA_ROOM` bytes (6.4 GB; free at the products), 15.3 GB without them: **one prime** (0 without the room bytes); without DKM
    300.6 − 296.9 + 12.8 = 16.5 GB → 0 primes after the margin.

##### 1.2 What a partial slot holds, and where its memory comes from

**`RNS_DIST_CACHE_PARTIAL=1`** (off by default; mn tier only):

- **Per grid product, from the block pool.** At the start of each mn grid product (`mn_grid`, more than one piece) the node takes the
  slot planes from the block pool (`db_pool_alloc`) at **the product's own q** (its largest piece's plane) and returns them at the
  product's end (`cache_pc_give`) — the cache's entries never outlive a product anyway (`cache_drop` at every product's end), so nothing
  is held across products and the tree's slack, the reciprocal's and the division's free pool bytes are all usable. **No byte is
  added to the node**: the slots are inside the arena (mapped at init, in the budget check and the memory model).
- **How many** (`cache_pc_take`): P = min over the four APUs of (the pool's free bytes − the product's own peak need
  `rns_mul_dist_mn_scratch` − a margin `RNS_DIST_CACHE_PARTIAL_MARGIN_GB` = 0.5 per APU) / (q × 8), and at most the blocks the free
  extents hold as they are (`db_pool_fit_count`, new in dbig.c: **no remap, no growth**); the group's minimum (a collective). Then
  P ≥ np: min(`RNS_DIST_CACHE_MN`, P / np) full slots; else **one slot of P primes** (CX: one slot of more primes beats more slots of
  fewer). `RNS_DIST_CACHE_PARTIAL_K=k` caps a slot at k primes; `RNS_DIST_CACHE_PARTIAL_FORCE=P` sets P whatever the room (tests only).
- **What a k-prime slot holds**: the forward-transformed planes of primes 0 … k − 1 of one operand piece (A's or B's), q limbs per
  prime per rank. A hit skips those primes' gathers and forward transforms (A: copied into xa[p], B: read in place in the pointwise
  product); the other primes are gathered and transformed as a miss, so the operand's redistribution stays unless k ≥ the product's
  primes (a full hit). This is CX's pricing (k / np of the transforms' part, no redistribution saved).
- **Both forms, 18- and 24-digit.** P24 (`MN_P24`) does not use the cache today; under the switch a P24 product is cached too: the P24
  planes are the same q per prime and the same transforms (only the gather differs: `k_gather_mn24(_np)`), so the key gets the form
  (`dist_slot.p24`; an 18-digit and a 24-digit transform of the same operand are never confused). A P24 partial A-hit gathers primes
  ≥ k with the per-prime `k_gather_mn24` (the four-plane fused gather is kept for a miss).
- **The loop along the longer axis** (CX's proposal): under the switch, when kb > ka `mn_grid` loops A's pieces outer and B's inner, and
  the slot designation is mirrored (B's pieces j < nB inside the loop, A's piece i in slot nB + i % nA): ka (kb − 1) reuses instead of
  kb (ka − 1). The digits do not depend on the order (exact adds into C; the first piece at shift 0 is still (0, 0)).
- **Pins** (`RNS_DIST_CACHE_HOLD`, off by default) are dropped under the switch, as P24 drops them (a pool slot does not outlive its
  product). Held slots (`RNS_DIST_CACHE_FIT` / the default) are not allocated by the mn tier under the switch.
- **Reporting**: `ECALC_VERBOSE` prints each grid product's slots (`transform cache (RNS_DIST_CACHE_PARTIAL) node r: n slots of k of np
  primes … from the block pool …`), the `dist_mn` line's `(cache n slots of k primes, partial …)`, and at the division's end the run's
  summary (products, with slots, the largest bytes, whether the pool grew). The memory report's pool figures hold the slots (they are pool
  blocks). The plan's `plan cache` line states the rule under the switch (§1.5).

**Safety.** The slot never takes more than the free extents hold (no remap, no growth), leaves the product's own modelled peak need
+ 0.5 GB per APU free, and is returned before X's add and the norm. What it can still do: fragment the pool inside the product so that
mn_core's own blocks need a VMM remap they would not have needed (the remap moves wholly free chunks; it maps no new memory unless the
free chunks are short, which the byte check prevents). The tests watch the pool's growth (`db_pool_hipmalloc_bytes` before / after each
take; `DB_POOL_VERBOSE`).

##### 1.3 The model term (`mn_model.py`)

- `CACHE_P24` (`MN_MODEL_CACHE_P24=1`): P24 products take the code's hits (`cache_hits_p24`: `_cache_pieces_grid` on the P24 grid, the
  planes on the points); `hit_cost` is CX's hit term shared by both forms (a k-prime slot saves k / np of the transforms, no
  redistribution; a full slot as before).
- `CACHE_PRIMES_PHASE = {'tree': k, 'dm': k}`: the primes per phase (the reciprocal and the division are the 'dm' phase).
- `cache_partial_rooms` / `cache_partial_primes` / `cache_partial` (`--cache-partial`, `--cache-partial-keep-room`): the pool's free bytes
  per phase as in §1.1, the primes the code's rule takes (margin 0.5 GB per APU), the estimate.

##### 1.4 The expected gain at 5.1 × 10¹³ (modelled at design time; `archive:results_raw_2026-10-19/PC15/model_*.txt`; the write at 0.6 GB/s; `CACHE_HIT_F` 0.96 fitted by CX) — superseded by §3 (the refit, the write at 1.0 GB/s, the room)

| configuration (`ECALC_NP=auto`, 1 slot) | primes tree / division (pool rule) | 0 slots | with PC | gain without / with the write |
|---|---|---|---|---|
| **`MN_P24=2 NEWTON_DKM=1`, P24 cached, the longer axis** | 2 / 1 | 285.0 s | **272.2 s (4.54 min)** | **−12.8 / −12.1 s** |
| the same, `BS_ARENA_ROOM`'s bytes kept out (conservative) | 2 / 0 | 285.0 s | 274.0 s | −11.0 / −11.0 s |
| the same, the loop unchanged | 2 / 1 | 285.0 s | 275.4 s | −9.7 / −9.2 s |
| `MN_P24=2 NEWTON_DKM=1`, **P24 not cached** (the cache as it is) | 2 / 1 | 285.0 s | 285.0 s | **0** |
| `MN_P24=0 NEWTON_DKM=1`, the longer axis | 2 / 1 | 370.6 s | 345.9 s | −24.7 / −23.4 s |
| `MN_P24=0`, no DKM, the longer axis | 2 / 0 | 400.0 s | 379.5 s | −20.5 / −20.5 s |

So under the likely defaults the item is worth **≈ −11…−13 s (≈ 4 %)**, half of CX's −26.7 s (which was priced on the 18-digit form: P24's
pieces are fewer and cheaper, so a hit saves less). Without the P24 path it would gain nothing under `MN_P24=2`. It needs no memory
(inside the arena). I judge it worth building (small code, off by default, no memory risk by construction); the user decides adoption.

##### 1.5 Test plan

1. **aac6 build**; `t_mn_grid`; `tests/cx_grid` (CX's 9 target-like shapes at cap 2²⁷) at 2 and 4 processes with the switch:
   `RNS_DIST_CACHE_PARTIAL=1 RNS_DIST_CACHE_PARTIAL_FORCE=<P> RNS_DIST_CACHE_PARTIAL_K=<k>` for k = 1, 2, 4 (the pool at these sizes
   is the test's own), slots 0 / 1 interleaved, 18-digit (`MN_P24=0`) and 24-digit (`MN_P24=2`): every product's share of C identical
   over the slot counts (`VERIFY OK`, 0 DIFFERS), the hits as `cache_hits_p24` / `cache_pieces` simulate; timing per shape.
2. **ecalc end to end** at 10¹⁰ on 2 and 4 node-processes with target-like grids (`POOL_LOG` 27 / 26 as CX), the switch with the pool
   rule (no force) and forced k = 1, 2: identical digits; the slots taken per product; the pool's growth 0.
3. **The gates**: `./mnaccept.sh $J --only unit,e9,mn,corr` with the switch, and with the switch + `MN_P24=2 NEWTON_DKM=1`; 4 × 10¹⁰
   identical (size 1: the switch does not act; checked it is inert).
4. **The model refit**: the hit pieces' measured saving against `hit_cost` (partial and P24 hits), `CACHE_HIT_F` checked; the
   estimate at 5.1 × 10¹³ re-run.

#### 2. Tests (stage 3)

##### 2.1 cx_grid at 2 node-processes, cap 2²⁵ (measured; job 21787, s24-26, 22:44–23:20 EDT; `archive:results_raw_2026-10-19/PC15/j1/`)

`tests/pc_j1.sh j1 2 24 cx_shapes_c25.txt off,k1,k2,k4,pool,off_24,k1_24,k2_24,k4_24,pool_24` (= `./mnrun.sh 2 env LIMB_BASE=10 ECALC_NP=4
RNS_POOL_GROW=1 RNS_DIST_CACHE_TRACE=1 ECALC_VERBOSE=1 [MN_P24=2] <cfg> ./tests/cx_grid 24 1 <slots> tests/cx_shapes_c25.txt`): CX's nine
target shapes (`tests/cx_shapes.py --emit 25 2`), a warm-up and one timed repetition, the slot counts interleaved per shape. Configurations:
**off** the switch off (`RNS_DIST_CACHE_FIT=1`, held slots 0 / 2: the reference), **k1 / k2** `RNS_DIST_CACHE_PARTIAL=1 _K=k _FORCE=k` (one
slot of k primes, slots 0 / 1), **k4** `_FORCE=8` (full pool slots, 0 / 1 / 2), **pool** the switch with the pool rule (0 / 1 / 2); `_24` =
`MN_P24=2` (every product P24). Analysis: `tests/pc_hashes.py`, `tests/pc_hits.py` (→ `archive:results_raw_2026-10-19/PC15/j1/analysis.txt`).

- **Digits: identical.** Every configuration VERIFY OK on both processes, 0 DIFFERS inside it; every product's share hash equals the switch-off
  run's (18-digit: 5 configurations × 18 (node, shape) products; 24-digit: 4 configurations against `off_24`). (The 24-digit and the 18-digit
  runs agree except node 0's share of the three low-cut products whose skipped pieces differ with the grid — the limbs below the cut, which
  the callers do not use; the high-cut and full products agree across the forms.)
- **Hits = the model's simulation** (`cache_pieces_t` / `cache_hits_p24`, the longer-axis loop) on all 27 + 27 + 36 + … forced products, 18- and
  24-digit (k1, k2, k4: 0 differ). E.g. 2 × 6 with one slot: 10 hits (the old loop: 6); 1 × 2 low cut: 1 (was 0).
- **The pool rule takes no more than the pool holds**: `pool` / `pool_24` never grew the pool (0 `GREW`); at this size the test's pool has
  0.75 GB free per APU against the margin 0.5 + the product's need 0.16, so it took 2 planes (one slot of 2 primes) on the products where
  that fitted and none elsewhere (the forced runs grow the pool by design: `GREW` 2, reported).
- **Timing, all nine shapes, node 0** (measured, one repetition; the aac6 loopback fabric):

| configuration | 18-digit: 0 → 1 slot (→ 2) | P24: 0 → 1 (→ 2) |
|---|---|---|
| k1 (1 prime) | 60.85 → 57.84 s (−4.9 %) | 34.73 → 33.34 s (−4.0 %) |
| k2 (2 primes) | 61.06 → 54.70 s (−10.4 %) | 36.16 → 33.15 s (−8.3 %) |
| k4 (full, 1 / 2 slots) | 60.85 → 46.88 → 45.09 s (−23.0 / −25.9 %) | 36.13 → 28.87 → 26.88 s (−20.1 / −25.6 %) |

- **The per-piece fit** (each hit piece against itself at 0 slots, same repetition; `tests/pc_hits.py … <k>`): measured / modelled (`hit_cost`
  × `CACHE_HIT_F` 0.96 on `aac6_fabric('tcp', 2)`): **k1 1.069** (7.1 vs 6.7 % of the piece, 64 pieces), **k2 1.093** (14.6 vs 13.3 %), full
  1.017 (31.4 vs 30.9 %, 137); P24: k1 1.075 (6.9 vs 6.5 %, 32), k2 1.087 (14.1 vs 12.9 %), full 1.039 (31.7 vs 30.6 %, 72). The model's
  partial term (k / np of the transforms, no redistribution) is **7–9 % conservative**; its full term and CX's `CACHE_HIT_F` hold for P24.

##### 2.2 ecalc 10¹⁰ end to end at 2 node-processes, target-like grids (measured; job 21788, s24-26, 23:21–23:50 EDT; `archive:results_raw_2026-10-19/PC15/j2/`)

`tests/pc_j2.sh j2 2 10000000000 27 2 c0,pc,c0_pd,pc_pd` (= `./mnrun.sh 2 env ECALC_NP=4 POOL_LOG=27 RNS_POOL_GROW=1 RNS_DIST_CACHE_TRACE=1
ECALC_VERBOSE=1 MEM_REPORT_DEVS=1 <cfg> ./ecalc 10000000000 /tmp/pc15/<tag>.txt`, then `digcmp.sh` against `results/e_1e10.out`, interleaved,
two rounds): **c0** `RNS_DIST_CACHE_MN=0`; **pc** `RNS_DIST_CACHE_PARTIAL=1` (the pool rule, nothing forced); `_pd` = `MN_P24=2 NEWTON_DKM=1`.

| config | `total` run 1 / run 2 (s) | mean | vs c0 | bs / dm (mean) | the slots the pool rule took (per run) |
|---|---|---|---|---|---|
| c0 | 271.23 / 281.79 | 276.5 | — | 59.4 / 209.3 | — |
| **pc** | 218.16 / 214.14 | **216.2** | **−21.8 %** | 45.6 / 162.7 | 8 grid products, all with slots: 2 × 4 primes (5), 1 × 4 (3); ≤ 2.15 GB per APU; no growth |
| c0_pd | 167.45 / 163.19 | 165.3 | — | 41.0 / 116.8 | — |
| **pc_pd** | 141.17 / 135.46 | **138.3** | **−16.3 %** | 37.0 / 93.7 | 7 grid products, all with slots (planes max 8, mean 5.7); no growth |

- **Digits: all 8 runs identical** to the reference, 3 × VERIFY OK each.
- **Memory: nothing added.** The device in use per phase is the same in every configuration (node-process 0: init 42.0, tree 50.6, bs 50.6 GB;
  the block pool 32.2 GB borrowed, 0 hipMalloc); only the pool's peak live rises (bs: 18.0 → 22.3 GB 18-digit, 17.2 → 20.9 P24). The pool had
  3.1–4.8 GB free per APU at these grid products against a need of 0.9–1.3 GB, so the rule took full slots (q 2²⁵, 0.54 GB per prime per APU).
- **The hit pieces against themselves in c0** (`tests/pc_hits.py e2e 2 c0_r1.log pc_r1.log pc_r2.log`, pieces matched by (product, i, j)):
  18-digit 45 hits, 30.2 / 32.0 % saved vs 31.4 % modelled (**0.962, 1.018**); P24 21 hits, 29.5 / 33.5 % vs 32.0 % (**0.923, 1.049**).
- The gain here (−16…−22 %) is full slots on a small run; at the target the pool holds 1–2 primes (§1.4, §3).

##### 2.3 The gates with the switch (measured; job 21789, s24-26, 23:51 EDT – 00:23 EDT; `archive:results_raw_2026-10-19/PC15/j3/`)

`tests/pc_j3.sh j3 unit,e9,mn,corr 1 RNS_DIST_CACHE_PARTIAL=1` = `env RNS_DIST_CACHE_PARTIAL=1 ./mnaccept.sh 21789 --only unit,e9,mn,corr`, then
4 × 10¹⁰ at size 1 with the switch set (the reference evicted from the page cache first; the switch is inert at size 1, so this is the gate on the
default size-1 path, which `cache_find` / `cache_plan` touch):

- **mnaccept: 19 PASS, 0 FAIL** (1462 s): unit (t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_patch, t_mn_grid at 2 procs),
  e9 at size 1 in both bases (identical), mn (10⁸ at 2 / 3 / 4, 10⁹ at 2 / 4: identical, every node VERIFY OK), corr (511461828 at size 1 with
  the recheck, 108388422 at 2 and 4: identical, patched).
- **4 × 10¹⁰: identical** to `results/e_4e10.out`, `total` 62.53 s, VERIFY OK.
- **With the switch + `MN_P24=2 NEWTON_DKM=1`** (job 21794, s24-16, 01:10–01:35 EDT; `archive:results_raw_2026-10-19/PC15/j4/`: `tests/pc_j3.sh j4 unit,e9,mn,corr 0
  RNS_DIST_CACHE_PARTIAL=1 MN_P24=2 NEWTON_DKM=1`): **19 PASS, 0 FAIL** (1473 s).
- At mnaccept's sizes (`POOL_LOG` 31) the mn tier's ecalc runs form no grid product, so the switch acts there only in t_mn_grid (its own
  lowered cap: 1 × 2, 2 × 3, 3 × 1 grids, cuts, X, GMP-checked) through the pool rule; §2.4 forces partial slots in t_mn_grid.

##### 2.4 t_mn_grid with forced partial slots (measured; job 21804, s24-16, 02:06–02:48 EDT; `archive:results_raw_2026-10-19/PC15/j5/`)

`tests/pc_j5.sh` (`./mnrun.sh <p> env ECALC_VERBOSE=1 [MN_P24=2] RNS_DIST_CACHE_PARTIAL=1 _K=k _FORCE=… ./tests/t_mn_grid`, GMP-checked):
**2 processes: k = 1, 2, full, 18- and 24-digit: 6 / 6 runs VERIFY OK on both, 0 FAILED** (240 slot takes each; hits 3302 partial,
3846 full). At 3 processes t_mn_grid died at init (`dpool_get_exact` 17.18 GB OOM: three processes × the default POOL_LOG 31 plane pools,
before any product: a test-setup error, not the switch); the 3- / 4-process and cx_grid-at-4 runs were not done (the batch's time, then
stopped by the integrator's token limit).

#### 3. The model refit and the estimate at 5.1 × 10¹³ (modelled)

- **`CACHE_PART_F = 1.08`** (fitted, `MN_MODEL_CACHE_PART_F`): a partial hit's measured saving over `hit_cost`'s k / np term (§2.1: 1.069, 1.093,
  1.075, 1.087 at k = 1, 2, 18- and 24-digit). The full hits keep CX's `CACHE_HIT_F = 0.96` (§2.1: 1.017, 1.039; §2.2 end to end 0.92–1.05).
  A likely reason (not measured): the model takes the operand's whole redistribution out of a partial hit's saving, and part of it overlaps
  the transforms on aac6. The factor is ASSUMED to carry to the target's fabric, as CX's does.
- **The estimate** (`mn_model.py --cache-partial [--cache-partial-arena-room R] [--cache-partial-digits T]`, INT3's line: `MN_P24=2
  MN_MODEL_DKM=1 MN_MODEL_P24_F=1.025 ECALC_NP_AUTO_MIN=1`, `ECALC_NP=auto`; **the write at 1.0 GB/s per node** (the user's Lustre test,
  main 28837f9 / int15i 88a2a9e, taken into this branch's `mn_model.py`) and at 0.6; the fabric assumed as in TARGET.md; `archive:results_raw_2026-10-19/PC15/est/w1_*.txt`):

| size, `BS_ARENA_ROOM` | configuration | primes tree / division | without the write | with the write @1.0 / @0.6 GB/s | gain (no write / @1.0 / @0.6) |
|---|---|---|---|---|---|
| **5.1 × 10¹³, 0.16** (the code's default) | cache 0 (INT3's / MS's 288.2) | — | 288.2 s (4.80 min) | 308.2 / 334.4 s | — |
| | **+ `RNS_DIST_CACHE_PARTIAL=1`** (the pool rule, the longer axis) | **2 / 1** | **274.0 s (4.57 min)** | **294.9 / 321.1 s** | **−14.1 / −13.3 / −13.3 s** |
| | the same, the room's bytes left alone in the division (conservative) | 2 / 0 | 276.1 s | 296.1 / 322.3 s | −12.1 / −12.1 / −12.1 s |
| | the same, the loop unchanged | 2 / 1 | 277.5 s | 298.1 / 324.3 s | −10.7 / −10.1 / −10.1 s |
| | the cache as it was (P24 not cached) | — | 288.2 s | 308.2 / 334.4 s | 0 |
| **5.167 × 10¹³, 0.16** (MS's largest with the room) | cache 0 → + the switch (tree slack 33.0 GB, division 18.3 GB) | 1 / 0 | 289.3 → 283.3 s | 309.8 → 303.7 / 336.4 → 330.3 s | −6.1 / −6.1 / −6.1 s |
| **5.1 × 10¹³, 0** (MS: needs `BS_ARENA_ROOM=0` on the launch line) | + the switch (arena 284.1: tree slack 20.0 GB, division 5.2 GB) | 1 / 0 | 282.1 s | 302.1 / 328.4 s | −6.1 / −6.1 / −6.1 s |
| **5.55 × 10¹³, 0** (MS's room-0 candidate) | cache 0 | — | 320.3 s | 341.6 / 370.2 s | — |
| | + the switch (arena 307.4: tree slack 20.3, division 6.1 GB) | 1 / 0 | 313.7 s | 335.0 / 363.6 s | −6.6 / −6.6 / −6.6 s |

For comparison without P24 (the 18-digit form, write @0.6; `est/partial_p24_0*.txt`): `NEWTON_DKM=1` 370.6 → 343.9 s (−26.7), no DKM
400.0 → 377.9 s (−22.1). **The room matters**: the tree's second prime and the division's prime come from the arena's room (0.16); with
`BS_ARENA_ROOM=0` (MS's route to 5.55 × 10¹³) the pool holds one prime in the tree only and the switch is worth ≈ −6 s.

The pool's rooms (the division's depends on DKM: the arena is laid for the default division) and the primes they give are modelled
(`mem_model` at d2d05b3; MS15's arena figures, which landed later, are the same: 300.65 GB at 5.1 × 10¹³ with the room, 284.15 without); the code decides at run time from the pool's own free bytes, so a
smaller room gives fewer primes (down to none), never a larger node.

#### 4. Open issues

1. **The target's rooms are modelled.** The primes the pool rule takes at 5.1 × 10¹³ (tree 2, division 1) come from `mem_model`'s arena, tree
   need and DKM's division need (MS15's arena figures agree). The code decides from the pool's own free bytes at each product, so a smaller
   room gives fewer primes, never a larger node; but the gain (−14.1 s) rests on these figures. The tree's second prime is tight: 9.1 GB free
   per APU at the tree's peak against 2 × 4.29 + 0.5 (modelled).
2. **Fragmentation inside a product.** The slot is taken whole from free extents; mn_core's own blocks may then need a VMM remap they would
   not have needed (a remap moves wholly free chunks; the byte check keeps it from mapping new ones). No run grew the pool or changed the
   device in use (§2.1, §2.2), but no run had the target's tight margins; a 1-node run at a lowered cap cannot reproduce them.
3. **The division's prime depends on DKM's unused hole.** If the follow-up that lays the arena for DKM (DKM15 §1.4, −19.9 GB per node) is
   built, the division gets 0 primes (−12.1 s instead of −14.1 s, modelled): a trade of ≈ 2 s for ≈ 20 GB. With `BS_ARENA_ROOM=0` (MS's route
   to 5.55 × 10¹³) the switch is worth ≈ −6 s (one prime in the tree).
4. **`CACHE_PART_F` (1.08) and `CACHE_HIT_F` (0.96) are fitted on aac6's loopback**, where the pieces are mostly exchange; the target's
   pieces are mostly local transforms (CX15 §5). Assumed to carry.
5. **The plan cannot show the slots** (`MN_PLAN_ONLY`): they are decided at run time; the `plan cache` line states the rule and
   `mn_model.py --cache-partial` gives the modelled primes per phase.
6. The run summary line (`transform cache (RNS_DIST_CACHE_PARTIAL): …`) is printed at each `rns_dist_cache_release` (the division's end)
   by every node-process, without a node label.
7. int15i has moved (MS merged; `MN_P24=2` / `NEWTON_DKM=1` the defaults, ab8daa2): a trial merge (merge-tree) of p15-PC with int15i is
   clean; the tests here ran on d2d05b3 + this branch with the switches set explicitly.

#### RESUME

- Done (the integrator stopped further experiments for the token budget). Commits on p15-PC from d2d05b3; aac6 `~/ntt-PC15` at 844eb92 (code
  identical to the branch head; later commits are the model, the report and logs). No PC job is queued or running; j6/j7 were cancelled.
- Not done: t_mn_grid at 3 / 4 processes (needs a lowered POOL_LOG argument), cx_grid at 4 processes, ecalc at 4 processes.
