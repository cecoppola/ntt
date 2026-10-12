# K8b - seed transfers without the HIP runtime inside init's mapping window (2026-10-12)

Task K8b (follow-up to K8, results/K8.md; coordinator input from O7b). Branch `k8b` off main a448d73, not merged.
Labels: measured (m) / modelled (mod) / assumed (a). Times Eastern. All runs on aac6 PPAC_MI300A_SPX, one node (4 APUs),
ROCm 7.2.4. Raw logs stay on aac6 (`~/k8b/`).

## 0. Summary

(filled in after the measurements; see RESUME)

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

## 3. Probe results

(filled in from `~/k8b/probe_summary.txt`)

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

## RESUME

- Branch `k8b` (worktree `../ntt-wt-k8b`): the switch, the probe, the drivers, this file.
- aac6: clone `~/ntt-k8b` (built), probe binary and logs in `~/k8b/`.
