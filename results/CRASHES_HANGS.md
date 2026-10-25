# CRASHES_HANGS — crashes, hangs and intermittent defects

Scope: the t_mn_grid "hangs", the multi-node segfault and hang track (ROCr vs libfabric memhooks), the startup segfault under Cray SHMEM, the leaf-length race, the batch-tier copy race, `DIST_LOGN_TEST` corrections, clean-failure and budget checks. Old item IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`; raw data and drivers are in `archive:results_raw_2026-10-19/` and `archive:drivers/`. Labels: m measured, mod modelled, a assumed, i inferred.

Sections moved to `archive:results_merged_2026-10-24/CRASHES_HANGS.md` (2026-10-24 cleanup): §A01; §T13; §S18W; §T23; §A05; §R; §C314.

## Topic summary

- **Current state (check ecalc/README.md; main wins on any difference).**
  - `FI_MR_CACHE_MONITOR=userfaultfd` is the `mnrun.sh` default (S46, adopted 2026-10-11): it removed the ROCr <-> libfabric lock-order deadlock (base 22/105 hangs vs 0/104). `FI_MR_CACHE_MONITOR=disabled` gives silently WRONG exchanges and is refused by `mnrun.sh` and `target_kit.sh`.
  - `ECALC_VMM_BG` (S34): `0` maps the arena synchronously (+20 s per 2-node run, m), `2` (S35, process-wide HIP lock) was dropped as a switch value (W2A A1; it hung 9/9 in the reproducer); default stays the background mapper. The production segfault in `hipMemMap` of `vmm_bg_map` (about 1 % of 2-node runs, 4/422 m) is not reproduced; the uffd default is the working fix hypothesis (a: same cause), watch 4-node real-size runs.
  - `COMM_INIT_EARLY` (S16) was adopted as built-in and its switch removed (W2A A2): SHMEM is initialised before any device mapping.
  - `ECALC_SEGV_TRACE=1` prints a signal-safe trace (S18W); `ECALC_FATAL_*` one fatal path and `ECALC_BUDGET_CHECK` (C314) exist, off by default.
  - Fixed defects: garbage leaf length (A114, `maxidx` race, per-thread buffer), batch-tier odd-node copy race (R, `mem_dev_copy_on` now waits), `mdb_add_shifted` round sizing (T23: the "65 corrections" of `DIST_LOGN_TEST=23` at 4 nodes was this, fixed in 9a746c8).
- **Not defects.** The t_mn_grid 2-node "hang" (A01, T13) was the TCP mesh over 1 GbE Ethernet; the mnaccept `HANG` item was the queue item's own `timeout 1500` (unit step needs about 1500 s). TCP transport no longer exists (W2B B4), so neither can recur; `t_mn_grid` now runs over SHMEM.
- **Open.** E1 (7.7e10 start-up hang, 1 in 31, single sighting; the 2026-10-24/25 soaks on current code saw 0 hangs in 301 runs at 7.7e10 (95 % bound 1.0 %) and 0 in 1127 at 1e9, §A114; not reproduced, excluded at its old 3.2 %); S30's 20 s stall of node 1 (host vs rank, cause of the draw unknown, see `results/NODE_SLOWMODE.md`); G2 soak lost (superseded, §G2); the 1 % segfault is still unproven fixed.
- **Superseded.** T13's lock-order suspicion for the t_mn_grid hang (refuted by A01); G12's attribution of "65 corrections" to the sharded division (refuted by T23).

## §HANG — t_mn_grid "hang" inside mnaccept --only unit,e9 (2026-10-13)

**Verdict: not a hang.** ADOPT's two queue items ran `timeout 1500 ./mnaccept.sh <hold> --only unit,e9` and recorded rc 124; both summaries show the eight single-process unit tests passed and no `t_mn_grid` line (the 9th and last unit step, its own limit 1200 s). A repeat on a 2-node hold (main b80ee2e): `--only unit` ran all 9 steps, 9 passed, `t_mn_grid` VERIFY OK (180 checks), **1491 s total** (99 % of a 1500 s limit; `e9` adds about 1 min). Standalone `t_mn_grid` is 100-130 s (m). Rule: queue items running `mnaccept.sh --only unit[,...]` need `timeout >= 2400` and EST_MIN >= 45 (daemon cap is 1.5 x EST_MIN + 5 min); `--only grid` alone is about 2 min. Optional: print each unit step's elapsed time in mnaccept.sh.

## §S46 — crash + hang track (batch 2026-10-11, track T1)

**Question.** The multi-node segfault in `hipMemMap` called from the background VMM mapper `vmm_bg_map` (`ecalc/dbig.c`; §S34) happens in about 1 % of 2-node runs at 6.441e10 digits per node (4/422, m). Candidates: `ECALC_VMM_BG=0` (synchronous mapping, +20 s per 2-node run) and `ECALC_VMM_BG=2` (s35 lock). A plain soak needs about 600 runs per arm, so this track builds a fast reproducer, runs the arms interleaved (aac7, 2 nodes), and soaks 1-node runs on aac6 for the start-up hang seen once in S42.

**Reproducer switches (off by default, digits identical; later removed in W2A A1).** `ECALC_VMM_REPRO=n` makes the mapper unmap/map each chunk n more times (with `DB_POOL_VMM_CHUNK_GB` small); `ECALC_VMM_REPRO_SPIN=us` adds a thread per APU that maps/unmaps a spare chunk every `us` microseconds, concurrent with the exchanges and libfabric registrations. Chosen reproducer **R1**: 1e10 digits, 2 nodes, chunk 0.0625 GiB, `REPRO=8`, `SPIN=1000`. Job A driver (watchdog, hang capture of `ps -L` stacks, Fisher stats, hold handoff) and an aac6 1-node soak; drivers in `archive:drivers/s46/`.

**Findings (m unless marked).**
1. More map calls alone do not segfault (calibration c1, c2: 0 failures in 20 runs, up to about 11 000 `hipMemSetAccess` + `hipMemMap` per APU, mod); concurrency with the rest of the run is the likelier condition (a).
2. With the spinner, `ECALC_VMM_BG=2` hangs inside `hipMemSetAccess -> ... -> KfdDriver::ImportDMABuf -> drmPrimeFDToHandle` (busy in the kernel, about 98 % CPU) holding the mapper and HIP locks: the lock cannot protect against a stuck driver call, and it blocks every wrapped HIP call.
3. **Base-arm hangs are a lock-order deadlock between ROCr's VMM map and libfabric's memory monitor.** Thread 1 (mapper): `hipMemMap -> ... -> MappedHandleAllowedAgent::EnableAccess -> mmap -> ofi_intercept_mmap -> ofi_intercept_handler -> pthread_mutex_lock` (holds ROCr's lock, wants libfabric's). Thread 2 (a compute thread freeing device memory): `hipFree -> ... -> rocr_mm_dealloc_cb` (libfabric's monitor callback, under its lock) `-> DeregisterReleaseNotifier -> pthread_rwlock_wrlock` (wants ROCr's lock). The segfault itself (a SIGSEGV at a small address inside `hipMemMap`, same mapper thread) is hypothesised to be the same interaction without the deadlock (a).
4. Job B crash (4 nodes, 6.441e10/node, 1 in 26 runs): SIGSEGV at about 42 s, faulting-thread frames in `libamdhip64.so.7` at identical offsets to S34's three traces (`+0x3f8624, +0x403b0d, +0x3cbd39, +0x8cf57, +0x8d21d, +0x36bb8e`), then the ecalc mapper frames.
5. `FI_MR_CACHE_MONITOR=disabled`: mn self-test 2097152 of 2097152 points differ, VERIFY FAILED, then SIGSEGV (stale registrations in the MR cache). **Never use.** `kdreg2` is listed by `fi_info -e` but `/dev/kdreg2` is absent on the compute nodes.
6. `FI_MR_CACHE_MONITOR=userfaultfd`: gate 2 nodes 1e9 PASS, digits identical, 18.72 s (base gate 19.55-21.22 s); with R1 PASS, 22.44 s. A gate with `FI_LOG_LEVEL=info` shows "Default memory monitor is: uffd" on both nodes and no fallback line (caveat a: memhooks may still be initialised, so the functional evidence is the 22 -> 0 change).

**Results (collected 2026-10-11 09:00, R1 on aac7, 322 runs, arms interleaved; aac6 soak 223 runs; Wilson 95 % CI).**

| arm | runs | ok | hangs | timeouts | segv | non-OK rate (95 % CI) | mean wall of ok runs |
|---|---|---|---|---|---|---|---|
| base | 105 | 83 | 22 | 0 | 0 | 21.0 % (14.3-29.7) | 44.7 s (ecalc 35.6) |
| `ECALC_VMM_BG=0` | 104 | 104 | 0 | 0 | 0 | 0 % (0-3.6) | 40.7 s (34.5) |
| `ECALC_VMM_BG=2` | 9 | 0 | 7 | 2 | 0 | 100 % (70.1-100) | n/a |
| `FI_MR_CACHE_MONITOR=userfaultfd` | 104 | 104 | 0 | 0 | 0 | 0 % (0-3.6) | 44.6 s (35.4) |
| aac6 soak, 1 node, defaults | 223 | 223 | 0 | 0 | 0 | 0 % (0-1.7) | 80.2 s (75.7) |

Fisher exact (two-sided): base vs bg0 22/105 vs 0/104 p < 1e-6; base vs uffd p < 1e-6; bg0 vs uffd p = 1.0 (rule-of-three bound 2.9 % per arm); base vs bg2 p < 1e-4 (bg2 worse; dropped after 9/9 failures). Segfaults: 0 in every arm, so no segfault test can reject anything. Wall-time means are unpaired across cycles (the 4.0 s bg0 gain is not an ABBA result). Stacks: of 22 base hang captures, all 22 show the compute-thread side and 21 also the mapper side (21 certain, 22 probable); 8 of 9 bg2 captures show the `hipMemSetAccess -> ImportDMABuf` stack.

**Recommendation for the target line (the user decided: uffd adopted).** (a) `ECALC_VMM_BG=0`: 0/104 hangs, +20 s per 2-node 1e9 on aac7 (m earlier), removes the mapper itself. (b) uffd: 0/104 hangs, wall 44.6 vs 44.7 s, removes libfabric's mmap intercept (the lock in the cycle), keeps the MR cache, digits identical. (c) both: no gain shown. Recommended (b) alone. Residual risk: the reproducer produces hangs, not the segfault, and 0 segfaults in 322 + 223 runs cannot test the shared-cause hypothesis; confirming (b) at 4 nodes and real size (rate about 1 in 26) needs 80+ clean runs (a, rule of three). Not tested: (b) at 4 nodes, real size.

## §S34 — where the rc 139 segfault is (2026-10-09)

**Finding (m, 3 independent traces, identical offsets):** the multi-node segfault is inside `hipMemMap`, called from `vmm_map_run` (`ecalc/dbig.c`) in `vmm_bg_map`, the background VMM mapper thread (one pthread per APU, started by `db_vmm_arena_alloc` when `first < arena`, released by `db_vmm_bg_release` from `rns_init`). Symbolised: `libamdhip64.so.7` (ROCm 7.2.4) fault at `+0x3f8624`, via `+0x403b0d, +0x3cbd39, +0x8cf57/+0x8d21d, +0x36bb8e`; at about 42 s wall, when the mapper runs against init / level 1 work and SHMEM/libfabric registration. Consequence: `ECALC_VMM_SAFE=2` (a tail-of-thread fix) cannot help.

**Switch `ECALC_VMM_BG=0`** (branch `s34`, off by default, digits unchanged): map the whole arena synchronously, no `vmm_bg_map` thread; cost is the mapping time the thread hid in init.

**Final soak (2026-10-09, branch s34 8424ed1, 6.441e10 digits per node, m; segfaults counted from log text).**

| arm | nodes | runs | segfaults | non-OK | mean wall |
|---|---|---|---|---|---|
| A base | 2 | 422 | 4 | 8 | 204.2 s |
| B `ECALC_VMM_BG=0` | 2 | 420 | 0 | 2 | 224.2 s (+20.0 s, +9.8 %) |
| A base | 1 | 121 | 0 | - | 118.6 s |
| B `ECALC_VMM_BG=0` | 1 | 121 | 0 | - | 120.0 s |

Fisher exact, 4/422 vs 0/420: one-sided p = **0.063**, two-sided **0.124**: suggestive, not conclusive; stays an off-by-default option. The soak ended early on 10-09 (holds cancelled, NODE_FAIL); "Memory required by task is not available" errors then were allocation teardown. aac7 now has a QOS of at most 6 nodes per user. Host finding: the slow host aac7-n01 (`db_from_bi` upload stall 20.7-25.2 s vs about 1 s, follows the host) is unique in the stall only; low MemAvailable (441 vs 520 GB on 13 nodes) is not unique to it and varies with page cache. Kit rehearsal (m): `t_nodechk` 1 GB upload 0.017 s per APU.

**S35: `ECALC_VMM_BG=2` (branch `s35`, commit 015994a; removed in W2A A1).** Kept the mapper thread but serialised it against every HIP memory-management call with one recursive process-wide mutex (`db_hipmm_lock`, link-time `--wrap` of `hipMalloc`, `hipFree`, VMM calls, stream create/destroy in `hipmm.c`). Gate 2 nodes x 1e9 PASS (digits identical, 21.16 s vs 23.50 s base, m). Not covered: HSA calls made by the libraries themselves. The S46 reproducer showed it hangs (9/9) and does not remove the hazard.

## §S30 — the reciprocal's late node, and today's two segfaults (2026-10-08; analysis only)

**Late node (m).** `NEWTON_MN_RECIP_TS=1` (print) and `NEWTON_MN_CHAIN_BCAST=1` (only node 0 runs the single-node chain `recip_db2` and allgathers r0; digits identical) were tested in an ABBA of 7 rounds at 10 nodes: B - A = **-2.70 s, CI [-13.3, +7.9]**, null (A 394.2 s, B 391.5 s). Why: node 1 is 19.3-25.0 s in the "chain" stage in all 16 runs while nodes 0, 2..9 take 1.0-1.2 s; in the B arm node 1 still needs 20.7-24.1 s although it runs no chain, and the other nodes wait 19.5-23.2 s in the allgather: the stall moved, total unchanged. On node 1 between gather and stage end only `db_init`, `db_from_bi` (first device allocation + 270 KB upload) and `bi_free` run; the stall sits there or is a host-level stall (i; not proven). Node 1 = aac7-n01 in every S27/S30/S31 run, so host and rank effects cannot be separated. The 1-node ladder (S25) scales with exponent 0.97 (14.4 s per 1e10), the 10-node exponent 1.48 is exchange/scale cost, not compute.

**Two segfaults (m).** d30_r4s2_A (s30 base arm, task 5 on aac7-n21) and x2_r3s2_B (s31 X2 arm, task 6 on aac7-n30), both at wall 42 s: last log line "background mapping done" (queued for the mapper lock 10.07 / 9.66 s), 0.3-0.5 s before the "bs: level 1 done" of a clean run, i.e. at the end of the init-time VMM background mapping / start of binsplit level 1. No backtrace (`ECALC_SEGV_TRACE` not set, no core). aac7-n30 also hosted the unexplained 2026-10-06 segfault (3 events, weak, p about 0.2). Crash rate (m): 2 of 54 10-node logs of that day = 3.7 % (95 % CI about 0.5-12.7 %), 0 of 59 before; the base arm crashed too, so not an S30/S31 switch. Plausible cause (i): unsynchronised tails of the mapper thread (`hipStreamDestroy`, `hipSetDevice` outside `g_vmm_map_mx`, parity-1 `db_vmm_arena_wait` not joining) overlapping level 1 with 40 mapper threads queued 8-11 s on the lock; alternatively libfabric/cxi or SHMEM at the same moment. Recommendation then: `ECALC_SEGV_TRACE=1` on every 10-node run, a per-rank host label, and a host-vs-rank permuted-nodelist test for node 1; follow-up is §S34 and §S46.

## §S16 — the intermittent startup segfault of ecalc under Cray OpenSHMEMX on aac7 (Phase 16 S)

**Symptom (m).** 2 nodes, `COMM_TRANSPORT=shmem`, 1e10 digits: task 1 segfaults inside Cray's `shmem_init_thread` in 3 of 23 launches (after `bs: arenas 45.1 GB allocated`); 0 of 24 at 1e9 / `t_comm`; PE 0 then hangs in PMI. At 1e10 the init runs in a process with about 45 GB of VMM address ranges mapped and a busy seed thread; at 1e9 the seeds are done.

**Switch `COMM_INIT_EARLY=1`** (b658922): pool rule and `mn_init` before `mem_sampler_start` / `rns_init`; self-tests stay after. Adopted later as built-in (W2A A2 removed the switch).

**Diagnosis (mod from four faulting frames).** The faulting thread is a ROCm HSA runtime worker (`libhsa-runtime64.so.1` + `libamdhip64.so.7` + `malloc` on a 294 MB rwxp thread stack) jumping through a corrupt pointer (`rip` = 0x21041, 0xfffffffffffff000, 0xffffffffffe00000). Cray OpenSHMEMX 11.8 over libfabric 2.3.1/cxi without `/dev/kdreg2` uses the **memhooks** MR-cache monitor, which binary-patches glibc's `mmap/munmap/madvise/brk/sbrk/mremap` during `shmem_init_thread`; the patch is not atomic against a thread executing those functions at that moment. Tools: `ecalc/tools/segv2.c`, a preload recording the library's SIGSEGV handler and printing the backtrace and maps lines first.

**Rates (m; 1e10 digits, 2 nodes).**

| toolchain | variant | segfaults / launches |
|---|---|---|
| ROCm 7.0.3, Cray OpenSHMEMX 11.8.0, libfabric 2.3.1 | control | **8 / 78 = 10.3 %** (A16 3/23, b1 1/10, b2 0/10, b3 3/25, b4 1/10) |
| same | `COMM_INIT_EARLY=1` | **0 / 55** |
| same | `DB_POOL_VMM=0` | 0 / 10 |
| same | `FI_MR_CACHE_MONITOR=disabled` | 0 / 10 |
| ROCm 7.2.4 | control / `COMM_INIT_EARLY=1` | 0 / 10, 0 / 10 (rate bounded, not measured: P(0 of 10 | 10.3 %) = 0.34) |

Control 8/78 vs early 0/55: one-sided Fisher **p = 1.2e-02**; binomial P(0 of 55 | 10.3 %) = 2.6e-03. Digits identical at 1e9 (2 nodes, 2 runs) and 1e10 (every batch's first run). `mnaccept --only unit,e9,mn` with the switch on ROCm 7.2.4: 15 passed, 1 failed (`t_mn_grid` 2 procs, rc 124 over the TCP transport, the same failure without the switch: §A01). No cost measured (total 53-59 s with and without).

**Trap for any SHMEM-on-cxi setup.** Cray OpenSHMEMX 11.8 / libfabric 2.3.1 (cxi) without `/dev/kdreg2` uses memhooks, which patches glibc's mmap family in place during `shmem_init_thread`; a thread executing those functions then can continue at a garbage address (about 10 % at 1e10 x 2 nodes, 0 at 1e9). Initialise SHMEM before any device mapping, or use `userfaultfd` (see §S46); never call `shmem_init` from a process with live HIP worker threads. The PE hangs in PMI afterwards (launch under `timeout`).

Other findings: `mnaccept.sh`'s `MNRUN_CPUS_PER_TASK=auto` broke every unit step on >= 2 nodes (`scontrol show node` printed one `CPUTot` per node; fixed in bc17d32 with `head -1`); the "killed step poisons the node" trap did not reproduce in 5 killed steps (`scancel <stepid>` after `timeout 300`); a ROCm 7.2.4 build needs `module unload rocm; module load rocm/7.2.4` at launch and the explicit `HIPCC` path (RUNPATH loses to the login's `LD_LIBRARY_PATH`).

## §G2 — hang bound for the `DBIG_QSEL=1` start-up hang (aac6 CPX soak)

TASKS item 2: `DBIG_QSEL=1` (now default) stalled once in 64 aac6 runs (1-node 4e9, after init), not reproduced in 44 repeats (`=0`: 0/14). The soak (armed 2026-10-12 06:30 ET on idle CPX nodes, main a448d73, `ecalc 4000000000`, `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30`, about 20-26 s per run, timeout 3x median, hang capture with gdb through a `PR_SET_PTRACER_ANY` wrapper) measures the 1-node no-network case; the multi-node S46 mechanism is different and not exercised. With zero hangs in n runs the one-sided 95 % upper bound is 3/n. **Results were never filled in** (pending at collection); the aac6 outputs are gone and the soak is superseded by the 2026-10-24 aac7 soak of §A114 E1 (main 2d54d6b, the same 1-node no-network case at 1e9 and 7.7e10: 0 hangs in 1127 + 45 runs). It is not repeated. Classes: OK, HANG, SEGV, VFAIL, OTHER.

## §A114 — A1 (Phase 14, W3): the intermittent leaf / seed-stage defect — root cause, fix, soak

**Evidence.** E1: job 21105, 7.70e10, size 1: a hang right after init (197 threads in `futex_wait_queue`, 2 in `kfd_wait_on_events`; the next line would have followed the `pthread_join` of the seed thread, binsplit.c 1228), 1 in 31 runs, not reproduced in 5. E2: job 21222, `mn e8 size 4` (1e8, 4 node-processes): node 3 printed `leaf P 18385101070989787659 limbs, Q 1466159 limbs` and the level-1 product refused it; the step passed in three other jobs. The garbage length 0xff2500040000020b is uninitialised memory read as a `size_t`.

**Root cause of E2 (i, the value fits).** At size > 1 a batch-tier leaf is copied to device numbers (`db_copy` -> `shift_into` -> `db_norm` -> `maxidx`, dbig.c); `maxidx` scans the four quarters with one OpenMP thread each. The host buffer for the per-block results was a process-wide `g_hred`, allocated on first use inside `flags_reserve` by `if (!g_hred) g_hred = malloc(...)`, unguarded, and `maxidx` read the global twice (as `hipMemcpy` destination, then as scan source). Two threads both seeing `g_hred == 0` both `malloc`; one then scans the other, never-written buffer: heap garbage as a length. It is P not Q and size > 1 because the leaf hand-over's `db_copy(&bs_Pd)` is the first `dbig` operation of a process (first-use), and it is rare because the preceding serialised allocations stagger the threads; at size 1 the same race is possible with a wrong top-level length as symptom (not seen). Not the Phase 12 copy-order race (the D2H copy is host-synchronous).

**Fix (`ecalc/dbig.c`).** `maxidx` copies each quarter into a per-thread stack buffer (`size_t hred[228 * 8]`, 14.6 KB); `g_hred` is gone; `maxidx` also aborts loudly if its result exceeds the limbs scanned. No computed value changes (bit-identical by construction).

**Mechanism measured (m, job 21230, 4 APUs; `tests/t_hred_race`, 20 000 rounds of the old pattern).** Old pattern: **9 884 of 20 000 rounds with more than one allocation, 29 402 garbage reads**; per-thread buffer: **0**, VERIFY OK. In ecalc the race needs a preemption (a scheduling event: idle-node soaks never show it, a loaded batch did).

**Soak (m).** Before-fix binary on s24-26: mn 1e8 size 4 x100, size 3 x30, size 2 x10: **140/140 ok** (so the idle-node rate is < 1/100; T1's failure was 1 in about 4 executions of the mnaccept step under load); after the fix: **140/140 ok**, 0 DIFFERS. E1 (hang after init) is **open**: hypothesis (a): a lost completion of the seed thread's last DMA wait (`mem_dev_copy_wait` does not `hipSetDevice(dev)` before waiting); the gdb-launch soak `h1` (1e9 size 1 x60, 7.7e10 size 1 x8) was running at write-up and its results were lost with the cluster.

**E1 re-soak, 2026-10-24 (main 2d54d6b, aac7, 1 node, default launch, layout D, `./mnrun.sh 1 env ./ecalc <digits>`, no digit file, `ECALC_INIT_TL=1` on every second run; timeout 3x the running median with `eu-stack` capture armed, never fired).** Eight node-streams of independent 1-node runs on six nodes (1e9, 15 runs per 1 run of 7.7e10): **1e9: 1127 of 1127 OK (about 12 s each); 7.7e10: 45 of 45 OK (98-127 s each, plan fit 316 GB device of 373); HANG 0, SEGV 0, VFAIL 0, OTHER 0** (measured; classes by exit code and the run's own VERIFY line). One-sided 95 % upper bounds 3/n: **0.27 % per run at 1e9, 6.7 % at 7.7e10** (measured, from zero events). E1's old rate (1/31 = 3.2 %) is not excluded at 7.7e10: with that rate 45 clean runs have probability 0.23 (modelled). Note the nodes of four of the eight streams had no working NIC (single-process runs, no network use). E1 is not reproduced on the code after the seed-pull rework; its cause is unknown.

**E1 soak extension, 2026-10-24 19:23-20:28 ET (main 1e7c781, same code as 2d54d6b; item 9zv4_soak5).** 7.7e10 only, same launch, four parallel 1-node streams on the four NIC-less nodes: **129 of 129 OK (33 + 33 + 33 + 30), HANG 0** (measured). With the 45 above: **0 hangs in 174 runs at 7.7e10, one-sided 95 % bound 1.7 % per run** (3/n, measured from zero events). E1's old rate 3.2 % would leave 174 clean runs with probability 0.0035 (modelled): **E1 is excluded at its old rate on current code.**

**E1 soak, 2026-10-25 01:39-02:40 ET (main 614267d; items a0e1_soak6, a0e2_soak7).** 7.7e10 only, same launch: two 1-node streams on the two NIC nodes (30 min, **20 of 20 OK**) and four on the four NIC-less nodes (60 min, **107 of 107 OK**, 26 + 27 + 28 + 26), HANG 0 (measured). With the 174 above: **0 hangs in 301 runs at 7.7e10, one-sided 95 % bound 1.0 % per run** (3/n, measured from zero events); E1 stays a watch item.

**Reading of the current seed path (2026-10-24).** (a) `mem_dev_copy_wait` (mem.c) is reached only in the direct (non-VMM) seed path and calls `hipStreamSynchronize` on the stream stored for `dev`; HIP does not need the current device to match, so the missing `hipSetDevice` is not a defect found by reading. (b) The default (VMM) path uses the persistent pull kernels: `db_pull_stop_all` runs at the end of `seeds_stream` before `mem_hstage_free` (hipHostUnregister), so the ordering that hung the dropped branch is not present; the pull streams are non-blocking, so null-stream syncs do not wait on them, but `hipDeviceSynchronize` in `mem_dev_alloc` / `dpool_fill` / dbig.c (pool growth) would wait for the pull kernels until the seed thread quits them: a delay while the seed thread works, a deadlock only if the seed thread ever waited on the thread that sits in that sync (it waits only for `pools_ready`, set before the kernels start). `db_pull_wait` aborts after 600 s instead of hanging. No code change made.
