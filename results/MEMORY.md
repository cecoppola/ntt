# MEMORY — device and host memory

Scope: device and host memory: accounting, the arena and its sizing, the reserved tail, init floor, early free, the memory ceiling. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| M | device memory: B5, B6, B4 / A3, the 8 × 10¹⁰ attempt (Phase 10, PLAN §21) |
| H | host memory: X never on the host (B1), the seeds streamed to the regions (B2) (Phase 10, PLAN.md §21) |
| A-mem | M9 memory accounting, C4 init sizing, C2 the region imbalance (Phase 9, PLAN §19) |
| M11 | memory: the arena's reserved tail (decision 5), the tree's slabs in the arena, the per-node model, 10¹¹ on one node (Pha |
| M13 | memory truth: one ceiling, the two O(share) terms bounded (Phase 13a, PLAN §30 row M; TASKS 1.1, 1.2, 1.3) |
| P13b | sizing follows the prime count; the plane cap as one switch; the one-node ceiling at three primes (Phase 13b, PLAN §31 r |
| R16 | the region-pool sizing abort of ecalc at 10¹¹ digits on 4 nodes (2.5 × 10¹⁰ per node) |
| AS15 | Phase 15 Batch 2, row 2: arena sizing for `BS_SEED_FILL=128` (the division's VMM remaps) |
| MAP15 | Phase 15 Batch 2, row 5 (N2): the arena's mapping, the seeds and level 1 |
| PS15 | Phase 15 agent PS: plane pool 1 at four primes (the rc 6 at 9.17 × 10¹⁰, `ECALC_NP=4`) |
| DL15 | the arena sized to DKM's division, and the room check on the model's node (Phase 15 Batch 3, agent DL) |
| I | the initialisation floor: every allocation form measured, the seeds against the mapping, the 3·2³⁰ planes on the number  |
| K8 | init and the seed wait at one node: where the time goes, and why reordering does not help (2026-10-12) |
| K8b | seed transfers without the HIP runtime inside init's mapping window (2026-10-12) |
| P3 | shorten the start-up mapping phase: evaluated by reading code and A09, no change made (STOPPED) |
| T114 | Phase 14 agent T1: E10a, the early free in the multi-node tree (`MN_TREE_EARLY_FREE`) |
| L114 | L1 — Phase 14 (PLAN §34), window W1: E2 tight reservation (`DM_TIGHT`) and E5's layout half (`DM_TAIL_DEAD`) |

## M — device memory: B5, B6, B4 / A3, the 8 × 10¹⁰ attempt (Phase 10, PLAN §21)

(former file: M.md)

Branch `m10` (from `main` @ 4aca721; the aac6 clone `~/ntt-m` at 38ed61b + the branch). Files owned: `ecalc/mem.c/.h`,
the pool functions of `ecalc/rns_mul.c` (untouched in the end), `binsplit_pregrow` / the layout / `binsplit_free_pools` in
`ecalc/binsplit.c`, the block pool in `ecalc/dbig.c/.h`, the pool-related lines of `ecalc/ecalc.c`. Everything below
continues `results/MEMORY.md §A-mem` (its open issues 2 and 3 are B5 and B6).

#### B5 — the non-zero ranks' region arenas (one line)

`rns_shutdown_hook = binsplit_release_arenas` is set where the arenas are allocated (`arena_get`), not only in
`binsplit_free_pools`, which the non-zero ranks of the `MN_DM=host` and M2 paths never reach. Every rank's
`[released]` row now shows `regions 0.0` (before: 22 GB per process stayed mapped until exit). The default
distributed-dm path already ran `binsplit_free_pools` on every rank after the tree, so there it changes nothing.

#### B6 — at size > 1 the leaf's regions go to the block pool before the tree

When the leaf tree ends on the batch tier (10⁸, 10⁹, 10¹⁰ at sizes 2–4), P_r and Q_r used to be copied to the host
(`region_copy`), then back to device numbers (`db_from_bi`) for `mn_tree`, whose packed slabs and shares came from
the block pool while the regions sat idle in `binsplit`'s hands — 11–44 GB of `hipMalloc` per process (A-mem open
issue 3). Now the tail of `binsplit_e` does at size > 1 what the device tier does at its first level: the idle parity
is donated, P_r and Q_r are copied into blocks from it (`db_copy` of the pool views into `bs_Pd`, `bs_Qd`), the other
parity follows; the driver already takes the device path when `bs_Pd.n` is set. Size 1 keeps the host copies (its
flow is unchanged; a 10⁹ run is identical). Also: `mem_report("tree")` in the distributed-dm path (it had no tree row).

| run (one node, `POOL_LOG` as shown) | digits | tree row: pool donated (borrowed) / hipMalloc, per process | before (A-mem) |
|---|---|---|---|
| 10⁸ size 2, PL 27 | identical | 0.2 / 0.1 | 11 GB hipMalloc |
| 10⁸ size 4, PL 27 | identical | 0.1 / 0.0 | |
| 10⁹ size 2, PL 29 | identical | 2.2 / 0.5 | |
| 10⁹ size 4, PL 29 | identical | 1.1 / 0.3 | |
| 10¹⁰ size 4, PL 29 | identical (146 s) | 11.3 / 4.9–5.4 | 11–16.5 GB hipMalloc |

At 10¹⁰ / 4 the tree still maps 5 GB per process: its peak live (11.0 GB) exceeds the leaf regions (11.3 GB donated,
of which P_r, Q_r hold 2.2) by the slabs' size — the genuine excess, no longer the whole tree. Every node's VERIFY OK,
part files concatenated and compared with the references (`cmp`); the `[released]` rows of the non-zero ranks show
`regions 0.0, planes 0.0` (B5 + A-mem's release).

#### B4 / A3 — the dm phase's block pool at 7 × 10¹⁰, and what the in-phase growth is

New accounting first: every in-phase `hipMalloc` of the block pool is logged (`DB_POOL_VERBOSE=1` or `RNS_VERBOSE`)
with the request, the free bytes, the extent count, the largest extent and the live bytes; the reciprocal prints
what the pool mapped inside the phase; a `division` row in `mem_report` (before `db_release_pools`); a failed device
allocation prints the request and the whole accounting before exiting (`mem_oom`, at every allocation site I own).
With that the growth at 4 × 10¹⁰ and 7 × 10¹⁰ reads:

| | 4 × 10¹⁰ | 7 × 10¹⁰ |
|---|---|---|
| regions donated (all APUs) | 95.7 GB | 169.6 GB |
| peak live in the dm phase | 121.5 GB (6.8 n_Q) | 202.2 GB (6.5 n_Q) |
| byte deficit | 25.8 GB | 32.6 GB |
| mapped inside the phase | 45.3 GB (9.8 in the device top levels + 35.6 in the reciprocal) | 62.2 GB (all in the reciprocal) |
| the reciprocal's growth | **one block per APU: 8.9 GB** = t1's quarter (2 n_Q / 4 limbs), with 8–10 GB free in 2–3 extents, the largest 4.3–5.6 | **one block per APU: 15.6 GB** = t1's quarter, with 7–13 GB free in 2 extents, the largest 4.4–9.9 |

So the "fragmentation over-growth" is not many small blocks: it is exactly the largest block of the phase (t1, the
product scratch of `newton_db_recip`, `max(n_Q + k, 2k) + 8` limbs), which needs contiguous room next to Q, S, r, r2
in the arena and does not find it — the arena's free bytes are there but in two or three holes. Everything else
of the phase fits the donated regions.

Three ways to place t1, measured at 7 × 10¹⁰ (job 20773, s24-26, reference evicted; wall / init / bs / recip / dm):

| 7 × 10¹⁰ | regions mapped at init | mapped in dm | wall | init | bs | recip | dm | digits |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| RESULTS §74 (main) | 169.6 | 62.2 | 163.8 | 25.3 | 65.8 | 35.3 | 72.6 | VERIFY OK |
| default (`ECALC_DM_POOL=0`) | 169.6 | 62.2 (in the phase, `reserve` 6.2 s) | 159.7 | 22.2 | 64.9 | 35.9 | 72.5 | VERIFY OK, T1/T2 |
| `ECALC_DM_POOL_K=6.6` (arena sized for dm at init) | 205.3 | 62.2 (still: 20.2 GB free per APU in two extents of 13.5 + 6.8 < 15.6) | 158.6 | 21.9 | 64.8 | 35.1 | 71.9 | VERIFY OK |
| **`ECALC_DM_POOL=1`** (C3: sized before the reciprocal) | 169.6 | 62.2 (before the phase, 4.25 s; `reserve` 0.00 s) | **157.2** | 20.4 | 65.4 | **34.2** | **71.3** | VERIFY OK |

`ECALC_DM_POOL=1` takes 1.7 s off the reciprocal (the four 15.6 GB fallbacks with their `hipMemset` +
`hipDeviceSynchronize` inside the Newton loop become one chunk per device before it) and the wall goes 159.7 →
157.2 s — real but small: the same 62 GB are mapped either way (0.06 s/GB), and the reciprocal's 34 s are its 93
distributed products (43 s of `dist` calls: ntt 25 s), not memory. A3's expected −10…−15 s is not there; the
reciprocal at 7 × 10¹⁰ is 2.2 × the 4 × 10¹⁰ one because the products are 1.75 × longer and each needs more
pieces. The dm-sized arena (`_K`) maps 36 GB more at init for nothing, since t1 still does not fit the holes; a
k that would (≈ 7.5) maps the same bytes as the fallback, earlier — no gain either. The C3 sizing (A-div's) was
right to the block: its estimate 36.5 GB per device against the measured need; the change here is that the
grown chunk must itself hold the largest block (`db_pool_largest_free`), whatever the byte deficit says (at
4 × 10¹⁰ A-div's rule grew 22.7 GB and the phase still fell back once per APU). `ECALC_DM_POOL` stays off by
default (the user's call; at 4 × 10¹⁰ it is 85.9 vs 85.3 s — noise); it is what the 8 × 10¹⁰ run below uses.

"The second parity's regions earlier" (the item's wording): at 7 × 10¹⁰ the device top levels' peak live (140 GB) is
inside the donated regions (169.6; `pl:hip 0.0` in the bs row), so there is nothing to bring forward there; at
4 × 10¹⁰ the top levels map 9.8 GB (3.3 per APU, the `bs` row) because the regions are sized to the batch levels'
need and the last device levels' P, Q exceed them by that — 0.6 s, left as is. "The planes shrunk after bs": the
dm phase's products at ≥ 4 × 10¹⁰ are 2³¹-point pieces (`split_grid`) and use all of pool 0 (4 q) and 3 q + 16 of
pool 1 — nothing to release without the grid's piece length changing (G's `rns_dist.c`; PLAN §22 "planes at 2³⁰
for the dm phase with more pieces").

##### 4 × 10¹⁰ gate (size 1, reference evicted, `cmp` against results/e_4e10.out)

| build / run | wall | init | bs | recip | dm | digits |
|---|---:|---:|---:|---:|---:|---|
| batch 1, `ECALC_DM_POOL=0` (B5/B6 only) | 88.8 | 15.6 | 38.9 | 16.9 | 34.3 | identical |
| batch 1, `ECALC_DM_POOL=1` (A-div's rule: grew 22.7 GB, then fell back once per APU) | 90.4 | 15.9 | 38.1 | 18.8 | 36.3 | identical |
| batch 2a, `ECALC_DM_POOL=0` | 85.3 | 16.0 | 36.8 | 15.8 | 32.5 | identical |
| batch 2a, `ECALC_DM_POOL=1` (the largest-block rule: grew 35.6 GB in 2.1 s, no fallback) | 85.9 | 15.8 | 37.5 | 16.0 | 32.6 | identical |
| batch 2, default (final build) | 85.3 | 16.2 | 36.4 | 16.0 | 32.7 | identical |
| batch 3, default (final build, after the staging change) | 88.1 | 15.1 | 38.7 | 16.9 | 34.3 | identical |

All within 86.4 ± 1.3 s (RESULTS §74); host HWM 48.8 GB as before. The accounting at 4 × 10¹⁰ (final build, default):

| phase | device | max APU | planes | regions | pool: don / bor / hip | pool: live / peak | host RSS | staging | X | other | HWM |
|---|---:|---:|---:|---:|---|---|---:|---:|---:|---:|---:|
| init | 216.6 | 58.0 | 120.3 | 95.7 | 0 / 0 / 0 | 0 / 0 | 45.7 | 42.9 | 0 | 2.8 | 45.7 |
| bs | 226.3 | 58.0 | 120.3 | 0 | 0 / 95.7 / 9.8 | 35.6 / 83.7 | 47.5 | 42.9 | 0 | 4.5 | 48.8 |
| recip | 261.9 | 66.9 | 120.3 | 0 | 0 / 95.7 / 45.3 | 53.3 / 121.5 | 6.7 | 0 | 0 | 6.6 | 48.8 |
| division | 261.9 | 66.9 | 120.3 | 0 | 0 / 95.7 / 45.3 | 0 / 121.5 | 31.4 | 0 | 0 | 31.4 | 48.8 |
| dm | 120.9 | 30.2 | 120.3 | 0 | 0 | 0 / 121.5 | 31.4 | 0 | 17.8 | 13.6 | 48.8 |
| end | 120.9 | 30.2 | 120.3 | 0 | 0 | 0 / 121.5 | 13.1 | 0 | 0 | 13.1 | 48.8 |

And at 7 × 10¹⁰ (default; with `ECALC_DM_POOL=1` the same bytes, the 62.2 mapped before the recip row):

| phase | device | max APU | planes | regions | pool: bor / hip | pool: live / peak | host RSS | staging | X | other | HWM |
|---|---:|---:|---:|---:|---|---|---:|---:|---:|---:|---:|
| init | 290.5 | 74.6 | 120.3 | 169.6 | 0 / 0 | 0 / 0 | 72.1 | 68.7 | 0 | 3.4 | 72.1 |
| bs | 290.5 | 74.6 | 120.3 | 0 | 169.6 / 0 | 62.2 / 140.0 | 73.2 | 68.7 | 0 | 4.4 | 76.4 |
| recip | 352.7 | 90.2 | 120.3 | 0 | 169.6 / 62.2 | 93.3 / 202.2 | 6.6 | 0 | 0 | 6.6 | 76.4 |
| division | 352.7 | 90.2 | 120.3 | 0 | 169.6 / 62.2 | 0 / 202.2 | 44.7 | 0 | 0 | 44.7 | 76.4 |
| dm | 120.9 | 30.2 | 120.3 | 0 | 0 | 0 / 202.2 | 44.7 | 0 | 31.1 | 13.6 | 76.4 |

Peak of the node: bs 290.5 + 73.2 = 364 GB; the reciprocal 352.7 + 6.6 = 359 GB. The node has 502 GB.

#### The 8 × 10¹⁰ attempt

Sizing from the tables (everything scales with n_Q: regions 2.4 GB per 10⁹ digits, the dm peak 6.5 n_Q, the
staging ≈ one parity of region 0): init 120 + 195 regions + host 78 staging ≈ 395 GB; the reciprocal 120 + 195 +
(231 − 195 + the t1 chunk) ≈ 390 + 7 host; the division + X on the host 36 GB ≈ 435 GB — under 502 with room, so
the memory line was not where the first attempt stopped:

1. **First attempt (batch 2, `ECALC_DM_POOL_K=6.6`)**: arenas 234.7 GB in 9.6 s, init 20.9 s, 355.6 GB device in use,
   then `bs: seed region larger than the staging buffer` — abort at the seeds. The seed-sized staging (Phase 8 step 3)
   was capped at the paper's 2^pool_log limbs (16 GiB per APU); at 8 × 10¹⁰ region 0's seed stage is 19.5 GB.
   One condition removed in the staging line of `ecalc.c` (H's area — the seeds' staging — but a cap, not the
   sizing; noted for H, whose B2 replaces the whole buffer).
2. **Second attempt (batch 3, job 20790, s24-30, `ECALC_DM_POOL=1`, the default arenas): 8 × 10¹⁰ digits in
   210.4 s, VERIFY OK (T1 residues, T2 windows), peak host 90.0 GB, peak device 386.6 GB** — init 25.1 (arenas
   176.8 GB in 11.0 s, staging 81.6 GB pinned), bs 86.1 (batch 46.6, top levels 38.8; the top levels' peak live
   171.8 GB inside the 176.8 donated, no `hipMalloc`), dm 99.1 (reciprocal 49.6: C3 grew 71.1 GB in 6.8 s before it,
   one fallback of 17.8 GB on APU3 whose arena is 0.8 GB smaller; division 49.5), the 80 GB file written in 169 s in
   the writer thread (hidden). The digit string's first and last digits: `2718281828459045235360287471352662497757247093699959574966967…52640279071809056008`
   (no reference file exists at this size; the first 70 × 10⁹ are §74's).

   | 8 × 10¹⁰ | device | max APU | planes | regions | pool: bor / hip | pool: live / peak | host RSS | staging | X | other | HWM |
   |---|---:|---:|---:|---:|---|---|---:|---:|---:|---:|---:|
   | init | 297.7 | 74.6 | 120.3 | 176.8 | 0 / 0 | 0 / 0 | 85.3 | 81.6 | 0 | 3.7 | 85.3 |
   | bs | 297.7 | 74.6 | 120.3 | 0 | 176.8 / 0 | 71.1 / 171.8 | 86.1 | 81.6 | 0 | 4.5 | 90.0 |
   | recip | 386.6 | 109.4 | 120.3 | 0 | 176.8 / 88.9 | 106.7 / 229.3 | 6.7 | 0 | 0 | 6.6 | 90.0 |
   | division | 386.6 | 109.4 | 120.3 | 0 | 176.8 / 88.9 | 0 / 229.3 | 49.2 | 0 | 0 | 49.2 | 90.0 |
   | dm | 120.9 | 30.2 | 120.3 | 0 | 0 | 0 / 229.3 | 49.2 | 0 | 35.6 | 13.6 | 90.0 |
   | end | 120.9 | 30.2 | 120.3 | 0 | 0 | 0 / 229.3 | 13.1 | 0 | 0 | 13.1 | 90.0 |

   The node's peak is the reciprocal: 386.6 + 6.7 = 393 GB of 502 (bs: 297.7 + 86.1 = 384). The driver's per-APU
   used bytes agree with the accounting to 0.3 GB (`MEM_REPORT_DEVS=1`). So **8 × 10¹⁰ fits one node with ≈ 110 GB
   to spare**; the same scaling (regions 2.2 GB, dm pool 2.9 GB, staging 1.0 GB and X 0.44 GB per 10⁹ digits) puts the
   next memory line at ≈ 10¹¹ (init ≈ 120 + 221 + 102 host = 443; the reciprocal ≈ 120 + 221 + 66 + 7 = 414 GB — but
   with no slack for the pool's holes), and the seed spans' `nspan` and the 2^31-point plane cap are unchanged. What
   stopped 8 × 10¹⁰ before was a staging cap, not memory. Time per digit: 2.63 ns (7 × 10¹⁰: 2.25 with the sized
   pool; 4 × 10¹⁰: 2.13) — the reciprocal's products at this length are 109 `dist` calls of 63.6 s (ntt 38.3 s).

#### Tests run (jobs 20764, 20769, 20773, 20790; s24-30 / s24-26)

| run | command (from `ecalc/`, `$J` the allocation) | result |
|---|---|---|
| 10⁹ size 1 (every build) | `./ecalc 1000000000 /tmp/e9v.txt` | identical to ref/e_1000000000.txt |
| 10⁸ sizes 2, 4 | `SLURM_JOB_ID=$J ./mnrun.sh <p> env POOL_LOG=27 ECALC_VERBOSE=2 ./ecalc 100000000 /tmp/e8v.txt` | identical (parts concatenated) |
| 10⁹ sizes 2, 4 | `./mnrun.sh <p> env POOL_LOG=29 ./ecalc 1000000000 /tmp/e9x.txt` | identical |
| 10¹⁰ size 4 | `./mnrun.sh 4 env POOL_LOG=29 ./ecalc 10000000000 /tmp/e10x.txt` | identical to results/e_1e10.out (145.7 s) |
| 4 × 10¹⁰ size 1 (× 6, see the gate table) | `[ECALC_DM_POOL=0/1] RNS_VERBOSE=1 ./ecalc 40000000000 /tmp/e4e10_m.txt` | identical to results/e_4e10.out |
| 7 × 10¹⁰ size 1 (× 3) | `ECALC_DM_POOL=<0/1> / ECALC_DM_POOL_K=6.6 RNS_VERBOSE=1 MEM_REPORT_DEVS=1 ./ecalc 70000000000 /tmp/e7_m.txt` | VERIFY OK (T1, T2; the digit prefix/suffix equal §74's) |
| 8 × 10¹⁰ size 1 | `ECALC_DM_POOL=1 MEM_REPORT_DEVS=1 ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc 80000000000 /tmp/e8_m.txt` | **VERIFY OK, 210.4 s, host 90.0 GB, device 386.6 GB** (first run at this size) |

Logs in `~/ntt-m/ecalc/results/m10/` on aac6 (`e4e10_*.log`, `e7e10_*.log`, `e8e10*.log`, `e8_s*.log`, `e9_s*.log`,
`e10_s4_b2.log`), batch scripts `results/m10b{1,2,2a,3}.sh`.

#### Open issues

1. `ECALC_DM_POOL` is off by default (−1.7 s of the reciprocal at 7 × 10¹⁰, noise at 4 × 10¹⁰): the user's call to
   make it the default above some digit count; the largest-block rule makes it safe at any size.
2. t1's quarter (2 n_Q / 4 limbs) is the one block that does not fit the arena's holes; an arena laid out so that
   t1's room is the contiguous tail (Q and S placed first, r and r2 after) would remove the 62 GB entirely — but
   the same bytes are then mapped at init (0.06 s/GB) for the same wall; only worth it for memory, not time.
3. At 10¹⁰ size 4 the tree still maps ≈ 5 GB per process beyond the donated leaf regions (its slabs); an arena
   sized for the tree's slabs at size > 1 (`region_need` + g × slab) would remove it — 0.3 s per process.
4. The seed staging at 8 × 10¹⁰ is 4 × 19.5 GB pinned; H's B2 (one reused buffer) brings it to 20 GB.

#### Files touched outside my list (minimal, commented `Phase 10 ... (agent M)`)

`ecalc/ecalc.c`: the C3 block (the largest-block rule, the growth print), `mem_report("tree")` in the distributed-dm
path, `mem_report("division")`, the staging cap (one condition). `ecalc/dbig.h`: three declarations.

## H — host memory: X never on the host (B1), the seeds streamed to the regions (B2) (Phase 10, PLAN.md §21)

(former file: H.md)

Branch `h10` (from `main` @ 38ed61b, the Phase 9 merge). Owned: `ecalc/mn_out.c/.h`, `ecalc/verify.c/.h`
(untouched), the output tail of `ecalc/ecalc.c`, the seed code of `ecalc/binsplit.c`, the staging size in
`rns_init`'s request. Touched outside, all minimal and commented `Phase 10 H`: `ecalc/newton_db.c` + `newton.h`
(G: one hook variable and a 6-line helper), `ecalc/dbig.c` (a mutex around `db_mod_qs`), `ecalc/mem.c/.h`
(one additive async-copy pair), the `binsplit_pregrow` entry (M: a one-line once-per-run guard).

#### B1 — X never on the host

**Size 1 (the device flow).** `newton_db_divmod_shifted` gains `newton_db_x_dev`: when set, X is moved there
(the device number, off the block pool) before the low product instead of being copied to the host
(`db_to_bi`, 17.8 GB at 4 × 10¹⁰, ≈ 0.5 s on the critical path); the ±1 corrections at the end are applied
to it in place (`db_share_add_val` over its n limbs; a carry out of the top limb aborts — it cannot happen
for e's X, whose top limb is 2…); the x hook is called as before, with the empty host X. The background
thread of the output stage (`x_bg`) then computes X's residues by the device kernel (`db_mod_qs`, 0.55 s
at 4 × 10¹⁰ instead of 2.8 s of host Horner) and runs the chunked writer over `src->dev` (A-out's device path,
until now used only by the multi-node shares): each 256 MB chunk's limbs come off the device by
`hipMemcpyAsync` on a non-blocking stream per APU into the pinned limb buffer (a null-stream `hipMemcpy`
would serialise with every kernel of the low product), formatted, residue-checked, windowed, written. The
block pool is released after the output stage instead of after the division (X lives in it). A correction
(0 in every run so far) is handled as before: the main thread joins the writer, recomputes X's residues from
the corrected device X and redoes the digits (the file is rewritten); the common path waits for nothing —
T1 waits only for the residue semaphore. `ECALC_OVERLAP=0` keeps the host flow (host A, host X; unchanged).

**Size > 1 (the distributed division).** `out_stage` reads this node's share of A-div's `mdb` X in place
(`src->dev = &Xm.sh`, `mdb_share` for `[lo, hi)`, `cnt` clipped to the share's length) — the stand-in
scatter from node 0's host X is gone from that flow (it stays for `MN_DM=host` / `MN_COMBINE=host`, where X
exists only on node 0's host). The residues of P, Q, R are every node's own: `newton_mn_divmod` already
computes them by `mdb_mod_qs` (each share's `db_mod_qs` scaled by B^lo and summed over the group — exactly
the "db_mod_qs on the share + mn_out_res_combine" of the hook comment), so the broadcast of node 0's is
dropped there; `out_ctx` receives them right after the sharded division, before the non-zero ranks leave
(the first multi-node run failed T1 on ranks ≥ 1 because the copy sat after their exit point). Node 0 no
longer gathers X (`mn_gather_host` of X is unused by this flow); the block pool is released after the output
stage on every rank. `mn_out.c`'s `db_mod_qs` shares a static scratch with the main thread's R residues, so
`db_mod_qs` now takes a mutex (dbig.c).

#### B2 — the seeds streamed into the regions

Before: each region's spans were computed into a region-sized pinned staging on its APU (4 × 10.7 GB at
4 × 10¹⁰: the largest pinned item of the run and the host peak of the bs phase), then copied into the region
by one DMA per region after all of them (I2: computed in a background thread during init, copied at the
start of bs). Now (`seeds_stream` in binsplit.c):

* `binsplit_seeds_begin` — the `rns_after_staging_hook`, called inside `rns_init` once the staging exists —
  starts the seed thread and then allocates the region pools itself (`binsplit_pregrow`, now once per run:
  the driver's later call returns; the same 96 GB mapped 5 s earlier, before the plane pools), then posts
  "pools ready" to the thread (a mutex/condvar).
* The thread computes the spans in chunks of `BS_SEED_CHUNK_MB` (2048; 20 chunks at 4 × 10¹⁰) with all
  threads (`omp for schedule(dynamic, 16)`; the old region-pinned assignment only matters with `MEM_PIN`,
  which is off). While the regions do not exist yet, a chunk goes into one of two pinned buffers (2 GiB each,
  `mem_hstage_alloc`, freed after the seeds); once they exist the threads **store straight into the region**
  (`BS_SEED_DIRECT=1`, default): CPU stores into device memory at the rate of the schoolbook (the seeds are
  compute-bound; RESULTS §56 measured streaming stores at ≈ 8 GB/s against ≈ 5 GB/s of seeds), no HIP call
  from the seed thread. The buffered chunks (two at most: the thread waits for the regions when both are
  full) are copied by `hipMemcpyAsync` on a non-blocking stream (`mem_dev_copy_async`, new in mem.c) once the
  regions exist. `BS_SEED_DIRECT=0` sends every chunk through a buffer and a DMA (measured below: slower —
  a HIP call from the seed thread blocks behind the main thread's `hipMalloc` of the plane pools, 0.4 s per
  call; on the null stream it was worse, the copy queued behind the plane pools' `hipMemset` at 4 GB/s).
* `binsplit_e` joins the thread, takes the node table and checks that the region pools are the ones the
  seeds went into (abort otherwise). Without the overlap (`ECALC_OVERLAP=0`, no hook) the same function runs
  synchronously in `binsplit_e` (all chunks direct). Host regions (`BS_DEVICE_POOLS=0`) keep the old path
  with malloc'd staging.
* The pinned staging of `rns_init` is no longer used by the seeds: in the decimal device flow it is 1 GiB
  per APU (the checkpoints' chunk buffer; `ECALC_STAGING=2` restores the seed-sized staging, `=0` the paper's
  64 GiB). The batch tiers never touch it in this flow (all products are device-registered, `gpucrt` always).

#### Tests (aac6, one node, clone `~/ntt-h`; jobs 20763, 20768, 20770, 20781, 20787; logs `~/h/`)

Every run VERIFY OK on every node and the digits byte-identical (`cmp`) to `~/ntt/ecalc/ref/e_<d>.txt`
(sizes > 1: `cat` of the sorted part files) or `results/e_4e10.out`. Size 1: `srun --jobid=$J -N1 --gpus=4
bash -lc "module load rocm; cd ~/ntt-h/ecalc; env <env> ./ecalc <d> /tmp/h_x.txt"`; size > 1:
`SLURM_JOB_ID=$J ./mnrun.sh <p> env <env> ./ecalc <d> /tmp/h_x.txt`. Scripts `~/h_batch{1,3,4,5}.sh`.

| run | env | result |
|---|---|---|
| t_out, t_newton, t_dbig 0 big, t_verify, t_bs | | OK (1, 696, 609, 334, 10 checks; t_bs's two SHA lines read `ref/*.sha256`, absent from the clone — the printed hashes equal `~/ntt/ecalc/ref`'s; `~/ntt`'s t_bs on the same node: 10 OK) |
| 10⁶, 10⁸, 10⁹ size 1 | `POOL_LOG=27/29 ECALC_VERBOSE=2` | identical; 10⁹ 8.4 s, VmHWM 16.5 GB (was 16.2: the 4 × 1 GiB staging + the two seed buffers vs 4 × 2 GiB) |
| 10⁸ size 1 | `ECALC_OVERLAP=0` (the host flow; the seeds streamed synchronously) | identical |
| 10⁸ size 1 | `BS_SEED_CHUNK_MB=8 MN_OUT_CHUNK_MB=8` (many chunks) | identical |
| 10⁸ size 1 | `BS_DEVICE_POOLS=0` (host regions: the old seed path) | identical |
| 10⁷ | `LIMB_BASE=2` | identical (the binary whole-string path unchanged) |
| 10⁸ sizes 2, 3, 4 | `POOL_LOG=27` (size 4 also with `BS_MDEV_LOGL=21`) | identical, every node's VERIFY OK |
| 10⁸ size 2 | `MN_DM=host`, `MN_COMBINE=host` (the stand-in scatter + broadcast paths) | identical |
| 10⁹ sizes 2, 4 | `POOL_LOG=29` (size 4 with `BS_MDEV_LOGL=25`) | identical; 27.1 / 19.6 s, VmHWM 19.3 / 18.9 GB per process |
| 10⁸ checkpoint + restart | `BS_CKPT_DIR BS_CKPT_EVERY=3`, then `BS_RESTART=1` (the 1 GiB staging as the ckpt buffer) | identical |
| **4 × 10¹⁰ size 1**, ×2 (final build) | `ECALC_VERBOSE=2 RNS_VERBOSE=1 ECALC_WINDOWS=ref/windows_4e10.txt`, file on `/tmp`, reference evicted | identical, VERIFY OK, **82.75 / 82.72 s, VmHWM 11.7 GB** |
| 4 × 10¹⁰, `BS_SEED_DIRECT=0` | (every seed chunk by DMA) | identical, 89.0 s, 12.6 GB |
| 4 × 10¹⁰, B1 + the first B2 (DMA thread, null stream), ×2 | job 20763 | identical, 89.2 / 88.4 s, 12.0 GB |

##### 4 × 10¹⁰, size 1: before / after

| | merged main (RESULTS §74, 5 runs) | h10 final (2 runs) |
|---|---:|---:|
| init | 16.1 ± 0.9 | 14.3 / 14.4 |
| bs (seeds inside bs / batch / top levels) | 37.3 (— / 23.5 / 13.5) | **37.0 / 37.1** (0.1 / 23.1 / 13.8) |
| dm (reciprocal / division) | 32.9 (16.2 / 16.7) | 31.3 (15.7 / 15.6) |
| phases | 70.3 ± 0.5 | 68.4 / 68.3 |
| **wall (`total`)** | **86.4 ± 1.3** | **82.75 / 82.72** |
| **peak host (VmHWM)** | **48.8 GB** | **11.7 GB** |

(The old bs line hid the seeds' join: I2's seeds were computed in init and copied at the start of bs; the
copy is gone and the thread finishes inside init — 13.2 s of 14.3.) The division: "X out + low product"
7.6 s with no X out; X's residues 0.55 s in the background. The 4 × 10¹⁰ mem_report of the final build:

| phase | device GB (max APU) | planes | regions | pool don / bor / hip | pool live / peak | host RSS | staging | X | digits | other | **HWM** |
|---|---:|---:|---:|---|---|---:|---:|---:|---:|---:|---:|
| init | 216.6 (58.0) | 120.3 | 95.7 | 0 / 0 / 0 | 0 / 0 | 11.5 | 8.6 | 0 | 0 | 3.0 | 11.5 |
| bs | 226.3 (58.0) | 120.3 | 0 | 0 / 95.7 / 9.8 | 35.6 / 83.7 | 9.0 | 4.3 | 0 | 0 | 4.7 | 11.7 |
| recip | 261.9 (66.9) | 120.3 | 0 | 0 / 95.7 / 45.3 | 53.3 / 121.5 | 6.9 | 0 | 0 | 0 | 6.9 | 11.7 |
| dm | 261.9 (66.9) | 120.3 | 0 | 0 / 95.7 / 45.3 | 17.8 / 121.5 | 7.5 | 0 | 0 | 0 | 7.5 | 11.7 |
| end | 120.9 (30.2) | 120.3 | 0 | 0 | 0 / 121.5 | 6.9 | 0 | 0 | 0 | 6.8 | 11.7 |

Against A-mem's table for `main` (RESULTS §74 / results/MEMORY.md §A-mem): host at init 45.7 → 11.5 (staging 42.9 →
8.6: 4 × 1 GiB + the two 2 GiB seed buffers, freed after the seeds), at bs 47.4 → 9.0, at dm 70.8 → 7.5
(X 17.8 → 0, the digit string 40 → 0 already by A-out), **HWM 48.8 → 11.7 GB**. The device side is
unchanged except that the block pool stays mapped through the output stage (the "dm" row shows X's 17.8 GB
live; released at "end"). What remains of the host: the ROCm runtime and the program (≈ 7 GB), the
checkpoints' staging 4.3 GB, the writer's two 256 MB chunks and a 128 MB limb buffer.

#### Open issues

- Host peak 11.7 GB at 4 × 10¹⁰: the gate said ≈ 20 GB (B2) — the pinned staging of `rns_init` could go
  to 0 in the decimal device flow (the checkpoint code would malloc its 1 GiB chunk buffers, `ckpt_buf`'s
  fallback) for another 4.3 GB; not done (T owns the checkpoint code).
- `mem_report`'s "dm" row now shows the block pool with X live (released after the output stage); the
  device figure at "dm" is the same as at "recip".
- The seed thread's two buffered chunks (4 GB) are copied by `hipMemcpyAsync` once the regions exist;
  issuing the two calls took 1.5 s at 4 × 10¹⁰ (the HIP runtime's lock is held by the main thread's
  `hipMalloc` of the plane pools) — inside init, harmless; a `hipMemcpy` on the null stream there queued
  behind the plane pools' `hipMemset` (4 GB/s) and pushed the seeds 6 s past init (the first version).
- `BS_SEED_THREADS` and `MEM_PIN` keep their meaning for the old path only; the streamed seeds use every
  thread on one chunk at a time (`omp for` dynamic), which is what the unpinned default did in effect.
- `binsplit_pregrow` runs inside `rns_init` (from the hook) when the overlap is on: the driver's "init:
  region pools %.2f s" line then reports 0 and the arenas' line prints under `ECALC_VERBOSE=2` only
  (`bs_verbose` is set after `rns_init`).
- The stand-in scatter (`mn_out_scatter_standin`) and the residue broadcast remain for the host flows
  (`MN_DM=host`, `MN_COMBINE=host`); they could go with those switches (PLAN §22 E1).
- A ±1 correction at size 1 redoes the whole digit string (never observed; the affected chunk alone would
  need restartable residue/T2 state in `mn_out_run`).

#### Files touched outside my list (minimal, commented `Phase 10 H`)

`ecalc/newton_db.c` (`newton_db_x_dev`, `db_add_small`, three lines in `newton_db_divmod_shifted`),
`ecalc/newton.h` (one declaration), `ecalc/dbig.c` (`db_mod_qs` under a mutex, the body renamed
`db_mod_qs_locked`), `ecalc/mem.c/.h` (`mem_dev_copy_async`, `mem_dev_copy_wait`), `ecalc/binsplit.c`
`binsplit_pregrow` (the once-per-run guard, first line). In `ecalc.c` above the output tail: the `Xdev`/`Xm`
declarations next to the bigints, `newton_db_x_dev` set beside the x hook, `db_release_pools` moved after
`out_stage` (size 1 and node 0), the `out_ctx` filled after the sharded division, the dm line prints the
device X's length; the staging request (1 GiB) in the init block.

## A-mem — M9 memory accounting, C4 init sizing, C2 the region imbalance (Phase 9, PLAN §19)

(former file: A-mem.md)

Branch `amem` (from `main` @ a75474d). Files owned: `ecalc/mem.c/.h`, `rns_init` and the pool functions
of `ecalc/rns_mul.c`, `binsplit_pregrow` and the level layout of `ecalc/binsplit.c`, the block pool's
accounting in `ecalc/dbig.c`. Touched outside (minimal, commented `A-mem`): `ecalc/ecalc.c` (the pool-1
request next to the staging sizing, the donation threshold, `mem_report` calls at the phase boundaries),
`ecalc/dbig.h` (one additive declaration), `ecalc/binsplit.h`.

#### What was measured first: the cost of mapping device memory (job 20708, s24-16)

`hipMalloc` on one APU: 2 × 14 GB 2.11 s, 1 × 28 GB 2.12 s, 8 × 1 GB 0.61 s, 16 GB 0.86 s — 0.054–0.075
s/GB with no per-call overhead worth chasing; four APUs in parallel 28 GB each 7.0 s wall (6.9 s per APU: the
driver serialises), serial 7.1–8.1 s. `hipFree` 0.004 s/GB. No flavour is cheaper: `hipExtMallocWithFlags`
uncached 1.26 s / fine-grained 1.21 s, `hipMallocManaged` 2.23 s, `hipHostMalloc` 2.73 s for 16 GB.
`hipMemGetInfo` works (137.4 GB per APU, free tracks allocations) and is used for the transform tables.
So: **bytes mapped over the run is the only lever**; one allocation per device for both parities buys no
driver time by itself — its value is contiguity for the block pool (below).

#### Design

##### M9 — accounting (`mem_report`)
`mem.c` keeps a table per phase. Modules that own device memory register a provider
(`mem_acct_register`) that adds its bytes per device by category: `rns_mul.c` the plane pools (`da.cap +
db.cap`) and the transform contexts (`hipMemGetInfo` around `ntt_ctx_create`); `dbig.c` the block pool —
every region with its kind (donated by bs, borrowed = the plane tails and the region arenas, the pool's own
`hipMalloc` fallback/pregrow), the bytes handed out now, their peak, and the free bytes; `mem.c` itself the
regions still in its registry (`mem_dev_alloc`). Host: the pinned staging and the registered blocks from
the registry, X and the digit string named by the driver (`mem_report_host_item`), other = VmRSS − known.
The driver calls `mem_report(phase)` after init, bs, (tree), recip, dm, T1 and at the end, then
`mem_report_summary()` prints one table; `MEM_REPORT_DEVS=1` adds per-APU rows with the driver's used/total.
At size > 1 every node-process prints its own lines prefixed `mem[rank]`.

##### C4 — init sizing
* Plane pool 1 is sized to what the dist tier uses at 2^pool_log points, 3 q + 16 limbs (12 GiB + 2 MiB at
  2^31; `rns_pool1_default_bytes`), instead of 2^pool_log limbs (16 GiB), in the all-device flow (decimal,
  device top levels, device dm, not `MN_COMBINE=host` — the host mdev tier is the only user of a full pool 1
  and is not reached there). `rns_pool1_bytes_req` / `RNS_POOL1_GB` override. The tail donation in the
  driver uses the same threshold, so it donates nothing now (it used to hand 4 GiB × 4 to the dm pool).
* The region pools of both parities come from **one allocation per device (the arena)** of exactly what
  the level layouts need: `region_need` lays out every batch level in advance with the seed bound `per`
  per span (node i of the level after l pairings covers spans [i 2^l, (i+1) 2^l); P and Q slots as the
  level loop places them; regions by the same `place_node`), takes the per-region maximum over the levels
  that live in the region pools (the tier switch taken at 0.85 × the bound so a level whose real sizes are
  just under the threshold is still counted), and `pool_get` adds 1/8. The bound is 7–11 % above the real
  sizes, so the margin is ≈ 20 %. This replaces `total0/4 (1 + 1/4)` per region and parity, which held
  1.4 × the live data and still grew at the five-node level.
* When both halves of an arena are handed to the dbig block pool (at the first device-tier level) they are
  joined into one borrowed region (`db_donate_adjacent`) so their extents coalesce: dm's largest quarters
  need contiguous space that two separate 14 GB regions could not give. The arena outlives the pool's use
  (borrowed regions are never freed by `db_release_pools`) and is released by `rns_shutdown`
  (`rns_shutdown_hook`). `ECALC_ARENA_GB=<per device>` makes the arena larger than bs needs (e.g. the dm
  phase's block-pool need, so dm never falls back to `hipMalloc`); `BS_REGION_FLAT=1` restores the flat
  sizing.
* The seed-sized pinned staging now applies at any `size` in the M3 flow (it was single-process only; the
  M3 tree and the device dm never touch `hstage`), so four node-processes on one node do not pin 4 × 64 GiB.

##### C2 — the five-node level
`place_node(i, n, offr, ra, rb)`: levels with more than `bs_balance_n` (16) nodes keep the subtree rule
(region NR i / n); smaller levels — where the odd carries make the counts 33, 17, 9, 5, 3, 2 — put each
output node in the least-loaded region (fewest limbs laid out so far), ties broken for the region holding
its inputs (b's, then a's), then the lowest index. Correct for any placement (the batch tier computes on the
device owning the result and reads operands where they are). At 4 × 10¹⁰ the level with five outputs goes
from shares 2/5, 1/5, 1/5, 1/5 (+ the small carried node) to 1/4 each, and no pool grows inside the phase
(`bs_st.n_grow`, printed after bs).

#### Tests (jobs 20708, 20718, 20731, 20739, 20746, 20749, 20750, s24-30 / s24-16; every ecalc run below VERIFY OK unless said)

Correctness against the reference files (`cmp`, `~/ntt/ecalc/ref`, `results/e_1e10.out`, `results/e_4e10.out`):

| run | command (from `ecalc/`, `$J` the allocation) | result |
|---|---|---|
| 10⁶, 10⁸, 10⁹ size 1 | `ECALC_VERBOSE=2 ./ecalc <d> out` | identical (every build; 10⁹ re-checked after each change) |
| 10⁸ sizes 2, 4 | `SLURM_JOB_ID=$J POOL_LOG=29 ./mnrun.sh <p> ./ecalc 100000000 out` | identical |
| 10⁹ size 2 | `POOL_LOG=30 ./mnrun.sh 2 ./ecalc 1000000000 out` | identical |
| 10¹⁰ size 1 | `./ecalc 10000000000 out` | identical |
| 10¹⁰ size 2 | `POOL_LOG=30 ./mnrun.sh 2 ./ecalc 10000000000 out` | identical |
| 10¹⁰ size 4 | `POOL_LOG=27 ./mnrun.sh 4 ./ecalc 10000000000 out` | identical (75 s) |
| 10¹⁰ size 4 | `POOL_LOG=29 ./mnrun.sh 4 ...` (×2), `POOL_LOG=28` | identical (final build; T1 failed in an earlier build, open issue 1) |
| 2 × 10¹⁰ size 2 | `POOL_LOG=30 ./mnrun.sh 2 ./ecalc 20000000000 out` | VERIFY OK (119 s; no reference file to compare) |
| 4 × 10¹⁰ size 1 | `RNS_VERBOSE=1 ECALC_VERBOSE=2 ./ecalc 40000000000 out` | identical, 8 runs over the variants below |

##### 4 × 10¹⁰, size 1 — the gate

`main` (a75474d) was rebuilt in a worktree of the same clone and run on the same node in the same
allocation, reference evicted before every run. Init is decomposed by the new prints (`ECALC_VERBOSE=2`,
`RNS_VERBOSE=1`): staging + contexts ≈ 3.9 s, the plane pools, peer access 0.2 s, the regions.

| 4 × 10¹⁰ | main (s24-16) | main (s24-16, 2nd) | A-mem, arenas = bs need (final) | A-mem, arenas dm-sized (`ECALC_DM_POOL_K=8`) |
|---|---:|---:|---:|---:|
| device mapped at init | 137 + 112 = 249 GB | 249 | 120 + 96 = **216 GB** | 120 + 142 = 263 GB |
| init | 17.7 | 16.1 (s24-30) | **15.5 / 16.9** | 19.9 / 19.6 |
| bs (region growth inside) | 43.6 (19.1 GB) | 44.3 (19.1 GB) | 42.6 / 41.5 (**none**) | 41.0 / 41.7 (none) |
| recip (block pool hipMalloc inside) | 14.9 (26.7 GB) | 15.7 (26.7 GB) | 15.7 / 15.5 (45.3 GB) | 13.6 / 13.5 (**0**) |
| dm | 34.1 | 35.7 | 34.1 / 33.9 | 32.0 / 32.2 |
| wall | 95.6 | 96.3 | **92.5 / 93.0** | 93.6 / 94.3 |

So against `main` on the same node: **init −1…−2 s, bs −2 s (the five-node growth is gone), wall −2.6…−3 s**,
digits identical. The dm-sized arena removes every `hipMalloc` from the run but pays for it at init (0.06
s/GB, 142 GB in 8.6 s) for the same wall clock within the spread; it stays a knob for A-div (the dm phase's
block-pool need is its business: at 4 × 10¹⁰ the peak of live device numbers is 121.5 GB, 30.4 GB per APU).
The single-node 8 GB per-call `hipMalloc` story: bytes mapped are the only lever, and the run maps 216 +
10 + 45 = 271 GB with the bs-need arenas against 249 + 19 + 27 = 295 for `main`.

The accounting at 4 × 10¹⁰, size 1 (final build, `mem_report_summary`; GB; device = all four APUs, the
largest APU in the second column):

| phase | device | max APU | planes | regions | pool: donated / borrowed / hipMalloc | pool: live / peak / free | tables | host RSS | staging | X | digits | other | HWM |
|---|---:|---:|---:|---:|---|---|---:|---:|---:|---:|---:|---:|---:|
| init | 216.6 | 58.0 | 120.3 | 95.7 | 0 / 0 / 0 | 0 / 0 / 0 | 0.61 | 45.7 | 42.9 | 0 | 0 | 2.8 | 45.7 |
| bs | 226.3 | 58.0 | 120.3 | 0 | 0 / 95.7 / 9.8 | 35.6 / 83.7 / 70.7 | 0.61 | 47.4 | 42.9 | 0 | 0 | 4.4 | 48.7 |
| recip | 261.9 | 66.9 | 120.3 | 0 | 0 / 95.7 / 45.3 | 53.3 / 121.5 / 88.4 | 0.61 | 6.6 | 0 | 0 | 0 | 6.5 | 48.7 |
| dm | 120.9 | 30.2 | 120.3 | 0 | 0 | 0 / 121.5 / 0 | 0.61 | 70.8 | 0 | 17.8 | 40.0 | 13.0 | 70.8 |
| T1 | 120.9 | 30.2 | 120.3 | 0 | 0 | 0 / 121.5 / 0 | 0.61 | 70.8 | 0 | 17.8 | 40.0 | 13.0 | 70.8 |
| end | 120.9 | 30.2 | 120.3 | 0 | 0 | 0 / 121.5 / 0 | 0.61 | 53.0 | 0 | 0 | 40.0 | 13.0 | 70.8 |

Regions from the layouts: 12.33 / 10.02 / 10.17 / 10.00 GB per parity (the flat rule gave 12.49 for each
and still grew region 0 to 19.1 GB). The 9.8 GB of pool `hipMalloc` inside bs is the device top levels'
numbers beyond the donated regions (P, Q of the last two levels, ≈ 8.9 GB per APU live). The "other" host
bytes are the ROCm runtime and the program (≈ 13 GB once the device queues exist, the same at 10⁶);
"tables" is the `hipMemGetInfo` delta around the transform contexts. `MEM_REPORT_DEVS=1` adds per-APU rows
with the driver's used/total: it agrees with the accounted total to 0.3 GB while allocations exist; after
`db_release_pools` the driver still reports the freed bytes as used (`hipFree` does not return them at once).

##### Sizes 2 and 4 on one node — capacity

Per node-process (POOL_LOG=30 → planes 4 × (8 + 6) GiB = 60 GB; POOL_LOG=29 → 30 GB; the dist tier grows
them on demand to what the tree's products need — at 2 × 10¹⁰ size 2 node 0's pools reached 137 GB in dm):

| run | phase | device / process (planes, regions, block pool) | host RSS / process |
|---|---|---|---|
| 2 × 10¹⁰ size 2, PL 30 | init | 147.7 (60.1, 86.4, 0) | 17.6 (staging 12.9) |
| | tree (node 1 / node 0) | 156.3 (68.7, 0, 86.4 borrowed) / 191.8 (60.1, 86.4, 44.0 hipMalloc) | 43.0 / 60.9 |
| | recip (node 0; node 1 released) | 269.1 (137.4, 0, 86.4 + 44.0; live 8.9, peak 44.4) | 56.9 |
| | dm / end (node 0) | 138.7 (137.4) | 65.8 / 50.2 (X 20.0) — HWM 83.5 |
| 10¹⁰ size 4, PL 29 | init | 55 (30.1, 22.5, 0); node 0 68 (35.6 regions) | 13.0 (staging 8.6) |
| | tree | 71–76 (34.4, 22.5, 11–16.5 hipMalloc) | 30–37 |
| | recip / dm (node 0) | 123 (68.7, 0, 35.6 + 16.4) / 140 (137.4) | 33 / 37 — HWM 46.3 |

The first attempts at these sizes were OOM-killed on node 0 in dm (both 2 × 10¹⁰ / 2 and 10¹⁰ / 4): the
node's 512 GB are shared by all processes' device and host memory, and the ranks that are done sat at the
final barrier holding their planes, regions and the tree's `hipMalloc` pools (150–200 GB each). Now a
non-zero rank releases its device memory (`db_release_pools`, `rns_shutdown`) before the barrier (driver,
one line), and the dm-sized arena applies to node 0 only. `main` cannot run either configuration on one node
(64 GiB of pinned staging per process at size > 1: 4 × 69 GB; with `POOL_LOG=27` to shrink it, the batch
tier's 2^30 tile overflows the pools: segfault) — the seed-sized staging at any size and the on-demand pool
growth in the batch tiers are what make them run.

**Per-node capacity (one node, 512 GB shared).** Size 1: unchanged in principle from RESULTS §71 (7 × 10¹⁰);
the run needs planes 120 GB + regions 2.4 GB per 10⁹ digits + dm's pool ≈ 3.9 GB per 10⁹ (mapped inside the
phase) + host 13 + 1.5 GB per 10⁹ (X, digits); at 7 × 10¹⁰ ≈ 120 + 168 + 273 + 118 — the same 7 × 10¹⁰ ceiling,
with 17 GB less in planes and ≈ 16 GB less in regions than before. Size 2: 2 × 10¹⁰ runs (peak ≈ 156 + 269
device + 43 + 66 host ≈ 530 GB counting node 1 before its release; ≈ 400 GB after); the limit is node 0's dm
(which holds the whole number until A-div distributes it) plus the other process's planes and arena: ≈
3 × 10¹⁰ per node at size 2. Size 4: 10¹⁰ runs at 305 GB peak (4 × 76 in the tree); 2 × 10¹⁰ would need
4 × 110 in the tree and ≈ 270 on node 0 in dm — over the node — so ≈ 1.5 × 10¹⁰ at size 4 on one node until
the division is distributed; with M4 the per-process dm need drops by 1/size and the leaf tree's regions are
already 1/size, so the capacity per node at size 4 returns to the size-1 figure minus the planes' 4 × 120 GB
(POOL_LOG 31) or 4 × 60 (30).

#### Open issues

1. **Resolved: 10¹⁰ at size 4 with `POOL_LOG=29` failed T1** (two runs; one leaf P differed by a limb) while
   `POOL_LOG=27` was identical: in that build pool 1 was 3 q also for small pools and the striped batch tier
   grew it *inside the phase* (3 → 4 GiB for the 2^29 tile); with pool 1 at the full pool for `POOL_LOG` ≤ 30
   (job 20751: 29 twice, 28 once — all identical to the reference, 68–70 s) the failure is gone. So growing a
   plane pool while the batch tier is running is unsafe (a stale plane pointer somewhere in that path — not
   chased, it is N-kernel's code); the on-demand `rns_dpool` calls stay as a guard for the case `main` would
   overflow (a 2^30 tile at 2^27–2^29 pools, which segfaults on `main`) but are never exercised at the default
   pool sizes: at `POOL_LOG=31` pool 1 (12 GiB) holds any batch tile (≤ 8 GiB). `main` cannot run 10¹⁰ at
   size 4 on one node at all (OOM at 29/30 from 4 × 64 GiB of pinned staging, segfault at 27).
2. The non-zero ranks' region arenas are not released by `rns_shutdown` there (`binsplit_free_pools` never
   runs on them, so the hook is not set): 22 GB per process stays mapped until exit — harmless, one line.
3. At size > 1 the leaf's regions are only donated to the block pool when the leaf ends on the device tier;
   when it ends on the batch tier the tree's packed slabs and shares come from `hipMalloc` (11–44 GB per
   process, "pool: hipMalloc" in the tree rows). Donating the regions before `mn_tree` needs the driver's flow
   (A-div's area now) — noted, not done.
4. `ECALC_DM_POOL_K` (dm-sized arena) is the knob for A-div if the distributed division wants its pool mapped
   at init.

#### Files touched outside my list (minimal, commented `A-mem`)

`ecalc/ecalc.c`: the pool-1 request and `host_combine` next to the staging sizing (the seed-sized staging now
applies at any size in the M3 flow); the donation threshold; `mem_report` calls; init stage timing print; the
non-zero ranks' release before the final barrier. `ecalc/rns_mul.c` batch tiers: four one-line `rns_dpool`
calls that grow a plane pool on demand before it is used (no-ops when it fits). `ecalc/dbig.h`: one
declaration. `ecalc/binsplit.h`: `bs_stats.n_grow/grow_bytes`, `binsplit_release_arenas`, `bs_balance_n`.

## M11 — memory: the arena's reserved tail (decision 5), the tree's slabs in the arena, the per-node model, 10¹¹ on one node (Phase 11, PLAN §26 row M)

(former file: M11.md)

Branch `m11` (from `main` @ 72aa2e9; the aac6 clone `~/ntt-m11`). Files owned and touched: `ecalc/dbig.c/.h` (the
block pool's reserved tail), `ecalc/binsplit.c/.h` (the layout: `dm_layout`, `tree_need_dev`, `arena_get`,
`donate_one`, `binsplit_pregrow` — the pregrow/layout functions only), NEW `ecalc/mem_model.py`. Nothing outside
my files. Everything below continues `results/MEMORY.md §M` (its open issues 2 and 3 are items 1 and 2 here).

#### 1. The arena laid out with t1's quarter as its contiguous tail (decision 5)

`results/MEMORY.md §M` found that the dm phase's in-phase `hipMalloc` is exactly one block per APU — t1's quarter
(`newton_db_recip`'s product scratch, `max(n_Q + k, 2k) + 8` limbs, a quarter per APU: 8.9 / 15.6 / 17.8 GB at
4 / 7 / 8 × 10¹⁰) — which the bs-sized arena cannot hold contiguously next to Q, S, r, r2, plus the byte deficit
of the phase (the peak of live device numbers, 6.5–6.8 n_Q, exceeds the two parities' regions by 26–33 GB).

**Layout (`binsplit_pregrow`).** `dm_layout(N, size)` computes from N alone what the driver's formulas will
request: n_Q = ⌈log₁₀ N! / 18⌉ + 2, k_mu = the limbs of 10^d + 4, t1 = max(n_Q + k, 2k) + 8, the hole = t1's
quarter (+ 1/64), and the dm need per device = Q + S (n_Q/4 with the bs bound's 10 %) + r + r2 ((k + 4)/4) + t1
+ the grid's piece temporary ((min(2^pool_log, n_Q + k) + 8)/4) + min(1 GiB, 1/8). At size > 1 the shares are
1/size and the top tree level's slab scratch is added (§2). The arena of region r (one per APU) is then
`max(2 × bs half, dm need, tree need)` = the two parities plus the "dm extra"; its last `hole` bytes are the
reserved tail — a policy over bytes the arena has anyway, not bytes added: at the dm phase the level pools are
dead and the pool's blocks keep out of the tail, so it is free whenever the arena holds the dm need at all
(v2; v1 added the hole to the arena — the batch-1 runs below: +3.7 GB at 4 × 10¹⁰, +32 GB at 9 × 10¹⁰ where
the bs regions exceed the dm need). The driver's `ECALC_DM_POOL_K` / `ECALC_ARENA_GB` still apply before it;
`ECALC_TAIL=0` restores the Phase 10 layout (no tail, no extra). The bs verbose line prints the layout:
`bs: dm layout: n_Q … t1 … per device: need … tail … (thresh …)` and `bs: arenas … (dm extra …, tails …)`.

**Allocation policy (`dbig.c`, `db_pool_set_tail`).** When the parity-1 half is donated (`donate_one`) it
carries the extra and the hole to the arena's end, and the pool is told the tail `[end − hole, end)` and a
threshold (5/8 of the hole: r and r2 at ≈ half of it stay out, t1 takes it). `ext_take`: a request ≥ thresh is
carved from the **back** of the extent that ends at the tail's end (t1 lands contiguous at the arena's end);
a request below it is best-fit over the extents with the tail's bytes excluded, so the tail stays whole until
the large request comes. Both preferences are soft: a large request the tail cannot hold takes the best fit
anywhere, a small one that fits nowhere else enters the tail (`db_pool_tail_stats` counts both), and only then
does the pool `hipMalloc` — never a failure the Phase 10 pool would not have had. `db_release_pools` clears the
tails with the regions.

**What C3 (`ECALC_DM_POOL=1`, agent P's switch) does with it:** nothing — its rule grows only when the free
bytes are short of its need or the largest extent is below t1's quarter; with the tail both hold.

**4 × 10¹⁰ (job 20804, s24-26, the reference evicted before each run, `cmp` against `results/e_4e10.out`; v1 layout):**

| run | arena at init (regions + dm extra + tails) | pool hipMalloc: bs / recip / division | device at the dm peak | wall | init | bs | recip | dm | digits |
|---|---|---|---:|---:|---:|---:|---:|---:|---|
| Phase 10 closing (5 runs) | 95.7 | 9.8 / 45.3 / 45.3 | 261.9 | 83.0 ± 1.2 | 16.4 | 36.3 | 15.2 | 30.1 | identical |
| M11 run 1 | 136.0 = 95.7 + 4.2 + 36.1 (in 5.3 s) | **0 / 0 / 0** | 256.9 | 79.8 | 15.8 | 35.6 | 13.2 | 28.4 | identical, VERIFY OK |
| M11 run 2 | 136.0 | 0 / 0 / 0 | 256.9 | 79.4 | 15.6 | 35.5 | 13.2 | 28.2 | identical, VERIFY OK |
| M11 run 3 | 136.0 | 0 / 0 / 0 | 256.9 | 82.3 | 18.7 | 35.4 | 13.2 | 28.2 | identical, VERIFY OK |

Wall 80.5 ± 1.5 s against 83.0 ± 1.2: not slower — the reciprocal loses its four in-phase `hipMalloc` + `hipMemset` +
`hipDeviceSynchronize` (15.2 → 13.2 s), the bs top levels their 3 × 3.26 GB fallbacks; init pays 40 GB more of
mapping (≈ 2.4 s at 0.06 s/GB, inside the same wall). `DB_POOL_VERBOSE=1` logs no fallback in any phase; the driver's
per-APU used bytes equal the accounting (`MEM_REPORT_DEVS=1`). The layout matched the model to the byte: APU0 36.78 GB,
APUs 1–3 33.07 GB (= max(2 × half + hole, dm need) per region).

**10¹¹ on one node (the same job; v1 layout): VERIFY OK in 416.6 s** (init 45.6 — arenas 333.1 GB in 14.8 s; bs 161.2 =
batch 61.6 + top levels 99.3; dm 209.7 = reciprocal 92.7 + division 117.0; the 100 GB file written in 231 s in the
writer thread, `/tmp` of the node), T1 8 primes ok, T2 windows ok, digits `2.718281828459045235360287471352662497757247093699959574966…328739822518252791537703810896884928977`
(the first 8 × 10¹⁰ digits' prefix equals §75's; no reference at this size). Memory: **device 454.0 GB from init to the
end of the division (planes 120.3, arena 333.1 = bs regions 242.8 + tails 90.3, zero hipMalloc), host 13.9 GB HWM
(staging 8.6 at init, 4.3 after) — node peak ≈ 468 of 502 GB**; the pool's peak live 283.3 GB against the dm need
the layout computed (298.4: +5 %). With v2 (the tails not added: max(242.8, 298.4) = 298.4) the same run maps
419 GB of device, node peak ≈ 433. Time per digit 4.17 ns (8 × 10¹⁰: 2.63; 4 × 10¹⁰: 2.0): the top levels and the
reciprocal at this length are 2^31-point pieces in grids of 9–12 (`split_grid`), and the reciprocal's 129 `dist`
calls take 170 s of which 107 s are `load` — the piece loads at 10¹¹ are the item to look at, not memory.

**8 × 10¹⁰ (the same job; v1 layout): VERIFY OK in 190.9 s** (Phase 10: 210.4 s) — init 20.7 (arenas 249.0 GB in
10.6 s = bs 176.8 + tails 72.2), bs 87.0, dm 83.1 (reciprocal 37.9 against 49.6: C3's 6.8 s of growth and the fallback
gone), the 80 GB file in 154 s in the writer thread. **Device 369.9 GB throughout (zero hipMalloc; Phase 10: 386.6 at
the reciprocal), host 12.8 GB: node peak 383 against 393 GB**; the pool's peak live 229.3 as before. With v2 the
arena is max(176.8, 243.0) = 243.0: device 363.3, node peak ≈ 376.

#### 2. The tree's slabs at size > 1 sized into the arena

`tree_need_dev(n_leaf, size)` lays out every tree level l (group g = 2^l, the last clipped to size) the way
`rns_mul_dist_mn` / `mn_core` size their scratch: the transform length n = 2^⌈log₂ 2 N_A⌉ over nr = 4 g_t ranks,
R = 2^(⌊log n / 2⌋ + `DIST_LOGR_DELTA`), rows = R / nr, q = n / nr; per device the slabs sb and rbA of g × S_max
limbs, rbB of g × S_B (S = (share / R + 2) rows, capped at q), cx and tmp of q limbs, two spill buffers of g × C × 4
limbs, the window temporary T (a quarter of the share + 1/8), beside the level's live shares (the inputs' P, Q and
the outputs' with the bound's 1/8); the largest level + 1/16 is the tree need; the top level's scratch alone is
added to the sharded division's need (the same products run there). The arena at size > 1 is
`max(2 × bs half, dm need / size + top scratch, tree need)` per device.

**v2 (the hole not added; job 20814, s24-30):** 4 × 10¹⁰ × 3 — arena 132.3 GB (= 95.7 + 36.6 of dm extra; tails 36.1
inside it, allocated in 5.8 s), **zero hipMalloc in bs, recip and division, device 253.1 GB at the dm peak (Phase 10:
261.9), digits identical, VERIFY OK; wall 83.2 / 86.8 / 84.7 s** = phases 66.7 / 66.9 / 67.2 (the Phase 10 closing's
66.7 ± 0.3 to the tenth: bs 37.0–37.3, dm 29.6–29.8) + init 16.5 / 19.9 / 17.5 (closing 16.4 ± 1.1: the 37 GB of extra
mapping is +2.2 s at 0.06 s/GB, partly hidden). Against the closing's 83.0 ± 1.2 the v2 mean 84.9 ± 1.8 is 1.9 s slower,
all of it in init; v1 on s24-26 was 80.5 ± 1.5 with phases at 63.6–64.0 — s24-26 runs the phases 3 s faster than
s24-30 in every series (RESULTS §68), so the two series are not comparable and the phases, which are the same
work, are the honest measure: unchanged. The tails' stats: 3 large blocks from the back per APU (t1 in the
reciprocal, then the division's two products), 0 spills.

**10¹⁰ at size 4 (v2, `POOL_LOG=29`):** identical to `results/e_1e10.out`, all four nodes VERIFY OK, **116.5 s**
(Phase 10: 145.7); per process the arena is 20.3 GB (= 11.3 leaf regions + 9.0 dm extra; tails 2.3), **the tree's
hipMalloc 0 (was 4.9–5.4 GB per process), the sharded dm's 0** (peak live 11.0 in the tree, 16.6 in the dm, inside
the 20.3); the tail served 16 large blocks per APU, 0 spills. Host 29.3 GB HWM per process.

**10¹¹ with v2 (the same job): VERIFY OK, but two APUs fell back** — the arena max(2 × half, dm need) = 74.6 GB on
every APU is *smaller* than v1's on APU0/1 (91.0 / 84.5: their bs halves are the larger ones), so on APU2/3 the device
top levels' P, Q (11.1 GB quarters, alive into dm) had to enter the tail (nothing else fit) and t1 found "free 30.2 GB
in 2 extents, largest 17.4": 22.2 GB of hipMalloc per APU, device 463.8 GB (v1: 454.0). Also 672 s against 416 —
bs 260 (batch 92, top levels 168) against 161 (62 / 99), every tier 50–70 % slower on s24-30 than on s24-26 at this
size, which no memory item explains (4 × 10¹⁰ on the two nodes differs by 3 s of phases); not chased.
**v3: the dm need also covers the top levels' live beside the tail** — the last level's inputs (2 P,
2 Q of n_Q/2) and outputs (P, Q of n_Q, the bound's margin) + 1/8, plus the hole: 78 GB per APU at 10¹¹ (v1: up to 91,
v2: 74.6), the same 33.1 at 4 × 10¹⁰ and 62.1 at 8 × 10¹⁰ (v2: 60.8). 8 × 10¹⁰ with v3: zero hipMalloc, device 369.1, 195.5 s.
At 10¹¹ v3 still lost one APU (one small block spilled into the tail's middle: "free 33.1 GB in 2 extents, largest
17.4"): the top levels' fragmentation is not bounded by the level totals + 1/8. **v4 (the final code): a small block
that must enter the tail is carved from the tail's very end**, so what it leaves is one hole contiguous with the
free bytes before it (22 GB of the 33 stay one extent) — the spill no longer costs t1 its place unless the tail
itself is short. **10¹¹ with v4: VERIFY OK in 262.9 s, zero hipMalloc on every APU, device 431.2 GB from init to the end of the
division (= the model's 431.2), host 14.0 GB — node peak 445 of 502 GB** (v1: 468; Phase 10's layout, modelled: 455).

RESULTS4

The model beyond the calibration points (size 1, tail v2; GB):

| D per node | n_Q (limbs) | t1 quarter | bs regions | dm need → arena | device init / dm peak | host HWM | node peak | Phase 10 layout: pool / node peak |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 4 × 10¹⁰ | 2.22 × 10⁹ | 8.9 | 95.7 | 132.3 → 132.3 | 216.6 / 253.1 (**measured 253.1**) | 15.6 | 265 | 142.8 / 276 |
| 8 × 10¹⁰ | 4.44 × 10⁹ | 17.8 | 176.8 | 248.2 → 248.2 | 297.7 / 369.1 | 15.6 | 381 | 262.5 / 395 |
| 9 × 10¹⁰ | 5.00 × 10⁹ | 20.0 | 222.0 | 279 → 279 | 342.9 / 400 | 15.6 | 412 | 292.4 / 425 |
| 10¹¹ | 5.56 × 10⁹ | 22.2 | 242.9 | 310.3 → 310.3 | 363.7 / 431.2 | 15.6 | 443 | 322.3 / 455 |
| 1.1 × 10¹¹ | 6.11 × 10⁹ | 24.4 | 266.0 | 341 → 341 | 386.9 / 462 | 15.6 | 474 | 352.2 / 485 |
| 1.2 × 10¹¹ | 6.67 × 10⁹ | 26.7 | 271.3 | 372.3 → 372.3 | 392.1 / 493.2 | 15.6 | 505 | 382.1 / 515 |

(host HWM 15.6 = the init peak with the two seed buffers: measured 11.5–13.9 — the runtime's share grows slowly
with D; the model keeps 7 GB.) **Ceilings per node at size 1 (v3): 502 GB → 1.19 × 10¹¹ with the tail layout
(1.16 × 10¹¹ without — the Phase 10 layout maps t1 twice over but nothing for the fit); 480 GB with a 5 % margin
(456 GB) → 1.04 × 10¹¹ (1.0 × 10¹¹ without).** The measured 10¹¹ runs sit at 454 + 13.9 = 468 GB (v1) and 464 + 14
(v2, with its two fallbacks); v3 models 443.

**At size g > 1 (the 576-node question).** Per node the leaf's bs regions are those of D digits, the dm phase's shares
are D's, and the tree's products add per device: the slabs (≈ 3 × share/4 — constant in g), cx and tmp (2 q,
q = n_top / (4 g) — constant in g), the window temporary, and **the two spill buffers of g × C × 4 limbs each,
which grow with g: C = n / R = 2^⌈log₂ n / 2⌉ ≈ 2^20 at the 576-node top level (n = 2^40–2^41), so 2 × 576 × 2^20 ×
32 B = 38.6 GB per APU = 155 GB per node** (`rns_dist.c` `mn_core`, `spill_sb` / `spill_rb`; `DIST_LOGR_DELTA=3`
makes C 8 × smaller: 4.8 GB per APU). The plane pools grow with the top level's slice too (n_top / (4 g) > 2^29 →
`rns_dpool` re-sizes pool 0 and 1: 214 GB per node above D ≈ 2 × 10¹⁰ at g = 576). And before L's `alltoallv`
(B7) the sharded division's `mdb_shift` scratch (g × share/4 limbs per APU) and `mdb_add_shifted` (g × 512 MB per
APU) make g = 576 impossible at any D (1.2 TB per node); after it the exchange is the counts' own size (17 GB per
node at D = 1.9 × 10¹⁰).

| 502 GB node, `alltoallv` in (L's B7) | g = 4 | g = 64 | g = 576, `DIST_LOGR_DELTA` 0 | g = 576, delta 3 |
|---|---:|---:|---:|---:|
| max D per node (tail layout) | 3.9 × 10¹⁰ | 3.9 × 10¹⁰ | 1.9 × 10¹⁰ | 2.2 × 10¹⁰ |
| node peak at it: planes / arena (dm need, tree need) / exchange / host | 121 / 238 (238, 198) / 35 / 18 = 411 | 121 / 246 (246, 207) / 35 / 18 = 420 | 121 / 318 (318, 316) / 17 / 18 = 476 | 214 / 251 (251, 244) / 20 / 18 = 501 |
| digits over g nodes | 1.5 × 10¹¹ | 2.5 × 10¹² | **1.1 × 10¹³** | **1.25 × 10¹³** |
| without B7 (`mdb_shift` padded) | 3.9 × 10¹⁰ (exchange 77) | 6 × 10⁹ (exchange 308) | none | none |

The g = 4 and g = 64 ceilings sit at 3.9 × 10¹⁰ because the top level's product at 4 × 10¹⁰ per node needs a 2^33 /
2^37 transform whose slice exceeds the 2^31 plane pool (the pools would double: a step, not a slope). The
Phase 10 closing figure (576 × 8 × 10¹⁰ = 4.6 × 10¹³) assumed the single-node profile per node; the tree's scratch
at g = 576 (spills, slabs, the grown planes) is what the model adds, and it costs a factor ≈ 4 in D per node —
the spill buffers being the item to fix (they hold 4 carry limbs per (segment, column) for every one of the g
segments; only the columns that spill are needed, and the exchange could go by counts like B7's).

**The size > 1 scratch after L's `alltoallv`, per node at g = 576 (the model's accounting; X's model puts the per-node
ceiling at 7.7–8 × 10¹⁰ from the same question):**

| D per node | bs regions | dm shares (Q, S, r, r2, t1, piece: 1/576 of the whole) | the top level's slabs (sb, rbA, rbB) per APU | cx + tmp (2 q) per APU | spills 2 g C × 4 limbs per APU, delta 0 / 3 | T per APU | planes | exchange (after B7) | host | node peak, delta 0 / 3 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10¹⁰ | 42 | 41 | 5.6 | 7.6 | 38.7 / 4.8 | 2.2 | 121 | 9 | 22 | 405 / 271 |
| 1.9 × 10¹⁰ | 65 | 74 | 10.6 | 7.6 | 38.7 / 4.8 | 4.2 | 121 | 17 | 22 | 474 / 339 |
| 2.2 × 10¹⁰ | 72 | 82 | 12.2 | 15.3 | 77.3 / 9.7 | 4.9 | 214 | 20 | 22 | 773 / 503 |
| 4 × 10¹⁰ | 104 | 132 | 22.3 | 30.5 | 77.3 / 9.7 | 8.9 | 428 | 36 | 22 | 1170 / 901 |
| 7.7 × 10¹⁰ | 194 | 239 | 42.8 | 30.5 | 77.3 / 9.7 | 17.1 | 428 | 69 | 22 | 1425 / 1155 |
| 8 × 10¹⁰ | 201 | 248 | 44.5 | 61.1 | 154.6 / 19.3 | 17.8 | 856 | 71 | 22 | 2305 / 1765 |

(GB; "per APU" items × 4 for the node. The exchange after B7 = 2 × share/4 limbs per APU + 64 MB: 17–71 GB per node,
no longer the item.) What decides the ceiling at 576 is not the exchange but the transform scratch of the top-level
product, which is a product of two 576-node-sized operands: (a) the plane pools — the slice q = n / (4 g) of a 2^40…2^43
transform exceeds the 2^31 pools' 2^29 limbs above D ≈ 2 × 10¹⁰, and `rns_dpool` grows pool 0 (4 q) and pool 1
(3 q) to it: 214 GB at 2.2–4 × 10¹⁰, 856 at 8 × 10¹⁰; (b) cx + tmp, 2 q per APU, the same slope; (c) the spills,
2 × g × C × 4 limbs per APU with C = n / R: 39–155 GB per node at delta 0. The bs regions and the dm shares are D's
own (1/576 of the whole number each) and would allow 8 × 10¹⁰ per node on their own (201 + 248 + 121 + 71 + 22 =
663 — over 502 by the planes' 121 + the dm's slabs; the single-node 8 × 10¹⁰ fits because its top product is
piece-gridded at 2^31). So the 576-node top product needs what the single-node reciprocal already has: **a piece
grid over the 2^31 planes instead of one transform over all of n** (`split_grid` in the mn tier), which removes
(a) and (b) and bounds (c) by C of a 2^31 plane; then the per-node ceiling returns to the shares' line — the
single-node profile minus the dm slabs: ≈ 8 × 10¹⁰ per node with the tail layout (X's 7.7–8 × 10¹⁰).

**Standing rule (PLAN §26): the 576-node estimate.** Modelled, with the code as it is (one transform over the whole top product): **1.1–1.25 × 10¹³ digits**
(1.9–2.2 × 10¹⁰ per node × 576, `alltoallv` in, the tail layout, `DIST_LOGR_DELTA` 0 / 3) at 502 GB per node. Modelled,
with the top-level product piece-gridded over the 2^31 planes (the single-node reciprocal's form; not written):
**≈ 4.4–4.6 × 10¹³ digits** (7.7–8 × 10¹⁰ per node, the shares' line, X's figure). The single-node ceiling is
1.19 × 10¹¹ (measured 10¹¹ at 445 GB). Wall: the per-node compute at 2 × 10¹⁰ is
≈ 2.0 ns/digit × 2 × 10¹⁰ ≈ 40 s (measured 79.4–82.3 s at 4 × 10¹⁰, extrapolated linearly — the sizes below 4 × 10¹⁰
run at the batch tier's rate) plus the tree's 10 distributed levels and the sharded division over the fabric,
which agent X's model prices; at Phase 10's "5–6 min plus the fabric" for 8 × 10¹⁰ per node the compute part
here is **≈ 1 min per node, extrapolated**, the fabric part modelled by X2.

#### 5. Tests run (aac6, one node; jobs 20804 (s24-26), BATCH3JOB; logs in `~/ntt-m11/ecalc/results/m11/`, scripts `m11b{1,2,3}.sh`)

| run | command (from `ecalc/`, `$J` the allocation; `ECALC_VERBOSE=2 RNS_VERBOSE=1 DB_POOL_VERBOSE=1` everywhere) | result |
|---|---|---|
| 10⁹ size 1 | `./ecalc 1000000000 /tmp/m11_e9.txt` | identical to ref/e_1000000000.txt |
| 10⁸ sizes 2, 4 | `SLURM_JOB_ID=$J ./mnrun.sh <p> env POOL_LOG=27 ./ecalc 100000000 /tmp/m11_e8.txt` | identical (parts concatenated), every node VERIFY OK |
| 10⁹ sizes 2, 4 | `./mnrun.sh <p> env POOL_LOG=29 ./ecalc 1000000000 /tmp/m11_e9x.txt` | identical |
| 4 × 10¹⁰ size 1, × 3 (v1) | `MEM_REPORT_DEVS=1 ./ecalc 40000000000 /tmp/m11_e4e10.txt`, reference evicted | identical, 79.8 / 79.4 / 82.3 s, zero hipMalloc |
| 10¹¹ size 1 (v1) | `./ecalc 100000000000 /tmp/m11_big.txt` | **VERIFY OK, 416.6 s, device 454.0 GB, host 13.9 GB** |
| 8 × 10¹⁰ size 1 (v1) | `./ecalc 80000000000 /tmp/m11_big.txt` | **VERIFY OK, 190.9 s, device 369.9 GB, host 12.8 GB** |
| 4 × 10¹⁰ size 1, × 3 (v2, job 20814, s24-30) | as above | identical, 83.2 / 86.8 / 84.7 s (phases 66.7–67.2 = the closing's), zero hipMalloc, device 253.1 |
| 10¹⁰ size 4 (v2) | `./mnrun.sh 4 env POOL_LOG=29 ./ecalc 10000000000 /tmp/m11_e10.txt` | identical to results/e_1e10.out, 4 × VERIFY OK, 116.5 s, tree + dm hipMalloc 0 (arena 20.3 GB per process) |
| 10¹¹ size 1 (v2) | | VERIFY OK, 672 s (s24-30, every tier 50–70 % slower — not memory), 2 APUs fell back 22.2 GB each, device 463.8 |
| 4 × 10¹⁰ size 1 (v3, job 20819, s24-16) | | identical, 85.3 s (phases 64.0, init 21.3), zero hipMalloc, device 253.1 |
| 10¹¹ size 1 (v3) | | VERIFY OK, 434.7 s, one APU fell back 22.2 GB (a top-level block spilled into the middle of its tail), device 453.4 |
| 8 × 10¹⁰ size 1 (v3) | | **VERIFY OK, 195.5 s, zero hipMalloc, device 369.1 GB, host 12.8: node peak 382 (Phase 10: 393)** |
| 10⁹ size 1; 10⁸ / 10⁹ sizes 2, 4 (v4, job 20829, s24-30) | as in the first rows | identical, 0 fallbacks in every run |
| 4 × 10¹⁰ size 1, × 3 (v4) | | identical, 83.2 / 87.6 / 83.8 s (phases 66.8 / 67.0 / 66.9, init 16.4 / 20.6 / 16.8), zero hipMalloc, device 253.1 |
| 8 × 10¹⁰ size 1 (v4) | | VERIFY OK, zero hipMalloc, device 369.1, host 12.8 (290.9 s on s24-30: the slow node, see the 10¹¹ v2 note; 195.5 on s24-16, 190.9 v1 on s24-26) |
| 10¹¹ size 1 (v4) | `./ecalc 100000000000 /tmp/m11_big.txt` | **VERIFY OK, 262.9 s, zero hipMalloc, device 431.2 GB, host 14.0: node peak 445 GB** (APU2: 6 spills into its tail, all at its end; t1 took the best fit) |

#### 6. Open issues

1. **The spill buffers of `rns_mul_dist_mn` scale with g** (2 × g × C × 4 limbs per APU: 155 GB per node at 576
   nodes and 2 × 10¹⁰ per node with `DIST_LOGR_DELTA=0`, 19 GB with 3): the first memory item on the target, in L's /
   X's file (`rns_dist.c`). The model has the knob; the code sizes the arena for it.
2. The plane pools' growth at the top tree level (n_top / (4 g) above 2^29 limbs per APU) is a step of +93…+120 GB
   per node; a 2^32-point pool (`POOL_LOG=32`, the paper's alternative) or a piece grid at the top level (as the
   single-node reciprocal does) would move it. Not mine.
3. At size > 1 the arena is now sized for the sharded division's shares + the top level's scratch (`dm_layout`);
   the size-4 dm phase at 10⁹ still showed 0.11 GB fallbacks per APU with v1 (before the top scratch was added) —
   the batch-3 row above says what v2 does at 10¹⁰ / 4. The sharded division's own temporaries (`recip_mn`,
   `newton_mn_divmod`: X's file) are modelled as 1/size of the single-node ones; a mem_report row per tree level
   would pin them down.
4. `ECALC_DM_POOL=1` (C3) is now a no-op with the tail (P's switch): its rule sees the largest extent ≥ t1's
   quarter and the free bytes ≥ its need; it can be deleted with the tail on by default.
5. The tail's policy is soft; `db_pool_tail_stats` (printed at `db_release_pools` under `DB_POOL_VERBOSE=1`) says
   how many large blocks it served and how many small ones spilled into it — 0 spills in every run here.
6. Time at 10¹¹: 4.17 ns/digit against 2.63 at 8 × 10¹⁰ — the reciprocal's 129 `dist` calls spend 107 s in `load`
   (the piece loads of a 2^31-point grid over 5.6 × 10⁹-limb operands); memory is no longer what limits one node
   below ≈ 1.2 × 10¹¹.

#### Files touched outside my list

None. (`ecalc/binsplit.h`: one declaration, `binsplit_dm_hole_bytes`; `ecalc/dbig.h`: three.)

#### 3. The per-node memory model — `ecalc/mem_model.py` (`mem_per_node(D, g, opts) -> dict`)

Device and host bytes of one node-process as functions of the digits per node D and the group size g, from the
code's own sizing formulas (ported, not fitted): `e_terms`, `seed_limbs`, `region_need` (the simulated layout,
in closed form per region), `dm_layout`, `tree_need_dev`, the plane pools (4 q + 3 q + 16 limbs per APU at
2^pool_log, grown on demand when the top tree level's slice n / (4 g) exceeds them), the sharded exchange's
scratch (`mdb_shift`: g × share/4 limbs per APU and `mdb_add_shifted`: g × 512 MB per APU before L's
`alltoallv`; the counts' own sizes after — `opts['alltoallv']`), and the host's measured constants (runtime 7.0,
staging 4 × 1 GiB, the seed buffers 4 GiB at init, the writer 0.65 GB, + 6 GB per process at size > 1).
`opts`: `pool_log`, `tail` (item 1's layout or Phase 10's), `alltoallv`, `logr_delta` (`DIST_LOGR_DELTA`),
`margin`. Agent X's `mn_model.py` imports it for the memory rows. `python3 mem_model.py` prints the tables.

**Calibration (size 1, GB; measured = the mem_report tables of results/MEMORY.md §M, H.md):**

| D | item | planes | regions | pool at the dm peak | device at the dm peak |
|---|---|---:|---:|---:|---:|
| 4 × 10¹⁰ | measured (Phase 10) | 120.3 | 95.7 | 141.0 | 261.9 |
| | model, tail off | 120.9 | 95.7 | 142.8 | 263.7 (+0.7 %) |
| | **measured, tail (v1, batch 1)** | 120.3 | 136.0 | 136.0 (hipMalloc 0) | 256.9 |
| | model, tail v1 / v2 | 120.9 | 95.7 | 136.0 / 132.3 | 256.8 / 253.1 |
| 7 × 10¹⁰ | measured (Phase 10) | 120.3 | 169.6 | 231.8 | 352.7 |
| | model, tail off | 120.9 | 169.6 | 232.6 | 353.4 (+0.2 %) |
| 8 × 10¹⁰ | measured (Phase 10) | 120.3 | 176.8 | 265.7 | 386.6 |
| | model, tail off | 120.9 | 176.8 | 262.5 | 383.3 (−0.8 %) |

## M13 — memory truth: one ceiling, the two O(share) terms bounded (Phase 13a, PLAN §30 row M; TASKS 1.1, 1.2, 1.3)

(former file: M13.md)

Branch `p13-M` (from `main` @ 203733b; aac6 clone `~/ntt-M13`, logs `~/ntt-M13/ecalc/results/m13/b{1,2}/`, scripts
`~/m13_b1.sh`, `~/m13_b2.sh`). Files touched: `ecalc/mem_model.py`; `ecalc/binsplit.c` (a new
`binsplit_layout_only` beside the sizing functions, one line in `binsplit_pregrow`; nothing in the checkpoint code);
`ecalc/newton_db.c` (`mdb_shift_g` and one summary line in the division); `ecalc/rns_dist.c` (`mn_core`'s result
exchange, `mdb_add_shifted`, `rns_mul_dist_mn_scratch`'s window term; no prime-count or plane-size line);
`ecalc/README.md` (three rows in the switch table — outside my list, a documentation-only addition).
**Defaults unchanged**: every new path is behind a switch that is off by default.

#### HEADLINE

| configuration (502 GB node; the safe budget 480 GB in brackets) | ceiling per node | digits in all | label |
|---|---|---|---|
| **size 1, the defaults** | **1.18 × 10¹¹** (1.11 × 10¹¹) | 1.18 × 10¹¹ | modelled; the model = measured device to ±0.05 % at 1e10, 4e10, 8e10, 1e11 |
| **576 nodes, the target as coded (SHMEM, pool-resident slabs, 8 GiB pool, `MN_GROUPS` default)** | **6.95 × 10¹⁰** (6.50 × 10¹⁰) | **4.00 × 10¹³** | modelled, conservative (the exchange scratch counted on top of the dm need) |
| 576, `MDB_SHIFT_CHUNK_MB=1024` | 8.30 × 10¹⁰ (7.78 × 10¹⁰) | 4.78 × 10¹³ | modelled |
| 576, `MN_T_CHUNK_MB=1024` | 7.55 × 10¹⁰ (7.05 × 10¹⁰) | 4.35 × 10¹³ | modelled |
| 576, both at 1024 MB | **9.24 × 10¹⁰** (8.58 × 10¹⁰) | **5.32 × 10¹³** | modelled |

`python3 ecalc/mem_model.py --ceiling` prints this table with the node peak split at each ceiling.

**The "4 × 10⁹ digits/node" disagreement, term by term.** Neither number came from the C code: both are
`mem_model.py`, in two versions and with two options.

| term (GB per node at 576 × 6.7 × 10¹⁰) | 7.1 × 10¹⁰ (G12 §4) | 6.7 × 10¹⁰ (Q.md, RESULTS §77, internal target notes) | worth |
|---|---:|---:|---:|
| the top level's product scratch the dm need carries (`top_scratch`) | 74.5 — G's line-by-line port of `rns_mul_dist_mn_scratch` (the code as merged) | 86.4 — Q's own estimate of the gridded form, written before G's code existed (slabs ≤ 3q/2, cx + tmp 2q, v-scratch 3q/4, T) | 0.21 × 10¹⁰ |
| the SHMEM transport's symmetric pool on the host | 0 — `transport='tcp'` | 8.6 — `COMM_SHMEM_POOL_MB` 8192 + 100 MB | 0.17 × 10¹⁰ |
| planes, bs regions, exchange, the rest of the host | 120.9 / 173.3 / 59.8 / 17.9 | the same | 0 |
| **ceiling** | **7.12 × 10¹⁰** | **6.70 × 10¹⁰** | **0.42 × 10¹⁰** |

Since the merge, `estimate.py` (which imports the merged `mem_model.py`) already gives **6.95 × 10¹⁰** — G's
scratch with Q's SHMEM pool; the 6.7 in RESULTS §77 and internal target notes is stale by the first row. The SHMEM
pool is real on the target (8 GiB by default, host-registered or a device heap: either way in the node's HBM), so
the target's number is 6.95, not 7.12.

**The C request against the model** (`BS_LAYOUT_ONLY`, new: the arena `binsplit_pregrow` would request, printed
without allocating it; `mem_model.py --check-c <log>` compares): after one fix (the model did not round the dm extra
to 2 MiB as `arena_get` does) **the model's arena equals the C request to the byte at all 35 points** — size 1 at
1e9 … 1e11, size 4 at 2.5 × 10⁹ (POOL_LOG 29) and 4 × 10¹⁰, 64 at 4 × 10¹⁰, 576 at 1e9 … 8 × 10¹⁰ per node, with and
without `MN_T_CHUNK_MB`. Every sub-term agrees exactly (n_Q, the hole, the dm need, the top scratch, the tree need,
the bs regions). There is no C/model disagreement in the arena.

**What the arena does not contain, and the model must** (the ceiling is the node peak, not the arena):

| term | in the C request? | in the model? | status |
|---|---|---|---|
| plane pools | no (`rns_init`) | yes | **was wrong**: the model had 120.3 GB always; since Phase 12 I the 3·2ᵏ planes are the default below 5 × 10¹⁰ digits of the run (d = D × g), 180.4 GB. Fixed (`planes_3q30`). No effect at 576 (d ≥ 5 × 10¹⁰), +60 GB at size 1 below 5 × 10¹⁰ |
| the arena mapped at init | — | **was wrong**: `dev_init` counted the bs regions only; the arena (bs + dm extra) is mapped at init since M11 v2. Fixed: the init peak (with the 4 GiB of seed buffers) now binds at size 1 → 1.19 → 1.18 × 10¹¹ |
| the sharded division's exchange (`mdb_shift`, 2 × share/4 per APU) | no: `db_pool_alloc` beyond the arena when it does not fit | yes, **added on top of the dm need** | the conservative assumption; measured at 1e10/4 it does not stack (below). Worth 6.95 → 8.41 × 10¹⁰ at 576 if it never stacks; `MDB_SHIFT_CHUNK_MB` removes the question (8.30) |
| host: runtime 7, staging 4.3, seed buffers 4.3 (init), comm 6 / process, SHMEM pool | no | yes (constants) | measured 12.1 (4e10) … 16.6 (1e10) at size 1 against 15.6 modelled; 24.8 per process at 1e10/4 over TCP against 21.6 — see open issue 1 |
| the transform cache over shares (`RNS_DIST_CACHE_MN`, 2 slots × 17.2 GB per APU) | no: `hipMalloc` at the first grid product | no | opportunistic: takes slots only while free − 24 GB per APU allows (`RNS_DIST_CACHE_MARGIN_GB`), so at the ceiling it takes none; below it, the margin (24 GB per APU) must exceed the in-phase growth — the exchange is ≤ 17.8 GB per APU up to 8 × 10¹⁰ at 576, so it holds |
| `v->spill` (4 C + 64 limbs per APU, `hipMalloc`), descriptors | no | no | ≤ 34 MB per APU: noise |

**Model against measurement** (`python3 ecalc/mem_model.py`, the calibration table; measured = `mem_report`, job 21008 and M11):

| run | measured device (planes + arena + tables) | model | device error | node (device + host HWM) error |
|---|---|---|---:|---:|
| 1e10 size 1 | 234.4 = 180.4 + 53.4 + 0.61, hipMalloc 0 | 234.36 | −0.02 % | −0.4 % |
| **4e10 size 1** | **313.3 = 180.4 + 132.3 + 0.61**, hipMalloc 0 (= RESULTS §77) | 313.26 | −0.01 % | +1.1 % |
| 8e10 size 1 (M11) | 369.1 | 369.10 | 0.0 % | +0.7 % |
| **1e11 size 1 (M11)** | **431.2** | 431.16 | −0.01 % | +0.3 % |
| 1e10 size 4 (POOL_LOG 29, per process) | 56.6 = 34.4 + 19.9 + 2.3, hipMalloc 0, peak live 14.3 | 58.04 | **+2.5 %** (the exchange 2.5 GB counted on top; it did not stack) | −9 % (host, open issue 1) |

Stated error: **device within 0.05 % at size 1 (1e10, 4e10, 8e10, 1e11); at size > 1 the model is conservative by
+2.5 % (1e10/4)**; the node total within ±1.5 % at size 1.

#### 1.2 — `mdb_shift` in rounds (`MDB_SHIFT_CHUNK_MB`)

`newton_db.c` `mdb_shift_g`: with `MDB_SHIFT_CHUNK_MB=m` the pieces are exchanged in K rounds, round c carrying the
sub-range [a + len·c/K, a + len·(c+1)/K) of every piece part; K = ⌈SL / chunk⌉ from the group's share bound (the same
on every node: one `alltoallv` per round on every mesh). sb and rb hold ≈ m MB per APU each instead of share/4 —
at 576 × 8 × 10¹⁰: 71 GB per node → 8.6 GB at m = 1024. The same limbs move to the same places: bit-identical by
construction, and measured identical (below). Off (0) by default; K = 1 is the old code path.

#### 1.3 — the window temporary T in rounds (`MN_T_CHUNK_MB`)

`rns_dist.c` `mn_core`: with `MN_T_CHUNK_MB=m` the window of a piece product goes through T in rounds of W = 4m MB of
limbs per node: round c exchanges every node's chunk c of its window (the rows by `alltoallv` from the CRT output, the
exact spills of the chunk), scatters it into a T of one chunk, and one thread (`omp single`) adds the chunk's spills
and the previous chunk's carry into T, then T into C's share (`mn_round_add`). T and rbO are O(W) instead of O(the
share of C). The first (direct) piece goes through the rounds too (the same value added to C's zero-filled share),
and T is allocated after the operands' sequences are freed. The carries, which is what makes it exact:

* within a round, T += spills + the previous chunk's carry: the chunk's digits are < 2B each (a row limb and at most
  one spill limb per position), so the two together carry out at most once (checked, abort otherwise);
* C's share receives the chunks by fixed-length adds; C < B^cn and the window's locally normalised part < B^tn, so
  all the adds together carry out of the share at most once (checked) → the node scan with the last add's propagate;
* the piece's carry out of a window that ends at its share's top is a unit at the next node's share bottom → a
  second scan (propagate 0: every carry-out it causes is new, `share_carry_fix`'s extra rounds); a window that ends
  inside its share adds it locally.

The value added to C is the same as the one-T form's, so the normalised result is identical. `mdb_add_shifted` gets
the same rounds (`mdb_add_shifted_rounds`: round c = every node's chunk c, APU d serving the quarter d of it — which
is the dbig's own quarter layout of a W-limb T), since after the pieces its T (≤ the share) would otherwise be the
peak. `rns_mul_dist_mn_scratch` (the arena request) takes min(window, W) for rbO and T, and `mem_model.mn_scratch`
the same (the `--check-c` comparison above includes the chunked request: exact).

**Measured (job 21008)**: correctness and the arena request —

| run (one node, s24-30; `ECALC_VERBOSE=2 MEM_REPORT_DEVS=1`) | switches | result | wall | block pool (arena) / peak live per process, tree / whole run |
|---|---|---|---:|---|
| `t_mn_grid 0.5 27` at 2 processes | both at 0.01 MB (W = 5 240 limbs per node: thousands of rounds) | **2 × VERIFY OK** — products, + X, low, low cut, held product, views, `mdb_add_shifted`, the subgroup pass | 15 min (the rounds' fixed costs at a tiny W) | — |
| `t_mn_grid 0.5 27` at 3 processes (the general map) | the same | first three shapes, no failure; cancelled at 4.5 min for time | — | — |
| 10⁸ at sizes 2, 3, 4, grids forced (`POOL_LOG=27 MN_TREE_LOGN_TEST=20 DIST_LOGN_TEST=20`) | both at 0.05 MB | **identical**, every node VERIFY OK | 20.1 / 25.1 / 16.7 s | — |
| 10⁹ at sizes 2, 4, grids forced (`POOL_LOG=29 MN_TREE_LOGN_TEST=23 DIST_LOGN_TEST=24`) | both at 2 MB (≈ 27 rounds per shift and per window) | **identical**, every node VERIFY OK | 38.6 / 23.7 s | — |
| 10¹⁰ at size 4, `POOL_LOG=29` | none | identical, 4 × VERIFY OK, hipMalloc 0 | 113.9 s | 19.9 / 9.2 / 14.3 |
| | `MDB_SHIFT_CHUNK_MB=64` | identical | 118.1 s | 19.9 / 9.2 / 14.3 |
| | `MN_T_CHUNK_MB=64` | identical | 101.4 s | **19.3** / 9.5 / 14.6 |
| | both at 64 | identical | 125.4 s | **19.3** / 9.5 / 14.6 |
| 10¹⁰ size 1 | none | identical to `results/e_1e10.out` | 36.2 s | device 234.4 = planes 180.4 + arena 53.4 + tables 0.61 |
| 4 × 10¹⁰ size 1 (reference evicted) | none | identical to `results/e_4e10.out` | 82.6 s | device 313.3 = 180.4 + 132.3 + 0.61 at init and throughout, hipMalloc 0, host HWM 12.1 |

What the 10¹⁰/4 rows say. (i) **The arena request follows the switch, measured**: 19.9 → 19.3 GB per process with
`MN_T_CHUNK_MB=64`, exactly the model's 19.90 → 19.27. (ii) **Neither term sets the peak at this size**: the peak live
is unchanged by `MDB_SHIFT_CHUNK_MB` (14.3: the dm phase's peak is a product, not a shift — so the model's
"exchange on top of the dm need" is conservative here, +2.5 %) and rose 0.3 GB with `MN_T_CHUNK_MB` — that build
allocated the chunk-sized T before the operands, where the direct piece had none; fixed since (T is allocated after
the operands' sequences are freed, 3601181), re-measured in batch 2: fixed. At 10¹⁰/4 the plane terms (q = 2²⁷
limbs per rank at POOL_LOG 29) are as large as the shares' quarter, so T and rbO sit below the operand phase's peak;
at 576 × 6.95 × 10¹⁰ the plane is capped (q = 4.8 × 10⁸ limbs per rank at the top level, 3.8 GB) while the share
grows with D (the top product's share of C 3.86 × 10⁹ limbs: T = 7.7 GB per APU, twice a plane) — the top level's
scratch is 19.1 GB per APU, 12.4 with `MN_T_CHUNK_MB=1024` (`mem_model.mn_scratch`, = the C formula), which is
where the switches' ceilings above come from. (iii) Time: the ±10 % spread of four TCP node-processes on one node hides the
rounds' cost at 64 MB (101–125 s against 114 base); batch 2 repeats it.

**Batch 2 (job 21015, s24-26, 3601181: T allocated after the operands, the `scratch(mn)` line = the measured bytes of
the largest mdb_shift slabs and of T + rbO per node-process; mem[0] = node-process 0):**

| run | switches | digits | wall | mdb_shift slabs (GB) | T + rbO (GB) | arena / peak live tree / whole run (GB) |
|---|---|---|---:|---:|---:|---|
| 10⁹/2, `POOL_LOG=29 MN_TREE_LOGN_TEST=23 DIST_LOGN_TEST=24` | none | identical | 37.6 s | 0.556 | 0.445 | 3.1 / 1.2 / 1.9 |
| | both at 8 MB | identical | 36.7 s | **0.079** | **0.067** | 3.0 / **1.0** / 1.8 |
| 10¹⁰/2, `POOL_LOG=29 MN_TREE_LOGN_TEST=26` | none | identical | 205.8 s | 5.556 | 4.445 | 39.7 / 11.3 / 23.2 |
| | both at 64 MB | identical | 208.0 s | **0.617** | **0.537** | 38.2 / **10.1** / 23.2 |
| 10¹⁰/4, `POOL_LOG=29` | none | identical | 111.3 s | 2.778 | 2.222 | 19.9 / 9.2 / 14.3 |
| | both at 64 MB | identical | 125.6 s (dm 64.9 → 77.4) | **0.556** | **0.537** | 19.3 / 9.2 / 14.3 |

**Both terms are bounded, measured**: the shift slabs fall 5.56 → 0.62 GB and T + rbO fall 4.45 → 0.54 GB per
node-process at 10¹⁰/2 (9×, 8×). Each is now about the chunk (64 MB × 4 APUs × 2 slabs), not the share. The **tree
peak** falls by 1.2 GB (11.3 → 10.1) and the arena by 1.5 GB (39.7 → 38.2, as the scratch formula says). The
whole-run peak does not move at these sizes (23.2, 14.3). That peak is the division's product: at aac6 sizes neither
term is at it. This is what the model's conservative "exchange on top of the dm need" misses (+2.5 % at 10¹⁰/4).
At 576 the two terms decide the ceiling, because the plane is capped and the share is not (the top level's T alone is
7.7 GB per APU against a 3.8 GB plane). Cost: +1 % at 10¹⁰/2 and +13 % at 10¹⁰/4 (the division, over TCP with four
processes on one node, 64 MB per APU-round). On the target the rounds should be at 1 GB, where there are ≈ 10 of them per
call; their cost there is not measured. The fix with T allocated late shows: at 10¹⁰/4 the tree's peak live is 9.2 with
the switches, as without them (batch 1's build: 9.5).

#### Tests

| what | command (from `~/ntt-M13/ecalc`, `$J` the job) | result |
|---|---|---|
| the C request, three sets | `BS_LAYOUT_ONLY=<16 points> ECALC_VERBOSE=2 ./ecalc 1000000 /tmp/x` (POOL_LOG 31, 29, and `MN_TREE…`/`MN_T_CHUNK_MB=1024`) | 35 `layout:` lines; `python3 mem_model.py --check-c <log> 31 [1024]` → **0.0000 %** on every arena and every sub-term |
| t_mn_grid, rounds | `SLURM_JOB_ID=$J ./mnrun.sh 2 env MN_T_CHUNK_MB=0.01 MDB_SHIFT_CHUNK_MB=0.01 ./tests/t_mn_grid 0.5 27` | 2 × VERIFY OK |
| 10⁸ sizes 2/3/4, 10⁹ sizes 2/4, rounds | `./mnrun.sh <p> env POOL_LOG=27 MN_TREE_LOGN_TEST=20 DIST_LOGN_TEST=20 MN_T_CHUNK_MB=0.05 MDB_SHIFT_CHUNK_MB=0.05 ./ecalc 100000000 f`; 10⁹: `POOL_LOG=29 MN_TREE_LOGN_TEST=23 DIST_LOGN_TEST=24`, 2 MB | identical (cmp of the concatenated parts) |
| 10¹⁰/4 × 4 variants | `./mnrun.sh 4 env POOL_LOG=29 MEM_REPORT_DEVS=1 ECALC_VERBOSE=2 [switches] ./ecalc 10000000000 f` | identical ×4 |
| 10¹⁰, 4 × 10¹⁰ size 1 | `srun … MEM_REPORT_DEVS=1 ECALC_VERBOSE=2 ./ecalc 10000000000 f` / `40000000000` (reference evicted) | identical; 234.4 / 313.3 GB device |
| the model | `python3 mem_model.py` (calibration), `--ceiling`, `--check-c`; `python3 estimate.py --max` | 576: 6.95 × 10¹⁰ in both |
| **`mnaccept.sh $J --only unit,e9,mn`** (defaults, 3601181, job 21015) | `./mnaccept.sh 21015 --only unit,e9,mn` | **15 PASS / 0 FAIL** |
| the same `mn` step with both switches at 1 MB | `MDB_SHIFT_CHUNK_MB=1 MN_T_CHUNK_MB=1 ./mnaccept.sh 21015 --only mn` | **5 PASS / 0 FAIL** (10⁸ at 2, 3, 4; 10⁹ at 2, 4 identical) |
| batch 2 (six runs above) | `~/m13_b2.sh` | all identical |

#### Open issues

1. **The host term at size > 1**: at 10¹⁰/4 the process's HWM is 24.8 GB (job 21008; M11 had 29.3) against 21.6
   modelled; `mem_report` puts the growth in "other" (4.4 at init → 8.8 after the tree → 19.8 in the dm phase): the TCP
   transport's host copies of the exchanges, which grow with the exchange sizes. On the target the SHMEM transport
   with pool-resident slabs has no such staging (the pool is counted separately), so the model's 6 GB per process
   is a TCP number at 10⁹; an 8 GB error at 576 is ≈ 0.15 × 10¹⁰ of the ceiling. Measure on the target.
2. **Whether the exchange stacks on the dm need at 576**: the model says yes (conservative, 6.95 × 10¹⁰); at 1e10/4 it
   did not (the dm peak is a product). With `MDB_SHIFT_CHUNK_MB` the question is moot (8.30 vs 8.41).
3. The dm need (per device) is itself an upper bound: at 1e10/4 the arena (19.9 GB) held a peak live of 14.3.
4. The rounds cost time (each round an `alltoallv`, a fixed-length add over the share); measured below at 1e10/4
   over TCP on one node, where the run-to-run spread is ±10 %: the target's cost is modelled nowhere yet.

#### RESUME

Done. Batches 1 (job 21008) and 2 (job 21015) are complete, the gate is met, and no job is live. What is left is for
the integrator: merge, and decide whether to adopt the switches. internal target notes and the RESULTS §77 figure (6.7 × 10¹⁰)
are stale: the merged model and `estimate.py --max` both give 6.95 × 10¹⁰. `estimate.py`'s "device" label still quotes
4e10 = 253 GB (now 313.3). Both are Q's files, which I did not edit.

## P13b — sizing follows the prime count; the plane cap as one switch; the one-node ceiling at three primes (Phase 13b, PLAN §31 row P)

(former file: P13b.md)

Branch `p13b-P` (from `p13b-step0` @ c988218). aac6 clone `~/ntt-P13b`, logs `~/ntt-P13b/ecalc/results/p13b/b*/`,
batch scripts `~/p13b_b*.sh`. Labels: **measured** (aac6; node named per run), **modelled** (the C layout report
`BS_LAYOUT_ONLY`, which is the same formula as `mem_model.py` to the byte, M13), **assumed**. Defaults unchanged: every
new path is behind a switch that is off (step 0's three primes and `NTT_MODMUL=1` are the base).

#### M-RUN SWITCHES

One switch, `ECALC_PLANE_CAP`, off by default (unset: `POOL_LOG` and the 3·2ᵏ size rule exactly as on step 0). Each
value is exactly the three knobs on its row; the switch sets them and refuses a conflicting value already in the
environment (exit 2).

| cap (points per product plane) | exact env | = the knobs it sets | plane pools per node at P = 3 (+0.61 GB tables) |
|---|---|---|---|
| 2³⁰ | `ECALC_PLANE_CAP=2^30` | `POOL_LOG=30 RNS_PLANES_3Q30=0 DIST_LOGN_TEST=30` | 60.1 GB |
| 3·2²⁹ | `ECALC_PLANE_CAP=3*2^29` | `POOL_LOG=30 RNS_PLANES_3Q30=1 DIST_LOGN_TEST=30` | 77.3 GB |
| 2³¹ | `ECALC_PLANE_CAP=2^31` | `POOL_LOG=31 RNS_PLANES_3Q30=0 DIST_LOGN_TEST=31` | 103.1 GB |
| 3·2³⁰ | `ECALC_PLANE_CAP=3*2^30` | `POOL_LOG=31 RNS_PLANES_3Q30=1 DIST_LOGN_TEST=31` | 154.6 GB |
| (fit) | `ECALC_PLANE_CAP=fit [ECALC_NODE_GB=480]` | the largest of the four whose modelled node total fits the budget | — |

Aliases: `30`, `3x29`, `31`, `3x30` (quote `3*2^29` in the shell: `ECALC_PLANE_CAP='3*2^29'`). With the switch unset
the cap is 3·2³⁰ below 5 × 10¹⁰ digits and 2³¹ from 5 × 10¹⁰ (the Phase 12 I size rule, unchanged), so at 4 × 10¹⁰
`ECALC_PLANE_CAP=3*2^30` is the default configuration. Verified on this branch: all four values byte-identical at 10⁹ and
4 × 10¹⁰ (below). Two more settings for the M-run's memory columns: `BS_LAYOUT_ONLY=<D>[:g],...` prints the modelled
node bytes at all four caps without a node (login node is fine); `ECALC_INIT_ONLY=1` maps the pools and the arena and stops
(the allocation edge in ~30 s).

#### What changed (code)

* **The plane pools from one formula at the prime count** (`rns_mul.c` `rns_plane_pool_bytes(pool_log, b3, np, &p0, &p1)`):
  q = 2^(pool_log−2), or 3·2^(pool_log−3) with the 3·2ᵏ planes; pool 0 = np·q limbs; pool 1 = 3q + 16 limbs (one prime
  at a time, so independent of np), never below 2^min(pool_log,30) limbs; each 2 MiB-aligned. `rns_init` (three primes),
  `rns_pool1_default_bytes` and the layout report use it. No value changes: the pools are the same bytes as on step 0
  (measured 287.5 GB at 4 × 10¹⁰, identical to P3's).
* **`BS_LAYOUT_ONLY` needs no device and reports the node** (`binsplit.c` `binsplit_node_bytes`, `ecalc.c`): served before
  `rns_init`, one extra `planes:` line per point with, for each of the four caps, plane pools (at `ECALC_NP`), arena
  (at that cap's `POOL_LOG` — the dm need's piece term is 2^pool_log limbs, so the arena is 8.1 GB smaller at `POOL_LOG=30`
  at 4 × 10¹⁰), device = planes + arena + 0.61 tables, node = device + the host init constants (15.6 GB, mem_model's).
  `*` marks the cap this run's settings give. The existing `layout:` line is unchanged (mem_model `--check-c` still parses it).
* **`ECALC_PLANE_CAP`** (`ecalc.c` `plane_cap_switch`): the switch above; `fit` picks the largest cap whose node total
  (`binsplit_node_bytes` at the prime count) fits `ECALC_NODE_GB` (default 480).
* **`ECALC_INIT_ONLY=1`** (`ecalc.c`): stop after the arena is mapped; prints the init `mem` report.

Files touched: `rns_mul.c`/`rns_mul.h` (the pool setup lines only), `binsplit.c`/`binsplit.h` (sizing and the layout
report), `ecalc.c` (the switch function, one call, the init probe: outside my list, a small self-contained addition).
Nothing in `rns_dist.c` (the cap reaches the dist tier through the existing `DIST_LOGN_TEST`).

#### For agent D (mem_model.py; I did not edit it)

Replace `planes_bytes`' pool-0 term `4 * q` by `np * q` and let the cap choose (pool_log, b3):

```
q  = 3 << (pool_log - 3) if b3 else 1 << (pool_log - 2)          # cap n = 4 q: 2^30 (30,0) 3*2^29 (30,1) 2^31 (31,0) 3*2^30 (31,1)
p0 = round2MiB(np * q * 8)                                       # np = 3 (decimal, the default) or 4
p1 = round2MiB(max((3 * q + 16) * 8, 8 << min(pool_log, 30)))   # one prime at a time: no np
planes = NR * (p0 + p1) + 0.61 GB                                # tables: 0.61 GB measured at P = 3 and 4, POOL_LOG 30 and 31
```

and `dm_layout`'s piece term already takes `pool_log` (use the cap's: 30 for 2³⁰ / 3·2²⁹). Checked against the C
report for all four caps × P ∈ {3, 4} and against the measured device totals below (≤ 0.1 GB).
**The node budget**: measured here, device allocations of 501.4 GB (125.3 GB per APU; the driver reports 137.4 GB per
APU) ran to VERIFY OK beside a 16.4 GB host HWM — 517.8 GB against the 502 GB the OS reports as total memory. The
model's single "device + host ≤ 502 GB" budget is conservative on MI300A: measured, a real run fits while device + host
HWM ≲ 524 GB (523.8 ran, ≈ 529.6 was OOM-killed), and the device allocation alone stops between 515.2 and 518.5 GB
(see the ceiling section). Suggested: a node budget of 524 GB for size-1 ceilings on aac6 (label it measured, s24-30),
keeping 502/480 for the target until measured there.

#### Sizing follows the prime count — measured at 4 × 10¹⁰ (job 21051, s24-16, reference evicted, `ECALC_VERBOSE=2 MEM_REPORT_DEVS=1`)

| run | device peak measured | C layout (modelled) | host HWM | total / phases (bs, dm) | digits |
|---|---|---|---|---|---|
| before: `ECALC_NP=4` (the four-prime sizing) | **313.3 GB** | 313.26 (planes 181.0 + arena 132.25) | 12.1 | 79.90 s / 58.4 (32.4, 25.9) | VERIFY OK |
| after: step 0 default (P = 3, switch unset) | **287.5 GB** | 287.49 (155.2 + 132.25) | 12.1 | 69.44 s / 46.6 (25.2, 21.4) | VERIFY OK |

−25.8 GB, all of it pool 0 (np q). The C report follows `ECALC_NP` (np 3 vs 4 in the `planes:` line) and equals the
measurement to 0.05 GB at both.

#### The cap switch at all four values (jobs 21046 s24-30, 21051 s24-16; three primes)

| cap | 10⁹ (vs ref) | 4 × 10¹⁰ digits (vs results/e_4e10.out) | 4 × 10¹⁰ total / phases (bs, dm), init | device peak: measured / C layout | host HWM |
|---|---|---|---|---|---|
| 2³⁰ | identical, 9.99 s | **identical** (cmp) | 84.20 s / 70.5 (33.9, 36.5), 13.7 | 184.9 / 184.86 | 12.1 |
| 3·2²⁹ | identical, 11.63 s | **identical** (cmp) | 76.79 s / 61.6 (31.1, 30.4), 15.2 | 202.1 / 202.05 | 12.1 |
| 2³¹ | identical, 13.56 s | **identical** (sha1, job 21051) | 70.73 s / 52.5 (28.1, 24.3), 18.2 (s24-30); 67.89 / 50.6 (s24-16) | 236.0 / 235.95 | 12.1–12.2 |
| 3·2³⁰ | identical, 16.78 s | **identical** (sha1, job 21051) | 65.09 s / 46.4 (25.1, 21.2), 18.7 | 287.5 / 287.49 | 12.1 |

One run per cell (the M-run does the three-run series). The sha1 of results/e_4e10.out is
43c84e49678df6d9010028b207c6a1b57f1a89d3 (`~/e_4e10.sha1` on aac6; a cmp over NFS takes 6 min, the sha1 of the local
output 1 min). Each step down in cap saves 17–52 GB of device and costs 6–9 s of phases at 4 × 10¹⁰.

#### The one-node ceiling at three primes

**Modelled** (the C layout report = mem_model: node = device + 15.6 GB host, against mem_model's budgets 502 / 480 GB):
2³⁰ 1.36 / 1.30 × 10¹¹; 3·2²⁹ 1.30 / 1.24; 2³¹ 1.23 / 1.16; 3·2³⁰ 1.06 / 1.02. (Four primes, the same model: 1.18 × 10¹¹
at 2³¹.)

**Measured, real runs** (no outfile: the run's own VERIFY is the check; `ECALC_VERBOSE=2 MEM_REPORT_DEVS=1`, `/usr/bin/time -v`):

| cap | D | result | total (phases: bs, dm; init) | wall | device peak measured / C | host HWM | device + host |
|---|---|---|---|---|---|---|---|
| 3·2³⁰ (fastest) | 1.00 × 10¹¹ | **VERIFY OK** (21051, s24-16) | 206.9 s (161.4: 76.3, 85.0; 45.5) | 3:29.7 | 465.5 / 465.52 | 14.1 | 479.6 |
| 3·2³⁰ | **1.14 × 10¹¹** | **VERIFY OK** (21059, s24-30) | 228.7 s (194.2: 104.2, 89.9; 34.5) | 3:51.5 | 509.0 / 508.96 | 14.8 | 523.8 |
| 2³⁰ (leanest) | **1.42 × 10¹¹** | **VERIFY OK** (21056, s24-30) | 677.2 s (642.8: 242.4, 400.2; 34.4) | 11:20.3 | 501.4 / 501.33 | 16.4 | 517.8 |
| 2³⁰ | 1.46 × 10¹¹ | **killed** (signal 9, the kernel's OOM killer) in bs right after the seeds, 1:24 in (21064, s24-30) | — | — | 513.8 at init / 513.75 | 15.8 (RSS at the kill) | ≈ 529.6 |
| 2³⁰ | **1.44 × 10¹¹** | **VERIFY OK** (21072, s24-26) | 1085.8 s (1038.7: 376.3, 662.2; 47.1) | 18:09.4 | 507.6 / 507.54 | 16.5 | 524.1 |
| 2³¹ | **1.30 × 10¹¹** | **VERIFY OK** (21069, s24-30) | 353.1 s (314.5: 148.6, 165.8; 38.6) | 5:56.2 | 507.1 / 507.06 | 15.7 | 522.8 |
| 3·2³⁰ | 1.16 × 10¹¹ | **killed** (signal 9, OOM killer) 1:04 in, in bs (21069, s24-30) | — | — | 515.2 at init / 515.17 | 13.5 (at the kill) | ≈ 528.7 |

(For comparison, four primes at 1 × 10¹¹ and 2³¹, M11: 263 s, 431.2 GB.)

**Allocation probes** (`ECALC_INIT_ONLY=1`: the plane pools and the arena mapped, then stop; host at init 11.8–12.7 GB):

| cap | mapped | refused (hipMalloc out of memory at the plane pool) |
|---|---|---|
| 2³⁰ | 1.30, 1.34, 1.38, 1.42, 1.46 × 10¹¹ (464.1 … 513.8 GB) | 1.48 (520.0 modelled), 1.50 |
| 3·2²⁹ | 1.40 (512.3) | 1.42 (518.5), 1.44 |
| 2³¹ | 1.20, 1.24, 1.28, 1.32 (476.1 … 513.3) | 1.34 (519.5), 1.36 |
| 3·2³⁰ | 1.02, 1.06, 1.10, 1.14, 1.16 (471.7 … 515.2) | 1.18 (521.4) |

What binds, measured: (i) the allocation at init fails between 515.2 and 518.5 GB of device (128.8–129.6 GB per APU;
the driver reports 137.4 per APU, and at 513.8 GB in use it reported 132.9 per APU used); (ii) a real run needs the host
beside it: 523.8 GB device + host HWM ran, ≈ 529.6 was OOM-killed in bs (the host's seed phase grows RSS from 12.2 to
~16 GB). So on one MI300A node the ceiling is **device + host HWM ≲ 524–529 GB**, not mem_model's 502: the model's
ceiling is conservative by +0.06 × 10¹¹ (2³⁰) and +0.08 × 10¹¹ (3·2³⁰). Modelled against measured edge:

| cap | modelled ceiling at 502 GB | measured: runs | measured: fails | modelled at a 524 GB node budget (host 15.6) |
|---|---|---|---|---|
| 2³⁰ | 1.36 × 10¹¹ | 1.44 | 1.46 | 1.44 |
| 3·2³⁰ | 1.06 × 10¹¹ | 1.14 | 1.16 | 1.13 |
| 2³¹ | 1.23 × 10¹¹ | 1.30 | (1.34: allocation refused) | 1.30 |
| 3·2²⁹ | 1.30 × 10¹¹ | (not run; 1.40 maps) | (1.42: allocation refused) | 1.38 |

#### Regression

| what | job | result |
|---|---|---|
| `./mnaccept.sh 21066 --only unit` (2487f75) | 21066, s24-30 | **8 passed, 0 failed** (1165 s): t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid 2 procs |
| `./mnaccept.sh 21069 --only e9` (2487f75) | 21069, s24-30 | **2 passed, 0 failed**: 10⁹ size 1 LIMB_BASE=10 identical (16.67 s), LIMB_BASE=2 identical (25.33 s) |

The `unit` step alone takes 19.4 min, so `unit,e9` does not fit one 20-minute job; it ran as two.

#### Open issues

* The ceiling is set by the node's memory as a whole: device + host HWM ≲ 524 GB ran and ≈ 529 GB was OOM-killed.
  The per-APU device allocation stops between 515.2 and 518.5 GB. Both figures are on s24-30 and s24-16, measured
  with at most ~12 GB of other use at init. mem_model's 502 GB is safe but under-states the edge by 0.06–0.08 × 10¹¹.
* `ECALC_PLANE_CAP=fit` uses the model's node total against `ECALC_NODE_GB` (default 480). It is tested on the login
  node (it picks 2³¹ at 1.1 × 10¹¹) but has not had a real run. At 524 it would pick the measured edges above.
* The size rule's 5 × 10¹⁰ threshold for the 3·2ᵏ planes dates from four primes. At three primes 3·2³⁰ runs to 1.14 × 10¹¹
  measured and is the fastest cap measured at 4 × 10¹⁰ (65.1 s against 67.9–70.7 at 2³¹). Whether the default should
  become "the largest cap that fits" (`fit`) is the user's decision; the M-run table decides. It is not changed here.
* At the leanest cap the large sizes are slow: 1.42 × 10¹¹ at 2³⁰ takes 677 s, and its dm phase alone is 400 s (the
  reciprocal's products are grids of many 2³⁰ pieces). 1.44 × 10¹¹ took 1085.8 s on s24-26, 1.6 × the 1.42 run on
  s24-30 for +1.4 % digits (reciprocal 308.6 against 184.7 s; the same 1.5 M major page faults). Node against size is
  not separated: one run each. The modelled 576-node wall should take its per-product times from
  that cap's own measurement, not from 3·2³⁰'s.

#### RESUME

* **Gate met.** Code: d1bf03b (the formula, the layout report, the switch), 2487f75 (the init probe); results commits
  after it. aac6 `~/ntt-P13b` is built at 2487f75, the code head.
* Batches: b1 21046, b2 21051, b3 21056, b4 21059, b5 21064, b6 21066 (mnaccept unit), b7 21069 (mnaccept e9 + edges),
  b8 21072 (2³⁰ at 1.44 × 10¹¹). All of them are done and cancelled; no job is held or queued.
* Summary of the edges at three primes, one node, measured: 2³⁰ 1.44 × 10¹¹ (1.46 OOM-killed); 2³¹ 1.30 × 10¹¹;
  3·2³⁰ 1.14 × 10¹¹ (1.16 OOM-killed). The model at 502 GB gives 1.36 / 1.23 / 1.06; at 524 GB it gives 1.44 / 1.30 / 1.13.

## R16 — the region-pool sizing abort of ecalc at 10¹¹ digits on 4 nodes (2.5 × 10¹⁰ per node)

(former file: R16.md)

Agent R, Phase 16. Branch `p16-R` from `main` e7dfcae. Files: `ecalc/binsplit.c` (`region_need`, the new `level_q_lower`,
`bs_pool_rule`), `ecalc/mem_model.py` (the port), `ecalc/README.md` (the `BS_POOL_RULE` row), this report. Times Eastern.

#### 1. Diagnosis

**Symptom** (B16, job 12178, 2026-10-04 03:30 EDT, `~/p16/B/n4/e11.log`): `mnrun.sh 4 env COMM_TRANSPORT=shmem … ./ecalc
100000000000` on 4 aac7 nodes aborts on rank 0 at the start of bs level 23 (the top level of the batch tier; B's text
says "level 22" because rank 0's `level 22` line is the last one printed before the abort): `bs: region pool 0 of parity 1
would grow inside bs, 10.87 -> 16.15 GB` (rc 6, `RNS_POOL_GROW=0`). Memory was not the limit (258 of 502 GB in use).

**Cause** (`binsplit.c region_need`, the sizing pass of Phase 9 C4, made an abort by Phase 12 R): the pass lays every level
of the local tree out with the seed bound `per` = (S × ⌈log₂(N + 2)⌉ + 128) / 59 + 2 limbs per span — N the **whole
series'** term count — and takes a level as an mdev-tier level (its outputs in device numbers, not in the region pools)
when `2 × ⌊0.9 × bound⌋ + 1 > 2^BS_MDEV_LOGL` (2³⁰ limbs). The 0.9 is the measured ratio of the real sizes to the bound
at size 1 (0.90–0.945). At size > 1 rank 0 holds the terms [1, N/g): its products are ⌈log₂(N/g)⌉-ish bits per term
against the bound's ⌈log₂ N⌉, so the real sizes are **0.88 of the bound** (10¹¹ on 4: level 23's inputs are 5.31 × 10⁸ limbs,
the bound 2²² × 144 + 22 = 6.04 × 10⁸). The rule said mdev (2 × 0.9 × 6.04e8 + 1 = 1.087e9 > 2³⁰ = 1.074e9); the run's own
decision, from the real sizes (2 × 5.31e8 + 1 = 1.062e9 ≤ 2³⁰), took the level through the batch tier, whose outputs (P and
Q of the single pair, 2.02 × 10⁹ limbs = 16.15 GB, both in region 0 of parity 1) did not fit the pool sized without that
level (need 9.66 GB, + 1/8 = 10.87 GB — the figures in the message). The comment on the rule foresaw the miss ("a miss
costs one pool growth, not a failure") from before Phase 12 R turned the growth into an abort.

Why the other sizes pass: the boundary level's real size has to fall between 0.9 × bound and the limit. 10¹¹ on one node
(ratio 0.92, the level counted), 10¹⁰ on 4 (the boundary is far from the limit), 9.16 × 10¹⁰ on 12 and on 576: the model
shows the old rule and the new one count the same levels (§2). The plane pools, `RNS_POOL1_GB`, the P24 form, the grid
pieces and the mn plan are not involved: the pool that grew is a **bs region** (the arena half of parity 1 on device 0).

Reproduced on the login node (`BS_LAYOUT_ONLY=25000000000:4`, measured, §2) and in `mem_model.py` (region 0's need 9.66 GB
with the old rule = the message's 10.87 GB after + 1/8).

#### 2. The fix — `BS_POOL_RULE` (default 1; 0 = the old rule)

`region_need` now predicts the tier from a **lower bound** on the level's largest Q: the last full node's product over its
own terms [t0, t1), (lgamma(t1) − lgamma(t0)) / ln 10 / 18 limbs (the measure `bs_seed_terms_for` uses), t0 = a0 + (nfull − 1)
× 2^l × S for this node's a0, S. A level is left out only when even that bound is over 2^BS_MDEV_LOGL — then the run is
certain to take it through the mdev tier. The bound is 0.97–0.99 of the real size, so at most one more level than before is
counted, and only where the old rule would have aborted or came within ≈ 3 % of it. Sizing only: digits unchanged. The
pool's +1/8 and the bound `per` for the counted levels are as before. `mem_model.py` (`level_q_lower`, `region_need(…, a0,
rule)`, `BS_POOL_RULE` from the environment) follows; `--check-c` exact (§3).

**The layout before / after** (`BS_LAYOUT_ONLY`, aac7 login node, `ECALC_NP=auto RNS_DIST_CACHE_FIT=1 COMM_TRANSPORT=shmem
COMM_SHMEM_ROUND_MB=1024`, the code's other defaults; **measured on the login node, bytes per node**; `~/p16/R/{before,after}.layout.*.log`):

| point | bs regions before → after | arena before → after | node at 2³¹ before → after |
|---|---|---|---|
| 2.5 × 10¹⁰ : 4 (10¹¹ on 4) | 71.65 → **93.39 GB** (region 0's need 9.66 → 19.33 GB, pool 10.87 → 21.75 GB) | 154.62 → **161.06 GB** (+6.44: the dm need covers most of it) | 279.91 → **286.35 GB** |
| 10¹⁰ : 4 (4 × 10¹⁰ on 4) | 44.57 → 45.00 GB (+0.44; the extra level's share of region 0 is 2 % above the lower level's) | 81.60 → 81.60 | unchanged |
| 10¹¹ : 1 | 261.24 → 261.24 | 283.47 → 283.47 | identical line |
| 9.16 × 10¹⁰ : 12 (Phase E, 1.0992 × 10¹²) | 223.23 → 223.23 | 292.06 → 292.06 | identical line |
| 9.16 × 10¹⁰ : 576 (the target, 5.276 × 10¹³) | 203.40 → 203.40 | 300.65 → 300.65 | **identical** (the `before` run had no `ECALC_NP=auto` — plan refused, layout printed; the regions and the arena do not depend on the prime count) |

So the correction changes nothing at the Phase E headline and at the target (the node totals of RESULTS §94 stand: delta
0, modelled), +6.44 GB per node at 2.5 × 10¹⁰ on 4 (286 GB of 501 GiB), +0 at 10¹⁰ on 4.

`MN_PLAN_ONLY=100000000000:4` (`~/p16/R/before.plan.1e11_4.log`): plan check OK, the largest piece 8.33 × 10⁹ limbs,
pool 8704 MiB (with `COMM_SHMEM_ROUND_MB=1024`; B's run without it: 21504) — the mn plan sizes the plane pools and the SHMEM
pool, not the bs regions; it was never the under-sized pool.

Model-only cross-check (`mem_model.region_need`, rule 0 / rule 1, region needs in GB): 2.5e10:4 (9.66, 9.66, 6.34, 6.17) /
(19.33, 9.66, 6.34, 6.17); 1e10:4 (9.80, 4.9, 2.6, 2.5) / (9.99, …); 1e11:1, 4e10:1, 1e9:1, 9.16e10:12, 9.16e10:576
identical under both rules.

#### 3. Tests

| step | command | size | result |
|---|---|---|---|
| layout, before | `~/p16/R/layout.sh before` (base b298542 = main's ancestor, no code change) | the five points | §2 table (measured, login node) |
| layout, after | `~/p16/R/layout.sh after` (61e3ce7) | the five points | §2 table |
| `mem_model.py --check-c` | on each `after.layout.*.log` with the launch-line environment | 5 points | **bs regions / node exact (+0.0000 %)**, planes exact at every cap |
| 10¹¹ on 4 nodes, **no GROW** (the fix) | job 12189 (aac7-n11, s2b0n0, s2b1n0, s3b0n0; ROCm 7.0.3, Cray OpenSHMEMX), `~/p16/R/job.sh`: `mnrun.sh 4 env COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_CHECKPOINT=1 ECALC_VERBOSE=2 ECALC_LOG_CLOCKS=1 ./ecalc 100000000000 ~/p16/R/n4/e11.out`, 11:36 EDT | 10¹¹ | **rc 0, all 4 nodes VERIFY OK, `digcmp.sh` against `results/e_1e11.out`: identical**; no `region pool` / `would grow` line; rank 0's level 23 ran in the batch tier (max_nl 5.22 × 10⁸) in its init-sized pool; `total 517.67 s` (bs 148.9, dm 176.7, dc 136.4, init 55.5; measured — B's GROW retry took 323 s: one node's level 24 mdev took 35.05 s against 1.5–1.7 s on the others, and bs 148.9 vs B's; not this fix's concern, the sizing is at init) |
| 4 × 10¹⁰ on 4 nodes | same job, `./ecalc 40000000000 ~/p16/R/n4/e10.out`, then `~/p16/R/pcmp.sh` (B's: one `unpack_digits` pass against the 10¹¹ reference's first D + 2 bytes and the length D + 3) | 4 × 10¹⁰ | **rc 0, identical** (first 40000000002 bytes = the reference, length 40000000003); `total 195.82 s` (bs 23.8, dm 77.0, dc 75.2, init 19.8; measured) |
| `mnaccept.sh 12189 --only unit,e9,mn` | same job (a 4-node allocation), `~/ntt-R/ecalc/results/mnaccept/12189/` | — | **mn e9 size 2 and size 4 PASS** (identical, VERIFY OK; 162.0 / 185.9 s). The rest did not run: every `unit` step and `e9` size 1 (both bases) failed with `execve(): 192: No such file` — `MNRUN_CPUS_PER_TASK=auto` on a multi-node job (`scontrol` prints one CPUTot per node: `-c 192\n192`), S16's bug, fixed in `p16-S` bc17d32 (`head -1`), now in this branch (`ecalc/mnaccept.sh`, the one hunk); `mn e8` sizes 2–4 reported DIFFERS with every node VERIFY OK because the clone's `ref/` holds only the 10⁹ symlink and `mnaccept.sh` then takes the clone's `ref/` for every size (`e_100000000.txt` absent) — a reference artifact, not a digit difference. A rerun on a 1-node job was not taken: B's 10-node and D's E jobs were pending for the nodes (13:05 EDT) |
| `mem_model.py --p15` | `BS_POOL_RULE=0` vs `=1` (login node here) | the target and the ceilings | byte-identical output: the target's node figures and the 576-node ceilings unchanged (modelled) |

#### Open issues

- The sizing-code gate `mnaccept --only unit,e9,mn` ran only its `mn e9` steps (above): the unit tests and e9 size 1 need a rerun on a 1-node job with the `head -1` fix (`./mnaccept.sh <J> --only unit,e9`, and `ref/e_100000000.txt` linked into the clone for `mn e8`). The change is in the sizing pass only (`region_need`), exercised by every run above and by `--check-c`.
- 10¹¹ on 4 nodes took 517.7 s here against B's 323 s with `RNS_POOL_GROW=1` on other nodes (one node's level 24 mdev 35 s, bs 148.9 s): a node / NFS effect to look at by the Phase E owner, not the sizing.
- `BS_POOL_RULE` is a sizing correction with the old rule kept behind `=0`; the user decides whether the default stays 1 (recommended: without it 10¹¹ on 4 nodes aborts, and the Phase E and target layouts are unchanged).

#### RESUME
- 2026-10-04 13:10 EDT: **done.** Job 12189 ran e11 (identical), e10 (identical), mnaccept (mn e9 sizes 2, 4 PASS; unit / e9-size-1 not run: S's `-c` bug, fix cherry-picked; mn e8 reference artifact) and was cancelled by the chain (`CHAIN_DONE`; no job of R's in the queue). Part files deleted. Open: the unit / e9 rerun on a 1-node job when nodes free up (ROCm 7.2.4 is the aac7 build default for new runs from now on: `module load rocm/7.2.4`, rebuild `~/ntt-R`).

## AS15 — Phase 15 Batch 2, row 2: arena sizing for `BS_SEED_FILL=128` (the division's VMM remaps)

(former file: AS15.md)

Agent AS, branch `p15-AS` from `main` f184d51 (B1). aac6 clone `~/ntt-AS15`, logs `~/AS15/<batch>/` (copied to `archive:results-raw/AS15/`).
Times Eastern (aac6 logs are Central: +1 h). Numbers are labelled **measured** (a run's log), **replayed** (the pool port below,
driven by a measured trace: exact, see §2) or **modelled**.

#### 1. Feasibility note (written first, PLAN §36.5)

- **Approach.** Record the block pool's every event in one run (`DB_POOL_TRACE=1`, print only), replay the pool offline with an
  exact port of `dbig.c`'s placement (`tests/as_pool_replay.py`), and try arena layouts on the replay before any node time. The
  request sequence per APU does not depend on addresses, so the replay of another layout is exact up to the point where the code
  itself would differ (it would not: only the pool's placement changes).
- **Risks.** A layout that removes the remaps at one size and not another; memory growth at 1.3 × 10¹¹ and at the target share
  (the arena is inside the node budget, `BS_LAYOUT_ONLY` shows it); digits cannot depend on addresses (the pool's placement only).
- **Test plan.** As the prompt: `t_dbig 0`, mnaccept `unit,e9,mn,stress`, 10¹¹ identical through `digcmp.sh`, paired timing at
  10¹¹ (≥ 3 + 3 without the digit file, ≥ 2 + 2 with it), one pair at 4 × 10¹⁰ and 1.3 × 10¹¹; `mem_model.py --check-c` exact.
- **Expected gain.** The remap time on B1 (measured below: 15 remaps, 9.5 s summed at 10¹¹, including 3 growths of 2 chunks with
  up to 3.8 s each) and whatever the placement costs beyond it (RL: `dm` loses more than the remaps' time).

#### 2. What the trace shows (10¹¹, B1 defaults, job 21610, s24-16)

**Measured** (run `t11`, `DB_POOL_VERBOSE=1 DB_POOL_TRACE=1`, without the digit file): total 207.41 s, bs 82.40, recip 38.58, `dm`
105.50 s; **15 remaps, 9.49 s summed** (APUs 1–3 each: 11.11 GB and 4.17 GB in the reciprocal, 22.22 GB for `t`, a 4.07 GB request
that found no wholly free chunk and **grew the arena by 2 chunks** (0.22 / 1.13 / 3.80 s), 22.22 GB for `xq`). APU 0 (arena
83.75 GB = 39 chunks, its bs region is larger at the fill) has none. APUs 1–3: 64.42 GB = 30 chunks, the dm need.

**Replay** (`python3 tests/as_pool_replay.py t11.trace`): 116 blocks, **0 mismatches** against the addresses the code chose, the
same 15 remaps and the same growth — the port is exact.

**Why** (`--dump 1`, APU 1):
1. The arena's bytes (63.82 GB) are not a whole number of 2 GiB chunks; the last chunk's remainder (0.60 GB) is free pool space
   *after* the tail's end, and it merges with the extent in front of it. The pool's rule "a spill into the tail goes to the tail's
   very end" checks for an extent ending at the tail's end, finds none, and carves the bs top level's second output (P, 11.11 GB)
   from the **front** of that extent, in the middle of the tail (38.36–49.47). The reciprocal then finds its 11.11 GB (r2) and
   4.17 GB temporaries split: 2 remaps per APU.
2. With the arena in whole chunks (no remainder), P goes to the tail's back (53.31–64.42) and the reciprocal fits; but at the
   division `t` (22.22 GB) meets Q, S's replacement and P's block with 31.09 GB free in two extents (11.11 + 19.98): the remap,
   and `xq` after it. The top level's second output does not fit outside the tail because the freed P₁, P₂ leave holes of
   8.07 GB (the top node's children are unequal at every span: 3.04 and 8.07 GB per APU) — dm_layout's v3 counts the bytes, not
   the holes.

| layout (APUs 1–3; APU 0 unchanged) | arena per APU | remaps (replayed) | growth |
|---|---|---|---|
| B1 | 64.42 GB (63.82 asked) | 15 (90 chunks moved) | 2 chunks × 3 |
| whole chunks (no memory: the chunks are mapped anyway) | 64.42 GB | 9 (69 chunks) | 1 chunk × 3 |
| whole chunks + 1 chunk | 66.57 GB | 6 (66 chunks) | 0 |
| **whole chunks + 2 chunks** | **68.72 GB** | **0** | 0 |
| not rounded, + 8 GB | 71.82 GB | 0 | 0 |

#### 3. More sizes (job 21615, s24-30), and the rule

Traces on the B1 defaults at 2, 4, 6, 8, 9 × 10¹⁰, 10¹¹ (fill 128 and 0), 1.16, 1.2, 1.3 × 10¹¹ (jobs 21610, 21615; all VERIFY
OK; `archive:results-raw/AS15/trace/`). **Every replay is exact** (0 mismatches, 52–164 blocks each). The same remaps appear on the node
(measured, from the runs' own `DB_POOL_VERBOSE` lines) and in the replay:

| size | remaps on B1, measured (in the division; growth events) | `dm` (s, one run, measured) | replayed B1 | replayed, whole chunks only | replayed, `BS_ARENA_ROOM=0.16` | arena change, `0.16` (GB per node, mapped) |
|---|---|---|---|---|---|---|
| 2 × 10¹⁰ | 0 | 8.32 | 0 | 0 | 0 | +4.3 |
| 4 × 10¹⁰ | 2 (2, 0.44 s) | 21.62 | 2 | 0 | 0 | 0 |
| 6 × 10¹⁰ | 4 (3, 0.91 s; 1 growth) | 41.83 | 4 | 0 | 0 | +8.6 |
| 8 × 10¹⁰ | 3 (3, 1.14 s; 1 growth) | 65.28 | 3 | 0 | 0 | +8.6 |
| 9 × 10¹⁰ | 9 (9, 3.35 s; 3 growths) | 83.29 | 9 | 6 | 0 | +12.9 |
| 10¹¹, fill 0 | 3 (3, 1.43 s) | 91.15 | 3 | 3 | 0 | +12.9 |
| **10¹¹ (B1)** | **15 (9 in the division, 8.40 s; 3 growths)** | **105.50** | 15 | 9 (growth) | **0** | **+12.9** |
| 1.16 × 10¹¹ | 4 (4, 2.33 s) | 127.12 | 4 | 2 | 0 | +8.6 |
| 1.2 × 10¹¹ | 4 (4, 2.17 s) | 129.91 | 4 | 2 | 0 | +8.6 |
| 1.3 × 10¹¹ | 4 (4, 3.01 s) | 144.52 | 4 | 4 | 0 | +17.2 |

(The 10¹¹ fill-0 run's `dm` is 14.4 s shorter than the fill-128 run's, one run each on the same node: the cost the task names.)

**The mechanism at the division (1.3 × 10¹¹, `--dump`):** `t` (28.89 GB) lands in the tail; the window `Aw` (14.44 GB) is a
"small" block that finds no room outside the tail and spills to the tail's back; `xq` (28.89 GB) then meets two extents of
27.34 and 18.57 GB and remaps — 1.55 GB short. The placement is greedy and depends on where earlier blocks landed, so the extra
room that removes the remaps is **not monotone** in the room: with a 2 MiB chunk (the room exact to the byte) the 10¹¹ trace
needs 2.84 GB per APU and the 1.3 × 10¹¹ trace 19.7 GB; with the real 2 GiB chunks both need 2 chunks. Neither the tail's size
(−2…+12 GB) nor the "large" threshold (0.3–1.0 of the hole) removes them at a fixed arena (replayed). So the rule is bytes, and
it is chosen on the replays:

**room = f × the hole (the division's largest block, per APU), in the dm need; every arena in whole chunks.** Sweep of f over
all ten traces (replayed): f = 0.0625 / 0.1 / 0.125 leave 17 / 13 / 2 remaps; **every f in [0.13, 0.23] leaves 0**; 0.25 leaves 4
(1.3 × 10¹¹). **f = 0.16** is inside that window and costs the least memory of it (the same as 0.13–0.16; 0.175 and above add a
chunk at 4 × 10¹⁰ and 1.3 × 10¹¹).

#### 4. The switch: `BS_ARENA_ROOM=<f>` (off by default)

- `ecalc/binsplit.c`: `bs_arena_room()` (the variable), `as_arena()` (an arena's bytes in whole `DB_POOL_VMM_CHUNK_GB` chunks),
  `dm_layout` gains `room = f × hole` in `need_dev` (struct field `room`), and the three places that assemble the arena
  (`binsplit_pregrow`, `layout_arena` for the budget / plan check, `binsplit_layout_only`) take `as_arena(base + extra)`. The
  `layout:` line prints `room` and `chunk`; the `bs: dm layout:` line says so. Needs the VMM pool (`DB_POOL_VMM=1`, the default);
  with it off the switch does nothing. Off (0): byte-identical layout to B1 (the check below).
- `ecalc/dbig.c` (named, outside my own files by default): `db_pool_vmm_chunk()` (the chunk as `db_vmm_arena_alloc` takes it, now
  shared), and **`DB_POOL_TRACE=1`** (print only: the `dbtrace:` lines). `ecalc/dbig.h`: the prototype.
- `ecalc/mem_model.py`: `dm_layout(..., room=)`, `arena_of(..., chunk=)`, `VMM_CHUNK`, `mem_per_node(opts arena_room=)` (no
  `VMM_DM_GROW_FILL` with it: the replays show no growth), `--check-c FILE 31 1024 128 <f>`. Separate from NP's prime edits (it
  touches `dm_layout`, `arena_of`, `mem_per_node`'s arena lines and `c_layout_check` only).
- `ecalc/README.md`: rows `BS_ARENA_ROOM`, `DB_POOL_TRACE`.
- `tests/as_pool_replay.py` (the replay: `--arena-add`, `--round-chunks`, `--tail-add`, `--thresh`, `--chunk-mb`, `--dump`),
  `tests/as_sweep.sh` (the node runs), `tests/as_plan_*.txt`.

##### 4.1 The layout and the model (aac6 login node, 22dc725)

`BS_ARENA_ROOM=0|0.16 BS_LAYOUT_ONLY=2e10,4e10,6e10,8e10,9e10,1e11,1.16e11,1.2e11,1.3e11 ./ecalc 1e6 /dev/null` and
`BS_ARENA_ROOM=0|0.16 BS_LAYOUT_ONLY=7.38e10:576 ECALC_NP=4 ./ecalc 42500000000000 /dev/null`, then
`python3 mem_model.py --check-c <file> 31 1024 128 <f>`: **exact at f = 0 and at f = 0.16 (10 points each, 0 terms off)**.

| size | arena, node (GB, the layout line) off → on | mapped chunks per APU off → on (modelled from the port) | mapped arena change | node total (the `planes:` line, cap 2³¹) off → on | model node peak off → on |
|---|---|---|---|---|---|
| 2 × 10¹⁰ | 96.06 → 103.08 | 20,10,8,8 → 20,10,9,9 | +4.3 | 215.4 → 222.4 | — |
| 4 × 10¹⁰ | 139.99 → 146.03 | 20,20,14,14 → same | 0 | 259.3 → 265.3 | 275.5 → 281.6 |
| 10¹¹ | 273.61 → 289.91 | 39,30,30,30 → 39,32,32,32 | +12.9 | 392.9 → 409.2 | 410.4 → 426.7 |
| 1.3 × 10¹¹ | 331.87 → 352.19 | 39,39,39,39 → 41,41,41,41 | +17.2 | 451.2 → 471.5 | 469.3 → 489.6 |
| target share 7.38 × 10¹⁰:576 (ECALC_NP=4) | 244.78 → 257.70 | 29 ×4 → 30 ×4 | +8.6 | 387.3 → 400.2 | 416.1 → 429.0 |

The layout line's "off" arena is the request, not what is mapped (the VMM arena maps whole chunks either way), so the mapped change
is the physical one. **At 10¹¹ the +12.9 GB are exactly the 2 chunks × 3 APUs that B1 already maps inside the division (the
growth events)**: the pool reaches the same size, at init instead of in `dm`. The model's peak rises by more than that because its
"off" arm counts the unrounded request; the measured peaks are in §5. The model's one-node ceiling (480 GB) drops from 1.34 to
1.24 × 10¹¹ with f = 0.16 (modelled; the model's "off" peak at 1.3 × 10¹¹, 469 GB, is itself 31 GB above the measured 438.7 of
RESULTS 85, so this is pessimistic). All sizes stay within 480 GB in the layout's node total.

#### 5. Node runs

##### 5.0 A bug found by the first series (job 21619, s24-26, 1b..ec4bbc6; stopped)

The first runs with the switch on mapped **33** chunks on APUs 1–3 and 40 on APU 0 (the layout said 32 and 39) and still remapped 6
times in the division (2.84–2.97 s), where the replay of that arena predicted none. Cause: `binsplit_pregrow` rounded the arena to
whole chunks from the *unrounded* `cap`, and `arena_get` then rounds `cap` and `extra` to 2 MiB — the arena ended a few MiB into a
33rd chunk, whose remainder lay after the tail's end again (the defect of §2 item 1). Fixed in 1667150 (the extra from the base
`arena_get` lays out; a warning if an arena under the switch is not whole chunks); the series was stopped by PID (its trap
cancelled the job) and restarted. Its runs (measured, `archive:results-raw/AS15/timing_bug/`): 4 × 10¹⁰ off / on `dm` 21.33 / 21.19 s
(2 / 0 remaps); 10¹¹ off `dm` 104.22 s (15 remaps, 10.01 s, 3 growths); on (the buggy layout) 91.14 / 91.74 s.

**Check run after the fix** (job 21626, s24-26, 1667150, `c1`: 10¹¹, `BS_ARENA_ROOM=0.16 DB_POOL_TRACE=1`, measured): arenas
39 / 32 / 32 / 32 chunks as the layout says, **0 remaps, 0 growths**, `dm` 88.48 s, total 177.73 s, VERIFY OK — the replay's
prediction.

##### 5.1 The paired series (job 21626 s24-26, 1667150; job 21634 s24-26 for its tail; all measured, VERIFY OK every run)

Interleaved on one node; `nw` = without the digit file, `w` = with it (packed, unpacked and hashed on the node against the
reference's sha1: **identical** in all four). Elapsed = launch to exit (the sweep's clock). Logs `archive:results-raw/AS15/timing/`.

| run | switch | total (s) | elapsed (s) | `dm` (s) | recip (s) | remaps (count; s summed; growths) |
|---|---|---|---|---|---|---|
| c1 (traced) | on | 177.73 | 180 | 88.48 | 36.58 | 0 |
| n1a / n1b / n1c | on | 180.59 / 179.87 / 178.56 | 183 / 182 / 181 | 88.62 / 88.32 / 88.51 | 36.66 / 36.57 / 36.57 | 0 / 0 / 0 |
| n0a / n0b / n0c | off | 195.82 / 189.22 / 190.60 | 198 / 192 / 193 | 103.17 / 99.74 / 99.69 | 37.63 / 37.58 / 37.73 | 15; 9.36 / 5.83 / 5.55; 3 each |
| w1a / w1b (digits written) | on | 180.06 / 179.16 | 183 / 181 | 89.19 / 89.52 | 36.83 / — | 0 / 0 |
| w0a / w0b (digits written) | off | 194.24 / 195.70 | 197 / 198 | 104.01 / 103.78 | 37.91 / 37.60 | 15; 9.20 / 9.43; 3 each |
| q4off / q4on (4 × 10¹⁰) | off / on | 57.14 / 56.98 | 59 / 59 | 21.45 / 20.87 | 9.33 / 9.30 | 2; 0.44 / 0 |
| h13off (1.3 × 10¹¹) | off | 268.73 | 271 | 139.51 | 54.83 | 4; 2.45 |
| h13on (1.3 × 10¹¹) | on | — | 21 | — | — | **refused at init: budget check rc 8, node peak 480.8 GB > 480** (fixed in 3fa6f30, §5.2) |

**At 10¹¹ (means, measured):** without the digit file `dm` **88.48 vs 100.87 s (−12.4 s)**, total **179.67 vs 191.88 s (−12.2 s,
−6.4 %)**, wall 182.0 vs 194.3 s (−12.3 s); with it `dm` 89.36 vs 103.90 s (−14.5 s), total **179.61 vs 194.97 s (−15.4 s)**, wall
182.0 vs 197.5 s (−15.5 s). The remaps went from 15 (5.6–9.4 s summed, 3 growths) to **0**. The gain exceeds the remap seconds
(the serialized events stall all four APUs, and the growth's `hipMemCreate` + access, up to 3.8 s on one APU): the switch recovers
the whole `dm` cost the fill brought (fill 0: `dm` 91.15 s in job 21610; with the switch 88.5 s). The reciprocal is −1.0 s too (its
two remaps). At 4 × 10¹⁰: `dm` −0.6 s, total −0.2 s (the remaps there were 0.44 s).

**Memory (measured, the runs' `mem` summaries):** 10¹¹ device 393.6 GB at every phase with the switch on and off (largest APU 109.7),
host HWM 27.3–27.6, node 420.9–421.2 GB in both arms: **the node peak is unchanged**. With the switch the 12.9 GB are mapped at init
instead of by the division's growth (3 × 2 chunks), so the peak is the same and reached earlier; MemAvailable's minimum 120.3 against
124.5 GB. At 4 × 10¹⁰: 275.7 vs 275.5 GB (no chunk added).

##### 5.2 The budget at 1.3 × 10¹¹, and the fix (3fa6f30)

With the room, 1.3 × 10¹¹ asks 2 more chunks per APU (+17.2 GB): the layout's node total is 471.5 GB, but the budget check counts
init's measured VmRSS (24.9 GB) where the layout counts 15.6, and refused the run at 480.8 GB. B1 at 1.3 × 10¹¹ remaps only 4 times
(2.45 s) — the room is worth little there and costs the one-node ceiling. So **the room is dropped when the node with it would exceed
`ECALC_NODE_GB`** (planes at the run's cap and prime count + the arena with the room + the host constants + a 10 GB margin for the
measured VmRSS; a `bs: BS_ARENA_ROOM: the room ... is dropped` line), and the arenas stay in whole chunks (no memory). The one-node
ceiling is therefore unchanged by the switch. `binsplit.c as_room_fits`; `mem_model.py as_room_fits` decides alike (the `layout:`
line prints the decision's plane figure, `room_planes`).

Layout (aac6 login, 3fa6f30, `BS_LAYOUT_ONLY` at 2, 4, 6, 8, 9 × 10¹⁰, 1, 1.16, 1.2, 1.25, 1.3 × 10¹¹ and 7.38 × 10¹⁰:576 with
`ECALC_NP=4`): `mem_model.py --check-c` **exact at f = 0 and f = 0.16** (0 terms off); the room kept everywhere except 1.3 × 10¹¹
(1.25 × 10¹¹: kept, arena 343.6 GB). The f = 0 lines are byte-identical to B1's (checked against the B1 build's output).

Note: the per-cap `planes:` line evaluates the decision at each cap (the room can differ between caps; the run's own cap is the one
marked `*`), and the "dropped" line can appear inside that line's output (cosmetic).

##### 5.3 The remaining runs (job 21651 + 21655, s24-26, b0db7ac = 3fa6f30 + a plan; 21:26–22:11 EDT; all measured, VERIFY OK)

| run | switch | total (s) | `dm` (s) | remaps | note |
|---|---|---|---|---|---|
| h13off (1.3 × 10¹¹) | off | 271.36 | 141.51 | 4; 2.52 s | |
| h13on (1.3 × 10¹¹) | on | 267.79 | 140.59 | 4; 2.50 s | the room dropped by the budget rule (the line says so); arenas in whole chunks; runs (no refusal) |
| v3 / v5 / v7 (3, 5, 7 × 10¹⁰) | on | 43.52 / 70.60 / 107.37 | 13.54 / 26.82 / 47.97 | 0 / 0 / 0 | sizes never traced before |
| v105 / v11 / v125 (1.05, 1.1, 1.25 × 10¹¹) | on | 191.89 / 203.97 / 260.31 | 93.51 / 103.35 / 136.13 | 0 / 0 / 0 | the room kept at 1.25 × 10¹¹ |
| wc1 (10¹¹, digits written) | on | 179.84 | 88.63 | 0 | **identical** by sha1 and **`digcmp.sh` against `~/ntt/ecalc/results/e_1e11.out`: identical** |

All seven new traces replay exactly (0 mismatches). The validation sizes were run with the switch on only (the "off" remap
counts there are not measured). At 1.3 × 10¹¹ the pair differs by −0.9 s in `dm` (noise: the same 4 remaps).

##### 5.4 Gates (job 21650, s24-16, b0db7ac, 21:26–21:51 EDT)

`BS_ARENA_ROOM=0.16 ./mnaccept.sh 21650 --stress --only unit,e9,mn,stress` (`tests/as_gates.sh`, `archive:results-raw/AS15/gates/room016.txt`):
**16 PASS, 0 FAIL** — t_ntt 24, t_mul 20, t_bs, **t_dbig 0 (555 checks)**, t_newton 20, t_verify, t_out, t_mn_grid (2 procs); e9 identical in
LIMB_BASE=10 and 2; mn e8 at sizes 2, 3, 4 and e9 at sizes 2, 4 identical; stress 10/10 identical with the forced growth.
With the switch off the layout is B1's byte for byte (§4.1), so the code path off is B1's except the print-only trace hooks.

#### 6. Summary for the integrator

- **Switch `BS_ARENA_ROOM=0.16`** (off by default; README row). 10¹¹, paired on s24-26 (measured): `dm` −12.4 s without the digit
  file (88.5 vs 100.9 s, 3 + 3 runs) and −14.5 s with it (2 + 2); **total 179.7 vs 191.9 s (−6.4 %) and 179.6 vs 195.0 s (−7.9 %);
  walls 182.0 vs 194.3 s and 182.0 vs 197.5 s**; remaps 15 (5.6–9.4 s, 3 growths) → 0; digits identical (sha1 ×2, `digcmp.sh` ×1).
  4 × 10¹⁰ −0.2 s; 1.3 × 10¹¹ unchanged (the room is dropped there).
- **Memory**: node peak at 10¹¹ **unchanged, 421 GB measured in both arms** (the 12.9 GB the division's growth mapped are mapped at
  init instead). Mapped arena change (modelled from the port, checked exact against the C layout): 0 GB at 4 × 10¹⁰, +12.9 at 10¹¹,
  0 at 1.3 × 10¹¹ (room dropped), +8.6 GB per node at the target share (416.1 → 429.0 GB modelled, within 480; NP's prime changes
  are not in this figure). Where B1 grew the pool in the division (6, 8, 9 × 10¹⁰, 10¹¹: measured), the peak is the same.
- **Recommendation**: adopt `BS_ARENA_ROOM=0.16` with `BS_SEED_FILL=128` (the user's decision). The whole-chunk rounding alone is
  free and already removes the reciprocal's remaps; it could be split into its own default if the user wants the no-memory part only.

#### Open issues

- f = 0.16 is chosen on 17 traced sizes (10 replayed off, 7 run on); the placement is greedy and not monotone in the room, so an
  untested size can still remap (cost: the remap, as today). `tests/as_pool_replay.py` answers any size from one traced run.
- `mem_model.py`'s node peak with the room is pessimistic: +16.3 GB at 10¹¹ (426.7 vs 410.4) where the measured peak did not move;
  its one-node ceiling reads 1.24 × 10¹¹ with the room vs 1.34 without (modelled). The real run cannot be refused by the room:
  the budget rule drops it (1.3 × 10¹¹ ran; 1.25 × 10¹¹ ran with it). The model's host/growth terms were not refit.
- The target share (size 576): the room is kept (+8.6 GB per node, modelled) but its time effect there is not measured or modelled
  (the division is sharded; whether it remaps at the target is unknown). A 2-node trace would tell.
- `planes:` line cosmetics (§5.2).
- Outside my own files: `ecalc/dbig.c` / `dbig.h` (`DB_POOL_TRACE` print hooks, `db_pool_vmm_chunk()` shared with the arena), named here.

#### 576-node estimate (standing rule)

Unchanged by this work (the switch is off by default and its effect at the target is not modelled): **4.25 × 10¹³ digits in 256.0 s
(4.27 min) without the write, 285.7 s (4.76 min) with the packed write at 0.6 GB/s per node** (modelled, RESULTS §87 / DOC15);
node memory 416.0 GB of 480 on the defaults, 429.0 GB with `BS_ARENA_ROOM=0.16` (modelled).

#### RESUME

- **Done.** Branch `p15-AS` (last code commit 3fa6f30; plans/report after). Every step in §2–§5 passed; no job pending (jobs 21610,
  21615, 21619, 21626, 21634, 21650, 21651, 21655 all ended and cancelled by their scripts). aac6 clone `~/ntt-AS15` at b0db7ac.
- Open: the adoption decision and the issues above.

## MAP15 — Phase 15 Batch 2, row 5 (N2): the arena's mapping, the seeds and level 1

(former file: MAP15.md)

Agent MAP, branch `p15-MAP` from `int15d` 3e0a0db (= main B1 f184d51 + NP + AS + N3x, all off by default). aac6 clone
`~/ntt-MAP15`, logs `~/MAP15/<batch>/` (copied to `results/MAP15/`). Times Eastern (aac6 logs are Central: +1 h). Numbers are
labelled **measured** (a run's log), **modelled** or **assumed**.

#### 1. Feasibility note (written first, PLAN §36.5; 2026-09-27 22:55 EDT)

**The measured timeline at 10¹¹** (job 21659, s24-30, 2add00f = int15d + `ECALC_INIT_TL=1`, without the digit file; seconds since
`rns_init` began; `results/MAP15/f1/`). Arena chunks are 2 GiB; "p0" = the parity-0 half (the seeds' target, mapped inside
`binsplit_pregrow` by four threads, one per APU), "p1" = the parity-1 half (level 1's outputs; mapped by the four background threads
after `rns_init`, one chunk at a time under the global mapper lock), "rest" = the dm extra past the halves.

| run | planes done | p0 mapped (64 chunks) | seed thread: 2 chunks done → waits for the pools | bg: p1 + rest | seeds end | level 1 starts | level 1 done |
|---|---|---|---|---|---|---|---|
| d0 (int15d defaults) | 6.88 | 7.25 → 19.13 (**0.186 s/chunk**) | 12.22 → 19.14 (**6.92 s idle**) | 19.41 → 33.62 (66 chunks, 0.215 s/chunk) | 35.49 | **35.96** | 40.74 |
| r0 (`BS_ARENA_ROOM=0.16`) | 7.46 | 8.28 → 18.48 (0.159) | 12.18 → 18.48 (6.30) | 18.61 → 34.67 (71, 0.226) | 34.62 | **35.07** | 39.50 |
| t96 (room + `BS_SEED_THREADS=96`) | 6.54 | 7.09 → 16.96 (0.154) | 12.06 → 16.96 (4.90) | 17.10 → 31.28 (71, 0.200) | 36.01 (spans 19.94 s vs 16.63) | 36.46 | 41.22 |
| aft (room + `ECALC_SEED_ORDER=after`: no seeds during p0) | 6.85 | 7.38 → 15.78 (**0.131**) | — (seeds from 17.86, in bs) | 15.90 → 32.19 (71, 0.229) | 37.19 | 37.90 | 42.49 |

(all measured). Per chunk, the background mapper holds its lock 0.15–0.24 s: `hipMemCreate` 0.12–0.16 s, `hipMemSetAccess`
≈ 0.058 s, `hipMemMap` and the zeroing ≈ 0; the four background threads queue 9–12 s each for the lock, and the lock is not fair
(one APU maps 4–7 chunks in a row while another waits). In p0 the four threads call the runtime at once and the call times overlap
(the runtime serializes them: per-APU sums of 20–36 s for an 8–12 s phase).

**What binds.** Level 1 needs p0 and p1 of every APU: **124 chunks** (266 GB) of the 129–135 (the rest, 5–11 chunks, is the dm
extra, first used after bs). The seeds end at 34.6–36.0 s and the p1 mapping at 31.3–34.7 s: both are on the critical path, 1–4 s
apart. The seed thread's own work is ≈ 20 s (buffers 1.4–1.6 + spans 16.6–17.1 + DMA and issue ≈ 2.6), but it starts at ≈ 7 s and
**sits idle 4.9–6.9 s waiting for all four p0 halves**; the mapping runs at 0.13 s/chunk when the CPU is free and 0.16–0.23 s/chunk
while 192 seed threads compute.

**What can overlap** (modelled from the measured parts):
1. **The seeds need only the chunks they write, in order** (region 0's first 8 GiB, then the next …). Mapping p0 in the seeds'
   order and letting each DMA wait for its own chunks removes the 4.9–6.9 s idle wait: the seed thread's work (≈ 20 s from ≈ 7 s)
   ends at **≈ 27 s**.
2. **One mapping stream in need order**: p0 in the seeds' order, then p1 of all APUs, then the rest — started right after the
   planes (`RNS_PLANES_FIRST` kept), without the main thread blocking in `binsplit_pregrow`, several chunks per call and the four
   APUs' calls issued together as in p0 today (0.13–0.16 s/chunk against the background's 0.20–0.23). 124 chunks from ≈ 7 s:
   **≈ 23 s (uncontended rate) … 27 s (0.16 s/chunk)** for level 1's chunks.
3. **The seed threads (b)**: 96 threads make the spans ×1.2 slower in the run (16.6 → 19.9 s, measured) but the mapping ×1.1–1.2
   faster; which wins depends on which of 1 and 2 binds after the change — measured, both ways.
4. **Level 1 waiting for its own chunks** (not the whole p1 half) overlaps level 1's 4.9–5.2 s with the last chunks: up to ≈ −2…−3 s
   more; needs level 1 split into sub-batches in output order (a small edit in the level loop, outside the seed code: named); risk:
   the mapping calls delay level 1's launches (RL: the runtime serializes its VMM calls). Built second, only if 1–2 leave the
   mapping on the critical path.

**Expected gain**: level 1 from 35.1–36.5 s to ≈ 27–30 s: **−5…−8 s at 10¹¹** (modelled), the same order at the target's top node
(its init + seeds have the same structure, V3 §1; assumed). With `BS_ARENA_ROOM=0.16` the same (its extra chunks are in "the rest").

**Risks**: a DMA into a chunk not yet mapped is a device fault (every seed DMA waits for its chunks: the wait is the existing
`db_vmm_arena_wait`, which is a no-op when the chunks are there); the budget check and `mem_report` count the arena by its
registered range (unchanged); the remap path already waits for the whole arena. Digits cannot depend on the mapping order.

**Test plan** (the prompt's): `t_dbig 0`; `mnaccept --stress --only unit,e9,mn,stress` with the switches on; 10¹¹ identical via
`digcmp.sh`; timing at 10¹¹ off/on ≥ 3 + 3 without the digit file and 2 + 2 with it, and one 4 × 10¹⁰ pair; all with and without
`BS_ARENA_ROOM=0.16`.

**Verdict: worth it.** Build 1 + 2 (+ the thread count as a measured option), then decide on 4.

#### 2. The stream (`DB_POOL_VMM_STREAM=<W>`)

##### 2.1 Design (18af9e9, 2880e30, 9dfe39d, 47ccca1)

- `dbig.c`: under the switch `db_vmm_arena_alloc` only reserves the VA and registers the arena (no mapping in
  `binsplit_pregrow`: `rns_init` returns at ≈ 8 s instead of ≈ 18 s). From `db_vmm_bg_release` (after the plane pools:
  `RNS_PLANES_FIRST` kept) W worker threads (1–4, detached) map units of `DB_POOL_VMM_STREAM_BATCH` (2) chunks in need order:
  phase 0 the parity-0 halves by APU (the seeds' region order), phase 1 the parity-1 halves round robin, phase 2 the rest (the
  dm extra) — released at bs's first device top-tier level (`db_vmm_stream_rest`, one call in the level loop) or by any waiter
  that needs chunks past the parity-1 half (donation, `vmm_make_room`, release); `DB_POOL_VMM_STREAM_REST=0` maps it at once.
  At most one worker per APU, so each APU's chunks map in increasing order and `mapped` keeps its meaning for every existing waiter.
  Each unit is zeroed (`hipMemsetAsync` on the worker's stream + sync) before it counts, as the background thread did.
- `binsplit.c` seed code: every seed DMA first waits for its own chunks (`seed_wait_map` → `db_vmm_wait_range`, counted in "waited
  for the regions"); the seeds' own `pool_get(0)` inside `rns_init` does not wait.
- `binsplit.c` outside the seed code (named): `pool_get` for parity 0 outside pregrow waits for the parity-0 half (a no-op without
  the stream: it is mapped inside pregrow), so any other first use of the regions (a checkpoint restart) is safe; the
  `db_vmm_stream_rest()` call at the first device top-tier level.

##### 2.2 First runs (job 21661, s24-26, 9dfe39d, 22:44–22:54 EDT; all measured)

| run | setting | seeds end | level 1's chunks mapped | level 1 starts | level 1 | total | note |
|---|---|---|---|---|---|---|---|
| e9s | 10⁹, W = 4 | — | — | — | — | 12.41 | VERIFY OK |
| r0c | room, stream off | 34.26 | 35.06 (bg) | 34.68 | 4.2 s | 179.07 | VERIFY OK |
| s4r | room, **W = 4** | 58.38 | 44.03 | 58.84 | 4.3 s | 216.91 | VERIFY OK; **one seed DMA launch blocked 32.9 s** behind the four workers; 0.27 s/chunk |
| s1r | room, **W = 1** | **28.33** (no wait for the regions; issue 2.2 s as before) | 34.93 (0.216 s/chunk) | 34.93 | 12.9 s (6.1 s waiting for APU0's p1; batch 4.45 s while the rest mapped) | — | **the node rebooted** during the run (after bs level 4, ≥ 12 s after the last mapping call; Slurm NODE_FAIL, "Node unexpectedly rebooted") |

Findings: (1) four concurrent workers starve the seed thread's kernel launches (the reason for the old mapper lock) and map
slower, 0.27 s/chunk: **W = 1** is the form. (2) With W = 1 the seeds are done 6.6 s before level 1's chunks: the mapping rate
(0.216 s/chunk with 192 seed threads computing; 0.131 with the CPU free, run `aft`) is now the floor, so the seeds can give up
threads to it (b). (3) The rest mapped beside level 1's batch cost ≈ 1.5 s of it: deferred to the device top tier in 47ccca1.

**The node failure** (s24-26, BootTime 22:53:14 EDT; job 21661 NODE_FAIL). The run had passed bs level 4 at ≈ 47 s; the stream had
finished every chunk at 37.6 s and its worker had exited. The project has two earlier NODE_FAILs on unchanged code (job 20629
s24-16 2026-09-17, job 20945 s24-30 2026-09-22; RESULTS.md), so a coincidence is plausible, but it is **not proven**. Rule adopted:
the next series repeats W = 1 first on s24-30; a second failure stops all node work with the switch. s24-26 stays down (needs the
admin).

##### 2.3 The repeat on s24-30, and the second node failure (job 21662, 47ccca1, 22:57–23:03 EDT; measured)

`s1r` again (room, W = 1, the rest now deferred to the device top tier): the seeds ended at 29.23 s (no wait for the regions),
level 1's chunks at 35.90 s, level 1 started 35.90 (11.4 s: 6.2 s waiting for APU0's parity-1 half). The rest (3 chunks on APUs 1–3)
was released at 70.5 s by the first device top-tier level and mapped by 72.7 s while the GPUs ran it. From there the run
degraded: **the mdev tier 39.6 s (≈ 27 s normally), the reciprocal 59.7 s (≈ 38 s)**, and the node rebooted in dm (Slurm
NODE_FAIL "Node unexpectedly rebooted", 23:02:51 EDT); the five runs after it failed at launch (rc 233). **No further node work
with the switch.**

| | W = 4 (s4r) | W = 1 (s1r, job 21661) | W = 1 (s1r, job 21662) |
|---|---|---|---|
| mapping while GPU kernels ran (beyond the seed DMAs) | none: every chunk mapped by 44.0 s, level 1 at 58.8 s | the rest (11 chunks) during level 1 (34.9–37.6 s) | the rest (9 chunks) during bs's device top tier (70.5–72.7 s) |
| outcome | VERIFY OK | node reboot after bs level 4 | slow mdev tier and reciprocal, then node reboot in dm |

Every other run of this report (15, stream off or W = 4, 2add00f–9dfe39d) finished with VERIFY OK. The common factor of the two
failures is **VMM chunks created, mapped and given access while the GPUs run the bs products** (the old code maps the arena's rest
while only the seed DMAs run; its dm remaps and growths happen between products). The mechanism is not known (driver or firmware;
no node-side log is available to me). 47ccca1's `DB_POOL_VMM_STREAM` therefore refuses to run without `DB_POOL_VMM_STREAM_ACK=1`
(b94072c).

#### 3. Conclusions

**(a) The arena's second half in the background, in chunks — not worth it as built, and not safe.**
- The seeds were the obvious waste (idle 4.9–6.9 s for all four parity-0 halves); the stream removes that wait exactly as
  modelled: **seeds end 28.3 / 29.2 s instead of 34.3–35.5 s** (measured, W = 1).
- But level 1 does not move (34.9 / 35.9 s against 34.7–35.4 s on the old path, measured), because **the mapping itself is the
  floor**: level 1 needs 124 chunks (266 GB), and the mapping runs at ≈ 0.2 s per 2 GiB chunk whoever calls it (one worker:
  0.216 s/chunk; the old four pregrow threads 0.16–0.19 and the old background mappers 0.20–0.23 with the seeds computing;
  0.131 only with the CPU idle). `hipMemCreate` is ≈ 70 % of it, `hipMemSetAccess` ≈ 30 %, `hipMemMap` and the zeroing ≈ 0.
- The runtime serializes the calls (RL); four workers at once map slower (0.27 s/chunk) and starve other threads' launches
  (32.9 s for one seed DMA): there is no concurrency to buy.
- Letting the bs levels wait only for their own chunks (item 4 of §1) would overlap level 1 with the mapping — exactly the
  regime that preceded both node failures. Not built.
- With `BS_ARENA_ROOM=0.16` the same (its extra chunks are in the rest, not on level 1's path).

**(b) The seed threads.** On the old path (f1, measured): 192 threads (the default): level 1 at 35.07 / 35.40 s; 96: 36.46 s (spans
16.6 → 19.9 s; the mapping ×1.1 faster does not make up for it); 48: 47.63 s (spans 35.7 s). **Keep all threads.** The freed threads
would only help the mapping, which the seeds then wait for; with the stream (seeds 6 s early) 96 / 128 threads were in the plan but
not run (the node failure).

**(c) What else the timeline shows** (measured, `ECALC_INIT_TL=1`, 5162a8f — the only part of this branch recommended for merging):
- rns_init: staging and contexts 1.5–2.5 s, the plane pools 4.2–5.6 s (hipMalloc, ≈ 0.045 s/GB), then the arenas.
- The old background mapper lock (`g_vmm_map_mx`) is not fair: one APU maps 4–7 chunks in a row while the other three queue
  (9–12 s each); harmless for level 1 (it needs every APU's half) but it is why the per-APU "done" times spread 4–15 s.
- The seed thread's pinned buffers (2 × 8 GiB) cost 1.4–1.6 s at its start, on its path.
- **The lever that remains is the mapping rate, and it is outside the VMM path**: hipMalloc maps at 0.036–0.05 s/GB against
  hipMemCreate's 0.077–0.1 (R114 §6b, measured there). R114 measured the VMM arena at **+8 s of init at 10¹¹** against the
  hipMalloc arena; it was adopted for the memory ceiling (no in-phase hipMalloc fallback, 1.4 × 10¹¹ at cap 2³¹). With
  `BS_ARENA_ROOM=0.16` the division at 10¹¹ remaps 0 times (AS15), so **`DB_POOL_VMM=0` at the sizes where memory allows** may be
  worth one paired series (existing switches, no code; untested here — the nodes went down). Under `DB_POOL_VMM=0` the seeds also
  store directly into the regions (no buffers, no DMA: R114's other +8 s of bs).

**Expected gain of this branch at 10¹¹ and at the target: 0** (the stream is refused; the timeline switch is print only).

#### 4. Tests

| test | command | size | result | when |
|---|---|---|---|---|
| timeline, stream off | `map_sweep.sh 2add00f ~/MAP15/f1 map_plan_f1.txt` (job 21659, s24-30) | 10¹¹ × 7 (defaults, room, 96 / 48 seed threads, `ECALC_SEED_ORDER=after`) | **7 × VERIFY OK**; totals 187.1–199.5 s (`results/MAP15/f1/summary.txt`) | 22:21–22:44 EDT |
| stream, e9 | `DB_POOL_VMM_STREAM=4 ./ecalc 1e9` (job 21661, s24-26) | 10⁹ | VERIFY OK, 12.41 s | 22:44 |
| stream off, room | r0c (job 21661) | 10¹¹ | VERIFY OK, 179.07 s | 22:47 |
| stream W = 4, room | s4r (job 21661) | 10¹¹ | VERIFY OK, 216.91 s (seed launch starved) | 22:51 |
| stream W = 1, room | s1r (job 21661, s24-26) | 10¹¹ | **node reboot** | 22:53 |
| stream W = 1, room, rest deferred | s1r (job 21662, s24-30) | 10¹¹ | **node reboot** | 23:02 |
| build | `make` on the aac6 login node, b94072c | — | builds; `DB_POOL_VMM_STREAM=1` refused at the first arena | 23:05 |

**Not run** (stopped after the second node failure; two of the three nodes down, the third G5's): `t_dbig 0`,
`mnaccept --stress --only unit,e9,mn,stress`, 10¹¹ via `digcmp.sh`, the timing series. With the stream off every new code path is
print only or a wait that returns at once (`pool_get` parity 0, `seed_wait_map`, `db_vmm_stream_rest`), and the f1 / r0c runs on
this branch (10¹¹ × 8, VERIFY OK) exercised them; the gates are still owed before anything here merges:
`./mnaccept.sh $J --stress --only unit,e9,mn,stress` and `./tests/t_dbig 0` at b94072c, stream off.

#### 5. Open issues

- **Two nodes down** (s24-26, s24-30: "Node unexpectedly rebooted", 22:53 and 23:02 EDT) — they need the admin to resume them. The
  cause is not established; the correlation with VMM mapping during GPU compute is strong (2 of 2 against 0 of 15).
- If the stream is ever tried again: a unit test that maps chunks on one APU while kernels run on all four, on a node the admin can
  spare, before any full run.
- The old path also maps during compute at some sizes (the background rest when the seeds end before it, and the dm growths): no
  failure is known there, but the same question applies.
- `DB_POOL_VMM=0` with `BS_ARENA_ROOM=0.16` at 10¹¹ (§3 c): an untested idea for the init floor.

**Outside my files** (named): `binsplit.c` outside the seed code — `pool_get`'s parity-0 wait (one line) and the
`db_vmm_stream_rest()` call in the level loop (one line), plus `ECALC_INIT_TL` marks at level 1–3 (print only); `dbig.h` prototypes.
`tests/map_sweep.sh`, `tests/map_plan_*.txt` are new.

#### 576-node estimate (standing rule)

Unchanged by this work (nothing here is on by default or recommended): **4.25 × 10¹³ digits in ≈ 256 s (4.3 min) without the write,
≈ 286 s (4.8 min) with the packed write at 0.6 GB/s per node** (modelled, RESULTS §87 / DOC15, as AS15 quotes); node memory 416 GB of
480 on the defaults (modelled).

#### RESUME

- **Stopped** after the second node failure (§2.3). Branch `p15-MAP` at b94072c + this report; aac6 clone `~/ntt-MAP15` at b94072c
  (built); scratch clone `~/MAP15/build/w` (47ccca1). Logs in `results/MAP15/{f1,s1,s2}`. No job of mine running or pending.
- Recommended for merging: `ECALC_INIT_TL` only (5162a8f; print only). `DB_POOL_VMM_STREAM` stays behind its ACK guard; do not adopt.
- Owed before any merge: the gates of §4 at b94072c with the stream off. For the user: the two nodes need the admin.

## PS15 — Phase 15 agent PS: plane pool 1 at four primes (the rc 6 at 9.17 × 10¹⁰, `ECALC_NP=4`)

(former file: PS15.md)

Agent PS. Branch `p15-PS` from `int15f` 2788e1d. aac6 clone `~/ntt-PS15` (bundle from `~/ntt` f184d51), logs in `~/ps15/`.
Times Eastern (aac6 logs are Central: +1 h). Numbers labelled **measured** / **modelled** / **assumed**.

#### 1. The cause (read from the code at 2788e1d and the failing log)

`~/fin15f/t2_B1_1.log` (job 21693, s24-16, 06:20 EDT): `ECALC_NP=4 ./ecalc 91694091804` ran the bs phase's batch levels 1–21
and stopped at the next level: `rns_dpool: plane pool 1 on APU 0 would grow inside a phase, 12.89 -> 17.18 GB` (rc 6), not at init.

- Plane pool 1 is made at init by `rns_plane_pool_bytes` (rns_mul.c): **3 q + 16 limbs whatever the prime count**
  (q = 2^(pool_log−2) = 2^29 at 2^31: 12.89 GB), the dist tier's xb | sbuf | rbuf, one prime at a time.
- The **batch-local tier** (`rns_mul_batch_local`) holds a tile's B planes in pool 1: `np × Mmax × L` limbs (paired: `np × Mmax/2 × L`),
  with `Mmax = plane_cap / (np L)` and plane_cap = pool 0 as the one-node tiers see it (`rns_plane_limbs`) = np q. So its request
  reaches **np q**: at three primes 3 q < 3 q + 16 (never grows); at four primes **4 q = 2^31 limbs = 17.18 GB** when L = q
  (2^29 points) and Mmax = 1 (non-paired) — the level of 10 pairs (20 products, max_nl 2.7 × 10⁸) after level 21 at 9.17 × 10¹⁰.
  The comment at the request already said "grown here if a tile needs more (rare: L = 2^28, 2^29 at 2^31 pools)": four primes
  grew pool 1 here before Phase 12 R made a growth inside a phase fatal; Phase 13a P3 made three primes the default, where it
  cannot happen, so no default run reached it since.
- `mem_model.py` ported the same 3 q + 16 (`pool_limbs`, `planes_bytes`).

#### 2. The fix

- **`rns_plane_pool_bytes`**: with four one-node primes pool 1 = max(3 q + 16, the 2^min(pool_log,30) tile, **4 q**). Three primes:
  unchanged. `ECALC_NP=auto`: unchanged (the one-node tiers run three primes there, `rns_plane_limbs` caps them at 3 q, whatever
  pool 0 is made for). `rns_pool1_default_bytes` (ecalc.c's `rns_pool1_bytes_req`) now asks at `ec_np_init()` instead of a fixed 4.
  At 2^31: pool 1 16.00 GiB instead of 12.00, **+17.18 GB per node** (modelled = the formula; C's `BS_LAYOUT_ONLY` agrees, §3).
- **`RNS_POOL1_4Q`** (default 1 = the above). **0** keeps pool 1 at 3 q + 16 and makes the batch-local tier fit its tiles to pool 1
  as it is (`Mmax ≤ pool1 / (np L)`, ×2 paired); a batch whose B planes cannot fit at all (L = q) returns to `rns_mul_batch`, which
  runs the striped tier (one prime per APU: L limbs of pool 1 per tile). The fit is a no-op wherever pool 1 holds np q (three
  primes; four with the default), and is skipped under `RNS_POOL_GROW=1` (the stress recipe grows pool 1 there on purpose).
- `mem_model.py`: `pool_limbs` / `planes_bytes` / `b_extra_limbs` take `pool1_np` (the one-node prime count: `np`, 3 under auto);
  `mem_per_node` passes it (option `pool1_np`, 3 = `RNS_POOL1_4Q=0`); `OLD13` (the calibration of the old runs) keeps 3.

#### 3. The sweep on the login node (aac6, no device; `~/ps15_sweep.sh`, outputs `~/ps15/base2` = 2788e1d, `~/ps15/fix2` = beaac4c)

`BS_LAYOUT_ONLY` at 20 sizes (10⁹ … 1.3 × 10¹¹ at size 1; 10⁹ at 2, 3, 4; 4 × 10¹⁰ at 2, 4; 7.38 × 10¹⁰ and 8.854 × 10¹⁰ at 576), the
same with `BS_ARENA_ROOM=0.16 DB_POOL_VMM=1`, `ECALC_PLANE_CAP` = 30, 3x29, 31, 3x30, fit at 4 × 10¹⁰, 9.169 × 10¹⁰, 10¹¹, 1.3 × 10¹¹,
and `MN_PLAN_ONLY` at 9.169e10:1, 4e10:1, 1e11:1, 1e9:4, 5.1e13:576 — each under `ECALC_NP` unset, 3, 4, auto and `ECALC_NP=4 RNS_POOL1_4Q=0`
(27 files per setting, compared without the META line's date):

| setting | against 2788e1d |
|---|---|
| `ECALC_NP` unset, 3, auto | **all 27 files identical** (three-prime sizing byte-identical) |
| `ECALC_NP=4 RNS_POOL1_4Q=0` | **all 27 identical** to 2788e1d at `ECALC_NP=4` |
| `ECALC_NP=4` (the fix) | the `planes:` lines: +17.18 GB at the 2^31 cap (120.88 → 138.05), +12.89 at 3·2^29 (90.81 → 103.69), +25.77 at 3·2^30 (181.01 → 206.77), none at 2^30 (pool 1 was already the 2^30 tile = 4 q); `room_planes` 120.88 → 138.05 GB, and the arena room is dropped at 1.16 × 10¹¹ (node over 480 with it); `MN_PLAN_ONLY` headers `pools 16.00 + 16.00 GiB` (was + 12.00) |

Node totals at 9.169 × 10¹⁰, one node (C's `binsplit_node_bytes`, the budget check's model; **modelled**): cap 2^31 **412.88 GB** (was
395.70); `ECALC_PLANE_CAP=fit` at `ECALC_NP=4` now picks **2^31 (412.9 GB)** instead of 3·2^30 (455.8 GB; 481.6 with the fix).
Fit at `ECALC_NP=4`: 4 × 10¹⁰ 3·2^30 (362.4, was 336.6), 10¹¹ 2^31 (427.2, was 3·2^30 at 470.2), 1.3 × 10¹¹ 3·2^29 (451.1, was 2^31 at 468.3).

**A side effect at `ECALC_NP=4`** (plan, `MN_PLAN_ONLY=4e10:1`): the dist tier's `RNS_STRATEGY=auto` (the default) takes form B for a
product whose two planes fit the pools (`b_place`: n in pool 0, n in pool 1). With pool 1 at 4 q, **2^31-point products now run in
form B** at four primes (before: C, or pieces of 3·2^29): the plan at 4 × 10¹⁰ has 75 pieces instead of 84 (recip 65 of 68, div 10 of 16).
Three primes cannot (3 q + 16 < 4 q). Digits are exact either way; the times are measured in §4.

`mem_model.py --check-c` (now also the `planes:` lines: pool 0 + pool 1 at every cap, exact at the printed 0.01 GB):
`--check-c FILE 31 1024 128 {0|0.16} {3|4|auto} {1|0}` on all ten fix2 layout files: **0 terms not exact** (arena, the dm terms,
and 80 plane figures per file; 76–78 in the room files, where the room message splits two lines).

The target (`mem_model.py --p15`, **modelled**): the launch line (ECALC_NP=4, COMM_SHMEM_ROUND_MB=1024) at 5.1 × 10¹³ / 576:
**472.6 GB per node** (was 455.4: init device 405.0 → 422.2); the defaults without ROUND_MB 513.1 (was 496.0); the 480 GB ceiling at
576 on the launch line 9.10 × 10¹⁰ per node = 5.24 × 10¹³ (was 9.67 × 10¹⁰ = 5.57 × 10¹³). `RNS_POOL1_4Q=0` keeps the old figures.
`--cache` (TC's report): 5.1 × 10¹³ node 472.6 without the cache (was 455.4), `RNS_DIST_CACHE_FIT` room 7.4 GB (was 24.6; 0 slots
either way); 4.74 × 10¹³ node 456.4 (was 439.2). `mem_model.py` (the calibration and profile), unchanged output (OLD13 keeps 3 q + 16;
the size-1 profile is three primes).

#### 4. Node tests (s24-16)

##### Job A — 21702, 09:03–09:47 EDT, beaac4c (`~/ps15A.sh`, logs `~/ps15/A/`; every run `ECALC_VERBOSE=2`, one process, the four APUs)

| run | command | result (**measured**) |
|---|---|---|
| t2_fix | `ECALC_NP=4 ECALC_LIVE=1 MEM_REPORT_DEVS=1 ./ecalc 91694091804` (the failing run) | **rc 0, VERIFY OK**, total **172.36 s** (bs 77.4 + dm 72.5 = 150.0; init 22.2); no pool growth |
| e4e10_np4 | `ECALC_NP=4 ./ecalc 40000000000 <file>` | rc 0, VERIFY OK, total 61.05 s (bs 27.3, dm 18.5, init 15.0); `digcmp.sh` vs `~/ntt/ecalc/results/e_4e10.out` **identical** |
| e1e11_np4 | `ECALC_NP=4 ./ecalc 100000000000 <file>` | rc 0, VERIFY OK, total 212.13 s (bs 91.5, dm 101.7, init 18.6); vs `e_1e11.out` **identical** |
| t2_4q0 | `ECALC_NP=4 RNS_POOL1_4Q=0 RNS_VERBOSE=1 … ./ecalc 91694091804` | **rc 0, VERIFY OK**; the batch-local tier sent level 22 (N = 20, L = 2^29) to the striped tier (`the B planes (4 x 536870912 limbs) exceed plane pool 1 (1610874880 limbs)`), no growth; total 431.98 s — **not a timing**: `RNS_VERBOSE=1` synchronizes and prints per tile (the batch levels ran 2 × slower than t2_fix) |
| e4e10_4q0 | `ECALC_NP=4 RNS_POOL1_4Q=0 RNS_VERBOSE=1 ./ecalc 40000000000 <file>` | rc 0, VERIFY OK, 67.31 s (verbose); vs `e_4e10.out` **identical** (no batch reached the fallback at 4e10) |

**The memory report of t2_fix** (measured; `mem summary`):

| phase | device (all APUs) | largest APU | planes | pool borrowed | host RSS / HWM |
|---|---|---|---|---|---|
| init | 397.3 | 115.7 | **137.4** (34.36 per APU) | 124.6 (+ regions 134.7) | 24.5 / 24.5 |
| bs | 402.2 | 116.1 | 137.4 | 264.1 | 8.9 / 27.2 |
| recip, division, dm | 402.2 | 116.1 | 137.4 | 264.1 | 6.8–7.2 / 27.2 |

The failing run (2788e1d, `~/fin15f/t2_B1_1.log`) had planes 120.3 and device 380.1 at init: **+17.1 GB of planes**, as modelled.
`budget:` line (the model: `binsplit_node_bytes` + the init peak): **peak 422.4 GB** (device 397.3, host 25.1) of 480, ECALC_BUDGET_CHECK ok.
**Node total at 9.169 × 10¹⁰ with `ECALC_NP=4`: device max 402.2 + host HWM 27.2 = 429.4 GB measured, against 480 GB (50.6 GB free).**

The side effect in §3 shows in the times: the top bs levels (mdev, 2^31 products) took 4.1 / 7.4 / 7.3 s in t2_fix (form B) against
16.5 / 33.7 / 33.6 s in t2_4q0 (form C, 2^31 = 2^15 × 2^16, ~3.9 s per product with RNS_VERBOSE's syncs) — to be timed without
RNS_VERBOSE (job C).

##### Job B — 21706, 11:13–11:39 EDT, beaac4c (`~/ps15B.sh`, `~/ps15/B/`, mnaccept logs `~/ntt-PS15/ecalc/results/mnaccept/21706/`)

| command | result (**measured**) |
|---|---|
| `ECALC_NP=4 ./mnaccept.sh 21706 --only unit,e9,mn` | **15 PASS, 0 FAIL** (1323 s): t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid (2 procs); e9 size 1 decimal (15.06 s) and binary identical; **mn**: 10⁸ at sizes 2, 3, 4 and 10⁹ at sizes 2, 4 identical, all nodes VERIFY OK |
| `./mnaccept.sh 21706 --only e9` (the default, three primes) | 2 PASS: e9 decimal identical (9.60 s), binary identical |
| `ECALC_NP=4 ./mnaccept.sh 21706 --stress --only stress` | **PASS** (195 s): 10 of 10 identical, all nodes VERIFY OK, 16 pool growths logged in every run (`RNS_POOL1_GB=0.4` still forces the growth: the fit in the batch-local tier is off under `RNS_POOL_GROW=1`) |

##### Job C — 21732, 11:55–12:14 EDT, beaac4c (`~/ps15C.sh`, `~/ps15/C/`): clean timings at 9.169 × 10¹⁰ (no RNS_VERBOSE), then the default 4e10

One run each, the same node, back to back (**measured**; `ECALC_VERBOSE=2 ./ecalc 91694091804`, no output file, all VERIFY OK, no growth):

| setting | total | bs | dm | init | top bs levels 22 / 23 / 24 / 25 / 26 (s) | budget peak (model) |
|---|---|---|---|---|---|---|
| `ECALC_NP=4` (the fix) | **173.96** (job A: 172.36) | 82.6 (77.4) | 71.8 (72.5) | 19.2 | 2.32 / 2.67 / 3.89 / 7.35 / 7.32 | 422.4 |
| `ECALC_NP=4 RNS_POOL1_4Q=0` | 188.97 | 79.6 | 89.9 | 19.2 | 2.91 / 2.65 / 5.65 / 10.84 / 10.64 | 405.2 |
| `ECALC_NP=4 RNS_STRATEGY=C` (the fix, form C forced) | 203.86 | 92.4 | 92.5 | 18.6 | 2.32 / 2.67 / 5.88 / 10.76 / 10.62 | 422.2 |
| `ECALC_NP=3` (for reference) | 158.45 | 64.0 | 76.2 | 17.9 | 1.83 / 2.76 / 4.00 / 8.79 / 8.72 | 388.0 |

Reading (one run per setting, so ± a few s): the fallback itself (level 22 on the striped tier) costs 0.6 s; the rest of the 15 s
between the fix and `RNS_POOL1_4Q=0` is form B at 2^31 (the mdev levels 24–26 and the dm phase's 2^31 products), which the larger
pool 1 enables (§3) — forcing form C with the fix's pools gives the `RNS_POOL1_4Q=0` times. So at `ECALC_NP=4` the 17.2 GB buy
≈ 15 s at 9.17 × 10¹⁰ on one node (measured, single runs). Four primes cost +15.5 s against three at this size.

| gate | result |
|---|---|
| `ECALC_VERBOSE=2 ./ecalc 40000000000 <file>` (default, three primes) | rc 0, VERIFY OK, 59.45 s (bs 24.4, dm 21.7, init 13.2); vs `e_4e10.out` **identical** |

#### 5. Summary

- **Cause**: plane pool 1 was 3 q + 16 at every prime count; the batch-local tier's B planes reach np q, i.e. 4 q at four primes
  (L = 2^29, one product per tile, the 20-product level at 9.17 × 10¹⁰). Three primes never reach it (3 q).
- **Fix** (default): pool 1 = 4 q with four one-node primes (`rns_plane_pool_bytes`, one formula for rns_init, the budget, the
  layout lines, `MN_PLAN_ONLY`); three primes and auto unchanged byte for byte (27 sweep files each). `RNS_POOL1_4Q=0`: the old
  pools, with the batch-local tier fitted to pool 1 (the striped tier at L = q) — also correct, 17.2 GB less, ≈ 15 s slower here.
- **The failing run passes**: `ECALC_NP=4 ./ecalc 91694091804` VERIFY OK in 172.4 / 174.0 s; node **429.4 GB measured** (device
  402.2 + host HWM 27.2) against 480; the budget model 422.4.
- 4 × 10¹⁰ and 10¹¹ at `ECALC_NP=4` identical to the references; mnaccept unit, e9, mn and stress at `ECALC_NP=4` pass; e9 and
  4 × 10¹⁰ at the default pass.

#### 6. Open issues (for the integrator / the user)

1. **The target's memory** (modelled): with `ECALC_NP=4` on the launch line every node gets +17.18 GB of pool 1: 5.1 × 10¹³ / 576
   is **472.6 GB per node** (was 455.4), 7.4 GB under 480; the 480 GB ceiling on the launch line drops from 5.57 to 5.24 × 10¹³.
   `RNS_POOL1_4Q=0` keeps 455.4 at ≈ 15 s per node-size run (measured at 9.17 × 10¹⁰ on one node; at 576 the one-node tiers are
   only part of the wall — not modelled here). `ECALC_NP=auto` is not affected (pool 1 at three). The user's choice.
2. **Form B at 2^31 is new at four primes** in the default flow (the dist tier's auto strategy follows the pool). It passed every
   test above (4e10, 1e11, 9.17e10, mnaccept), and it is the faster form, but it was not a measured path before.
3. `ECALC_PLANE_CAP=fit` at `ECALC_NP=4` changes its picks (9.17 × 10¹⁰: 2^31 instead of 3·2^30; 10¹¹: 2^31; 1.3 × 10¹¹: 3·2^29):
   the pools it compares are now the real ones.
4. `BS_ARENA_ROOM=0.16` at `ECALC_NP=4`: the room is dropped at 1.16 × 10¹¹ and above (the node with it exceeds 480; layout lines).
5. Not done: RNS_POOL1_4Q=0 at 10¹¹ (only 4e10 and 9.17e10 were run with it); repeated timings (one run each in job C).

#### RESUME

- Step 1 (done, bda8966): the cause, the init-sizing fix in `rns_plane_pool_bytes` and mem_model.py.
- Step 2 (done, 66ee3a1 + beaac4c): `RNS_POOL1_4Q` (0 = pool 1 at 3 q + 16 with the batch-local tier fitted to it), README row.
- Step 3 (done, 1b62486): the sweep (§3), `--check-c` with the `planes:` lines, the target model.
- Step 4 (done): job A (§4).
- Step 5 (done, §4): job B = `~/ps15B.sh beaac4c` (setsid; job 21706 `-J PS`, pending behind G5 / CX), summary `~/ps15/B/summary.txt`:
  `ECALC_NP=4 ./mnaccept.sh $J --only unit,e9,mn`, `./mnaccept.sh $J --only e9` (default), `ECALC_NP=4 ./mnaccept.sh $J --stress --only stress`.
  It cancels its own job. If stopped: `squeue -u <user> -n PS`.
- Step 6 (done, §4): job C. §5 summary, §6 open issues. Nothing of mine is queued or running on aac6.
- aac6: `~/ntt-PS15` at ebf3749 (the tests ran at beaac4c: the same C code; later commits are mem_model.py --check-c and this file), `~/ps15/` (sweeps base2 /
  fix2, jobs A, B, C), `~/ps15/wt_base` (a worktree of the clone at 2788e1d, the sweep's base build).

## DL15: the arena sized to DKM's division, and the room check on the model's node (Phase 15 Batch 3, agent DL)

(former file: DL15.md)

Branch `p15-DL` from `int15i` ab8daa2. The worktree HEAD was main 28837f9, so I branched from ab8daa2. aac6 clone: `~/ntt-DL15`, and the comparison build `~/ntt-DL15base` (ab8daa2). Times below are Eastern.

#### New ceilings on 576 nodes, room on (modelled; the arena measured by `BS_LAYOUT_ONLY`; `tests/dl15_ceilings.py`)

| | largest total digits that fits 480 GB **with the room kept** | the room turns off from | largest size that fits at all (room dropped, arenas in whole chunks) |
|---|---|---|---|
| **built (`NEWTON_DKM=1`, the default)** | **5.396 × 10¹³** (node 471.93 GB) | 5.398 × 10¹³ | **5.532 × 10¹³** (471.93 GB) |
| `NEWTON_DKM=0` (today's layout, the new check) | 5.166 × 10¹³ | 5.168 × 10¹³ | 5.422 × 10¹³ |
| one node, built | 1.320 × 10¹¹ | 1.322 × 10¹¹ | 1.382 × 10¹¹ |

The launch line was `ECALC_NP=auto RNS_DIST_CACHE_FIT=1 COMM_SHMEM_ROUND_MB=1024 COMM_TRANSPORT=shmem`. The node figures per point (the `room:` line, measured layout + the model's host):
- 5.1 × 10¹³: 471.93 → **463.34 GB** (arena 300.65 → 292.06 GB).
- 5.167 × 10¹³: **463.34 GB**, room on.
- 5.276 × 10¹³: **471.93 GB**, room on. Today's build drops the room here under the new check.
- 5.55 × 10¹³: room dropped, **480.52 GB: over** the budget.

**Finding: the room-0 figures overstate the ceiling by one chunk.** The VMM pool maps every arena in whole 2 GiB chunks even with `BS_ARENA_ROOM=0` (`dbig.c db_vmm_arena_alloc`: m0 = ⌈bytes / chunk⌉). `mem_model`'s room-0 node counts the arena unrounded. So MS's room-0 ceilings (5.571 × 10¹³; 5.696 × 10¹³ with DKM) are really **5.422 / 5.532 × 10¹³**, and 5.55 × 10¹³ does not fit.

#### What was built (`binsplit.c`, `mem_model.py`)

1. **`dm_layout` follows `NEWTON_DKM`.** The DKM layout applies at size > 1 always, and at size 1 only in the device flow (n_Q ≥ 9/8 · 2^`BS_MDEV_LOGL`).
   - The reciprocal's jl is taken at h.
   - The division set is DKM's.
   - The hole is DKM's largest block. At size 1 that is step 1's `X_hi·Q`, which `mul_grid` reserves whole: n_Q + ⌈k/2⌉ + 8 limbs. At size > 1 it is t, k + 8 limbs.
   - The model had used k + 8 at size 1 too, which is smaller than that block. It is changed to match.

   With `NEWTON_DKM=0` the layout lines are byte-identical to ab8daa2's. The only differences are the `room_planes` field and the room decisions (item 2).
2. **`as_room_fits` counts the model's node.** It adds:
   - the planes by the `ECALC_NP=auto` rule;
   - the arena with the room;
   - the 6.0 GB of VMM bs growth;
   - the host HWM: at size 1 the measured fit; at size > 1, 7.0 GB + 4 GiB of staging + the seed buffers + 6.0 GB of transport + the SHMEM pool, or the division's host if that is larger.

   `mem_model.as_room_fits` / `room_host` is the twin. `BS_LAYOUT_ONLY` prints a `room:` line, and the layout line gains ` dkm 1`.
3. **`mem_model.py --check-c` is exact** on 16 layout files: 576 at 5.1, 5.167, 5.276 and 5.55 × 10¹³, with and without shmem; size 1 at 4 × 10¹⁰, 10¹¹ and 1.3 × 10¹¹; sizes 2–4 at 10¹⁰; each with `NEWTON_DKM` 0 and 1. That is 0 terms not exact, including the room terms (`archive:results-raw/DL15/checkc.txt`).
4. **Changes outside my files:**
   - `mn_model.py` passes `dkm=DKM` to mem_model (2 lines).
   - `--check-c`'s `MN_P24` default is now 2, the code's default.
   - `DEFAULTS15D` is the new base of `mem_per_node`.
   - README rows for `NEWTON_DKM`, `BS_ARENA_ROOM` and `BS_LAYOUT_ONLY`.
   - New files: `archive:drivers/ecalc/dl15_batch.sh` and `tests/dl15_ceilings.py`.

#### Tests (aac6, measured)

**Batch A** (job 21801, s24-16, 01:35–02:05): 10¹¹ run 4 times, alternating new and base (ab8daa2), all with `NEWTON_DKM=1`.
- **Correctness.** new_1 is identical to `e_1e11.out` (`digcmp.sh`), and the other three runs are the same bytes.
- **Memory.**
  - The arena is 283.5 GB against 289.9 GB (−6.4 GB); device at init 387.2 against 393.6 GB.
  - Every run had **0 remaps, 0 growths and 0 hipMalloc**.
  - The division's window peak is 54.07 GB per APU in both builds (76.3 % of the new pool).
  - The new tail (16.93 GB) took 4 large blocks on every APU.
- **Time.**

  | run | `dm` (s) | division (s) | `total` (s) |
  |---|---|---|---|
  | new_1 | 72.49 | 59.28 | 176.2 |
  | base_1 | 72.93 | 59.63 | 169.9 |
  | new_2 | 74.24 | 60.32 | 202.9 |
  | base_2 | 74.63 | 59.49 | 175.3 |

  `dm` is **neutral**. new_2's bs was an outlier (105.1 s against 78.8–82.9 s). The init and bs spread is noise, not the layout, which does not touch bs. The plain-defaults timing series (stage D) was not run, on the integrator's instruction.

**Batch B** (job 21805, s24-30, from 02:12):
- **1.3 × 10¹¹, new.** The room was kept; the arena is 335.0 GB. **0 remaps**, dm 114.25 s, VERIFY OK. The budget check's peak was 464.1 GB.
- **1.3 × 10¹¹, base.** The old check dropped the room, and the run had **2 remaps** (0.92 s) and dm 117.70 s.
- **4 × 10¹⁰.** Identical, 0 remaps.
- **10¹⁰ at sizes 2, 3 and 4** (`mnrun.sh`, `POOL_LOG=29`). All identical, all VERIFY OK, with 0 remaps, 0 growths and 0 hipMalloc. The division's window peak is 5.52 / 2.96 / 2.76 GB per APU, 43–49 % of the pool.

**Batch C** (job 21808): `./mnaccept.sh <job> --stress --only unit,e9,mn,corr,stress` gave **20 passed, 0 failed**. That includes `t_dbig 0`, e9 in both bases, mn, corr and stress (10 of 10).

#### Open issues

1. **The target's mn division peak is only counted, not measured.**
   - DKM's mn path peaks at 5.5 u + the product scratch (u = the Q share). These are the step 2 moments; `mn_divmod_dkm` keeps Qw and copies of Ah / m2. The reservation beside the scratch at 5.1 × 10¹³ is 55.54 GB per APU against 54.11 GB counted, so the margin is 1.4 GB (modelled).
   - Today's mn path counts 6.0 u = 59.0 GB against 56.83 reserved, already over (M6's finding).
   - At sizes 2–4 on one node the pool is not dm-bound (43–49 %), so these runs do not test the margin. A 2-node `ECALC_LIVE=1` run at a dm-bound size would.
2. **Whole-chunk accounting at `BS_ARENA_ROOM=0`** (above). `mem_model`'s node should round the VMM arenas to chunks. Not done; it is outside the room and layout terms.
3. **The size-1 room host uses log10 N! for the digits**, where the model's node uses the requested D. The difference is < 0.02 GB.

#### RESUME

Done. Commits are on `p15-DL`. No jobs are running or pending. The logs are in aac6 `~/dl15tmp/{lay,A,B,C}/` and `~/ntt-DL15/ecalc/results/mnaccept/21808/`. Stage D of `dl15_batch.sh` (the timing series) is written but was not run.

## I — the initialisation floor: every allocation form measured, the seeds against the mapping, the 3·2³⁰ planes on the number (Phase 12, PLAN §27 row I)

(former file: I.md)

Branch `i12` (from `main` @ 7aded87; aac6 clone `~/ntt-i`, logs in `~/i12/`). Files: `ecalc/mem.c/.h` (the allocation form),
the pools' creation in `ecalc/rns_mul.c` (the plane switch's default and the verbose line only), the init lines of `ecalc/ecalc.c`
(the C3 block deleted), `binsplit_seeds_begin` in `ecalc/binsplit.c` (the seed order), NEW `ecalc/tests/t_alloc.c` (+ its
Makefile rule). Nothing else touched. Every ecalc run below: `ECALC_VERBOSE=2 RNS_VERBOSE=1`, 4 × 10¹⁰ with the reference
evicted before each run and the digits `cmp`'d against `~/ntt/ecalc/results/e_4e10.out` after it; jobs 20873 (s24-30), 20894 and
20903 (s24-16), 20929, 20934, 20936 (s24-26).

#### Summary

| item | result | adopted |
|---|---|---|
| 1. every allocation form (`t_alloc`) | no form maps cheaper than `hipMalloc` in a fresh process: 0.057–0.072 s/GB, the kernel's page work on the calling thread (system time = wall) under one lock; the host forms cost more and route `hipMemcpy` through the SDMA engine (21 GB/s) | `MEM_ALLOC=hipmalloc\|fine\|uncached\|managed\|host\|mmap` in mem.c, default `hipmalloc` |
| 2. the allocator behind the pools, regions, the block pool | one pair (`mem_dev_malloc` / `mem_dev_release`) under every `hipMalloc` of mem.c (`mem_dev_alloc`, `dpool_get`, `dpool_get_exact`); M's tail layout and the accounting untouched | yes (bit-identical in every form tried) |
| 3. the seeds vs the mapping window (`ECALC_SEED_ORDER=overlap\|first\|after`) | overlap 81.5 s, seeds first 88.5, seeds after 88.3: the overlap wins by 7 s because the seeds slow the mapping by only ≈ 1 s while their 14 s of CPU work is hidden | overlap stays the default |
| 4. the 3·2³⁰ planes on the floor | same node, same batch: phases 58.5–59.3 against 63.1–64.8, init +4 s, **wall 80.8 ± 1.3 against 82.0 ± 0.3** | **the size rule is the default** (on below 5 × 10¹⁰ at 2³¹ pools) |
| 5. `ECALC_DM_POOL` / the C3 block | deleted (`ECALC_TAIL=0` remains the fallback; the in-phase-growth report line stays) | yes |

**The floor.** Init at 4 × 10¹⁰ is the bytes the run needs, mapped by the driver at 0.05–0.075 s/GB, with the seed thread's
14 s of CPU work hidden inside it: 252 GB (arenas 132.3 GB in 5.5–7.9 s, plane pools 120 GB in 8–9 s) = 16–19 s with the
planes off, 312 GB = 20–23.5 s with them on. Nothing measured maps cheaper.
What the cost is: **CPU time in the kernel on the allocating thread** — a fresh process's first `hipMalloc` of 28 GB on one
APU takes 2.01 s of wall of which 2.01 s is system time (`t_alloc`'s probe, `getrusage(RUSAGE_THREAD)`), 0.072 s/GB = 14 GB/s,
the rate of one core allocating and clearing pages (on the MI300A the "VRAM" of a hipMalloc is system memory of the APU's
NUMA node, handed out by the amdgpu/TTM allocator in zeroed pages). Four threads allocating on the four APUs at once take
7.9 s of wall for 1.5–2.4 s of system time each: **serialised by one lock** (the same 0.071 s/GB aggregate as one thread
making the four calls, 0.069). The only cheaper mapping seen is a **re-allocation of memory the process freed**: 0.035 s/GB
(the probe run after a 28 GB allocation had been freed: 3.9 s for the same 112 GB, system 1 s per thread) — the runtime or
the driver keeps the freed pages and skips the clearing; that is what A-mem's 0.047 and the 0.035 of an earlier build of this
branch were. So a persistent pool would help only within one process (nothing in the run frees and re-maps at this size:
zero `hipMalloc` inside the phases since M11), and across runs only a daemon holding the memory would — not a design for 576
nodes. `hipMallocAsync` from a mempool with the release threshold at max is the same idea inside one process: its first
allocation costs the same (11.7 s for 200 GB) and a re-allocation after `hipFreeAsync` faulted on this ROCm (an illegal
address in the first kernel over it), so it is not a candidate.

#### 1. `tests/t_alloc` — the table (job 20873, s24-30; 50 GB per APU on the four APUs at once, one thread per device as init does)

`./tests/t_alloc 50` (all forms but `async`; `async` on request, `none` = the probe alone). Columns: `alloc` = the wall of
the slowest thread, s/GB = that wall over the bytes of all four APUs (they serialise), `fill` = a first-touch kernel over the
whole buffer, `touch` / `regist` = the mmap forms' CPU first touch on the node and `hipHostRegister`, `bw` = a 16-byte copy
kernel over 25 GB (GB/s, read + write), `ntt` = `ntt_fwd` at 2²⁸ points × 4 (s), `d2d loc` / `pr` = `hipMemcpyAsync` of 8 GiB
between the halves on the same APU / to the next APU's buffer (GB/s), `cpu` = 48 threads of the buffer's node storing 4 GB
into it (GB/s), `free` = the release.

| form | alloc (s) | s/GB | fill | touch | regist | bw GB/s | ntt s | d2d loc | d2d peer | cpu GB/s | free | note |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `hipMalloc` (first in the process) | 14.38 | 0.072 | 0.03 | | | 3029 | 0.049 | 1449 | 91 | 2.6 | 0.53 | 12.56 s / 0.063 in the first run of the day |
| `hipMallocAsync` from a mempool, release threshold max | 11.74 | 0.059 | 0.02 | | | | | | | | 0.78 | the cold allocation; the warm re-allocation after `hipFreeAsync` faulted (illegal access in the fill kernel) |
| `hipMallocManaged` | 22.90 | 0.114 | 0.02 | | | 3065 | 0.049 | **21** | 21 | 31.2 | 0.99 | the copies on the SDMA engine |
| `hipExtMallocWithFlags` uncached | 8.60 | 0.043 | 0.02 | | | 3047 | 0.050 | 1608 | 92 | 2.6 | 0.52 | (after the frees above: warm pages, see the probe) |
| `hipExtMallocWithFlags` fine-grained | 7.03 | 0.035 | 0.02 | | | 3032 | 0.049 | 1591 | 92 | 2.6 | 0.53 | (warm; 0.057–0.066 in a fresh process, job 20873 `b3`) |
| `hipHostMalloc` coherent | 18.47 | 0.092 | 0.02 | | | 2986 | 0.049 | 21 | 21 | 30.7 | 0.73 | |
| `hipHostMalloc` non-coherent | 18.35 | 0.092 | 0.02 | | | 3016 | 0.049 | 21 | 21 | 30.7 | 0.76 | |
| `hipHostMalloc` NumaUser (thread pinned to the node) | 18.14 | 0.091 | 0.02 | | | 3018 | 0.049 | 21 | 21 | 30.6 | 0.73 | |
| mmap + `MADV_HUGEPAGE` + touch on the node + `hipHostRegister` | 16.08 | 0.080 | 0.02 | 2.60 | 13.48 | 3032 | 0.049 | 21 | 21 | 29.9 | 0.86 | the registration is the cost (0.067 s/GB), the touch 0.013 |
| mmap without huge pages | 136.2 | 0.681 | 0.04 | 30.2 | 105.9 | 1818 | 0.126 | 18 | 18 | 33.2 | 13.8 | 4 KiB pages: the GPU runs at 60 % (TLB) |
| `MAP_HUGETLB` | — | | | | | | | | | | | no reserved huge pages on the nodes (`nr_hugepages` 0) |

Read across: **every form runs the kernels at HBM speed** (bw 3.0 TB/s, the 2²⁸ transform 49 ms — the same to the ms) except
4 KiB-paged host memory; the host-side forms (`hipHostMalloc`, mmap + register, managed) let the CPU store 12 × faster
(30 GB/s against 2.6 — the seeds' stores would gain) **but `hipMemcpy` between two such buffers goes to the SDMA engine at
21 GB/s** (the blit kernels do 1.4–1.6 TB/s on device memory), which the checkpoints, the tree's slab copies and every
`mem_dev_copy` would pay — and they map no cheaper (0.08–0.11 s/GB). The device forms (`hipMalloc`, fine, uncached) are
the same memory at the same cost: the 0.035–0.043 of the two `hipExtMallocWithFlags` rows is the warm re-allocation of the
200 GB the earlier rows had freed, not the form — a fresh process pays 0.057–0.066 for them too (`b3.out`: hipmalloc / fine /
hipmalloc / fine / uncached / hipmalloc at 28 GB per APU: 0.065, 0.066, 0.064, 0.057, 0.063, 0.057 — the run-to-run noise,
no ordering by form). That is why `MEM_ALLOC`'s default went from `fine` (the first commit, on the 0.035 reading) back to
`hipmalloc`: the 4 × 10¹⁰ runs with `fine` (init 18.5 / 19.2 s, wall 84.9 / 85.1) were no faster than `hipmalloc` (16.0 /
17.7 / 18.1 / 17.3, wall 82.3 / 81.5 / 82.3 / 82.1) on the same node.

**The probe (fresh process, `t_alloc 28 none`, job 20903, s24-16):** four threads on the four APUs, 28 GB each: wall 7.6–7.9 s
per thread, user 0, system 1.5–2.4 s — 7.9 s of wall for 112 GB (0.071 s/GB) with the four threads mostly *waiting*; one
thread making the four calls: 7.77 s (0.069); one thread, one APU, 28 GB: wall 2.01 s = system 2.01 s (0.072 s/GB). The
mapping is CPU work in the kernel on the caller's thread, serialised across callers by a lock: neither more threads nor
another API changes it. (Four processes would be the test of a per-process lock; a fork of a HIP process cannot allocate,
and one node-process per node is the design, so it was not pursued.)

**The seed-like team (`t_alloc 50`, the second part):** `hipMalloc` of 200 GB while a team of compute-bound threads runs —
none: 11.25 s (0.056 s/GB); all 192 cpus computing only: 15.7 s (0.079); all cpus computing *and storing into device memory*
(the seeds' pattern, here at the team's full store rate of 200 GB/s): 44 s (0.221); 4 or 8 cpus left free: 56 / 38 s; half the
cpus: 42 s. So the mapping is slowed 1.4 × by CPU contention and 4 × by CPU stores into device memory *at the team's full rate*
— but the real seeds store only ≈ 6 GB/s (their arithmetic is the work), and in the run the plane pools map at 0.067–0.075
s/GB under the seeds against 0.064 alone (item 3 below): the contention P saw (0.08–0.12 s/GB) is mostly the fresh-process
cost itself plus the pool lock, not the seeds.

#### 2. `MEM_ALLOC` — the allocator (mem.c)

`mem_dev_malloc(dev, bytes)` / `mem_dev_release(dev, p)` are the pair every large device allocation goes through: `mem_dev_alloc`
(the regions / arenas, the block pool's chunks through `db_pregrow`'s own hipMalloc are dbig.c's — untouched, zero of them
since M11), `dpool_get` and `dpool_get_exact` (the plane pools), `dpool_free`, `mem_dev_free`, `mem_dev_free_raw`. The form
is `MEM_ALLOC`: `hipmalloc` (default), `fine`, `uncached`, `managed`, `host` (NumaUser, the thread pinned to the APU's node),
`mmap` (anonymous, `MADV_HUGEPAGE`, first touch by the node's threads, `hipHostRegister`; a table keeps the lengths for
`munmap`). The verbose pool line prints the form. `mem_oom` names it. Nothing else changed: M's tail layout, the donation,
the accounting (`mem_report`) see the same pointers.

Tried at 4 × 10¹⁰ (job 20873, s24-30, one batch): `fine` 84.9 / 85.1 s (init 18.5 / 19.2), `hipmalloc` 82.3 (init 16.0);
identical digits, VERIFY OK. The host forms were not run through ecalc: the 21 GB/s copies rule them out before the wall does.

#### 3. The seeds against the mapping window (`ECALC_SEED_ORDER`, job 20894, s24-16, one batch)

`binsplit_seeds_begin` (the `rns_after_staging_hook`, inside `rns_init` after the staging): `overlap` (default) starts the seed
thread, maps the arenas, posts them to the thread, returns — the plane pools map while the seeds stream; `first` maps the
arenas, then joins the seed thread before returning (nothing maps while the seeds run, the plane pools map alone after);
`after` maps the arenas and returns without starting the seeds — they run synchronously in `binsplit_e` with everything mapped.

| 4 × 10¹⁰ | init | of it: arenas / seed wait / plane pools | seeds (where) | bs | dm | phases | **wall** |
|---|---:|---|---|---:|---:|---:|---:|
| (c) overlap (default) | 17.7 | 6.9 / — / 8.8 (0.073 s/GB under the seeds) | 16.4 s in init (spans 8.5, waited 5.5 for the regions, 1.7 issuing the two DMAs) | 35.2 | 28.5 | 63.8 | **81.5** |
| (b) seeds first, then the pools | 24.2 | 6.8 / 7.6 / 8.1 (0.067 alone) | 14.4 s in init (joined before the pools) | 35.6 | 28.7 | 64.4 | 88.5 |
| (a) mapping first, then the seeds | 16.2 | 6.8 / — / 7.7 (0.064 alone) | 9.1 s in bs (spans 8.5, all direct) | 43.9 | 28.2 | 72.1 | 88.3 |

The wall decides for the overlap: the plane pools map only 0.7–1.1 s slower under the seeds (8.8 against 7.7–8.1 s for 120
GB), while moving the seeds out of the window puts 7–9 s of them on the wall. (The seeds alone are 9 s of spans with the
regions mapped; in the overlap they wait 5.5 s for the regions and issue their two buffered DMAs 1.7 s late behind the
runtime's lock — 16.4 s of thread time for 8.5 s of work, all inside init's 17.7.) `BS_SEED_THREADS=96` (half the cpus, to
leave the mapping thread alone) made the spans 13.9 s and the wall 83.7; 176: 82.0 — no gain either way. Digits identical
in all three orders.

#### 4. The 3·2³⁰ planes on the floor (job 20903, s24-16, one batch, the reference evicted before each)

| 4 × 10¹⁰ | init (arenas / pools) | bs (batch / top levels) | recip | dm | phases | **wall** | digits |
|---|---|---|---:|---:|---:|---:|---|
| `RNS_PLANES_3Q30=1` (planes 24 + 18 GiB per APU, 180 GB) | 22.5 (7.3 / 13.4 = 0.074 s/GB) | 32.7 (22.1 / 10.5) | 12.5 | 26.4 | **59.2** | **81.7** | identical, VERIFY OK |
| planes off (16 + 12 GiB, 120 GB), run 1 | 18.1 (7.4 / 9.1) | 35.2 (22.5 / 12.6) | 13.5 | 28.9 | 64.2 | 82.3 | identical, VERIFY OK |
| planes off, run 2 | 17.3 (6.3 / 9.3) | 35.8 (22.8 / 12.8) | 13.6 | 28.9 | 64.8 | 82.1 | identical, VERIFY OK |

The 60 GB more map at the same 0.074 s/GB (+4.4–5.2 s of init) and the phases gain 5.0–5.6 s — P's 3.6 s in the top levels
and the reciprocal, plus 2.5 s in the division that M11's tail layout uncovered (the division's products on 3·2³⁰ planes
with nothing mapped inside the phase). Net −0.4…−0.6 s of wall: by P's rule ("on below 5 × 10¹⁰ if it wins") **the size rule
is now the default** (`rns_planes_3q30_default`: on at 2³¹ pools below 5 × 10¹⁰ digits; `RNS_PLANES_3Q30=0/1` overrides;
7–8 × 10¹⁰ do not fit with them and stay off). The gain is within one run's noise on the wall but not on the phases (5 s,
every tier), and it is the fastest phase time of this code (59.2 s; P's 62.4 on the old layout). The final-default runs
are in §6.

#### 5. `ECALC_DM_POOL` deleted

The C3 block in `ecalc.c` (the block pool pre-grown to the reciprocal's scratch, on from 5 × 10¹⁰) is gone: with the tail
layout t1's quarter lands in the arena's reserved tail and the pool never grows (zero `hipMalloc` in every phase at 4, 8 ×
10¹⁰ here, as in M11). `ECALC_TAIL=0` is the fallback (the Phase 10 layout, the pool growing inside the phase), and the
"recip: the block pool grew by … inside the phase" line stays as the check. `ECALC_DM_POOL_K` (the arena sized at init) is
untouched.

#### 6. The gate

| test | command (from `ecalc/`) | result |
|---|---|---|
| 10⁹ decimal, binary | `./ecalc 1000000000 /tmp/x` ; `LIMB_BASE=2` | identical to `ref/e_1000000000.txt` in every build: 14.3 / 24.7 s with the planes off, 18.2 / 23.8 on the final defaults, and identical with `MEM_ALLOC=fine` |
| 4 × 10¹⁰, `MEM_ALLOC=fine` × 2 | job 20873 | identical, VERIFY OK, 84.9 / 85.1 s (init 18.5 / 19.2) |
| 4 × 10¹⁰, `MEM_ALLOC=hipmalloc`, planes off × 5 | jobs 20873, 20894, 20903 | identical, VERIFY OK: 82.3 / 81.5 / 82.0 / 82.3 / 82.1 s (init 16.0 / 17.7 / 18.2 / 18.1 / 17.3) — **82.0 ± 0.3 s, init 17.5 ± 0.9** |
| 4 × 10¹⁰, the seed orders, `BS_SEED_THREADS` 96 / 176 | job 20894 | identical (§3) |
| 4 × 10¹⁰, planes on (the final default) × 6 | jobs 20903, 20929, 20934 | identical, VERIFY OK: 81.7 / 80.4 / 78.8 / 82.2 / 81.7 / 79.8 s (§6b) |
| 8 × 10¹⁰ × 2 | `./ecalc 80000000000 /tmp/x` (planes off by the size rule) | **VERIFY OK, 192.0 and 189.3 s** (init 20.9 / 23.1: arenas 248.2 GB in 9.7–10.8 s, plane pools 18.5–21.2; bs 86.3 / 83.6, dm 84.7 / 82.5), **device 369.1 GB throughout, zero hipMalloc on every APU, host 13.0: node peak ≈ 382 GB** (M11: 190.9–195.5 s, the same 369.1 / 382) |
| `t_ntt 24` | `./tests/t_ntt 24` | VERIFY OK (565 checks); 2²⁴ fwd 0.6–0.7 ms (1.22–1.29 TB/s), batched log L 14 / 17: 1026–1219 / 1068–1105 GB/s (P's log: 1006–1058 / 914–1022) |
| 10⁸ sizes 2, 4 | `SLURM_JOB_ID=$J ./mnrun.sh <p> env POOL_LOG=27 ./ecalc 100000000 ~/i12/mn/x` | every node VERIFY OK, the concatenated parts identical to `ref/e_100000000.txt` (jobs 20903, 20936) |
| `t_alloc` | `./tests/t_alloc 50`, `28 none`, `28 <form>` | §1 |

##### 6b. The final defaults (planes by the size rule, `MEM_ALLOC=hipmalloc`, seeds overlapped; jobs 20903 s24-16, 20929 s24-26)

| run | node | init | bs (batch / top levels) | recip | dm | phases | **wall** | digits |
|---|---|---:|---|---:|---:|---:|---:|---|
| 1 | s24-16 | 22.5 | 32.7 (22.1 / 10.5) | 12.5 | 26.4 | 59.2 | 81.73 | identical, VERIFY OK |
| 2 | s24-26 | 21.9 | 32.5 (21.9 / 10.5) | 12.3 | 26.0 | 58.5 | 80.44 | identical, VERIFY OK |
| 3 | s24-26 | 20.1 | 32.6 (22.0 / 10.4) | 12.2 | 26.0 | 58.7 | 78.81 | identical, VERIFY OK |
| 4 | s24-26 | 23.5 | 32.6 (21.9 / 10.5) | 12.3 | 26.1 | 58.7 | 82.19 | VERIFY OK (the job's time limit cut the `cmp`) |
| 5 | s24-26 | 23.1 | 32.6 (22.0 / 10.5) | 12.2 | 26.0 | 58.6 | 81.71 | identical, VERIFY OK |
| 6 | s24-26 | 20.5 | 33.0 (22.3 / 10.5) | 12.4 | 26.3 | 59.3 | 79.77 | identical, VERIFY OK |
| planes off on the same node (`RNS_PLANES_3Q30=0`) | s24-26 | 18.9 | 34.9 | 13.2 | 28.1 | 63.1 | 81.99 | identical, VERIFY OK |

**Wall 80.8 ± 1.3 s, phases 58.8 ± 0.3, init 22.1 ± 1.3** (six runs) against the Phase 11 close (81.5 ± 1.4, phases 66.0 ± 0.4, init
15.5 ± 1.1) and against this branch with the planes off (82.0 ± 0.3, phases 63.8–64.8, init 17.3–18.9): **the phases are
5–7 s faster and the wall 1 s**, all of the phase gain being M11's layout (−2 s: no in-phase mapping) plus the 3·2^k planes
(−5 s), and all of the init cost being the 60 GB they map.

#### Init before / after (4 × 10¹⁰)

| | Phase 11 close (P's gate, s24-26) | this branch, planes off (s24-16/30) | this branch, planes on (default) |
|---|---:|---:|---:|
| staging + contexts | 1.0–1.1 | 1.0–1.2 | 0.9 |
| arenas (132.3 GB) | 4.8–5.3 (95.7 GB) | 5.8–7.5 | 7.3 |
| plane pools | 12.4–15.7 (120 GB, the arenas' time included in P's line) | 8–9 (120 GB) | 13.4 (180 GB) |
| peer access | 0.3 | 0.3–0.6 | 0.7 |
| init | 15.5 ± 1.1 | 16.0–18.2 | 22.5 |
| phases / wall | 66.0 / 81.5 | 63.8–64.8 / 82.0 ± 0.3 | 59.2 / 81.7 |

(The arenas are 132.3 GB since M11 against P's 95.7: the tail layout's 36.6 GB of dm extra, which bought the zero in-phase
growth; the plane pools' 8–9 s is the same 120 GB at the same rate as P's once the arenas are taken out of P's "pools"
figure.) The floor is 0.06–0.075 s/GB × the bytes mapped at init — 252 GB → 15–16 s, 312 GB with the planes → 21 s — and
the seeds fit inside it. The bytes are M's (sized to the phases' need with zero growth); the rate is the driver's.

#### Open issues

* The mapping rate is the kernel's page clearing on one core under one lock; a driver that clears pages on the SDMA engines,
  or hands out pre-cleared pages from a pool, would be the fix — not ours. `amdgpu`'s TTM page pool keeps freed pages (the
  0.035 s/GB of a re-allocation) but a fresh process starts empty.
* `hipMallocAsync` / `hipMemPool`: the warm re-allocation faulted on this ROCm; not chased.
* The seeds' two buffered chunks (the first 4 GB, computed before the regions exist) wait 1.4–1.8 s behind the runtime's lock
  to issue their DMAs; harmless inside init's wall, gone if the arenas were mapped before the seed thread starts (then the
  first chunks wait 7 s for the regions instead — worse).
* With the planes on, init is 22.5 s of which the seed thread is 21 s: any further cut of the mapping would expose the
  seeds (14 s of CPU work, 8.5 of spans) — the next item after the mapping is the seeds' own rate, not measured here.
* `t_alloc`'s host forms show the CPU storing into host-registered memory at 30 GB/s against 2.6 into device memory: a
  region for the seeds in host memory would make their stores free, but the region is then read by the batch tier at HBM
  speed only from its own APU (same as now) and copied at 21 GB/s by any `hipMemcpy` — not tried.

## K8 - init and the seed wait at one node: where the time goes, and why reordering does not help (2026-10-12)

(former file: K8.md)

Task K8 (OPPORTUNITIES B.3; sources S38, Q18 9b; coordinator input from O7b). Branch `k8` off main a448d73, not merged.
Labels: measured (m) / modelled (mod) / assumed (a). Times Eastern. All runs on aac6 PPAC_MI300A_SPX, one node, one run per
arm unless stated, `ECALC_INIT_TL=1 ECALC_VERBOSE=2`, no digit file, every run VERIFY OK. Raw logs stay on aac6 (`~/k8/`).

**Result: diagnosis only, no switch proposed.** Three ways to reorder or parallelise init were built and timed behind switches
(off by default). All three were slower than the default at 6.441e10. The clearest one, starting the seeds earlier, was
11.5 s slower there (m, one run). So the code on this branch is unchanged. The patches are kept in the appendix below so the
runs can be repeated. With this window's binding resource (below), no saving of 1 s or more from reordering looks
plausible. The ABBA driver was therefore not armed, as the task allows.

#### 1. Where the time goes (default, main a448d73)

Times are measured on the init timeline, from the start of `rns_init`. "Level 1" is the point where bs level 1 starts
(init + seed wait = the GPU-idle window).

| step | 1e10 (prof1) | 6.441e10 (p64 default, two runs) |
|---|---|---|
| staging + contexts (pinned staging 9.3 GB, ntt contexts) | 1.5-2.4 s | 1.1-2.6 s |
| plane pools (hipMalloc + memset, 103 GB, 4 APUs in parallel) | 4.1-5.6 s (18-25 GB/s) | 4.1-6.0 s |
| region arenas, parity-0 half (VMM, 4 APUs in parallel, no mapper lock) | 2.1 s (13 chunks, 26 GiB) | 7.0 s (39 chunks, 78 GiB) |
| peer access | 0.1-0.25 s | 0.1-0.3 s |
| = RESULT init | 8.3-10.4 s | 14.1-14.5 s |
| background mapping of the parity-1 halves (one chunk at a time, `g_vmm_map_mx`) | 13 chunks, 2.6-2.8 s, 0.4-2.3 s queued per APU | 45 chunks, 9.4-11.5 s, 4-8 s queued per APU |
| seed thread (starts after the plane pools) | 5.3-5.9 s: buffers 0.6, spans 2.3, DMA issue 2.1-2.4 | 18.7 s: buffers 1.5, spans 11.8, DMA issue 3.1 |
| bs waiting for the seeds | 2.8-3.3 s | 10.2-11.5 s |
| level 1 starts | 11.3-13.8 s | 24.8-26.5 s |
| total | 23.2-25.6 s | 73.0-74.7 s |

What binds (m, from the timelines):
- **The seeds and the background mapping finish together** ("co-bind"). At 6.441e10 the last background mapper ended at
  25.9 s and the seed thread at 26.2 s. At 1e10 APU0's mapper ended at 11.1 s and the seeds at 11.2 s. Level 1 needs
  both: it waits for the parity-1 halves, and on APU0 that is the whole arena.
- **The seeds' DMA issue time is the contention.** `hipMemcpyAsync` (for VMM targets, the copy kernel) blocks while
  another thread is inside a HIP memory call. Issue time is 3.1 s with the background mappers running. It is 0.24 s with
  `ECALC_VMM_BG=0` (no mappers; same 8 chunks of 8 GiB). With `RNS_PLANES_FIRST=0` it is 10.1 s, and one issue took
  8.3 s behind the plane pools' hipMalloc/hipMemset.
- **The device-memory mapping rate is driver-bound and does not scale with threads.** The VMM arenas take about
  11-13 GiB/s with four APUs in parallel and about 8 GiB/s for the one-at-a-time background mappers. Hard evidence: with
  the fill switch below, four parallel threads mapped no faster in total than two. hipMalloc for the plane pools runs at
  18-25 GB/s. The whole init window maps about 159 GB (1e10) or about 280 GB (6.441e10) of device memory.
- Modelled floor at 6.441e10 (mod): staging 2 s + planes 5 s + arenas about 13 s at the parallel VMM rate gives about
  20-21 s before level 1 can start, against 24.8-26.5 s measured. This leaves 4-5 s, and only if the seeds could finish
  inside the mapping without using the HIP runtime. That is the coordinator's "5-8 s" window (O7b) seen from the mapping
  side. The seeds cannot use it, because each seed chunk needs a region to land in (2 pinned buffers only) and a DMA that
  queues behind the mapping.

#### 2. Levers tried (behind switches, off by default; patches in the appendix)

| lever | switch (patch) | 1e10, level 1 | 6.441e10, level 1 | 6.441e10 total | verdict |
|---|---|---|---|---|---|
| default | - | 13.84 s (paired run) | 26.46 / 24.78 s | 74.7 / 73.0 s | |
| **seeds earlier**: the hook (region arenas + seed thread) runs in its own thread, concurrently with the plane pools, from staging end | `RNS_HOOK_CONCUR=1` (`rns_hook_concur.patch`) | 13.25 s (-0.6, one pair: noise level) | **37.98 s (+11.5)** | 96.3 s (+21.6) | rejected. hipMemCreate waits behind the planes' hipMalloc (arena creates 5-8 s); the seeds stall 6.2 s for the regions, then their DMA issue takes 9.7 s behind the mappers; level 1 also waits 0.5-2.5 s for arena chunks |
| seeds earlier, regions before the planes (existing knob) | `RNS_PLANES_FIRST=0` | - | 28.96 s (+2.5) | 77.5 s (+2.8) | worse: the planes take 14.7 s with the seed DMAs contending, one DMA issue blocks 8.3 s |
| balance the parallel arena phase: an APU whose parity-0 half is done maps further chunks while another still has 2 or more to go | `ECALC_VMM_FILL=1` (`vmm_fill.patch`) | 5.91 s vs 5.28 s from the end of the plane pools to level 1 (+0.6, worse) | 28.13, 29.04 s vs 24.78 s (+3.4, +4.3; ABA) | 76.2, 77.3 vs 73.0 s | rejected. The driver does not map faster in parallel, so the extra chunks lengthen the parity-0 phase (7 s to 12 s) and the seeds wait 7.5 s for their regions |
| bigger VMM chunks (existing knob) | `DB_POOL_VMM_CHUNK_GB=8` | - | 28.43 s (+2.0) | 77.2 s (+2.5) | worse |
| no background mapper (existing knob) | `ECALC_VMM_BG=0` | - | 30.06 s (+3.6) | 77.5 s (+2.8) | worse in the window (seeds wait 7.75 s for the regions), though the DMA issue drops to 0.24 s |
| no VMM arenas (existing knob) | `DB_POOL_VMM=0` | - | 30.56 s (+4.1) | 79.3 s (+4.6) | worse (arenas by hipMalloc at about 20 GB/s, but all synchronous: seeds wait 8.8 s) |

The differences above are 2.0-11.5 s and the default runs vary by 1.7 s (level 1 at 24.78 and 26.46 s). So all the
6.441e10 verdicts except the 1e10 pairs hold without an ABBA. The only arm that was not worse,
`RNS_HOOK_CONCUR` at 1e10 (-0.6 s), reverses sign at 6.441e10, so it was not taken to an ABBA. Digits: every run VERIFY OK.
Neither switch alters any byte (same allocations and addresses), but neither went through `gate.sh`, since nothing is
proposed.

#### 3. What could still work (not built here)

1. **Seed DMAs that need no HIP runtime call.** Example: seeds written by CPU stores into memory the GPU can read
   directly (hipHostRegister'd or XNACK-enabled host memory, picked up by level 1's kernels), or one copy kernel launched
   once and fed through a host flag. The seeds could then finish inside the mapping window, worth about 4-5 s at
   6.441e10 (mod). This needs a design for the VMM regions (a CPU store into a VMM range faults). Size: M, engine-adjacent.
2. **Fewer bytes mapped before level 1.** The parity-1 halves (90 GiB at 6.441e10) are first used by level 1's outputs.
   Mapping them lazily per level, rather than all before level 1, would take about 8 GiB/s × the deferred bytes off the
   window, in proportion to what level 1 does not need (mod; needs the level-1 output footprint per APU). This interacts
   with the background mapper `vmm_bg_map` (TASKS 1, the deferred segfault); do it only after G1.
3. The O2 finding (8 GiB of bounce buffers pinned on the first host copy, about 2.2 s) is outside this window at one node.
   The first host copy comes after bs, so `DBIG_SMALL_BOUNCE` does not interact with K8 (inferred from the timelines).

Payoff on aac7 (S38: 3-11 s queued for the mapper lock at 6.441e10) is the same mechanism. The aac7 numbers were not
re-measured here.

#### 4. Tests (aac6, 2026-10-12 02:15-03:10 ET, own hold, cancelled)

- Build: a448d73 clone by full bundle, `make -s -j16` with `module load rocm`, rc 0, for each patch.
- 1e10: `SLURM_JOB_ID=<hold> ./mnrun.sh 1 env ECALC_INIT_TL=1 ECALC_VERBOSE=2 [switch] ./ecalc 10000000000`: prof1 (default),
  `ECALC_VMM_FILL` 1/0, `RNS_HOOK_CONCUR` 1/0. Total 23.2-25.6 s.
- 6.441e10: the same with `64410000000`: default (two runs), `RNS_HOOK_CONCUR=1`, `RNS_PLANES_FIRST=0`, `DB_POOL_VMM_CHUNK_GB=8`,
  `ECALC_VMM_BG=0`, `DB_POOL_VMM=0`, `ECALC_VMM_FILL` 1/0/1.
- No `gate.sh` and no ABBA: nothing is proposed for adoption.

#### RESUME

- Branch `k8`: this file and the two patches in the appendix. No `ecalc/` change.
- aac6: the hold was cancelled and the clone `~/ntt-k8` removed. Logs are in `~/k8/` (delete them when no longer needed).
- Next, if wanted: option 1 or 2 of section 3, as a new task with a design step first. K8 as a reordering lever is closed.

#### Appendix: the two patches (against a448d73; not applied on this branch)

##### RNS_HOOK_CONCUR (ecalc/rns_mul.c)

```diff
diff --git a/ecalc/rns_mul.c b/ecalc/rns_mul.c
index 89fc588..c1c170b 100644
--- a/ecalc/rns_mul.c
+++ b/ecalc/rns_mul.c
@@ -140,6 +140,7 @@ static void rns_acct(int ndev, size_t b[][MEM_DEV_NCAT])
     for (int d = 0; d < ndev && d < EC_NP; d++) { b[d][MEM_DEV_PLANES] += D[d].da.cap + D[d].db.cap; b[d][MEM_DEV_TABLES] += g_tables[d]; }
 }
 static size_t dev_used(int d) { size_t f = 0, t = 0; int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d)); if (hipMemGetInfo(&f, &t) != hipSuccess) f = t = 0; HIP_CHECK(hipSetDevice(cur)); return t - f; }
+static void *rns_hook_thread(void *a) { (void)a; HIP_CHECK(hipSetDevice(0)); rns_after_staging_hook(rns_hook_arg); db_tl("the hook (region arenas, the seed thread started) done"); return 0; }   /* K8: RNS_HOOK_CONCUR=1 */
 int rns_init(int pool_log)
 {
     if (g_nd) return g_nd;
@@ -178,7 +179,14 @@ int rns_init(int pool_log)
      * transform of the dist tier) are then the victims and the tier's gathers and transforms run 4-10 x slower
      * (results/N414.md).  Mapped first, they take 2 MiB blocks; the seeds start after them (default 0: the hook first). */
     int planes_first = getenv("RNS_PLANES_FIRST") ? atoi(getenv("RNS_PLANES_FIRST")) : 1;   /* default 1 since Phase 14 (N414: the planes get the 2 MiB pages) */
-    if (rns_after_staging_hook && !planes_first) rns_after_staging_hook(rns_hook_arg);     /* Phase 8 I2: the seeds start now, during the pool allocations below */
+    /* K8 (results/MEMORY.md §K8): RNS_HOOK_CONCUR=1 -- the hook (binsplit_seeds_begin: the seed thread, the region arenas' parity-0 halves) runs
+     * in a thread of its own from here, concurrently with the plane pools below (joined after them), instead of after them (RNS_PLANES_FIRST=1)
+     * or before them (0).  The seeds start about the plane pools' time earlier, and the arenas' VMM mapping (hipMemCreate) overlaps the
+     * planes' hipMalloc.  Same allocations, same sizes, same addresses within each range: digits identical.  Default 0 (off). */
+    int hook_concur = rns_after_staging_hook && par && getenv("RNS_HOOK_CONCUR") && atoi(getenv("RNS_HOOK_CONCUR")) == 1;
+    pthread_t hook_th;
+    if (hook_concur) { if (pthread_create(&hook_th, 0, rns_hook_thread, 0)) { ec_fatal(EC_RC_FATAL, "rns_init: pthread_create (RNS_HOOK_CONCUR)\n"); } db_tl("the hook runs concurrently with the plane pools (RNS_HOOK_CONCUR=1)"); }
+    else if (rns_after_staging_hook && !planes_first) rns_after_staging_hook(rns_hook_arg);     /* Phase 8 I2: the seeds start now, during the pool allocations below */
     size_t b1 = rns_pool1_bytes_req ? rns_pool1_bytes_req : bytes;   /* C4: pool 1 sized to the dist tier's 3 q when the flow is all-device (the host mdev tier needs the full 2^pool_log) */
     if (getenv("RNS_POOL1_GB")) b1 = (size_t)(atof(getenv("RNS_POOL1_GB")) * 1e9);
 #pragma omp parallel for num_threads(g_nd) schedule(static) if(par)
@@ -191,7 +199,8 @@ int rns_init(int pool_log)
         dpool_get_exact(&D[d].db, d, b1);
     }
     db_tl("plane pools done (%.2f s)", mem_now() - ti1);
-    if (rns_after_staging_hook && planes_first) { HIP_CHECK(hipSetDevice(0)); rns_after_staging_hook(rns_hook_arg); db_tl("the hook (region arenas, the seed thread started) done"); }   /* N4: the region arenas and the seeds after the plane pools */
+    if (hook_concur) { pthread_join(hook_th, 0); db_tl("the hook (region arenas, the seed thread started) joined after the plane pools (RNS_HOOK_CONCUR)"); HIP_CHECK(hipSetDevice(0)); }
+    else if (rns_after_staging_hook && planes_first) { HIP_CHECK(hipSetDevice(0)); rns_after_staging_hook(rns_hook_arg); db_tl("the hook (region arenas, the seed thread started) done"); }   /* N4: the region arenas and the seeds after the plane pools */
     mem_par_init = 0;
     mem_acct_register(rns_acct);
     double ti2 = mem_now();
```

##### ECALC_VMM_FILL (ecalc/dbig.c)

```diff
diff --git a/ecalc/dbig.c b/ecalc/dbig.c
index 702dca2..e53f6d6 100644
--- a/ecalc/dbig.c
+++ b/ecalc/dbig.c
@@ -268,6 +268,15 @@ static void *vmm_spin(void *arg)
 }
 static int g_vmm_go;                                   /* the background mapping starts when init's plane pools are allocated (db_vmm_bg_release from rns_init), so that the seeds get their half first and the pools their turn */
 void db_vmm_bg_release(void) { db_tl("the background mapping is released"); pthread_mutex_lock(&g_pool_mx); g_vmm_go = 1; pthread_cond_broadcast(&g_vmm_cv); pthread_mutex_unlock(&g_pool_mx); }
+/* K8 (results/MEMORY.md §K8): ECALC_VMM_FILL=1 -- the synchronous arena mapping (the four APUs in parallel, db_vmm_arena_alloc) is bound by the
+ * largest arena's parity-0 half (APU0's); the others finished early and left their parity-1 chunks to the background mappers, which
+ * run one at a time (g_vmm_map_mx) during the seeds' DMAs.  With the switch an APU that has mapped its parity-0 half maps its next
+ * chunks in the calling thread while another APU still has at least one parity-0 chunk not started (g_fill_left[d] >= 2: the one in
+ * progress plus one more), so the waiting time maps chunks and fewer go to vmm_bg_map.  Same chunks, same addresses: digits
+ * identical.  vmm_bg_map itself is unchanged.  Default 0 (off). */
+static int vmm_fill(void) { static int f = -1; if (f < 0) f = getenv("ECALC_VMM_FILL") ? atoi(getenv("ECALC_VMM_FILL")) : 0; return f; }
+static int g_fill_left[DB_NQ];   /* the parity-0 chunks of APU d not yet mapped (ECALC_VMM_FILL) */
+static int vmm_fill_others_busy(int dev) { for (int d = 0; d < DB_NQ; d++) if (d != dev && __atomic_load_n(&g_fill_left[d], __ATOMIC_SEQ_CST) >= 2) return 1; return 0; }
 static int vmm_vb(void) { static int vb = -1; if (vb < 0) vb = getenv("DB_POOL_VERBOSE") ? atoi(getenv("DB_POOL_VERBOSE")) : (getenv("RNS_VERBOSE") ? 1 : 0); return vb; }
 static int vmm_map_run(int d, int slot0, int m, hipMemGenericAllocationHandle_t *hs)   /* hs[i] (0: create one) mapped at slot0 + i, access for every APU */
 {
@@ -358,7 +367,14 @@ void *db_vmm_arena_alloc(int dev, size_t bytes, size_t first)   /* the arena of
     v->m0 = m0; v->bytes = bytes; v->mapped = 0; v->bg_on = 0;
     v->mark_m = first ? (int)((2 * first + C - 1) / C) : m0; if (v->mark_m > m0) v->mark_m = m0;   /* Phase 15 MAP (the timeline): binsplit.c's arena_get passes the parity half as `first`; level 1 waits for 2 first */
     double tm = mem_now(); db_tl("APU%d arena: mapping chunks 0..%d of %d (the parity-0 half)", dev, m1 - 1, m0);
-    if (!vmm_map_run(dev, 0, m1, v->h)) mem_oom("db_vmm_arena_alloc (chunks)", dev, bytes);
+    if (!vmm_fill()) { if (!vmm_map_run(dev, 0, m1, v->h)) mem_oom("db_vmm_arena_alloc (chunks)", dev, bytes); }
+    else {   /* K8: ECALC_VMM_FILL=1 -- one chunk at a time; once this APU's parity-0 half is mapped, keep mapping its next chunks here while another APU still has one not started */
+        __atomic_store_n(&g_fill_left[dev], m1, __ATOMIC_SEQ_CST);
+        for (int k = 0; k < m1; k++) { if (!vmm_map_run(dev, k, 1, &v->h[k])) mem_oom("db_vmm_arena_alloc (chunks)", dev, bytes); __atomic_sub_fetch(&g_fill_left[dev], 1, __ATOMIC_SEQ_CST); }
+        int mf = m1;
+        while (m1 < m0 && vmm_fill_others_busy(dev)) { if (!vmm_map_run(dev, m1, 1, &v->h[m1])) mem_oom("db_vmm_arena_alloc (chunks)", dev, bytes); m1++; }
+        if (m1 > mf) db_tl("APU%d arena: %d more chunks (%d..%d) mapped while another APU's parity-0 half was mapping (ECALC_VMM_FILL)", dev, m1 - mf, mf, m1 - 1);
+    }
     for (int r = 0; r < vmm_repro(); r++) for (int k = 0; k < m1; k++)   /* S46: ECALC_VMM_REPRO=n also here (the synchronous part; all of it under ECALC_VMM_BG=0): n more unmap + map + access cycles per chunk, zeroed below */
         if (hipMemUnmap(v->base + (size_t)k * C, C) != hipSuccess || !vmm_map_run(dev, k, 1, &v->h[k])) mem_oom("db_vmm_arena_alloc (ECALC_VMM_REPRO remap)", dev, C);
     double tz = mem_now();
```

## K8b - seed transfers without the HIP runtime inside init's mapping window (2026-10-12)

(former file: K8b.md)

Task K8b (follow-up to K8, results/MEMORY.md §K8; coordinator input from O7b). Branch `k8b` off main a448d73, not merged.
Labels: measured (m) / modelled (mod) / assumed (a). Times Eastern. All runs on aac6 PPAC_MI300A_SPX, one node (4 APUs),
ROCm 7.2.4. Raw logs stay on aac6 (`~/k8b/`).

#### 0. Summary (probe done; gate + ABBA armed, see RESUME)

- The probe reproduces the queueing and refutes most of the modelled 4-5 s: (1) with the mappers running, `k_copy_h2d`'s
  launches block 0.69 s in all (the first launch on each APU 0.13-0.21 s, later ones 0) against 0.007 s with no mapper, and
  `hipMemcpy`/`hipMemcpyAsync` block 1.1-1.4 s; (2) **the mapping rate is 9.3-10.0 GiB/s whatever the seed thread does**
  (alone, with any transfer variant, and even with the four mappers in parallel: 9.0 GiB/s), so the background mapping of
  the 90 GiB parity-1 halves is a floor of ~9.5-11 s after its release at 14.5 s, i.e. level 1 cannot start before ~24-25.5 s
  at 6.441e10 (mod); the K8 default started it at 24.8-26.5 s. (3) The persistent pull kernel makes the transfers free of
  runtime calls (issue 0.000 s, same completion time as the copy kernel, content verified) and its four launches cost 0.38 s
  even under the mappers.
- Expected gain of `BS_SEED_PULL=1` at 6.441e10 (mod): the seed thread ends ~3-3.5 s earlier (issue 3.1 s + DMA wait 0.5 s),
  but level 1 then waits for APU2's parity-1 half (25.38 s in the K8 run), so level 1 moves by ~0.5-1 s, not 4-5 s. The
  ABBA measures it (primary: level-1 start; secondary: total).

#### 1. The question

K8 found that at 6.441e10 bs waits 10.2-11.5 s for the seed thread, whose 8 copies of 8 GiB (pinned host buffer -> VMM
arena) cost 3.1 s of *issue* time (inside `db_copy_h2d_async`: `hipSetDevice`, `hipHostGetDevicePointer`, the launch of
`k_copy_h2d`) because every HIP runtime call queues behind the four background VMM mappers (`vmm_bg_map`: `hipMemCreate` +
`hipMemMap` + `hipMemSetAccess` + `hipMemsetAsync` per 2 GiB chunk, one at a time under `g_vmm_map_mx`). With no mapper the
issue time is 0.24 s. The lever is to make the seed transfers need no runtime call at all inside the window.

The K8 default-run timeline at 6.441e10 (m, `~/k8/p64_RNS_HOOK_CONCUR_0.log`), the reference for everything below:

| t (s) | event |
|---|---|
| 7.38 | plane pools done; 7.45 the seed thread starts |
| 7.94-14.40 | region arenas' parity-0 halves mapped synchronously (4 APUs in parallel, 39 chunks of 2 GiB, 78 GiB: 12 GiB/s) |
| 8.94 | the seed buffers registered (1.50 s for 2 x 8 GiB `hipHostRegister` under the parallel mapping) |
| 12.74 | 2 seed chunks computed; waits 1.66 s for the region pools |
| 14.53 | the background mapping is released (45 chunks, 90 GiB, one at a time) |
| 16.86 / 18.77 / 20.95 / 25.38 | parity-1 half mapped on APU1 / APU3 / APU0 / APU2 |
| 22.02 / 22.46 / 22.97 / 25.87 | mapper done on APU3 / APU1 / APU0 / APU2 (11-12 chunks each: create 2.1-2.3 s, access 0.6-0.7 s, queued 4.2-7.9 s) |
| 25.55 | every seed span computed (spans 11.80 s; issue 3.10 s, max 0.95; DMA wait 0.53 s) |
| 26.16 | seed thread ends (18.72 s); 26.46 bs level 1 starts |

Two things bind together: the seed thread (ends 26.16) and the last parity-1 half (APU2, 25.38). The serialised background
mapping maps 90 GiB in 11.3 s (8 GiB/s) regardless of the seeds, so removing the seeds' 3.1 s of issue time alone moves
level 1 by at most ~1 s unless the mappers also run faster without the seed thread's runtime calls in their way - which the
probe measures (mapper rate with and without each seed variant).

#### 2. The micro-probe (`tools/k8b_probe.cpp`, `tools/k8b_probe_driver.sh`)

A standalone HIP program that reproduces the window: four mapper threads (one chunk of 2 GiB at a time under a mutex,
12 chunks per APU = 96 GiB, exactly the `vmm_bg_map` sequence) released at t = 0, while the main thread plays the seed
thread: 8 chunks of 8 GiB, each filled (the "compute", ~1 s) into one of two pinned host buffers (posix_memalign + huge
pages + `hipHostRegister`, as `mem_hstage_alloc`) and transferred to pre-mapped device memory (chunk c to APU c % 4),
then waited for. Every variant verifies the device content (checksum kernel per APU against the host's sum).

| variant | the transfer | wait |
|---|---|---|
| `kern` | ecalc's default `db_copy_h2d_async`: `hipGetDevice`/`hipSetDevice`, `hipHostGetDevicePointer`, `k_copy_h2d` launched on a pre-created non-blocking stream, `hipSetDevice` back (each timed) | `hipStreamSynchronize` |
| `kern0` | the launch alone (device pointer and stream resolved before the window) | `hipStreamSynchronize` |
| `masync` | `hipMemcpyAsync` H2D on the pre-created stream into the VMM range | `hipStreamSynchronize` |
| `masyncm` | the same into `hipMalloc`'d memory (the SDMA path) | `hipStreamSynchronize` |
| `memcpy` | `hipMemcpy` (synchronous) into the VMM range | - |
| `pull` | a persistent kernel per APU (456 blocks) launched before the window; a copy = descriptor + release store of `seq` in coherent pinned host memory; the kernel copies and adds to `done` | host spin on `done` |
| `pull_late` | the same, the four kernels launched after the mappers started (the launch cost under contention) | host spin |
| `cpustore` | CPU `memcpy` into `hipMalloc`'d memory (ecalc's `DB_POOL_VMM=0` path; a VMM range faults) | - |
| `none` | the mappers alone | - |

Plus: `NOMAP=1` (the variant alone, no mappers), `REG_LATE=1` (the two buffers registered after the mappers start, as
ecalc does), `MAP_PAR=1` (the mappers without the mutex: how fast the driver maps in parallel).

#### 3. Probe results (m; aac6 hold 22990, 03:22-03:29 ET; `~/k8b/probe_summary.txt`, per-run logs `~/k8b/probe/`)

Mappers: 48 chunks of 2 GiB (96 GiB). Seeds: 8 x 8 GiB. "issue" = seconds inside the transfer calls, summed over the 8 chunks
(max per call); "wait" = seconds waiting for completion; "ended" = when the seed side finished (window time). All VERIFY OK.

| variant | mappers (GiB/s) | issue s (max) | of which setdev / getptr / launch | wait s | seeds ended s |
|---|---|---|---|---|---|
| none (mappers alone) | 9.61 (10.0) | - | - | - | - |
| kern (ecalc default) | 9.86 (9.7) | 0.690 (0.214) | 0.000 / 0.000 / 0.690 | 0.543 | 1.68 |
| kern, NOMAP | - | 0.007 (0.002) | 0 / 0 / 0.006 | 0.542 | 1.08 |
| kern, REG_LATE (buffers registered under the mappers: 1.25 s, = 1.24-1.27 s without them) | 10.12 (9.5) | 0.547 (0.204) | 0 / 0 / 0.546 | 0.377 | 2.44 |
| kern0 (launch on another APU's stream without hipSetDevice) | crash: `invalid resource handle` (so hipSetDevice is required per launch) | | | | |
| masync (hipMemcpyAsync, VMM dst) | 10.04 (9.6) | 0.308 (0.108) | | 1.199 | 2.37 |
| masync, NOMAP | - | 0.098 (0.026) | | 1.188 | 2.14 |
| masyncm (hipMemcpyAsync, hipMalloc dst) | 9.88 (9.7) | 1.090 (0.315) | | 1.189 | 2.86 |
| memcpy (hipMemcpy, VMM dst) | 9.90 (9.7) | 1.416 (0.296) | | 0 | 2.12 |
| **pull** (persistent kernel, launched before) | 9.86 (9.7) | **0.000** | | 0.596 | **1.23** |
| pull, NOMAP | - | 0.000 | | 0.563 | 1.14 |
| pull_late (the 4 launches under the mappers: 0.38 s) | 9.62 (10.0) | 0.000 | | 0.463 | 1.12 |
| cpustore (CPU memcpy into hipMalloc memory) | 10.36 (9.3) | 177.7 (22.6) | | 0 | 178.5 |
| none, MAP_PAR (mappers without the mutex) | 10.69 (9.0); per-APU create 5.8 map 2.1 access 1.3 | | | | |
| kern, MAP_PAR | 9.78 (9.8) | 2.637 (0.880) | 0 / 0 / 2.637 | 0.382 | 3.28 |

Reading:
- The launch is what blocks (setdev and getptr 0): on each APU the *first* launch under the mappers blocks 0.13-0.21 s
  (kern: chunks 0-3 issue 0.214 / 0.181 / 0.165 / 0.129, chunks 4-7 0.000), i.e. the per-device first-launch work
  (code object load) waits for the runtime's lock; later launches do not. Without the mutex (MAP_PAR) every launch blocks
  (2.64 s, max 0.88): that is why `g_vmm_map_mx` exists. The ecalc window is harsher than the probe (3.1 s over 8 launches,
  max 0.95: the seed thread's launches there also compete with 96 span threads and the hipHostRegister of 16 GiB), but the
  mechanism is the same and the pull kernel removes it entirely.
- hipMemcpyAsync and hipMemcpy are no better: 0.3-1.4 s of issue, and hipMemcpyAsync completes slower (1.2 s wait: ~55 GB/s
  against ~110 GB/s for the kernels). CPU stores into hipMalloc memory run at 0.37 GB/s: not a path.
- The mapping rate is the driver's, 9-10 GiB/s, and is the same with four mappers in parallel (then the time moves from the
  mutex queue into hipMemCreate / hipMemMap / hipMemSetAccess themselves). So the seeds do not slow the mapping, and
  nothing on the seed side can move the mapping's end.
- Registering the two 8 GiB buffers costs 1.25 s with or without the mappers (first-touch and pinning of 16 GiB), so the
  1.50 s K8 saw is not contention.

#### 4. The switch: `BS_SEED_PULL=1` (off by default)

`dbig.c`: `k_pull`, `db_pull_start_all` / `db_pull_resolve` / `db_pull_copy_async` / `db_pull_wait` / `db_pull_stop_all`;
`binsplit.c`: `seeds_stream` picks the pull functions instead of `db_copy_h2d_async` / `db_copy_h2d_wait` when the switch is
on (VMM arenas only), resolves the two buffers' device pointers right after registering them, and stops the kernels before
unregistering them; `binsplit_seeds_begin` launches the kernels right after `seed_stream_pools` (main thread, the parity-0
halves mapped, the background mapping not yet released: nothing contends with the four launches). The copies are the same
bytes to the same addresses as before: digits identical. One row in `ecalc/README.md`; registry regenerated
(`tools/switch_registry.py --check`: 0 problems).

#### 5. Gate and ABBA (`tools/k8b_driver.sh`, `tools/k8b_stats.py`)

Collected 2026-10-12 (hold 22999 ran 04:05-04:06 ET; marker `K8B_DONE`: **FAILED**, measured).

- Gate (1 node, 1e9, `BS_SEED_PULL=1`): **PASS** (rc 0, VERIFY OK, digits identical, segv 0, ecalc total 10.48 s, run 16 s).
- The driver nevertheless aborted: its check `grep -q "^PASS" gate/gate.txt` does not match the line, which starts with `gate PASS`. A driver bug, not a gate failure. The hold was cancelled at once (steps left 0), so **the ABBA at 6.441e10 never ran** (`pairs.tsv` has only the header). No timing result for `BS_SEED_PULL`; the 0.5-1 s expectation stays modelled.
- Fix needed in `tools/k8b_driver.sh` (match `gate PASS`, e.g. `grep -q "gate PASS"`), then re-arm one hold for the ABBA (skip the gate with `SKIP_GATE=1`). No hold or driver of k8b is queued or alive now.

#### 6. K8c: the ABBA hang and its fix (2026-10-12)

Second ABBA (hold 23008, 04:21-04:44 ET, commit 228aa93, gate skipped; `~/k8b/summary.txt`, `~/k8b/log/`): **FAILED, 2 failed runs** (m).

| run | arm | rc | result |
|---|---|---|---|
| r1_A1 | off | 0 | VERIFY OK, level 1 27.65 s, total 76.62 s |
| r1_B1 | on (`BS_SEED_PULL=1`) | 124 | killed by the driver's 600 s timeout |
| r2_A1 | off | 0 | VERIFY OK, level 1 29.29 s, total 78.69 s |
| r2_B1 | on | 124 | killed by the driver's 600 s timeout |

- **Which arm, how:** only the switch-on arm, 2 of 2, both a **hang** (no VERIFY FAIL, no segfault, no hipError, no OOM).
  Both B logs end the same way: `seed pull kernels launched on 4 APUs` (~13-15 s), init finishes, `bs: joining the seed
  thread`, all background mappings finish (~23-25 s), then nothing until slurm kills the step. The seed thread never prints
  `every span computed` (the off runs print it at ~27 s). Not environmental: the off arm passed both times on the same node
  in the same hold, and the background mapper had finished before the stall.
- **Cause (defect in the switch, `dbig.c`):** one descriptor per APU (`dst/src/n` + `seq`). `seeds_stream` keeps two
  buffered chunks and, once the regions exist, issues both back to back; at 6.441e10 (8 chunks, 2 per region) both go to the
  **same APU** (`dev = r % nd`). The second `db_pull_copy_async` overwrote the descriptor and stored `seq = 2` while the
  456 blocks were still asleep in `s_sleep` or copying copy 1. A block that first sees `seq = 2` jumps `my` from 0 to 2 and
  adds `done` once, so `done` stays below `2 x 456` and `db_pull_wait` spins forever (its 600 s fatal never fired because
  the driver's own 600 s timeout came first). Copy 1 is also partly lost (blocks read copy 2's descriptor). Neither the
  probe (one copy in flight per APU) nor the 1e9 gate (no two buffered chunks for one region, assumed from its `seeds 0.0`)
  reached this case.
- **Fix (commit b1ff573):** `db_pull_copy_async` first calls `db_pull_wait(dev)`, so a descriptor is overwritten only after
  every block finished the previous copy and `seq` advances by exactly one. The two copies to one APU were serial on its
  one stream before too, so only the split between issue and DMA wait changes. The off path is untouched (no change outside
  the `BS_SEED_PULL` functions): byte-identical to main.
- **Re-armed (05:48 ET, aac6 hold 23023, pending on priority; re-armed 05:51 ET with a 23:00 ET start deadline so the 45 min hold ends before the 00:00-03:30 ET aac6 quiet window):** `~/k8c/k8b_driver.sh` with `O=~/k8c NPRE=5 RUN_TMO=300`:
  gate (`--build`, 1e9, switch on), then **5 switch-on runs at 6.441e10 (each must give VERIFY OK, 300 s timeout)**, then
  ABBA rounds as in section 5 only if all 5 pass. Driver additions: `NPRE`, `RUN_TMO`, `O` (defaults keep the old behaviour).

#### RESUME (K8c re-armed 05:51 ET 2026-10-12; the notes below the first bullet are the original K8b ones)

- **K8c (current):** collect with `ssh aac6 'cat ~/k8c/K8B_DONE; cat ~/k8c/summary.txt; cat ~/k8c/pairs.tsv; cat ~/k8c/gate/gate.txt'`
  (gives up waiting for the hold at 23:00 ET 2026-10-12; hold 23023 cancels itself). Fill section 6 (5 pre-runs: level 1, total,
  VERIFY; ABBA table and stats) and section 0. Remove `~/ntt-k8b` when done; `squeue -s -j 23023` must show no step.

- Branch `k8b` (worktree `../ntt-wt-k8b`), commits ca28494 (switch + probe), b740ef7 (driver, stats), then this file.
- aac6: clone `~/ntt-k8b` at the branch head (built by `make -s -j16`; the driver rebuilds with `gate.sh --build`), probe binary
  and logs in `~/k8b/` (`probe_summary.txt`, `probe/*.log`).
- **Armed:** `~/k8b/k8b_driver.sh` (hold 22999, 45 min, PPAC_MI300A_SPX, queued behind our k6 and o5 holds: expected start
  ~05:00-06:00 ET, ~35 min of node time; it gives up at 12:00 ET). Steps: `tools/gate.sh --build --nodes 1 --hold --line none
  --env "BS_SEED_PULL=1"` (1e9 digits against `~/ntt/ecalc/results/e_1000000000.out`), then ABBA rounds A1 B1 B2 A2 at
  6.441e10 (A default, B `BS_SEED_PULL=1`; `ECALC_INIT_TL=1 ECALC_VERBOSE=2`, no digit file), stop after round >= 2 when
  p < 0.05 or the 95 % CI half-width <= 0.5 s on the level-1 start time, cap 4 rounds; `tools/k8b_stats.py` (paired t, no scipy).
- **Collect:** `ssh aac6 'cat ~/k8b/K8B_DONE; cat ~/k8b/summary.txt; cat ~/k8b/pairs.tsv; cat ~/k8b/gate/gate.txt'`.
  Then fill sections 0 and 5 of this file (gate line; per-round table; mean difference B - A with 95 % CI and p for level 1
  and total; the `seeds were computed` line of one A and one B run, which has the issue / DMA-wait split), commit, push.
  Verdict rule: adopt only if the level-1 (or total) difference is negative with p < 0.05; expected ~0.5-1 s (mod), bounded by
  the mapping floor. Remove `~/ntt-k8b` and the hold when done (`squeue -s -j 22999` must show no step).
- Not touched: `vmm_bg_map`, its mutex and the `ECALC_VMM_BG` defaults (TASKS 1 / G1).
- Beyond this task (mod, from the probe): the only lever left in the window is the mapping itself (90 GiB at 9-10 GiB/s after
  the plane pools), i.e. K8 section 3 option 2 (map fewer bytes before level 1: lazy parity-1 mapping), or starting the
  background mapping earlier; parallel mappers do not help (9.0 GiB/s).

## P3 -- shorten the start-up mapping phase: evaluated by reading code and A09, no change made (STOPPED)

(former file: P3.md)

Labels: measured (m) / modelled (mod) / assumed (a). Nothing was built or run; no jobs, no worktrees.

#### Starting point (m, archive:results/A09.md, 6.441e10 digits per node, 2 runs per node count)
GPUs idle until level 1 at 24.4 / 28.3 / 29.4 / 35.1 s on 1 / 2 / 4 / 6 nodes (32 / 20 / 16 / 12 % of the run). At 1 node the
seed thread binds (ends 24.0 s, last background chunk 23.0 s); at 2 and 4 nodes the mapping binds by 1.4-2.5 s. The two chains
are within about 1.5 s of each other, so any single fix is worth at most about 1.5 s (mod), both together about 3 s.

#### Ideas, by code reading (dbig.c db_vmm_arena_alloc / vmm_bg_map, binsplit.c seeds_stream / arena_get, rns_mul.c rns_init)
| idea | verdict | modelled gain at 1 node |
|---|---|---|
| overlap mapping with CPU binary splitting | already done: the seed thread computes spans during the background mapping | 0 |
| map lazily per pool on first use | already so for the parity-1 half (db_vmm_arena_wait before level 1) and the tail (waited only before donation); level 1 does not wait for the tail | 0 |
| larger mapping granularity | DB_POOL_VMM_CHUNK_GB exists; the driver page work is per GB and serialised (A09: lock-held time per chunk is flat), so no per-GB saving expected | ~0 |
| plane pools overlapped with the arena mapping | the order planes-first is deliberate (N414: late planes get small pages, 4-10x slower gathers) | unsafe |
| start GPU work on already-mapped pools | binary splitting is level-synchronous and level 1 needs every seed; a per-region start is a restructure of binsplit_e, not an init fix | not small |
| publish the parity-0 chunks one by one so the seed thread DMAs early (A09 6.3) | the only candidate: seeds idle 3.3 s at 1 node (10.1 -> 13.2 s). Needs: VA reserve split from mapping (the region pointers exist only after db_vmm_arena_alloc returns), per-chunk SetAccess and zeroing (zeroing the whole half at the end would erase early DMA data), and seed DMA launches while 4 mapper calls hold the runtime lock (the reason K8b and the pull kernels exist) | 1.0-1.5 (mod), ~0 at 2/4 nodes |

#### Why stop
- Best candidate saves about 1-1.5 s of a 76 s run at 1 node (1.3-2 %, mod), nothing at 2/4/6 nodes where mapping binds; the
  4-round ABBA budget cannot resolve a 1 s effect against run-to-run noise.
- It adds concurrent mapping and launch traffic in exactly the window where the hipMemMap crash and the lock-order deadlock lived.
- At the target the mapping is expected 7x faster (a, S22 3.1): the seed thread then does not wait and the whole phase is hidden
  (init floor 16.5 s), so the gain does not carry over.

#### If the user still wants it (not started)
Split db_vmm_arena_alloc into reserve / map-first-half with per-chunk publish (mapped counter, access, zero per chunk); have
seed_stream_pools publish the reserved pointers early and the seed DMA call db_vmm_arena_wait(dev, end of its chunk). Switch
ECALC_VMM_PUBLISH, default off. Gate: tools/gate.sh --build 1 and 2 nodes; 4 ABBA rounds with ECALC_INIT_TL=1.

#### RESUME
Nothing pending. No branch, worktree, switch or job exists for P3.

## T114 — Phase 14 agent T1: E10a, the early free in the multi-node tree (`MN_TREE_EARLY_FREE`)

(former file: T114.md)

Branch `p14-T1`, based on `p14-R1v` (5dbab07: main + DM_TIGHT + DB_POOL_VMM). Task: APUMULT_STUDY §4 E10 / results/DIVISION.md §R114 §4
(branch p14-R1): free the dead P_i / P_run shares between a tree level's two products; `tree_need_dev` counts
max(2c + 2r + n, c + r + 2n) instead of 2c + 2r + 2n; behind `MN_TREE_EARLY_FREE=1` (off by default).

#### 1. Design

* `mn.c tree_level_k` (k-way levels, Horner from the top child): after `P_n = P_i Q_run + P_run` (the first product) and before
  `Q_n = Q_i Q_run`, with the switch: my child's P share (`P->sh`, when I am in child i, or in the top child whose P is the
  not-yet-owned running P), and the owned running `Pr.sh`, are freed. The second product reads only Q_i, Q_run.
  The existing frees after the second product become no-ops for those (db_free of a zeroed dbig).
* `mn.c tree_level` (binary levels, nch = 2): the same — my child's P (P_A or P_B) freed between the two products. So the
  formula holds at every level (r = 0 there: max(2c + n, c + 2n)).
* `binsplit.c tree_need_dev`: live = max(2c + 2r + n, c + r + 2n) with the switch (c, r, n = quarter_bytes of the child's,
  running, new share + 1/8, as before); the scratch term unchanged. The switch is read in `dm_switches()` (global
  `mn_tree_early_free`, declared in binsplit.h). `BS_LAYOUT_ONLY` prints `early_free 0|1` at the end of the `layout:` line.
* `mem_model.py`: `tree_need_dev(..., early_free)`, `mem_per_node` opt `early_free`, `--check-c` reads `early_free` from the
  line; `--e10a` prints the 576 ceilings.
* README switch table: one row.

#### 2. Modelled 576 per-node ceilings (`python3 mem_model.py --e10a`, MODELLED)

Target switches (mem_model TARGET576: SHMEM resident staging, MDB_SHIFT_CHUNK_MB=1024, COMM_ALLTOALLV_DEPTH=2); "digits" =
576 × the per-node D (R1's convention divided by the top node's 1.036 — given in brackets).

| variant | early free | 480 GB per node | total | 502 GB per node | total |
|---|---|---|---|---|---|
| V0 | off | 8.09e10 | 4.66e13 [4.50e13] | 8.61e10 | 4.96e13 [4.79e13] |
| V0 | on | 8.09e10 (dm binds) | 4.66e13 | 8.61e10 | 4.96e13 |
| **DM_TIGHT=1** | off | 8.73e10 (tree binds) | 5.03e13 [4.85e13] | 9.34e10 | 5.38e13 [5.19e13] |
| **DM_TIGHT=1** | **on** | **9.39e10** (dm binds) | **5.41e13** [5.22e13] | **1.000e11** | **5.76e13** [5.56e13] |
| DM_TIGHT=1 MN_T_CHUNK_MB=1024 | off | 9.86e10 | 5.68e13 | 1.054e11 | 6.07e13 |
| DM_TIGHT=1 MN_T_CHUNK_MB=1024 | on | 1.064e11 | 6.13e13 | 1.133e11 | 6.53e13 |

With DM_TIGHT the early free is worth +7.6 % (480) / +7.1 % (502) per node; R1 modelled 5.18e13 (its V2 as a ratio); with the
real DM_TIGHT layout it is 5.22e13 in R1's convention.

#### 3. Tests

##### 3.1 The layout check (aac6 login node, no GPU; MEASURED = the C request, exact comparison)

`DM_TIGHT=t MN_TREE_EARLY_FREE=e BS_LAYOUT_ONLY=7.38e10:2,7.38e10:4,7.38e10:576,8.5e10:2,8.5e10:4,8.5e10:576 ./ecalc 1000000 /tmp/T114x`
for t, e in {0, 1}², then `python3 mem_model.py --check-c <file>`: **0 terms not exact** in all four (24 points × 11 terms);
files in `archive:results_raw_2026-10-19/T114/lay_t*_e*.txt`. (Fix needed in `--check-c`: its `key value` regex parsed `div 456 early_free` as the
value `e`; a value must now end at a non-word character.)

Per-device tree need and the node arena (the C lines):

| D per node | g | tree need / dev, early free off → on | arena / node, DM_TIGHT=0: off → on | arena, DM_TIGHT=1: off → on |
|---|---|---|---|---|
| 7.38e10 | 2 | 61.20 → 51.40 GB | 311.8 → 311.8 (dm binds) | 275.9 → 275.9 |
| 7.38e10 | 4 | 65.55 → 55.75 | 328.2 → 328.2 | 292.2 → 292.2 |
| 7.38e10 | 576 | 73.58 → 63.78 | 309.2 → 309.2 | **294.3 → 273.3** (−21.0 GB) |
| 8.5e10 | 2 | 67.28 → 55.99 | 347.0 → 347.0 | 302.4 → 302.4 |
| 8.5e10 | 4 | 73.97 → 62.68 | 372.2 → 372.2 | 327.6 → 327.6 |
| 8.5e10 | 576 | 84.19 → 72.90 | 354.0 → 354.0 | **336.8 → 309.4** (−27.4 GB) |

The early free lowers the tree need by 13–15 % everywhere; the arena moves only where the tree binds — at 576 under DM_TIGHT.

##### 3.2 Correctness on one node (job 21218, s24-16, 17:15–17:25 CDT; binary at 1167339; MEASURED)

`SLURM_JOB_ID=21218 ./mnrun.sh <p> env <switches> ./ecalc <d> /tmp/T114_<name>`, the part files concatenated on the node and
`cmp`'d against `~/ntt/ecalc/results/e_1e10.out` / `~/ntt/ecalc/ref/e_1000000000.txt` (script `archive:results_raw_2026-10-19/T114/T114_batch.sh`):

| run | size | digits | switches | result | wall |
|---|---|---|---|---|---|
| e10_s4_ef1 | 4 | 1e10 | MN_TREE_EARLY_FREE=1 POOL_LOG=29 ECALC_LIVE=1 | **identical**, 5 × VERIFY OK | 186 s (first run: cold) |
| e10_s4_ef0 | 4 | 1e10 | POOL_LOG=29 ECALC_LIVE=1 | identical | 106 s |
| e9_s2_ef1 | 2 | 1e9 | MN_TREE_EARLY_FREE=1 POOL_LOG=29 | **identical** | 34 s |
| e9_s3_ef1 | 3 | 1e9 | MN_TREE_EARLY_FREE=1 POOL_LOG=29 | **identical** | 23 s |
| e9_s3g3_ef1 | 3 | 1e9 | + MN_GROUPS=3 (one 3-way level: tree_level_k, P_run not owned then owned) | **identical** | 22 s |
| e10_s4g4_ef1 | 4 | 1e10 | + MN_GROUPS=4 (one 4-way level, 3 combines) | **identical** | 106 s |
| e10_s4g4_ef0 | 4 | 1e10 | MN_GROUPS=4 | identical | 96 s |

The tree levels' live windows (`live: mn node r tree level l`, per APU) are **the same with and without the switch** at these
sizes: binary levels 1.73–2.66 GB, the 4-way level 2.48 (3.00 on node 0, whose window holds its leaf); the `mem[r] tree` rows
(pl:peak 9.2–10.7 GB per process, cumulative from bs) likewise. So at 1e10 on one node a level's peak is in its FIRST product
(P_n = P_i Q_run + P_run: the addend's window), where nothing is dead yet; the early free lowers the second product's live set,
which is what the arena formula counts (the formula's single scratch term is the larger of the two products', so the bound
stays an upper bound). Commit 3545dd8 adds `live:` lines between the two products (`... mul1`, `... cN mul2`) to measure the
second product's window separately — run in a second job (§3.3).

`MN_TREE_EARLY_FREE=1 ./mnaccept.sh 21218 --only unit,mn` (17:25–17:45 CDT, 1238 s): **13 passed, 0 failed** (the 8 unit tests,
mn e8 at sizes 2, 3, 4 and e9 at 2, 4 identical, all nodes VERIFY OK); `archive:results_raw_2026-10-19/T114/mnaccept_21218_ef1.txt`.

##### 3.3 Each product's window (job 21222, s24-16, 17:46–17:55 CDT; binary at 33b9b04 with the `mul1` / `mul2` lines; MEASURED)

The same 1e10 at size 4 runs, POOL_LOG=29 ECALC_LIVE=1, all four **identical** to e_1e10.out (5 × VERIFY OK each; 192 s cold
first run, 106 / 93 / 106 s). Window peak per APU (GB), from each `live:` line (the window since the previous line):

Binary tree (levels 1 and 2):

| node | L1 MUL1 | L1 MUL2 off → on | L2 MUL1 | L2 MUL2 off → on | live after MUL1 (L2) off → on |
|---|---|---|---|---|---|
| 0 | 1.73 | 1.71 → **1.45** | 2.31 | 2.03 → **1.76** | 0.81 → 0.55 |
| 1 | 2.05 | 1.91 → **1.63** | 2.31 | 2.03 → **1.76** | 0.81 → 0.55 |
| 2 | 2.36 | 2.11 → **1.82** | 2.64 | 2.23 → **1.95** | 0.85 → 0.57 |
| 3 | 2.66 | 2.27 → **1.98** | 2.64 | 2.23 → **1.95** | 0.85 → 0.57 |

MN_GROUPS=4 (one 4-way level, 3 combines; c1 = child 2 into the running pair of child 3, ...):

| node | c1 MUL2 off → on | c2 MUL1 | c2 MUL2 off → on | c3 MUL1 | c3 MUL2 (the level's line) off → on |
|---|---|---|---|---|---|
| 0 | 1.22 → 1.22 (holds neither P) | 2.06 | 1.86 → **1.71** | 3.00 | 2.53 → **2.06** |
| 1 | 1.26 → 1.26 | 2.09 | 1.89 → **1.47** | 2.48 | 2.01 → **1.80** |
| 2 | 1.48 → **1.20** | 1.54 | 1.34 → **1.20** | 2.48 | 2.01 → **1.80** |
| 3 | 1.57 → **1.28** | 1.54 | 1.34 → **1.20** | 2.48 | 2.01 → **1.80** |

MUL1 windows are unchanged to the 10 MB (nothing is dead yet); every MUL2 window drops by the freed P shares (0.14–0.47 GB per
APU = 1–2 shares of 0.14–0.28 GB); nodes that hold neither P_i nor P_run (0, 1 at c1) are unchanged, as designed. At this size
the level peak stays in MUL1 (the addend's window), so the whole-level peak does not move. The arena formula has one scratch term
(the larger product's) and, without the switch, MUL2's live set 2c + 2r + 2n; with it MUL2 falls to c + r + 2n and the bound
becomes MUL1's 2c + 2r + n (at 576's 3-way levels c : r : n = 1 : 2/3 : 1, so 5.33 → 4.33 shares) — the measured MUL2 drop is the
drop the formula takes; the formula still covers MUL1 exactly as before.

##### 3.4 mnaccept without the switch (job 21222, 17:55–18:15 CDT, binary 33b9b04)

`./mnaccept.sh 21222 --only unit,mn`: **12 passed, 1 failed** (`mn e8 size 4`); `archive:results_raw_2026-10-19/T114/mnaccept_21222_ef0.txt`, the
failing log `archive:results_raw_2026-10-19/T114/mnaccept_21222_ef0_mn_e8_4.log`. The failure is **not the switch's** (it was off) **and not in the
tree**: node 3's bs leaf came out with a garbage size (`leaf P 18385101070989787659 limbs`, i.e. a negative count; Q fine), so
`rns_mul_dist_mn` refused the level-1 product ("the added operand ... exceeds the product") and the run exited before any tree
code ran. The same test passed in the switch-on pass (21218) and in the integrator's regressions; it looks like the open
intermittent leaf-level fault (Phase 10/11 notes), not reproduced here. Not re-run (no new node jobs, per the integrator).

#### 4. Summary

* Gates: 11 mnrun runs identical to the references (§3.2, §3.3: 1e10 at size 4 binary and 4-way, 1e9 at sizes 2, 3 and 3-way);
  mnaccept unit,mn 13/13 with the switch; 12/13 without (the leaf fault above, before the tree); layout check exact (§3.1).
* Measured: every second product's window drops by the freed P shares (0.14–0.47 GB per APU at 1e10 / size 4); the first
  product's is unchanged, which is where the level peak sits at this small size.
* Modelled (§2), DM_TIGHT=1, 576 target switches: 8.73e10 → **9.39e10** per node at 480 GB (5.03 → 5.41e13 total), 9.34e10 →
  **1.000e11** at 502 GB (5.38 → 5.76e13); +7.6 % / +7.1 %. Without DM_TIGHT the dm need binds and the switch buys nothing.

#### RESUME

* Step 1 done (code + model + README); step 2 done (aac6 clone ~/ntt-T114 built on p14-T1; layout check exact, §3.1).
* Step 3 IN FLIGHT: aac6 `~/T114_batch.sh` (copy in the scratchpad) launched 17:15 CDT with setsid nohup; it submitted job 21218
  (-J T1, 45 min), waits for it, runs mnrun size 4 1e10 on/off, size 2, 3 1e9 on, MN_GROUPS=3 size 3 1e9 on, MN_GROUPS=4 size 4
  1e10 on/off (all ECALC_LIVE=1, POOL_LOG=29, parts cmp'd), then `mnaccept.sh --only unit,mn` with the switch (dir moved to
  results/mnaccept/21218_ef1) and without; scancels at exit. Progress: `~/T114/batch.txt`, logs `~/T114/*.log`.
* COMPLETE (22:05 EDT): all steps done, §4 is the summary. No job of mine running or pending. Open: the intermittent leaf fault
  of §3.4 (not T1's), and the adoption decision on MN_TREE_EARLY_FREE (off by default).
* (history) The 7 mnrun runs done; mnaccept ef1 running at 17:25 CDT (its unit step takes ~20 min: t_ntt 24 alone
  ~10 min, as in the integrator's 21215). Plan: when `mnaccept ef1 rc` appears in ~/T114/batch.txt, kill the batch script by PID
  (its trap scancels 21218), deliver the bundle, rebuild, and launch `~/T114_batch2.sh` (setsid nohup; copy in archive:results_raw_2026-10-19/T114/):
  job T1b, 45 min: the 4 per-product-window runs, then `mnaccept --only unit,mn` WITHOUT the switch (→ results/mnaccept/<J>_ef0).
  This is automated by `~/T114_chain.sh` (launched 17:36 CDT; log ~/T114/chain.txt; bundle at 33b9b04 already on aac6);
  batch 2 progress in ~/T114/batch2.txt.
* After the batch: deliver 3545dd8+ (bundle), rebuild, second job: size 4 1e10 and MN_GROUPS=4 on/off with ECALC_LIVE=1 for the
  per-product windows (§3.3).
* Then: read batch.txt and the logs (live lines `live: mn node r tree level l ... window peak`, the `mem[r]` tables), fill §3.2,
  commit; if the job ran out, requeue the missing steps.

## L114 — L1 — Phase 14 (PLAN §34), window W1: E2 tight reservation (`DM_TIGHT`) and E5's layout half (`DM_TAIL_DEAD`)

(former file: L114.md)

Branch `p14-L1` from main @ ae1d872. Files owned and changed: `ecalc/binsplit.c` (dm_layout, the device tier's top levels, the layout
report, binsplit_pregrow's guard), `ecalc/binsplit.h` (the switches' externs), `ecalc/newton_db.c` (recip_db2's reservations only),
`ecalc/dbig.c` / `dbig.h` (`db_pool_retarget_tail`), `ecalc/mem_model.py`, `ecalc/README.md` (two switch rows). Nothing outside
the owned list. Labels: **measured** (an aac6 run, job named), **modelled** (`mem_model.py` = `dm_layout` to the byte, checked below),
**assumed** (stated where used).

#### 0. RESUME

*(updated after every step; the integrator resumes from here)*

- W2, 09:40 EDT: batch 5's 4e10: the reciprocal clean, t short by 0.11 GB after 7 spills into the tail; fixed by pinning r into the tail
  (916fbae); batch 6 (behind job 21154; gives up 10:45, stops 11:05 EDT) is the gate run: 4e10 default/tight, 1e11 tight/default, 1.4e11 tight.
- W2, 09:20 EDT: batch 4 measured (§5.3): the tight reciprocal clean and faster, one fallback left (the last doubling's shift into the
  small r2), fixed in 23a7f43; batch 5 (job 21154, the same five runs, gives up 10:00 EDT, stops 10:20) is the E2 gate now.
- W2, 08:40 EDT: the third form of `DM_TIGHT` (commit 540a20d: the tail = t's block holding r + t1; Aw before the low product, S freed
  there; `dm_layout` / `mem_model.py` re-derived, the layout sweep exact for all six variants) is built in `~/ntt-L114` and batch 4
  (`archive:results_raw_2026-10-19/l114/l114_node4.sh`, job 21150, queued behind the integrator's regression; gives up 09:25 EDT, stops 09:45) runs on one
  node: 4e10 default and tight (both cmp'd), 1e11 tight and default (VERIFY), 1.4e11 tight at 2³¹. The E2 gate = every run
  digit-identical / VERIFY OK with **hipMalloc 0 inside the phase** in the `dbig pool:` lines and the `mem [recip]` / `[division]`
  tables; its summary lands in `~/L114/node4_summary.txt`, the logs in `~/L114/node4_*.log`.
- Next step if the gate is met: §5.3 written with the same-node numbers (wall, dm, mem tables), the E2 elasticity at 1.4e11; then
  the remaining W2 items (E5's spill wiring is NOT worth it on one node: §3.3). If not met: the `dbig pool:` line names the block
  (need, free, extents, live blocks) — the third form's placement argument is in §1.1; the next lever is an explicit slot table.
- Done before: W1 (§1–§4), batches 1–3 (§5.1–§5.2).

#### 1. What the code does

##### 1.1 `DM_TIGHT=1` (E2, APUMULT_STUDY §3B and §4)

**Reciprocal (`newton_db.c recip_db2`).** Today r, r2, t1 are reserved once at their final capacity (k + 4, k + 4,
max(n_Q + k, 2k) + 8 limbs; `newton_db.c:72`). Tight (the second form, commit 9f04815): they are reserved once, right after the
seed, at the **last doubling's** sizes — jl = the chain's last start (⌈k/2⌉ under the anchored default): r at jl + 4 (r has j + 2
limbs, measured), r2 at 2 jl + 4 (`db_pow_sub` / `db_sub_pow` write 2j + 1 limbs of d whatever d.n ≈ j − 1 is, and r' = (r << j) ±
corr needs r.n + j + 1), t1 at take + jl + 2 + 8 (the grid's own reserve for Q_t·r at jl; the hole of `dm_layout`) — and the blocks
never move: r' is copied from r2 into r's block at every doubling but the last (`db_copy`, ≈ n_Q limbs of device copies in all;
the last doubling swaps, so µ takes r2's block as before). The values are the same; only capacities change — bit-identical by
construction.

The first form (commit df36cef: t1 freed and re-reserved per doubling, r2 reserved at 2j + 4 before d and freed at every swap) had
the same peak but churned: at 4e10 the last doubling's r2 (4.44 GB per APU) found 8–11 GB free in 3 extents, largest 3.9–4.4, and the
pool fell back to hipMalloc (jobs 21131, 21132: §5). Reserving once, while the pool holds only P and Q, makes the placement
deterministic, as today's is.

The last doubling's live set (k ≈ n_Q + 1): P + Q (2 × 1.1 n_Q with the bound's margin) + r (½) + r2 (1) + t1 (n_Q + jl + 10 ≈ 1½)
+ the grid's piece = **5.2 n_Q + piece**, against 6.2 n_Q + piece today. Measured today at 1.058 × 10¹¹ (G13d `a_S8_hi_1058e8`,
n_Q = 47.0 GB): peak live 294.7 GB = 6.27 n_Q — the formula's set.

**Block pool (`dbig.c ext_take`, `db_pool_pack_large`).** Under `DM_TIGHT` requests ≥ the tail's threshold are placed by a packing
rule: during bs (mode 1) at the highest free end *outside* the tail (the top level's P, Q land just below it, so t1 finds the tail
whole — with the tail rule alone P took the tail's back and nothing ended there any more); in the dm phase (mode 2) the tail's back
first (t1), then the highest free end (r2, just below Q). Small requests keep the best fit from the front outside the tail. A block
may now be carved from an extent's middle (`carve_at`), the remainder above becoming its own extent.

**Device tier's top levels (`binsplit.c` level loop, `dev_mdev`).** Each pair's P1, P2 are freed after P = P1·Q2 + P2 and Q1, Q2 after
Q = Q1·Q2 (children that are device numbers; the first device level's children are region views and are donated after the level as
before). At the top level Q = Q1·Q2 then runs beside 2 inputs + 2 outputs (3.3 n_Q) instead of 4 + 2 (4.4 n_Q). Skipped under
`ECALC_RES_LOG` (its per-level check reads the children after the level).

**`dm_layout`** (per device; `mem_model.py dm_layout` is the same expression):
- v2 = 2·Q(+10 %) + r(jl + 4) + r2(2 jl + 4) + hole + piece, hole = t1's quarter at the last doubling, (take + jl + 2 + 8) limbs ·
  (1 + 1/64); jl = ⌈k/2⌉ (NEWTON_ANCHOR=0: k − 1, the bound); + the slack min(1/8, 1 GiB).
- **new in every variant, the division's own term** div = 2·Q(+10 %) + max(µ(k + 1) + t(2k + 8), X(k) + xq(n_Q + k + 8)) + piece
  = 5.2 n_Q + piece; v2 = max(v2, div). It never binds in the default layout (5.2 < 6.2), so the default arena is byte-identical
  (checked: §2), but it is the floor once the reciprocal is tight or P is spilled.
- v3 (the top level beside the tail) = max(4 in + out, 2 in + 2 out) · (1 + 1/8) + hole (in = n_Q/2 + 5 %, out = n_Q + 10 %) instead
  of (4 in + 2 out) · 9/8 + hole.
- The `layout:` line of `BS_LAYOUT_ONLY` now ends with `tight T tail_dead U jl J v2 V v3 W div X` (per device, bytes).

##### 1.2 `DM_TAIL_DEAD` (E5's layout half, F1 §0 "V2 tail policy")

- `=1`: v3 without the hole — the top level may use the whole arena. After the top level (its inputs freed, the consumed parity
  donated) the block pool's tail is moved to the back of each APU's largest free extent (`dbig.c db_pool_retarget_tail`: the last
  `hole` bytes of it, or the whole extent when it is shorter — then the line says `SHORT` and t1 falls back as the pool always did,
  an in-phase hipMalloc). The arena-end tail set at init stays as the soft preference during bs. A `dbig pool: APUd tail moved …`
  line under `DB_POOL_VERBOSE` / `RNS_VERBOSE` gives the extent found (largest, free, extents, live).
  Safe on its own: the arena still holds v2 with P resident.
- `=2`: also v2 without the P term (P on disk during the reciprocal — the E5 layout). Needs the spill: `binsplit_pregrow` refuses to
  map it (`exit(2)` with the message) unless `dm_p_spill_wired` (set by W2's wiring) or `ECALC_INIT_ONLY=1` (the mapping test);
  `BS_LAYOUT_ONLY` prints it.

Contiguity (assumed until measured): the top level's inputs were carved consecutively from one extent at the level below (a quarter
per APU each: P0 Q0 P1 Q1), so once freed they form one extent of 2 n_Q/4 per APU ≥ t1's 1.5 n_Q/4. Under `DM_TIGHT` the level below
frees its own inputs per pair, and the best fit may then place P1, Q1 into that hole — the inputs can end up in two extents. The
`tail moved` line measures this; the 4e10 run of §5 is the first test.

#### 2. `mem_model.py --check-c` against `BS_LAYOUT_ONLY` (login node, no GPU)

Command per variant (aac6 login node, the build of df36cef):
`env DM_TIGHT=t DM_TAIL_DEAD=d BS_LAYOUT_ONLY=1e10,4e10,1e11,1.3e11,1.68e11,7.38e10:576 ./ecalc 1000000 x > layout_t{t}_d{d}.txt;
python3 mem_model.py --check-c layout_t{t}_d{d}.txt` — the check compares n_Q, hole, v2, v3, division, jl, dm need, top scratch,
tree need, bs regions and the arena, term by term, and counts terms that are not exact.

##### 2.1 Result (aac6 login node, 2026-09-24 ≈ 01:05 EDT, build df36cef)

All six variants (`DM_TIGHT` ∈ {0, 1} × `DM_TAIL_DEAD` ∈ {0, 1, 2}) at all six points: **every term exact, `largest arena difference
0.0000 %; 0 term(s) not exact`** (36 `layout:` lines, the files `~/L114/layout_t{t}_d{d}.txt(.chk)` on aac6). The default variant's
arena is byte-identical to the pre-change layout (1e11: 310 286 221 312 B = M13's 310.3 GB; 576 share: 309 212 479 488 B = RESULTS
§82's 309.2). Sample lines (per device, bytes):

| point | variant | hole | v2 | v3 | division | dm need | arena / node |
|---|---|---|---|---|---|---|---|
| 1e11 | V0 | 22.57 G | 74.60 G | **77.57 G** | 62.07 G | 77.57 G | 310.29 GB |
| 1e11 | tight + tail_dead 1 | 16.93 G | **63.41 G** | 41.25 G | 62.07 G | 63.41 G | 258.63 GB |
| 7.38e10 × 576 | V0 | 16.66 G | 56.47 G | — | — | 77.30 G (+ top scratch 20.06) | 309.21 GB |
| 7.38e10 × 576 | tight + tail_dead 1 | 12.49 G | 48.20 G | — | — | 68.26 G; the tree's 73.58 G binds | 294.34 GB |

At 1e11 under `DM_TIGHT` the division's own term (62.07 G) is within 2 % of the tight reciprocal's (63.41 G including the slack):
the floor of §3.3.

#### 3. The modelled savings (`python3 mem_model.py --savings`; size 1 at 2³¹, three primes, the host fitted; 576 = the target's
switches: SHMEM resident, MDB_SHIFT_CHUNK_MB=1024, COMM_ALLTOALLV_DEPTH=2 — reproduces RESULTS §82's 452 GB at 7.38 × 10¹⁰)

Node peak, GB (modelled):

| variant | 1e10 | 4e10 | 1e11 | 1.3e11 | 1.68e11 | 7.38e10 × 576 |
|---|---|---|---|---|---|---|
| V0 (defaults) | 220.7 | 299.6 | 427.9 | 522.0 | 641.0 | 451.7 |
| `DM_TIGHT=1` (E2) | 218.3 (−2.4) | 281.7 (−17.9) | **376.3 (−51.7)** | 441.9 (−80.1) | 531.3 (−109.8) | **428.2 (−23.5)** |
| `DM_TAIL_DEAD=1` alone | 220.7 | 299.6 | 416.1 (−11.9) | 500.1 (−21.9) | 606.5 (−34.5) | 448.6 (−3.1) |
| `DM_TIGHT=1 DM_TAIL_DEAD=1` | 218.3 | 281.7 | 376.3 (−51.7) | 441.9 | 531.3 | 428.2 |
| `DM_TIGHT=1 DM_TAIL_DEAD=2` (the E5 layout; needs the spill) | 218.3 | 281.3 | 375.5 (−52.4) | 440.5 (−81.4) | 529.5 (−111.5) | 428.2 |

Arena per node at 1e11: 310 → 259 GB (tight; v2 = 254 binds, v3 = 232); at the 576 share the arena is the tree's 294 GB as soon
as the dm need (309 → 273) drops below it, so the node saving there is the dm − tree gap, 23.5 GB (452 → 428), not the 33 GB of
F2's estimate (which did not have the tree binding).

Ceilings (the largest D whose modelled node peak fits; one node at the 502 GB node / the measured 524 GB edge; 576 at 480 / 502 GB
per node):

| variant | one node 502 / 524 GB | 576: per node 480 / 502 GB | total |
|---|---|---|---|
| V0 | 1.235e11 / 1.305e11 (measured 1.30, P13b) | 8.09e10 / 8.61e10 | 4.66e13 / 4.96e13 |
| `DM_TIGHT=1` | **1.554e11 / 1.648e11** (+26 %) | **8.73e10 / 9.34e10** (+8 %) | 5.03e13 / 5.38e13 |
| `DM_TAIL_DEAD=1` | 1.306e11 / 1.384e11 (+6 %) | 8.24e10 / 8.87e10 | 4.75e13 / 5.11e13 |
| both (=1) | 1.554e11 / 1.648e11 | 8.73e10 / 9.34e10 | 5.03e13 / 5.38e13 |
| `DM_TIGHT=1 DM_TAIL_DEAD=2` (E5 layout) | 1.561e11 / 1.656e11 | 8.73e10 / 9.34e10 | 5.03e13 / 5.38e13 |

##### 3.3 Finding: E5 (P spilled) gives ≈ 0 over E2 until E6 — the division's own set is the floor

With the reciprocal tight, the arena's dm term is max(reciprocal 5.2 n_Q + piece, **division 5.2 n_Q + piece**, top level 3.3 n_Q ·
9/8 + hole). Spilling P lowers only the reciprocal's term (4.1 n_Q + piece); the division (S + Q + µ + t, or S + Q + X + xq, each
5.2 n_Q + piece) stays, so `DM_TAIL_DEAD=2` moves the 1e11 node by −0.8 GB and the one-node ceiling by +0.5 % over `DM_TIGHT`
alone. The study's V2 ceiling (1.68 × 10¹¹, F1) was computed against V0's reciprocal (6.2) with the division not modelled
separately; E2 by itself reaches 1.55–1.65 × 10¹¹ (modelled) and takes that gain without any disk. E5 pays only together with E6
(Q spilled through the high product, S freed before the low product: the division's sets 4.1 / 4.2 n_Q + piece), which is then
worth −1 n_Q of dm need — **but then the bs regions bind**: modelled ad hoc (the same formulas, dm = max(reciprocal Q + r + r2 +
t1 + piece, high product S + µ + t + piece, low product Q + X + xq + Aw + piece), P and Q spilled, S freed before the low product):

| size | bs regions (two parities) | dm need E5 + E6 | arena E5 + E6 | node E5 + E6 | node `DM_TIGHT` | node `DM_TIGHT` + E5 layout |
|---|---|---|---|---|---|---|
| 1e11 | 242.9 | 248.0 | 254.5 | 372.2 | 376.3 | 375.5 |
| 1.3e11 | 296.5 | 316.0 | 316.9 | 435.5 | 441.9 | 440.5 |
| 1.68e11 | 364.8 | 402.4 | 402.3 | 522.0 | 531.3 | 529.5 |
| 1.8e11 | 442.2 | 429.6 | 450.3 | 570.4 | 574.4 | — |

So on one node the whole spill programme (E5 + E6, two exposed disk reads) buys 4–9 GB over `DM_TIGHT` alone: the arena is then the
bs regions' own size (≈ 5.5 n_Q: a level's P, Q for every node, two parities, + 1/8), and the next lever is the batch tier's ring
layout (F2's `BATCH_PAIR_INMADV` row, L effort), not a spill. The study's "1.30 → 1.68 × 10¹¹ for V2" is therefore mostly E2's
gain, obtainable without disk. Recommendation for W2: measure `DM_TIGHT` at 1.4–1.6 × 10¹¹ first (the elasticity is then compute
only); wire the P spill only as the tool for 576 nodes' margin, where it is the tree level that binds anyway. For the 576 share
none of the dm variants matters beyond the first 23.5 GB: the tree's top level (294 GB) binds.

#### 4. Tests run (login node)

| step | command | size | result | time |
|---|---|---|---|---|
| build | `bash -lc "module load rocm; cd ~/ntt-L114/ecalc && make -s -j16"` at df36cef | — | ok (ecalc, tests/t_newton rebuilt) | ≈ 3 min |
| model, default variant | `python3 mem_model.py` (the calibration table) | — | unchanged against the pre-change table (spot-checked 4e10 / 7e10 / 8e10 rows) | s |
| model, savings | `python3 mem_model.py --savings` | — | §3 | 40 s |
| layout sweep | `bash -l l114_layout.sh` (§2) | 6 points × 6 variants | all 36 exact | 2 min |
| node batch 1 | `l114_node.sh` (§5.1) | job 21131 | all digits identical; one in-phase hipMalloc at 4e10 (fixed in 47ced5f, batch 2) | 22 min |

#### 5. Node tests (one ≤ 30-min job, `archive:results_raw_2026-10-19/l114/l114_node.sh`, unattended: job 21131 submitted 01:02 EDT, pending behind the CI array; gives up at 03:58 EDT, skips what would not finish by 04:27, cancels itself)

1. `NEWTON_DEVICE=1 tests/t_newton 20` (the device reciprocal, `recip_db2`) with `DM_TIGHT=1` and without.
2. e9 in both bases with `DM_TIGHT=1` (`LIMB_BASE=10`, `LIMB_BASE=2`), `cmp` against `~/ntt/ecalc/ref/e_1000000000.txt`; and e9 with
   `DM_TIGHT=1 DM_TAIL_DEAD=1 BS_MDEV_LOGL=24` (the top levels forced onto the device tier at 10⁹: the pair release, the device dm flow
   and the tail retarget exercised at a small size).
3. 4e10 with `DM_TIGHT=1 RNS_VERBOSE=1 NEWTON_VERBOSE=1 MEM_REPORT_DEVS=1`, the reference `~/ntt/ecalc/results/e_4e10.out` evicted
   from the page cache first, `cmp`; the `newton(db)` lines give r, d, t1 per doubling (the schedule's assumed sizes, measured),
   the `mem [recip]` peak-live against 5.2 n_Q + piece, and `dbig pool:` any in-phase hipMalloc.
4. `ECALC_INIT_ONLY=1` at 1e11 with `DM_TIGHT=1` and with `DM_TIGHT=1 DM_TAIL_DEAD=1`: the `mem [init]` device total against the
   `planes:` line of `BS_LAYOUT_ONLY`.

##### 5.1 Batch 1 results (job 21131 on aac6-spx-b, 01:10–01:32 EDT; build df36cef; logs `~/L114/node_*.log` on aac6) — measured

| test | command (in `~/ntt-L114/ecalc`, under `srun --gpus=4`) | result |
|---|---|---|
| t_newton | `NEWTON_DEVICE=1 tests/t_newton 20` | VERIFY OK (recip 0.024 s) |
| t_newton tight | `NEWTON_DEVICE=1 DM_TIGHT=1 tests/t_newton 20` | VERIFY OK (recip 0.059 s: the per-doubling free/reserve at 2²⁰ limbs; nothing at real sizes, next row) |
| e9 decimal | `DM_TIGHT=1 LIMB_BASE=10 ./ecalc 1e9 f` | VERIFY OK, digits identical to the reference, 11.67 s |
| e9 binary | `DM_TIGHT=1 LIMB_BASE=2 ./ecalc 1e9 f` | VERIFY OK, identical, 25.54 s |
| e9, top levels forced onto the device tier | `DM_TIGHT=1 DM_TAIL_DEAD=1 BS_MDEV_LOGL=24 LIMB_BASE=10` | VERIFY OK, identical, 9.73 s; the pair release and the tail retarget ran: `APU0 tail moved to the top level's dead inputs: 0.17 GB at the back of the largest free extent (0.46 GB; free 0.69 GB in 2 extents, live 0.22 GB)` |
| **4e10 tight** | `DM_TIGHT=1 ECALC_VERBOSE=2 RNS_VERBOSE=1 NEWTON_VERBOSE=1 MEM_REPORT_DEVS=1 DB_POOL_VERBOSE=1 ./ecalc 4e10 f`, the reference evicted first | **VERIFY OK, digits identical to `e_4e10.out`; total 62.16 s (bs 23.5, dm 22.7; recip 10.01 s) against the five-run baseline 63.5 ± 1.5 (RESULTS §80): no time cost.** Arena 114.4 GB (132.3 before). **But** the pool fell back inside the reciprocal: `APU0 hipMalloc 4.44 GB inside the phase (free 10.81 GB in 3 extents, largest 4.39; live 17.78 GB in 16 blocks)` — r2's block at the last doubling (2jl + 4 limbs = 4.44 GB per APU) found no extent: a placement failure, not bytes (10.8 GB free). Device at the recip peak 235.8 GB (planes 103.1 + pool 132.1 of which 17.8 hipMalloc); peak live 100.7 GB |
| init-only 1e11 tight | `DM_TIGHT=1 ECALC_INIT_ONLY=1` | `mem [init] device 362.3 GB` = the `planes:` line's `cap 2^31: arena 258.63 device 362.32` — **measured = modelled to 0.01 GB**; arenas 258.6 GB in 10.96 s (tails 67.7 GB) |
| init-only 1e11, the E5 layout | `DM_TIGHT=1 DM_TAIL_DEAD=2 ECALC_INIT_ONLY=1` | device 361.6 GB, arena 257.9 (the guard lets init-only through) |

The placement failure and its fix (commit 47ced5f): with the tail rule alone the top level's P (≥ thresh, "large") takes the tail's
back; at the reciprocal nothing ends at the tail's end any more, so t1 and r2 are best-fit into the middle and the free space
splits. Under `DM_TIGHT` large requests are now carved from the back of the **highest-ending** free extent that holds them
(`dbig.c ext_take`, `db_pool_pack_large`), so P, Q, t1, r2 stack at the arena's top and the space below stays one extent (hand
simulation of the 4e10 sequence: 8.0 + 2.9 GB free at the last doubling, no fallback). Batch 2 (`archive:results_raw_2026-10-19/l114/l114_node2.sh`)
re-runs the 4e10 with it, plus 4e10 with `DM_TAIL_DEAD=1`, e9 × 3 and a 1e11 `DM_TIGHT=1` run (VERIFY only, no reference on aac6).

##### 5.2 Batches 2 and 3 (jobs 21132 on s24-16 and 21134 on s24-16; commits 47ced5f = the packing rule, 9f04815 = the second form) — measured

Every run digit-identical / VERIFY OK: 4e10 `DM_TIGHT=1` 63.36 s (batch 2) / **61.17 s** (batch 3), 4e10 + `DM_TAIL_DEAD=1` 62.86 /
63.47 s, e9 × 3 both batches, 1e11 `DM_TIGHT=1` 308.56 s (dm 183.7; batch 2, with 22 GB of in-phase hipMalloc) / **253.99 s (dm 139.1;
batch 3)**. But in-phase hipMalloc remained: batch 2 4e10: 4.44 GB on one APU (free 8.59 GB in 3 extents, largest 3.92); batch 3 4e10:
4.44 GB with **5 blocks live (22.22 GB = S, Q, X, xq)** — the window `Aw` (n_Q + 2 limbs); batch 3 1e11: **22.22 GB with 3 blocks live
(33.33 GB = S, Q, µ)** — the division's `t = A_h·µ` (2k + 8 limbs), free 35.07 GB in 3 extents, largest 16.09 (= the tail minus what r
left in it). So the second form's reciprocal is fine (peak live 106.7 GB at 4e10 = 6.0 n_Q incl. the piece … its blocks are placed
once); **the division is what does not fit**: it needs t (2 n_Q) contiguous where the tight reciprocal left t1 (1.5) and r (0.5)
apart, and its window moment S + Q + X + xq + Aw = 6.2 n_Q exceeds any 5.2 n_Q arena. The third form (§1.1, commit below) sizes the
tail to t and reorders the window; batch 4 measures it.

##### 5.3 Batch 4 (job 21151 on aac6-spx-a, 08:42–09:13 EDT; commit 540a20d = the third form) — measured, same node

| run | wall | dm | recip | arena | hipMalloc inside the phase | digits |
|---|---|---|---|---|---|---|
| 4e10 default | 63.01 s | 21.9 | 9.70 | 132.3 GB | 0 | identical |
| 4e10 `DM_TIGHT=1` | 63.51 s | 22.9 | 10.53 | 114.4 GB (−17.9) | 4.44 GB × 4 APUs, at the reciprocal's end (see below) | identical |
| 1e11 default | 235.83 s | 119.0 | 62.05 | 310.3 GB | 0 | VERIFY OK |
| 1e11 `DM_TIGHT=1` | 237.15 s | 123.4 | **45.63** | 259.9 GB (−50.4) | 22.22 GB (t, the division) on 2 APUs | VERIFY OK |
| 1.4e11 `DM_TIGHT=1`, 2³¹ | — | — | 112.19 | 359.3 GB (node 463.0 + host 13) | the reciprocal clean (peak live 326.7 GB = 5.25 n_Q, the tight set); t (31.1 GB) found the tail 2.6 GB short, the driver refused the fallback (117 of 137 GB per APU): `mem_oom`, exit segfault (TASKS A3) | — |

The reciprocal itself is faster under `DM_TIGHT` (45.6 against 62.1 s at 1e11: its blocks sit in one place, the products' loads are
local); the dm phase as a whole is within 1–4 s of the default, the 22 GB fallback's hipMalloc included. The one remaining
fallback has one cause: at the last doubling the anchored target k is one below 2j, and the shift `r >>= 1` went into r2 — which after
the final swap is the small 0.5 n_Q block — so r2 was regrown by 1.0 n_Q with everything live (4e10: `live 22.22 GB in 20 blocks` =
P, Q, r, r2, t1 right after the last `newton(db)` line); that block disturbed the tail for the division's t. Commit 23a7f43 shifts
through the dead t1 and copies back into r's block; batch 5 (job 21154) re-runs the same five runs.

##### 5.4 Batch 5 (job 21154 on s24-16, 09:13– EDT; commit 23a7f43) — measured, same node

4e10 default 63.54 s (dm 22.3); 4e10 `DM_TIGHT=1` 63.62 s (dm 24.1, recip 9.61), identical. **The reciprocal is clean now** (pool 114.5 GB,
hipMalloc 0 through it), the one fallback left is the division's t (8.89 GB) finding the tail's free part 8.78 of 9.03 GB: the run's
tail line says `7 small blocks spilled into it` — a small request that found nothing outside was placed in the tail's front and one
was still live. Fix (commit 916fbae): r is reserved *inside* the tail right below t1 (`db_pool_pin_tail`), so the tail is full
during the reciprocal (nothing can spill into it) and whole after it; the outside then holds r2 and the piece temporaries. Batch 6
(`l114_node6.sh`, behind job 21154) is the gate run for it. 1e11 / 1.4e11 lines of batch 5: below when they land.

#### 6. Open issues

- The tight schedule's formula assumes the last doubling converges at once (repeats = 0 there; every measured run to 1.25 × 10¹¹
  shows `corrections 0/0` and the anchored chain's repeats are not printed per doubling — the new verbose line prints d's limbs).
  A repeat with d longer than take regrows t1 by the grid's own reserve: correct, +≤ 0.5 n_Q of in-phase pool at that moment.
- Fragmentation under the per-doubling churn (t1 freed and re-reserved each doubling; r2 freed at each swap): the pool coalesces,
  the tail keeps the largest block at the extent's back, and the fallback is the in-phase hipMalloc as before — to be read off
  `DB_POOL_VERBOSE` at 4e10 and, in W2, at 1.4–1.6 × 10¹¹.
- The size > 1 formula keeps the current convention (the size-1 shape over the shares); the sharded reciprocal's real live set was
  not re-derived (its shares are already allocated per op, `newton_db.c mfree`).
