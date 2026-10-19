# SINGLE_NODE — single-node speed and options

Scope: single-node speed and options: init, level pairing, remaining options on the defaults, development items, seed terms, combined switches. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| P | single-node speed: B3 planes at init, A2 level-22 pairing, DM_POOL default, I11 init, A4 tile budget (Phase 11, PLAN §26 |
| V214 | the remaining one-node options on the Phase 14 defaults: auto's per-piece cost, DIST_TWREC, checkpoints with O_DIRECT (P |
| V314 | Phase 14 agent V3: the remaining development items (PLAN §33.2 B1–B7, APUMULT E11), with numbers |
| T215 | Phase 15, row 1c: `BS_SEED_TERMS` chosen per run so the batch tier fills its transform lengths |
| CAND | the three off-by-default speed switches combined (interaction test) |
| S21 | single-node A37 measurements (R2 seed microbench, Q1 init timeline, R4 dc split, R6 small-j doublings, Q4 NTT structure) |
| B08 | how fast can the unit tests be? (2026-10-11, branch b08) |

## P — single-node speed: B3 planes at init, A2 level-22 pairing, DM_POOL default, I11 init, A4 tile budget (Phase 11, PLAN §26)

(former file: P.md)

Branch `p11` (from `main` @ 72aa2e9; aac6 clone `~/ntt-p`). Files: `ecalc/rns_mul.c` (pools, the striped batch path),
`ecalc/rns_mul.h` (additive), the init lines of `ecalc/ecalc.c` (the plane switch, the C3 default), `ecalc/ntt.c` (the
context tables' upload only). Touched outside my list, minimal and commented `Phase 11 ... (agent P)`: `ecalc/rns_dist.c`
(L's file) — `dist_r3()` defaults to the plane switch, `dist_core` takes `xb | sbuf | rbuf` from pool 1 when it holds them,
the grid cost model counts a 3·2^k plane at 21/20 per point and the one-plane form is taken only when no grid is cheaper
(`plane_pts`/`split_grid_cap` gained an `r3` argument; the mn tier passes 0). Every run below is on aac6 (jobs 20807 on
s24-30, 20816 on s24-26, the gate job below), 4 × 10¹⁰ with the reference evicted before each timed run and the digits
`cmp`'d against `~/ntt/ecalc/results/e_4e10.out` after it; logs in `~/p11/out{1,2,3}/` on aac6.

#### Summary

| item | switch (default) | measured at 4 × 10¹⁰ | adopted |
|---|---|---|---|
| B3: 3·2^k-point planes sized at init (pool 0 = 3·2³⁰ limbs = 24 GiB, pool 1 = 3q + 16 = 18 GiB per APU) | `RNS_PLANES_3Q30=0/1/auto` (**0**) | phases −3.6 s (top levels 13.2 → 10.4, reciprocal −0.6, level 21 paired −0.2; the 15 s first-use cost of A-grid's C5 gone), but the 60 GB more of pools cost **+4.7 / +6.3 s at init** (the pools map at 0.08–0.10 s/GB while the seeds stream): wall +1.0 / +2.6 s | no — code and switch kept; `auto` = on below 5 × 10¹⁰ at 2³¹ pools (the size rule asked for) if the mapping ever gets cheaper |
| A2: the level-22 products (8 at 2³⁰ points, 4 pairs) paired in the striped batch path on 3·2²⁸ planes | `RNS_STRIPED_PAIR=0/1` (**1**) | level 22: 2.16 → 1.55–1.63 s (**−0.55 s**), the same pools (a pair = 12 + 6 GiB); at 7 × 10¹⁰ the level of 14 products 2.7 s | yes |
| §23-3: `ECALC_DM_POOL` on from 5 × 10¹⁰ | `ECALC_DM_POOL=0/1` (**on at d ≥ 5 × 10¹⁰**) | 7 × 10¹⁰: **153.5 s** (was 159.7 default, 157.2 with the pool; A2 gives the rest), VERIFY OK, planes automatically off (16 + 12 GiB) | yes |
| I11: init — the context tables and pass twiddles uploaded in one `hipMalloc` + `hipMemcpy` each (22 pairs per context, 4 per pass before); engine 2's context built only for engine 2; the init breakdown printed | — | contexts 0.3–0.8 s of the 1.0 s "staging+contexts" (inside the parallel per-APU loop); init is regions 4.8 s + pools 14.0 s of mapping, the seed thread (15 s) alongside — nothing left to share across APUs (the tables are 8 KB per context; sharing them would put twiddle reads on xGMI); no `hipMemset` remains on the plane pools (`mem_dev_alloc`'s memset of a fresh allocation is 0.01 s) | yes (bit-identical, `t_ntt` 565 checks) |
| A4: the batch tier's tile budget in pair mode | `RNS_BATCH_TILE_GB` (**15**) | 25.8 GB (2²⁹ points per prime plane): batch tier 22.6 → 22.5 s, transforms −0.9 s of 15.2 spread over the levels but the wall the same (83.0 vs 81.7, the noise of init) | no — knob kept |

#### B3 — the 3·2^k planes sized at init

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

#### A2 — level 22 paired

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

#### §23-3 — `ECALC_DM_POOL` on from 5 × 10¹⁰

One comparison in the C3 block: `getenv("ECALC_DM_POOL") ? atoi(...) : d >= 5e10`. 7 × 10¹⁰ on the final build (job 20816,
s24-26, defaults): **153.5 s, VERIFY OK** — init 18.4, bs 67.3 (batch 37.7, top levels 24.9; the level of 14 products at
3·2²⁸ paired 2.7 s), dm 67.7 (recip 32.3: C3 grew the pool before it, largest block 15.6 GB; division 35.4); planes 16 + 12
GiB (off by the size rule and by the default); RESULTS §75: 159.7 s default, 157.2 s with the pool. 4 × 10¹⁰ is below the
threshold and unchanged (M: 85.9 vs 85.3 s with it, noise).

#### I11 — init

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

#### A4 — the tile budget in pair mode

`RNS_BATCH_TILE_GB` (rns_init) sets `rns_batch_tile_bytes`; 25.8 GB = 2²⁹ points per prime plane in pair mode (a + b/2
planes; the pools cap it). Job 20816, s24-26, planes off: batch tier 22.6 s (15 GB) vs 22.5 s (25.8 GB); the transforms
15.2 → 14.3 s summed over the levels (tiles 2× larger at every level, level 3·2²⁵ at tile 4 instead of 2), but the wall
81.7 vs 83.0 s (init 15.6 vs 17.3: the mapping noise). Kept at 15 GB; the knob stays.

#### Tests

| test | command (from `ecalc/`) | result |
|---|---|---|
| t_mul 20, t_mul 0 batch (binary), t_ntt 24 | `./tests/t_mul 20`; `LIMB_BASE=2 ./tests/t_mul 0 batch`; `./tests/t_ntt 24` | VERIFY OK 189 / 72 / 565 |
| t_dbig 24 big with the planes | `RNS_PLANES_3Q30=1 ./tests/t_dbig 24 big` | the 2³⁰ × 2³⁰ product on the 3·2³⁰ plane checked, then killed at the 3.2e9-limb product (host memory for the GMP reference, as A-grid saw) |
| 10⁹ | `./ecalc 1000000000 out`, `LIMB_BASE=2`, `RNS_PLANES_3Q30=0/1` | identical to `ref/e_1000000000.txt` in every build (planes on: 20.1 s — init 16.3 vs 8.9, the same mapping story at 10⁹) |
| 10⁸ sizes 2, 4 | `SLURM_JOB_ID=$J ./mnrun.sh <p> env POOL_LOG=27 ./ecalc 100000000 out` | identical, all nodes VERIFY OK (the pools are sized per process; POOL_LOG 27 keeps the planes off in every mode) |
| 4 × 10¹⁰ | see the tables above and the gate below | identical in every run (9 runs) |
| 7 × 10¹⁰ | `./ecalc 70000000000 out` (defaults) | VERIFY OK, 153.5 s |

#### Gate (five runs of 4 × 10¹⁰ on the final build, `~/p11/b3.sh`)

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

#### Open issues

* B3's bytes: the 60 GB of larger planes pay 4.7–6.3 s of mapping at init for 3.6 s of products. The mapping rate under
  the seed stream (0.08–0.12 s/GB) is the number to attack: the seeds and the pool mapping contend (both ≈ 15 s alone,
  20 s together with the planes). Off by default; `RNS_PLANES_3Q30=1` for the phase time, `auto` for the size rule.
* The transform cache at size 1 (G, not adopted for the same mapping reason) could share B3's planes when the product is
  not radix-3 — not tried.
* `t_dbig 24 big` with the planes cannot finish on a node (the host reference product), as before.
* The striped path's pair mode is taken only by the tree's level 22 at 4 × 10¹⁰ (todec's batches share one B: `grpB`).

## V214 — the remaining one-node options on the Phase 14 defaults: auto's per-piece cost, DIST_TWREC, checkpoints with O_DIRECT (Phase 14, agent V2)

(former file: V214.md)

Branch `p14-V2` from `p14-defaults` @ 4855842 (all Phase 14 defaults on). aac6 clone `~/ntt-V214`; scripts and logs in
`~/V214/` (`job.sh`, `chain.sh`, `<batch>.steps`, `<batch>/<tag>.log|.sha`). Labels: **measured** (aac6, node named),
**modelled** (`mn_model.py`, `MN_PLAN_ONLY`), **assumed**.

#### 1. C2 — `RNS_AUTO_PIECE_COST=1` (off by default)

##### Design

`rns_dist.c` `split_grid` (auto's grid, `RNS_STRATEGY_GRID`): a grid of more than one piece now also pays, per piece,
the pipeline law's per-piece terms (results/TIME_MODEL.md §D213d, fit_pipe on G13d's logs: + 0.075 s per 2³¹ B points of the piece
+ 0.079 s per 2³¹ limbs of the whole product, its accumulation pass over C). In split_grid's units (C plane points; the
pipeline's C product costs ≈ 0.86 s per 2³¹ points, `mn_model.t_prod × PIPE_ONE` at 2²⁶–2³⁰, **modelled**) that is
`+ 0.087 × B points + 0.092 × nc` per piece. The one-plane choice (1 × 1, C form) is in the same loop, so a product that
fits one C plane but not the B pools can now stay whole. The mn tier (`split_grid_cap` with minpts) and the C strategy
are unchanged. The digits do not depend on the grid.

##### MN_PLAN_ONLY at size 1 (login node, `MN_PLAN_ONLY=<D>:1 ./ecalc`, off → on; **modelled** = the code's own plan)

| digits | recip pieces | div pieces | total | the large products |
|---|---|---|---|---|
| 1e10 | 58 → 58 | 2 → 2 | 60 → 60 | unchanged |
| 4e10 | 68 → 66 | 16 → 10 | 84 → 76 | |
| 7.64e10 | 73 → 73 | 25 → 25 | 98 → 98 | unchanged |
| 1e11 | 97 → 87 | 59 → 40 | 156 → 127 | div A_h mu / X Q: 7 × 7 B 3·2²⁹ (28 / 31 formed) → 5 × 6 C 2³¹ (20 / 20); recip Q_t r 9 × 3 → 5 × 3; leaf levels 25/26: 3 × 3 → 2 × 3, 8 × 1 → 5 × 1 (C 2³¹) |
| 1.16e11 | 120 → 100 | 86 → 52 | 206 → 152 | div X Q 7 × 10 B (43) → 5 × 8 C 2³¹ (26) |
| 1.3e11 | 103 → 102 | 101 → 61 | 204 → 163 | div X Q 9 × 9 B (48) → 6 × 8 C 2³¹ (30) |

The pipeline law (**modelled**, `mn_model._law_piece`) on the changed products: 1e11 X Q 30.6 → 28.0 s, recip Q_t r
21.2 → 18.2 s, A_h mu 27.6 → 28.0 s, leaves ≈ equal; 1.16e11 X Q 45.3 → 38.1 s; 1.3e11 X Q 53.3 → 45.7 s.

##### Measured (s24-16, job 21443, same node back to back; `ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc <D> /tmp/v2_<tag>.txt`, sha1 by `dd iflag=direct`)

| digits | switch | total s | bs | dm (recip) | T1 | division corrections | products C + B (dist_core s) | sha1 / digits |
|---|---|---|---|---|---|---|---|---|
| 1e11 | off (defaults) | **234.55** | 96.7 | **99.9** (41.7) | 20.9 | 0/2 | 14 + 198 (11.97 + 68.25) | 578f5efb… = e_1e11.out: **identical** |
| 1e11 | `RNS_AUTO_PIECE_COST=1` | **206.02** | 95.7 | **94.6** (38.8) | 0.0 | 0/0 | 90 + 81 (75.79 + 11.07) | 578f5efb… = e_1e11.out's sha1 (read in job 21443): **identical**, VERIFY OK |
| 4e10 | `RNS_AUTO_PIECE_COST=1` | 64.22 | 31.2 | 22.6 | 0.0 | | | 43c84e49… = e_4e10.out's sha1: **identical** |

Above 10¹¹ (s24-26, job 21447, same node back to back; no reference at these sizes: the pair's sha1s equal, VERIFY OK each):

| digits | switch | total s | bs | dm (recip) | T1 | corrections | products C + B (dist_core s) | dc | sha1 |
|---|---|---|---|---|---|---|---|---|---|
| 1.16e11 | off | **287.74** | 116.7 | **149.3** (60.2) | 0.0 | 0/0 | 3 + 275 (2.55 + 107.72) | 19.5 | 9f836740… |
| 1.16e11 | on | **258.80** (−10.1 %) | 112.3 | **130.0** (54.5) (−12.9 %) | 0.0 | 0/0 | 116 + 90 (95.37 + 15.46) | 26.5 | 9f836740… (same) |
| 1.3e11 | off | **325.17** | 141.6 | **163.7** (59.6) | 0.0 | 0/0 | 37 + 259 (30.47 + 101.27) | 22.3 | b1946e71… |
| 1.3e11 | on | **304.25** (−6.4 %) | 136.7 | **147.2** (59.5) (−10.1 %) | 0.0 | 0/0 | 138 + 95 (114.03 + 18.18) | 25.9 | b1946e71… (same) |

Here no run corrected, so the gain is the grids alone: dm −5 % at 1e11, −13 % at 1.16e11, −10 % at 1.3e11, and bs
−1…−5 s (its top leaf levels, 2³¹ C pieces instead of 3·2²⁹ B). The dist_core seconds rise (C products are slower per
point) but the grids' accumulation passes and piece overheads fall by more — what D213d's law predicted. (For
comparison, D213d's auto dm at 1.16e11 on the pre-Phase-14 defaults was 162–171 s.) dc (the file write after
`total`) is 3–7 s longer with the switch in these runs; it is not on `total`'s path.

**Verdict C2: adopt `RNS_AUTO_PIECE_COST=1`** (one node, ≥ 4 × 10¹⁰: −12.2 % / −10.1 % / −6.4 % of `total` at 1e11 /
1.16e11 / 1.3e11, digits identical, 4e10 identical and no slower: 64.2 s against the 63.5 ± 1.5 s default history). It
does not touch the mn tier (the 576 target's distributed products), only the size-1 / node-local auto grids.

(The integrator's defaults run at 1e11 on s24-16 an hour earlier: total 232.42, dm 99.4, T1 20.9 — the same as ours.)

**Where the 28.5 s come from (measured):** dm −5.3 s (−5.3 %: recip −2.9 s, the division −2.4 s) and T1 −20.9 s.
The T1 part is the division's correction count: with the defaults the A_h mu product is a 7 × 7 grid with 21 pieces
skipped below the low cut (each skipped piece can leave X low by one; results/DIVISION.md §A-div B3), and the run took 2
up-corrections. The writer thread starts formatting X at the hook, before the corrections; a correction after it makes
`out_stage` join the writer (T1 waits 20.9 s: `ecalc.c` `xres_bg = … && !c->ncorr`) and then **redo the digits and
rewrite the file** ("X corrected after the formatting started": dc 57.0 s against 22.5 s; dc runs after `total`, so the
srun wall is 294 s against 231 s — 63 s end to end). The 5 × 6 grid skips 10 pieces and took 0 corrections. The
correction count is data-dependent, so this part is not guaranteed at every size (fewer skipped pieces make it less
likely). **Finding (not changed here):** at 1e11 the defaults pay ≈ 21 s of `total` and ≈ 35 s more of dc for two
corrections; a correction test before the hook (or fewer low-cut skips in A_h mu) would remove it on the defaults too.

#### 2. DIST_TWREC=1 at 1e11 — NOT MEASURED (stopped by the integrator)

Batch C (job 21451, s24-16, 21:08 EDT) was cancelled after 3.5 min of its first run (`tw_def1`), on the integrator's
instruction that job 21447 was the last node batch tonight; no result. The steps are ready in `~/V214/C.steps`:
defaults / `DIST_TWREC=1` × 2 interleaved at 1e11, `RNS_AUTO_PIECE_COST=1 DIST_TWREC=1`, and a second
`RNS_AUTO_PIECE_COST=1` run (`bash ~/V214/chain.sh any now C`, ≈ 35 min).
**Expectation (modelled from the evidence, not measured):** on the defaults `DIST_TWREC` only reaches the C-form
(four-step) products; at 1e11 those are 14 products, 11.97 s of dist_core in a 234.5 s run (A3's log above), and the
pack/unpack is a fraction of that — X13b's kernel gain (×1.4–1.65 on the pack) bounds the saving at well under 1 % of
`total` (X13b measured no gain at 4e10). With `RNS_AUTO_PIECE_COST=1` the C-form share grows to 90 products / 75.8 s
at 1e11, so if TWREC is ever worth adopting it is together with C2; that is the run to make. **Verdict: no evidence to
adopt; open.**

#### 3. Checkpoints with O_DIRECT — NOT MEASURED (stopped by the integrator)

Batch D was never started. Its steps are ready in `~/V214/D.steps`: at 1e11 `ECALC_CKPT_TOP` off / =1 / =2 (the top set,
≈ 89 GB, into `<outfile>.top` on the node's NVMe `/tmp`: 1.1–1.2 TB free on s24-16/26 measured), then size 2 on one node
at 10¹⁰: no checkpoints / tree sets only (`BS_CKPT_DIR`, `BS_CKPT_MIN_LEVEL=99`) / leaf + tree sets, then the e9 pair by
hand. The lines to read: `checkpoint: … GB/s`, `waited … for P before S = P + Q, … for Q …, … at the join`, dm and
`total`. **Verdict: open.** (Known before: 113 s at 0.31 GB/s buffered, Phase 12; the node's NVMe took 1.1–1.6 GB/s in
W's measurement, where the model says the P wait would vanish at ≳ 1.1 GB/s at 4e10 — the 1e11 set is 2.5 × larger with
a reciprocal ≈ 2.5 × longer, so the same threshold applies, **modelled**.)

#### RESUME

- 17:30 EDT: code committed (7f61890), clone `~/ntt-V214` built, plan-only checks done (above).
- 17:32 EDT: `~/V214/chain.sh aac6-spx-b 17:22 A B C` launched (setsid; job 21431 begins 17:22 CDT = 18:22 EDT,
  after the integrator's window). A = mnaccept unit,e9 + C2 at 1e11 (off/on) + 4e10 on (sha vs e_4e10); B = C2 at
  1.16e11 / 1.3e11; C = DIST_TWREC 1e11 × 2 each. D (checkpoints) not yet launched: check `/tmp` space in `A/node.txt`
  first (1e11 output 100 GB + top set ≈ 89 GB), then `bash chain.sh <node> now D`.
- 17:32–17:52 CDT job 21431 (s24-26): `./mnaccept.sh 21431 --only unit,e9`: **9 passed, 1 failed (unit t_bs: rc 139,
  segfault in section 1)**; e9 identical in both bases (11.45 s b10, 20.43 s b2). The batch then stopped: mnaccept read
  the steps file from stdin (job.sh fixed: every command `< /dev/null`). t_bs passed at 4c5d2b3 (int14e, job 21290);
  suspected cause: DB_POOL_VMM=1 by default (b257753) — t_bs writes pool memory from the host. Being checked in A2.
  /tmp on s24-26: 1.2 TB free NVMe.
- 17:55 CDT `chain.sh any now A2 B C D` (job 21441…): A2 = t_bs on the base 4855842 (`~/ntt-V214-base`), on this
  branch with DB_POOL_VMM=0 and default, then C2 at 1e11 off/on, 4e10 on, the e_1e11 reference's sha1.
- 18:28 CDT job 21441 (s24-26): **t_bs segfaults on the base 4855842 too** (rc 139, section 1) and passes with
  `DB_POOL_VMM=0` (VERIFY OK, 10 checks): the t_bs failure is the Phase 14 default DB_POOL_VMM=1, not this branch
  (for the integrator). Both 1e11 runs stopped at the budget check (rc 8: 513 GB counted) — the integrator's 2085d1e
  fixes the report; **branch rebased onto 2085d1e** (p14-defaults head), clone rebuilt, chain relaunched:
  `chain.sh any now A3 B C D` (A3 = mnaccept e9, C2 1e11 off/on, 4e10 on, t_bs, ref sha1).
- 18:35–19:06 CDT job 21443 (s24-16): C2 1e11 pair + 4e10 above (all identical: e_1e11.out sha1 578f5efb… — read from
  the shared FS in 943 s, kept in `~/V214/e_1e11.sha1`; do not re-read it). `mnaccept.sh --only e9` failed there with
  "mn_out: cannot open /tmp/mnaccept_21443/e9_b10.txt" (the harness's `/tmp` dir missing on s24-16 — VERIFY OK, not a
  digit difference; the e9 pair is rerun by hand at the end of D). t_bs on the rebased branch: segfault (the VMM default).
- 19:06 CDT B running (job 21447): C2 at 1.16e11 / 1.3e11. Then C (TWREC + a second pc run at 1e11), D (checkpoints + e9).
- 20:31–21:01 EDT job 21447 (s24-26): B done (above). 21:08 EDT C started (job 21451, s24-16); 21:12 EDT cancelled with
  the chain, on the integrator's instruction (no more node jobs tonight). No V2 job queued or running; `/tmp/v2_*` on
  s24-16 checked empty.
- **Done / open.** Done: C2 implemented, planned, measured at 1e11 / 1.16e11 / 1.3e11 (+ 4e10), verdict adopt. Open:
  items 2 and 3 (steps ready: `bash ~/V214/chain.sh any now C D` from `~/V214`, ≈ 35 + 30 min); the e9 gate on the
  rebased head (passed on 7f61890, the same rns_dist.c; the rebase added only 2085d1e's report fix); t_bs vs the VMM
  default (integrator); the correction-after-hook cost (§1).

## V314 — Phase 14 agent V3: the remaining development items (PLAN §33.2 B1–B7, APUMULT E11), with numbers

(former file: V314.md)

Branch `p14-V3` from `p14-defaults` @ 4855842. **No pipeline code changed.** Throwaway prototypes and scratch models only:
`tests/v3_brk.py` (the model's phase breakdown), `tests/v3_span_bench.c` (the seed spans, four forms), `tests/v3_r3_bench.c` (the
radix-3 pass inside a 3·2ᵏ transform), `tests/v3_b2.py` (the middle product), `tests/v3_b4.py` (more transform lengths),
`tests/v3_e11.py` (band-sized t1 and the chunked window), `tests/v3_node_batch.sh` (the one node job). The node logs are in
`results/V314/`. Every number is labelled **measured**, **modelled** or **assumed**.

#### 0. Answer in one table (ranked by benefit per day of effort)

Reference walls: **10¹¹ on one node, the current defaults: 215.9 / 217.1 s** (measured, s24-30, job 21436: init 17.2 / 18.5, bs
96.3 / 96.6, dm 101.6 / 101.2, recip 43.0 / 42.7). **576 nodes at 4.25 × 10¹³: 244.9 s** (modelled, `estimate.py --target --chunk both`,
the fabric assumed).

| rank | item (code) | 10¹¹ one node | 576 target | memory | effort | risk | per day |
|---|---|---|---|---|---|---|---|
| **1** | **B1 / TASKS 2.2 the decimal `mul_1` and a fused span** (`bigint.c mul1_serial`, `binsplit.c span`) | **−9 s (−4 %)**, to the mapping floor (modelled from measured parts) | **−9…−11 s (−4 %)** (modelled; the model lacks this term, §1) | 0 | ½–1 d | low (exact by construction; prototype 0 mismatches) | ≈ 12 s/d |
| **2** | **BS_SEED_TERMS chosen per run** so the batch tier's products fill their lengths (a rule in `binsplit.c seed_limbs`) | **−6.6 s (−3 %)** measured, one run (S = 168); 4 × 10¹⁰: **−5.5 s (−8 %)** (S = 236) | −3…−7 s (assumed: the leaf's bs is the same code) | +0.9 GB of regions | ½ d + a series | low (digits exact; a knob that exists) | ≈ 10 s/d |
| **3** | **B5: the radix-3 pass fused into the first 2ᵏ pass** (`ntt3.c k_r3_fwd / k_r3_inv` + `ntt.c`), and `pick_len` preferring 2ᵏ⁺² at k ≤ 12 (`rns_mul.c`) | **−6…−8 s (−3…−4 %)** (modelled from the measured pass share: 20 % of a 3·2²⁹ transform, 33–50 % at 3·2¹⁰–3·2¹⁷) | −3…−4 s (−1.5 %) (the leaf only; the mn tier has no 3·2ᵏ) | 0 | 2–3 d | medium (new kernels; outputs canonical, so bit-identical) | ≈ 3 s/d |
| 4 | **B2: the reciprocal's Q_t·r as a middle product** — the grid form (`newton_db.c recip_db2 / recip_mn`: mul_grid's lowcut **and highcut**) | −3.4 s (−1.6 %) (modelled, scaled to the measured recip) | −1.0 s (−0.4 %) (modelled) | −(t1's top third) = E11's first half | 1 d (+½ d for the wrapped one-plane form: −0.5 s more) | low (R114's argument extends: two guard limbs) | ≈ 3 s/d (1e11), 1 s/d (target) |
| 5 | B5: the general-map twiddle packs (`rns_dist.c k_twpack_g / k_unpacktw_g`, a `DIST_TWREC`-style recurrence) | 0 | −2…−3 s (−1 %) (assumed shares) | 0 | 1–2 d | medium; only measurable multi-node | ≈ 1.7 s/d (target only) |
| 6 | E11 (V6): band-sized t1 + the division's window in chunks (`binsplit.c dm_layout`, `newton_db.c`) | 0 s; **one-node ceiling 1.41 → 1.52 × 10¹¹ at 480 GB, 1.50 → 1.64 × 10¹¹ at 502 GB** (modelled) | 0 s; −17 GB per node (424 → 407), 576 ceiling 9.2 → 9.8 × 10¹⁰ per node (modelled) | the point of it | 3–4 d (the band part shared with 4) | medium (layout + `mem_model --check-c`) | memory only |
| 7 | B5: the 2³¹ plan's 5-stage top pass (`ntt.c`) | ≤ −1 s (modelled from K13b: 4 ms of ≈ 99 ms per 2³¹ transform) | ≤ −0.3 s | 0 | 1–2 d | medium | < 1 s/d |
| 8 | B4: transform lengths 5·2ᵏ, 7·2ᵏ | −3 s (the batch tier's padding; assumed 1.10 × per point) — **the same padding is free through item 2** | −0.3…−0.8 s (modelled); **does not soften the target's steps** (they are counts of pieces at the 2⁴⁰ cap) | 0 | **≥ 2 weeks: needs a new prime set** (no current prime has 7 \| p − 1; only p₀ has 5) | high (CRT margin, every kernel) | ≈ 0 |
| — | B1 / TASKS 2.1 (seeds before the planes), 6.4 (seeds on the GPU) | ≈ 0 after item 1 (the mapping floor binds, §1) | ≈ 0 after item 1 | 0 / + | ½ d / 2–3 d | — | only if the mapping gets faster |
| — | B2's piece loading (TASKS 2.3) | closed: recip 45.6–45.9 s → **43.0 / 42.7 s** measured (A5's `RNS_PLANES_FIRST` + `NEWTON_RECIP_CUT`) | — | — | — | — | done |
| — | B5: `ntt3.c` on the reduced-correction modmul; the s_lo 17/24 penalty | ≈ 0: the r3 pass runs at 1.58 TB/s (memory-bound, measured); the penalty is left only at 2²⁴ (≤ 0.2 s) | ≈ 0 | 0 | ½ d | — | subsumed by 3 |
| — | B6: tier balance (APU 3 idle in B / mdev), B4's push, B at size > 1 | ≤ 0.3 s at 4 × 10¹⁰ (P3); B4's push = its saving (B13b) | 0 (the mn tier is C by design) | — | — | — | not recommended |
| — | B7: CPX (H1), SDMA (H5) | CPX splits every APU into 6 devices, against the dist tier; SDMA: the X fetch is 0.87 s off the critical path, checkpoints are off | ≈ 0 | — | admin / 1–2 d | — | not recommended |

**Recommended order**: 1 → 2 (then a five-run series at 10¹¹ and 4 × 10¹⁰ with both) → 3 → 4 (with E11's band part if the
one-node ceiling matters) → 5 when multi-node time is available → C1 (refit the model, which also lacks item 1's term and
`NEWTON_RECIP_CUT` at 576, §8). E11 only if > 1.5 × 10¹¹ on one node is wanted; B4, B6, B7 not recommended. Items 1 + 2 + 3 + 4
together: ≈ −25 s at 10¹¹ (−12 %, not additive exactly: item 2's seed share shrinks after item 1), ≈ −15…−20 s at the target (−6…−8 %).

#### 1. B1 — initialization and the seeds (the largest item, and a term the model lacks)

**What it is.** `binsplit.c binsplit_seeds_begin` starts a thread (`pre_seeds_run → seeds_stream → seed_spans → span`) that computes
the level-0 spans (256 terms each, Horner: `bi_add(P, P, Q); bi_mul_u64(Q, Q, k)`) on all CPU cores and, under `DB_POOL_VMM`
(default), stages them through two 8 GB pinned buffers and a DMA into the region arenas. `binsplit_e` joins it before level 1.
Since `RNS_PLANES_FIRST=1` (default) the thread starts **after** the plane pools (`rns_mul.c` 177).

**Measured at 10¹¹ on the defaults** (two runs, `results/V314/e11_def*.log`):

| | run 1 | run 2 |
|---|---|---|
| init | 17.2 s | 18.5 s |
| the seed thread | 30.9 s: waited 5.3 for the regions, **spans 22.0**, buffers 1.6 + 0.1, DMA 0.7 + issuing 1.3 | 31.1 s: 5.0, **21.9**, 1.4 + 0.2, 0.7 + 2.0 |
| **bs waiting for the seeds after init** | **19.7 s** | **20.5 s** |

So the seeds hold level 1 back by ≈ 20 s — **9 % of the 10¹¹ wall**. (G13d's runs on the older defaults waited 13.1–13.8 s at
1.03 × 10¹¹ and 4–8 s at 7.7 × 10¹⁰; the VMM staging and the planes-first order added ≈ 6 s.)

**The prototype** (`tests/v3_span_bench.c`, the node's CPU, 96 threads, 400 000 spans, **measured**, 0 mismatches against the
pipeline's `span()` on every 97th span):

| k (the term index) | pipeline `span()` | widened Barrett (shift 40) | fused P += Q; Q ·= k, quotient by a double estimate + exact integer fix, carry off the chain |
|---|---|---|---|
| 4 × 10⁹ (< 2³³: the fast path; every one-node run ≤ 8 × 10¹⁰) | 3.36 ns/term | ×1.10 | **×1.69** |
| 10¹⁰ (> 2³³: `u128 % 10¹⁸` and `/`) | 4.38 | ×1.65 | **×2.11** |
| 3.5 × 10¹² (the 576 target's top node: **every** term) | 5.68 | ×1.45 | **×2.19** |

Per limb-step the slow path costs ×1.29 the fast one on Zen 4 (×2.0 on aac6's login node, Zen 2; ×5 on an older Intel).
`mul1_serial` takes its Barrett step only for m < 2³³; **at the target every node but the first two has k > 2³³**, so every
one-node measurement understates the target's seeds by ≈ 29 %.

**Benefit.** The seeds cannot finish before the arena's second half is mapped (level 1 writes into it): init 17.2 s + the second
half (≈ 130 GB at the measured 0.084 s/GB of `region pools`, 10.9 s for the first half) ≈ **28 s** (modelled floor). Today level 1
starts at 36.9–39 s. With the fused form the spans take ≈ 22.0 / 1.75 ≈ 12.6 s, the seeds end at ≈ 27.5 s: **−9 s at 10¹¹** (to the
floor; modelled on measured parts). At the target's top node (7.64 × 10¹⁰ digits, k ≈ 3.5 × 10¹²): spans ≈ 7.64 × 2.10 s × 1.29 ≈ 20.7 s
today → 9.4 s; the wait today ≈ 18 s on this structure (derived), the floor ≈ init + 8 s → **−9…−11 s** (modelled; less if the
multi-node init — meshes, the 45 GB SHMEM pool — is longer and already hides part of the seeds).

**The model lacks this term**: `mn_model`'s init/bs laws are fitted on G13d's one-node runs (older defaults, k < 2³³, bs wait 4–8 s at
7.7 × 10¹⁰). Under the current defaults and the target's k the top node's seed wait is ≈ +10 s above what the model carries; item 1
removes about that much. So item 1 mostly *secures* the modelled 245 s rather than beating it.

**2.1 and 6.4.** Starting the thread before the planes gains ≤ 5 s of region waiting today, and ≈ 0 once item 1 puts the seeds at
the mapping floor. Seeds on the GPU (≈ 1–3 s of kernels, modelled: 7.4 × 10¹¹ limb-steps at 10¹¹) likewise gain nothing more until the
mapping floor drops (VMM maps at 0.077–0.084 s/GB against hipMalloc's 0.036–0.05). Recommended: item 1 only; revisit 6.4 if the
mapping gets faster.

**Effort / risk.** ½–1 day: `span()` as one fused pass per term with the double-estimate quotient (exact: |q_est − q| ≤ 1 for
m < 2⁵⁰, the remainder corrected in 64-bit arithmetic), `mul1_serial`'s Barrett widened to m < 2⁴⁴ for the other callers; a unit
test against the current `span()` for random k up to 2⁴⁴ and against GMP; e9 both bases; 4 × 10¹⁰ identical.

#### 2. The seed span as a padding knob (`BS_SEED_TERMS`) — found here

The batch tier's first products have nc = 2 × (the span's limbs) and take the next 2ᵏ (3·2ᵏ only from 3·2¹⁰): at 10¹¹, 286 limbs
→ 512 points (1.79 ×), then 1.34 × for every level from 4 on. The span length sets where the levels fall. **Measured, one run each**
(`results/V314/e11_s168.log`, `e4_*.log`, VERIFY OK):

| run | total | init | bs (seeds wait / batch / mdev) | dm | batch-tier ntt |
|---|---|---|---|---|---|
| 10¹¹, S = 256 (default), 2 runs | 215.9 / 217.1 | 17.2 / 18.5 | 96.3 / 96.6 (19.7–20.5 / 37.3 / 38.7–39.0) | 101.6 / 101.2 | 25.3 |
| 10¹¹, **S = 168** (nc 188 → 256, 3072 from level 5) | **209.3** | 16.8 | **86.7** (14.8 / 37.2 / 34.5) | 105.0 | 21.2 |
| 4 × 10¹⁰, S = 256 | 66.7 | 11.1 | 32.0 (8.2 / 15.7 / 8.1) | 23.5 | 10.6 |
| 4 × 10¹⁰, **S = 236** (nc 253 → 256 at every level) | **61.1** | 11.6 | **26.3** (8.3 / 13.1 / 4.8) | 23.1 | 7.5 |
| 4 × 10¹⁰, S = 176 | 62.2 | 12.8 | 26.2 (6.3 / 15.7 / 4.1) | 23.1 | 8.8 |

The gains come from three places: fewer seed CPU (∝ S), less padding in the batch tier, and a different split of the top levels between
the batch and mdev tiers (mdev 8.1 → 4.1–4.8 s at 4 × 10¹⁰). dm at 10¹¹ S = 168 is +3.4 s (one run; the dm phase does not depend on S, so this is
likely node noise, but it is not proven). **A five-run series is needed before adopting**; the rule itself (pick S near 256 so that
2 × span limbs sits just under 2ᵏ or 3·2ᵏ, using the run's top k) is a few lines. After item 1 the seed share of the gain falls.

#### 3. B5 — kernels

**The radix-3 pass** (`tests/v3_r3_bench.c`, APU0, prime 0, 3·2²⁹ points per call, median of 5, **measured**):

| 3·2ᵏ, k | 10 | 12 | 17 | 22 | 26 | 28 | 29 |
|---|---|---|---|---|---|---|---|
| fwd3 ms | 33.8 | 33.9 | 48.5 | 68.1 | 69.4 | 81.8 | 84.8 |
| of which the r3 pass | **49.8 %** | 39.3 % | 32.7 % | 23.8 % | 23.6 % | 19.9 % | **19.8 %** |
| inv3: the r3 pass | 45.3 % | 43.3 % | 31.1 % | 23.0 % | 22.5 % | 19.2 % | 18.6 % |

The pass moves 25.8 GB in ≈ 16.3 ms (1.58 TB/s): memory-bound, so **the reduced-correction modmul in it gains ≈ 0**; fusing it into
the first 2ᵏ pass (DIF: the radix-3 butterfly on the three thirds' tiles as the first stage of the b16/b1 pass; the inverse mirrored)
removes most of it. Also: at k = 10 a 3·2ᵏ transform costs **1.23 × a 2ᵏ⁺² transform of the same product** (3 × 33.8 against 4 × 20.6
ms; inverse 1.20 ×), ≈ 1.06 × at k = 12, and wins from k ≈ 17 up (0.96 at 17–22, 0.79 at 26, 0.87 at 29): `pick_len` should take
2ᵏ⁺² for k ≤ 12 (bit-identical: the product is exact either way).

Benefit (modelled on these shares): the batch tier's ntt is 25.3 s at 10¹¹ (measured sum of the level lines), ≈ 22.7 s from level 4 on in
3·2ᵏ, ≈ 30 % of it the r3 pass → ≈ −4.5 s with fusion at 70 % recovery; the dm / mdev products' 3·2²⁹ B pieces (assumed half of
≈ 60 s of transforms) ≈ −3 s more: **−6…−8 s at 10¹¹**. At 576 only the leaf gains (the mn tier's lengths are 2ᵏ only): **−3…−4 s**.

**The 2³¹ plan's top pass** (K13b): 26.1 ms at 1316 GB/s → ≈ 21.5 ms: −4 ms per 2³¹ transform (≈ 4 %). Only whole 2³¹-point
transforms use that plan (one-plane B/C products; the grids use 3·2²⁹ B pieces): ≤ 1 s at 10¹¹, ≤ 0.3 s at 576 (modelled).
**The s_lo 17/24 penalty**: `NTT_PLAN=1` (default) removed it at 2³¹; it remains at 2²⁴ only (≤ 0.2 s). **General-map twiddle
packs**: X13b measured `k_twpack` at ≈ 23 % of a four-step's local transform and `DIST_TWREC` 1.43–1.65 × faster on the equal path;
the general map (576 = 2⁶·9: the 192 and 576 levels, the reciprocal and division, ≈ 94 s of the modelled wall) has its own
`k_twpack_g / k_unpacktw_g` with no recurrence form: **−2…−3 s at 576** (assumed: packs ≈ 10 % of the local piece time, a third
of it saved), 0 at one node. Measurable only multi-node.

#### 4. B2 — the reciprocal

**What it is.** `newton_db.c recip_db2` (and `recip_mn` over shares): per doubling Q_t·r (take ≈ 2j by j + 1 → t1 ≈ 3j limbs,
read as u = t1 >> (take − j)) and r·d (j + 1 by j + 2, read as corr = t1 >> j). `NEWTON_RECIP_CUT` (default) already skips the
pieces wholly below the band.

**The middle product.** |u − B²ʲ| < Bʲ⁺¹, so u is fixed by its low j + 2 limbs: Q_t·r is needed only on [v, v + j + 2) with
v = take − j. In grid form that is mul_grid's `lowcut` = v − 2 and `highcut` (`w`) = v + j + 4 (both exist); for a product that fits
one plane it is a cyclic convolution of length ≥ 2j + 3 instead of 3j + 3 (the wrap lands below v − 2; R114's guard argument covers
the carry from the wrapped part with two guard limbs since n B² · B^(v−2) < B^v). r·d is a high product with no known part: unchanged.

**Modelled** (`tests/v3_b2.py`, the pipeline law): at 10¹¹ the top doubling's 9 × 3 grid forms 20 pieces instead of 24 (21.3 → 17.7 s),
the one-plane doublings save 0.6 s by the wrap: **52.8 → 48.6 s of the modelled reciprocal (−4.2 s, −8 %)**; the model's reciprocal is
high against the measured 43.0 s, so ≈ **−3.4 s** measured-scale (−1.6 % of the wall). 7.64 × 10¹⁰: −1.6 s; 1.3 × 10¹¹: −5.6 s. **576**:
the top doubling's 5 × 2 grid forms 8 instead of 9 pieces: **−1.0 s** of 30.2 (−0.4 % of the wall). TASKS 6.3's −25…−35 % does not hold
here: the cut already took the grid's easy part, r·d cannot shrink, and plane-sized pieces are coarse against the band.

**TASKS 2.3 (piece loading)** is closed: the reciprocal at 10¹¹ is 43.0 / 42.7 s measured (45.6–45.9 before A5 and E7).

#### 5. B4 — transform lengths 5·2ᵏ and 7·2ᵏ

**Prerequisite not in TASKS 6.2**: the primes are c·2⁴⁴ + 1 with c = 240, 216, 207 (`modarith.h`): only p₀ has 5 | p − 1, none has 7.
Every prime of a product must support the length, so 5·2ᵏ / 7·2ᵏ need a **new three-prime set** with 105 | c and enough CRT margin
(today 6.8 at 2³³ points; a smaller c loses it), new constants for the reduced-correction modmul and the CRT, radix-5/7 passes on the
model of `ntt3.c`, and re-verification of everything: **≥ 2 weeks, high risk**.

**Benefit (modelled, `tests/v3_b4.py`)**: the mn tier (`mn_shape`: powers of two only) with +3,5,7·2ᵏ: 244.9 → 244.1 s at the target
(the reciprocal's one-plane doublings); the tree levels are unchanged because their pieces sit at the 2⁴⁰ cap — **the target's grid
steps are counts of cap-sized pieces and more lengths do not soften them** (5·2³⁸ and 3·2³⁹ exceed the per-APU plane memory, 7·2³⁷ is
below the cap). One node: the batch tier's padding (1.34–1.79 ×) is the real waste, ≈ −3 s at 10¹¹ with 5·2ᵏ (assumed 1.10 × per
point), and item 2 removes the same padding with no code. Not recommended.

#### 6. B6 / B7

- **B6**: with three primes the B form and the mdev tier leave APU 3 without transforms. B13b measured the alternative (B4: APU 3
  takes the upper residues) 3–7 % slower, its push at best equal to its saving; P3 measured the striped batch tier's idle ≤ 0.3 s at
  4 × 10¹⁰. B at size > 1 would broadcast whole operands across nodes (25–50 GB/s per APU against 51 over xGMI): the mn tier stays C.
  ≈ 0 at both scales.
- **B7**: CPX needs an administrator and makes every APU six devices; the batch tier might gain cache locality (unknown, 0–10 % of
  ≈ 25 s of batch ntt) while the dist tier, which wants one partition per APU, would lose. SDMA: the division's X fetch is 0.87 s and
  streamed under the low product (measured `dc` line), checkpoints are off by default. ≈ 0.

#### 7. E11 — band-sized reciprocal products and the chunked division window

**What it is.** `binsplit.c dm_layout` reserves (under `DM_TIGHT`) the reciprocal's hole for t1 at Q_t r's full 3j size and the
division's window A − X Q at nq + k limbs. V6: t1 written only for its band (item 4's middle product makes this natural), the window
formed a plane at a time.

**Modelled** (`tests/v3_e11.py`: `mem_model.dm_layout` with the changed terms, three primes, cap 2³¹, `DM_TIGHT`):

| | one-node ceiling 480 / 502 GB | node at 10¹¹ | at 576 (4.25 × 10¹³, chunked, early free) |
|---|---|---|---|
| as built | 1.41 / 1.50 × 10¹¹ | 378 GB | 424 GB; ceiling 9.2 × 10¹⁰ per node |
| band-sized t1 only | 1.46 / 1.56 × 10¹¹ | 376 GB | — |
| chunked window only | 1.41 / 1.50 × 10¹¹ (the top bs levels bind) | 378 GB | — |
| **both** | **1.52 / 1.64 × 10¹¹** | 364 GB | **407 GB; 9.8 × 10¹⁰ per node** |

**Not 2.1–2.3 × 10¹¹**: that figure (APUMULT_STUDY) assumed P and Q spilled first (E5/E6, rejected in L114); without them 2 × 10¹¹ needs
590 GB. The time effect is item 4's (the band) and 0 for the chunked window (a plane-sized round is what the grid does anyway).

#### 8. Where the target's wall goes (the model, for context)

`tests/v3_brk.py` (modelled, 576 nodes, 4.25 × 10¹³, the fabric assumed): **244.9 s = init 23.6 + batch 28.9 + top 24.0 + the
distributed levels 86.6 (29.3 exposed) + reciprocal 30.2 (12.2 exposed) + division 29.1 (12.1 exposed) + the part file 22.3**.
Per-node compute ≈ 169 s (69 %). Items 1–3 act on init + batch + top (76.5 s); items 4–5 on the mn tier. Two things for C1:
(a) the model's 576 reciprocal has no `NEWTON_RECIP_CUT` (the C plan forms 67 recip pieces, the model 69: ≈ −1.9 s); (b) the seed term
of §1 (≈ +10 s). Net the model is ≈ 8 s optimistic (derived).

#### 9. Outside my items (for the integrator)

- **The budget check stops 10¹¹ on one node on the defaults**: `~/ref1e11/run_default.log` (4855842) exits rc 8 — "peak 513.2 GB
  (device 488.1 + host 25.1)" against 480. The device line counts regions 259.9 + block pool 124.6 (borrowed) + planes 103.1; the
  measured runs here (with `ECALC_BUDGET_CHECK=0`) completed with host HWM 27 GB and MemAvailable ≥ 152 GB at every bs level. The
  borrowed pool seems counted on top of the regions it borrows from. Not investigated further.
- The target's part file (22.3 s exposed, at the **assumed** 2 GB/s) is larger than any B item: measure `/ssd0` early (TARGET_TASKS).

#### 10. Tests and commands (all measured on aac6 unless marked)

| what | command | result |
|---|---|---|
| model breakdown | `python3 tests/v3_brk.py` (login) | §8 |
| B2 / B4 / E11 models | `python3 tests/v3_b2.py`, `tests/v3_b4.py`, `tests/v3_e11.py` (login) | §4, §5, §7 |
| plan at the target | `MN_PLAN_ONLY=42500000000000:576 ./ecalc 42500000000000` (aac6 login, `~/ntt-V314`) | recip 67 pieces with the cut, top doubling 5 × 2 (9 formed, 1 skipped) |
| span bench, login (Zen 2) | `OMP_NUM_THREADS=8/1 ./v3sb <k> 20000/4000` | slow path × 2.0 per limb; fused form 1.5 × / 3.1 × |
| node batch (job 21436, s24-30, cancelled itself) | `tests/v3_node_batch.sh` | `results/V314/`: bench.log (spans, r3 pass), e11_def, e11_def2, e11_s168, e4_def, e4_s236, e4_s176 — all **VERIFY OK**; the S runs are digit-exact by construction but were not compared byte-for-byte (no output written) |

The 10¹¹ runs used `ECALC_BUDGET_CHECK=0` (§9). Nothing of mine is running or pending.

#### RESUME (18:55 EDT)

- **Done**: every item analysed; the node batch ran (job 21436, cancelled by its script); logs in `results/V314/`; the report above.
- aac6: clone `~/ntt-V314` at this branch (built), scratch `~/V314/` (bench binary, logs). No job pending.
- **Open (for whoever builds)**: item 2 needs a five-run series before adoption; item 3's per-point gain should be measured on a
  prototype kernel before the full port; items 1–3 are then worth a C1 refit.

## T215 — Phase 15, row 1c: `BS_SEED_TERMS` chosen per run so the batch tier fills its transform lengths

(former file: T215.md)

Agent T2, branch `p15-T2` from int15 0b80ca3 (B0), aac6 clone `~/ntt-T215`. Times Eastern. Numbers are labelled
**measured** (a run's log), **modelled** (`tests/t2_seed_model.py`) or **assumed**.

#### 1. What decides the fill (from the code and V3's logs)

`binsplit.c`'s level loop combines spans of S terms pairwise. Level l (1-based, as the log prints it) has inputs of at most
`max_nl(l)` limbs and forms the products P₁Q₂ (+P₂) and Q₁Q₂ with `nc = na + nb ≤ 2·max_nl(l)` points. A batch level
(`2·max_nl + 1 ≤ 2^30`) uses **one length for all its products**: `rns_mul.c pick_len(maxnc)` = 2ᵏ with k = ⌈log₂ maxnc⌉ ≥ 10,
or 3·2ᵏ⁻² when k − 2 ≥ 10 and maxnc ≤ 3·2ᵏ⁻². A node's Q has ⌊log₁₀Q / 18⌋ + 1 limbs and P ≤ Q, so with m₁ = the largest
seed's limbs (the last span's Q), **max_nl(l) ≤ 2ˡ⁻¹·m₁ and maxnc(l) ≤ 2ˡ·m₁**, and m₁ ≈ S·log₁₀(k_max)/18.

- **m₁ ≤ 2ʲ** (the "2ᵏ family"): every product from 2¹⁰ points up fills a 2ᵏ length (fill ≥ 0.99, modelled), and the batch tier's
  top level ends exactly at the mdev threshold (inputs ≤ 2²⁹, products ≤ 2³⁰), so **one more level runs in the batch tier** (3–4×
  cheaper per output limb than mdev, measured: V3's S = 168 level 23 in batch 2.85 s against ≈ 10 s for the same level in mdev).
- **m₁ ≤ 3·2ʲ** (the "3·2ᵏ family"): fills 3·2ᵏ from 3·2¹⁰ up; the top batch products are 3·2²⁸.
- Otherwise the padding is 1.34× (m₁ just above 3·2ʲ... 2ʲ⁺¹) up to 1.5× (just above 2ʲ). **S = 256 at 10¹¹ has m₁ = 143:
  fill 0.74 from level 4 on; at 4 × 10¹⁰ m₁ = 138: 0.71** (modelled; the model's max_nl matches the log's at all 26 levels).

The model reproduces `max_nl` exactly (10¹¹, S = 256: 143, 285, 570 … 594141204, 1178075121, 2338544164, 4533431218 — the log's
numbers). Its cost terms, fitted to V3's six logs (modelled): batch = 3.39e−10 s per per-APU point + 1.15e−7 s per span (the batch
column within ±0.5 s of measured); mdev = 1.23e−18 s per limb² of the products + 0.89 s per mdev level (±3 s: crude).

**Predicted rule** (modelled): **S = the largest S whose last span has ≤ 128 limbs** (`BS_SEED_FILL=128`): S = 239 at 4 × 10¹⁰,
**229 at 10¹¹**, 235 on the 576-node plan's node 0 (terms [1, 6.09 × 10⁹)), ≈ 184 on its last node (k ≈ 3.5 × 10¹²). Because S
depends on log₁₀ k_max, **no single `BS_SEED_TERMS` fits every size** (and not every node of a multi-node run): hence the rule.

| size | S | m₁ | batch / mdev levels | batch padding | modelled bs − seeds (s) | what |
|---|---|---|---|---|---|---|
| 4 × 10¹⁰ | 256 | 138 | 22 / 3 | 1.55 | 22.7 | default (measured bs − seeds: 23.8) |
| 4 × 10¹⁰ | 179 | 96 | 23 / 2 | 1.28 | 20.7 | fill 96 (V3's 176: measured 19.8) |
| 4 × 10¹⁰ | **239** | 128 | 23 / 2 | 1.17 | **18.5** | **fill 128** (V3's 236: measured 17.9) |
| 4 × 10¹⁰ | 242 | 130 | 22 / 3 | 1.64 | 24.0 | control: fill 128 + 3 terms (spills every length) |
| 4 × 10¹⁰ | 358 / 478 | 192 / 256 | 22 / 2 | 1.11 / 1.05 | 17.2 / 15.9 | fill 192 / 256 (+ seed CPU ∝ S) |
| 10¹¹ | 256 | 143 | 22 / 4 | 1.49 | 75.4 | default (measured 76.4, 76.0) |
| 10¹¹ | 172 | 96 | 23 / 3 | 1.29 | 72.5 | fill 96 (V3's 168: measured 71.7) |
| 10¹¹ | **229** | 128 | 23 / 3 | 1.17 | **67.0** | **fill 128** |
| 10¹¹ | 232 | 130 | 22 / 4 | 1.64 | 79.9 | control |
| 10¹¹ | 344 / 459 | 192 / 256 | 22 / 3 | 1.11 / 1.05 | 63.8 / 60.4 | fill 192 / 256 (+ seed CPU ∝ S) |

The seeds' wait is not in the table: on B0 it grows with S (10¹¹: 19.7–20.5 s at 256, 14.8 s at 168, measured: ≈ 0.056 s per term of
S, modelled from those two points); at 4 × 10¹⁰ it sits on the mapping floor (8.2 / 8.3 / 6.3 s at 256 / 236 / 176, measured). So on
B0 the larger fills (192: +5 s of seeds at 10¹¹, 256: +11 s, modelled) should lose to 128; **after S1** (a 1.7–2.2× faster span) the
seed cost per term roughly halves and fill 192 or 256 may win — that is what step 2's sweep decides. *(Correction after job 21461:
the wait is not linear in S — 19.7 s at S = 229, the same as at 256, and 15.0 s at 168, measured — so the "+5 / +11 s" above is an
upper bound; the sweep measures it.)*

#### 2. The switch (off by default)

`BS_SEED_FILL=<limbs>` (README row): `binsplit.c bs_seed_terms_for(bend)` — the largest S with ⌊log₁₀ Q(bend − S, bend) / 18⌋ + 1 ≤
limbs (lgammal; binary limbs use 64·log₁₀2 digits per limb), bend = the process's last term + 1 (`bs_b1`, or N + 1). It overrides
`BS_SEED_TERMS`; 0 / unset = off, byte-identical by construction (the seed span changes only the tree's shape; P, Q are unique).

- `ecalc/binsplit.c`: the function above `seed_limbs` (lines 256–272), one line in `seed_limbs` (276), and in `binsplit_e`
  `S = bs_seed_terms` after `seed_limbs` (1292; S was read before `seed_limbs` refreshed it). **S1 edits `span()` / the seeds nearby:
  these are the only lines touched.**
- `ecalc/binsplit.h`: the prototype. **Outside my files:** `ecalc/mn_plan.c plan_leaf` (one line) reads the span through
  `bs_seed_terms_for(b1)` so `MN_PLAN_ONLY` plans the same tree (checked: `BS_SEED_FILL=128 MN_PLAN_ONLY=1e11:1` → "45562845 spans of
  229, 23 levels below the device tier, 3 device (mdev) levels"; `MN_PLAN_ONLY=4.25e13:576` node 0: 256 → 235 spans, **mdev levels
  3 → 2, leaf dist_db pieces 16 → 12** (modelled)). `mn_model.py` keeps `BS_SEED_TERMS = 256` (model only; not changed).
- `tests/t2_seed_model.py`: the model (`--levels <digits> <S>`, `--fit`, the candidate table).

#### 3. The sweep script

`tests/t2_sweep.sh <commit> <outdir> <plan> [sbatch options]` (run with `setsid nohup … < /dev/null &` on aac6): checks the clone is at
`<commit>` (else WRONG COMMIT), builds, submits one `-J T2` 1-node job at a time (≤ 45 min; runs that do not fit go to the next
job; at most `T2_MAXJOBS` = 4), runs each plan line `<tag> <digits> <nw|w> [VAR=value …]` with `ECALC_VERBOSE=2`: `nw` = the wall
without the digit write, `w` = digits written to the node's `/tmp` and hashed there with O_DIRECT against the reference sha1
(4 × 10¹⁰: `43c84e49…` from `results/e_4e10.out.sha1`; 10¹¹: `578f5efb…` from `~/V214/e_1e11.sha1`), then deleted. One summary line
per run (S, total, init, bs with seeds / batch / mdev, recip, dm, T1, VERIFY, digits, elapsed, node); the job cancelled at the end
and on any exit. Plans: `tests/t2_plan_dm.txt` (step 1c), `tests/t2_plan_sweep.txt` (step 2: 4 × 10¹⁰ and 10¹¹, fills 96 / 128 /
192 / 256, the default and the spilling control, both walls).

#### 4. The +3.4 s in `dm` (V3, 10¹¹, S = 168)

From V3's logs (measured, one run against two): `dm` 105.02 vs 101.61 / 101.20 s. **The reciprocal is the same** (42.95 vs 42.99 /
42.68), so all of it is in the division: its two products +1.02 s (26.49 + 28.81 vs 26.12 + 28.15), the rest (A·μ shift, X out, the
window and corrections, R's residues, the writer's hook) +2.43 s (6.77 vs 4.34). The run's block pool was laid out differently: S
changes the region sizes (arena 255.3 vs 259.9 GB, dm extra 18.0 vs 17.0 GB; block pool 257.7 vs 262.0 GB, APU 0 64.42 vs 68.72 GB).
Two readings: node noise (V3's view), or the pool layout (a VMM remap or a worse placement of t1 / xq inside the division).
The node batch below separates them (`DB_POOL_VERBOSE=1` prints every VMM remap and tail move; `RNS_VERBOSE=1` splits divmod).

**Node batch** (job 21461, `aac6-spx-b`, 2026-09-26 23:09–23:34 EDT, 594ba4c = B0 + the switch off, 10¹¹ without the digit
file, `tests/t2_sweep.sh … tests/t2_plan_dm.txt`, logs in `archive:results-raw/T215/dm/`; all **measured**, VERIFY OK):

| run | S | total | init | bs (seeds / batch / mdev) | recip | dm | VMM remaps in recip + dm: count, s (sum over APUs) |
|---|---|---|---|---|---|---|---|
| d1 | 256 | 210.75 | 17.6 | 93.31 (19.5 / 35.9 / 37.8) | 41.22 | 98.84 | 3, 1.48 (APUs 1–3, the low product's xq) |
| s1 | 168 | 200.64 | 15.5 | 83.13 (14.9 / 34.7 / 33.4) | 41.16 | 100.91 | 8, 3.74 (all four APUs, t and xq) |
| d2 | 256 | 211.53 | 17.0 | 94.15 (20.5 / 35.6 / 37.9) | 41.28 | 99.42 | 3, 1.48 |
| s2 | 168 | 204.92 | 18.4 | 84.34 (15.1 / 35.7 / 33.3) | 41.34 | 101.14 | 8, 3.7 |
| dv (`RNS_VERBOSE`) | 256 | 210.73 | 17.1 | 93.45 (20.5 / 35.4 / 37.5) | 41.59 | 99.19 | — |
| sv (`RNS_VERBOSE`) | 168 | 202.39 | 15.7 | 84.08 (15.3 / 35.4 / 33.2) | 41.61 | 101.57 | — |
| **f128** (`BS_SEED_FILL=128`) | **229** | **198.79** | 18.4 | **78.57 (19.7 / 29.5 / 29.2)** | 42.10 | 100.80 | 12, 3.99 (APUs 1–3: 2 in recip, 2 in dm) |

**Finding: the `dm` penalty is real, not noise, and it is the block pool's VMM remaps.** dm at S = 168: 101.21 s mean of 3 against
99.15 s at 256 (**+2.06 s**, measured; V3 saw +3.4 in one run). The reciprocal and the division's two products are the same (±0.3 s);
`divmod(dev)`'s split (dv / sv) puts the difference in **"A mu + shift" 25.50 → 27.47 s** (its product 25.25 / 25.37 s) and "X out +
low product" 30.11 → 30.50 s. `DB_POOL_VERBOSE=1` shows why: the division's two 22.22 GB blocks per APU (t, then xq) need a
contiguous extent. At 256, t goes into the arena's reserved tail ("tail 22.57 GB: 1 large block from its back") and only xq needs a
remap, on APUs 1–3 (APU 0's arena is 68.72 GB = 32 chunks, the others 64.42 GB = 30); at 168 the tail is taken by then ("0 large
blocks"), so **t and xq both remap, on all four APUs: 8 remaps of 11 × 2 GiB chunks, 0.45–0.49 s each**. `vmm_make_room` runs with
the global pool lock held, so the four APUs' remaps are **serialized**: 4 × 0.46 = 1.84 s before t's product against the
+1.97 s measured in "A mu + shift" (sum of remaps 3.74 vs 1.48 s: +2.26 s, against +2.06 s in dm). Which blocks land where depends
on the tree's shape (S changes the regions and the order of the top products), so any S can win or lose ≈ 2 s here; S = 229 also
remaps (3.99 s of remaps, dm 100.80 s, recip +0.8 s).

**Consequence and suggestion (outside my files: `ecalc/dbig.c`, not changed):** remap each APU under a per-device lock (the four in
parallel: ≈ 0.5 s per event instead of 1.5–1.9 s), or map the arenas of APUs 1–3 one chunk larger as APU 0's already is: modelled
−1.0 s at the default (3 serialized remaps → 1), −2.8 s at S = 168, −2.5 s at S = 229 (12 → 4 events of ≈ 0.3–0.5 s). This makes
the seed-span choice independent of the pool's luck.

**The rule's first run (f128, one run, measured):** total **198.79 s against 211.00 s (mean of 3) at the default: −12.2 s (−5.8 %)**;
bs 78.57 against 93.64 (batch −6.1 s, mdev −8.5 s, seeds ±0), dm +1.65 s (the remaps). S = 168: 202.65 s mean of 3 (−8.3 s).
The model predicted −8.4 s for bs − seeds at 229 (modelled) against −14.7 s measured: it under-counts the mdev tier's gain (29.2 s
against 37.7 s: level 23 in the batch tier, and three mdev levels of 1.06 / 2.08 / 4.03 × 1.52 G-limb inputs instead of four).

#### 5. Step 2: the sweep on int15-seed (B0 7367e25 + p15-S1 + p15-T2, 4e0edd4)

My merged lines in `binsplit.c` read as intended (function 266–282, `seed_limbs` 281, `binsplit_e` 1297; `mn_plan.c` 87).
Command: `T2_MAXJOBS=6 bash ~/T215/t2_sweep.sh 4e0edd4 ~/T215/sweep2 ~/T215/t2_plan_sweep2.txt` (plan = `tests/t2_plan_sweep2.txt`),
then `tests/t2_gates.sh`, chained unattended (`~/T215/chain2.sh`). Jobs 21519 (s24-16), 21529 (s24-26), 21541 (s24-16), 21545 gates
(s24-16), 2026-09-27 11:00–13:34 EDT, ≈ 2.6 node-hours. Every run has `DB_POOL_VERBOSE=1`; each configuration ran with
`BI_MUL1_FAST=0` and then `=1`, back to back in one job. Logs and `summary.txt` in `archive:results-raw/T215/sweep2/`. **All 40 runs VERIFY OK;
all 16 written runs byte-identical to the reference** (sha1 hashed on the node: 4 × 10¹⁰ `43c84e49…`, 10¹¹ `578f5efb…`).
**One run per cell (measured)**; the run-to-run noise at 10¹¹ is ≈ ±3 s (§5.3).

##### 5.1 4 × 10¹⁰ (all in job 21519, s24-16; seconds; FAST = `BI_MUL1_FAST`)

| config | S | wall without the write, FAST 0 / 1 | wall with the write, FAST 0 / 1 | bs 0 / 1 (seeds / batch / mdev, FAST 1) | dm 0 / 1 | remaps (count, s) |
|---|---|---|---|---|---|---|
| default | 256 | 67.40 / 64.74 | 66.34 / 65.85 | 32.5 / 30.7 (7.3 / 15.5 / 7.8) | 22.67 / 22.65 | 4, 0.86 |
| fill 96 | 179 | 59.26 / 59.40 | — | 25.3 / 25.4 (6.5 / 14.9 / 4.0) | 21.96 / 22.08 | 0 |
| **fill 128** | 239 | 60.53 / **58.19** | 61.69 / **59.12** | 25.5 / **23.8** (6.6 / 12.5 / 4.6) | 22.51 / 22.39 | 2, 0.44 |
| fill 192 | 358 | 60.68 / 58.87 | 61.49 / 61.68 | 26.6 / 25.6 (9.2 / 12.5 / 3.9) | 22.14 / 22.03 | 0 |
| fill 256 | 478 | 62.47 / 61.78 | 62.78 / 61.33 | 28.4 / 27.3 (11.9 / 10.8 / 4.5) | 22.08 / 22.25 | 0 |
| control (S = 242: fill 128 + 3 terms) | 242 | 65.14 / 64.70 | — | 31.0 / 30.3 (7.7 / 16.2 / 6.3) | 22.64 / 22.48 | 2, 0.42 |

##### 5.2 10¹¹ (nw: default and fill 128 in job 21519 s24-16, the rest in 21529 s24-26; all w in 21541 s24-16)

| config | S | wall without the write, FAST 0 / 1 | wall with the write, FAST 0 / 1 | bs 0 / 1 (seeds / batch / mdev, FAST 1, nw) | dm 0 / 1 (nw) | remaps (count, s) |
|---|---|---|---|---|---|---|
| default | 256 | 213.13 / 208.91 | 236.64 / 232.33 | 95.6 / 92.3 (17.3 / 37.2 / 37.7) | 100.42 / 99.27 | 3, 1.37–1.46 |
| fill 96 | 172 | 205.71 / **200.75** | — | 84.1 / 81.5 (13.0 / 35.0 / 33.3) | 101.66 / 101.23 | 8, 3.69–3.77 |
| **fill 128** | 229 | 213.41ᵃ / 203.31 | 224.30 / **216.62** | 90.1ᵃ / **79.8** (15.9 / 33.5 / 30.1) | 103.16 / 103.67 | 12, 3.84–3.96 |
| fill 192 | 344 | 207.37 / 205.17 | 231.99 / 226.15 | 88.7 / 85.7 (22.6 / 29.5 / 33.5) | 101.11 / 101.50 | 8, 3.68–3.77 |
| fill 256 | 459 | 208.87 / 203.97 | 228.84 / 228.57 | 88.7 / 83.6 (28.6 / 25.7 / 29.3) | 101.55 / 101.34 | 9, 3.38–3.49 |
| control (S = 232) | 232 | 211.20 / 209.19 | — | 92.4 / 89.2 (16.7 / 38.9 / 33.5) | 100.96 / 101.46 | 12, 3.84–3.90 |

ᵃ b128n0's level 1 took 9.46 s (5.08–6.47 s in the other three fill-128 runs, 5.23 s in job 21461) and its seed wait 23.7 s: the
arena's background VMM mapping ran 15–22 s on three APUs during that run (4–14 s in the others). Treat it as an outlier.

##### 5.3 What the sweep shows (measured unless marked)

- **The fill mechanism is confirmed.** The controls, 3 terms above the fill-128 S, lose almost the whole gain. At 4 × 10¹⁰:
  64.7–65.1 s against 58.2–60.5 s. At 10¹¹: bs 89.2 against 79.8 s (FAST 1).
- **Fill 128 is the best or within noise of the best at both sizes.**
  - 4 × 10¹⁰, FAST 1: −6.6 s without the write, −6.7 s with it (−10 %).
  - 10¹¹, FAST 1: bs −12.5 s (nw) / −15.8 s (w). The walls are −5.6 s without the write and −15.7 s with it; the with-write
    difference includes T1's wait for the writer, 20.8 against 26.0 s, which varies by itself.
  - Fill 96 is as good without the write at both sizes (4 × 10¹⁰ 59.4 s; 10¹¹ 200.8 s, on a different node from the default pair).
- **Larger fills lose even with S1.** The seed span's cost grows with S: at 10¹¹ the seed wait is 22.6 s at fill 192 and 28.6 s at
  fill 256, against 15.9 s at fill 128 (FAST 1). Their batch and mdev gains (batch 25.7–29.5 s) do not cover it. S1's switch
  shortens the wait by 3–6 s at 10¹¹ and by 0.5–1.5 s at 4 × 10¹⁰, where the mapping floor hides most of it. That is too little to
  move the optimum past 128. **The rule stays `BS_SEED_FILL=128` with or without `BI_MUL1_FAST`.**
- **FAST 1 against FAST 0** (the same configuration, back to back): 10¹¹ bs −3.3 s at the default and −10.3 s at fill 128 (the
  latter inflated by the outlier; −7.5 s in the w pair). Walls −4.2 / −4.3 s at the default (nw / w).
- **The dm penalty.** Every fill has 8–12 remaps (3.4–4.0 s) against 3 (1.4 s) at the default, so dm is +1–4 s. At fill 128 it is
  +3.5–4.4 s (103.2–103.8 against 99.3–100.4 s): about a quarter of the bs gain.
  - At 4 × 10¹⁰ the remaps are ≤ 4 (≤ 0.9 s) and dm is flat.
  - RL's per-device remap lock should return ≈ 2–3 s of this at 10¹¹ (modelled: 12 serialized events → about 3 of ≈ 0.3–0.5 s).
- **Noise.** Level 1's time (4.4–9.5 s) follows the arena's background VMM mapping, which overlaps levels 1–2 and takes 3–22 s per
  APU. Single runs at 10¹¹ differ by ≈ ±3 s for this reason.

##### 5.4 Gates (job 21545, s24-16; `tests/t2_gates.sh 4e0edd4 ~/T215/gates`; `archive:results-raw/T215/gates/`)

| gate | command | result |
|---|---|---|
| unit + e9, defaults | `./mnaccept.sh 21545 --only unit,e9` | **10/10 PASS**: t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid (2 procs), e9 LIMB_BASE=10 and 2 identical |
| e9 + mn with the switch on | `BS_SEED_FILL=128 ./mnaccept.sh 21545 --only e9,mn` | **7/7 PASS**: e9 both bases identical; mn e8 at sizes 2, 3, 4 and e9 at sizes 2, 4 identical, all nodes VERIFY OK (each node takes its own S from its last term) |
| 4 × 10¹⁰ and 10¹¹ identical with the switch on | the sweep's `w` runs | fills 128 / 192 / 256 × FAST 0 / 1: identical at both sizes (12 runs) |

Gap: mnaccept runs without `ECALC_VERBOSE`, so its logs do not print the span. They do not prove the environment reached every
process. `mnaccept.sh` passes the environment through `srun` and `mnrun.sh env`, which the other switches' gates rely on too.

#### 6. Tests (step 1)

| test | command | result |
|---|---|---|
| build | `module load rocm; make -s -j16` on the aac6 login node (594ba4c; 4e0edd4) | ok (no new warnings) |
| rule vs model | `BS_SEED_FILL=96/128/192/256` through the C function vs `t2_seed_model.py seed_terms_for` | identical: 179/239/358/478 (4e10), 172/229/344/459 (1e11) |
| plan | `BS_SEED_FILL=128 MN_PLAN_ONLY=1e11:1 ./ecalc 1e11`; `MN_PLAN_ONLY=4.25e13:576` with 0 / 128 / 96 | S 229, 23 batch + 3 mdev levels; node 0: 256 / 235 / 176, mdev levels 3 / 2 / 3 |
| step 1c + the rule's first run | `tests/t2_sweep.sh 594ba4c ~/T215/dm tests/t2_plan_dm.txt` (job 21461, s24-26) | 7 runs at 10¹¹, all VERIFY OK; §4 |

#### 7. Recommendation and the 576-node estimate

- **Adopt `BS_SEED_FILL=128`** (the user's decision), after a five-run paired series at 10¹¹ on one node: default vs fill 128, both
  with `BI_MUL1_FAST=1`, and ideally after RL's remap lock. The gain is measured in single runs so far: bs −12…−16 s, walls
  −6…−16 s at 10¹¹; −6.6 s (−10 %) at 4 × 10¹⁰.
- **576 nodes** (modelled, not re-run): the baseline stays **4.25 × 10¹³ digits in ≈ 4.1 min** (PLAN §36.10, part file at an assumed
  2 GB/s per node; 5.3–5.8 min at the measured Lustre rate).
  - With fill 128 the leaf loses one mdev level on node 0 (3 → 2) and 4 of 16 leaf pieces (`MN_PLAN_ONLY`).
  - PLAN's −3…−7 s at the target (assumed) remains the estimate: the leaf holds 1/576 of the terms at larger k, and the 10¹¹ gain
    does not scale to it directly.

#### Open issues

- The dm remap penalty (§4, §5.3) belongs to `ecalc/dbig.c` (RL is changing it). Rerun the fill-128 pair on RL's branch to see the
  gain it returns.
- `ecalc/mn_plan.c` (not mine, one line) and `ecalc/binsplit.h` (prototype) were touched. `mn_model.py` still assumes S = 256; to
  model the target with the rule, it needs `seed_terms_for` from `tests/t2_seed_model.py`.
- The seed model's mdev term is crude (±3 s). It under-predicted fill 128's measured gain at 10¹¹.

#### RESUME

- Step 1 and step 2 are **done**. Branch `p15-T2` holds the switch, scripts, plans, logs and this report. `int15-seed` (4e0edd4)
  is unchanged; the sweep and gates ran on it.
- If the integrator wants the series: add a plan of 5 × (`BI_MUL1_FAST=1`) + 5 × (`BI_MUL1_FAST=1 BS_SEED_FILL=128`), interleaved,
  at 10¹¹ nw (≈ 37 min, one job). Run it with
  `setsid nohup bash ~/T215/t2_sweep.sh <commit> ~/T215/series <plan> > ~/T215/series.out 2>&1 < /dev/null &`.

## CAND - the three off-by-default speed switches combined (interaction test)

(former file: CAND.md)

Branch `cand` (off `main` a448d73). Labels: measured / modelled / assumed. Times Eastern.

#### Design
- Code only, squashed from the three branches (one commit each; their results files and tools stay on their branches):
  - `o2`: `DBIG_SMALL_BOUNCE` (+ print-only `DBIG_BOUNCE_TS`, `tests/t_bounce.c`) - results/DIVISION.md §O2 on o2.
  - `k1b`: `DIST_GATHER_RAW` (+ `DIST_GATHER_RAW_MARGIN_GB`, `tests/t_gather_raw_probe.c`) - results/EXCHANGE.md §K1b on k1b.
  - `k8b` at 99eee11 (includes the hang fix b1ff573): `BS_SEED_PULL` - results/MEMORY.md §K8b on k8b.
- No textual conflicts. Switch registry regenerated with `tools/switch_registry.py`; the three new switches given
  `experiment` rows in `ecalc/switches_manual.tsv`; `--check`: 269 rows, 0 problems (measured). README rows from each branch kept.
- All switches off by default; with them unset the cand binary takes the main code paths.
- Local build not possible (no hipcc on the workstation); built on aac7 (SHMEM) and aac6 by the gates below.

#### Measurement (armed 2026-10-12 ~07:10 ET)
Arms: A = main binary, switches unset; B = cand binary with `DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1`.
- (a) aac7, 2 nodes, queue item `tools/cand_items/7e_cand_two.sh` (as `~/aac7q/queue/7e_cand_two.sh`): `tools/gate.sh --build
  --nodes 2` all on, then paired AB/BA at 6.441e10 digits per node (1.2882e11 total), aac7 LINE, no digit writes; A = the main
  build in the h4main worktree (copied to `~/cand/ecalc_main`). Stop: >= 4 pairs and p < 0.05 on the total, or 6 pairs, or no
  run after 65 min. NIC launch group and rank-0 memory per run. Output `~/cand/` (summary.txt, runs.tsv, stats.txt, groups.tsv,
  log/), marker `~/cand/DONE`.
- (b) aac6, 1 PPAC_MI300A_SPX node, `tools/cand_items/cand6_driver.sh`: own clones `~/ntt-cand` (cand) and `~/ntt-cand-main`
  (main a448d73) from a full bundle; `gate.sh --build --nodes 1 --line none` all on; paired AB/BA at 6.441e10, stop >= 4 pairs
  and p < 0.05 on the total, or 8 pairs, or no run after 40 min of the 45-min hold. Output `~/cand6/` (summary.txt, pairs.tsv,
  runs.tsv, log/), marker `~/cand6/DONE`.
- Stats: `tools/cand_items/cand_stats.py` (copy of o2's `o2_stats.py`, stop rule on the total; paired B-A, Student t, 95 % CI).

#### Results (collected 2026-10-12; all measured unless marked)
Arms: A = main binary (85d9d73f), switches unset; B = cand binary (7e7cd7da) with `DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1`. 6.441e10 digits per node, aac7 line, no digit writes. Every run VERIFY OK (aac7 24/24, aac6 8/8). Gates: aac7 2 nodes all on PASS (1e9 digits identical, -DCOMM_SHMEM) twice (rounds 1 and 2); aac6 1 node PASS.

##### aac7, 2 nodes (item 7e round 1, rerun as round 2), paired AB/BA, B-A
| round | pairs | mean total A -> B | paired B-A total (95 % CI, p) | paired B-A chain (95 % CI, p) |
|---|---|---|---|---|
| 1 (`~/cand`) | 6 | pair 1 was cold (A 170.0, B 145.5) | -4.95 s [-15.45, +5.55], p 0.280 | -4.51 s [-8.39, -0.64], p 0.030 |
| 2 (`~/cand2`) | 6 | - | -6.22 s [-12.20, -0.24], p 0.044 | -4.62 s [-7.61, -1.64], p 0.011 |
| pooled (computed from the 12 per-run totals) | 12 | A 140.10 s, B 134.51 s | **-5.58 s [-10.54, -0.63], p 0.031** | **-4.57 s [-6.57, -2.57], p 0.0004** (A chain mean 4.57 s vs B 0.06 s in all 12) |
| pooled without round-1 pair 1 (cold, A 170.0 s) | 11 | A 137.38 s, B 133.52 s | -3.86 s [-7.39, -0.33], p 0.035 | - |

- B was faster in 11 of 12 pairs (pair differences, s: -24.5, -0.2, -5.5, -1.1, +3.3, -1.7, -2.1, -1.2, -9.6, -9.6, -0.6, -14.3).
- "Chain" is the db_to_bi conversion chain: arm B 0.05-0.06 s in every run; arm A 2.0-2.4 s normally, but 6.1-10.8 s in 5 of 12 A runs (late PE up to +6.8 s on one node). Those A-arm spikes explain the large spread; the B arm has none. A single-switch effect cannot be separated here (all three on).
- The stop rule of the item (p < 0.05 on the total) was met in round 2 alone (p 0.044) and by the pooled 12 pairs (p 0.031); the p values are two-sided Student t on few pairs, round-1 total alone is not significant (CI crosses 0).
- NIC launch groups (A/B label of each run; fgfc_M 0.0 in all): both groups occur in both arms and rounds (round 1: arm A 3 x A / 3 x B, arm B 5 x A / 1 x B; round 2: arm A 5 x A / 1 x B, arm B 5 x B / 1 x A). The A-arm chain spikes occurred in 3 group-A runs and 2 group-B runs (5 spikes in 12 A runs, of which 8 were group A and 4 group B): no group pattern visible with this n ( a group effect cannot be excluded).
- Memory, rank 0, max over 6 runs per round and arm: device in use 317.8 GB, pool peak live 201.80 GB, hipMalloc 0.00 GB, APU driver-used 82.2 GB, identical in A and B: no memory cost (m).

##### aac6, 1 node (`~/cand6`), 4 pairs, stop rule met after pair 4
- Totals A: 74.10, 73.47, 72.86, 74.54 (mean 73.74 s); B: 66.08, 66.55, 66.10, 67.61 (mean 66.59 s).
- Paired B-A total **-7.16 s [-8.08, -6.23], p 0.0001** (n 4; all four pairs negative). Gate 1 node PASS (1e9). (Memory columns NA: no per-rank memory lines at one node.)

##### aac6 profile of cand all-on (`~/p2a`), 6.441e10, 1 node, one profiled + one unprofiled run (indicative)
- Profiled total 66.45 s, unprofiled total 66.91 s (both VERIFY OK). Unprofiled phases: init 13.74 s (region pools 7.24 s inside rns_init; arenas 6.98 s), bs 36.56 s (seeds 8.2, batch 21.1, mdev 7.1; the bs thread waited 8.24 s for the init thread), dm about 16.6 s (derived: total - init - bs).
- Against main (archive:results/O7b.md on branch o7b, same size, 1 node, main 0298fe2, unprofiled): total 73.32 s -> 66.91 s (-6.4 s, one pair vs one run, indicative), init 15.5 -> 13.7 s, bs 38.2 -> 36.6 s (seed wait 10.0 -> 8.2 s), dm 19.5 -> about 16.6 s. This matches the paired aac6 result (-7.16 s).
- Kernel profile: GPU busy per APU 47.8 / 49.0 / 48.7 / 41.9 s (O7b main: 43.7 / 44.7 / 44.4 / 38.1 s), i.e. more GPU time but a shorter wall. New in the top list: `k_pull` 8.6 s per APU (18.5 % of GPU time, 4 calls; the BS_SEED_PULL kernel moving seeds on the device) and `k_gather_raw<1>` 2.0 s per APU (4.3 %). GPU timeline busier early (first 5 s bin 2.4 s busy, bin 5-10 s fully busy).

##### Verdict
- All three switches on together: faster at both 1 node (-7.16 s, aac6) and 2 nodes (-5.58 s pooled, aac7), digits identical, no memory change. The 2-node gain is about 4.6 s from the chain (db_to_bi) plus a smaller remainder from the other two switches; per-switch shares not separated (not tested alone here; see o2, k1b, k8b results). Interaction vs the sum of the single results is not evaluated in this file.
- Adoption is the user's decision.

##### Pending
- Item 7g (4 nodes, `~/cand4/`) is running on aac7, expected about 10:00 ET; its result goes here when collected.

#### RESUME
- Collect 4-node round: `ssh <aac7> 'cat ~/cand4/DONE; cat ~/cand4/summary.txt'`, add as "aac7, 4 nodes" with the same columns (paired total and chain, groups, memory).
- Cleanup once done: `~/ntt-wt/cand` (aac7), `~/ntt-cand`, `~/ntt-cand-main` (aac6).

## S21: single-node A37 measurements (R2 seed microbench, Q1 init timeline, R4 dc split, R6 small-j doublings, Q4 NTT structure)

(former file: S21.md)

Source: aac7 `~/s21/{results.txt,summary.txt,log/*}`, driver `archive:drivers/ecalc/s21_batch.sh` on origin/s21 (worktree 58c61c6d). Ran 2026-10-07 21:02-21:13 EDT on hold 12377
(nodes aac7-n50, s5b1n0, s6b0n0) and a CPX node of hold 12294 for R2. All runs rc 0, VERIFY OK; gates passed (digits identical with every switch
off and on against the 1e9 reference; `t_ntt 24` VERIFY OK). `~/s21/prev1` (build error) and `prev2` (false DIFFERS from a missing `tools/unpack_digits`) are
the failed earlier attempts; ignored. Labels: (m) measured, (mod) modelled, (a) assumed.

#### 1. A37-R2: seed microbench `b_seed64` (CPX node, 192 threads, MI300A CPU) (m)

Schemes: dec18 = ecalc's decimal seed (`bi_span_step`); b64kK = a37v1-style base-2^64 K-way (K = 4...32). 451 leaves x 4 schemes, P and Q compared as decimal
strings: **identical in every case**. Two repetitions agree to 0.1 % (1 thread) and 5-20 % (all-core wall).

| case (rank) | nspan | dec18 ns/leaf, 1 thread | b64k32 ns/leaf, 1 thread | ratio dec18/b64k32 | seed s, 1t x nspan / 192 (dec18 / b64k32) | all-core wall x nspan (dec18 / b64k32) s |
|---|---|---|---|---|---|---|
| 10 nodes, rank 0 | 26.46 M | 36.29 k | 34.06 k | 1.07 | 5.00 / 4.69 | 13.4-15.4 / 7.1-7.2 |
| 10 nodes, rank 5 | 28.65 M | 35.09 k | 35.44 k | 0.99 | 5.24 / 5.29 | 10.3 / 8.0 |
| 576 nodes, rank 0 | 22.64 M | 36.35 k | 33.86 k | 1.07 | 4.29 / 3.99 | 11.5-12.3 / 6.1 |
| 576 nodes, rank 288 | 28.27 M | 30.49 k | 35.31 k | 0.86 | 4.49 / 5.21 | 9.0-9.2 / 7.9 |

- b64k32 is the best b64 variant (K=4: 48 k, K=8: 38.4 k, K=16: 35 k ns/leaf). Its time is 81-83 % `combine` (the same big multiplications), 11 % convert, 5 % accumulate.
- **Single-thread: no gain** (ratio 0.86-1.07, i.e. -14 % to +7 %). The CPU rewrite does **not** close the seed gap by itself. This matches A37CMP's expectation (assumed) that a37v1's leaf cost does not beat ecalc's.
- **All-core:** the projected "1 thread / 192" figure (4-5 s) is 2-3x too optimistic: the measured all-core wall scaled to nspan is 9-15 s for dec18, which
  agrees with production (single-node seed spans 10.7 s, section 2). The b64k32 all-core number is 6-8 s, 25-45 % less (m, noisy: dec18's all-core
  wall varies 10-15 s between reps). If real in production it would be worth about -3...-5 s of seed-thread time (mod); it is not measured in the engine.
- **Verdict for the A37-R1 gate:** the gate is **open** (a CPU rewrite does not already close the gap). But the number that matters for R1 is the seed
  thread's *exposed* wait, which section 2 shows is small.

#### 2. A37-Q1: init timeline at 6.441e10 on one node (`ECALC_INIT_TL=1`) (m)

Two runs on aac7-n50 (aac7, so the mapping is the slow aac7 mapping, 0.070 s/GB; total 96.4 / 93.4 s; init 14.3 / 14.4 s; VmHWM 26.9 GB).

| event (s since `rns_init` began) | run 1 | run 2 |
|---|---|---|
| plane pools done | 6.55 | 6.82 |
| seed thread starts | 6.61 | 6.88 |
| seed: 2 chunks computed, waits for region pools | 10.83 | 11.11 |
| region pools there (waited) | 14.11 (3.28 s) | 14.20 (3.10 s) |
| bs joins the seed thread | 14.45 | 14.57 |
| last background mapping done | 27.84 (APU0) | 23.97 (APU2) |
| every seed span computed (spans) | 25.09 (10.74 s) | 24.64 (10.75 s) |
| seed thread ends (thread wall) | 25.50 (18.90 s) | 24.89 (18.00 s) |
| bs level 1 starts | 27.11 (APU0 waited 1.11 s for arena chunks) | 25.39 |

- **Seed vs mapping on aac7:** they finish together. The seed ends 2.3 s *before* the last mapping in run 1 and 0.9 s *after* it in run 2; bs waits about 11 s at the join (14.5 to 25.5). Neither binds clearly.
- **Seed cost without the pool wait:** thread wall 18.0-18.9 s minus 3.1-3.3 s waiting for the region pools = **about 15 s** (m), of which the span arithmetic is 10.7 s.
  A37CMP's modelled 22 s seed (from the SC rehearsal at 9.17e10) is therefore too high for 6.44e10 by about 7 s (m vs mod).
- **Target (mod, low confidence):** with the mapping 7x faster (MAP_RATE 0.010 s/GB), plane pools (5.3 s) and the background mapping (5-14 s per APU, much of it queueing for the mapper
  lock) shrink to about 1-2 s, so the seed thread would start at about 1 s, wait nothing and end at **about 16-17 s** (1 + 15). The target's fixed reference init is 16.5 s (A37CMP).
  So the seed and the init floor would again finish together, within about 0-3 s. A GPU seed at 10 s (mod, a37v1's N13 scaled) would save **about 0-6 s**, not 7-12 s.
- **Go / no-go for A37-R1 (GPU seed): NO-GO now (confidence: medium).** The gain is within 0-6 s (mod) for a 2-4 day change with ordering risks; the
  single-node target measurement of Q1 itself (not aac7) is what would change this. The cheapest decisive step: run `ECALC_INIT_TL=1` once on the target's share (a few minutes) and look at "seed thread ends" against the last mapping.

#### 3. A37-R4: split of `dc` (DC_STATS, one node, 6.441e10 digits) (m)

Two runs; the output loop is 4.10-4.11 s over 240 chunks of 14.9 M limbs (3.578e9 limbs).

| part | s | share of loop |
|---|---|---|
| D2H fetch (27-28 GB/s; slowest chunk 0.016-0.027) | 1.03-1.06 | 26 % |
| limb reversal | 0.48 | 12 % |
| T1 digit residues | 2.26-2.27 | 55 % |
| T2 windows | 0.32 | 8 % |
| writer wait | 0.00 | 0 % |

- The loop is streamed with the low product, so the **on-the-clock `dc` is only 0.02-0.03 s** at one node (m); at 10 nodes S19B measured 2.8-4.1 s (that includes the all-gather and the multi-node loop).
- **No ASCII formatting is in the run**: `ECALC_OUT_PACKED=1` (default since 2026-09-27) writes base-1e18 limbs as they are; formatting happens off the clock in `tools/unpack_digits`.
  The "digit residues" are the T1 checksum `digits == X mod q`, not formatting.
- **Verdict: the fmt18 two-digit table is not worth building for the clock** (formatting share 0 %). The only meaningful piece is the T1 residue loop
  (2.3 s, 55 %), a verification pass that could be overlapped or moved to the GPU for at most about -2 s at 10 nodes (mod); it is bit-identical work but gains little. Recommended next step: none for R4; keep the residue idea in the backlog.

#### 4. A37-R6: reciprocal doublings, `NEWTON_DOUBLING_TS` (one node, k = 1 789 166 669, 3 578 333 337 limbs) (m)

| doubling range | cost, run 1 / run 2 (s) |
|---|---|
| j 2 to 34126 (about 15 doublings) | 0.001-0.003 each; **total about 0.02 s** (to j 27 301: 0.0207 / 0.0200) |
| j up to 1.7 M | each 0.005-0.011; cumulative about 0.07 s |
| last 4 doublings (j 2.2e8 to 1.8e9) | 0.38 / 0.31, 1.16 / 0.61, 1.29 / 1.24, 3.82 / 3.86 |
| whole chain, first to last doubling | 11.24 / 6.85 (including a start-up of **4.14 s** in run 1 vs 0.43 s in run 2 before the first doubling) |

- **The small-j floor on one node is 0.02 s** (m), nothing to remove. The 2.28 s floor of S19B (10 nodes, min) cannot be a one-node small-j cost; at 10 nodes each small doubling is
  a distributed exchange with fixed per-message latency (8.5 us per put, F16), which this single-node run does not exercise.
- **One-APU small-j path: not worth it on this evidence.** To judge it, a 10-node run with the per-doubling clocks (A37-Q8) is needed.
- **New observation:** the chain start-up varied 4.14 s vs 0.43 s between two runs (3.7 s, m); run 1 total was 106.3 s vs 93.9 s (+12.4 s). Cause unknown (first-touch / allocation / cold path); worth one look if it recurs.

#### 5. A37-Q4: forward NTT at 2^31, real vs NOP-modmul (tests/t_ntt vs t_ntt_nop; 2 runs each, median of 5) (m)

| per forward (ms) | real (run 1 / 4) | NOP (run 2 / 3) | real - NOP |
|---|---|---|---|
| whole L31 | 90.39 / 90.56 | 76.65 / 76.87 | **13.7-13.8** |
| pass 0 [26..30] | 23.99 / 23.82 | 17.34 / 17.64 | 6.5 |
| pass 1 [19..25] | 20.83 / 20.82 | 18.96 / 18.95 | 1.9 |
| pass 2 [12..18] | 19.45 / 19.47 | 17.14 / 17.14 | 2.3 |
| pass 3 [0..11] | 25.80 / 25.84 | 24.22 / 23.49 | 1.6-2.3 |
| inverse L31 | 92.8 / 92.8 | 76.5 / 77.0 | 16 |
| L28 whole fwd | 10.29 / 10.37 | 9.17 / 9.09 | 1.2 |
| L24 whole fwd | 0.580 / 0.558 | 0.555 / 0.554 | 0.02 |

- The production plan at 2^31 is **4 passes**; the NOP passes 0-2 reach 1.8-2.0 TB/s, pass 3 only 1.4-1.46 TB/s. Today's real build is 90.4 ms (earlier K13b: 99-100 ms; same hardware class, different tree).
- **Decomposition (ecalc 90.4 ms):** modmul 13.7 ms (15 %), structure + HBM 76.7 ms. Taking a37v1's HBM floor of 51 ms (a, three passes at about 2 TB/s) as the HBM share:
  a 4th pass costs about 17 ms (mod, 34.4 GB / 2 TB/s), structure about 8-9 ms (mod). So **51 HBM + 17 extra pass + 9 structure + 14 modmul = 90 ms** (mod split, m total).
  a37v1: 144 = 51 HBM + 50 structure + 43 modmul (m, its spec). ecalc's modmul share is 3x smaller and its structure 5x smaller.
- **Where the headroom is:** the extra memory pass (about 17-20 ms, 20 %) and, inside it, pass 3 (12 stages, 24 ms against 17 for the others); a 3-pass plan at 2^31 is the only large lever left. Modmul is exposed mostly in pass 0 (6.5 ms). WbPowTab-style twiddle seeding (a37v1 #58) can gain at most the 6.5 ms of pass 0 (mod).
  Note: the NOP build may change scheduling, so "real - NOP" is an upper bound on the modmul-only share (a).

#### 6. Decisions for the user

| decision | options | cost / benefit |
|---|---|---|
| A37-R1 GPU seed | (a) no-go now, measure `ECALC_INIT_TL=1` on the target first; (b) build it | (a) minutes of target time, avoids 2-4 days for 0-6 s (mod); (b) the original -7...-12 s at 576 no longer holds |
| A37-R4 fmt18 | drop (formatting is off the clock) | frees an S item; T1 residues remain the only 2 s |
| 3-pass NTT at 2^31 | (a) investigate; (b) leave | (a) up to about -17...-20 ms of 90 per forward (mod), applies to every large transform; M-L effort; (b) none |

## B08: how fast can the unit tests be? (2026-10-11, branch b08)

(former file: B08.md)

#### 1. Tasks per step (R() / N())
Measured (`SLURM_NTASKS`, `SLURM_PROCID` printed inside the step): on aac7 (1-node hold 12545) and aac6 s24-16
(1-node hold 22908), `srun -N1 --mem=0 --gpus=4 [-n1] --overlap bash -lc ...` starts **1 task** with and without `-n1`.
The "4 copies" seen in B05 §9 were **not reproduced**; a hold submitted with `sbatch -N1 --gpus=4` (NumTasks=1) never
gave more than one. Likely cause: B05's hold had a larger task count (job 22905's request is unknown). Not tested: a
2-node hold on aac7 (no node budget). Change kept as a harmless guard: `R()` and `N()` now use `-n1`.

#### 2. t_ntt 24, one task (measured)
| node | setting | wall | section 3 (convolution) |
|---|---|---|---|
| aac7 | -c 192 (auto), OMP 192 | 285.9 s | 5.5 -> 282.6 s (266 s) |
| aac7 | -c unset, OMP unset | 276.2 s | 6.7 -> 273.2 s (266 s) |
| aac6 s24-16 | -c unset | timeout at 420 s, stuck in section 3 | > 415 s |
| aac6 s24-16 | -c 48, OMP 48 | timeout at 420 s, stuck in section 3 | > 415 s |
Sampling (top/rocm-smi on the node during the stall): one `t_ntt` thread at 80-95 % CPU, all four GPUs at 0 %.
**The stall is a single-thread host wait, not a GPU wait, and `-c` / `OMP_NUM_THREADS` do not matter.**

Cause (read from `tests/t_ntt.c` section 3b): the GPU product is turned into a GMP integer by
`for (i = n; i-- > 0;) { mpz_mul_2exp(C, C, 16); mpz_add_ui(C, C, hz[i]); }`, which is O(n^2) (n up to 2^20, 5 primes x 2 modes).
Fix: carry-propagate into 16-bit limbs and one `mpz_import` (O(n)); same integer, same checks.

#### 3. After the fix (aac6 s24-16, own clone built from b08, -n1)
| setting | wall (measured) | checks |
|---|---|---|
| -c unset | 8.6 s | VERIFY OK (3215 checks) |
| -c 48, OMP 48 | 8.3 s | VERIFY OK (3215 checks) |
Before: > 420 s (aac6), 276-286 s (aac7). Speed-up of t_ntt 24: about 30x on aac7 (modelled from the aac7 section-3 time, 266 s -> ~3 s); aac6 measured > 49x.

#### 4. Whole suite `mnaccept.sh --skip-grid --only unit,e9` (aac7 1 node, measured, with -n1 only, t_ntt not yet fixed)
755 s total, 8 passed, 2 failed: `t_bs` (reference `ref/e_1000000.sha256` missing: ECALC_REF not set in that run) and
`t_ntt 24` (rc 137: step killed at 137 s into section 3 by a SIGKILL I did not send; cause unknown, possibly another agent's
step cancel on the shared account; doubt). a04: about 900 s. The 755 s is therefore a partial figure.
Modelled with the t_ntt fix: 755 - 137 (cut t_ntt) = 618 s for the other tests + ~9 s = about 630 s (modelled, not run;
aac7 was reserved by the coordinator). e9 base10 13.9 s, base2 29.0 s, identical to the reference.

#### 5. Recommendation
1. Adopt the `t_ntt.c` section-3b fix (merge b08): saves ~4.5 min of every unit run on aac7 and removes the >7 min hang on aac6.
2. Leave `MNRUN_CPUS_PER_TASK` as is (auto on aac7); no timing difference between unset / 48 / 192 for t_ntt.
3. Keep `-n1` in `R()`/`N()` (no cost). Set `ECALC_REF` in unit runs so `t_bs` passes.
4. One final full-suite timing after merge when a node is free (expected ~630 s modelled).

#### RESUME
Branch b08 pushed; no jobs or clones left. Open: t_ntt rc 137 cause; 2-node hold task count on aac7 untested.
