# P — single-node speed: B3 planes at init, A2 level-22 pairing, DM_POOL default, I11 init, A4 tile budget (Phase 11, PLAN §26)

Branch `p11` (from `main` @ 72aa2e9; aac6 clone `~/ntt-p`). Files: `ecalc/rns_mul.c` (pools, the striped batch path),
`ecalc/rns_mul.h` (additive), the init lines of `ecalc/ecalc.c` (the plane switch, the C3 default), `ecalc/ntt.c` (the
context tables' upload only). Touched outside my list, minimal and commented `Phase 11 ... (agent P)`: `ecalc/rns_dist.c`
(L's file) — `dist_r3()` defaults to the plane switch, `dist_core` takes `xb | sbuf | rbuf` from pool 1 when it holds them,
the grid cost model counts a 3·2^k plane at 21/20 per point and the one-plane form is taken only when no grid is cheaper
(`plane_pts`/`split_grid_cap` gained an `r3` argument; the mn tier passes 0). Every run below is on aac6 (jobs 20807 on
s24-30, 20816 on s24-26, the gate job below), 4 × 10¹⁰ with the reference evicted before each timed run and the digits
`cmp`'d against `~/ntt/ecalc/results/e_4e10.out` after it; logs in `~/p11/out{1,2,3}/` on aac6.

## Summary

| item | switch (default) | measured at 4 × 10¹⁰ | adopted |
|---|---|---|---|
| B3: 3·2^k-point planes sized at init (pool 0 = 3·2³⁰ limbs = 24 GiB, pool 1 = 3q + 16 = 18 GiB per APU) | `RNS_PLANES_3Q30=0/1/auto` (**0**) | phases −3.6 s (top levels 13.2 → 10.4, reciprocal −0.6, level 21 paired −0.2; the 15 s first-use cost of A-grid's C5 gone), but the 60 GB more of pools cost **+4.7 / +6.3 s at init** (the pools map at 0.08–0.10 s/GB while the seeds stream): wall +1.0 / +2.6 s | no — code and switch kept; `auto` = on below 5 × 10¹⁰ at 2³¹ pools (the size rule asked for) if the mapping ever gets cheaper |
| A2: the level-22 products (8 at 2³⁰ points, 4 pairs) paired in the striped batch path on 3·2²⁸ planes | `RNS_STRIPED_PAIR=0/1` (**1**) | level 22: 2.16 → 1.55–1.63 s (**−0.55 s**), the same pools (a pair = 12 + 6 GiB); at 7 × 10¹⁰ the level of 14 products 2.7 s | yes |
| §23-3: `ECALC_DM_POOL` on from 5 × 10¹⁰ | `ECALC_DM_POOL=0/1` (**on at d ≥ 5 × 10¹⁰**) | 7 × 10¹⁰: **153.5 s** (was 159.7 default, 157.2 with the pool; A2 gives the rest), VERIFY OK, planes automatically off (16 + 12 GiB) | yes |
| I11: init — the context tables and pass twiddles uploaded in one `hipMalloc` + `hipMemcpy` each (22 pairs per context, 4 per pass before); engine 2's context built only for engine 2; the init breakdown printed | — | contexts 0.3–0.8 s of the 1.0 s "staging+contexts" (inside the parallel per-APU loop); init is regions 4.8 s + pools 14.0 s of mapping, the seed thread (15 s) alongside — nothing left to share across APUs (the tables are 8 KB per context; sharing them would put twiddle reads on xGMI); no `hipMemset` remains on the plane pools (`mem_dev_alloc`'s memset of a fresh allocation is 0.01 s) | yes (bit-identical, `t_ntt` 565 checks) |
| A4: the batch tier's tile budget in pair mode | `RNS_BATCH_TILE_GB` (**15**) | 25.8 GB (2²⁹ points per prime plane): batch tier 22.6 → 22.5 s, transforms −0.9 s of 15.2 spread over the levels but the wall the same (83.0 vs 81.7, the noise of init) | no — knob kept |

## B3 — the 3·2^k planes sized at init

Design. `rns_planes_3q30` (rns_mul.c, set by the driver before `rns_init` from `RNS_PLANES_3Q30`) sizes plane pool 0 to
exactly 3·2^(pool_log−1) limbs (`dpool_get_exact`: 24 GiB, not the 32 GiB of `dpool_get`'s power-of-two growth A-grid
paid) and pool 1 to the dist tier's 3q + 16 limbs at q = 3·2^(pool_log−3) (18 GiB + 2 MiB), inside init's parallel per-APU
allocation. `rns_pool1_default_bytes` follows the switch, so the driver's tail donation (`rns_dpool_donate_tail` at I3)
donates nothing, and `rns_plane_limbs()` (pool 0's real capacity) replaces `1 << pool_log` in the batch tiers' tile
arithmetic. `dist_r3()` defaults to the switch (`DIST_R3` still overrides); `dist_core` takes `xb | sbuf | rbuf` from pool 1
when its capacity holds 3q + 16 (else, `DIST_R3=1` alone, from the block pool as A-grid did). The grid's cost model
(`split_grid_cap`) charges a 3·2^k plane 21/20 per point (A-grid: 1.57× the time for 1.5× the points), and `mul_grid` takes
the one-plane form for a product above 2³¹ limbs only when no grid is cheaper — A-grid's level-25 case (2.19e9 × 2.7e7
limbs) stays 3 × 1 pieces of 3·2²⁸ (1.57 s) instead of one 3·2³⁰ plane (1.85 s); level 24 (1.06e9 × 1.13e9) becomes one
3·2³⁰ plane (1.85 s, was 1 × 2 pieces of 2³¹ = 2 × 1.19 + the shifted add); the division's A_h μ and X Q (2.22e9 × 2.22e9)
become 1 × 3 pieces of 3·2³⁰ (3 × 1.9 s, were 2 × 3 of 2³¹ at 6 × 1.2 with one skipped), Q_t r (2.22e9 × 1.11e9) stays 3 × 1
of 2³¹. In the batch-local tier pool 1's request in pair mode is M/2 B planes (it asked for M: with the larger pool 0 the
level-21 tile of 2 would have grown pool 1 inside the phase), and level 21 (16 products at 3·2²⁷) pairs (tile 2).

Memory (4 × 10¹⁰, `mem_report`): init 276.7 GB (planes 180.4, regions 95.7) instead of 216.6; the recip peak 322 GB
instead of 262; node peak ≈ 334 GB of 502. At 7–8 × 10¹⁰ the switch is off under `auto` (290.5 + 60 at init, 353 + 60 at the
recip would still fit 7 × 10¹⁰ but not 8 × 10¹⁰'s 387 + 60 + 90 host).

Measured (job 20807 on s24-30, then 20816 on s24-26; each pair on the same node in the same allocation, A2 on in both):

| 4 × 10¹⁰ | planes off | planes on | | planes off | planes on |
|---|---:|---:|---|---:|---:|
| node | s24-30 | s24-30 | | s24-26 | s24-26 |
| init (pools mapped) | 13.9 (12.4) | 20.2 (18.6) | | 15.6 (14.0) | 20.3 (18.6) |
| bs (batch / top levels) | 37.4 (23.6 / 13.6) | 34.6 (23.4 / 10.9) | | 35.9 (22.6 / 13.2) | 32.8 (22.2 / 10.4) |
| dm (recip / division) | 31.9 (16.0 / 15.9) | 31.0 (15.2 / 15.8) | | 30.1 (15.1 / 15.0) | 29.6 (14.5 / 15.2) |
| phases | 69.3 | 65.6 | | 66.0 | 62.4 |
| **wall** | **83.3** | 85.9 | | **81.7** | 82.7 |
| digits | identical | identical | | identical | identical |

The products gain what A-grid predicted (−3.6…−3.7 s of phases, the first-use cost gone), the mapping of the 60 GB more
costs 4.7–6.3 s at init: pools 120 GB map in 12.4–14.6 s and 180 GB in 18.6 s here — 0.08–0.12 s/GB, not A-mem's
0.047, because the seed thread streams into the regions at the same time (the seeds' own "issuing" goes 3.2 → 6.2 s
under the mapping, and both end together: init = max(seeds ≈ 15–19 s, staging 1 + regions 4.8 + pools)). Mapping the
extra bytes later costs the same per byte (RESULTS §70: a background `hipMalloc` stalls the GPU levels), and nothing
already mapped is idle at the top levels (the donated arenas hold 84 of 96 GB live at their peak) or in the reciprocal
(121.5 of 141 GB). So the default is off. What would make it pay: cheaper mapping (the driver zeroes pages at ≈ 10–20 GB/s
per process), or the seeds off the init path, or M's tail layout removing the reciprocal's 35.6 GB of in-phase growth
(independent bytes, but then 60 GB at init against 0 inside the phases reads differently). `RNS_PLANES_3Q30=1` is the
fastest phase time measured on this code (62.4 s).

Could the planes be sized out of the region arenas instead (PLAN §23-7's "unless the planes replace part of the region
arenas")? The arenas (95.7 GB, sized to the batch levels' need + 1/8) are live through the batch tier and are donated to the
block pool at the first device-tier level, where the top levels' numbers reach 83.7 of the 95.7 GB; in the reciprocal the
pool holds 121.5 GB live of 141 (the 45.3 GB beyond the arenas are mapped inside the phases — 9.8 in bs, 35.6 = t1's quarter
in the reciprocal). The extra plane bytes (14 GiB per APU, 60 GB) are needed exactly at those two peaks, so no idle mapped
memory covers them: the arenas would have to be 60 GB larger, which is the same mapping at init. What would recover B3 is
M's tail layout for t1 (§23-5), which removes the 35.6 GB mapped inside the reciprocal: then the run with the planes maps
276.7 + 9.8 GB in total against today's 216.6 + 45.3 — 25 GB more instead of 60, i.e. ≈ +2 s of init against −3.6 s of phases,
a net −1.5 s (extrapolated from 0.08 s/GB); and if the seeds' streaming were moved off the pool-mapping window (the pools
mapped at 0.047 s/GB alone, A-mem), the 60 GB would cost 2.8 s and B3 would pay by itself. The code is ready for either:
`RNS_PLANES_3Q30=auto` gives the size rule.

## A2 — level 22 paired

The tree's level of 8 products at 2³⁰ points (4 pairs sharing Q₂) cannot go through the batch-local tier (a tile of one
product at 2³⁰ × 4 primes exceeds pool 0) and ran the striped path (one prime per device) unpaired: 24 transforms of 2³⁰ per
device, 2.16 s. Now `rns_mul_batch`'s striped path pairs products 2j, 2j+1 with the same B (the batch-local tier's
condition) and takes the radix-3 length when nc fits it (`pick_len`): per pair a₁, a₂ scattered into two planes and B once
(`k_scatter1`, the pair layout of `k_scatter4` for one prime), 3 forward transforms and the fused inverse with
`NTT_Y_PAIR` — 5 transforms of 3·2²⁸ points per pair instead of 6 of 2³⁰, in the pools as they are (a pair = 2 × 6 GiB in pool
0, 6 GiB in pool 1; the tile is capped by the pools' real capacities, so nothing grows inside the phase, and both forms are
dropped when the tile would not fit). Needs the GPU CRT (the host-CRT path reads 2^logL planes from the staging) and
registered operands (the staged fallback copies B per product). `RNS_STRIPED_PAIR=0` restores the old path; `RNS_BATCH_PAIR=0`
and `RNS_R3=0` apply as before.

4 × 10¹⁰ (job 20807, same node): level 22 `batch N=8 L=2^30 tile 1: 2.161 s (scatter 0.350 ntt 1.475 crt 0.367)` →
`batch N=8 L=3*2^28 tile 2 pair: 1.627 s (scatter 0.293 ntt 1.022 crt 0.342)`; wall 85.5 → 83.3 s (bs 37.75 → 37.40; the
rest of the difference is init's mapping noise, 16.2 vs 13.9). Digits identical; `t_mul 20` (189 checks) and `t_mul 0
batch` (binary, 72) pass; 10⁹ identical in both bases.

## §23-3 — `ECALC_DM_POOL` on from 5 × 10¹⁰

One comparison in the C3 block: `getenv("ECALC_DM_POOL") ? atoi(...) : d >= 5e10`. 7 × 10¹⁰ on the final build (job 20816,
s24-26, defaults): **153.5 s, VERIFY OK** — init 18.4, bs 67.3 (batch 37.7, top levels 24.9; the level of 14 products at
3·2²⁸ paired 2.7 s), dm 67.7 (recip 32.3: C3 grew the pool before it, largest block 15.6 GB; division 35.4); planes 16 + 12
GiB (off by the size rule and by the default); RESULTS §75: 159.7 s default, 157.2 s with the pool. 4 × 10¹⁰ is below the
threshold and unchanged (M: 85.9 vs 85.3 s with it, noise).

## I11 — init

The breakdown (`RNS_VERBOSE=1`, per APU: device open, contexts, stream + staging; then the pools and peer access; the
regions are allocated inside the seed hook before the pools since Phase 10 H): at 4 × 10¹⁰ staging + contexts 1.0–1.1 s
(contexts 0.3–0.8 s per APU in parallel — the HIP allocations, not the table arithmetic), regions 4.8–5.3 s (95.7 GB),
pools 12.4–15.7 s (120 GB, under the seed stream), peer access 0.3 s. Done: every context's tables in one buffer (one
`hipMalloc` + `hipMemcpy` instead of 22 pairs) and each pass's twiddles likewise (16 contexts × ~20 lengths × 4 passes × 2
directions of lazy builds inside bs: 4 allocations → 1 each); the engine-2 context only with `RNS_ENGINE=2`. Bit-identical
(`t_ntt 24` 565 checks, 10⁹ identical). Not done: sharing the tables across APUs (8 KB per context; reads over xGMI would
cost more in every pass than the upload does once) and pool clears by kernels (there are none left on the plane pools;
`mem_dev_alloc`'s `hipMemset` on a fresh allocation is 0.01 s for 22 GB — the mapping is in `hipMalloc` itself). Init on the
final build: 15.6–17.3 s at 4 × 10¹⁰ (RESULTS §75: 16.4 ± 1.1) — the two mapping steps and the seed thread are the whole of it.

## A4 — the tile budget in pair mode

`RNS_BATCH_TILE_GB` (rns_init) sets `rns_batch_tile_bytes`; 25.8 GB = 2²⁹ points per prime plane in pair mode (a + b/2
planes; the pools cap it). Job 20816, s24-26, planes off: batch tier 22.6 s (15 GB) vs 22.5 s (25.8 GB); the transforms
15.2 → 14.3 s summed over the levels (tiles 2× larger at every level, level 3·2²⁵ at tile 4 instead of 2), but the wall
81.7 vs 83.0 s (init 15.6 vs 17.3: the mapping noise). Kept at 15 GB; the knob stays.

## Tests

| test | command (from `ecalc/`) | result |
|---|---|---|
| t_mul 20, t_mul 0 batch (binary), t_ntt 24 | `./tests/t_mul 20`; `LIMB_BASE=2 ./tests/t_mul 0 batch`; `./tests/t_ntt 24` | VERIFY OK 189 / 72 / 565 |
| t_dbig 24 big with the planes | `RNS_PLANES_3Q30=1 ./tests/t_dbig 24 big` | the 2³⁰ × 2³⁰ product on the 3·2³⁰ plane checked, then killed at the 3.2e9-limb product (host memory for the GMP reference, as A-grid saw) |
| 10⁹ | `./ecalc 1000000000 out`, `LIMB_BASE=2`, `RNS_PLANES_3Q30=0/1` | identical to `ref/e_1000000000.txt` in every build (planes on: 20.1 s — init 16.3 vs 8.9, the same mapping story at 10⁹) |
| 10⁸ sizes 2, 4 | `SLURM_JOB_ID=$J ./mnrun.sh <p> env POOL_LOG=27 ./ecalc 100000000 out` | identical, all nodes VERIFY OK (the pools are sized per process; POOL_LOG 27 keeps the planes off in every mode) |
| 4 × 10¹⁰ | see the tables above and the gate below | identical in every run (9 runs) |
| 7 × 10¹⁰ | `./ecalc 70000000000 out` (defaults) | VERIFY OK, 153.5 s |

## Gate (five runs of 4 × 10¹⁰ on the final build, `~/p11/b3.sh`)

Job 20828, s24-26, `main`-equivalent defaults of this branch (planes off, A2 on, DM_POOL off below 5 × 10¹⁰, I11, tile 15 GB),
the reference evicted before every run, digits `cmp`'d after each:

| run | wall | phases | init | bs (batch / top levels) | dm (recip / division) | digits |
|---|---:|---:|---:|---|---|---|
| 1 | 81.99 | 65.8 | 16.2 | 35.6 | 30.1 (15.1 / 15.0) | identical, VERIFY OK |
| 2 | 82.18 | 65.8 | 16.4 | 35.4 | 30.3 (15.2 / 15.1) | identical, VERIFY OK |
| 3 | 82.74 | 66.6 | 16.1 | 36.0 | 30.6 (15.4 / 15.2) | identical, VERIFY OK |
| 4 | 79.13 | 65.5 | 13.6 | 35.4 | 30.0 (15.0 / 15.0) | identical, VERIFY OK |
| 5 | 81.26 | 66.1 | 15.2 | 35.7 | 30.3 (15.2 / 15.1) | identical, VERIFY OK |

**Wall 81.5 ± 1.4 s, phases 66.0 ± 0.4 s** (RESULTS §75: 83.0 ± 1.2, phases 66.7 ± 0.3 — bs 36.3 → 35.6, dm 30.1 → 30.3),
init 15.5 ± 1.1. The target of ≤ 79 s is not met: A2 is the only adopted phase gain (−0.55 s); the spread of the wall is
the init mapping (13.6–16.4 s for the same bytes). With `RNS_PLANES_3Q30=1` the phases are 62.4 s but the wall 82.7 s (the
same-node pair in the B3 table). 10⁹ identical in both bases on the final build (14.4 s decimal, 26.4 s binary).

## Open issues

* B3's bytes: the 60 GB of larger planes pay 4.7–6.3 s of mapping at init for 3.6 s of products. The mapping rate under
  the seed stream (0.08–0.12 s/GB) is the number to attack: the seeds and the pool mapping contend (both ≈ 15 s alone,
  20 s together with the planes). Off by default; `RNS_PLANES_3Q30=1` for the phase time, `auto` for the size rule.
* The transform cache at size 1 (G, not adopted for the same mapping reason) could share B3's planes when the product is
  not radix-3 — not tried.
* `t_dbig 24 big` with the planes cannot finish on a node (the host reference product), as before.
* The striped path's pair mode is taken only by the tree's level 22 at 4 × 10¹⁰ (todec's batches share one B: `grpB`).
