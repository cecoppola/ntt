# CRASHES_HANGS — crashes, hangs and intermittent defects

Scope: the t_mn_grid "hangs", the multi-node segfault and hang track (ROCr vs libfabric memhooks), the startup segfault under Cray SHMEM, the leaf-length race, the batch-tier copy race, `DIST_LOGN_TEST` corrections, clean-failure and budget checks. Old item IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`; raw data and drivers are in `archive:results_raw_2026-10-19/` and `archive:drivers/`. Labels: m measured, mod modelled, a assumed, i inferred.

Sections moved to `archive:results_merged_2026-10-24/CRASHES_HANGS.md` (2026-10-24 cleanup): §A01; §T13; §S18W; §T23; §A05; §R; §C314.

## Topic summary

- **Current state (check ecalc/README.md; main wins on any difference).**
  - `FI_MR_CACHE_MONITOR=userfaultfd` is the `mnrun.sh` default (S46, adopted 2026-10-11): it removed the ROCr <-> libfabric lock-order deadlock (base 22/105 hangs vs 0/104). `FI_MR_CACHE_MONITOR=disabled` gives silently WRONG exchanges and is refused by `mnrun.sh` and `target_kit.sh`.
  - `ECALC_VMM_BG` (S34): `0` maps the arena synchronously (+20 s per 2-node run, m), `2` (S35, process-wide HIP lock) was dropped as a switch value (W2A A1; it hung 9/9 in the reproducer); default stays the background mapper. The production segfault in `hipMemMap` of `vmm_bg_map` (about 1 % of 2-node runs, 4/422 m) is not reproduced; the uffd default is the working fix hypothesis (a: same cause), watch 4-node real-size runs.
  - `COMM_INIT_EARLY` (S16) was adopted as built-in and its switch removed (W2A A2): the transport is opened before any device mapping.
  - `ECALC_SEGV_TRACE=1` prints a signal-safe trace (S18W); `ECALC_FATAL_*` one fatal path and `ECALC_BUDGET_CHECK` (C314) exist, off by default.
  - Fixed defects: garbage leaf length (A114, `maxidx` race, per-thread buffer), batch-tier odd-node copy race (R, `mem_dev_copy_on` now waits), `mdb_add_shifted` round sizing (T23: the "65 corrections" of `DIST_LOGN_TEST=23` at 4 nodes was this, fixed in 9a746c8).
- **Not defects.** The t_mn_grid 2-node "hang" (A01, T13) was the TCP mesh over 1 GbE Ethernet; the mnaccept `HANG` item was the queue item's own `timeout 1500` (unit step needs about 1500 s). TCP transport no longer exists (W2B B4), so neither can recur; `t_mn_grid` now runs over xshmem.
- **Closed 2026-10-25.** E1 (7.7e10 start-up hang, 1 in 31, single sighting; the 2026-10-24/25 soaks on current code saw 0 hangs in 301 runs at 7.7e10 (95 % bound 1.0 %) and 0 in 1127 at 1e9, §A114; not reproduced, excluded at its old 3.2 %); reopen rule: a hang caught with `ECALC_INIT_TL=1` plus `eu-stack` reopens it.
- **Open.** S30's 20 s stall of node 1 (host vs rank, cause of the draw unknown, see `results/NODE_SLOWMODE.md`); G2 soak lost (superseded, §G2); the 1 % segfault is still unproven fixed.
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

`ECALC_VMM_BG=0` (synchronous mapping) gave 0/420 segfaults against 4/422 for the default (Fisher two-sided p = 0.124, m: suggestive, not conclusive). The follow-up soak and the adopted fix are in §S46.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/CRASHES_HANGS.md`.)

## §S30 — the reciprocal's late node, and today's two segfaults (2026-10-08; analysis only)

Late node (m): `NEWTON_MN_CHAIN_BCAST` (only node 0 runs the one-node chain) was null in a 7-round ABBA at 10 nodes (-2.70 s, CI [-13.3, +7.9]); the slow chain stage follows node 1, not the algorithm (switch removed 2026-10-11). Two start-up segfaults at the end of the init-time VMM mapping, no backtrace; they lead to §S34 and §S46.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/CRASHES_HANGS.md`.)

## §S16 — the intermittent startup segfault of ecalc under Cray OpenSHMEMX on aac7 (Phase 16 S)

**Symptom (m).** 2 nodes, `COMM_TRANSPORT=shmem`, 1e10 digits: task 1 segfaults inside Cray's `shmem_init_thread` in 3 of 23 launches (after `bs: arenas 45.1 GB allocated`); 0 of 24 at 1e9 / `t_comm`; PE 0 then hangs in PMI. At 1e10 the init runs in a process with about 45 GB of VMM address ranges mapped and a busy seed thread; at 1e9 the seeds are done.
**Switch `COMM_INIT_EARLY=1`** (b658922): pool rule and `mn_init` before `mem_sampler_start` / `rns_init`; self-tests stay after. Adopted later as built-in (W2A A2 removed the switch).
**Trap for any SHMEM-on-cxi setup.** Cray OpenSHMEMX 11.8 / libfabric 2.3.1 (cxi) without `/dev/kdreg2` uses memhooks, which patches glibc's mmap family in place during `shmem_init_thread`; a thread executing those functions then can continue at a garbage address (about 10 % at 1e10 x 2 nodes, 0 at 1e9). Initialise SHMEM before any device mapping, or use `userfaultfd` (see §S46); never call `shmem_init` from a process with live HIP worker threads. The PE hangs in PMI afterwards (launch under `timeout`).
Other findings: `mnaccept.sh`'s `MNRUN_CPUS_PER_TASK=auto` broke every unit step on >= 2 nodes (`scontrol show node` printed one `CPUTot` per node; fixed in bc17d32 with `head -1`); the "killed step poisons the node" trap did not reproduce in 5 killed steps (`scancel <stepid>` after `timeout 300`); a ROCm 7.2.4 build needs `module unload rocm; module load rocm/7.2.4` at launch and the explicit `HIPCC` path (RUNPATH loses to the login's `LD_LIBRARY_PATH`).

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/CRASHES_HANGS.md`.)

## §G2 — hang bound for the `DBIG_QSEL=1` start-up hang (aac6 CPX soak)

TASKS item 2: `DBIG_QSEL=1` (now default) stalled once in 64 aac6 runs (1-node 4e9, after init), not reproduced in 44 repeats (`=0`: 0/14). The soak (armed 2026-10-12 06:30 ET on idle CPX nodes, main a448d73, `ecalc 4000000000`, `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30`, about 20-26 s per run, timeout 3x median, hang capture with gdb through a `PR_SET_PTRACER_ANY` wrapper) measures the 1-node no-network case; the multi-node S46 mechanism is different and not exercised. With zero hangs in n runs the one-sided 95 % upper bound is 3/n. **Results were never filled in** (pending at collection); the aac6 outputs are gone and the soak is superseded by the 2026-10-24 aac7 soak of §A114 E1 (main 2d54d6b, the same 1-node no-network case at 1e9 and 7.7e10: 0 hangs in 1127 + 45 runs). It is not repeated. Classes: OK, HANG, SEGV, VFAIL, OTHER.

## §A114 — A1 (Phase 14, W3): the intermittent leaf / seed-stage defect — root cause, fix, soak

**Evidence.** E1: job 21105, 7.70e10, size 1: a hang right after init (197 threads in `futex_wait_queue`, 2 in `kfd_wait_on_events`; the next line would have followed the `pthread_join` of the seed thread, binsplit.c 1228), 1 in 31 runs, not reproduced in 5. E2: job 21222, `mn e8 size 4` (1e8, 4 node-processes): node 3 printed `leaf P 18385101070989787659 limbs, Q 1466159 limbs` and the level-1 product refused it; the step passed in three other jobs. The garbage length 0xff2500040000020b is uninitialised memory read as a `size_t`.

**Root cause of E2 (i, the value fits).** At size > 1 a batch-tier leaf is copied to device numbers (`db_copy` -> `shift_into` -> `db_norm` -> `maxidx`, dbig.c); `maxidx` scans the four quarters with one OpenMP thread each. The host buffer for the per-block results was a process-wide `g_hred`, allocated on first use inside `flags_reserve` by `if (!g_hred) g_hred = malloc(...)`, unguarded, and `maxidx` read the global twice (as `hipMemcpy` destination, then as scan source). Two threads both seeing `g_hred == 0` both `malloc`; one then scans the other, never-written buffer: heap garbage as a length. It is P not Q and size > 1 because the leaf hand-over's `db_copy(&bs_Pd)` is the first `dbig` operation of a process (first-use), and it is rare because the preceding serialised allocations stagger the threads; at size 1 the same race is possible with a wrong top-level length as symptom (not seen). Not the Phase 12 copy-order race (the D2H copy is host-synchronous).

**Fix (`ecalc/dbig.c`).** `maxidx` copies each quarter into a per-thread stack buffer (`size_t hred[228 * 8]`, 14.6 KB); `g_hred` is gone; `maxidx` also aborts loudly if its result exceeds the limbs scanned. No computed value changes (bit-identical by construction).

**Mechanism measured (m, job 21230, 4 APUs; `tests/t_hred_race`, 20 000 rounds of the old pattern).** Old pattern: **9 884 of 20 000 rounds with more than one allocation, 29 402 garbage reads**; per-thread buffer: **0**, VERIFY OK. In ecalc the race needs a preemption (a scheduling event: idle-node soaks never show it, a loaded batch did).

**Soak (m).** Before-fix binary on s24-26: mn 1e8 size 4 x100, size 3 x30, size 2 x10: **140/140 ok** (so the idle-node rate is < 1/100; T1's failure was 1 in about 4 executions of the mnaccept step under load); after the fix: **140/140 ok**, 0 DIFFERS. E1 (hang after init) is **open**: hypothesis (a): a lost completion of the seed thread's last DMA wait (`mem_dev_copy_wait` does not `hipSetDevice(dev)` before waiting); the gdb-launch soak `h1` (1e9 size 1 x60, 7.7e10 size 1 x8) was running at write-up and its results were lost with the cluster. **Status: closed 2026-10-25** (not reproduced on current main: 0 hangs in 301 runs at 7.7e10, one-sided 95 % bound 1.0 %, measured); a hang caught with `ECALC_INIT_TL=1` plus `eu-stack` reopens it.

**E1 re-soak, 2026-10-24 (main 2d54d6b, aac7, 1 node, default launch, layout D, `./mnrun.sh 1 env ./ecalc <digits>`, no digit file, `ECALC_INIT_TL=1` on every second run; timeout 3x the running median with `eu-stack` capture armed, never fired).** Eight node-streams of independent 1-node runs on six nodes (1e9, 15 runs per 1 run of 7.7e10): **1e9: 1127 of 1127 OK (about 12 s each); 7.7e10: 45 of 45 OK (98-127 s each, plan fit 316 GB device of 373); HANG 0, SEGV 0, VFAIL 0, OTHER 0** (measured; classes by exit code and the run's own VERIFY line). One-sided 95 % upper bounds 3/n: **0.27 % per run at 1e9, 6.7 % at 7.7e10** (measured, from zero events). E1's old rate (1/31 = 3.2 %) is not excluded at 7.7e10: with that rate 45 clean runs have probability 0.23 (modelled). Note the nodes of four of the eight streams had no working NIC (single-process runs, no network use). E1 is not reproduced on the code after the seed-pull rework; its cause is unknown.

**E1 soak extension, 2026-10-24 19:23-20:28 ET (main 1e7c781, same code as 2d54d6b; item 9zv4_soak5).** 7.7e10 only, same launch, four parallel 1-node streams on the four NIC-less nodes: **129 of 129 OK (33 + 33 + 33 + 30), HANG 0** (measured). With the 45 above: **0 hangs in 174 runs at 7.7e10, one-sided 95 % bound 1.7 % per run** (3/n, measured from zero events). E1's old rate 3.2 % would leave 174 clean runs with probability 0.0035 (modelled): **E1 is excluded at its old rate on current code.**

**E1 soak, 2026-10-25 01:39-02:40 ET (main 614267d; items a0e1_soak6, a0e2_soak7).** 7.7e10 only, same launch: two 1-node streams on the two NIC nodes (30 min, **20 of 20 OK**) and four on the four NIC-less nodes (60 min, **107 of 107 OK**, 26 + 27 + 28 + 26), HANG 0 (measured). With the 174 above: **0 hangs in 301 runs at 7.7e10, one-sided 95 % bound 1.0 % per run** (3/n, measured from zero events); E1 stays a watch item.

**Reading of the current seed path (2026-10-24).** (a) `mem_dev_copy_wait` (mem.c) is reached only in the direct (non-VMM) seed path and calls `hipStreamSynchronize` on the stream stored for `dev`; HIP does not need the current device to match, so the missing `hipSetDevice` is not a defect found by reading. (b) The default (VMM) path uses the persistent pull kernels: `db_pull_stop_all` runs at the end of `seeds_stream` before `mem_hstage_free` (hipHostUnregister), so the ordering that hung the dropped branch is not present; the pull streams are non-blocking, so null-stream syncs do not wait on them, but `hipDeviceSynchronize` in `mem_dev_alloc` / `dpool_fill` / dbig.c (pool growth) would wait for the pull kernels until the seed thread quits them: a delay while the seed thread works, a deadlock only if the seed thread ever waited on the thread that sits in that sync (it waits only for `pools_ready`, set before the kernels start). `db_pull_wait` aborts after 600 s instead of hanging. No code change made.
