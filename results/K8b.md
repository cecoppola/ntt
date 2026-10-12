# K8b - seed transfers without the HIP runtime inside init's mapping window (2026-10-12)

Task K8b (follow-up to K8, results/K8.md; coordinator input from O7b). Branch `k8b` off main a448d73, not merged.
Labels: measured (m) / modelled (mod) / assumed (a). Times Eastern. All runs on aac6 PPAC_MI300A_SPX, one node (4 APUs),
ROCm 7.2.4. Raw logs stay on aac6 (`~/k8b/`).

## 0. Summary (probe done; gate + ABBA armed, see RESUME)

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

## 1. The question

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

## 2. The micro-probe (`tools/k8b_probe.cpp`, `tools/k8b_probe_driver.sh`)

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

## 3. Probe results (m; aac6 hold 22990, 03:22-03:29 ET; `~/k8b/probe_summary.txt`, per-run logs `~/k8b/probe/`)

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

## 4. The switch: `BS_SEED_PULL=1` (off by default)

`dbig.c`: `k_pull`, `db_pull_start_all` / `db_pull_resolve` / `db_pull_copy_async` / `db_pull_wait` / `db_pull_stop_all`;
`binsplit.c`: `seeds_stream` picks the pull functions instead of `db_copy_h2d_async` / `db_copy_h2d_wait` when the switch is
on (VMM arenas only), resolves the two buffers' device pointers right after registering them, and stops the kernels before
unregistering them; `binsplit_seeds_begin` launches the kernels right after `seed_stream_pools` (main thread, the parity-0
halves mapped, the background mapping not yet released: nothing contends with the four launches). The copies are the same
bytes to the same addresses as before: digits identical. One row in `ecalc/README.md`; registry regenerated
(`tools/switch_registry.py --check`: 0 problems).

## 5. Gate and ABBA (`tools/k8b_driver.sh`, `tools/k8b_stats.py`)

(filled in from `~/k8b/summary.txt`)

## RESUME (armed 03:30 ET 2026-10-12)

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
