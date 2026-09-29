# TARGET.md — the runbook for the 576-node target (PLAN.md §25; Phase 12 agent Q, 2026-09-21; Phase 13b agent D, 2026-09-23: §1, §3, §6; Phase 15 agent DOC, 2026-09-27: the user's decisions — §1, §3, §4, §5, §6, §7; Phase 15 agent TGT, 2026-09-28: **the target is 5.1 × 10¹³** — §1, §3, §4, §5, §6, §7, §8; Phase 15 agent DOC2, 2026-09-28: **the Batch 2 decisions** — `ECALC_NP=auto` and `RNS_DIST_CACHE_FIT=1` on the launch line, `BS_ARENA_ROOM=0.16` and `DIST_TWREC_G=1` by default — §1, §3, §4, §5, §6, §8)

The target: 576 MI300A nodes (4 APUs each, 2 304 APUs), HPE Slingshot-2 dragonfly (diameter 3, groups all-to-all
inside and globally), two 400 Gb/s NICs per APU (100 GB/s per APU, 400 GB/s per node), SHMEM (Cray OpenSHMEMX
expected; rocSHMEM or a SOS-class OpenSHMEM acceptable), `srun`. Everything below was checked against the code at
`main` 7aded87 plus the Phase 12 branches; every environment variable named here exists in the code (§9 lists the
grep). The numbers come from `ecalc/estimate.py` (the model of `mn_model.py` + `mem_model.py`; results/Q.md) and are
labelled **measured** (a recorded aac6 run), **modelled** (arithmetic on measured inputs) or **assumed** (a target
parameter no aac6 measurement can give).

## 1. What to expect (the standing estimate; Phase 13b: the design table)

**Phase 15 (agent DOC2, 2026-09-28 — the user's decisions of 2026-09-28, RESULTS §88: main = B2. This block supersedes the figures
below, which are kept as history.)** The launch line of §4 now carries **`ECALC_NP=auto`** (four primes only for the products over the
three-prime bound; decision 1) and **`RNS_DIST_CACHE_FIT=1`** (the mn transform cache bounded by the budget: **0 slots at the target**;
decision 2); the code's defaults add `BS_ARENA_ROOM=0.16` (decision 3), `DIST_TWREC_G=1` (decision 4) and keep `RNS_POOL1_4Q=1` (decision
5). The target stays **5.1 × 10¹³** (decision 11; re-chosen after P24). `./estimate.py --target` (**modelled**; the fabric **assumed**:
100 GB/s per APU, 2 µs per message; 576 nodes writing at once, each at its single-stream rate, **assumed**); the cache priced at the
slots FIT allows (0), each hit at CX's fitted 0.96 of the modelled saving; the pieces are the C code's own plan with `ECALC_NP=auto`
(**measured** on aac6's login node with the launch line's environment at be2eec3, results/DOC215/sweep_auto.txt — the same counts as
with `ECALC_NP=4`):

| total digits | without the disk write | with it @ 2.0 GB/s | @ 0.8 GB/s | @ 0.6 GB/s | pieces: node 0 / critical path (tree_max + recip + div) | node |
|---|---|---|---|---|---|---|
| **5.10 × 10¹³ (the target: the last size below the step)** | **400.0 s (6.67 min)** | 394.6 s (6.58 min) | 404.1 s (6.73 min) | **420.5 s (7.01 min)** | 222 / 242 (128 + 74 + 40) | **471.9 GB** |
| 5.11 × 10¹³ (node 0 steps; the critical path does not) | 400.5 s | 395.1 s | 404.7 s | 421.1 s | 226 / 242 | 471.9 GB |
| 5.12 × 10¹³ (the step: critical path 242 → 246) | 407.1 s | 401.6 s | 411.3 s | 427.8 s | 230 / 246 | 471.9 GB |
| 5.17 × 10¹³ (the next step) | 436.1 s | 430.6 s | 440.8 s | 457.4 s | 230 / 266 | 480.5 GB (over 480) |
| **4.74 × 10¹³ (one step below: the runtime test after the headline)** | **368.7 s (6.14 min)** | 363.7 s | 375.4 s | **390.7 s (6.51 min)** | 202 / 226 (124 + 68 + 34) | 454.8 GB |
| 4.75 × 10¹³ (its step) | 380.5 s | 375.5 s | 385.5 s | 400.8 s | 209 / 233 | 454.8 GB |
| 4.25 × 10¹³ (the target until 2026-09-27, 23:50 EDT) | 288.6 s | 284.1 s | 297.1 s | 310.8 s | 180 / 182 | 429.0 GB |

- **Why it is longer than yesterday's 344.7 / 376.2 s (TGT, below):** that figure priced the code's default two cache slots (−74 to −81 s)
  that no memory inside 480 GB holds (TC15, CX15: one slot is 68.7 GB per node); with FIT the run takes none. `ECALC_NP=auto` (the
  leaf and the one-node tiers on three primes, the distributed pieces over the bound on four) is −23.3 s against `ECALC_NP=4`,
  `DIST_TWREC_G` −6.8 s; the arena room has no modelled time at size > 1 (its division remaps exist at size 1 only).
- **The step margin** is as before: 5.10 × 10¹³ is the last size at 222 / 242 pieces; 5.11 steps node 0, 5.12 the critical path
  (**+7.1 s**, modelled). The plan check at 5.1 × 10¹³ with the launch line's environment: **`plan check 5.1e+13 digits g 576,
  ECALC_NP=auto: OK -- 1240 products`**, the largest piece 1.096 × 10¹² limbs, `bound per product (four primes over 58424467928 terms:
  151 of 264 pieces)`; **`plan primes … pieces at four primes tree 76 of 108, recip 35 of 74, div 40 of 40 | leaf dist_db 0 of 12,
  recip single-node chain 0 of 30`**; `plan pool` 9472 MiB (measured, login node, results/DOC215/plan_51e13_auto.txt).
- By phase at 5.1 × 10¹³ (modelled): init 20.1 + seed wait 12.1 + batch 24.2 + top levels 27.0 + distributed levels 185.9 +
  reciprocal 54.0 + division 68.8 + other 0.2 + the digits' residues 5.4 + the exit 2.4 = 400.0 s. The part file 39.35 GB per node
  packed; 19.7 / 49.2 / 65.6 s of writing at 2.0 / 0.8 / 0.6 GB/s, of which 39.7 s hide under the division (fitted at size 1, assumed
  at 576). With the ASCII file: 399.2 / 465.6 / 502.5 s at 2.0 / 0.8 / 0.6 GB/s.
- **Node memory 471.9 GB** (modelled; = max(init 421.5 + host 44.4, bs 427.5 + 44.4, dm 429.8 + 28.8); arena 300.6 = the dm need 296.9
  in whole VMM chunks, planes 120.9 at 2³¹ with pool 0 at four planes and pool 1 at three; `mem_model.py --p15`), **8.1 GB below 480**.
  The C layout with the launch line's environment (`BS_LAYOUT_ONLY=88541666666.6667:576`, measured on the login node, `mem_model.py
  --check-c` exact): planes 120.88 + arena 300.65 = device 421.53 GB, node 443.12 GB with the layout's own host figure. The arena
  room is **+16.5 GB** per node here (284.15 → 300.65 GB of arena, measured by the layout: 4 × 0.16 × the 19.98 GB hole + the chunk
  rounding), not the ≈ 9 GB first quoted (that was the one-node share's): the node is 471.9 GB, not ≈ 464. The mn transform cache
  adds **0** (FIT: room 480 − 443.12 − 32 = 4.88 GB < one slot, `plan cache … -> 0 slots`, measured). **The settings that now decide
  whether the target fits** (each alone, modelled): `ECALC_NP=4` 489.1 GB, `COMM_SHMEM_ROUND_MB` off 512.5, `MN_T_CHUNK_MB=0` 497.7,
  `MN_TREE_EARLY_FREE=0` 489.1, `DM_TIGHT=0` 506.3 — each over 480; `BS_ARENA_ROOM=0` 455.4; the cache without FIT (the code's 2
  slots) 609.4, one slot 540.7. The largest size inside 480 GB is 5.167 × 10¹³ (1.3 % above the target; 435.7 / 457.0 s, past a step).
- **The mn transform cache (modelled, `estimate.py --target --cache-slots n`)**: at 5.1 × 10¹³ one slot would be 350.3 / 379.0 s (−49.7 s)
  and two 325.7 / 356.2 s (−74.3 s), at 540.7 / 609.4 GB per node — neither fits. CX's partial slot (a slot of 1–2 primes, up to
  −26.7 s, CX15 §3) is Batch 3's (after DKM). At 4.74 × 10¹³: 324.9 / 353.3 s at 1 slot, 302.1 / 332.2 s at 2 (523.5 / 592.2 GB).
- **`ECALC_NP=4` instead of auto** (modelled): 423.3 / 443.8 s at 5.1 × 10¹³ and **489.1 GB — over 480** (+17.2 GB: pool 1 at 4 q with `RNS_POOL1_4Q`;
  pool 0 holds four planes under auto as well); 392.1 / 414.1 s, 471.9 GB at 4.74 × 10¹³.
- **The top node's share** is unchanged (9.169 × 10¹⁰ digits, 1.0356 × the average). Its one-node rehearsal on today's defaults:
  `./ecalc 91694091804` (at size 1 `ECALC_NP=auto` is three primes): the C layout (measured, login node) planes 103.70 + arena 268.44
  = device 372.13 GB, node 387.72 GB; `mem_model` 405.1 GB, 162.0 s without the write (modelled). Measured on s24-16 (RESULTS §88):
  `ECALC_NP=auto` 159.6 / 163.0 s (388 GB), with the room 156.5 / 162.4 s (397 GB).
- **The steps before the headline** (§5, modelled on the launch line; FIT allows both cache slots at these sizes and the model prices
  them): step 3 (64 × 10¹⁰) 36.0 / 40.6 s; step 4 (576, 10¹²) 17.0 / 17.8 s; step 5 (576, 2.2 × 10¹³) 121.5 / 139.4 s (2.0 / 2.3 min),
  334.5 GB (FIT's room 142 GB: 2 slots, 137.4 GB, within the budget by the code's rule).
- The disk and off-the-clock figures of the TGT block below are unchanged (39.35 GB per node packed, 22.7 TB; 51.0 TB ASCII).

**Phase 15 (agent TGT, 2026-09-28 — history since DOC2 (2026-09-28, the Batch 2 decisions): the block above supersedes it. The user's decision of 2026-09-27, 23:50 EDT: the target is 5.1 × 10¹³ digits
on 576 nodes; it was 4.25 × 10¹³. Its times priced two cache slots that do not fit, on `ECALC_NP=4`.)** The same launch line (§4: the defaults of
2026-09-27, `ECALC_NP=4`, `COMM_SHMEM_ROUND_MB=1024`, the packed part file started at the division's hook, no top set) with the
argument `51000000000000`. `./estimate.py --target` (**modelled**; the fabric **assumed**: 100 GB/s per APU, 2 µs per message;
576 nodes writing at once, each at its single-stream rate, **assumed**); the pieces are the C code's own plan (`MN_PLAN_ONLY`,
**measured** on aac6's login node with the launch line's environment, results/TGT15/sweep576.txt):

| total digits | without the disk write | with it @ 2.0 GB/s | @ 0.8 GB/s | @ 0.6 GB/s | pieces: node 0 / critical path (tree_max + recip + div) | node |
|---|---|---|---|---|---|---|
| **5.10 × 10¹³ (the target: the last size below the step)** | **344.7 s (5.75 min)** | 339.3 s (5.66 min) | 359.8 s (6.00 min) | **376.2 s (6.27 min)** | 222 / 242 (128 + 74 + 40) | **455.4 GB** |
| 5.11 × 10¹³ (node 0 steps; the critical path does not) | 345.3 s | 339.9 s | 360.5 s | 376.9 s | 226 / 242 | 455.9 GB |
| 5.12 × 10¹³ (the step: critical path 242 → 246) | 349.9 s | 344.4 s | 365.1 s | 381.6 s | 230 / 246 | 456.4 GB |
| 5.17 × 10¹³ (the next step) | 374.1 s | 368.6 s | 389.7 s | 406.4 s | 230 / 266 | 459.0 GB |
| **4.74 × 10¹³ (one step below: the runtime test after the headline)** | **320.3 s (5.34 min)** | 315.3 s | 336.3 s | **351.5 s (5.86 min)** | 202 / 226 (124 + 68 + 34) | 439.2 GB |
| 4.75 × 10¹³ (its step) | 328.5 s | 323.5 s | 343.4 s | 358.6 s | 209 / 233 | 439.7 GB |
| 4.25 × 10¹³ (the target until 2026-09-27, 23:50 EDT) | 256.0 s | 251.5 s | 272.0 s | 285.7 s | 180 / 182 | 416.0 GB |

- **The step margin**: 5.10 × 10¹³ is the last size, in steps of 0.01 × 10¹³, at 222 / 242 pieces (the plan is flat from 5.05 × 10¹³).
  At 5.11 × 10¹³ node 0's count steps to 226 while the critical path stays 242; at 5.12 × 10¹³ the critical path steps to 246
  (+5.2 s, modelled). So the margin to the next step is **0** at the 0.01 × 10¹³ resolution (the node-0 step) and 0.02 × 10¹³ to the
  step that costs time — a rounding of the digit count upward is not free. The plan check at 5.1 × 10¹³: **`plan check … OK`**,
  1240 products, the largest piece 1.096 × 10¹² limbs (dist_mn level 8, the top combine's P), transforms up to 2⁴⁰ of the roots' 2⁴⁴.
- By phase at 5.1 × 10¹³ (modelled): init 19.3 + seed wait 12.9 + batch 30.8 + top levels 30.5 + distributed levels 150.1 +
  reciprocal 43.4 + division 49.7 + other 0.2 + the digits' residues 5.4 + the exit 2.4 = 344.7 s. The part file: **39.35 GB per
  node packed** (8.854 × 10¹⁰ digits × 8/18 B), 22.67 TB in all; 19.7 / 49.2 / 65.6 s of writing at 2.0 / 0.8 / 0.6 GB/s, of which
  28.7 s hide under the division (the early writer's overlap, fitted at size 1 and assumed at 576). With the ASCII file
  (`ECALC_OUT_PACKED=0`: 88.5 GB per node, 51.0 TB) it would take 355.2 / 421.6 / 458.5 s (modelled).
- Against 4.25 × 10¹³: +20 % digits for +88.7 s (+35 %) without the write; the pieces on the critical path 182 → 242, the
  tree's levels 107.0 → 150.1 s. `ECALC_NP=auto` (agent NP, branch `p15-NP`: four primes only where the pieces need them) is
  **pending the user's Batch 2 decision**; the integrator's model of it at 5.1 × 10¹³ (`estimate.py --np-mn auto` on that branch):
  322.6 s without the write, 354.1 / 337.7 / 317.2 s at 0.6 / 0.8 / 2.0 GB/s, node 455 GB (modelled). Until adopted, the target runs
  `ECALC_NP=4`.
- **Node memory 455.4 GB** (modelled; = max(init 405.0 + host 44.4, bs 411.0 + 44.4, dm 417.0 + 28.8); arena 284.1, planes 120.9 at
  2³¹; `mem_model.py --p15`), **24.6 GB below 480**. The C layout (`BS_LAYOUT_ONLY=8.854e10:576`, measured on the login node):
  planes 120.88 + arena 284.14 = device 405.02 GB, node 426.61 GB with the layout's own host figure. The SHMEM pool stays **9472
  MiB** (`plan pool`, measured on the login node; heap ≥ 9984 MiB). AS's `BS_ARENA_ROOM=0.16` (pending adoption) adds ≈ 9 GB per
  node (≈ 464 GB). **Three defaults are now load-bearing at the target** (modelled): without `COMM_SHMEM_ROUND_MB=1024` the node is
  496.0 GB, with `MN_T_CHUNK_MB=0` 490.5 GB, with `MN_TREE_EARLY_FREE=0` 482.5 GB, with `DM_TIGHT=0` 502.8 GB — each over 480.
- **The top node's share**: the code gives each node the same number of terms, so at 5.1 × 10¹³ the top node holds
  **9.169 × 10¹⁰ digits = 1.0356 × the average 8.854 × 10¹⁰** (node 0: 6.849 × 10¹⁰, 0.774 ×), from the plan's N = 4 184 661 447 309
  terms (modelled, exact arithmetic on the code's term split: `mn_model.top_factor`). At 4.74 × 10¹³: 8.523 × 10¹⁰ (1.0357 × 8.229 × 10¹⁰).
- **The one-node rehearsal of the top node** (the equivalent of Phase 13's 7.64 × 10¹⁰ share run): `./ecalc 91694091804` on one aac6
  node **fits**: `BS_LAYOUT_ONLY=91694091804:1` (the defaults, three primes; measured on the login node) device 362.93 GB, node 378.52
  GB; `mem_model` 396 GB with the host; `MN_PLAN_ONLY=91694091804:1` plan check OK, 113 pieces (113–114 from 8.8 to 9.2 × 10¹⁰;
  the next size-1 step, 123 pieces, is at or below 9.3 × 10¹⁰); modelled 174.2 s without the write. With `ECALC_NP=4` (the target's primes): layout node 395.70 GB, model
  413 GB, 194.0 s (modelled). Both are well inside 480 GB and aac6's measured 524 GB edge.
- **The disk** (Lustre `/ssd0`, 122 TB shared): the packed parts 22.67 TB (18.6 %); the ASCII parts after the conversion 51.0 TB; both
  kept until the checks pass: **73.7 TB (60 %)**. A single concatenated ASCII file beside its parts and the packed parts would be
  124.7 TB — **over the file system: do not concatenate** (the parts, in order, are the file; trap 14). The free space on the day is
  unknown (**assumed** available: check `lfs df -h` first). At 4.74 × 10¹³: 21.07 TB packed, 47.4 TB ASCII.
- **Off the clock, per node, the nodes in parallel** (modelled from results/IO215.md's measured conversion, which runs at the disk's
  write rate, 0.58–0.59 GB/s per stream): RECHECK of the packed part reads 39.35 GB (46–50 s at Lustre's 0.78–0.86 GB/s read);
  the conversion writes **88.5 GB of ASCII per node: 111–148 s (1.8–2.5 min) at 0.8–0.6 GB/s** (the 39.35 GB read, 24 s at 1.67 GB/s,
  and the formatting hide under the write); RECHECK of the ASCII parts reads 88.5 GB (103–114 s). All of this assumes the file
  system carries 576 streams at once (≈ 346 GB/s of aggregate writing); at an aggregate A GB/s it is 51.0 TB / A instead (e.g.
  8.5 min at 100 GB/s, **assumed** for illustration).

**Phase 15 (agent DOC, 2026-09-27 — history since 2026-09-28: the target was then 4.25 × 10¹³).**
The code's defaults of 2026-09-27 (`ecalc/README.md`'s header list) on the launch line of §4 — **four primes** (`ECALC_NP=4`:
three cannot hold the target's pieces, and `MN_PLAN_ONLY` refuses them), `COMM_SHMEM_ROUND_MB=1024`, the part file **packed**
(0.444 B/digit: 32.8 GB per node, 18.9 TB in all) and started at the division's hook (`MN_OUT_EARLY=1`), no top set.
`./estimate.py --target` (**modelled**; the fabric **assumed**: 100 GB/s per APU, 2 µs per message; 576 nodes writing at once
each at its single-stream rate **assumed**):

| total digits | without the disk write | with it @ 2.0 GB/s | @ 0.8 GB/s | @ 0.6 GB/s | pieces (tree_max + recip + div) | node |
|---|---|---|---|---|---|---|
| **4.25 × 10¹³ (the target)** | **256.0 s (4.27 min)** | 251.5 s (4.19 min) | 272.0 s (4.53 min) | **285.7 s (4.76 min)** | 88 + 66 + 28 = 182 | **416.0 GB** |
| 4.29 × 10¹³ (the last size below the step) | 256.4 s | 251.9 s | 272.7 s | 286.5 s | 182 | 418.1 GB |
| 4.30 × 10¹³ (the step: tree_max 88 → 100) | 271.0 s | 266.5 s | 287.5 s | 301.3 s | 194 | 418.6 GB |
| the 480-GB ceiling: 5.57 × 10¹³ (9.67 × 10¹⁰ per node) | 411.9 s (6.9 min) | 406.0 s | 426.9 s | 444.8 s (7.4 min) | 166 + 79 + 46 | 479.8 GB |

- By phase at 4.25 × 10¹³ (modelled): init 17.2 + seed wait 10.9 + batch 25.3 + top levels 19.6 + distributed levels 107.0 +
  reciprocal 33.5 + division 35.5 + other 0.1 + the digits' residues 4.5 + the process's exit 2.4 = 256.0 s. The part file:
  32.8 GB at 0.6 / 0.8 / 2.0 GB/s = 54.7 / 41.0 / 16.4 s of writing, of which 20.5 s hide under the division (the size-1
  overlap, **fitted** there and **assumed** at 576). At 2 GB/s the whole write hides and the with-write wall is below the
  no-write one: without a digit file the residue pass runs after T1 (4.5 s), with one it runs inside the early writer.
- Against the Phase 14 defaults on four primes (P15 option (a): 277.9 s without the write, 396.3 s with the ASCII file at
  0.6 GB/s, **modelled**): −21.9 s of compute (the seed fill −13.5 s of bs, the fast `mul_1` −7.3 s of seed wait, the middle
  product and `DIST_TWREC` −3.6 s of the distributed products, +2.4 s of exit now counted) and −88.7 s of exposed write (packed + early).
  With the ASCII file (`ECALC_OUT_PACKED=0`) the target would take 267.9 / 323.2 / 354.0 s at 2.0 / 0.8 / 0.6 GB/s.
- **Node memory 416.0 GB** (modelled; = max(init 365.6 + host 44.4, bs 371.6 + 44.4, dm 377.9 + 28.8)); the fourth prime costs
  +17.2 GB of pool 0 (16 instead of 12 GiB per APU), the fill and the early writer nothing that binds at the target (the arena
  is the dm need's, 244.7 GB, which the fill leaves unchanged; `mem_model.py --check-c` exact against `BS_LAYOUT_ONLY` with
  `ECALC_NP=4`). The SHMEM pool stays **9472 MiB** (`plan pool`, measured on the login node with `ECALC_NP=4`).
- **The grid step**: 4.25 × 10¹³ is 1.2 % below the step at 4.29 → 4.30 × 10¹³ (the code's plan with `ECALC_NP=4`:
  results/P15/sweep576_np4.txt; the grids are the same at three and four primes); the step costs +15.0 s.
- **The calibration** (`mn_model.py --calib15b`): at 10¹¹ on one node the model is within −3.1 … +2.0 % of the integrator's
  paired series (RESULTS §86, measured): the new defaults 224.7 s with the ASCII file, **203.7 s packed**, 193.6 s without
  a file; the Phase 14 defaults (B0) 296.9 / 217.6 s. The node-to-node spread in that series (s24-16 against s24-26) is ≈ 5 %.
- What is **assumed** beyond the fabric: the fill's bs gain (−23 % of bs without the seed wait, measured at 10¹¹) carries to
  the target's leaf; `DIST_TWREC`'s −2 % carries to the mn tier's local passes; the fill's division remaps (+4.9 s net at 10¹¹ on
  one node) do not occur at size > 1; the early writer's Lustre traffic does not slow the division's exchanges on the same
  Slingshot NICs (aac6's shared 1 GbE link slowed the low product by 12.9 s: results/IO15.md §2.6 — measure at step 3).

**Phase 13b (agent D).** The estimate is now of the code after step 0 (three primes `ECALC_NP=3`, `NTT_MODMUL=1`) and of
every design that still differs: `ecalc/design_table.py` prints the 96 combinations of product strategy
(`RNS_STRATEGY`), plane cap (`ECALC_PLANE_CAP`), exchange-scratch chunking (`MDB_SHIFT_CHUNK_MB`, `MN_T_CHUNK_MB`) and
uneven-exchange depth (`COMM_ALLTOALLV_DEPTH`) to `results/DESIGN_TABLE.md`. Each row gives the one-node 4 × 10¹⁰ wall and
peak, the 576-node maximum digits at 502 and 480 GB, the wall at that maximum and at a common 4 × 10¹³, and that wall at
50 and 200 GB/s per APU. Every cell is labelled measured / modelled / assumed. The table as committed takes the M-run's
measurements (`--mrun results/mrun_13b.log`): the one-node inputs of every C / auto / B4 row at all four caps are measured (K's
kernels on); the 576-node cells stay modelled on them.

| design (576 nodes, 100 GB/s per APU assumed) | per node (502 GB) | digits (502 / 480 GB) | wall at 4 × 10¹³ | wall at the 502-GB maximum |
|---|---|---|---|---|
| step 0 alone (C, the cap rule = 2³¹ at 576, no chunking, depth 1) | 7.28 × 10¹⁰ | 4.19 / 3.94 × 10¹³ | 3.70 min | 3.81 min |
| fastest: `RNS_STRATEGY=auto ECALC_PLANE_CAP=2^31 COMM_ALLTOALLV_DEPTH=2` | 7.2 × 10¹⁰ | 4.16 / 3.89 × 10¹³ | 3.59 min | 3.66 min |
| recommended: the fastest + `MDB_SHIFT_CHUNK_MB=1024 MN_T_CHUNK_MB=1024` | 9.6 × 10¹⁰ | 5.52 / 5.19 × 10¹³ | 3.83 min | 6.30 min |
| largest: `RNS_STRATEGY=B4 ECALC_PLANE_CAP=2^30`, both chunkings, depth 1 | 1.1 × 10¹¹ | 6.40 / 6.04 × 10¹³ | 5.41 min | 12.2 min |

`./estimate.py --max --g 576` gives step 0 alone; `--strategy --cap --chunk --depth` give any row. The table also prints the ceiling at
524 GB, the edge measured on one aac6 node (agent P: device + host HWM 523.8 GB ran, 529.6 GB was OOM-killed). **Size the target by
the 502 / 480 GB columns until the target's own edge is measured** (§6 item 9).

Labels:
- **Modelled**: the per-node compute, from measured one-node runs of the current code (four primes: 4 × 10¹⁰ 81.5 s,
  8 × 10¹⁰ 190.7 s, 10¹¹ 262.9 s; three primes: 4 × 10¹⁰ 68.3 s) and S13's measured per-product times.
- **Modelled**: the memory, from the code's own sizing formulas (within 0.05 % of every measured device total).
- **Assumed**: the fabric (100 GB/s per APU, 2 µs per message), the part file (2 GB/s per node — *Phase 15*: the target's
  `/ssd0` is Lustre, 0.6–0.8 GB/s single-stream measured there: 316–347 s ASCII, 264–278 s packed, modelled on three primes
  (2026-09-26); *2026-09-27*: 272–286 s packed with the early writer on four primes, the block above; §6 item 5), and the cost of one
  extra exchange round (`T_ROUND`, 0.03 s, range 0.01–0.1 s: the aac6 chunk sweep shows no trend above its noise).

The ranking of the rows does not change between 50 and 200 GB/s per APU (Spearman ≥ 0.999). The chunk rounds' cost does
move the chunked rows: the recommended row takes 3.70 min at 0.01 s per round and 4.47 min at 0.1 s. The product
strategy barely moves the 576-node wall (it acts only on each node's own top levels, and B / B4 pay for their extra
planes in mapping time), auto (never more memory than C) is the recommendation; at size 1 it is the fastest form measured. The exposed
communication is about 25 % of the 576-node wall. That follows from X13's measured overlap: the equal-slab path hides
3/4 of its xGMI time, and the general map hides 1.4 % at depth 1 and 74 % at depth 2 (X13b, two real nodes).

The Phase 12 figures below are kept for reference (four primes, before step 0; `estimate.py --legacy` reproduces them;
Phase 13a superseded the 6.7 × 10¹⁰ ceiling with 6.95 × 10¹⁰, M13, and step 0 raises it to 7.29 × 10¹⁰).

| per node | digits total | per-node wall | node peak | fits |
|---|---|---|---|---|
| **3.8 × 10¹⁰** (every level product one piece) | 2.19 × 10¹³ | **2.0 min** | 395 GB | yes, with margin |
| **6.1 × 10¹⁰** — the safe size (480 GB) | **3.5 × 10¹³** | **3.8 min** | 479 GB | yes, with margin |
| **6.7 × 10¹⁰** — the ceiling (502 GB) | **3.9 × 10¹³** | **4.0 min** | 501 GB | no margin |
| 7.7 × 10¹⁰ (Phase 11's headline) | 4.4 × 10¹³ | 4.7 min | 541 GB | **no** |

Per-node compute measured (one node: 4 × 10¹⁰ in 81.5 s, 8 × 10¹⁰ in 195.5 s, 10¹¹ in 262.9 s); the distributed
levels, the sharded division and the memory at g > 1 modelled; the fabric's 100 GB/s per APU, 2 µs per message and
the part file's 2 GB/s per node assumed. The exposed communication is 15–18 % of the wall; the distributed tree
levels are ≈ 35 % of it. The table assumes the two Phase 12 forms: agent G's gridded top product (`--tree grid`)
and agent S's pool-resident slabs (`--staging resident`). **With the code as merged at 7aded87 (`estimate.py
--as-is`) the ceiling is 9.5 × 10⁹ per node = 5.5 × 10¹² digits**: the tree's arena request (trap 6) alone caps at
1.9 × 10¹⁰ per node, and the SHMEM transport's staging (trap 11: every live communicator keeps its largest
exchange's send + receive staging in the pool, 345 GB per node at 576) alone at 1.9 × 10¹⁰ too. Freeing the
staging after every exchange (`--staging per_exchange`, a small change in `comm_shmem.c`) gives 5.6 × 10¹⁰ per
node (3.2 × 10¹³ digits, 3.3 min) without S's resident slabs.

Sensitivity (576 × 6.1 × 10¹⁰; `estimate.py --g 576 --D 6.1e10 ...`): per-message cost 2 → 20 µs: 3.8 → 4.3 min;
injection 100 → 50 GB/s per APU: 4.6 min; part files at 1 GB/s: 4.3 min; global links at half the injection
(`--taper 0.5`): 4.1 min; the third layer (`--layers 3`): 4.5 min (it doubles the NIC bytes; it pays only if the
per-message cost is the limit).

> **2026-09-29, the target's write rate (the user's Lustre test): ≈ 1 GB/s per node.** The models now use 1.0 GB/s by default (`mn_model.TARGET_WRITE_BW`; 0.6 and 2.0 still printed). At 1.0 GB/s on B2 the packed part file is fully hidden under the division: **5.1 × 10¹³ in 400.0 s without the write, 394.6 s with it** (modelled; the with-write wall is lower because the digit residues run inside the writer). With P24 + DKM (Batch 3, pending the user's decision) ≈ 285 / 305 s. ASSUMED: every node keeps ≈ 1 GB/s with 576 nodes writing at once (measure the aggregate at bring-up, §6 item 5(b)). Figures below at 0.6 GB/s are kept as history.

> **2026-09-29, B3: the target is 5.276 × 10¹³ digits** (the user's Batch 3 decision; `ecalc 52760000000000`). Defaults add `MN_P24=2`, `NEWTON_DKM=1`, DKM's arena layout and the corrected room check (DL: the room is kept up to 5.396 × 10¹³; the absolute ceiling is 5.532 × 10¹³). Plan check OK (149 pieces, 159 on the critical path). Modelled: **292.4 s (4.87 min) without the write, 313.8 s (5.23 min) with the packed write at 1 GB/s**, node ≈ 471.9 GB. Launch line unchanged otherwise (`ECALC_NP=auto`, `RNS_DIST_CACHE_FIT=1`, `COMM_SHMEM_ROUND_MB=1024`, `ECALC_MEM_GUARD_GB=6`). Off by default, for later decisions: `MN_OUT_DKM_HI` (EW, −7 s with the write), `RNS_DIST_CACHE_PARTIAL` (PC, −6…−14 s). Open risk: the multi-node division's peak is counted, not measured (≈ 1.4 GB margin; DL15). Figures below are history.

## 2. Build

```
cd ecalc
make                      # SHMEM=1 is the default where `oshcc` exists (aac6: OpenMPI OSHMEM); on the target set the flags:
make SHMEM=1 SHMEM_CFLAGS="-DCOMM_SHMEM $(cc --cray-print-opts=cflags)" SHMEM_LIBS="$(cc --cray-print-opts=libs) -lsma"
                          # Cray: the SHMEM headers/libs from the cc wrapper (module load cray-openshmemx); or for SOS/rocSHMEM:
make SHMEM=1 SHMEM_CFLAGS="-DCOMM_SHMEM -I$SOS/include" SHMEM_LIBS="-L$SOS/lib -lsma"
make SHMEM=0              # without the transport (COMM_TRANSPORT=shmem then aborts at start; TCP is the fallback)
```

`HIPCC`, `ARCH` (gfx942) as in the Makefile; `hipcc` links with lld, so a SHMEM library that does not carry its own
dependencies needs them on `SHMEM_LIBS` (aac6's OSHMEM: `-lopen-rte -lopen-pal`). Check the build once:
`./tests/t_comm` under `srun -N1 -n2` (§4) and `./ecalc 1000000000 /tmp/e9.txt` on one node, `cmp` against
`ref/e_1000000000.txt` (identical, 14–16 s).

## 3. Environment — every variable that matters, with the target's value and why

Transport (`comm_shmem.c`, `mn.c`; results/S.md):

| variable | target | why |
|---|---|---|
| `COMM_TRANSPORT=shmem` | set | selects the SHMEM transport (default TCP: `COMM_HOSTS`/`COMM_PORT`, the aac6 correctness path) |
| `COMM_SHMEM_SERIAL=0` | set (Cray / SOS) | one context per communicator and blocking `wait_until`; the default 1 is one process-wide lock around every library call (OSHMEM 4.1's `SHMEM_THREAD_MULTIPLE` is nominal). The code falls back to serial if `shmem_init_thread` does not provide MULTIPLE |
| `COMM_SHMEM_DEVHEAP=1` | set where the symmetric heap is device memory (Cray on the APU, rocSHMEM); 0 with a host heap | skips the `hipHostRegister` of the pool; the staging copies become D2D. Untested on aac6 (no such implementation): run `t_comm` and `t_dist` first (§4) |
| `COMM_SHMEM_POOL_MB` | **from the measured law (Phase 14 P2, results/P214.md)**: `MN_PLAN_ONLY=<digits>:<g> ./ecalc` prints it (`plan pool`); at 4.25 × 10¹³ on 576 nodes **77824** with the defaults (81.6 GB: the node total 525 GB does not fit 480) or **43008** with `MN_T_CHUNK_MB=1024` (45.0 GB, node 460 GB); `COMM_SHMEM_POOL_AUTO=1` sets it at init | every exchange of `ecalc` is staged through the pool (per exchange, released after it): the pool holds 4 APU threads × the largest exchange's send + receive (the division's A_h mu result exchange: my rows of the piece + a quarter of my share of C inside it) + the control blocks (0.9 GB at 576) — measured to 0.1 MiB at 10⁸–10¹⁰ on 2 nodes and 4 processes. A pool too small stops the run with `comm_shmem: pe r: the symmetric pool … cannot hold …`, naming the model's need; below the need at init a warning names it. *Phase 14 V1*: `COMM_SHMEM_POOL_AUTO=1` is the default (the pool raised to the need at init), and a heap set on the launch line below the pool + 512 MiB stops every rank before `shmem_init` with both sizes named (rc 8) — the launch-line rule is in §4. With `COMM_SHMEM_ROUND_MB=1024` (off by default; **on the target's launch line by the user's decision D2**, PLAN §36) the pool is **9472** MiB at the target (9.8 GB, node ≈ 424 GB modelled with the defaults; results/V114.md). *2026-09-27*: the same 9472 MiB with `ECALC_NP=4` (`plan pool`, login node); node 416.0 GB modelled. *2026-09-28 (the target 5.1 × 10¹³)*: **9472 MiB, unchanged** (`plan pool` with the launch line's environment, measured on the login node: staging 8192 MiB = 4 × 2048 MiB, control 894.2 MiB); node 455.4 GB modelled; without the rounds the pool is 48128 MiB and the node 496.0 GB (over 480). *DOC2, the Batch 2 launch line (`ECALC_NP=auto`, `RNS_DIST_CACHE_FIT=1`, `BS_ARENA_ROOM=0.16`)*: 9472 MiB (measured, login node); node 471.9 GB, 512.5 GB without the rounds (modelled) |
| `SHMEM_SYMMETRIC_HEAP_SIZE` | `COMM_SHMEM_POOL_MB` + 512 MiB (`mnrun.sh` sets it) | the library's heap must hold the pool; the name is OpenSHMEM's, Cray reads `XT_SYMMETRIC_HEAP_SIZE` too — set both |
| `COMM_SHMEM_FENCE=1` | set on a conforming implementation | orders the data before its signal with `shmem_ctx_fence` (one call) instead of `quiet`; OSHMEM 4.1.6's fence does not order nbi puts (trap 2) — verify with `t_comm` before switching |
| `COMM_SHMEM_RING_KB` | 256 (default) | the point-to-point ring per (source, dest); only small values flow through it |
| `COMM_SHMEM_TRACE=1` | debugging only | per-exchange trace on stderr |
| `MN_TOPO_GROUP` | **0** (off) until measured; then the machine's nodes per dragonfly group (64 if the cabling says so) | the third layer of the all-to-all (APU × node-in-group × group): fewer messages (1.28 M → 0.40 M per APU at 4 × 10¹⁰ × 576), twice the NIC bytes; the model says it does not pay at 2 µs per message and starts to at ≈ 15–20 µs. The tree's node groups must be contiguous in the node numbering for the groups to coincide with dragonfly groups: `--distribution=block` and a node list ordered by group |
| `MN_GROUPS` | `2,4,8,16,32,64,192,576` (the model's pick, results/Q.md §2) or `2,4,8,16,32,64,576` | the tree's level → group-size schedule (results/L.md); the code's default at 576 is `2,4,…,512,576` (a 512 + 64 join at the top). The three cost the same within the model's error (44–50 s of levels at 4 × 10¹⁰); the two explicit ones keep the six doublings inside a 64-node dragonfly group. **Check that the schedule is wired before relying on it** (§9: at 7aded87 `mn_groups_parse` is defined but `mn_tree` still walks binary levels — agent G's tree) |
| `MN_TOPO_TRACE=1` | debugging only | prints the in-group / cross-group mesh creation |

Layout and the distributed product (`rns_dist.c`, `ntt_dist.c`, `newton_db.c`):

| variable | target | why |
|---|---|---|
| `POOL_LOG` | 31 (default) | the 2³¹-point plane pools per APU (17 + 12 GiB); one process per node on the target — the aac6 values 27–29 are for several processes on one node |
| `DIST_CHUNKS` | 4 (default) | slabs in flight in the pipelined transform; the exchange of chunk k under the row pass of chunk k+1 |
| `DIST_STATS=1` | on for the calibration runs (§6), off after | per-part timing of the distributed transform: the exposed exchange time is the number that calibrates `--bw` |
| `DIST_LOGR_DELTA` | 0 (default) | the plane's R/C balance; with G's exact spills it no longer moves memory |
| `RNS_DIST_CACHE_MN` | 2 (default), **bounded by `RNS_DIST_CACHE_FIT=1`** | the transform cache over shares: two slots (B's piece across A's pieces, A's piece 0 across B's). *2026-09-28*: its default without FIT takes 16 GiB per slot per APU at the first mn grid product (tree level 1 at the target) whenever `hipMemGetInfo` shows it free — outside every budget: at the target ≈ 609 GB per node (modelled); on aac6 it cost +37.5 % at 10¹⁰ (memory pressure) or ran out of memory (CX15, TC15). **Never run the target without `RNS_DIST_CACHE_FIT=1` or `RNS_DIST_CACHE_MN=0`** (trap 15) |
| `RNS_DIST_CACHE_FIT` | **1 on the launch line** (the user's decision 2, 2026-09-28; not a code default) | the slots sized to the cap and bounded by the budget: at most (the rank's room under `ECALC_NODE_GB` − `RNS_DIST_CACHE_FIT_RESERVE_GB` 32) / one slot. At 5.1 × 10¹³: room 4.88 GB → **0 slots** (`plan cache`, measured on the login node); at 4.74 × 10¹³ 22.06 GB → 0; at §5's steps 3–5 both slots fit. **Measure on the target whether a slot fits** (TARGET_TASKS T7): the `transform cache:` line at init of the step-5 and step-6 runs |
| `RNS_DIST_CACHE_HOLD` | 0 (default) | X3 (holding Q's pieces from the reciprocal into the division): unmeasured, leave off |
| `NEWTON_MN_GROUPS` | 1 (default) | X1: the reciprocal's early doublings on the smallest prefix group by a cost rule |
| `NEWTON_MN_BW`, `NEWTON_MN_LAT`, `NEWTON_MN_FIXED` | 100, 2e-6, 0 (defaults = the target); set to the measured values after §6 | the constants of X1's rule (GB/s per APU, s per message, s per exchange); `MN_MODEL_TCP=1` is aac6's loopback set — never on the target |
| `NEWTON_MN_SPLIT` | 65536 (default) | the precision below which the reciprocal chain is replicated on every node |
| `DIST_GEN=1`, `DIST_LOGN_TEST` | tests only | force the general map at a power of two / lower the plane cap so grids form at small sizes |

The design table's axes (Phase 13b). Each switch lives on its agent's branch until merged; check it with the grep of §9
after the merge:

| variable | target | why |
|---|---|---|
| `ECALC_NP` | **`auto` on the target's launch line** (the user's decision 1 of 2026-09-28; it was 4 by the decision of 2026-09-27; the code's default stays 3 for decimal limbs) | three primes (−17 % wall, −25.8 GB of planes per node, RESULTS §78) cannot hold the target's mn pieces: a piece of pa + pb > 58 424 467 928 terms needs four, and at 4.25 × 10¹³ on 576 nodes 84 products exceed it from tree level 5 on (`MN_PLAN_ONLY` refuses `ECALC_NP=3`, rc 3: results/P15.md). Four primes: +46.8 s and +17.2 GB per node against three (modelled); four only where needed (option (b), −20 s) is for Batch 2. *2026-09-28*: at the target 5.1 × 10¹³ the plan check is OK with `ECALC_NP=4` (1240 products, the largest piece 1.096 × 10¹² limbs); `ECALC_NP=auto` (agent NP, `p15-NP`: the plan check also OK at 5.1 × 10¹³, measured by the integrator; 322.6 s against 344.7 s without the write, modelled) is **pending the user's Batch 2 decision**. *2026-09-28 (DOC2)*: **adopted for the launch line** — `plan check … ECALC_NP=auto: OK` (151 of 264 pieces over the bound at four primes; `plan primes` tree 76 of 108, recip 35 of 74, div 40 of 40), 400.0 s without the write and 471.9 GB per node against 423.3 s and **489.1 GB (over 480)** with `ECALC_NP=4` (modelled, with `BS_ARENA_ROOM=0.16`) |
| `NTT_MODMUL` | 1 (the default since step 0) | the reduced-correction Barrett: +5–12 % per transform, bit-identical |
| `RNS_STRATEGY` | the recommended row of `results/DESIGN_TABLE.md` (auto as of the M-run) | the single-node product's form: C four-step, B prime-per-APU, B4 over all four APUs, or auto. At 576 it acts on the leaf's top levels (agent B, p13b-B) |
| `ECALC_PLANE_CAP` | the recommended row (2^31 as of this writing) | the plane cap 2^30 / 3*2^29 / 2^31 / 3*2^30; it sets `POOL_LOG`, `RNS_PLANES_3Q30` and `DIST_LOGN_TEST`. `fit` takes the largest cap that fits (agent P, p13b-P) |
| `MDB_SHIFT_CHUNK_MB`, `MN_T_CHUNK_MB` | 1024 each in the recommended row (the defaults since Phase 13c / 14) | the sharded division's shift and the window temporary, in rounds: +1.4 × 10¹³ digits at 576, at one round's cost each (§6 item 4). *2026-09-27*: `MN_T_CHUNK_MB=1024` stays the default; **test 0 against 1024 on the target** once the per-round cost is measured (the user's decision 13; §6 item 4). *2026-09-28*: at 5.1 × 10¹³, 0 is modelled 19.6 s faster (325.1 s without the write) but the node is **490.5 GB, over 480** (fits 502 only) — keep 1024 at the target |
| `COMM_ALLTOALLV_DEPTH` | 2 in the recommended row | the uneven exchange (the 192- and 576-node levels, the machine-wide products) pipelined two deep (agent X, p13b-X) |

Memory and the single-node pipeline (`binsplit.c`, `rns_mul.c`, `ecalc.c`, `mem.c`):

| variable | target | why |
|---|---|---|
| `ECALC_TAIL` | 1 (default) | the arena with t₁'s quarter as its reserved tail: zero `hipMalloc` inside the phases (results/M11.md) |
| `BS_ARENA_ROOM` | **0.16 (the default since 2026-09-28**, the user's decision 3) | the arenas in whole VMM chunks + 0.16 × the division's largest block: the division without remaps (measured at 10¹¹ on one node: −14.7 s of division); **+16.5 GB of arena per node at 5.1 × 10¹³** (300.65 against 284.15 GB, `BS_LAYOUT_ONLY`, measured on the login node): node 471.9 GB modelled; `binsplit.c` drops the room itself when the node with it would pass `ECALC_NODE_GB` |
| `DIST_TWREC_G` | **1 (the default since 2026-09-28**, the user's decision 4) | the general map's (192, 576) twiddled packs by a row recurrence, bit-identical; −6.8 s at 5.1 × 10¹³ (modelled) |
| `RNS_POOL1_4Q` | 1 (default; the user's decision 5) | pool 1 at 4 q with four one-node primes; no effect under `ECALC_NP=auto` |
| `ECALC_DM_POOL` | unset (on by the size rule at ≥ 5 × 10¹⁰ per node; a no-op with the tail) | PLAN §27 row I deletes it; harmless either way |
| `RNS_PLANES_3Q30` | 0 (default) | the 3·2³⁰ planes: −3.6 s of phases for +5–6 s of mapping on aac6 (results/P.md); `auto` = on below 5 × 10¹⁰ if the target's mapping is cheaper (agent I) |
| `RNS_STRIPED_PAIR` | 1 (default) | the level-22 products paired (−0.55 s) |
| `RNS_BATCH_TILE_GB` | 15 (default) | the batch tier's tile budget |
| `ECALC_OVERLAP` | 1 (default) | the Phase 8 overlap (seeds during init, T1 during bs, the digits during the low product) |
| `ECALC_BG_THREADS`, `BS_SEED_THREADS` | 48 / default | the background OpenMP team; size to the node's cores (an MI300A node: 96 cores) |
| `ECALC_STAGING` | 1 (default) | the pinned staging sized to the seeds |
| `ECALC_ARENA_GB`, `ECALC_DM_POOL_K`, `ECALC_POOL_GROW_GB`, `BS_REGION_SLACK`, `BS_BALANCE_N`, `BS_MDEV_LOGL`, `BS_DEV_MDEV`, `BS_SEED_TERMS`, `BS_SEED_CHUNK_MB` | defaults | tuning knobs of the arena, the pool, the leaf layout; nothing on the target asks for them |
| `MN_OUT_CHUNK_MB` | 256 (default) | the writer's chunk per node; the part file streams during the low product |
| `MN_OUT_EARLY` | **1 (the default since 2026-09-27**, the user's decision 8) | at size > 1 the part file starts at the division's hook and streams during the low product as on one node (W5d; results/IO15.md); `=0` writes it after T1 (the comparison of §6 item 5(d)) |
| `ECALC_OUT_PACKED` | **1 (the default since 2026-09-27**, the user's decision 7) | the part file as base-10¹⁸ limbs, 0.444 B/digit (32.8 instead of 73.8 GB per node at 4.25 × 10¹³; **39.35 instead of 88.5 GB at 5.1 × 10¹³**); converted to ASCII **off the clock** by `tools/unpack_digits` (§4, after the launch line); `ecalc/digcmp.sh` compares packed or ASCII output with a reference |
| `ECALC_OUT_MODE`, `ECALC_ODIRECT` | `ECALC_ODIRECT=auto` (**the default since 2026-09-27**, the user's decision 9) | the write mode per file system: O_DIRECT on Lustre is **assumed** until §6 item 5(a) measures it; `ECALC_ODIRECT_LUSTRE=0` or `ECALC_OUT_MODE=sync` / `drop` if buffered writes are faster there (never plain `buffered`: the page cache is HBM) |
| `MN_OUT_STRIPE`, `MN_OUT_WAVES` | **no default (the user's decision 12): measure striping and waves on the target** (§6 item 5(b)/(c)) | a Lustre layout per part file; at most ⌈576/n⌉ nodes writing at once (waves are ignored with `MN_OUT_EARLY=1`) |
| `ECALC_MEM_GUARD_GB` | **6 on the launch line** (the user's decision 10, 2026-09-27; not a code default) | the sampler stops the run with rc 9 and one line naming rank, host and phase when MemAvailable falls below it — a named stop instead of the OOM killer |
| `MEM_REPORT_DEVS=1` | on for the first runs | the per-APU rows of the memory table every node prints (`mem[rank]`) |
| `ECALC_VERBOSE=2`, `RNS_VERBOSE=1`, `DB_POOL_VERBOSE=1`, `NEWTON_VERBOSE=1` | on for the smoke and calibration runs | per-level lines, per-call times, the pool's fallbacks and tail statistics, the reciprocal's steps |
| `MN_DM=host`, `MN_COMBINE=host` | never | the host-flow stand-ins (a cross-check on aac6; node 0's host cannot hold the target's numbers) |
| `LIMB_BASE` | 10 (default) | decimal limbs; `2` is the paper's binary pipeline (slower, more memory) |

Checkpoints and verification (`binsplit.c`, `mn.c`, `verify.c`, `mn_out.c`; README "Checkpoints per node"):

| variable | target | why |
|---|---|---|
| `BS_CKPT_DIR=<node-local dir>` | development steps only, not the headline (2026-09-27: record timing runs write no checkpoint sets); set (the node's NVMe; one directory per node or a shared one — the names carry the rank; *Phase 15*: the target has no node-local disk — a Lustre directory, 0.6–0.8 GB/s per node single-stream) | leaf sets `n<rank>_level_LLL.*` and tree sets `n<rank>_tree_LLL.*`; a leaf set is ≈ 35 GB / 4 × 10¹⁰ per node, a tree set the same; at ≈ 1 GB/s per node each set costs ≈ 30–60 s of the writer thread (hidden or not by the file system — measure at 10¹⁰, §5) |
| `BS_CKPT_MIN_LEVEL` | 16 (default), `BS_CKPT_EVERY` 4 | the leaf sets from the top levels only (a snapshot is a full pass of the pools) |
| `BS_CKPT_TREE` | 1 (default with `BS_CKPT_DIR`), `BS_CKPT_TREE_EVERY` 1 → **3** at 576 | a tree set after every level costs 10 writes; every third level plus the top (always written) is enough for a restart above the leaves |
| `BS_RESTART=1` | on a restart, with the same `BS_CKPT_DIR`, digits and base on every node | the nodes agree on the lowest complete tree level and resume above it, or each inside its leaf tree; bit-identical |
| `ECALC_CKPT_TOP` | **unset (off: the default) for the headline and every timed run** (the user's decision 11, 2026-09-27) | writes the top-level P, Q (≈ 35 GB per node at the target) so `ECALC_RECHECK` can run in its full form; without it RECHECK runs in its residue form, which checks the digits as fully (results/IO15.md W6); at size > 1 the top tree set is also written by `BS_CKPT_TREE` with `BS_CKPT_DIR` |
| `ECALC_CHECKPOINT=1` | **the development and bring-up steps** (§5 steps 1–5, 7), never the headline | the budgeted top set (`ECALC_CKPT_TOP=2`: dropped, never waited for, when the disk cannot finish it); `ECALC_CKPT_TOP` set overrides it |
| `ECALC_RECHECK=1` | after every large run, from the same launch line (`srun … ./ecalc <digits> <outfile>` with `ECALC_RECHECK=1`) | recomputes the digit residues from the part files (packed or ASCII), X mod q from them, P and Q mod q from the checkpointed top-level shares (or, without the top set, from `<outfile>.t1`: the residue form), the term recurrence and the T2 windows, and re-runs T1 with the run's residues: RECHECK OK / FAILED per node. No pools, no computation; ≈ 1 min per node packed (§7) |
| `ECALC_RES_LOG=1`, `ECALC_RES_LOG_LEVEL`, `ECALC_LEAF_DUMP=<dir>` | only when a VERIFY fails | the per-level residue log and the leaf dump of the first wrong node (results/V.md) |
| `MEM_DPOOL_FILL`, `RNS_POOL_GROW` | tests only | the zero-memory probe; the forced growth of the stress step (agent R) |
| `BS_CKPT_ABORT*` | tests only | die after a set is written |

## 4. The launch line

One process per node, four APUs per process (the process drives its APUs with four threads; no task per APU):

```
export COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1
export ECALC_NP=auto                          # the user's decision 1 of 2026-09-28 (was 4): four primes only for the products over the three-prime bound; never 3 (refused), and 4 is 489 GB per node (over 480)
export RNS_DIST_CACHE_FIT=1                   # the user's decision 2 of 2026-09-28: the mn transform cache bounded by the budget (0 slots at 5.1e13); NEVER without it (trap 15)
export MN_T_CHUNK_MB=1024                     # the default since Phase 14 (adopted 2026-09-26); shown for clarity (0 against 1024: §6 item 4)
export COMM_SHMEM_ROUND_MB=1024               # the user's decision D2 (PLAN §36, 2026-09-26): exchanges staged in rounds; pool 45.0 -> 9.8 GB
export COMM_SHMEM_POOL_MB=9472                # the measured law at 5.1e13 / 576 with all of the above (`plan pool`, 2026-09-28, with ECALC_NP=auto too; the same at 4.25e13)
export SHMEM_SYMMETRIC_HEAP_SIZE=9984M XT_SYMMETRIC_HEAP_SIZE=9984M  # the pool + 512 MiB
export MN_GROUPS=2,4,8,16,32,64,192,576 MN_TOPO_GROUP=0
export ECALC_MEM_GUARD_GB=6                   # the user's decision 10 (2026-09-27): a named stop (rc 9) instead of the OOM killer
export ECALC_VERBOSE=2 MEM_REPORT_DEVS=1
unset ECALC_CHECKPOINT ECALC_CKPT_TOP BS_CKPT_DIR   # the record run: no top set, no checkpoint sets (the user's decision 11)
OUT=/ssd0/<dir>/e51                           # Lustre (no /tmp: §6 item 10); its stripe layout from §6 item 5(c) if measured to help; 22.7 TB of parts
srun -N 576 --ntasks=576 --ntasks-per-node=1 --gpus-per-node=4 --distribution=block --export=ALL \
     bash -c 'export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; exec ./ecalc 51000000000000 '$OUT'/e.out'     # the target since 2026-09-27 23:50 EDT (was 42500000000000)
```

The run writes **packed part files** (`ECALC_OUT_PACKED=1`, the default since 2026-09-27): `$OUT/e.out.part0000` …
`e.out.part0575`, each a 4096-byte `ECPACK18` header (the part's limb and digit ranges and its digit residues) and the node's
base-10¹⁸ limbs, 39.35 GB per node, 22.67 TB in all (at 4.25 × 10¹³: 32.8 GB, 18.9 TB); `$OUT/e.out.t1` (node 0) holds the residues RECHECK needs. The wall of
the record is the run's (D3: `total` and the process's exit, without and with the write); what follows is **off the clock**.

**Off the clock: verify, convert, verify the converted output** (the user's decision 7):
1. **RECHECK the packed parts**, the same launch line with `ECALC_RECHECK=1` (no pools; the nodes in parallel): each node
   reads its 39.35 GB part (46–50 s at Lustre's 0.78–0.86 GB/s single-stream read, **modelled**; 38–42 s at 4.25 × 10¹³) and re-runs the residues, the
   windows and T1 in the residue form (no top set) → `RECHECK OK` on every node, `mn: all 576 nodes: RECHECK OK`.
2. **Convert to ASCII** with `tools/unpack_digits` (built by `make` in `ecalc/`). Each part's formatted digits are checked
   mod the eight T1 primes against the residues its header stores, as it converts. Time per part (**modelled** from
   results/IO15.md §2.2: 100 GB of ASCII written in 179 s, 0.56 GB/s, on aac6's NVMe; the read at 1.66 GB/s and the
   formatting, 2.6 s per 10¹¹ digits, hide under the write): 88.5 GB of ASCII per node at 0.6–0.8 GB/s = **≈ 1.8–2.5 min
   per node (111–148 s), the nodes in parallel** (2026-09-28, at 5.1 × 10¹³; 1.5–2.1 min at 4.25 × 10¹³) — if the file system's
   aggregate carries 576 writers (§6 item 5(b)); 51.0 TB at the aggregate otherwise. The ASCII parts need 51.0 TB of the 122 TB
   beside the 22.7 TB of packed parts: **keep the parts, do not concatenate them into one file on the same file system** (trap 14). *Phase 15 IO2 (eb3b29f)*: the tool converts any consecutive subset of parts, so every node converts
   its own part (`e.txt.part<k>` from `e.out.part<k>`: "2." on part 0000, the newline on the last; the concatenation is the
   single file; measured on aac6 at 1e9 sizes 2 and 4, 1e10 size 2, results/IO215.md):
   `srun -N 576 --ntasks-per-node=1 bash -c 'k=$(printf %04d $SLURM_PROCID); exec tools/unpack_digits -q -o '$OUT'/e.txt.part$k '$OUT'/e.out.part$k'`.
3. **Verify the converted output**: (a) every conversion exits 0 (its residue check); (b) `ECALC_RECHECK=1` on the ASCII
   parts (`<outfile>` = `$OUT/e.txt`, the same launch line: 88.5 GB read per node, 103–114 s, modelled; 73.8 GB, 86–95 s at 4.25 × 10¹³) → `RECHECK OK` on every
   node; (c) the leading 10¹¹ digits against the 10¹¹ reference (`results/e_1e11.out`, sha1 578f5efb…): `cat
   $OUT/e.txt.part0000 $OUT/e.txt.part0001 | head -c 100000000002 | cmp - <(head -c 100000000002 e_1e11.out)` — or before
   converting, from the packed parts with the tool as it is: `tools/unpack_digits -q $OUT/e.out.part* | head -c 100000000002 |
   cmp - <(head -c 100000000002 e_1e11.out)` (it stops after ≈ 1.4 parts). Keep the packed parts until (a)–(c) pass.

**The pool and the heap (Phase 14 V1).** The pool is carved from the SHMEM library's heap unless the heap is SOS's external
heap (`COMM_SHMEM_DEVHEAP=1` on SOS with the patch: a HIP buffer of exactly the pool). So, on the target:
1. With the exact environment of the run, on the login node (no device is touched, < 0.1 s):
   `MN_PLAN_ONLY=51000000000000:576 ./ecalc | grep 'plan pool'` — it prints `COMM_SHMEM_POOL_MB=<need>` and the heap
   (`>= <need + 512> MiB`). Every switch that shapes the exchanges must be the run's (`MN_GROUPS`, `MN_T_CHUNK_MB`,
   `MDB_SHIFT_CHUNK_MB`, `COMM_SHMEM_ROUND_MB`, `COMM_SHMEM_RING_KB`, `ECALC_PLANE_CAP` / `POOL_LOG`).
2. Export `COMM_SHMEM_POOL_MB=<need>` and, **unless** the heap is the SOS external heap, the library's heap variable at
   `<need + 512>M` — `SHMEM_SYMMETRIC_SIZE` (SOS, Cray; Cray also `XT_SYMMETRIC_HEAP_SIZE`), `SHMEM_SYMMETRIC_HEAP_SIZE`
   (OSHMEM); a device heap that the library sizes from such a variable (Cray on the APU, rocSHMEM's own variable) is a
   heap too: set it the same way.
3. `COMM_SHMEM_POOL_AUTO=1` (the default) then finds the pool at the need; if the pool had to grow past the heap it
   stops before `shmem_init` on every rank: `comm_shmem pool: the SHMEM heap SHMEM_SYMMETRIC_SIZE=… MiB cannot hold the
   symmetric pool COMM_SHMEM_POOL_MB=… MiB (+ 512 MiB; the modelled need …)` (rc 8) — fix the launch line, nothing ran.
   A heap the transport cannot see (a variable other than the two above) is not checked: `shmem_malloc`'s failure then names
   the size at init.
aac6's `mnrun.sh` does steps 1–2 itself when `COMM_SHMEM_POOL_MB` is not set (the command's SHMEM-linked executable and its
digit count; `MNRUN_PLAN_POOL=0` skips it). The launch line above has the values of step 1 for the defaults.

The argument is the **total** digit count, not the per-node share (the pre-13c text had 61000000000, the per-node
share of the old safe size, which would have run 6.1 × 10¹⁰ digits in all). **5.1 × 10¹³ is the target since the user's decision of 2026-09-27, 23:50 EDT** (§1, §5 step 6): the last size at 222 / 242 pieces before the grid steps at 5.11 (node 0) and 5.12 × 10¹³ (the critical path); the runtime one step below, 4.74 × 10¹³ (below the step at 4.75), is tested after the headline (§5 step 6c). *History*: 4.25 × 10¹³ was the target from Phase 13d (RESULTS §82) to 2026-09-27: the last flat stretch below the grid steps at 4.29 → 4.30 and 4.39 → 4.40 × 10¹³.
The defaults (Phase 13c, extended in Phase 14: `RNS_PLANES_FIRST`, `NEWTON_RECIP_CUT`, `ECALC_ODIRECT`, `DM_TIGHT` + `DB_POOL_VMM`, `MN_TREE_EARLY_FREE`, `MN_T_CHUNK_MB=1024`, `ECALC_BUDGET_CHECK`; RESULTS §85) are the chosen design — `RNS_STRATEGY=auto`, `ECALC_PLANE_CAP=2^31`, `MDB_SHIFT_CHUNK_MB=1024`,
`COMM_ALLTOALLV_DEPTH=2`, with K's kernels `NTT_B1R=3 NTT_PLAN=1` (RESULTS §80) — so none of them needs setting; set one only to leave the design.
**The user's decisions of 2026-09-27** added to the defaults (nothing to set): `RNS_AUTO_PIECE_COST=1` (D1), `ECALC_CORR_PATCH=2`,
`NEWTON_RECIP_MID=1`, `BS_SEED_FILL=128`, `BI_MUL1_FAST=1`, `DIST_TWREC=1`, `ECALC_OUT_PACKED=1`, `MN_OUT_EARLY=1`,
`ECALC_ODIRECT=auto`; the top set off (`ECALC_CHECKPOINT=1` for development). **The user's decisions of 2026-09-28** (main B2) added
`BS_ARENA_ROOM=0.16`, `DIST_TWREC_G=1` and keep `RNS_POOL1_4Q=1` (nothing to set). On the launch line, not defaults:
**`ECALC_NP=auto`** (was `ECALC_NP=4`), **`RNS_DIST_CACHE_FIT=1`**, `ECALC_MEM_GUARD_GB=6`, `COMM_SHMEM_ROUND_MB=1024`. Off and not set:
`NTT_R3_FUSE`, `RNS_R3_MINK` (the user's decision 6). Not set on the target: `MN_OUT_STRIPE` and `MN_OUT_WAVES` until measured (§6
item 5), `DB_POOL_VMM_PAR` / `DB_POOL_VMM_EXTEND` (not merged); E11 / `DM_BAND` and MAP's `DB_POOL_VMM_STREAM` were dropped (not in
the code).

`COMM_RANK`/`COMM_SIZE` are what `mn_init` reads for the rank and the size (under SHMEM the PE number is checked
against them); `--mpi=pmix` where the SHMEM library is launched by PMIx (OSHMEM; Cray SHMEM uses the ALPS/PMI of
`srun` directly). Per node count: `-N g --ntasks=g` with the same line — the tree's schedule and every group are
derived from `COMM_SIZE`; `MN_GROUPS` must end at or above g (anything ≥ g ends the list at g). For small g the
schedule is `2,4,…` (the default) — set `MN_GROUPS` only at 576 (or a multiple of 9 · 2ᵏ). aac6's `mnrun.sh`
does the same for several processes per node (it adds `setarch x86_64 -L`, OSHMEM's MCA variables and the heap size;
none of that is needed with a proper SHMEM, trap 1).

Sanity of the transport first, on 2 and then 8 nodes: `srun -N2 -n2 ./tests/t_comm` (all-to-all 1 B – 3 MiB,
all-gathers, alltoallv, max, sum mod q, point-to-point, the strided PE sets: VERIFY OK on every PE), then
`srun -N4 -n4 env DIST_LAYERED=1 ./tests/t_dist 24` (the layered all-to-all over 4 nodes × 4 APUs against the
one-rank engine) and `srun -N9 -n9 ./tests/t_mn_grid 0.5 27` (the any-size map at 9 nodes: 200 checks per node).

## 5. The sizes to run, in order

Every run: `ecalc/digcmp.sh <outfile> <reference>` where a reference exists (`ref/e_10^8`, `ref/e_10^9` in the
tree; 10¹⁰ and 4 × 10¹⁰ from a single-node run of the same digits, which is bit-identical to any size — convert its packed
file with `tools/unpack_digits -o`, or run it with `ECALC_OUT_PACKED=0`; `results/e_1e11.out` for 10¹¹) — it compares the packed
parts through `unpack_digits --cmp` without an ASCII copy and prints `identical`; else the `VERIFY OK` on every node and node 0's
`mn: all n nodes: VERIFY OK`, then `ECALC_RECHECK=1` (§3). *2026-09-27*: **the development and bring-up steps (1–5, 7) run with
`ECALC_CHECKPOINT=1`** (the budgeted top set: RECHECK in its full form, a restart point for the division); **the headline (6)
does not** (a record timing run: no top set, no `BS_CKPT_DIR`). Every step reports two walls (D3). The estimates are
`estimate.py` on the launch line of §4 (*2026-09-28*: `ECALC_NP=auto`, `RNS_DIST_CACHE_FIT=1` with the slots it allows, main B2's defaults; packed, the early writer; **modelled**, the part file at 0.6 GB/s).

| step | nodes | digits (total) | per node | expect | what it checks |
|---|---|---|---|---|---|
| 1 smoke | 2, 3, 4, 9 | 10⁸ | 2.5–5 × 10⁷ | ≈ 10 s each, identical to `ref/e_100000000.txt` | the transport, the general map (3, 9), the part files |
| 2 | 2, 4, 64 | 10⁹ | 1.6 × 10⁷ – 5 × 10⁸ | ≈ 15–20 s, identical to `ref/e_1000000000.txt` | the pipelined exchange over real NICs (2, 4), the dragonfly group (64), `DIST_STATS=1` on: **the first calibration number** (§6) |
| 3 | 64 | 6.4 × 10¹¹ | 10¹⁰ | ≈ 36 s without the write, 41 s with it (36.0 / 40.6 s, modelled on the launch line of 2026-09-28; FIT allows both cache slots here; 40 / 44 s on 2026-09-27, ≈ 1.3 min on the Phase 13 model); `digcmp.sh` against a single-node 10¹⁰ run | the tree at 6 levels, the checkpoints' cost (`ECALC_CHECKPOINT=1`, `BS_CKPT_DIR` on), the recheck; §6 items 4 (`MN_T_CHUNK_MB` 0 vs 1024) and 5 (b)–(d) (the aggregate write, stripes, waves, `MN_OUT_EARLY=0` vs 1) |
| 4 | 576 | 10¹² | 1.7 × 10⁹ | ≈ 17 s without the write, 18 s with it (17.0 / 17.8 s, modelled 2026-09-28); VERIFY OK everywhere | the whole machine at a size where everything is small: the 9-way / 3·3 level, the PE sets at 576, the collectives. Run it with each `MN_GROUPS` of §3 and keep the faster |
| 5 | 576 | 2.2 × 10¹³ | 3.8 × 10¹⁰ | **≈ 2.0 min without the write, 2.3 min with it** (121.5 / 139.4 s modelled on the launch line of 2026-09-28, with the 2 cache slots FIT allows here: room 142 GB; 131.6 / 149.7 s on 2026-09-27; ≈ 1.8 min on three primes before) | the first large run (334.5 GB per node without the cache, 471.9 with its 2 slots, modelled). **Read the `transform cache:` line**: the slots FIT took and its room (T7). The recheck after it; the off-the-clock conversion of §4 rehearsed here (0.43 of the 5.1 × 10¹³ target's bytes; 0.52 of 4.25 × 10¹³'s) |
| 6 **the target** | 576 | **5.1 × 10¹³** (*2026-09-28*: the user's decision of 2026-09-27, 23:50 EDT; was 4.25 × 10¹³) | **8.854 × 10¹⁰** (average; the top node **9.169 × 10¹⁰** = 1.0356 ×) | the launch line of §4 (*2026-09-28*: `ECALC_NP=auto`, `RNS_DIST_CACHE_FIT=1` — 0 cache slots, packed, the early writer; §1): **400.0 s (6.67 min) without the write; 420.5 s (7.01 min) with it at 0.6 GB/s, 404.1 s at 0.8, 394.6 s at 2.0**; **471.9 GB per node** (modelled; the fabric and the write rate with 576 writers assumed). *History*: 344.7 / 376.2 s, 455.4 GB (TGT, 2026-09-28: `ECALC_NP=4` and two cache slots that do not fit); `ECALC_NP=auto` then 322.6 / 354.1 s (modelled, the same slots). 4.25 × 10¹³ (the target from Phase 13d to 2026-09-27): 256.0 s / 285.7 s at 0.6 GB/s, 416.0 GB (2026-09-27); 3.9 min / 452 GB (Phase 13d, the flat 8 GiB pool), 525 / 460 GB with the measured pool (Phase 14 P2), 231.1 s / 398.8 GB on three primes (2026-09-26, not runnable) | the headline run: **242 pieces** on the critical path (128 + 74 + 40; node 0's 222), the last size before the step (5.11 × 10¹³: node 0 → 226; 5.12 × 10¹³: the critical path → 246, +7.1 s modelled; **0 margin** at the 0.01 × 10¹³ resolution: run exactly `51000000000000`). **No `ECALC_CHECKPOINT`, no top set.** Check before the run, with the launch line's environment (`ECALC_NP=auto RNS_DIST_CACHE_FIT=1 COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576`): `MN_PLAN_ONLY=51000000000000:576 ./ecalc` — the last lines must read **`plan check 5.1e+13 digits g 576, ECALC_NP=auto: OK -- 1240 products`** (the largest piece 1.096 × 10¹² limbs at dist_mn level 8; 151 of 264 pieces over the bound; with three primes `plan REFUSED …`, rc 3), **`plan primes … pieces at four primes tree 76 of 108, recip 35 of 74, div 40 of 40 | leaf dist_db 0 of 12, recip single-node chain 0 of 30`**, **`plan cache … RNS_DIST_CACHE_FIT=1 … -> 0 slots`** (room 4.88 GB; `RNS_DIST_CACHE_FIT=0` here means the launch line is wrong), `plan summary` **222 / 242** pieces, `plan pool` 9472 MiB (all measured on the login node at be2eec3, results/DOC215/plan_51e13_auto.txt). *History*: at 4.25 × 10¹³, 182 pieces (88 + 66 + 28), 1.2 % below the step at 4.29 → 4.30 × 10¹³; the top node 7.64 × 10¹⁰ (1.036 × 7.38 × 10¹⁰) |
| 6b off the clock | 576 | — | — | RECHECK ≈ 46–50 s per node (39.35 GB read); the conversion **≈ 1.8–2.5 min per node** in parallel (88.5 GB of ASCII written at 0.8–0.6 GB/s; one part per node: Phase 15 IO2); the ASCII RECHECK ≈ 1.7–1.9 min per node (modelled at 5.1 × 10¹³; at 4.25 × 10¹³: 1, 1.5–2.1, 1.5 min). Disk: 22.7 TB packed + 51.0 TB ASCII = 73.7 TB of the 122 TB — do not concatenate (trap 14) | §4 "Off the clock": RECHECK of the packed parts, `tools/unpack_digits`, the converted output verified (its residue check, RECHECK on the ASCII parts, the leading 10¹¹ digits against `e_1e11.out`) |
| 6c **the runtime one step below** (after the headline, the user's request of 2026-09-27) | 576 | **4.74 × 10¹³** | 8.229 × 10¹⁰ (the top node 8.523 × 10¹⁰) | **368.7 s (6.14 min) without the write; 390.7 s (6.51 min) at 0.6 GB/s, 375.4 s at 0.8, 363.7 s at 2.0; 454.8 GB per node** (modelled 2026-09-28, the launch line with FIT: 0 slots; C layout node 425.94 GB, measured; *history*: 320.3 / 351.5 s, 439.2 GB with `ECALC_NP=4` and two slots) | the same launch line with `ecalc 47400000000000` (the same pool, 9472 MiB): `MN_PLAN_ONLY=47400000000000:576` → `plan check … ECALC_NP=auto: OK` (131 of 244 pieces over the bound), `plan primes` tree 68 of 100, recip 29 of 68, div 34 of 34, `plan cache … -> 0 slots` (room 22.06 GB), `plan summary` **202 / 226** (124 + 68 + 34); the last size below the step at 4.75 × 10¹³ (209 / 233, +11.8 s modelled). It measures what one grid step costs on the machine (242 − 226 = 16 pieces on the critical path; modelled 31.3 s for 7.6 % of the digits). No top set; RECHECK after it; its 21.1 TB of packed parts can be deleted after RECHECK OK |
| 7 the 480 GB ceiling | 576 | **5.167 × 10¹³** (*2026-09-28, DOC2*: the launch line with `ECALC_NP=auto` and the arena room; 5.57 × 10¹³ on 2026-09-27 with four primes and no room; 4.66 × 10¹³ on the Phase 13d model) | 8.97 × 10¹⁰ | 435.7 s (7.3 min) without the write, 457.0 s (7.6 min) with it at 0.6 GB/s (modelled), 471.9 GB | **not recommended**: past the step at 5.12 × 10¹³ and at 5.17, +8.9 % time for +1.3 % digits against 5.1 × 10¹³ (modelled; the arena room's +16.5 GB leaves the target only 1.3 % below the memory ceiling); only if the digits themselves matter, and only after step 6's `mem[rank]` tables agree with the model on every node |

Between 6 and 7, `estimate.py --g 576 --D <D>` (the launch line's design by default since 2026-09-27) gives the peak per D in 10⁹ steps; take the largest whose modelled
peak stays below 502 GB minus the measured error of step 6. Evict the reference file from the page cache before a
timed run (`posix_fadvise DONTNEED`, RESULTS §68) if the reference lives on the node; the target's part files go to
the parallel file system (`/out`), the checkpoints to node-local disk. *Phase 15 IO*: the target node has no node-local
disk: `/ssd0` is Lustre (shared, 0.6–0.8 GB/s single-stream, measured by the catalog), so part files and checkpoints both
go there, and **nothing large goes to `/tmp`** (§6 item 10).

## 6. What to measure first, and how to feed it into the model

Phase 13b: the items below come in the order in which the design table's assumed inputs matter. Each item names the run
that measures it, the line to read, and the option that feeds it. **After items 1 and 2, regenerate the table** and run
the recommended row's environment from `results/DESIGN_TABLE.md`, not a fixed one:

```
cd ecalc && ./design_table.py --bws <bw/2>,<bw>,<2 bw> --lat <s> --write-bw <GB/s> [--hide-pow2 <f> --gen-hide2 <f>]
```

It takes two minutes on a login node. If a one-node run was taken on the target, run `--calibrate` first.

1. **The injection bandwidth per APU** (assumed 100 GB/s; the table's (f) columns bracket it at 50 and 200). Step 2 at
   2 and 4 nodes with `DIST_STATS=1` prints, per part of the distributed transform, the exchange time and the exposed
   part. A 2³¹-point piece over 2 nodes sends 8 × 2²⁸ B = 2.1 GB per APU per transform exchange, so the `exchange` seconds
   give the GB/s per APU. Feed: `design_table.py --bws`, `estimate.py --bw`, and `NEWTON_MN_BW=<GB/s>` in the
   environment (X1's rule).
2. **The overlap of the two fabrics.** On aac6 this was measured over loopback only: the equal-slab path hides 0.75 of
   its xGMI time and the general map 0.011 at depth 1; depth 2 is modelled at 0.75. Measure it with
   `COMM_LAYER_STATS=1 COMM_XGMI_STATS=1` on the step-2 runs at 2 nodes (the equal path) and at 3 nodes (the general map),
   at both depths (`COMM_ALLTOALLV_DEPTH=1|2`), and read the "xGMI link time hidden under the fabric" line. Feed:
   `design_table.py --hide-pow2 <f> --gen-hide2 <f>` (or `HIDE_POW2`, `GEN_HIDE_DEPTH` in `mn_model.py`). This decides
   the depth axis.
3. **The per-message cost** (assumed 2 µs). `t_comm` prints the all-to-all times at 1 B … 3 MiB over 2–8 PEs; the
   1 B row over 8 PEs divided by 7 is the per-message cost of a put + signal from a host thread. Feed:
   `design_table.py --lat <s>`, `estimate.py --lat <s>`, `NEWTON_MN_LAT=<s>`. At 20 µs the 576-node wall rises 12 %, and
   the third layer (`MN_TOPO_GROUP`) is then worth one measurement at step 4, both ways (`MN_TOPO_GROUP=0` and `=64`).
4. **The cost of one chunk round** (`T_ROUND`: fitted on aac6 loopback at 0.03 s ± 100 %; it moves the chunked rows by
   up to 0.8 min at 576). Run step 3 (64 nodes, 10¹⁰ per node) three times: no chunking, `MDB_SHIFT_CHUNK_MB=1024`, and
   both switches at 1024. The difference in `dm`, over the extra rounds the model counts, is the cost. Feed: the three
   runs as an M-run-format log (`design_table.py --mrun` refits `T_ROUND` from any runs of one size that differ only in
   chunking), or `T_ROUND` in `mn_model.py`.
   **Then `MN_T_CHUNK_MB` 0 against 1024** (the user's decision 13, 2026-09-27: 1024 stays the default until this is
   measured): with the per-round cost known, `estimate.py --chunk shift` (= `MN_T_CHUNK_MB=0`) against the default at the
   target's size, and `mem_model.py --p15` for the node (0 costs +28.5 GB per node at 4.25 × 10¹³, 444.5 GB against 416.0,
   modelled); if the model says 0 pays and fits, confirm it at step 5 with both values, the same nodes, and keep the faster.
   *2026-09-28, at the target 5.1 × 10¹³*: 0 costs +35.1 GB, **490.5 GB against 455.4 — over 480** (fits 502 only), for −19.6 s
   without the write (325.1 against 344.7 s, modelled; the design table's fastest row, which cannot hold 5.1 × 10¹³ at 480 GB).
   At this target the test is worth running only if the target's measured memory edge (item 9) is above ≈ 500 GB.
   *2026-09-28 (DOC2), on the Batch 2 launch line (`ECALC_NP=auto`, FIT: 0 cache slots, `BS_ARENA_ROOM=0.16`)*: 0 is −19.9 s (380.1
   against 400.0 s without the write) and **497.7 GB against 471.9 — over 480** (modelled); the same conclusion.
5. **The part-file bandwidth** (assumed 2 GB/s per node until Phase 15; *Phase 15 IO, 2026-09-26*: **the prior is now
   0.6–0.8 GB/s single-stream** — the target node's `/ssd0` is **Lustre over Slingshot, 122 TB shared**, measured there at
   0.58–0.64 GB/s single-stream write and 0.78–0.86 GB/s read (`apucode/apumult-ntt-reverse-port-catalog.md`), not
   node-local NVMe). At 0.6–0.8 GB/s the 576-node wall grows 260 → 316–347 s (modelled, `mn_model.py --D 7.38e10 --g 576
   --write-bw <x>`); with the packed part file (`ECALC_OUT_PACKED=1`, 0.444 B/digit: 32.8 instead of 73.8 GB per node) the
   same disk is worth 2.25× the rate, 264–278 s (modelled on three primes, 2026-09-26). *2026-09-27*: packed and early are the
   defaults and the target runs on four primes: 272.0 / 285.7 s at 0.8 / 0.6 GB/s against 256.0 s without the write (§1;
   `estimate.py --target`, which takes the packed bytes itself: pass the measured GB/s as it is). *2026-09-28, the target 5.1 ×
   10¹³*: 39.35 GB per node packed; 359.8 / 376.2 s at 0.8 / 0.6 GB/s against 344.7 s without the write (§1). *DOC2, the Batch 2 launch
   line*: 404.1 / 420.5 s at 0.8 / 0.6 GB/s against 400.0 s without the write (§1). Measure, in this order:
   - **(a) one node, one stream**: `dd if=/dev/zero of=<dir>/dd.bin bs=64M count=64 oflag=direct` and the same with
     `conv=fsync` (buffered), then a 10¹⁰ one-node run into the same directory with `ECALC_VERBOSE=2`: the `wrote … (x GB;
     write y s in the writer thread …)` line gives the writer's GB/s. Repeat with `ECALC_OUT_MODE=direct|sync|drop` and
     `MN_OUT_THREADS=8|32` (results/IO15.md W1: on aac6's NFS O_DIRECT was the fastest form, 1.5–2× buffered + fsync at 10 GbE, and threads and chunk did not matter; on Lustre it is **unmeasured**; `tools/wbench` repeats the writer's pattern without a GPU);
   - **(b) 64 nodes writing at once** (step 3 with the digit file): each node's `wrote` line and `dc`. **Waves: the user's
     decision 12 (2026-09-27) sets no default — measure `MN_OUT_WAVES` here** (e.g. unset, 2, 4 at 64 nodes, with
     `MN_OUT_EARLY=0`, which waves need) and again at 576 at step 4 or 5 if the aggregate saturates. The aggregate
     (64 × the per-node rate) against (a) says whether the file system, not the node, is the limit; the 576-node write is
     then 51.0 TB (ASCII) or 22.7 TB (packed) at the 5.1 × 10¹³ target (42.5 / 18.9 TB at 4.25 × 10¹³) at the **aggregate** rate, which at 576 nodes will be far below
     576 × 0.6 GB/s = 346 GB/s if the file system has few storage targets;
   - **(c) the storage targets**: `lfs df -h <dir>` (the number of OSTs and their sizes), `lfs getstripe -d <dir>` (the
     default stripe count and size of the output directory). With few OSTs and stripe count 1, 576 files land on the same
     few targets: set a layout per part file (`MN_OUT_STRIPE=<count>:<MB>:<OSTs>`, or `lfs setstripe -c <count> -S <MB>M
     <dir>` on the directory before the run) and, if (b) shows the aggregate saturating, cap the concurrent writers with
     `MN_OUT_WAVES=<n>` (at most ⌈576/n⌉ nodes write at once; the others hold their digits on the device). **Striping: no
     default either (the user's decision 12) — measure `MN_OUT_STRIPE` (or the directory's `lfs setstripe`) at 64 nodes
     against the default layout**, and keep what the aggregate says;
   - **(d) the exposed write**: with `MN_OUT_EARLY=1` (the default since 2026-09-27) the part file streams during the
     division's low product, as on one node (Phase 15 IO W5d; with `=0`, at size > 1 the whole write comes after T1 and is
     exposed: MD15). Compare `total`, the division's `X Q` line and the `wrote` line with `=1` and `=0` at step 3: on aac6 the
     writer and the division's exchange shared one link and the low product slowed by 12.9 s (results/IO15.md §2.6).
   Feed: `--write-bw` = min(the per-node rate of (a), the aggregate of (b) / nodes), divided by 0.444 for a packed run.
   Report both walls (PLAN §36.2 D3): without the write and with it.
6. **The mapping rate of device memory** (measured 0.057–0.072 s/GB on aac6; `MAP_RATE` 0.065 in `mn_model.py`). It
   prices the planes of every row. Read init's `pools … s` line. Feed: `MAP_RATE`.
7. **The checkpoint bandwidth** (assumed 1 GB/s per node to local disk; *Phase 15 IO*: there is no node-local disk — the
   checkpoints go to the same Lustre as the part files, 0.6–0.8 GB/s single-stream at best). The `mn: node r: checkpoint
   tree level l` lines print GB and GB/s. `BS_CKPT_TREE_EVERY` and `BS_CKPT_MIN_LEVEL` are the knobs if it does not hide.
   The top set (`ECALC_CKPT_TOP`, ≈ 35 GB per node at 4.25 × 10¹³, ≈ 42 GB at 5.1 × 10¹³ by scaling (modelled), written during the division) competes with the part
   file for the same bandwidth: 58 s at 0.6 GB/s (modelled). *2026-09-27*: it is off by default and off for the headline
   (the user's decision 11); the development steps take it with `ECALC_CHECKPOINT=1`, which is `ECALC_CKPT_TOP=2`: it drops
   the set when the disk cannot finish it in time; RECHECK then runs in its residue form (results/IO15.md W6: the digits are checked as fully; only the stored P, Q
   limbs are not re-read). `ECALC_ODIRECT=auto` picks O_DIRECT by file-system type (Lustre: O_DIRECT, **assumed**; check
   with (a) above whether buffered + `fsync` is faster there and set `ECALC_ODIRECT_LUSTRE=0` if it is).
10. **`/tmp` on the compute node** (Phase 15 IO): `df -h /tmp; findmnt -T /tmp`. If it is `tmpfs` it is memory — on the
   APU the same HBM the run needs: **no digit file, checkpoint or reference goes to `/tmp` on the target**; the part files
   and `BS_CKPT_DIR` go to the Lustre directory (with its stripe layout, (c) above).
8. **The global-link taper** (assumed 1.0). Compare the 64-node run (step 2/3) with the 576-node run at the same D per
   node (step 4 with D = 10¹⁰: `estimate.py --g 64 --D 1e10` against `--g 576 --D 1e10`). The levels above 64 are the
   only difference; if their exposed time exceeds the model's, `--taper` moves it.
9. **The node's memory edge** (502 GB assumed; 524 GB measured on aac6, P13b). Run one node at a size whose modelled peak is
   515–525 GB (`estimate.py --g 1 --D ...`) and watch for the allocation failure or the OOM kill. Feed: the budget in
   `design_table.py` (`EDGE_GB`) and in `estimate.py --max`.

To rebuild the M-run log from a campaign's tagged per-run logs (tags `s1_<strategy>_<cap>_r<n>`, `koff_*`, `series_*`,
`p4_d<depth>_<off|shift|both>_r<n>`, `p3_d<depth>_r<n>`; verdicts in `progress.txt` beside the log directory):
`./design_table.py --regen <logdir> [<progress.txt>] > mrun.log`, then `./design_table.py --calibrate --mrun mrun.log` and
`./design_table.py --mrun mrun.log`.

A one-node run on the target is worth taking before step 5: 4 × 10¹⁰, the recommended row's environment,
`MEM_REPORT_DEVS=1`. Write it as an M-run line:

```
digits=40000000000 size=1 <env> | <the total line> | device <the mem init device total> GB
```

Then `design_table.py --calibrate --mrun` compares it with the model, and `design_table.py --mrun` scales the row's
per-node compute to it.

After the first 576-node run, compare:
- the per-node `mem[rank]` tables with `estimate.py --verbose` (planes, arena, top scratch, exchange, host);
- the levels' times with the per-level lines (`mn: node 0 level l [g0, g1): … in s`);
- the reciprocal's `dist_mn` lines (pieces per product) with the model's piece counts.

The model's constants are all at the top of `mn_model.py` (the Phase 13b block: `T_PIECE_31_NP`, `HIDE_POW2`,
`GEN_HIDE_DEPTH`, `F_MM1`, `MAP_RATE`, `T_ROUND`) and of `mem_model.py`, each with the RESULTS section or result file
it comes from.

## 7. Checkpoints and restart in practice

- `BS_CKPT_DIR` on every node; the leaf sets from level 16 every 4 levels; the tree sets every third level and at
  the top. A 6.1 × 10¹⁰-per-node run writes ≈ 55 GB per node per set: at 1 GB/s ≈ 1 min of writer per set, hidden
  behind the next level's compute if local disk keeps up — watch the `checkpoint … GB/s` lines at step 3.
- A failed run: relaunch the same line with `BS_RESTART=1`; the nodes agree on the lowest complete tree level (each
  node removes its older sets only after every node has the newer one), and resume above it; a node with no set
  recomputes its leaf tree. The digits are bit-identical.
- The recheck (`ECALC_RECHECK=1`, same line, same `BS_CKPT_DIR`, same `<outfile>`) needs the top-level tree set and
  `<outfile>.t1` — keep both until the recheck says OK on every node. *Phase 15 IO (W6)*: without the top set it runs in
  its residue form (P, Q mod q from `<outfile>.t1`, still checked against the recomputed term recurrence); only `.t1` and the
  part files are needed then.
- *2026-09-28, the target 5.1 × 10¹³* (the same rates, modelled): packed 39.35 GB per node, 46–50 s of reading; ASCII 88.5 GB,
  103–114 s; the conversion 111–148 s per node at 0.8–0.6 GB/s; 22.7 TB packed, 51.0 TB ASCII in all. The figures of the next
  bullet are for 4.25 × 10¹³ (history).
- **Read-back times at the target** (Phase 15 IO, W9; modelled from the measured Lustre read rate 0.78–0.86 GB/s
  single-stream): RECHECK reads each node's part file once (73.8 GB ASCII: 86–95 s; packed 32.8 GB: 38–42 s) and, with the
  top set, its 35 GB (41–45 s): **≈ 2–2.5 min per node for ASCII, ≈ 1.5 min packed, all nodes in parallel** — if the file
  system's aggregate read rate carries 576 streams; else 42.5 TB (ASCII) / 18.9 TB (packed) at the aggregate. A compare
  against a reference reads two files (twice that). Converting a packed file to ASCII (`tools/unpack_digits`) reads
  0.444 B and writes 1 B per digit: at Lustre rates the write dominates (73.8 GB at 0.6 GB/s ≈ 2 min per part, on the nodes
  in parallel); to check without converting, `tools/unpack_digits --cmp <reference> <parts…>`, or `unpack_digits <parts…> |
  sha1sum`. *2026-09-27*: the headline writes no top set, so its RECHECK is the residue form: ≈ 40 s of reading per node
  packed.
- **The ASCII file after a packed run, off the clock (Phase 15 IO2)**: convert **on every node, its own part, in parallel**
  — one stream over all 576 parts would be 51.0 TB through one node (≈ 24 h at 0.6 GB/s; 42.5 TB, ≈ 20 h at 4.25 × 10¹³). The converter takes any
  consecutive subset of parts and writes exactly that subset's byte range of the ASCII file ("2." only from part 0, the
  newline only from the last part), so the part outputs concatenated in part order are the exact file:
  ```
  srun -N576 --ntasks-per-node=1 bash -c 'k=$(printf %04d $SLURM_PROCID); \
      tools/unpack_digits -q -o <outdir>/e.txt.part$k <outfile>.part$k'      # the parts are on the shared file system: any node converts any part
  cat <outdir>/e.txt.part* > e.txt                                            # optional: one file (parts sort by name) -- NOT on the same Lustre at 5.1e13 (trap 14)
  ```
  Each part's digits are checked against the residues its run stored in the header. `--cmp <reference>` on one part
  compares it with the same byte range of the reference (the offset comes from the part's digit range in its header), so
  a reference check also runs per node in parallel. The byte range of part k: from byte 0 (part 0) or k0 + 1 (its first
  digit k0) to k1 + 1, plus the newline for the last part (k0, k1 in the header; the converter prints them).

## 8. Known traps

1. **`setarch x86_64 -L` under OSHMEM only.** OpenMPI 4.1.6's OSHMEM registers every anonymous `rw-p` mapping below
   the executable's `_end` and their count must match on every PE — with ASLR's top-down layout it does not, and
   `shmem_init` crashes in the modex about half the time; the legacy bottom-up layout fixes it (results/S.md).
   `mnrun.sh` adds it for `COMM_TRANSPORT=shmem`. Not needed on Cray / SOS / rocSHMEM; harmless if kept.
2. **`shmem_ctx_fence` does not order nbi puts on OSHMEM 4.1.6** (the receiver saw partial slabs at the signal); the
   transport orders by `quiet` unless `COMM_SHMEM_FENCE=1`. Test a conforming implementation with `t_comm` before
   switching (the 3 MiB rows would fail).
3. **`SHMEM_THREAD_MULTIPLE` is nominal on OSHMEM 4.1.6** (four threads in `wait_until` crash); hence `COMM_SHMEM_SERIAL=1`
   by default. `=0` is the target's setting — verify with `t_comm` at 8 PEs, `t_dist` layered, 10⁸ at size 4.
4. **The TCP port base** (`COMM_PORT`, the fallback transport) must stay below 32768: above it a listener collides
   with an ephemeral outgoing port now and then (`bind: Address already in use`, one process exits, the rest hang).
   `mnrun.sh` draws 20000–26000. Irrelevant under SHMEM.
5. **The page-cache rule** (RESULTS §68): a timed single-node run with the reference file in the page cache is 3–5 s
   faster in init (the seeds' input) — evict it first (`posix_fadvise(…, POSIX_FADV_DONTNEED)`); on the target the
   references are elsewhere, but the same holds for any large file the node just wrote (a previous run's part file,
   a checkpoint): the first run after a write is not the timing.
6. **The arena request at size > 1 is `binsplit.c`'s `tree_need_dev`**, which at 7aded87 sizes the top level as ONE
   transform (n = 2 N_A uncapped, q = n / (4 g)) with two g × C × 4-limb spill buffers — at 576 nodes and 4 × 10¹⁰ per
   node ≈ 440 GB per node of arena, which no node has, although `rns_dist.c` would have formed capped pieces. Agent
   G's gridded tree must re-derive it (the model's `grid` form: `mem_model.tree_need_dev`), else the target run fails
   at init above ≈ 1.9 × 10¹⁰ per node whatever the pieces do.
7. **`MN_GROUPS` is parsed but, at 7aded87, not walked**: `mn_tree` (mn.c) still forms binary levels `2^l` clipped to
   the size, and `mn_group_at` the groups `[k 2^l, (k+1) 2^l)`; `mn_groups_parse` (rns_dist.c) has no caller
   (`grep -n mn_groups_parse ecalc/*.c`). Until G's tree walks the parsed schedule, `MN_GROUPS` has no effect and the
   schedule is the binary one (which the model costs within 5 % of the others).
8. **A communicator id is used once per run** (the SHMEM mailbox row is never cleared); every group is created once
   by `mn_group_at` — a restart is a new process, so no issue; a tool that creates groups in a loop would hit it.
9. **s24-30 runs 50 % slower** than the other aac6 nodes at the large sizes (results/M11.md): a measurement note —
   on the target, take the per-node phase lines of every node from the first large run and look for outliers before
   trusting a wall.
10. **The reference for a 576-node run does not exist**; the checks are T1 (eight 62-bit primes, corrected in Phase
    11), T2 (windows) and the recheck. A single-node run of the same D per node is *not* the same number.
11. **The SHMEM transport stages every exchange through its pool and keeps the staging per communicator**
    (`comm_shmem.c`: `staging(p, send, recv)` grows `sst`/`rst` to the largest exchange seen on that communicator and
    frees them only at `comm_destroy`; the tree's level meshes live to `mn_finalize`). At 576 nodes the result
    exchange of a piece at the cap is q × 8 B = 4.3 GB per APU thread each way, so each of the ten level meshes holds
    ≈ 8.6 GB per thread: ≈ 345 GB per node (`mem_model.shmem_staging`), on top of the 8 GiB default the pool would
    abort at the first level. Agent S's Phase 12 form (the callers' slabs resident in the pool, no staging) removes
    it; failing that, free the staging in `wait()` after the H2D copy (`--staging per_exchange`: 54 GB per node at
    6 × 10¹⁰) and size `COMM_SHMEM_POOL_MB` to the `pool` column of `estimate.py`.
    *Phase 14 P2 (measured)*: the staging has been freed per exchange since Phase 12, and the callers' slabs are not in the
    pool (`rns_dist.c` stages every exchange; TASKS 2.4's resident slabs measured and rejected: +3 q per APU of pool, the peak
    unchanged). The pool's peak is 4 × the largest exchange's send + receive: 81.6 GB per node at the target with the
    defaults, 45.0 GB with `MN_T_CHUNK_MB=1024` (results/P214.md; `estimate.py`'s `pool` column follows the law).

12. **(2026-09-27) Convert per node, not in one stream.** `tools/unpack_digits` over all 576 parts in one process is 42.5 TB
    through one node (≈ 20 h at 0.6 GB/s; 51.0 TB, ≈ 24 h at 5.1 × 10¹³); give each node its own part (§4, Phase 15 IO2).
13. **(2026-09-27) Three primes are refused at the target.** `ECALC_NP` unset means three primes for decimal limbs; at 4.25 ×
    10¹³ on 576 the plan check stops the run (rc 3) at tree level 5. The launch line sets `ECALC_NP=4`; `MN_PLAN_ONLY` with the
    launch line's environment must end with `plan check … OK`. *2026-09-28*: at 5.1 × 10¹³ three primes are refused the same way
    (85 of 1240 products over the term bound, the first at tree level 5; rc 3, measured on the login node). *2026-09-28 (DOC2)*: the
    launch line sets **`ECALC_NP=auto`** (the user's decision 1): four primes for the 151 of 264 pieces over the bound, three for the
    rest and for the one-node tiers; `plan check … ECALC_NP=auto: OK` and a `plan primes` line. **Do not go back to `ECALC_NP=4`**:
    it is +17.2 GB per node (pool 1 at 4 q, `RNS_POOL1_4Q`) — **489.1 GB at 5.1 × 10¹³ with the arena room, over 480** — and +23.3 s
    (modelled); its layout node is 460.29 GB against 443.12 (measured, `BS_LAYOUT_ONLY`).
14. **(2026-09-28) The disk holds the target's digits once in each form, not twice.** At 5.1 × 10¹³ the packed parts are 22.7 TB
    and the ASCII parts 51.0 TB: 73.7 TB of the 122 TB Lustre (shared: check `lfs df -h` for the free space before the run). A
    concatenated `e.txt` beside its parts and the packed parts is 124.7 TB — more than the file system. The parts in order are
    the file (§7); concatenate on another file system, or on this one only after the packed parts are deleted (all checks
    passed) and with ≥ 51 TB free (102 TB in use then). The 4.74 × 10¹³ run (§5 step 6c) adds 21.1 TB packed: delete it after its RECHECK.
15. **(2026-09-28) Never run the target without `RNS_DIST_CACHE_FIT=1` (or `RNS_DIST_CACHE_MN=0`).** The mn transform cache's default
    (`RNS_DIST_CACHE_MN=2`) allocates 2 slots × 16 GiB per APU (137.4 GB per node) at the first mn grid product — tree level 1 at the
    target — whenever `hipMemGetInfo` less 24 GB shows them free, in no budget: `hipMemGetInfo` does not see the VMM arena's regions
    (measured, TC15), so the slots can be taken and the node would reach ≈ 609 GB (471.9 + 137.4, modelled): an OOM kill or rc 6 at
    `rns_dist.c:255` in the tree. On aac6 the default cost +37.5 % at 10¹⁰ (the memory pressure slowed the host-staged exchanges
    12–19×) and ran out of memory at 4 processes (CX15, TC15). With FIT the slots are sized to the cap and bounded by the budget: **0 at
    5.1 × 10¹³ and 4.74 × 10¹³** (`plan cache … -> 0 slots`), 2 slots at the development steps 3–5 (modelled). The estimates price exactly that. Check
    the `plan cache` line of `MN_PLAN_ONLY` and the `transform cache:` line at init: `RNS_DIST_CACHE_FIT=1` must be in it.

## 9. The variables named here exist in the code (checked 2026-09-21 on the q12 branch)

`grep -ohE 'getenv\("[A-Z0-9_]+"\)' ecalc/*.c ecalc/tests/*.c | sort -u` lists every variable the code reads; every variable named in this
file is in that list or in `mnrun.sh` / `mnaccept.sh` / the Makefile (`SHMEM`, `SHMEM_CFLAGS`, `SHMEM_LIBS`, `HIPCC`,
`ARCH`; `COMM_HOSTS`/`COMM_PORT` for TCP; `SHMEM_SYMMETRIC_HEAP_SIZE` and `OMPI_MCA_memheap_base_max_segments` are the
library's, set by `mnrun.sh`; `XT_SYMMETRIC_HEAP_SIZE` is Cray SHMEM's own; `DIST_LAYERED`/`DIST_XGMI`/`COMM_RANK`
modes are `t_dist`'s; `RNS_POOL_GROW` is agent R's stress switch of this phase — grep after the merge). The check
script: `for v in $(grep -oE '`[A-Z][A-Z0-9_]+' docs/TARGET.md | tr -d '`' | sort -u); do grep -q "\"$v\"\|\b$v\b" ecalc/*.c ecalc/*.sh ecalc/Makefile || echo "NOT IN CODE: $v"; done`.
