# A-mem — M9 memory accounting, C4 init sizing, C2 the region imbalance (Phase 9, PLAN §19)

Branch `amem` (from `main` @ a75474d). Files owned: `ecalc/mem.c/.h`, `rns_init` and the pool functions
of `ecalc/rns_mul.c`, `binsplit_pregrow` and the level layout of `ecalc/binsplit.c`, the block pool's
accounting in `ecalc/dbig.c`. Touched outside (minimal, commented `A-mem`): `ecalc/ecalc.c` (the pool-1
request next to the staging sizing, the donation threshold, `mem_report` calls at the phase boundaries),
`ecalc/dbig.h` (one additive declaration), `ecalc/binsplit.h`.

## What was measured first: the cost of mapping device memory (job 20708, s24-16)

`hipMalloc` on one APU: 2 × 14 GB 2.11 s, 1 × 28 GB 2.12 s, 8 × 1 GB 0.61 s, 16 GB 0.86 s — 0.054–0.075
s/GB with no per-call overhead worth chasing; four APUs in parallel 28 GB each 7.0 s wall (6.9 s per APU: the
driver serialises), serial 7.1–8.1 s. `hipFree` 0.004 s/GB. No flavour is cheaper: `hipExtMallocWithFlags`
uncached 1.26 s / fine-grained 1.21 s, `hipMallocManaged` 2.23 s, `hipHostMalloc` 2.73 s for 16 GB.
`hipMemGetInfo` works (137.4 GB per APU, free tracks allocations) and is used for the transform tables.
So: **bytes mapped over the run is the only lever**; one allocation per device for both parities buys no
driver time by itself — its value is contiguity for the block pool (below).

## Design

### M9 — accounting (`mem_report`)
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

### C4 — init sizing
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

### C2 — the five-node level
`place_node(i, n, offr, ra, rb)`: levels with more than `bs_balance_n` (16) nodes keep the subtree rule
(region NR i / n); smaller levels — where the odd carries make the counts 33, 17, 9, 5, 3, 2 — put each
output node in the least-loaded region (fewest limbs laid out so far), ties broken for the region holding
its inputs (b's, then a's), then the lowest index. Correct for any placement (the batch tier computes on the
device owning the result and reads operands where they are). At 4 × 10¹⁰ the level with five outputs goes
from shares 2/5, 1/5, 1/5, 1/5 (+ the small carried node) to 1/4 each, and no pool grows inside the phase
(`bs_st.n_grow`, printed after bs).

## Tests (jobs 20708, 20718, 20731, 20739, 20746, 20749, 20750, s24-30 / s24-16; every ecalc run below VERIFY OK unless said)

Correctness against the reference files (`cmp`, `~/ntt/ecalc/ref`, `results/e_1e10.out`, `results/e_4e10.out`):

| run | command (from `ecalc/`, `$J` the allocation) | result |
|---|---|---|
| 10⁶, 10⁸, 10⁹ size 1 | `ECALC_VERBOSE=2 ./ecalc <d> out` | identical (every build; 10⁹ re-checked after each change) |
| 10⁸ sizes 2, 4 | `SLURM_JOB_ID=$J POOL_LOG=29 ./mnrun.sh <p> ./ecalc 100000000 out` | identical |
| 10⁹ size 2 | `POOL_LOG=30 ./mnrun.sh 2 ./ecalc 1000000000 out` | identical |
| 10¹⁰ size 1 | `./ecalc 10000000000 out` | identical |
| 10¹⁰ size 2 | `POOL_LOG=30 ./mnrun.sh 2 ./ecalc 10000000000 out` | identical |
| 10¹⁰ size 4 | `POOL_LOG=27 ./mnrun.sh 4 ./ecalc 10000000000 out` | identical (75 s) |
| 10¹⁰ size 4 | `POOL_LOG=29 ./mnrun.sh 4 ...` | **T1 FAILED** twice (see open issues) |
| 2 × 10¹⁰ size 2 | `POOL_LOG=30 ./mnrun.sh 2 ./ecalc 20000000000 out` | VERIFY OK (119 s; no reference file to compare) |
| 4 × 10¹⁰ size 1 | `RNS_VERBOSE=1 ECALC_VERBOSE=2 ./ecalc 40000000000 out` | identical, 8 runs over the variants below |

### 4 × 10¹⁰, size 1 — the gate

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

### Sizes 2 and 4 on one node — capacity

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

## Open issues

1. **10¹⁰ at size 4 with `POOL_LOG=29`: T1 fails** (two runs, nodes 2's / 0's leaf P differ by a limb from the
   passing run's), while the same command with `POOL_LOG=27` is identical to the reference, size 2 at 10¹⁰ is
   identical, and size 4 at 10⁸ is identical. `main` cannot run this configuration on one node at all (OOM or
   segfault), so pre-existing vs new is undetermined. The difference between 27 and 29 in this build was
   which pools grew inside the batch tiers (the 2^29 tile fits pool 0 exactly at 29 and pool 1 grows from 3 q
   to 4 GiB in the striped path); the final build sizes pool 1 to the full pool for `POOL_LOG` ≤ 30 (batch 7
   re-runs 29 twice and 28 once — see the end of this file for the result).
2. The non-zero ranks' region arenas are not released by `rns_shutdown` there (`binsplit_free_pools` never
   runs on them, so the hook is not set): 22 GB per process stays mapped until exit — harmless, one line.
3. At size > 1 the leaf's regions are only donated to the block pool when the leaf ends on the device tier;
   when it ends on the batch tier the tree's packed slabs and shares come from `hipMalloc` (11–44 GB per
   process, "pool: hipMalloc" in the tree rows). Donating the regions before `mn_tree` needs the driver's flow
   (A-div's area now) — noted, not done.
4. `ECALC_DM_POOL_K` (dm-sized arena) is the knob for A-div if the distributed division wants its pool mapped
   at init.

## Files touched outside my list (minimal, commented `A-mem`)

`ecalc/ecalc.c`: the pool-1 request and `host_combine` next to the staging sizing (the seed-sized staging now
applies at any size in the M3 flow); the donation threshold; `mem_report` calls; init stage timing print; the
non-zero ranks' release before the final barrier. `ecalc/rns_mul.c` batch tiers: four one-line `rns_dpool`
calls that grow a plane pool on demand before it is used (no-ops when it fits). `ecalc/dbig.h`: one
declaration. `ecalc/binsplit.h`: `bs_stats.n_grow/grow_bytes`, `binsplit_release_arenas`, `bs_balance_n`.
