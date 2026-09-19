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

## Tests

(filled in below from the batches)
