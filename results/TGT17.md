# TGT17 — the 4.08 × 10¹³ target: set to fit the device-memory edge comfortably (2026-10-06)

Login-node and local work only (aac7 `uan1`, no GPU, no compute jobs), on `main` fast-forwarded to `eebb0da` in the
`~/ofimem17` clone (rebuilt, `source ecalc/aac7env.sh && make -s -j16`), the launch line of docs/TARGET.md §4
(`DM_MN_LEAN=1`, `comm_ofi` default on cxi). This note follows **the user's decision of 2026-10-06**: set the target
digit count so the run fits COMFORTABLY in the target's available memory, not razor-thin against a bar; it can be
raised later if the memory configuration changes. It replaces results/CAP17.md's 4.452 × 10¹³ answer (margin 0.53
GB to CAP17's 363 GB bar) with a size chosen against tighter bars (353 GB device, 480 − 30 GB node) and a larger
margin. Every number is **measured** (the C binary's own `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` print, no HIP calls)
unless marked **modelled** or **assumed**.

## 1. The bars

- (a) device layout ≤ 373 − 20 = **353 GB/node** (the target's `hipMalloc` edge, A6 **measured**, minus a 20 GB
  margin — tighter than CAP17's 10/5 GB margins).
- (b) node total + 30 GB (the B7 v-exchange-slot allowance, **assumed**: STD17 found ≈ 13 GB/node of general-map
  v-exchange slots outside the layout at 10 nodes, 15–30 GB estimated at 576) ≤ **480 GB**, i.e. node total ≤ 450 GB.
- (c) `plan check` OK, and the size sits **just below** a grid step, not on it.

## 2. Method

Same as CAP17 §2: `BS_LAYOUT_ONLY=<D per node>:576 ./ecalc <D> /dev/null` and `MN_PLAN_ONLY=<D>:576 ./ecalc` with
the launch line's environment: `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto
RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 DM_MN_LEAN=1
COMM_SHMEM_ROUND_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6
COMM_OFI_PLAN_CXI=1` (the last substitutes for a real cxi NIC, absent on the login node, per CAP17's trap 20 —
without `COMM_TRANSPORT=shmem` the `room:` line silently drops the SHMEM and `comm_ofi` pools). `device` = `room
planes` + `room arena_with_room` + `room ofi_pool` (bytes; CAP17's definition, excludes the SHMEM symmetric pool and
the host-side terms, which the code buckets under `host`). `node` = the C binary's own `node` figure (planes + arena
+ bs_grow + host, which includes the SHMEM pool, the OFI pools and the seed buffers). Every figure cross-checked
with `python3 mem_model.py --check-c`: **exact (0.0000 %) at every point tested**.

## 3. The sweep

At the default `ECALC_PLANE_CAP=2^31`, `ECALC_NP=auto`, sweeping `D` (total digits on 576 nodes):

| D range | arena | device (planes 120.877 + arena + ofi_pool 9.664) | vs 353 GB | node | vs 450 GB |
|---|---|---|---|---|---|
| 4.0–4.0815 × 10¹³ | 214.748 GB | **345.29 GB** | **fits, +7.71** | 387.37 GB | fits, +62.63 |
| 4.0816–4.0899 × 10¹³ | 223.338 GB | 353.88 GB | fails, −0.88 | 395.96 GB | fits, +54.04 |
| 4.1–4.44 × 10¹³ | 223.338 GB | 353.88 GB | fails | 395.96 GB | fits |
| 4.452 × 10¹³ (CAP17's boundary) | 231.928 GB | 362.47 GB | fails, −9.47 | — | — |

The step from 214.748 GB to 223.338 GB — the one that matters for the 353 GB bar — was bisected on the login node
(`BS_LAYOUT_ONLY`/`MN_PLAN_ONLY`, 576 nodes) to **1 × 10⁹ digit resolution**:

| D | arena (bytes) |
|---|---|
| 40,815,000,000,000 | 214,748,364,800 (the lower tier) |
| 40,816,000,000,000 | 223,338,299,392 (the step) |

**The grid step is exact at 4.0815 × 10¹³ → 4.0816 × 10¹³.** Both device bound (a) and node bound (b) are flat
across the whole lower tier (4.0–4.0815 × 10¹³) — device 345.289514112 GB, node 387.374957984 GB — because planes,
arena and the ofi pool are all unchanged in this range (the same mechanism CAP17 found: the grid/plane structure is
flat across narrow ranges). Bound (b) (node total + 30 ≤ 480) is **not binding anywhere in this sweep** — even the
failing 4.0816–4.44 × 10¹³ range sits at 395.96 + 30 = 425.96 GB, 54 GB under 480. Bound (a) (device ≤ 353 GB) is
the one that decides the answer, exactly as CAP17 found for its own bars.

## 4. The answer

**4.08 × 10¹³ digits** (`ecalc 40800000000000`) — a clean round number inside the lower tier, **1.6 × 10¹⁰ digits
(0.039 %) below the grid step at 4.0816 × 10¹³**, not on it.

- **Device layout: 345.289514112 GB** (planes 120,877,472,896 B + arena_with_room 214,748,364,800 B + ofi_pool
  9,663,676,416 B) — **7.71 GB of margin to the 353 GB bar** (bar (a)).
- **Node total: 387.374957984 GB**; **+ 30 GB (B7) = 417.37 GB — 62.63 GB of margin to 480 GB** (bar (b)).
- **`plan check` OK — 1238 products**, the largest piece 1,416,666,666,673 limbs (dist_mn j 566,666,666,668 →
  1,133,333,333,335 Q_t r, g 576; P24 piece 1,133,333,333,338 + 283,333,333,335 limbs = 850,000,000,004 +
  212,500,000,002 points), bound per product (four primes over 58,424,467,928 terms: **119 of 155 pieces**); the
  longest transform 2^40 of 2^44.
- **Distance to the next step: 16,000,000,000 digits** (4.0816 × 10¹³ − 4.08 × 10¹³), where the arena steps 214.748
  → 223.338 GB and device jumps to 353.88 GB (−0.88 GB, fails bar (a)).
- `mem_model.py --check-c`: **exact, 0.0000 %** (0 terms not exact).

This is a **22.7 % cut from the 5.276 × 10¹³ former headline** and **21.1 % from the 5.167 × 10¹³ former second
test size** — a larger cut than CAP17's 4.452 × 10¹³ answer (15.6 %/13.8 %), in exchange for 7.71 GB of margin
(bar a) instead of CAP17's razor-thin 0.53 GB (against its looser 363 GB bar). Raise the target later (closer to
CAP17's 4.452 × 10¹³, or back to the 5.276 × 10¹³ headline with `ECALC_PLANE_CAP=2^30`, or to the full headline
outright) if the memory configuration — the 373 GB/node `hipMalloc` edge, or the B7 v-exchange-slot allowance — is
lifted or found larger than assumed.

## 5. Modelled walls (`estimate.py`, EST17.md's flag set)

Command (per-node D = 4.08 × 10¹³ / 576 = 70,833,333,333.33): `DM_MN_LEAN=1 MN_OUT_DKM_HI=1
MN_MODEL_MAP_RATE=0.070 python3 estimate.py --g 576 --D 70833333333.33 --np-mn auto --lat 8.5e-6 --hide-pow2 0.72
--t-round 0.015 --bw <BW> [--local-factor 1.22]`. The target fabric profile's default `write_bw` is already 1.0
GB/s/node (matching EST17's "write" column; confirmed by varying `--write-bw` explicitly: 0.6 GB/s gives a visibly
different write column, 1.0 GB/s matches the no-flag default).

| case (`--bw`) | ROCm 7.2.4 no-write / write | ROCm 7.0.3 (`--local-factor 1.22`) no-write / write |
|---|---|---|
| (i) 4 NICs, aac7-measured efficiency (`--bw 11`, measured aac7 / assumed on the target) | 606.7 / 602.4 s | 642.3 / 638.0 s |
| (ii) 8 NICs, same efficiency (`--bw 47`, assumed) | **282.4 / 282.9 s** | 318.0 / 316.0 s |
| (iii) line rate (`--bw 100`, assumed, old) | 229.6 / 236.6 s | 265.4 / 270.0 s |

All **modelled**, labels as EST17.md §6 (bw 11 measured-aac7/assumed-at-576, bw 47 and bw 100 assumed). Node memory
printed by `estimate.py` at this size: dev ≈ 348 GB, host ≈ 44 GB, pool ≈ 10 GB, node ≈ 386 GB (model; within ~0.2 %
of the C-measured 345.29 / 387.37 GB above — the usual model/C gap at the default plane cap). Every case is
**faster than the former 5.276 × 10¹³ headline at the same `--bw`** (EST17.md §3: case i 750.1/744.6 s, case ii
352.6/360.6 s, case iii 288.0/302.7 s at 7.2.4) — fewer digits costs no time at all, it saves it.

## 6. Open items

- **`TARGET_BELOW` (mem_model.py, 4.74 × 10¹³) was not touched.** It is now *above* the new target, not below it,
  so `estimate.py --target`'s "one step below" row is stale until a fresh lower-tier sweep picks a real value (a
  finer sweep below 4.08 × 10¹³ found tiers at 3.5/3.6/3.7/3.8/3.9 × 10¹³ with arena 201.86/206.16/206.16/210.45/
  214.75 GB — no single clean "one step below" was chosen here, out of this task's scope).
- **`ecalc/e16_headline.sh` was reviewed, not changed.** Its `E16_DIGITS` default (`G * 91600000000`) is a
  separately *measured* 10/12-node scaled test slice tied to specific plan-check figures (102/98 products, pool
  size, grid schedule) at the former 5.276 × 10¹³ target's per-node share — not the 576-node target digit count
  itself. A proportional scale-down (≈ 7.08 × 10¹⁰) is arithmetically trivial but would need a fresh `MN_PLAN_ONLY`
  sweep at g = 10/12 to re-measure the plan-check figures the script's comments assert; left for whoever next runs
  Phase 16 E at the new target.
- Bound (b) (node total + 30 ≤ 480) was never binding in this sweep — the device bound (a) decided the answer at
  every size tested, consistent with CAP17.

## 7. Labels

measured: all `BS_LAYOUT_ONLY`/`MN_PLAN_ONLY` figures (§3, §4), the `hipMalloc` edge (A6), the VMM-shares-the-edge
finding (OFI17/RESULTS §107), the aac7 comm_ofi efficiency (EST17 §1).
modelled: every wall and node-GB figure from `estimate.py`/`mn_model.py`/`mem_model.py` arithmetic (§5), the
`mem_model.py --check-c` exactness (arithmetic identity, not a new measurement).
assumed: the 30 GB B7 allowance (bar b; STD17 estimated 15–30 GB at 576 from a 10-node ≈13 GB measurement), the
8/4-NIC injection bandwidths (bw 47/11) and line rate (bw 100), the write rate holding at 576 writers, the
ROCm 7.0.3 local-factor 1.22 (fitted at 10¹¹ on aac7).
