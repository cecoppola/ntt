# M — device memory: B5, B6, B4 / A3, the 8 × 10¹⁰ attempt (Phase 10, PLAN §21)

Branch `m10` (from `main` @ 4aca721; the aac6 clone `~/ntt-m` at 38ed61b + the branch). Files owned: `ecalc/mem.c/.h`,
the pool functions of `ecalc/rns_mul.c` (untouched in the end), `binsplit_pregrow` / the layout / `binsplit_free_pools` in
`ecalc/binsplit.c`, the block pool in `ecalc/dbig.c/.h`, the pool-related lines of `ecalc/ecalc.c`. Everything below
continues `results/A-mem.md` (its open issues 2 and 3 are B5 and B6).

## B5 — the non-zero ranks' region arenas (one line)

`rns_shutdown_hook = binsplit_release_arenas` is set where the arenas are allocated (`arena_get`), not only in
`binsplit_free_pools`, which the non-zero ranks of the `MN_DM=host` and M2 paths never reach. Every rank's
`[released]` row now shows `regions 0.0` (before: 22 GB per process stayed mapped until exit). The default
distributed-dm path already ran `binsplit_free_pools` on every rank after the tree, so there it changes nothing.

## B6 — at size > 1 the leaf's regions go to the block pool before the tree

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

## B4 / A3 — the dm phase's block pool at 7 × 10¹⁰, and what the in-phase growth is

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

### 4 × 10¹⁰ gate (size 1, reference evicted, `cmp` against results/e_4e10.out)

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

## The 8 × 10¹⁰ attempt

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

## Tests run (jobs 20764, 20769, 20773, 20790; s24-30 / s24-26)

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

## Open issues

1. `ECALC_DM_POOL` is off by default (−1.7 s of the reciprocal at 7 × 10¹⁰, noise at 4 × 10¹⁰): the user's call to
   make it the default above some digit count; the largest-block rule makes it safe at any size.
2. t1's quarter (2 n_Q / 4 limbs) is the one block that does not fit the arena's holes; an arena laid out so that
   t1's room is the contiguous tail (Q and S placed first, r and r2 after) would remove the 62 GB entirely — but
   the same bytes are then mapped at init (0.06 s/GB) for the same wall; only worth it for memory, not time.
3. At 10¹⁰ size 4 the tree still maps ≈ 5 GB per process beyond the donated leaf regions (its slabs); an arena
   sized for the tree's slabs at size > 1 (`region_need` + g × slab) would remove it — 0.3 s per process.
4. The seed staging at 8 × 10¹⁰ is 4 × 19.5 GB pinned; H's B2 (one reused buffer) brings it to 20 GB.

## Files touched outside my list (minimal, commented `Phase 10 ... (agent M)`)

`ecalc/ecalc.c`: the C3 block (the largest-block rule, the growth print), `mem_report("tree")` in the distributed-dm
path, `mem_report("division")`, the staging cap (one condition). `ecalc/dbig.h`: three declarations.
