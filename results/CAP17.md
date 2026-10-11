# CAP17 — the device-memory edge at the target (373 GB/node) against the layout (2026-10-06)

Login-node and local work only (aac7 `uan1`, no GPU, no compute jobs), on `main` at `8e03ba0` (rebased; `comm_ofi`
default on cxi; the launch line of internal target notes §4, `DM_MN_LEAN=1`). Build: fresh clone `~/ntt-cap17` on aac7,
`source ecalc/aac7env.sh && make -s -j16` (rocm/7.2.4, the aac7 default toolchain; the build is login-node-only
sizing, the toolchain used does not affect the layout). Every number below is **measured** (the C binary's own
`BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` print, on the login node, no HIP calls) unless marked **modelled** or **assumed**.

## 1. Background

- A6 (**measured**, target): `hipMalloc` stops at **93.36 GB/APU = 373 GB/node**.
- OFI17 / RESULTS §107 (**measured**, aac7 SPX, job 12287): VMM (`hipMemCreate`) edge 444.0 GB vs `hipMalloc` dev
  edge ≈ 448.0 GB per node — within one 4 GB step, so **VMM shares the device cap**.
- So the device-located layout — `hipMalloc` planes + VMM arena + the `comm_ofi` device pools (Phase 17 OFIMEM,
  RESULTS §108) — must fit under **373 GB/node minus a margin**. This task uses two margins: **363 GB** (373 − 10)
  and **368 GB** (373 − 5).

## 2. Method

`BS_LAYOUT_ONLY=<D per node>:576 ./ecalc 1 /dev/null` and `MN_PLAN_ONLY=<D>:576 ./ecalc 1 /dev/null` with the
launch line's environment (internal target notes §4): `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1
ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 DM_MN_LEAN=1
COMM_SHMEM_ROUND_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6`, plus
`COMM_OFI_PLAN_CXI=1` (the login node has no cxi; `mnrun.sh` sets this from the first compute node, per
`comm_ofi.c`'s comment) so the plan and the room line see the `comm_ofi` pools exactly as the compute nodes would.

**Trap found (new; see internal target notes update below): without `COMM_TRANSPORT=shmem` in the environment, `as_room_fits`
silently returns 0 for both the SHMEM pool and the `comm_ofi` pools** (`binsplit.c`'s `as_shmem_pool`: `if (size < 2
|| !(tr && !strcmp(tr, "shmem"))) return 0;`) — the `room:` line still prints `fits 1` and a plausible-looking device
figure, 9.66 GB *short*, with no warning. The first pass of this task's sweep used `BS_LAYOUT_ONLY` alone (no
`COMM_TRANSPORT`) and got exactly this: `ofi_pool 0` throughout. All numbers below are the corrected, full-launch-line
runs; every `device` figure in §3 includes the `comm_ofi` pools.

`device` (the quantity compared to 373/363/368) = `room planes` + `room arena_with_room` + `room ofi_pool` from the
C binary's own `room:` print line (bytes; decimal GB below). This matches the task's definition exactly (planes +
VMM arena + `comm_ofi` device pools) and **excludes** the SHMEM symmetric pool and the host-side terms (seed
buffers, `bs_grow`), which the code's own `room:` line buckets under `host` even though `COMM_SHMEM_DEVHEAP=1` backs
the SHMEM pool with device memory — this is a labelling choice in the C code (budget-checked against 480 GB total
node, not against 373 GB device-only), not a measurement gap; flagged as an open item in §5.

Every `device`/`room` figure below was cross-checked with `python3 mem_model.py --check-c <captured output>`:
**exact (0.0000 %) at every point tested**, including the `comm_ofi` pool term (Phase 17 OFIMEM, merged into `main`
as 9e42e7a, already covers this).

## 3. The headline sizes' device layout (measured)

At the default `ECALC_PLANE_CAP=2^31`, `ECALC_NP=auto`:

| total digits | planes | arena (+room) | `comm_ofi` pools | **device** | vs 363 GB | vs 368 GB | vs 373 GB (raw edge) |
|---|---|---|---|---|---|---|---|
| 5.276 × 10¹³ (the launch line's headline) | 120.877 GB | 274.878 GB | 9.664 GB | **405.42 GB** | **+42.42** | **+37.42** | +32.42 |
| 5.167 × 10¹³ (the second test size) | 120.877 GB | 274.878 GB | 9.664 GB | **405.42 GB** | **+42.42** | **+37.42** | +32.42 |

Both headline sizes give the **same** device figure (the grid/plane structure is flat across this narrow range) —
and both are **over the raw 373 GB edge itself**, before any margin, by nearly 33 GB. Neither headline size fits
the device-memory edge at all under the launch line's current settings. (`plan check` is `OK` at both, 1240
products each — the digit count is not the blocker, the layout is.)

## 4. The sweep: the largest digit count that fits

Sweeping `D` downward (same environment, `ECALC_PLANE_CAP=2^31`) finds the grid-structure step where `arena`
drops a tier (the arena is quantized to whole VMM chunks plus `BS_ARENA_ROOM`'s 0.16 × hole, so it steps rather
than scaling smoothly; `comm_ofi_pools` stayed flat at 9.664 GB across the whole range tested, 4.0–5.276 × 10¹³):

| total digits | arena tier | **device** | vs 363 GB | vs 368 GB |
|---|---|---|---|---|
| 4.454 × 10¹³ and up to 4.632 × 10¹³ | 240.518 GB | 371.06 GB | over (+8.06) | over (+3.06) |
| **4.452 × 10¹³ (the boundary, exact)** | **231.928 GB** | **362.47 GB** | **fits (+0.53 margin)** | **fits (+5.53 margin)** |
| 4.1–4.44 × 10¹³ | 231.928 GB | 362.47 GB | fits | fits |
| 4.0 × 10¹³ | 214.748 GB | 345.29 GB | fits (+17.71) | fits (+22.71) |

**The grid step is exact at 4.452 × 10¹³ → 4.454 × 10¹³** (bisected to 0.002 × 10¹³ resolution; `MN_PLAN_ONLY` at
4.452 × 10¹³: `plan check OK -- 1240 products`, largest piece bound "121 of 161 pieces"). `mem_model.py --check-c`
matched exactly at this point (room node 404,554,831,488 bytes C = model, 0.0000 %).

**Answer: the largest total-digit count on 576 nodes whose device layout fits both bars is 4.452 × 10¹³ digits**
(device 362.47 GB; margin 0.53 GB to the 363 GB bar, 5.53 GB to the 368 GB bar, 10.53 GB to the raw 373 GB edge).
This is a cut of **15.6 %** from the 5.276 × 10¹³ headline and **13.8 %** from the 5.167 × 10¹³ second test size.

**The 363 GB margin here is razor-thin (0.53 GB)** — under one VMM chunk (2 GiB) of real-world slop. For a size
actually run on the target, the next tier down, **4.1–4.44 × 10¹³ (device 362.47 GB is the same tier; the next
*lower* tier is ≤ 4.0 × 10¹³ at 345.29 GB, 17.7/22.7 GB of margin)**, is the safer recommendation unless the comm
pool / SHMEM-pool labelling question in §5 is resolved in the code's favor first.

Wall time (modelled, `estimate.py --target`-style single-point run, `--g 576 --D 77291666666.67 --np-mn auto --lat
8.5e-6 --hide-pow2 0.72 --t-round 0.015`): **260.7 s without the write, 279.0 s with it @ 1.0 GB/s** at 4.452 × 10¹³
— *faster* than the headline's 290.2/303.9 s (fewer digits), not slower; the digit cut costs capability, not time.

## 5. Levers that could raise the fit without a size cut

All **modelled** unless stated; no compute jobs were run (login-node sizing only). Every lever below is an
existing, off-by-default environment switch — no code change, digits stay byte-identical.

### (a) Host-pinned planes/arena (a redesign) — not recommended, no credible number without new code
Cited at internal code notes A2 and the decision register's **ME10** (internal code notes):
`hipMemcpy` to host-backed memory **measured** collapsing to **21 GB/s**, against MI300A's HBM **measured** at
**3.2 TB/s sustained per APU** (TARGET_HW_REVIEW row 7) — roughly **150× less bandwidth**. The arena (232–275 GB)
and planes (69–121 GB) are read and written on essentially every tree/reciprocal/division pass, not once; a single
extra full pass over the arena alone would cost ≈ 240 GB / 21 GB/s ≈ 11.4 s (**modelled, order-of-magnitude,
assumed one pass**), and the real code touches this memory many times per run, so the realistic cost is minutes,
not seconds. No defensible point estimate exists without actually building the redesign and re-benchmarking —
this is a research item, not a lever to pull before the target run.

### (b) A smaller plane cap (`ECALC_PLANE_CAP`) — the strongest lever found; keeps the full headline size
Measured device (full launch-line env, including `comm_ofi` pools) at the **headline** 5.276 × 10¹³ / 5.167 × 10¹³:

| `ECALC_PLANE_CAP` | planes | arena | ofi pools | **device** | vs 363 | vs 368 | wall (modelled) |
|---|---|---|---|---|---|---|---|
| `2^31` (current default) | 120.877 | 274.878 | 9.664 | 405.42 | +42.42 | +37.42 | 290.2 s (baseline) |
| `3*2^29` | 90.813 | 266.288 | 9.664 | **366.76** | **−3.76 (fails)** | **+1.24 (barely fits)** | ≈ 496.6 s modelled (+71 %) |
| `2^30` | 69.329 | 266.288 | 9.664 | **345.28** | **+17.72 (fits)** | **+22.72 (fits)** | ≈ 527.3 s modelled (+82 %) |

`plan check` is `OK` at every cap tested, at both headline sizes (1216 products at `2^30`/`3*2^29` vs 1240 at the
default — fewer total products, but far more pieces per the time model's own categorization: 453 vs 159). **With
`ECALC_PLANE_CAP=2^30`, the full 5.276 × 10¹³ (and 5.167 × 10¹³) headline fits both the 363 and 368 GB bars with
17.7–22.7 GB to spare — no digit cut needed at all.** The cost is a modelled **+82 % wall time** (≈ 527 s vs 290 s).
`3*2^29` is a weaker version of the same lever: it clears 368 but not 363, at a smaller (but still large) +71 %
modelled slowdown.

**Caveat:** `estimate.py`'s own `mn_model.memory()` does not reproduce the C-measured device bytes at these two
off-default caps (it printed 380.3 / 358.8 GB against the measured 357.10/335.62 GB from an earlier, SHMEM-pool-less
sweep — a known calibration gap outside `ECALC_PLANE_CAP=2^31`). The **memory** numbers in the table above are the
C-measured ones (trustworthy, `--check-c`-style exact arithmetic, no HIP calls); the **wall-time** numbers are
`estimate.py`'s piece-count-driven model and should be read as a *trend* (roughly double the time), not a
calibrated absolute, until a real run confirms it.

### (c) `ECALC_NP` / `RNS_STRATEGY` — already at the cheapest setting; not a lever
`ECALC_NP=auto` (the launch line's setting) is already the minimum-primes choice. Forcing `ECALC_NP=4` costs **+17.2
GB more** per node at the target (internal target notes' existing figure, modelled) — strictly worse. `RNS_STRATEGY=auto`
already selects the cheaper B/B4 form where it fits the pools, falling back to C only when it must; there is no
further headroom to find here without new code.

### (d) Ask the admins for the amdgpu/TTM limits — zero engineering cost, depends on the answer
TARGET_WISHLIST §2.1 already names the parameters: `amdgpu` module params `gttsize`, `vm_size`,
`no_system_mem_limit`; `ttm` module params `pages_limit`, `page_pool_size`. OFI17/RESULTS §107 already answered half
of the open question (VMM shares the `hipMalloc` edge on aac7's SPX nodes) — the remaining ask is whether the
*target's* kernel allows raising the edge itself (currently 373 GB/node, A6, **measured**). If it can be raised even
modestly, both headline sizes (405.42 GB device, 32.42 GB over the raw 373 GB edge) could fit without any cap change
or digit cut. No modelled cost; this is the cheapest lever by far if the admins can grant it, and the one this report
recommends asking about first.

## 6. Summary

| option | digits | device | fits 363/368 | wall (modelled, 5.276e13-equivalent effort) |
|---|---|---|---|---|
| headline, as launched today | 5.276 × 10¹³ | 405.42 GB | **no / no** (over the raw edge) | 290.2 / 303.9 s |
| size cut only | **4.452 × 10¹³** | 362.47 GB | yes (thin) / yes | 260.7 / 279.0 s (faster: fewer digits) |
| `ECALC_PLANE_CAP=2^30`, full headline | 5.276 × 10¹³ | 345.28 GB | yes / yes | ≈ 527.3 s modelled (+82 %) |
| `ECALC_PLANE_CAP=3*2^29`, full headline | 5.276 × 10¹³ | 366.76 GB | no / yes (thin) | ≈ 496.6 s modelled (+71 %) |
| ask the admins to raise the edge | 5.276 × 10¹³ | 405.42 GB | depends on the answer | 290.2 / 303.9 s (unchanged) |

**Open items carried forward:** the SHMEM pool's device-vs-host labelling under `COMM_SHMEM_DEVHEAP=1` (§4's
"razor-thin margin" note — if the SHMEM pool (1.61 GB) also turns out to be device-resident, 4.452 × 10¹³ fails the
363 GB bar by ≈ 1.1 GB and the safe choice becomes ≤ 4.0 × 10¹³); the host-pinned redesign (a) remains unquantified
without new code; the `ECALC_PLANE_CAP=2^30` wall-time estimate (b) is a trend, not a calibrated number, pending a
real run.
