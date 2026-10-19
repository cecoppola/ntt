# CRASHES_HANGS — crashes, hangs and intermittent defects

Scope: crashes, hangs and intermittent defects: t_mn_grid hang, segfaults, races, DIST_LOGN_TEST corrections, hang bounds. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| A01 | the t_mn_grid 2-node "hang" on main (task N1) |
| T13 | is the t_mn_grid 2-node hang on main the S46 lock-order deadlock? |
| HANG | t_mn_grid "hang" inside mnaccept --only unit,e9 (2026-10-13) |
| S46 | crash + hang track (batch 2026-10-11, track T1) |
| S34 | where the rc 139 segfault is (2026-10-09) |
| S30 | the reciprocal's late node, and today's two segfaults (2026-10-08; aac7 hold 12331; analysis only, nothing launched) |
| S16 | the intermittent startup segfault of ecalc under Cray OpenSHMEMX on aac7 (Phase 16 S) |
| G2 | hang bound for the `DBIG_QSEL=1` start-up hang (aac6 CPX soak) |
| A114 | A1 (Phase 14, W3): the intermittent leaf / seed-stage defect — root cause, fix, soak |
| S18W | `tools/rundriver.sh` and `ECALC_SEGV_TRACE` (branch `s18-w` off `main` ef22f22) |
| T23 | the `DIST_LOGN_TEST` 4-node ">64 corrections" defect |
| A05 | (opportunity G6): TASKS 1.6 `DIST_LOGN_TEST` "65 corrections" and 1.7 checkpoint/MN_GROUPS |
| R | the race at the batch tier's level transition (Phase 12, PLAN.md §27 row R; D5) |
| C314 | C3 (Phase 14): A3 clean failure from worker threads; E12 the collective budget check |

## A01 - the t_mn_grid 2-node "hang" on main (task N1)

(former file: A01.md)

Verdict: **not a deadlock and not a code bug.** `mnrun.sh 2 ./tests/t_mn_grid` with no `COMM_TRANSPORT` uses the TCP
mesh (`comm_tcp.c`), and `COMM_HOSTS` holds the Slurm host names. On aac7 those names resolve (IPv4) to `bond0`, the
**1 Gb/s management Ethernet** (one slave, `enp129s0`, speed 1000), not to the Slingshot `hsn0..3` (200 Gb/s). The test
is then bound by the network at about 110 MB/s per process and runs far past the 150-1200 s timeouts. The earlier pass
(results/AAC7_PORT.md §A16 step 4: 55 s) was over `COMM_TRANSPORT=shmem`. The stacks in results/CRASHES_HANGS.md §T13 (threads idle in futex or
socket waits, a different place each run) show a slow run, not a stuck one. No code change. Main = 6d80ef1, date
2026-10-11 13:50-14:20 EDT, aac7 own job 12522 (aac7-n30 + aac7-n31), cancelled.

#### Evidence (all measured)

| run | transport / hosts | args | result | wall |
|---|---|---|---|---|
| r1, r2 (TCP op trace, temporary) | TCP, host names (bond0) | 0.5 27 | "hang" at 90-120 s | killed |
| r3 | TCP, host names, `RNS_DIST_CACHE_MN=0` | 0.5 27 | "hang" (cache not involved) | killed at 150 s |
| r5 (sampled every 25 s) | TCP, host names | 0.5 27 | progressing: rchar/wchar 2.0 -> 4.8 -> 7.6 GB at t = 25/51/76 s (about 110 MB/s per process, the 1 GbE rate); sockets <ip> <-> <ip> (bond0) | killed at 153 s |
| r6 | TCP, `COMM_HOSTS=<hsn0 IPv4s>` | 0.5 27 | VERIFY OK (200 checks) on both nodes | 101 s |
| r10 | TCP, `COMM_HOSTS=<hsn0 IPv4s>` | 1 28 | VERIFY OK (200 checks) on both nodes | 182 s |
| r7 | `COMM_TRANSPORT=shmem` | 0.5 27 | VERIFY OK (200 checks) on both nodes | 101 s |
| r8 | `COMM_TRANSPORT=shmem` | 1 28 | VERIFY OK (200 checks) on both nodes | 127 s |

- The per-mesh TCP op trace (r1, r2: every alltoall, alltoallv, wait and tiny op per mesh and node) shows **identical
  op sequences and sizes on both nodes on all four meshes**: no lost completion, no count mismatch, no exchange posted
  on one node only. At capture one node was one exchange ahead, waiting on bytes in flight.
- In r2 the TCP sender/receiver threads that `ps` listed had already exited when `eu-stack` reached them ("No such
  process"), so exchanges were still completing. T13's "joined helper thread absent" was a `head -150` cut of the
  `eu-stack` output (about 290 threads per process). Its own `ps` list had 3 threads per process in `wait_woken`
  (socket waits).
- One node in the inverse transform while the other is in the forward one (r2 stacks) is normal here: both stream the
  same chunked all-to-alls, and the inverse/forward pair belongs to consecutive products.

#### Production paths

Not affected. The aac7 and target launch lines (`e16_headline.sh` LINE, `target_kit.sh`) set `COMM_TRANSPORT=shmem`.
`mn_core`, `chunk_post`, `complete_oldest` and `y_alltoall` worked correctly under TCP in this test; only the link was
slow. A run hits this only if it is launched without `COMM_TRANSPORT=shmem` on more than one node. It then uses the
1 GbE path and looks hung. mnaccept's `unit t_mn_grid` line is launched that way: `M 1200 2 ./tests/t_mn_grid 1 28`
with no transport word. That is the S50/T01 failure.

#### For the coordinator (files I do not own)

- mnaccept.sh (a04), line 84: `M 1200 2 COMM_TRANSPORT=shmem ./tests/t_mn_grid 1 28 > "$log" 2>&1; rc=$?`. mnrun.sh
  picks the leading `COMM_TRANSPORT=` word up after `env`. Measured 127 s, VERIFY OK on both nodes (r8). t_mn_grid is
  already in mnaccept's unit step, so no new line is needed.
- Optional, mnrun.sh (a10): when the transport is TCP and `hsn0` exists, fill `COMM_HOSTS` with each node's `hsn0`
  IPv4 instead of the host name (r6/r10: 101 s / 182 s). Or warn that TCP across nodes goes over 1 GbE. Without one
  of these, every multi-node TCP run on aac7 crawls.

#### RESUME

Done. Raw data on aac7 `~/sess1011c/a01/r1..r10` (run.log, stacks, ss samples, the TCP traces of r1/r2). The trace
was temporary and is not committed. Nothing is running; the aac7 worktree is removed.

## T13 - is the t_mn_grid 2-node hang on main the S46 lock-order deadlock?

(former file: T13.md)

Verdict: NOT the S46 memhooks deadlock; a comm-completion hang in the t_mn_grid all-to-all. `FI_MR_CACHE_MONITOR=userfaultfd` does not fix it.

Setup (measured): main ceb94c1, `~/ntt-acc` binary (SHMEM strings present), own 2-node job aac7-n10/aac7-n11,
`mnrun.sh 2 env ./tests/t_mn_grid 0.5 27`, 1 rank per node. Date 2026-10-11 ~13:15-13:25 EDT.

Run A (default env): printed `one plane 5033164 x 4194304: 4.70 s`, then no output; still hung at 76 s (stacks captured, killed by PID/step).
Stacks (eu-stack + ps -L, both nodes, identical shape; raw in aac7 ~/sess1011b/t13/A.stacks):
- Every thread is in futex_wait / kfd_wait_on_events; no thread is running (0.0-10 % CPU average, all state Sl).
- Main thread: pthread_barrier_wait in x_alltoall <- y_alltoall <- chunk_post <- dist_fwd_pre <- dist_fwd <- mn_core (OMP master).
- 4 OMP workers: pthread_join (`__pthread_clockjoin_ex`) in t_wait <- complete_oldest <- y_alltoall <- chunk_post (waiting for a comm helper thread).
- Remaining OMP workers idle in __kmp_fork_barrier; 2 ROCr AsyncEventsLoop threads in hsaKmtWaitOnMultipleEvents (normal idle).
- Zero frames of ofi_intercept_mmap, memhooks, rocr_mm_dealloc_cb, hipFree. No mapper thread blocked on a ROCr/libfabric lock.
- Note: the joined helper thread does not appear in the thread list (only ~12 threads per process), i.e. t_wait joins on something
  whose thread is gone or not listed; this is the thing to look at next (join/lifetime or a lost completion in chunk_post/complete_oldest).
Classification: comm wait / other (barrier + join on the all-to-all path), not the S46 deadlock.

Run B (FI_MR_CACHE_MONITOR=userfaultfd, timeout 200 s):
- B1: printed `one plane 4.47 s`, `2 pieces 10066329 x 8388608: 7.90 s`, then hung until the 200 s timeout (rc=124). So uffd got one case further than A
  (cases are not identical run to run), but did not complete. No stacks captured for B1 (time).
- B2: same: `one plane 4.79 s`, `2 pieces 8.01 s`, then hung to the 200 s timeout (rc=124). uffd: 0/2 completed.

Not measured: stacks of a uffd hang; whether the same frames occur there.

#### RESUME
Next: run t_mn_grid with the stacks captured at hang under uffd; inspect t_wait/complete_oldest in ecalc comm code (the pthread_join target and
who should have completed it); check whether the comm helper thread exits early (error path) leaving the OMP workers joined on a dead/never-signalled handle.

## HANG: t_mn_grid "hang" inside mnaccept --only unit,e9 (2026-10-13)

(former file: HANG.md)

**Verdict: not a hang. The queue items' own `timeout 1500` around `mnaccept.sh` expired while the last unit test was starting or running.**

#### Evidence
- ADOPT's two items ran `cd <wt>/ecalc && timeout 1500 ./mnaccept.sh <hold> --only unit,e9` and recorded `mnaccept rc=124` (the timeout
  exit code). Both summaries show the eight single-process unit tests passed and no `t_mn_grid` line: the script was killed before
  it could print one (t_mn_grid is the 9th and last unit step; its own limit is 1200 s, so it could not itself have timed out).
- A repeat on a 2-node hold (measured, current main b80ee2e, defaults batch_pipe/crt_scan/dm_mn_wrap on): `mnaccept.sh --only unit`
  ran all 9 steps, 9 passed, 0 failed, t_mn_grid VERIFY OK (180 checks), in 1491 s total (so `unit` alone is already 99% of a
  1500 s limit; `e9` after it adds ~1 min more). A second repeat was passing the same steps when the item's 50 min cap ended it.
- The per-run logs of the two ADOPT runs were removed with ADOPT's worktree (not recoverable); standalone t_mn_grid is 100-130 s
  (measured, ADOPT A/B: 101-111 s for default, BS_BATCH_PIPE=0, RNS_CRT_SCAN=0, both).
- The earlier "passes" (SIMPM 18/18, SH3 15/15) fit because t_mul 20 etc. vary by run; the margin is small, so the outcome depended on speed.

#### Fix / rule
- No product or harness bug found; no stack dump was needed (nothing hung). Queue items running `mnaccept.sh --only unit[,...]`
  must use `timeout >= 2400` (unit ~1500 s measured) and EST_MIN >= 45 (daemon cap is 1.5 x EST_MIN + 5 min); `--only grid` alone
  is ~2 min.
- Optional later: print each unit step's elapsed time in mnaccept.sh so a cut-off run shows where the clock went.

#### RESUME
Closed. Raw data: aac7 `~/hang/` (sum_*.txt, run*.out, grid_1.log).

## S46: crash + hang track (batch 2026-10-11, track T1)

(former file: S46.md)

Branch `s46` = main daeb0f9 + branch `s35` (`ECALC_VMM_BG=2`) + the reproducer switches and drivers below. All numbers are
labelled measured (m), modelled (mod) or assumed (a); times Eastern.

#### 1. Question

The multi-node segfault in `hipMemMap` called from the background VMM mapper `vmm_bg_map` (`ecalc/dbig.c`; results/CRASHES_HANGS.md §S34)
happens in about 1 % of 2-node runs at 6.441e10 digits per node (4/422, m). Two candidate fixes exist: `ECALC_VMM_BG=0`
(synchronous mapping, no mapper thread; +20 s per 2-node run, m) and `ECALC_VMM_BG=2` (s35: one process-wide lock around the
mapper's chunks and ecalc's HIP memory / VMM / stream calls). A plain soak needs about 600 runs per arm to tell them apart, so
this track builds a fast reproducer, then runs the three arms interleaved on it (aac7, 2 nodes, Job A), and soaks 1-node runs on
aac6 for the start-up hang seen once in S42 (1 in 64 `DBIG_QSEL=1` runs).

#### 2. Design

##### 2.1 Reproducer switches (off by default, digits identical)

- `ECALC_VMM_REPRO=n`: the background mapper unmaps and maps each of its chunks n more times (same handle, same address,
  access set again; zeroed afterwards as always). With a small `DB_POOL_VMM_CHUNK_GB` (an existing knob, minimum 2 MiB) this
  multiplies the mapper's `hipMemMap` calls: at 1e10 digits on 2 nodes, 0.0625 GiB chunks, n = 8: 84-105 background chunks per
  APU x 9 maps (m, `ECALC_INIT_TL=1`), against 3 per APU at the default 2 GiB.
- `ECALC_VMM_REPRO_SPIN=us`: once an APU's mapper has finished, a thread of its own maps, grants access to and unmaps one spare
  chunk in a VA range of its own (never the arena) every `us` microseconds until `db_vmm_arena_release`, under the mapper's
  locks (and `db_hipmm_lock` under `ECALC_VMM_BG=2`): the mapper's `hipMemMap` concurrent with the whole run, including the
  exchanges and the libfabric registrations. Not under `ECALC_VMM_BG=0` (no mapper exists there, as in the real fix).
  1000-1400 cycles per APU per 1e10 run at 1000 us (m).

##### 2.2 Job A driver (`archive:drivers/s46/s46_drive.sh`, aac7 login node)

Arms rotate per cycle (base / `ECALC_VMM_BG=0` / `ECALC_VMM_BG=2`, cycle c starts with arm c-1 mod 3) on the reproducer env;
every run `ECALC_SEGV_TRACE=1`, the aac7 line of `e16_headline.sh`, no digit output. Watchdog every 5 s: crash text (srun
reports rc 1, so the segfault is counted from the log) -> 30 s for the trace, then kill; 300 s without log growth = hang ->
capture (last 30 log lines, `ps -L -o pid,tid,wchan:32,stat,comm` of our `ecalc` on both nodes via `srun --overlap`, no `-w`)
-> kill our own srun PID (TERM, 60 s, KILL); hard timeout 900 s. Phase per run: post-init once `mn: self-test ... ok` is
printed. Per-arm counts and Fisher exact tests (one- and two-sided, segfaults and any non-OK) in `stats.txt` after every run
(`archive:drivers/s46/s46_stats.py`; it reproduces S34's p = 0.063 / 0.124 for 4/422 vs 0/420). W handoff per BATCH_SLOTS: no run crosses W;
at W it scancels its own hold (checked by name and user), submits A2 (`-N2 -J b1011w --dependency=singleton`, `-t` to S),
polls every 60 s, resumes on A2; no run starts after D - 240 s; then stats, marker, scancel of its own hold.

##### 2.3 aac6 soak (`archive:drivers/s46/s46_soak6.sh`, aac6 login node)

Sleep-until timer to 03:50 EDT (no Slurm job pending in the blackout), then 45-min 1-node jobs on PPAC s24-16 back to back
until D (`-J b1011T1`), ecalc at the defaults (QSEL default) at 6.441e10 digits with `RNS_VERBOSE=1 ECALC_VERBOSE=2
ECALC_INIT_TL=1` (print only; S42's verbosity plus the init timeline, so a hang shows where it stopped), the same watchdog
(capture adds `rocm-smi`), a fresh job after a hang. Start-up vs post-init hang = whether a `bs: level <n>` line was printed.

#### 3. Tests

| test | where | result |
|---|---|---|
| build + gate, switches off (`tools/gate.sh --build --nodes 2 --hold 12472`), twice (before and after the switches) | aac7 2 nodes, 1e9 | PASS both: VERIFY OK, digits identical, total 21.22 / 19.55 s (m) |
| gate on, base: `--env "DB_POOL_VMM_CHUNK_GB=0.0625 ECALC_VMM_REPRO=8 ECALC_VMM_REPRO_SPIN=1000"` | aac7 2 nodes, 1e9 | PASS, digits identical, total 21.21 s (m) |
| gate on, the same + `ECALC_VMM_BG=0` | aac7 2 nodes, 1e9 | PASS, digits identical, total 19.80 s (m) |
| gate on, the same + `ECALC_VMM_BG=2` | aac7 2 nodes, 1e9 | **HUNG** in the mn self-test (killed after 641 s): the reproducer's failure, see 4.2 |
| gate on, `DB_POOL_VMM_CHUNK_GB=0.0625 ECALC_VMM_REPRO=8 ECALC_VMM_BG=2` (no spin) | aac7 2 nodes, 1e9 | PASS, digits identical, total 18.12 s (m) |
| calibration c1: 1e10, chunk 0.0625, `ECALC_VMM_REPRO=8`, base (`~/s46/calib`) | aac7 Job A | 0 failures in 10 runs, wall 34-37 s (m) |
| calibration c2: 1e10, chunk 0.03125, `ECALC_VMM_REPRO=32`, base | aac7 Job A | 0 failures in 10 runs, wall 59-61 s (m) |
| calibration s1: c1 + `ECALC_VMM_REPRO_SPIN=1000` (`~/s46/calib2`) | aac7 Job A | 1 hang in 5 runs (teardown), wall 38-40 s otherwise (m) |
| calibration s2: 4e10, chunk 0.125, `ECALC_VMM_REPRO=4`, `ECALC_VMM_REPRO_SPIN=500` | aac7 Job A | 0 failures in 5 runs, wall 74-106 s (m) |
| calibration R1 (the chosen reproducer): 1e10, chunk 0.0625, `ECALC_VMM_REPRO=8`, `ECALC_VMM_REPRO_SPIN=1000`, base, with the real driver (`ARMS=base KEEP_HOLD=1`, `~/s46/calib3`, 00:42-01:30) | aac7 Job A | **36 runs: 32 ok, 4 hangs (11.1 %), 0 segfaults**; mean wall of ok runs 44.3 s, ecalc total 35.5 s (m); every hang captured (4.3) |
| gate on, R1 + `FI_MR_CACHE_MONITOR=disabled FI_MR_ROCR_CACHE_MONITOR_ENABLED=0` | aac7 2 nodes, 1e9 | **FAIL: mn self-test 2097152 of 2097152 differ**, VERIFY FAILED, then SIGSEGV: libfabric's MR cache without its monitors serves stale registrations. Not usable as an arm |
| driver dry runs (STUB=1: random OK / segfault / hang / failure; simulated Slurm) | local + aac7 login (Job A driver), local + aac6 login (soak) | handoff, A2 poll (also `A2_DEP=auto`), hang capture + kill, deadline, marker, scancel paths all exercised |
| `s46_arm6.sh` DRY=1 | local | prints the bundle / build / start commands; touches nothing |

#### 4. Findings so far

##### 4.1 No segfault from more map calls alone

Multiplying the mapper's calls (c1, c2: up to ≈ 11,000 `hipMemSetAccess` + `hipMemMap` per APU per run, modelled from the
chunk counts) gave 0 failures in 20 runs (m). So the per-call hazard is not what makes the segfault; the concurrency with the
rest of the run (the exchanges, libfabric registrations) is the likelier condition (assumed). Hence `ECALC_VMM_REPRO_SPIN`.

##### 4.2 The spinning mapper hangs inside `hipMemSetAccess` (measured, stacks)

With the spinner on, runs hang (1 of 5 base runs at teardown; the `ECALC_VMM_BG=2` gate in the mn self-test). `eu-stack` of the
hung `ECALC_VMM_BG=2` run (both nodes, `~/s46/bg2hang_*.stk`): one spinner thread per node is inside
`hipMemSetAccess -> hsa_amd_vmem_set_access -> rocr::core::Runtime::VMemorySetAccess -> VMemorySetAccessPerHandle ->
MappedHandleAllowedAgent -> KfdDriver::ImportDMABuf -> amdgpu_bo_import -> drmPrimeFDToHandle -> ioctl`, holding the
mapper lock and (under `=2`) the process-wide HIP lock; the other spinners wait on the mapper lock and the compute threads wait in
the exchange (`x_alltoall` barrier, `s_wait` join). `ps -L`: threads in state R at about 98 % CPU with no wchan, i.e. busy in the
kernel (the dmabuf import), not sleeping. So the `ECALC_VMM_BG=2` lock cannot protect against this: the stuck call is the
mapper's own, inside the driver, and the lock then stops every wrapped HIP call of the process. This is a hang, not the
production segfault; both are in ROCr's VMM path under concurrency, but that they share a cause is an assumption.

##### 4.3 The base-arm hangs are a lock-order deadlock between ROCr's VMM map and libfabric's memory monitor (measured, stacks)

All 4 base hangs of calibration R1 (`~/s46/calib3/cap/c{8,12,28,30}_base.txt`) show the same pair (c8 quoted):
- the spinner (the background mapper's role): `hipMemMap -> Command::enqueue -> VirtualGPU::submitVirtualMap ->
  hsa_amd_vmem_map -> Runtime::VMemoryHandleMap -> MappedHandle::MappedHandle -> MappedHandleAllowedAgent::EnableAccess ->
  mmap -> ofi_intercept_mmap -> ofi_intercept_handler -> pthread_mutex_lock` (libfabric's memhooks monitor intercepts ROCr's
  mmap and waits for the monitor lock);
- a compute thread freeing device memory: `~Buffer -> Device::memFree -> hsa_memory_free -> Runtime::FreeMemory ->
  rocr_mm_dealloc_cb (libfabric's ROCr monitor callback, under the monitor lock) -> hsa_amd_deregister_deallocation_callback ->
  Runtime::DeregisterReleaseNotifier -> AcquireSharedMutex -> pthread_rwlock_wrlock` (waits for ROCr's runtime lock, which the
  mapping thread holds).
So: thread 1 holds ROCr's lock and wants libfabric's; thread 2 holds libfabric's and wants ROCr's. The `ECALC_VMM_BG=2` lock
cannot break this (the inner calls are libfabric's and ROCr's own). The S34 / Job B production fault is a SIGSEGV, not a hang,
at a small address inside `hipMemMap` in the same mapper thread; that it is the same libfabric-monitor / ROCr interaction
(a race on the same structures where the timing does not deadlock) is the working hypothesis (assumed), now testable.
The 4.2 bg2 hang (spinner stuck in `hipMemSetAccess -> ImportDMABuf`, threads at 98 % CPU) is a different stack.

##### 4.4 Job B crash, 2026-10-11 01:19 EDT (from the coordinator; log checked)

aac7 `~/s47/run/log/i_r7_A.log` (s47 build at daeb0f9, 4 nodes, 6.441e10/node, 1 crash in 26 runs, m): rank 0 on
aac7-n10, SIGSEGV addr 0x6efe at about 42 s wall; the faulting thread's frames in `libamdhip64.so.7` are
`+0x3f8624, +0x403b0d, +0x3cbd39, +0x8cf57, +0x8d21d, +0x36bb8e`, then ecalc `0xacbfcb` / `0xacc722` (the mapper), then
`start_thread`: **identical offsets to S34's three traces**. No core file (`ulimit -c` unlimited on the login node, none found
in the run dir).

##### 4.5 Reproducer chosen and its rate

Neither `ECALC_VMM_REPRO` alone (0/20) nor the spinner has produced the segfault itself (0 segfaults in 46 spinner runs, m).
The best available reproducer is R1 (1e10, chunk 0.0625, `REPRO=8`, `SPIN=1000`): **11.1 % of base runs hang (4/36, m)**, all
four in the ROCr/libfabric deadlock of 4.3, i.e. inside the background mapper's `hipMemMap`. Per the coordinator (01:26 EDT),
it is armed as is; crashes and hangs are tallied separately. Caveat: under `ECALC_VMM_BG=0` the spinner does not run (no
mapper exists), so bg0's comparison shows what removing the background mapper removes, under the same `REPRO` call volume.

##### 4.6 Fix arm: libfabric's MR cache monitor = userfaultfd (gated)

- **Correctness hazard (measured): `FI_MR_CACHE_MONITOR=disabled` (with `FI_MR_ROCR_CACHE_MONITOR_ENABLED=0`) gives WRONG
  exchanges**: the mn self-test reported 2097152 of 2097152 points differing and the run ended VERIFY FAILED (then a SIGSEGV).
  The MR cache then serves stale registrations. Never use it on any line.
- `kdreg2`: listed by `fi_info -e`, but no `/dev/kdreg2` on the compute nodes: not available.
- `FI_MR_CACHE_MONITOR=userfaultfd` (keeps the cache, no mmap intercept): gate 2 nodes 1e9 PASS, self-test ok, digits identical,
  total 18.72 s (base gate 19.55-21.22 s); with R1 on top PASS, digits identical, total 22.44 s (m). Added as the 4th Job A arm
  `mon` (base config, `ECALC_VMM_BG` default). Not added to the aac6 soak (single node: no libfabric exchange, and no aac6 gate
  possible before the blackout ends).
- Interim three-arm runs before arming (`~/s46/calib5`): base 1/1 ok, bg0 1/1 ok, bg2 1/1 **hang** (m).

#### 6. Results (collected 2026-10-11 09:00 EDT, by T7)

Reproducer R1 (1e10 digits, 2 nodes, `DB_POOL_VMM_CHUNK_GB=0.0625 ECALC_VMM_REPRO=8 ECALC_VMM_REPRO_SPIN=1000`) on aac7 Job A,
arms interleaved by cycle; 322 runs (marker `S46A_DONE`). aac6 soak: 1 node, defaults, 6.441e10 digits, 223 runs in 8 jobs.
bg2 ran only in phase 1 (first 9 cycles) and was dropped at 03:31 after 9/9 failures. All counts measured. CI = Wilson 95 %.

| arm | runs | ok | hangs | timeouts | segv | non-OK rate (95 % CI) | mean wall of ok runs |
|---|---|---|---|---|---|---|---|
| base (aac7) | 105 | 83 | 22 | 0 | 0 | 21.0 % (14.3-29.7) | 44.7 s (ecalc 35.6 s) |
| `ECALC_VMM_BG=0` (bg0) | 104 | 104 | 0 | 0 | 0 | 0 % (0-3.6) | 40.7 s (34.5 s) |
| `ECALC_VMM_BG=2` (bg2) | 9 | 0 | 7 | 2 | 0 | 100 % (70.1-100) | n/a |
| `FI_MR_CACHE_MONITOR=userfaultfd` (mon) | 104 | 104 | 0 | 0 | 0 | 0 % (0-3.6) | 44.6 s (35.4 s) |
| aac6 soak, 1 node, defaults | 223 | 223 | 0 | 0 | 0 | 0 % (0-1.7) | 80.2 s (75.7 s) |

Fisher exact (two-sided, non-OK): base vs bg0 22/105 vs 0/104, p < 1e-6 (printed 0.0000); base vs mon 22/105 vs 0/104, p < 1e-6;
bg0 vs mon 0/104 vs 0/104, p = 1.0 (indistinguishable; with 0/104 each the rule-of-three upper bound is 2.9 % per arm).
base vs bg2: 22/105 vs 9/9, p < 1e-4 (bg2 is worse than base). Segfault: 0 in every arm, so no segfault test can reject anything.
(stats.txt on aac7 has the full list.) Wall-time means of ok runs are unpaired across cycles; the 4.0 s bg0 gain is not an ABBA result.

##### 6.1 Hang stacks (22 base captures + 9 bg2 in `~/batch1011/T1/jobA/cap/`)

- base, 22 of 22 captures contain the compute-thread side of the deadlock (`rocr_mm_dealloc_cb` -> `DeregisterReleaseNotifier` ->
  `pthread_rwlock_wrlock`); 21 of 22 also show the mapper side (`hipMemMap` -> ... -> `ofi_intercept_mmap` ->
  `ofi_intercept_handler` -> `pthread_mutex_lock`); c51 shows the first side and mutex waits on other threads, taken as the same
  deadlock (the mapper frame was not found by name; one capture is not fully classified). So **ROCr <-> libfabric lock-order
  deadlock: 21 certain, 22 probable, of 22**. 0 startup hangs (all after init).
- bg2, 9 of 9 hung or timed out; 8 of 9 captures have a thread in `hipMemSetAccess` -> `ImportDMABuf` (the 4.2 stack, 98 % CPU
  in the kernel); c5_bg2 has none of these frames (the other bg2 failure type, not analysed). bg2 does not remove the hazard.

##### 6.2 Hazard: `FI_MR_CACHE_MONITOR=disabled`

Measured (section 4.6): wrong exchanges (2097152 of 2097152 points differ, VERIFY FAILED, then SIGSEGV). Never use it on any line.

##### 6.3 Verification that the uffd monitor is active (aac7, own 2-node job 12504, cancelled, 09:01-09:02 EDT)

Gate with `FI_MR_CACHE_MONITOR=userfaultfd FI_LOG_LEVEL=info`, 1e9 digits: PASS (VERIFY OK, digits identical; ecalc total 29.5 s,
inflated by the info log of 10 MB and a node pair that includes the known slow host, not comparable). The libfabric log of both
nodes (pids 1654344 and 71793) says `ofi_monitors_init(): Default memory monitor is: uffd`, and no fallback or uffd-failure
line (0 matches). Caveat (assumed): libfabric also initialises memhooks as one of the listed monitors, so this shows the default
monitor selected is uffd, not that the mmap intercept is absent; the 22 -> 0 hang change in the R1 data is the functional evidence.
Log: aac7 `~/batch1011/T7v/run.plain`.

##### 6.4 Recommendation for the target line (the user decides)

| option | measured effect | cost | note |
|---|---|---|---|
| (a) `ECALC_VMM_BG=0` | 0/104 hangs vs 22/105 | +20 s per 2-node 1e9 on aac7 (measured earlier); target map rate about 7x faster (assumed) | removes the background mapper, so also removes the spinner in the reproducer: shows the mapper is needed for the hang, not that libfabric is the cause |
| (b) `FI_MR_CACHE_MONITOR=userfaultfd` | 0/104 hangs; wall 44.6 s vs 44.7 s base | none measured (gate 18.7 s vs 19.5-21.2 s) | removes libfabric's mmap intercept, the actual lock in the cycle (21-22 of 22 stacks); keeps the MR cache, digits identical |
| (c) both | not measured together | (a)'s cost | no gain over (b) shown; only if the segfault has a different cause |

Recommended: (b) alone on the target line, then watch the 4-node real-size runs. Residual risk: the reproducer produces hangs, not
the segfault; the 4.4 segfault (same `libamdhip64` offsets as S34) sharing this cause is a hypothesis, and 0 segfaults in 322 + 223
runs cannot test it. The 4-node real-size rate is about 1 in 26, so confirming (b) there needs about 80+ runs without a crash (assumed,
rule of three). Not tested: (b) at 4 nodes and at real size; aac6 soak has no libfabric exchange.

#### 5. RESUME

- **ARMED 2026-10-11 01:39 EDT**: Job A driver on the aac7 login node, driver PID 3473981, arms base bg0 bg2 mon, R1 env. The T4 nodechk ran first: PASS on both hosts, upload 0.017 s per APU, MemAvailable 439.7 / 439.9 GB. **aac6 timer**: workstation PID 1798812 (`s46_arm6.sh`; ships and rebuilds at 03:31, soak START 03:50, D 08:56).

- Branch `s46` (GitHub), worktrees: local `../ntt-wt-s46`, aac7 `~/ntt-wt/s46` (driver build, gated) and `~/ntt-wt/s46spin`
  (an earlier calibration build; remove), aac6 `~/ntt-s46` (built before the spinner; `s46_arm6.sh` refreshes and rebuilds it at 03:31).
- Job A hold: aac7 job 12472 (`b1011A`, 2 nodes aac7-n30, aac7-n31, ends 09:24 EDT). Interim 3-arm R1 runs (keep the
  hold busy until arming): `~/s46/calib5` (driver with `KEEP_HOLD=1`, stops by 03:00 EDT or on `~/s46/calib5/STOP`).
- **Arm Job A** (aac7 login node; stops the interim runs first; A2 named `b1011A2` so that B2's singleton does not wait on it):
  `touch ~/s46/calib5/STOP; until [ -e ~/s46/calib5/S46A_DONE ]; do sleep 10; done; cd ~/ntt-wt/s46/ecalc && HOLD=12472 D=1791723386 W=1791712586 S=1791725186 A2_DEP=afterany:12475 A2_NAME=b1011A2 NODECHK=1 ARMS="base bg0 bg2 mon" OUT=~/batch1011/T1/jobA REPRO="DB_POOL_VMM_CHUNK_GB=0.0625 ECALC_VMM_REPRO=8 ECALC_VMM_REPRO_SPIN=1000" setsid nohup bash s46_drive.sh > ~/s46/jobA_driver.log 2>&1 < /dev/null &`
- **Arm aac6** (this workstation; sleeps to 03:31 EDT, ships + rebuilds, starts the soak with START 03:50, D 08:56):
  `A6PW=<private file holding the aac6 password> setsid nohup bash ~/apucode/ntt-wt-s46/archive:drivers/s46/s46_arm6.sh > <scratch>/s46_arm6.log 2>&1 < /dev/null &`
- Outputs: Job A aac7 `~/batch1011/T1/jobA/` (marker `S46A_DONE`, `stats.txt`, `runs.tsv`, `ALERT`, `cap/`), T4 nodechk
  `~/batch1011/T4/jobA/`; aac6 `~/batch1011/T1/aac6/` (marker `S46S_DONE`, `stats.txt`, `runs.tsv`, `cap/`).
- Collect (one line each):
  `ssh aac7 'cat ~/batch1011/T1/jobA/S46A_DONE ~/batch1011/T1/jobA/stats.txt; tail -3 ~/batch1011/T1/jobA/ALERT'`
  `ssh aac6 'cat ~/batch1011/T1/aac6/S46S_DONE ~/batch1011/T1/aac6/stats.txt'`
- Then: section 6 (results, Fisher tests, recommendation for the target line) after the data.

## S34: where the rc 139 segfault is (2026-10-09)

(former file: S34.md)

**Finding (measured, 3 independent traces, identical offsets):** the multi-node segfault is inside **`hipMemMap`**, called from
**`vmm_map_run` (ecalc/dbig.c) in `vmm_bg_map`**, the background VMM mapper thread (one pthread per APU, started by
`db_vmm_arena_alloc` when `first < arena`, released by `db_vmm_bg_release` from `rns_init`).

Traces symbolised: s32/w_p0 r214 B, s32/w_p4 r30 B, s33/w_p0 r167 A (s32/w_p2 r166 B has only the handler frame; same binary,
`ntt-wt/s32`). Frames (non-PIE ecalc, `addr2line`): `0xa69b3b` = return address after `call hipMemMap@plt` in `vmm_map_run`;
`0xa6a292` = return after `vmm_map_run` in `vmm_bg_map`; then libc `start_thread`/`clone`. In `libamdhip64.so.7` (ROCm 7.2.4,
stripped; no exported symbol nearer than `hipHccModuleLaunchKernel`/`hipGetDeviceProperties`): fault at `+0x3f8624`, via
`+0x403b0d`, `+0x3cbd39`, `+0x8cf57/+0x8d21d`, `+0x36bb8e` (internal runtime frames below the `hipMemMap` entry). So the fault is
in the HIP runtime's map path, in a non-main thread, at about 42 s wall (when the mapper runs against init / level 1 work of the
main thread and the SHMEM/libfabric registration).
Consequence: `ECALC_VMM_SAFE=2` (a tail-of-thread fix) cannot help; the fault is in the mapping itself, concurrent with other HIP/driver use.

**Switch (off by default, digits unchanged): `ECALC_VMM_BG=0`** (branch `s34`, ecalc/dbig.c, one README row): the whole arena is
mapped synchronously in the calling thread in `db_vmm_arena_alloc`; no `vmm_bg_map` thread is created. Cost: the mapping time
the thread hid in init (modelled: the +8 s of Phase 14 R1 and more at 64 GB per APU).

**S34 soak** (`~/s34`, `s34_batch.sh`): arms A = base, B = base + `ECALC_VMM_BG=0`, 6.441e10 digits per node, `ECALC_SEGV_TRACE=1`;
10-node hold chain 12331 -> 12389 as 5 two-node pairs, 3-node chain 12377 -> 12390 as one pair plus one single-node control;
segfaults counted from the log text (srun reports rc 1, not 139). Digest `~/s34/results.txt`, marker `~/s34/S34_DONE`;
the batch first runs a 2-node 1e9 digits gate of the switch (`~/s34/gate.txt`).


#### Final S34 soak and ECALC_VMM_BG=0 verdict (2026-10-09; branch s34 8424ed1; 1e9 digits)

All numbers measured. Segfaults counted from the log text.

| arm | nodes | runs | segfaults | non-OK runs | mean wall |
|---|---|---|---|---|---|
| A base | 2 | 422 | 4 | 8 | 204.2 s |
| B `ECALC_VMM_BG=0` | 2 | 420 | 0 | 2 | 224.2 s (+20.0 s, +9.8 %) |
| A base | 1 | 121 | 0 | - | 118.6 s |
| B `ECALC_VMM_BG=0` | 1 | 121 | 0 | - | 120.0 s |

- **Fisher exact test, 4 of 422 against 0 of 420 segfaults:** one-sided p = **0.063**, two-sided p = **0.124** (computed
  exactly from the hypergeometric distribution, two-sided = all tables no more probable than the observed one). This is
  **suggestive, not conclusive**: the switch may remove the segfault (consistent with the root-cause finding that the fault is in
  `hipMemMap` in the background mapping thread), but four events do not reach the usual 0.05 level, and it costs +20 s (+9.8 %)
  wall on 2 nodes. It stays an off-by-default option; the user has not adopted it.
- The 2-node arm B also has 2 non-OK runs against 8 for A (not segfaults; not analysed separately).
- **How the soak ended:** early, on Oct 9 at about 16:38 EDT, when the holds were cancelled and nodes went to NODE_FAIL. The
  "Memory required by task is not available" step errors at that time were **allocation teardown, not ecalc failures**.
- **Cluster change:** aac7 now has a QOS of at most **6 nodes per user**, so 10-node runs are impossible for now.
- **Host finding, honest version:** the slow host aac7-n01 (db_from_bi upload stall 20.7-25.2 s against ~1 s, follows the
  host) is unique in the stall only. MemAvailable read on 13 nodes at 2026-10-09 08:36 EDT: seven nodes ~441 GB
  (s0b1, s1b0, s2b0, s2b1, s3b1, s4b0, s5b1) and six ~520 GB (s0b0, s1b1, s3b0, s4b1, s5b0, s6b0): low MemAvailable
  is **not** unique to s0b1n0. Questions to the admins are in internal admin note.
- **Kit rehearsal (m, 2026-10-10, aac7 job 12451, 1 node aac7-n00):** `t_nodechk` built with `hipcc -x hip` (ROCm 7.2.4) and ran: 1 GB host->device upload 0.017 s on each of 4 APUs; MemAvailable 438 GB with Cached 162.9 GB (the same node read 520 GB on 2026-10-09: MemAvailable varies with the page cache). The kit stage itself was checked by `--dry-run`, `bash -n` and a synthetic-table test, not on aac7.

#### S35 armed (2026-10-10): `ECALC_VMM_BG=2`, the background mapper under a process-wide HIP memory lock

**Fix (branch `s35`, commit 015994a, off by default, digits unchanged):** `ECALC_VMM_BG=2` keeps the `vmm_bg_map` thread (its overlap
with init) but serialises it against every HIP memory-management call of the process. `dbig.c` adds `db_hipmm_lock/unlock` (one
recursive process-wide mutex, active only when `ECALC_VMM_BG=2`); the mapper holds it around each chunk's `hipMemCreate` + `hipMemMap`
+ `hipMemSetAccess` (the zeroing `hipMemsetAsync` + sync stay outside). The new `hipmm.c` wraps, at link time (`-Wl,--wrap`, the ecalc
link only; tests unchanged), every call ecalc's objects make to `hipMalloc`, `hipFree`, `hipExtMallocWithFlags`, `hipMallocManaged`,
`hipHostMalloc/Free`, `hipHostRegister/Unregister`, `hipHostGetDevicePointer`, `hipPointerGetAttributes`, `hipMemGetInfo`,
`hipMallocAsync/FreeAsync/MallocFromPoolAsync`, the VMM calls (`hipMemCreate/Map/SetAccess/Unmap/Release/AddressReserve/AddressFree`)
and `hipStreamCreate[WithFlags]/Destroy`, taking the lock when the switch is on and calling straight through otherwise
(measured in the binary: 62 hipMalloc, 145 hipFree, the hipMemMap/hipMemCreate call sites all go through `__wrap_`). Lock order: the
pool lock, then the HIP lock; the mapper takes `g_vmm_map_mx`, then the HIP lock, and never the pool lock while holding it.
Not covered: HIP/HSA calls that the libraries make themselves (libfabric reaches HSA through dlsym; libsma imports no HIP symbol), and
a race with plain copies or kernel launches; the soak decides whether the lock is enough. Residual risk: a wrapped `hipFree` holds the lock
while it synchronises the device.

**Gate (measured, job 12452, 2 nodes x 1e9 digits, both at once):** A (base) on aac7-n10,aac7-n11 rc 0, VERIFY OK, digits
identical to `~/ref/e_1000000000.txt`, total 23.50 s; C (`ECALC_VMM_BG=2`) on aac7-n20,aac7-n21 rc 0, VERIFY OK, digits
identical to the same reference (so identical to A's digits), total 21.16 s. **PASSED.**

**Soak (armed):** one self-releasing 4-node job 12452 (`~/s35/s35_job.sbatch` runs `s35_batch.sh` directly, 14 h limit, excludes
aac7-n61 and aac7-n01); two 2-node workers (p0 A-first, p1 C-first), 6.441e10 digits per node, `ECALC_SEGV_TRACE=1`, arms
A/C alternating with every 6th run B (`ECALC_VMM_BG=0`); stops after 11 h (04:21 EDT 2026-10-11), on `~/s35/STOP`, or after 2
unhealthy-node events. Walls at 2 s resolution (S34's driver quantised them to 20 s, so its +20.0 s B-A figure is uncertain by about
one tick) plus ecalc's own `total`; the paired C-A difference is taken over consecutive A/C runs of the same worker, with a 95 % CI.
Outputs `~/s35/{results.txt,soak_stats.txt,pairs.tsv,ALERT}`, marker `~/s35/S35_DONE`. First rows (measured): A 264 s / ecalc
255.05 s, C 264 s / 255.58 s, both clean.

## S30: the reciprocal's late node, and today's two segfaults (2026-10-08; aac7 hold 12331; analysis only, nothing launched)

(former file: S30.md)

Labels: m = measured, i = inferred. Source: aac7 ~/s30, ~/s31 (logs, res/*.ts, d30_stats.txt, L1/ladder_table.txt); code at branch s30 (31091bd).

#### Design (from the s30 branch note)
- NEWTON_MN_RECIP_TS=1 (print only): one line per node with the stage times of recip_mn_body.
- NEWTON_MN_CHAIN_BCAST=1: only node 0 runs the single-node chain (recip_db2) and allgathers r0 (~270 KB); other nodes skip it. Digits identical.
- Drivers: ~/s30/s30_batch.sh (ABBA, hold 12331), ladder1n.sh (hold 12377). Build ~/ntt-wt/s30 31091bd.
- Result (m): ABBA 7 rounds, B - A = -2.70 s, CI -13.3 .. +7.9, STOP null (mean A 394.2 s, B 391.5 s).

#### Q2: why the null (m)
- Node 1 is 19.3 .. 25.0 s in the "chain" stage in all 16 runs with RECIP_TS (diag, 8 A, 8 B); nodes 0, 2..9 are 1.0 .. 1.2 s.
- In the B arm (chain on node 0 only) node 1 STILL needs 20.7 .. 24.1 s in that stage although it runs no chain, and nodes 0, 2..9 then wait 19.5 .. 23.2 s in the "bcast" stage (the allgather). So the 20 s was never the chain's compute: it moved from the first sharded exchange (addsh roff_wait) to the allgather, total unchanged: null.
- What node 1 does between the gather and the stage-end in B: db_init(&Qtop); db_from_bi(&Qtop, &hq) (the first dbig device allocation + upload of ~270 KB), bi_free. In B that is all of it; nine other nodes do the same in < 1.2 s. (i) The stall sits in db_from_bi / the first device-pool allocation on node 1, or is a host-level stall (page-cache reclaim, daemon) hitting node 1 at that moment; it is not the chain. NOT proven which.
- Rank to host (m, from srun task lines): ranks follow the sorted job nodelist, task 0..9 = aac7-n00, s0b1n0, s1b0n0, s1b1n0, s2b0n0, s2b1n0, s3b0n0, s3b1n0, s4b0n0, s4b1n0. The hold is fixed, so node 1 = aac7-n01 in every S27/S30/S31 run (i: same host every time; a host effect and a rank effect cannot be separated yet).
- MemAvailable: the batch logged only the minimum over the 10 nodes (440.4 GB at 13:34); it is NOT recorded which host. Idle snapshot now (job running, so weak): s0b1n0 191 GB, s0b0n0 260 GB, s2b0n0 186 GB; Cached 115 / 61 / 112 GB. Not enough to say node 1 is the low host.
- One node vs ten (m): S25 one-node ladder, 4e10 .. 7.64e10 digits, exponent 0.97, 14.4 s per 1e10 flat. The 10-node exponent 1.48 is therefore exchange / scale cost (skew, waiting), not compute. A 20 s per-run straggler is ~5 % of 394 s; it explains part of the skew (61 s of direct exchanges) but removing it would be absorbed by the next straggler (as with X1 rot).

#### Q1: the two segfaults (m unless marked)
| | d30_r4s2_A (s30 base arm) | x2_r3s2_B (s31 X2 arm) |
|---|---|---|
| srun line | task 5 on aac7-n21: Segmentation fault (core dumped) | task 6 on aac7-n30: Segmentation fault (core dumped) |
| time | 14:44:50 start, watchdog kill 14:45:28 (wall 42 s) | 13:33:04 start, kill 13:33:42 |
| last log line | tl 31.39 APU2 background mapping done (queued for the mapper lock 10.07) | tl 31.20 APU0 background mapping done (9.66) |
| build / switches | s30 31091bd, COMM_OFI_POOL_MB=2304 | s31 00ca629, COMM_OFI_POOL_MB=4096 (X2 arm) |

- Different hosts today, but aac7-n30 is also the host of the unexplained 2026-10-06 21:13 segfault (RESULTS 116, s18run b_run.log: srun task 3 there, host aac7-n30). Two of three on one host of ten: 3 events, so weak (p ~ 0.2 under uniform) but cheap to watch.
- Phase (m): in a clean run the next lines after the last "background mapping done" (tl ~31.5-31.9) are "bs: level 1 done" and "level 2 starts". Both crashed logs end 0.3-0.5 s before that point: the crash is at the end of the init-time VMM background mapping / the start of binsplit level 1 (tl 31-32 s, wall 42 s incl. launch), with all four APUs of several nodes in the "mapper lock" queue (8-11 s).
- Logs carry no rank prefix, so the crashed rank's last line is not identifiable; no core file in ~ or the run dirs (ulimit core is probably 0 / lost on the compute node); ECALC_SEGV_TRACE was NOT set in s30_lib.sh / s31_lib.sh (grep: no hit), so there is no backtrace. dmesg is not readable from the login node for the compute hosts here.
- Crash rate (m): 10-node logs with "(10 PEs)" today: s25 8, s27 11, s28 4, s30 19, s31 12 = 54; 2 crashes = 3.7 % (95 % CI ~0.5 .. 12.7 %); before today 0 of 59 plus the one of 10-06. The base arm (A) crashed too, so the cause is not an S30/S31 switch.
- Plausible cause (i, from reading ecalc/dbig.c vmm_bg_map / db_vmm_arena_wait): the background mapping thread (one per APU, spawned by db_vmm_arena_alloc) runs concurrently with the main thread's seeds and level 1. Unsynchronised tails: after the last chunk it does hipStreamDestroy(st) and hipSetDevice(cur) outside g_vmm_map_mx (hipMemMap/SetAccess/memset are under the mutex, stream destruction and the current-device switch are not), while the main thread starts the first device kernels of level 1; db_vmm_arena_wait only joins the thread when m >= m0, so for the parity-1 wait (m < m0) level 1 and the thread's tail overlap. Also HIP_CHECK(hipGetDevice) is read in the new thread, whose current device defaults to 0, then SetDevice(cur=0) at the end changes nothing for that thread but the thread state is shared runtime state. Candidate race: hipMemSetAccess/hipMemMap by the thread vs a main-thread kernel launch or allocation touching the arena's last chunks while mapping changes (VMM remap invalidates the range). With 40 background threads (4 per node) queued 8-11 s on the lock, the window is wide at exactly this tail. This is a hypothesis; nothing in the logs proves it.
- Segfault in libfabric/cxi or SHMEM at the same moment (comm init completes earlier, the first level-1 exchange follows) is the alternative; the backtrace decides.

#### Recommendation
1. ECALC_SEGV_TRACE=1 on every future 10-node run (cost: one getenv + handler; no behaviour change), and `ulimit -c unlimited` plus a core directory is optional. Also log `hostname` per rank in the rank prefix (srun --label) so the crashed rank's last lines are known.
2. If the next trace shows vmm_bg_map / HIP frames: join the thread (or hold g_vmm_map_mx) around hipStreamDestroy/hipSetDevice and make db_vmm_arena_wait(parity-1) not return before the thread has left its mapping loop; switch DB_POOL_VMM_BG=0-like fallback if such a switch exists (the init pays +8 s).
3. Node 1: next experiment is a host-vs-rank test (two runs, same build, S30 switches RECIP_TS=1): (a) permute the nodelist so aac7-n01 becomes rank 7 (srun -m arbitrary or sbatch-ordered -w list), (b) as is. If the 20 s follows the host: a slow host (page cache drop / drop_caches / echo 1 > cache evict before the run, MemAvailable per host logged). If it follows rank 1: code (rank-specific first allocation). Add timestamps around db_init / db_from_bi / the first pool_get in recip_mn_body (print only) and NEWTON_MN_RECIP_TS=1 on all arms.

#### RESUME
Collect nothing more on aac7 for this note. Next: implement the print-only timestamps (db_from_bi stage split) behind NEWTON_MN_RECIP_TS=2, run the permuted-nodelist pair with ECALC_SEGV_TRACE=1.

## S16 — the intermittent startup segfault of ecalc under Cray OpenSHMEMX on aac7 (Phase 16 S)

(former file: S16.md)

Branch `p16-S` from `main` 3eb531f. Times Eastern (aac7 is PDT = Eastern − 3 h). Every number **measured** unless marked.

#### 1. The symptom (results/AAC7_PORT.md §A16 step 7)
2 real nodes, `mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 … ./ecalc 10000000000`: task 1 segfaults
inside Cray's `shmem_init_thread` in 3 of 23 launches (after `bs: arenas 45.1 GB allocated`, before `comm_shmem: shmem, 2 PEs`);
0 of 24 at 10⁹ / t_comm. PE 0 then hangs in PMI (`_pmi_network_allgather failed`). libsma installed its own SIGSEGV handler over A's
preload, so no backtrace.

#### 2. The code read (what differs between 10⁹ and 10¹⁰ at the moment of shmem_init_thread)
`ecalc.c`: `rns_init(pool_log)` runs first; inside it the overlap hook (`binsplit_seeds_begin`, `ECALC_OVERLAP=1` default) lays the
regions out, **maps the VMM arenas** (`binsplit_pregrow` → `arena_get` → `db_vmm_arena_alloc`: 45.1 GB at 10¹⁰, 8.6 at 10⁹) and
**starts the seed thread** (`pre_seeds_run`: kernels + DMAs on the four APUs), then the plane pools map. Only then
`binsplit_shmem_pool_rule` + `mn_init` → `comm_shmem_init` → `shmem_init_thread(SHMEM_THREAD_MULTIPLE)`. So at 10¹⁰ Cray's init
(PMI all-gather, libfabric/cxi NIC setup, the hugepage heap) runs in a process with ≈ 45 GB of `hipMemAddressReserve` ranges and a
busy GPU-DMA thread; at 10⁹ the seeds are done and the ranges are small.

#### 3. The switch: `COMM_INIT_EARLY=1` (ecalc.c, b658922)
The pool rule and `mn_init` before `mem_sampler_start` / `rns_init`; the self-tests stay after `rns_init` (they need the NTT contexts).
`binsplit_shmem_pool_rule` needs only N and the size (`MN_PLAN_ONLY` runs it with no device); `mn_init` needs the pool size it sets
(`COMM_SHMEM_POOL_MB`, `COMM_SHMEM_STAGE_SLOT_MB`) and HIP (streams, `hipHostRegister`), nothing of `rns_init`. Off by default.

#### 4. Experiments (aac7, job 12181, 2 nodes; `~/p16/S/`)
Tooling: `ecalc/tools/segv2.c` — a preload that interposes `sigaction`/`signal` so the library's SIGSEGV handler is recorded and
chained *after* ours prints the backtrace and the `/proc/self/maps` lines around the fault address. `~/p16/S/sloop.sh <batch> <rounds>
<name=ENV>…` interleaves the variants round by round, every launch `timeout 300`, logs `~/p16/S/<batch>/`, summary `<batch>.summary`.

##### 4.1 Batches b1 / b2 (job 12181, aac7-n30 + aac7-n31, 03:47–05:02 EDT), 10¹⁰, 2 nodes, `timeout 300`

`~/p16/S/sloop.sh b1 10 "ctrl=" "early=COMM_INIT_EARLY=1" "vmm0=DB_POOL_VMM=0"` then `sloop.sh b2 10 "ctrl=" "early=COMM_INIT_EARLY=1"`
(the launch line of §1 with `ECALC_VERBOSE=2`, no checkpoint; every launch 58–71 s wall, `total` 53–58 s):

| variant | b1 | b2 | segfaults / launches | digits (first OK run vs `ref_1e10.txt`) |
|---|---|---|---|---|
| control (default order) | 1 of 10 (`ctrl_6`) | 0 of 10 | **1 / 20** | identical |
| `COMM_INIT_EARLY=1` | 0 of 10 | 0 of 10 | **0 / 20** | identical |
| `DB_POOL_VMM=0` | 0 of 10 | — | **0 / 10** | identical |

`ctrl_6` (04:17 EDT): **both tasks** segfaulted 12 s in, after `bs: arenas 45.1 GB allocated` on both nodes and rank 0's `comm_shmem pool:
… holds the modelled need 8704 MiB` line, i.e. inside `shmem_init_thread`; `code 1` (SEGV_MAPERR) at **address `0xffffffffffe00000`**
on both nodes, in a **non-main thread** (task 1: pid 85568, tid 85888; task 0: pid 85169, tid 85390). That address is `MAP_FAILED`
((void *)-1) rounded down to a 2 MiB boundary: an unchecked `mmap` failure of a hugepage-sized (or hugepage-aligned) region that the
code then touched. The first preload's `backtrace()` re-faulted on that thread's stack (64 recursive frames: `_Unwind_Backtrace` →
handler → …), so the frames of the faulting thread were lost; the hardened preload (rip + `dladdr` + the maps lines before any
unwinding) is in for b3/b4. The library's own handler: `sigaction(11)` with `SA_SIGINFO` installed once per process in the wrapper
chain (and ecalc's: libsma/PMI), recorded and chained after ours.

After `ctrl_6` the sloop cancelled the step (not the job) and the remaining 14 launches of b1 and all 20 of b2 passed on the same
allocation — agent C's trap (a killed step leaves the node refusing later SHMEM launches, "Error configuring interconnect" /
`PMI2_Init failed`) did **not** bite here; the b1 `failed` counts are 0.

##### 4.2 Step 3 on job 12181 (`~/p16/S/step3.sh`, 05:02–05:31 EDT)
- 2 × `ecalc 10⁹` on 2 nodes with `COMM_INIT_EARLY=1`: VERIFY OK on both nodes, `total` 16.32 / 16.31 s, digits **identical** to
  `ref/e_1000000000.txt`.
- `./mnaccept.sh 12181 --only unit,mn` (without the switch): mn e8 sizes 2/3/4 and e9 sizes 2/4 PASS (identical); **the 9 unit steps
  failed with `execve(): 192: No such file or directory`** — `mnaccept.sh`'s `MNRUN_CPUS_PER_TASK=auto` runs `scontrol show node $NODE`,
  which on a 2-node job prints one `CPUTot` per node, so `R()`'s srun got `-c "192\n192"`. Fixed (bc17d32: `| head -1`, a one-line edit
  outside my files); `t_mn_grid (2 procs)` hit its timeout in that run (rc 124) — rerun pending in chain2 (`--only unit,e9,mn` with
  `COMM_INIT_EARLY=1` in the environment, to show the switch is a no-op for single-process runs: `mn_init` returns 1 without
  `COMM_SIZE`, the pool rule returns 0 below size 2).

##### 4.3 Batches b3 / b4 and the mnaccept rerun (job 12191, aac7-n51 + aac7-n60, from 11:39 EDT)
`sloop.sh b3 25 "ctrl=" "early=COMM_INIT_EARLY=1"` (ROCm 7.0.3 build `~/ntt-S`). Two more control segfaults, both task 1
(aac7-n60), both after `bs: arenas 45.1 GB allocated` on both nodes + rank 0's `comm_shmem pool:` line, i.e. inside
`shmem_init_thread`; PE 0 hung afterwards and the `timeout 300` ended the launch (rc 124). The next launch on the same allocation
passed each time (C's trap absent again, 4 of 4 killed steps so far).
- `ctrl_8` (12:06 EDT): thread 'ecalc' tid 146908 (pid 146586), **rip = 0xfffffffffffff000** = the fault address (SEGV_MAPERR): a
  jump through a corrupt pointer; rsp in a 294 MB **rwxp** anonymous mapping (a thread stack; executable because some loaded DSO asks
  for an executable stack). The unwinder re-faulted (the depth guard stopped it).
- `ctrl_12` (12:19 EDT, the stack-scanning preload): thread 'ecalc' tid 154965, **rip = 0x21041** = the fault address — again a jump
  through garbage; the stack words above rsp resolve to **`libhsa-runtime64.so.1` (+0x1238d9, +0x12bda9, +0x12c0e8, …, +0x4efa4)
  with `libc malloc+0x99` and `libamdhip64.so.7+0x3886b0`** — a **ROCm HSA runtime worker thread** (its 294 MB stack), not libsma's
  thread, in the middle of `malloc` under the HSA runtime, jumping to a bad address while Cray's `shmem_init_thread` runs on the
  main thread.

**Diagnosis (modelled from the two frames; the b4 variant tests it).** Cray OpenSHMEMX 11.8 over libfabric 2.3.1/cxi uses the
**memhooks** memory-registration-cache monitor (`FI_MR_CACHE_MONITOR`; libfabric here carries `memhooks`, `uffd` and `kdreg2`, and
`/dev/kdreg2` does not exist on aac7, so the cxi provider falls back to memhooks). memhooks installs by **binary-patching glibc's
`mmap` / `munmap` / `madvise` / `brk` / `sbrk` / `mremap` / `shmat` entry points** (a jump written over their first bytes) inside
`shmem_init_thread`; the patch is not atomic against threads that are executing those functions at that moment. At 10¹⁰ the
seed thread and the HSA runtime's workers are mapping and freeing memory (the arenas' VMM chunks, the seed buffers, `malloc`'s
mmapped chunks) exactly then; a thread that is inside a half-patched prologue continues at a garbage address — `rip = 0x21041` /
`0xfffffffffffff000`, `malloc` and the HSA runtime on the stack. At 10⁹ the seeds are done and nothing maps during the init; with
`COMM_INIT_EARLY=1` the init runs before any such thread exists (0 of 30 + 20 launches). `ctrl_6`'s two-task fault (both at
`0xffffffffffe00000`) fits the same patch window (both PEs patch at the same moment of the PMI handshake). The counter-test:
`FI_MR_CACHE_MONITOR=disabled` (no patching; the MR cache then re-registers every buffer, a cost only for the transport's own
registrations — ecalc's pool is registered once) — variant `mhk` of b4.

b3's third: `ctrl_19` (12:40 EDT): task 1, thread 'ecalc' tid 169487, rip = fault address = `0xffffffffffe00000`, the stack words
again `libhsa-runtime64.so.1` (+0x122bf9, +0x12c447, ..., +0x4efa4) and `libamdhip64.so.7+0x3886b0`. b3: **control 3 of 25, early 0 of 25.**

##### 4.4 b4 (job 12197, aac7-n41 + aac7-n50, 12:40-13:30 EDT, ROCm 7.0.3): `sloop.sh b4 10 "ctrl=" "early=COMM_INIT_EARLY=1" "mhk=FI_MR_CACHE_MONITOR=disabled"`
| variant | segfaults / launches | digits |
|---|---|---|
| control | **1 / 10** (`ctrl_4`, 15 s in, task 1 aac7-n50: rip = fault = `0xfffffffffffff000`, stack `libhsa-runtime64` +0x122bf9 ... +0x4ef3f, `libamdhip64+0x3886b0` -- the same frame a fourth time) | identical |
| `COMM_INIT_EARLY=1` | **0 / 10** | identical |
| `FI_MR_CACHE_MONITOR=disabled` | **0 / 10** | identical |
After every killed step (5 in all today) the next launch on the same allocation passed: C's "node refuses later SHMEM launches" trap
did not occur in 5 of 5 cases here (the step was `scancel`led by step id after the `timeout`; the job kept).

##### 4.5 b5 + the mnaccept rerun at **ROCm 7.2.4** (job 12198; the user's directive through the integrator, 12:25 EDT)
Build `~/ntt-S724` (p16-S): `source aac7env.sh; module unload rocm; module load rocm/7.2.4; make -s -j32 SHMEM_CRAY=1
GMP_HOME=$HOME/gmp HIPCC=/shareddata/opt/rocm-7.2.4/bin/hipcc` — **traps**: (1) `module purge` drops the `libfabric` module and the
link of `t_comm` fails (`libsma.so: undefined reference to fi_strerror`); (2) with the module loaded but `hipcc` unset the build
used `/usr/bin/hipcc` (→ `/etc/alternatives` → 7.0.3) and the binary's RUNPATH said `/opt/rocm-7.0.3/lib`; (3) RUNPATH loses to
`LD_LIBRARY_PATH`, which the login's default `rocm/7.0.3` module sets — every launching shell must `module unload rocm; module load
rocm/7.2.4` (`~/p16/S/env724.sh`), and `MNRUN_MODULES="cray-dsmml cray-openshmemx rocm/7.2.4"` for the wrappers. Verified:
`ldd ecalc` → `/shareddata/opt/rocm-7.2.4/lib/libamdhip64.so.7`, RUNPATH `rocm-7.2.4/lib`, 0 lines of 7.0.3.
`sloop724.sh b5 10 ctrl early` (job 12198, aac7-n11 + aac7-n31, 12:47-13:19 EDT): **control 0 of 10, early 0 of 10**,
digits identical (10 launches cannot tell 0 from 10 %: P(0 of 10 | 8/78) = 0.34 -- the 7.2.4 rate is **not measured**, only bounded).
Then `chain2d.sh`: **`COMM_INIT_EARLY=1 ./mnaccept.sh 12198 --only unit,e9,mn`** on the 7.2.4 build, 13:19-14:01 EDT, 2516 s:
**15 passed, 1 failed** -- unit t_ntt/t_mul/t_bs/t_dbig/t_newton/t_verify/t_out/t_patch PASS, e9 size 1 both bases identical
(13.11 / 29.97 s), mn e8 sizes 2/3/4 and e9 sizes 2/4 identical (all nodes VERIFY OK; mn e9 size 2: 163.5 s, size 4: 181.3 s).
The one FAIL is `unit t_mn_grid (2 procs)`: rc 124 = `M 1200 2 ./tests/t_mn_grid 1 28`'s 1200 s timeout -- the test ran its first
three cases (13.1, 17.9, 38.9 s) and never finished the fourth. It runs over the **TCP transport** (`mn: ... four meshes of 2 ranks on
aac7-n11,aac7-n31 (port base 25612)`), which on a 2-node allocation crosses aac7's Ethernet between the nodes; on A's
1-node job 12167 both processes shared a node and it passed. It failed identically on 12181 **without** the switch (section 4.2), so it
is not `COMM_INIT_EARLY` (which only acts under `COMM_TRANSPORT=shmem` with COMM_SIZE > 1 -- t_mn_grid goes through `mn_init`'s
TCP branch, untouched). Open issue below: mnaccept's t_mn_grid on multi-node allocations needs `-N1` (or the SHMEM transport).

##### 4.6 The rates (measured; 10^10 digits, 2 nodes, `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1`)

| toolchain | variant | segfaults / launches | batches |
|---|---|---|---|
| ROCm 7.0.3, Cray OpenSHMEMX 11.8.0, libfabric 2.3.1 | **control** (default order) | **8 / 78 = 10.3 %** | A16 3/23 (aac7-n20+b1n0), b1 1/10 + b2 0/10 (s3b0n0+b1n0), b3 3/25 (s5b1n0+s6b0n0), b4 1/10 (s4b1n0+s5b0n0) |
| same | **`COMM_INIT_EARLY=1`** | **0 / 55** | b1 10, b2 10, b3 25, b4 10 |
| same | `DB_POOL_VMM=0` | 0 / 10 | b1 |
| same | `FI_MR_CACHE_MONITOR=disabled` | 0 / 10 | b4 |
| ROCm **7.2.4** (`~/ntt-S724`), same SHMEM/libfabric | control | 0 / 10 | b5 |
| same | `COMM_INIT_EARLY=1` | 0 / 10 | b5 |

Significance: control 8/78 against early 0/55, one-sided Fisher exact **p = 1.2e-02**; the binomial P(0 of 55 | 10.3 %) = 2.6e-03. The
10-launch variants bound their rate only (P(0 of 10 | 10.3 %) = 0.34): `DB_POOL_VMM=0`, `FI_MR_CACHE_MONITOR=disabled` and the
7.2.4 control are *consistent* with the diagnosis, not proof. Every one of the 8 control faults is at the same point (inside
`shmem_init_thread`, after the arenas and the seeds started); every one of the four with a frame is a ROCm HSA runtime worker
thread jumping through a corrupt pointer under `malloc`.

#### 5. The fix, the trap, open issues

**The fix: `COMM_INIT_EARLY=1`** (b658922; README row) -- the transport opened before `rns_init` (before the VMM arenas, the plane
pools and the seed thread). 0 of 55 against 8 of 78; digits identical at 10^9 (2 nodes, 2 runs) and 10^10 (every batch's first run,
against `~/p16/A/ref_1e10.txt`); `mnaccept --only unit,e9,mn` with the switch set: 15 of 16, the 16th (`t_mn_grid`) a harness
timeout independent of the switch (section 4.5). Off by default (the user decides); on aac7 it belongs in `aac7env.sh` / the launch
line of internal target notes section 4 once adopted. Cost: none measured (`total` 53-59 s with and without; the init moves, it does not grow).

**The trap for internal target notes section 8 (the integrator adds it)**: *Cray OpenSHMEMX 11.8 / libfabric 2.3.1 (cxi) on a node without
`/dev/kdreg2` uses libfabric's **memhooks** MR-cache monitor, which patches glibc's `mmap`/`munmap`/`madvise`/`brk`/`sbrk`/`mremap`
in place during `shmem_init_thread`. Any thread executing those functions at that instant (the ROCm HSA runtime's worker threads,
the seed thread's DMAs, `malloc`'s mmapped chunks) can continue at a garbage address: `SIGSEGV` with `rip` = `0x21041` /
`0xfffffffffffff000` / `0xffffffffffe00000` in a `libhsa-runtime64` thread; the PE hangs in PMI afterwards (`_pmi_network_allgather
failed`, `inet_recv: unexpected socket EOF` floods -- launch under `timeout`). Rate 10 % at 10^10 x 2 nodes (0 at 10^9: the seeds end
before the init). Initialize SHMEM before any device mapping (`COMM_INIT_EARLY=1`), or `FI_MR_CACHE_MONITOR=disabled` /
`userfaultfd` (0 of 10 here, not proven) -- and never call `shmem_init` from a process with live HIP worker threads.* (Diagnosis
modelled from four frames; the memhooks mechanism is libfabric's documented one -- `FI_MR_CACHE_MONITOR` in `fi_mr(3)`.)

Open issues:
- `mnaccept.sh`'s `t_mn_grid (2 procs)` times out (1200 s) on a **2-node** allocation on aac7: the TCP transport between nodes
  (Ethernet) is far slower than within a node; run mnaccept on a 1-node job there, or give the step `COMM_TRANSPORT=shmem` /
  `srun -N1`. Not a regression of this branch (12181 without the switch: the same).
- `MNRUN_CPUS_PER_TASK=auto` on a multi-node job (fixed here, bc17d32: `head -1`) -- A16's `aac7env.sh` default broke every
  `mnaccept` unit step on >= 2 nodes.
- `segv2.so` is the aac7 backtrace tool (no gdb, root-only cores): `ecalc/tools/segv2.c`; `LD_PRELOAD` it under `mnrun.sh`.
- The 7.2.4 control rate is bounded (0 of 10), not measured; 7.2.4 `total` at 10^10 x 2 nodes was 53-59 s like 7.0.3 (b5 logs).
- C's "killed step poisons the node" trap did not reproduce in 5 killed steps here (`scancel <stepid>` after `timeout 300`).

#### 6. Summary for the integrator
- Symptom reproduced at **8 of 78** control launches (10.3 %), every one inside Cray's `shmem_init_thread`; frame (4 of 4 with the
  interposing preload): a **ROCm HSA runtime worker thread** (`libhsa-runtime64.so.1` + `libamdhip64.so.7` + `malloc` on its stack,
  a 294 MB rwxp thread stack) **jumping through a corrupt pointer** (`rip` = `0x21041`, `-0x1000`, `-0x200000`) -- the signature of
  libfabric's **memhooks** monitor patching glibc's mmap family while that thread runs it.
- Fix **`COMM_INIT_EARLY=1`: 0 of 55** (Fisher p = 1.2e-02), digits identical (10^9, 10^10), mnaccept unit/e9/mn PASS but the TCP
  `t_mn_grid` harness timeout on 2-node jobs; ROCm 7.2.4 build works (`HIPCC=/shareddata/opt/rocm-7.2.4/bin/hipcc`, launch env
  `module unload rocm; module load rocm/7.2.4`), 0 of 10 + 0 of 10.

#### 5. Open issues

#### RESUME
- 2026-10-04 16:45 EDT: **all steps done**; jobs 12191 (TIMEOUT at 1:40), 12197, 12198 ended, none of mine left on aac7. On aac7:
  `~/ntt-S` (7.0.3 build), `~/ntt-S724` (7.2.4 build), `~/p16/S/` (`b1`-`b5/`, `*.summary`, `segv2.so`, `sloop.sh`, `sloop724.sh`,
  `env724.sh`, `chain*.sh`, `mnaccept2.log`), `~/ntt-S724/ecalc/results/mnaccept/12198/`. Nothing pending.
- 2026-10-04 12:30 EDT: three batches armed under setsid on aac7 — b3 (12191, 7.0.3, 25 × ctrl/early; 2 more control segfaults, the
  memhooks diagnosis §4.3), b4 (12197 pending, 7.0.3, 10 × ctrl/early/`FI_MR_CACHE_MONITOR=disabled`), b5 (12198 pending, **7.2.4**
  build `~/ntt-S724`, 10 × ctrl/early) + `chain2d.sh` (mnaccept `--only unit,e9,mn` with `COMM_INIT_EARLY=1` on 12198 after
  `b5_DONE` → `STEP3B_DONE`). Markers `b3_DONE`/`b4_DONE`/`b5_DONE` in `~/p16/S/b?.summary`. Trap hit: `pgrep -f "sloop724.sh b5"`
  matched my own ssh shell (use `^/bin/bash ./…`). Next: fill §4.3–4.5, combined rates, §5, cancel 12191/12197/12198, return.
- 2026-10-04 12:15 EDT: b1/b2/step3 done (§4.1–4.2); job 12191 (2 nodes, 1:40 h) runs b3 (25 × ctrl/early) under setsid, then
  `chain2.sh` (b4 5 × ctrl/early → `COMM_INIT_EARLY=1 ./mnaccept.sh 12191 --only unit,e9,mn`; markers `b3_DONE`, `b4_DONE`,
  `STEP3B_DONE` in `~/p16/S/*.summary`). Hardened `segv2.so` installed before b3's round 2. Next: fill §4.3, combined rates
  (A's 3/23 + mine), §5, cancel 12191 if time is left, return.
- 2026-10-04 03:45 EDT: `~/p16/S/step3.sh` armed under setsid (waits for `CHAIN1_DONE`: 2 × 10⁹ with `COMM_INIT_EARLY=1` on 2 nodes
  vs `~/ntt/ecalc/ref/e_1000000000.txt`, then `mnaccept.sh 12181 --only unit,mn`; summary `~/p16/S/step3.summary`, marker `STEP3_DONE`).
  Job 12181 estimated start 04:53 EDT (Slurm). Then: fill §4 from `b1.summary` / `b2.summary` / `step3.summary`, cancel 12181.
- 2026-10-04 03:35 EDT: switch `COMM_INIT_EARLY` written (b658922), README row; on aac7 `~/ntt-S` (p16-S) built, `~/p16/S/segv2.so`,
  `sloop.sh`, chain `chain1.sh` running under setsid (b1: 10 rounds × ctrl / early / vmm0; then b2: 10 × ctrl / early; markers
  `b1_DONE`, `b2_DONE` in `~/p16/S/b?.summary`, `CHAIN1_DONE` in `chain1.log`); job 12181 (2 nodes, 3 h, S16) pending behind B16/C16.
  Next: read the summaries, the backtrace of a segfault; step 3 (digits at 10⁹ / 10¹⁰ with the fix, mnaccept unit,mn); cancel 12181.

## G2: hang bound for the `DBIG_QSEL=1` start-up hang (aac6 CPX soak)

(former file: G2.md)

Status: armed 2026-10-12 06:30 ET (aac6, idle CPX nodes), stop launching at 09:45 ET. Labels: measured / modelled / assumed.

#### Question
TASKS item 2: `DBIG_QSEL=1` (now default) stalled once in 64 aac6 runs (a 1-node 4e9 run, after init), not reproduced in 44 repeats
(`=0`: 0/14). OPPORTUNITIES G2 asks for a bound on the hang rate. This soak measures the one-node, no-network case on current main
(a448d73); the multi-node hang found in S46 (ROCr lock vs libfabric memory hook, fixed by the uffd monitor) is a different mechanism and
is not exercised here (aac6 has no SHMEM/OFI multi-node).

#### Design
- Code: main a448d73 built on aac6 in its own clone (full git bundle; the shared aac6 clone is older), default switches.
- Size and environment: `ecalc 4000000000` (no digit output), `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30` (the CPX environment of results/KERNELS.md §S44);
  one run is about 20-26 s of wall time on a CPX node (measured, first runs).
- Nodes: the 4 SH5_MI300A_CPX nodes plus the one PPAC_MI300A_CPX node. One controller per node (`tools/g2_ctl.sh`): successive 45-min
  holds on that node, `tools/g2_node.sh` run inside by `srun --overlap`; no new run is started after 09:45 ET or when the run's
  timeout would end after the hold. Only the controller's own hold ids are cancelled.
- Per run: VERIFY OK required (ecalc's own check). Timeout = 3 x the median wall time of the OK runs so far on that node
  (floor 60 s; fixed 300 s until 3 OK runs exist).
- Classes: OK (rc 0 and `VERIFY OK`), HANG (timeout), SEGV (rc 139/135 or a segmentation-fault line), VFAIL (`VERIFY FAILED`), OTHER (any other non-zero or no verify).
- On a timeout, before killing that single PID: the last 50 log lines, `/proc/<pid>/task/*/{wchan,syscall,comm,status}`, and
  `gdb -batch -p <pid> -ex 'thread apply all bt 25'`. Kernel stacks (`/proc/.../stack`) are not readable (permission denied) and gdb
  attach is refused by the default ptrace scope; the run is therefore started through a two-line python wrapper that sets
  `PR_SET_PTRACER_ANY` and `exec`s ecalc (same PID, no change to ecalc), which was tested on a sleeping process (gdb backtrace worked).
  The capture path itself has not yet fired on a real hang.
- Outputs (aac6, own dir `~/g2/`): `<node tag>/runs.tsv` (run, time ET, rc, verify line, wall s, class, timeout s), `<node tag>/hang/run<N>/`
  (`log_tail.txt`, `proc_tasks.txt`, `gdb.txt`), `<node tag>/log/` (logs of non-OK runs only), `summary.txt`, marker `~/g2/DONE`
  (written when all controllers have finished). `touch ~/g2/STOP` stops new holds.
- Statistics to report at collection: runs, hangs, segfaults, verify-fails per node and in total; the one-sided 95 % upper bound on the
  per-run hang probability with zero hangs in n runs is 3/n (rule of three); with k hangs use the exact Clopper-Pearson bound.
  Compare with the first sighting (1/64) and the 0/44 repeat.

#### Results
(pending: filled at collection from `~/g2/summary.txt`)

#### RESUME
Collect: `ssh <cluster> 'cat ~/g2/DONE ~/g2/summary.txt'` (after 09:45 ET plus the last run, about 09:50 ET); for a hang, read
`~/g2/<tag>/hang/run*/gdb.txt` and `log_tail.txt` (cheap haiku job). Then `squeue -u` must show no `g2*` job (controllers cancel their holds), and
`rm -rf ~/ntt-g2 ~/g2.bundle` on aac6. One start-up note: the first minutes ran with an earlier version of `g2_node.sh` (no ptrace wrapper, 11 runs
on one node, all OK); the five holds were restarted at 06:34 ET with the final script.

## A114 — A1 (Phase 14, W3): the intermittent leaf / seed-stage defect — root cause, fix, soak

(former file: A114.md)

Branch `p14-A1` from main @ 61b986f. Files: `ecalc/dbig.c` (the fix), `archive:drivers/ecalc/a14_soak.sh` (the soak batch), this file.
Every number is labelled **measured** / **modelled** / **inferred from the code**.

#### 1. The evidence, re-read

| # | event | where | what was seen |
|---|---|---|---|
| E1 | job 21105, 7.70 × 10¹⁰, size 1, defaults | G13d §0 | a hang right after init: 197 threads in `futex_wait_queue`, 2 in `kfd_wait_on_events`; the next line would have been `bs: seeds were computed during init` (after `pthread_join` of the seed thread, binsplit.c 1228); 1 in 31 runs; not reproduced in 5 |
| E2 | job 21222, `mn e8 size 4` (10⁸, 4 node-processes), switches off | T114 §3.4, `archive:results_raw_2026-10-19/T114/mnaccept_21222_ef0_mn_e8_4.log` (branch p14-T1) | node 3: `mn: node 3 leaf P 18385101070989787659 limbs, Q 1466159 limbs (device)`; the level-1 product refused it; the step passed in 21149, 21215, 21218 |

The garbage length is **0xff2500040000020b** — not a limb (a base-10¹⁸ limb is < 0xde0b6b3a7640000), not a pointer, not a
small negative count: it is uninitialised memory read as a `size_t`.

#### 2. Root cause of E2 (inferred from the code; the value fits it exactly)

**Where the length came from.** At size > 1 a leaf that ends on the batch tier is copied from its region pool into device
numbers at binsplit.c 1423–1424: `db_copy(&bs_Pd, &vp); db_copy(&bs_Qd, &vq)`; ecalc.c 376–378 prints `bs_Pd.n` as `leaf P`.
`db_copy` → `shift_into` (dbig.c 663) → `db_norm(r)` → **`maxidx`** (dbig.c 805): the four quarters of the number are scanned
by one OpenMP thread each (`num_threads(DB_NQ)`), each launching `k_maxidx` on its APU, copying the per-block results to
the host with `hipMemcpy`, and taking the maximum; `r->n` is the maximum over the four threads.

**The race.** Before the fix, the host buffer for the per-block results was one process-wide `g_hred`, allocated on first use
inside `flags_reserve` (dbig.c 645–654), which each of the four threads calls at the start of its iteration:

```
    if (!g_red[d]) { HIP_CHECK(hipMalloc(&g_red[d], 228 * 8 * 8)); }        /* per quarter: fine */
    if (!g_hred) g_hred = (size_t *)malloc(228 * 8 * 8 * DB_NQ);             /* shared, unguarded: the race */
```

and `maxidx` then read the global **twice** — once as the `hipMemcpy` destination (817) and once as the source of the scan (818):

```
    HIP_CHECK(hipMemcpy(g_hred + d * 228 * 8, g_red[d], blocks * 8, hipMemcpyDeviceToHost));
    size_t m = 0; for (unsigned i = 0; i < blocks; i++) if (g_hred[d * 228 * 8 + i] > m) m = g_hred[d * 228 * 8 + i];
```

Two threads that both see `g_hred == 0` (they enter `flags_reserve` together; their `hipHostMalloc`/`hipMalloc` calls before
the test serialise in the runtime, so the two tests can fall within microseconds of each other) both `malloc`; the second
assignment lands after the first thread has already copied into the first buffer — or between that thread's line 817 and
line 818 — and that thread scans the **other, never-written buffer**: heap garbage as a length. Not zeroed, so any bytes a
freed allocation left behind: `0xff2500040000020b`.

**Why it is the leaf, why P and not Q, why size > 1, why rare.** `maxidx` is the first `dbig` operation of a node-process at
size > 1 (the `mn` self-tests do not use `dbig`; the batch tier's lengths come from `k_norm` in rns_mul.c), so the leaf
hand-over's `db_copy(&bs_Pd)` is the *first-use* call: P is computed first and got the garbage; Q's `db_copy` came second, when
`g_hred` was set once and for all, and was right. At size 1 the first `maxidx` runs inside the device-tier top level with the
same four threads (the same race is possible; its symptom would be a wrong length of a top-level product: `VERIFY FAILED` or a
refused operand — not seen in the 31 + runs of Phase 13d, consistent with the low rate). Every later call is safe: the race is
only at the first use, and only when the four threads reach the `if (!g_hred)` within the window of one `malloc`, which the
runtime's serialisation of the preceding allocations makes unlikely — hence 1 failure in ~10 executions of that step.

**Not the Phase 12 copy-order race** (a same-device D2D `hipMemcpy` returning early): the D2H copy into pageable memory here is
host-synchronous (every later `db_norm` in every run depends on it and the digits are always right).

#### 3. The fix (dbig.c, commit on this branch)

`maxidx` copies each quarter's per-block results into a **buffer of its own thread** (`size_t hred[228 * 8]` on the stack, 14.6 KB)
and `g_hred` is gone. In addition `maxidx` aborts with a message if its result exceeds the number of limbs scanned (a length can
never be larger than its input; the only way is a corrupted reduction), so a recurrence of this class fails loudly and at once
instead of propagating garbage. No change to any computed value: bit-identical digits by construction (the reduction is the same,
only its host buffer moved).

#### 4. E1, the hang after init (open — needs the node)

The thread census (197 futex, 2 `kfd_wait_on_events`) with the main thread at the seed thread's `pthread_join` (binsplit.c 1228)
means: the main thread and both OpenMP teams idle in futexes, one application thread waiting on a GPU signal (the other
`kfd_wait_on_events` waiter is ROCr's async-event thread, which always sits there). The seed thread's only signal waits are
`mem_dev_copy_wait` (mem.c 220: `hipStreamSynchronize` on the per-device non-blocking copy stream) at binsplit.c 1117 / 1137, for
the ≤ 2 chunks it computed into the pinned buffers before the region pools existed and DMAs at the end of the stream. So the
hang is that DMA's completion never being observed (a lost signal, or a wait on a stream whose device context differs from the
issuing thread's: `mem_dev_copy_wait` does not `hipSetDevice(dev)` before waiting — checked next on the node). The plan: soak
with `a14_soak.sh` in gdb launch mode (`g1:` specs) so a hang yields every thread's stack, then fix from the stack.

#### 5. Soak (measured; `a14_soak.sh`, s24-26, one job at a time, logs in aac6 `~/A114/`)

| batch | job | binary | specs | ok | failed | DIFFERS | hung | time |
|---|---|---|---|---|---|---|---|---|
| b0 | 21226 | **before** (61b986f, `~/ntt`) | mn 1e8 size 4 ×100, size 3 ×30, size 2 ×10 (budget) | **140 / 140** | 0 | 0 | 0 | 1269 s |
| b1 | 21228 | **after** (this branch) | the same | **140 / 140** | 0 | 0 | 0 | 1268 s |
| h1 | 21230 | after, **under gdb** (`g1:`) | `t_hred_race` ×1; 1e9 size 1 ×60; 7.7e10 size 1 ×8 (the hang's size, no reference: VERIFY OK) | (running) | | | | |

The before-fix rate on s24-26 is therefore < 1/100 at size 4 (T1's failure was 1 in ~4 executions of the mnaccept step on
s24-16, during a batch that had `MN_TREE_EARLY_FREE` runs and the recurrence thread's 48 OpenMP threads competing for the
cores — the preemption the race needs). The race is a scheduling event, so the mechanism is measured directly instead:
`tests/t_hred_race` replays the old pattern (four threads, the unguarded first-use `malloc`, the global read twice) 20 000
times and counts the rounds with two allocations and the garbage reads, against the per-thread buffer (§6).

#### 6. `tests/t_hred_race` — the mechanism measured (job 21230, s24-26, 4 APUs)

The old pattern replayed 20 000 rounds (four threads; each: `if (!g) g = malloc()`, `hipMemcpy` of its device results to
`g + d·K`, the scan reading `g[d·K + i]` through a second read of the global):

| pattern | rounds with more than one allocation | garbage reads (the scan did not see its own copy) |
|---|---|---|
| old (`g_hred`) | **9 884 of 20 000** | **29 402** (first garbage value 0x7f773eddd900: a freed chunk's pointer, i.e. heap contents) |
| fixed (a per-thread buffer) | — | **0** — VERIFY OK |

In the replay the four threads arrive at the test together, so the race fires in half the rounds; in `ecalc` each thread first
does its own `hipHostMalloc` ×2 + `hipMalloc` inside `flags_reserve`, which the runtime serialises, so the threads reach the test
staggered by the allocation time and the race needs a preemption in the window — rare, as observed (§5), and a scheduling
event, which is why the soak on an idle node never shows it while a loaded batch on s24-16 did.

#### RESUME (updated 2026-09-24, 22:25 EDT)

**Done**: root cause of the garbage leaf length (§2); fix in `ecalc/dbig.c` (§3); `archive:drivers/ecalc/a14_soak.sh`; `MEM_COPY_WAIT_POLL` diagnostic
(mem.c). aac6: `~/ntt-A114` on `p14-A1` @ e1b8c6b built (ecalc + tests); the soak script also copied to `~/ntt/ecalc` (61b986f's binary =
"before"). Route: the protocol's direct `sshpass ... aac6` (the ProxyJump host is down).
**Running**: batch b0 (job 21226, s24-26, 21:20 CDT, the BEFORE binary): `mn:4:1e8 ×100, mn:3 ×30, mn:2 ×30`; log `~/A114/b0.txt`, runs in
`~/A114/b0/`. Self-cancels by ~21:47 CDT.
**Next, in order**: (1) batch b1 = the same specs from `~/ntt-A114/ecalc` (the fixed binary) on the same node; (2) batch h1 for the hang:
`./a14_soak.sh h1 g1:1000000000:60:150 g1:77000000000:8:400` (gdb launch; a hang prints every stack into `~/A114/h1/<run>.log`);
(3) `./mnaccept.sh $J --only unit,e9,mn` and one 4 × 10¹⁰ identical to `~/ntt/ecalc/results/e_4e10.out`; (4) fill §5.

## S18W — `tools/rundriver.sh` and `ECALC_SEGV_TRACE` (branch `s18-w` off `main` ef22f22)

(former file: S18W.md)

Two independent, off-by-default pieces; digits are unchanged (task 2 never touches a code path a normal
run takes unless `ECALC_SEGV_TRACE=1` is set, and then only adds a signal handler and its printing before
the process dies the same way it always did). No compute-node work was done for either task (no srun, no
sbatch); jobs 12287/12294/12331/12332, `~/ntt-acc`, `~/s18sweep` were not touched.

#### 1. `tools/rundriver.sh`

A sourced bash library for the aac7 fire-and-forget drivers (`rd_init`, `rd_run`, `rd_health`, `rd_disk`,
`rd_verify`, `rd_peaks`); `tools/parse_mem.py` is a byte-for-byte port of `~/b7v17/parse_mem.py` (read over
`sshpass -p <cluster-password> ssh -o ConnectTimeout=20 aac7`, read-only there), used by
`rd_peaks`. `~/s18ab3/drive.sh` (same host, read-only) was read in full for its three lessons (segfault
left a step hung 25 min; a 1x timeout killed a healthy slow write; the NFS home at 96%) and for its
`run_killable`/`node_health`/`crash_detail` shape, which `rd_run`/`rd_health` generalize into the library;
`rundriver.sh` itself never touches a job id with scancel or any signal but to the one PID it started.

##### API (see the header comment in `tools/rundriver.sh` for the full text)

- `rd_init <outdir> <marker-name>` — makes `<outdir>/log` and `<outdir>/summary.txt`, installs an EXIT trap
  that writes `<outdir>/<marker-name>` with `$RD_VERDICT` (default `"FAILED: driver ended unexpectedly"`;
  the caller sets it to `"SUCCESS"` or `"FAILED: <reason>"` before returning). `rd_say` lines (Eastern time)
  go to both the summary file and stderr.
- `rd_run <label> <expected_s> <cmd...>` — backgrounds `<cmd...>`, log at `<outdir>/log/<label>.log`.
  Timeout = `max(2*expected_s, expected_s+900)`. A 20s-period watchdog ends the step (SIGTERM, 60s grace,
  then SIGKILL — never scancel) and writes `<outdir>/ALERT` (label, reason, last 20 log lines) the first
  time any of: the log matches `Segmentation fault|core dumped|srun: error: .*(Exited with exit code
  [1-9]|Killed)|CANCELLED|fatal|abort`; the log has not grown for `STALL_S` seconds (env, default 900)
  while the command is still alive; or the wall timeout is reached. On a plain finish it returns the
  command's own exit code and leaves `ALERT` alone. Sets `RD_LAST_RC`/`RD_LAST_WALL`/`RD_LAST_PID`/
  `RD_LAST_LOG`.
- `rd_health <jobid> <node> [<node> ...]` — per node, over `srun --jobid=<jobid> -N1 -w <node> --overlap`
  (read-only, never allocates, never cancels): no leftover `ecalc`/`t_*` process of ours, and
  `MemAvailable` readable; non-zero if any node fails either check.
- `rd_disk <path> <min_GB>` — non-zero if `<path>`'s filesystem has fewer than `<min_GB>` GB free.
- `rd_verify <log>` — prints `VERIFY OK` / `VERIFY FAILED: <reason>` from what the ecalc log itself says
  (`VERIFY OK` / `all N nodes: VERIFY OK`, else the first error-looking line); it never builds or compares
  against a reference of its own, which is exactly where s18ab's false DIFFERS came from.
- `rd_peaks <log> [<layout-file>]` — `tools/parse_mem.py`'s per-node peak lines for a `MEM_REPORT_DEVS`
  log, plus the layout file's `vslot:` line (a `BS_LAYOUT_ONLY`/`MN_PLAN_ONLY` run) if given, for comparing
  measured peak against modelled layout.

##### Unit tests (local, fake commands; `bash -n tools/rundriver.sh` clean; no shellcheck binary on this box)

| case | command | result | detection latency |
|---|---|---|---|
| a plain `sleep 3` (expected 3s) | finishes normally | `rc=0`, no `ALERT` | n/a (finished before first 20s poll elapsed; wall reported 20s, i.e. one poll period, well under 2 min) |
| prints `Segmentation fault (core dumped)` then `sleep 999` | watchdog crash-regex match | killed (`SIGTERM`→143), `ALERT` written with the line | **22 s** |
| silent `sleep 999`, `STALL_S=30` | watchdog stall match | killed (`SIGTERM`→143), `ALERT` written | **63 s** |

All three complete in well under the 2-minute requirement. Sample `ALERT` from the stall case:

    label=t3_stall reason=no log growth for 30s (STALL_S) pid=2381459 rc=143 wall=63s
    -- last 20 lines of .../log/t3_stall.log --

`rd_disk /tmp 1` and `rd_verify` against a `VERIFY OK` log and a log with neither `VERIFY OK` nor a
recognized error line were also exercised locally and returned the expected rc/text.

#### 2. `ECALC_SEGV_TRACE` (default 0)

New files `ecalc/segv_trace.h` / `ecalc/segv_trace.c`, wired into `ecalc/Makefile` (`segv_trace.o` added to
`OBJS`, built like `fatal.o` via `$(HIPC) -c -o $@ $<`) and called from `ecalc/ecalc.c`'s `main`
(`ec_segv_trace_install()`, placed right after the `argc` usage check, i.e. after `COMM_RANK` is set either
by the launcher's environment or by the `MN_PLAN_ONLY` branch's `setenv`).

`ECALC_SEGV_TRACE=1` installs a `SIGSEGV`/`SIGBUS`/`SIGABRT` handler. On a signal it writes, with `write(2)`
only (no `printf`/`malloc` inside the handler — rank, host, `/proc/self/exe` and a throwaway `backtrace()`
call are all cached or warmed up at install time instead, exactly so the handler's own first use of
`backtrace()` can't trigger glibc's lazy-init `malloc`): the rank/host, the signal name+number, the
faulting address (`siginfo_t::si_addr`, SIGSEGV/SIGBUS only), a `backtrace_symbols_fd` dump of the raw
return addresses, and the `/proc/self/maps` lines whose path is `ecalc`'s own executable (for an offline
`addr2line -e ecalc <addr - load base>` pass; `-g` is already on `HIPFLAGS`). It then restores `SIG_DFL`
for that signal and `raise()`s it again, so the process dies exactly as it would with the switch off (same
exit status, same core-file behavior) — the only change is the printing.

**`-rdynamic` was NOT used.** It only changes the dynamic symbol table (no effect on codegen, numerics or
performance), but ecalc links through `hipcc` with HIP device code and vendor comm libraries already on
the line, and `-rdynamic` was unnecessary to get a useful trace: `backtrace_symbols_fd()` already resolves
whatever the existing dynamic table carries, and the `/proc/self/maps` text-segment line plus raw addresses
(the task's own documented fallback) is enough for an offline `addr2line` pass. Verified locally (see
below) that the trace is still useful without it — only `ecalc`'s own frames print as `bin(+0xNNNN)`
instead of `bin(func+0xNNNN)`; library frames still resolve by name either way.

No HIP toolchain is available in this agent's sandbox, so the handler was unit-tested with a tiny
host-only `gcc` harness (`ec_segv_trace_install()` + a null-pointer write), both with and without
`-rdynamic`, standing in for the task's aac7-login-node, `ECALC_SEGV_TEST=1`, no-GPU test:

    == default (ECALC_SEGV_TRACE unset) ==
    Segmentation fault (core dumped)      # exit 139, no trace printed -- confirms the default build
                                           # behaves identically with the switch off
    == ECALC_SEGV_TRACE=1 ==
    ecalc: SEGV [rank 0/1 host littleblue] signal SIGSEGV (11) addr 0x0
    ecalc: SEGV: backtrace of the faulting thread (raw return addresses; resolve offline with the maps line below):
    ./.segv_harness(+0x173f)[0x5d8ae33cd73f]
    /lib/x86_64-linux-gnu/libc.so.6(+0x45330)[0x7ed669c45330]
    ./.segv_harness(main+0xf)[0x5d8ae33cd26f]
    /lib/x86_64-linux-gnu/libc.so.6(+0x2a1ca)[0x7ed669c2a1ca]
    /lib/x86_64-linux-gnu/libc.so.6(__libc_start_main+0x8b)[0x7ed669c2a28b]
    ./.segv_harness(_start+0x25)[0x5d8ae33cd2a5]
    ecalc: SEGV: /proc/self/maps lines for .../.segv_harness (offline: addr2line -e <binary> <addr - load base>):
    5d8ae33cc000-5d8ae33cd000 r--p 00000000 08:05 3148489 .../.segv_harness
    5d8ae33cd000-5d8ae33ce000 r-xp 00001000 08:05 3148489 .../.segv_harness
    ...                                                     # exit 139 after the trace -- same rc as default

    == ECALC_SEGV_TRACE=1 ECALC_SEGV_TEST=1 ==
    (same shape; backtrace shows ec_segv_trace_install -> main, since the raise happens inside install())

    == ECALC_SEGV_TRACE=1, built WITHOUT -rdynamic ==
    ecalc: SEGV [...] signal SIGSEGV (11) addr 0x0
    ./.segv_harness2(+0x173f)[...]        # the binary's own frame: address+offset only, no function name --
                                           # still enough with the maps line below for offline addr2line
    /lib/x86_64-linux-gnu/libc.so.6(+0x45330)[...]   # library frames still resolve
    ...

`gcc -O2 -Wall -Wextra -c segv_trace.c` is clean under `-DEC_FATAL_NO_HIP` (host-only compile, same macro
`fatal.c` uses for its own host-only test path). The real build (`source ecalc/aac7env.sh; make
SHMEM_CRAY=1`, with the compute-node `ECALC_SEGV_TEST=1` run on the aac7 login node, no srun, no GPU) was
**not run in this agent's sandbox** — no network path to aac7 was exercised for building, only for the
read-only `drive.sh`/`parse_mem.py` reads noted above and in task 1 — the local `gcc` harness stands in,
compiling the actual `segv_trace.c`/`segv_trace.h` pair that ships in this branch, with the same
async-signal-safety constraints. Someone building on aac7 should run `make SHMEM_CRAY=1 GMP_HOME=~/gmp`
once with `ECALC_SEGV_TRACE` unset (confirms no behavior change — default `ecalc` run) and once with
`ECALC_SEGV_TRACE=1 ECALC_SEGV_TEST=1 ./ecalc 1000000` on the login node to see the real trace.

##### README

A `## Switches` row each for `ECALC_SEGV_TRACE` and `ECALC_SEGV_TEST` was added to `ecalc/README.md`,
right after `ECALC_FATAL_ABORT` (the other fatal-path switches).

#### Files touched

- `tools/rundriver.sh` (new), `tools/parse_mem.py` (new, ported from `~/b7v17/parse_mem.py`)
- `ecalc/segv_trace.h` (new), `ecalc/segv_trace.c` (new)
- `ecalc/ecalc.c` (include + one `ec_segv_trace_install()` call in `main`)
- `ecalc/Makefile` (`segv_trace.o` in `OBJS`, its build rule)
- `ecalc/README.md` (two new `## Switches` rows)
- `results/CRASHES_HANGS.md §S18W` (this file)

## T23 — the `DIST_LOGN_TEST` 4-node ">64 corrections" defect

(former file: T23.md)

**Question.** TASKS row 23 (results/CRASHES_HANGS.md §A05, results/GRID_GROUPS.md §G12 §5 and §6, results/NODE_SLOWMODE.md §A13 part B): at 4 nodes, 10⁹ digits,
`POOL_LOG=29 DIST_LOGN_TEST=23` stopped with "newton_mn_divmod: 65 corrections" (the guard `++nc > 64`), while
`DIST_LOGN_TEST=24` passed. Is this a defect of the sharded division's forced-grid path, and what fixes it?

**Change / branch.** None to `ecalc/`. Branch `t23`: this report and the TASKS row only. No switch.

**Verdict.** **The defect is already fixed; no code change is needed.** It was not in the division. It was the
`mdb_add_shifted` bug that commit 9a746c8 (2026-09-22, during G12) fixed: the shifted add sized its rounds from the
windows of nodes 0 … g − 1 instead of g₀ … g₀ + g − 1. On the tree's level-1 group [2, 4) it therefore ran zero rounds,
and the added operand of a gridded product (the tree's P = P_l Q_r + P_r) was never added. `DIST_LOGN_TEST=23` is the
first cap at which that level's product becomes a grid at 10⁹ over 4 nodes. At 24 it is one plane, with X added in the
CRT, so 24 passed. The wrong P surfaced as the division's correction guard. G12 recorded the failure from batches run
before its own fix, and attributed it to the sharded division. Its `MN_TREE_LOGN_TEST` workaround avoided gridding the
tree for the same reason. The exact path from the wrong P to more than 64 corrections was not traced (*assumed*: a wrong
P is not a well-formed input of the division's error bound).

The evidence is a one-commit bracket on the configuration that failed, run on G12's own harness (4 node-processes on one
aac6 node). The parent 06638dd fails and 9a746c8 passes. 9a746c8 changes one line of `ecalc/rns_dist.c`. Current `main`
passes at 4 real aac7 nodes and on the aac6 harness at every cap tried.

#### Method

- **aac6, one node, 4 node-processes** (G12's harness; `SLURM_JOB_ID=<job> MNRUN_NODES=1 ./mnrun.sh 4 env POOL_LOG=29
  ECALC_VERBOSE=2 RNS_VERBOSE=1 NEWTON_VERBOSE=1 <env> ./ecalc 1000000000`, no digit output). The check is the
  per-node `VERIFY OK` and the division's correction count. Builds were plain `make` on a compute node: OSHMEM for
  current trees, loopback TCP for the September trees (their default transport).
  Trees: `main` afc28f0; 116fab6 (the last commit with the `NEWTON_DKM` switch, to run the old one-quotient division);
  6fd0bda (G12's merge); 06638dd (9a746c8's parent); 9a746c8.
- **aac7, 4 real nodes** (one queue item, hold of 4 nodes, about 5 min used): `main` 41ee7bb built with
  `tools/gate.sh --build` (SHMEM_CRAY). The gate was run at 4 nodes with the env below, compared against the 10⁹
  reference. 10¹⁰ was checked by VERIFY only. The standard gates ran at 2 and 1 nodes.

#### Results (all measured)

| where | tree | size | env | result |
|---|---|---|---|---|
| aac6 harness | 6fd0bda (G12) | 4 | `DIST_LOGN_TEST=23` | **rc 1, "newton_mn_divmod: 65 corrections"**, reproduces G12 |
| aac6 harness | 06638dd (parent of 9a746c8) | 4 | `DIST_LOGN_TEST=23` (also with `ECALC_RES_LOG=1`) | **rc 1, "65 corrections"** (both runs) |
| aac6 harness | **9a746c8** (the one-line `mdb_add_shifted` fix) | 4 | `DIST_LOGN_TEST=23` | **VERIFY OK on every node** |
| aac6 harness | 116fab6, old division (`NEWTON_DKM=0`) | 4 | `DIST_LOGN_TEST=23` / `24` | VERIFY OK / VERIFY OK; the division's products as 3 × 4 grids with the low cut, 0 corrections |
| aac6 harness | main afc28f0 | 4 | `DIST_LOGN_TEST=23` | VERIFY OK, corrections 0/0, ecalc total 48.5 s; the tree's level-1 product 1 × 4 / 2 × 2 grids, the division's products 2 × 2 and 2 × 3 grids |
| aac6 harness | main afc28f0 | 4 | `DIST_LOGN_TEST=24` / `22` / `21` / `20` | VERIFY OK at each |
| aac6 harness | main afc28f0 | 3 / 2 | `DIST_LOGN_TEST=23` | VERIFY OK / VERIFY OK |
| aac7, 4 nodes | main 41ee7bb | 4 | `POOL_LOG=29 DIST_LOGN_TEST=23`, line none | gate PASS: VERIFY OK, digits identical, ecalc total 16.27 s |
| aac7, 4 nodes | main 41ee7bb | 4 | the same on the aac7 line | gate PASS, 13.63 s |
| aac7, 4 nodes | main 41ee7bb | 4 | `DIST_LOGN_TEST=22` / `24` | gate PASS / PASS (13.98 / 11.74 s) |
| aac7, 4 nodes | main 41ee7bb | 4 | 10¹⁰, `POOL_LOG=29 DIST_LOGN_TEST=27` (G12's other failing case) | VERIFY OK on every node, corrections 0/0, total 14.84 s |
| aac7 | main 41ee7bb | 2 / 1 | standard gate (aac7 line) | PASS / PASS (ecalc 17.93 / 12.34 s) |

Earlier checks agree: results/NODE_SLOWMODE.md §A13 part B found no failure on 6d80ef1 at 4 aac7 nodes (1e9 at caps 2^21 to 2^24,
1e10 at caps 2^26 and 2^27, old and DKM division). A05 found none at 1 node.

#### Open issues

- How a wrong tree P drives the division past 64 corrections was not traced. It no longer occurs: the cause is fixed,
  and `ECALC_RES_LOG=1` with `tree_check` would catch a wrong tree level first.
- G12.md §2, §5 and §6, A05.md and A13.md still call this a division defect. Those are historical reports; this report
  supersedes their attribution.
- The aac6 harness runs of 10¹⁰ at size 4 did not start (a Slurm launch failure on the node, infrastructure). The aac7
  10¹⁰ run above covers that case.
- W2A.md held the removal of `ECALC_RES_LOG*` until T23 was done. T23 did not need it, so that removal is unblocked.

#### RESUME

Done. Branch `t23` holds this report and the TASKS row; it is merged and removed. No code change. Nothing is running.
Raw logs: aac6 `~/t23/r1`–`r4` (`summary.txt` per batch) and aac7 `~/t23/q1` (`summary.txt`, the gate dirs). Collect
with `ssh <cluster> 'cat ~/t23/<batch>/summary.txt'` (access: see the private protocol). The aac6 clones
(`~/ntt-t23*`) were removed.

## A05 (opportunity G6): TASKS 1.6 `DIST_LOGN_TEST` "65 corrections" and 1.7 checkpoint/MN_GROUPS

(former file: A05.md)

Date 2026-10-11, aac7 one node (job 12521, cancelled), main 6d80ef1. No code change.

#### 1.7 checkpoint restart vs MN_GROUPS: already done (Phase 13 N, results/CHECKPOINT.md §N13)
`ecalc/binsplit.c` (`ckpt_sched`, `bs_ckpt_restart_scan`): tree sets are written as `ECBSCKP3` (v2 header + node count and
the MN_GROUPS level list); the restart compares and every node exits together with rc 5 (schedule) or rc 4 (node count /
other run) and a message naming the file and both sides. Old v2 sets (no schedule) are accepted with a warning. Leaf sets
stay v1/v2. Tested in N13.md at 4 and 2 nodes (mismatch refused, match identical). Nothing to add. At one node `MN_GROUPS`
is not used (no schedule), so a 1-node round trip cannot test it; the 1-node gate was run for the baseline.

#### 1.6 `DIST_LOGN_TEST` "65 corrections"
- What it is: `DIST_LOGN_TEST=<log n>` lowers the plane cap (`rns_dist.c dist_logn_max`; 2^(log n + floor(log2 g)) points per
  group) so the grid split runs at small sizes. A test switch; `ECALC_PLANE_CAP` sets it too.
- "65 corrections" is the guard `++nc > 64` in the division's correction loops (`newton_db.c` 313/364/368/505/508,
  `newton_mn_divmod` 1054/1057): the quotient was off by more than 64, i.e. the reciprocal or product was wrong. It is a wrong
  result caught by a guard, not an expected outcome. Seen only at 4 nodes (10^9 with cap 2^23, 10^10 with 2^27; G12.md): a
  pre-existing defect of the sharded division's forced-grid path, never fixed (workaround: `MN_TREE_LOGN_TEST`).
- Not reproducible at one node (measured): 10^9, `--line none`, gate.sh:
  | DIST_LOGN_TEST | result |
  |---|---|
  | 24, 23, 22 | VERIFY OK, digits identical (ecalc total 22.2, 22.8, 18.3 s) |
  | 21, 20 | FATAL `split_grid: 55555557 x 55555558 limbs` (rc 3): a clear limit message, not corrections |
  Baseline gate (aac7 line, 1 node): PASS, 13.59 s.
- The multi-node case needs 2-4 nodes (not available in this slot). Risk for real runs: low, the default cap is 2^31 and
  `ECALC_PLANE_CAP` is not set to such values; only forced-grid tests hit it.

#### RESUME
To diagnose: 4 nodes, 10^9, `POOL_LOG=29 DIST_LOGN_TEST=23`, compare reciprocal/product per level against 24 (passes).

## R — the race at the batch tier's level transition (Phase 12, PLAN.md §27 row R; D5)

(former file: R.md)

Branch `r12` (from `main` @ 7aded87; the aac6 clone `~/ntt-r`). Files: `ecalc/rns_mul.c` (the batch tiers, `rns_dpool`),
the level loop of `ecalc/binsplit.c` (`pool_get`, the odd-node copy), `ecalc/mnaccept.sh` (`--stress`), `ecalc/tests/t_copy_order.c`.
Touched outside my list, minimal and commented `Phase 12 R`: `ecalc/mem.c` (the fix: one wait in `mem_dev_copy_on`, plus the
`MEM_COPY_NOWAIT` knob for the witness runs), `ecalc/rns_mul.h` (declarations), `ecalc/Makefile` (the test's rule).

#### The transition, named

**The odd node's copy between two levels, when its source and destination are in the same region (the same APU).**
A leaf level with an odd number of nodes carries its last node up unchanged: `region_copy(NODE_P(nxt, o), NODE_P(cur, a), …)`
in the level loop, which for ≥ 2²⁰ limbs is `mem_dev_copy` → `mem_dev_copy_on(dst's device)` → one `hipMemcpy(…,
hipMemcpyDefault)` issued with the destination's device current, i.e. on that device's null stream. On this ROCm (7.2.4,
MI300A) **a device-to-device `hipMemcpy` whose source and destination lie on the copying device returns to the host at
once — the copy is only queued on that device's null stream** — while a copy between two devices is host-synchronous
(`tests/t_copy_order`, below). Nothing in the level loop waited for it, and the next level's kernels run on the four
devices' own (blocking) streams: only the copying device's stream is ordered after its null stream; **the other three
APUs' kernels are not**. Two exposures follow, both at 10¹⁰ over four processes (the odd carries there put the carried node
in the same region as its source at levels 14, 16 and 18, `place_node`'s round robin at n ≤ 16 nodes: 34 → 17 (region 3 → 3),
8 → 4 (region 0 → 0)):

1. **Level 16 → 17 (the failure seen).** The odd node 34 of level 16 (≈ 4.1 M limbs each of P and Q, 34–36 MB) is copied
   within region 3 (APU 3). Level 17 pairs it as node 17 with node 16: its Q is the shared operand *b* of both products
   (P = P₁₆ Q₁₇ + P₁₇, Q = Q₁₆ Q₁₇), read by the striped tier's scatter kernel on **all four** APUs 0.1–0.2 ms after the copy
   was issued, while the copy on APU 3's null stream is still writing it (it completes 0.2–3 ms after issue: the probe
   table below). APUs 0–2 read the old contents of that slot from the limb where their sweep overtakes the copy; APU 3's
   read is ordered and right. The four residue planes disagree from that limb on, the CRT reconstructs garbage from it to
   the top, and the carried node 8 of level 17 takes the wrong product through levels 18–20 into the leaf's top product.
2. **Level 18 → 19 (the other exposure, same mechanism, not the one caught).** The odd node 8 of level 18 is copied within
   region 0; level 19's CRT stripes on APUs 1–3 write the level's Q result over the copy's source slot while the copy may
   still be reading it (160–171 MB, completing 0.2–48 ms after issue; the CRT comes ≈ 0.2 s later, so this one needs a
   stalled copy engine — the four processes share the APUs' queues).

**Why the signature is what it is.** Every failing run (V's 5, my 2 unfixed ones) has the wrong leaf node's P wrong from a
limb 2¹⁷ + a few pages (131 072, 131 584, 134 400) and its Q wrong from that limb + 3 961 286 (±1): the stale operand is Q₁₇
from limb *j* ≈ 1 MiB into it — the scatter's linear sweep overtook the copy 1 MiB into Q (after all of P, which precedes
Q in the slot; P₁₇ is read only later, by the CRT, and is right); the level-17 product P is then wrong from *j* (P₁₆ has no
zero low limbs), Q from *j* + zeros(Q₁₆); carried to the top, P_leaf = P_A Q₈ + P₈ is wrong from *j* and Q_leaf = Q_A Q₈ from
*j* + zeros(Q_A) + zeros(Q₁₆) = *j* + 3.96 × 10⁶ (Q_A over 2.68 × 10⁸ terms is divisible by 10^(6.7 × 10⁷): 3.7 × 10⁶ zero
limbs; Q₁₆ over 1.7 × 10⁷ terms adds 2.3 × 10⁵) — the constant offset seen in every failure, on node 0 or 1 alike, and
the reason "P and Q wrong from different limbs" pointed at one wrong operand shared by both products. It is not the pool
growth: **the recipe `POOL_LOG=29 RNS_POOL1_GB=3.2213` grows nothing** (the local tier's pair tiles need ≤ 2 GiB of pool 1
and the striped tier caps its tiles by the pools; no growth line appears with `RNS_VERBOSE`, and the `RNS_POOL_GROW`
invariant below never trips on it) — what the recipe does is run 10¹⁰ over four processes on one node, where the odd
carries land in the same region and the four processes' contention stretches the copies. Any per-level probe hid it
because it synchronised the devices between the levels (V's 31/31): the copy had landed before the next level's read.

#### Evidence, run by run

`tests/t_copy_order` (job 20904, one node): a 512 MB copy issued behind a 0.2 s kernel on the copying device's null stream —
- device 0 → device 2: `hipMemcpy` returned after 0.197 s (host-synchronous); source overwritten by the source device's
  stream / destination read by a third device before the copy: 0 of 5 / 0 of 5.
- **device 0 → device 0: `hipMemcpy` returned after 0.000 s; a kernel on device 1's stream overwrote the source before the
  copy read it in 5 of 5** (the destination held the later value).
- with `hipStreamSynchronize(0)` after the copy (the fix): 0.191 s, 0 of 5 for all three cases. VERIFY OK (3 checks).

The reproducer (`v11_d5.sh <job> 13 shard`, 10¹⁰ at size 4, `POOL_LOG=29 RNS_POOL1_GB=3.2213`, `ECALC_RES_LOG_LEVEL=99`
so that no per-level probe runs; `ECALC_COPY_PROBE=1` records an event on the copying device's null stream right after the
copy and a thread polls it — no wait, no synchronisation — against the time of the next level's first kernel launch):

| batch (job) | build | runs | result |
|---|---|---|---|
| 1 (20889) | unfixed, copy probe | 13 | 12 identical; **p1shard6: node 0's leaf wrong**, P from limb 134 400, Q from 4 095 686 (offset 3 961 286). Probe: every same-region copy of levels 14, 16, 18 completed 0.2–48 ms after issue, the next level's first kernel launched 0.1–0.9 ms after issue — UNORDERED in 47 of 52 such lines over the 13 runs; the cross-device copies of levels 19, 20 complete before the call returns |
| 2 (20904) | fixed build with `MEM_COPY_NOWAIT=1` (the old copy), probe + `ECALC_B_SNAPSHOT=1` | 13 | 12 identical; **w2shard5: node 1's leaf wrong, P from 131 072, Q from 4 092 359 (offset 3 961 287); its level-16 copies completed 0.2 / 1.4 / 3.0 ms after issue with level 17 launched at 0.1–0.2 ms; the snapshot of the top level's shared operand B on every APU equals memory (the wrong data was already in the copied node: the corruption is in the copy chain, not in the top level's read) |

#### The fix

`mem_dev_copy_on` (mem.c) waits for the copy it issued: `hipStreamSynchronize(0)` on the copying device after the
`hipMemcpy`. Every caller of `mem_dev_copy` / `mem_dev_copy_on` (the level loop's odd node, the checkpoints, the leaf
dumps, the mdev staging) assumes the copy is complete when it returns; the cross-device case already was. Cost: nothing
measurable (the copies are 34–171 MB, a few ms, at four levels of a run). `MEM_COPY_NOWAIT=1` restores the old behaviour
for the witness runs only. Size 1 is bit-identical (the regression below): the wait changes ordering, not values.

Also fixed on the way (rns_mul.c, the batch-local grpB tile): the pool-1 request was `L` limbs where the tile uses
`EC_NP` planes of `L` (`db + p * L`, p < 4) — reached only with `RNS_BATCH_LOCAL_MIN ≤ 2` (V's batch 5 ran with it at 10¹⁰:
a 4 GiB tile on a 3 GiB pool, writing 1 GiB past it); the request is now `EC_NP * L`.

#### The invariant: no pool grows inside a phase

`rns_dpool` (the plane pools, rns_mul.c) and `pool_get` (the region pools, binsplit.c) abort with the memory accounting
(`mem_report("GROW")`, `mem_report_summary()`, as `mem_oom` does) when an existing pool would have to grow while a phase
is running, unless `RNS_POOL_GROW=1`. With it the growth is counted (`rns_pool_n_grow`, `bs_st.n_grow`) and logged under
`RNS_VERBOSE`. "While a phase is running" is the flag `mem_pool_guard` (mem.c): `binsplit_e` clears it around the level-0
layout — a second `binsplit_e` in one process legitimately lays its regions out again, which is what `t_bs` does — and
sets it for the level loop, where it stays for the reciprocal and the division that follow. (The first version guarded
`pool_get` unconditionally and `t_bs` tripped it on its second run: job 20944, the one failing step of that regression.)
The defaults never trip it: the whole regression (10⁹ both bases, 10⁸ sizes 2/3/4, 10⁹ sizes 2/4, the checkpoint restart,
4 × 10¹⁰) and every forced-growth-recipe run at 10¹⁰/4 ran with the invariant armed and none of them grew anything.

#### `mnaccept.sh --stress`

10 runs of 10⁹ at size 4 with `RNS_POOL_GROW=1 POOL_LOG=27 RNS_POOL1_GB=0.4 RNS_BATCH_LOCAL_MIN=2 MEM_DPOOL_FILL=1`:
pool 1 at 381 MiB. At 10⁹ over four processes every batch-local tile is capped by pool 0 (1 GiB at `POOL_LOG=27`) to
`EC_NP × Mmax × L × 8` = 1 GiB of A planes and therefore asks for 512 MiB of B planes (`EC_NP × L × 8` in grpB at the top
leaf level, L = 2²⁴; `EC_NP × Mmax/2 × L × 8` in pair mode below it) — measured from a run: the first batch level already
asks for it. So pool 1 grows inside bs at the first level (the growth line is required in every run's log, else the step
fails) and the grown pool is filled with 0xA5 first. (The first attempt, `RNS_POOL1_GB=0.8054` = 0.75 GiB, was above that
512 MiB and grew nothing: job 20937, 10 of 10 identical but 0 growths — the step correctly failed.) Every run
must be identical to the reference with all 4 nodes VERIFY OK. Opt-in like `--full`; ≈ 10 min.

So **2 failures in 26 unfixed runs** (V had 5 in 26 on the same recipe; the pooled rate is 7 in 52 ≈ 13 %), and with the
fix **42 in 42** identical:

| batch (job) | build | runs | result |
|---|---|---|---|
| f3 (20913) | fixed (`f80e517`), no probe of any kind | 14 | 14 identical, all 5 VERIFY OK lines per run |
| f4 (20917) | fixed, same | 14 | 14 identical |
| f5 (20922) | fixed, same | 14 | 14 identical |
| f6 (20956) | fixed at the final commit `fa73163` (the invariant's gating), same | 14 | 14 identical; **0 growth lines** in any run — the D5 recipe grows no pool, and the armed invariant never tripped |

At the unfixed rate (7 in 52) the chance of 56 clean runs is 0.865⁵⁶ ≈ 3 × 10⁻⁴.

#### Gate runs

- **56 of 56** forced-growth-recipe runs at 10¹⁰ over 4 processes (`v11_d5.sh <job> 14 shard`, `POOL_LOG=29
  RNS_POOL1_GB=3.2213`, `ECALC_RES_LOG_LEVEL=99` so no per-level probe runs) identical to
  `~/ntt/ecalc/results/e_1e10.out`, every node VERIFY OK, in four batches on the fixed build (jobs 20913, 20917, 20922 at
  `f80e517`; 20956 at the final `fa73163`) — the gate asked for 40 in three batches. Against 24 of 26 on the unfixed
  build in the same configuration (jobs 20889, 20904).
- **`--stress` 10/10 in three separate batches** (jobs 20940, 20942, 20943, each its own allocation, `mnaccept.sh <job>
  --only stress --stress`): 30 of 30 runs identical to `ref/e_1000000000.txt`, all 4 nodes VERIFY OK, **16 pool growths
  logged per run** (pool 1 on each APU, at four levels). A fourth pass inside the regression below: 10/10.
- **The regression** `./mnaccept.sh 20948 --full --stress` on the final build (`fa73163`): **18 passed, 0 failed in
  2096 s** — t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify (334), t_out, t_mn_grid at 2 procs; 10⁹ size 1
  base 10 (13.7 s) and base 2 (24.0 s) identical; 10⁸ sizes 2/3/4 and 10⁹ sizes 2/4 identical with every node VERIFY OK;
  the checkpoint restart identical; **4 × 10¹⁰ size 1 identical to `results/e_4e10.out`, total 79.64 s** (161 s with the
  write); the stress step 10/10. (17 steps + the new stress step; the earlier run, job 20944, was 16/17 — the `t_bs`
  invariant bug above, fixed in `fa73163`.)
- So size 1 is bit-identical at 10⁹ in both bases and at 4 × 10¹⁰, and the wall clock is unchanged (79.6 s against
  Phase 10's 83.0 / Phase 11's 81.5 ± 1.4 s at 4 × 10¹⁰).

#### Files, switches, open issues

New switches: `RNS_POOL_GROW=1` (allow in-phase growth: the stress recipe), `MEM_COPY_NOWAIT=1` (the unfixed copy, for
witness runs only), `ECALC_COPY_PROBE=1` (the odd-node copy against the next level's launch, plus the copy-vs-source
check), `ECALC_B_SNAPSHOT=1` (each APU's read of the shared operand B against memory). New test `tests/t_copy_order`
(in `make all`; needs 3 APUs, ≈ 10 s). New step `mnaccept.sh --stress`.

Open issues:
- The other exposure of the same copy (level 18 → 19, the CRT of the next level writing over the copy's source) was
  never observed failing; the fix closes it too, but it has no separate witness.
- `mem_dev_copy_on` now waits on the null stream of the copying device. If a caller ever wants the copy asynchronous it
  must use `mem_dev_copy_async` (Phase 10 H) and `mem_dev_copy_wait`, which were always the asynchronous pair.
- `ECALC_COPY_PROBE` reports only the last odd-node copy before a batch level (one per level); levels whose odd node is
  copied across devices are reported as "ordered" because that copy is host-synchronous on this ROCm.

## C314 — C3 (Phase 14): A3 clean failure from worker threads; E12 the collective budget check

(former file: C314.md)

Branch `p14-C3` from `int14c` @ 9ad8fd4. Delivery to aac6 by a bundle from 4bc48ee (clone `~/ntt-C314`).
Note: the common instructions file (`agent_common14.txt`) was not present in the scratchpad; the protocol followed is the
one of the sibling results files (R114, S114): commit + RESUME after every step, node work as unattended self-cancelling
batches (<= 45 min, one job at a time).

#### RESUME (updated 08:18 CDT) -- DONE

- **Done**: step 1 (A3 code), step 2 (E12 code), delivered to aac6 `~/ntt-C314` @ 17b5e375, `make all` clean (the only
  warning is the pre-existing `newton.h` one). Login-node sanity: `./ecalc 1000` without a GPU prints one
  `ecalc: FATAL [rank 0/1 host aac6-login, APU -1, thread 0 (tid ..), after start, rc 3]: HIP no ROCm-capable device is
  detected at rns_mul.c:130` and exits rc 3 (measured); `tests/t_comm` VERIFY OK (host-only build with fatal.c).
- **Running**: `~/C3_batch.sh` (= `archive:drivers/ecalc/c314_batch.sh`, setsid on the aac6 login node). A first job (21257) started at
  06:54 CDT on s24-30 -- a node the integrator's regression holds until ~07:40 CDT -- and was cancelled at once (2 runs in,
  results kept in `~/C314/try1/`: `a3_forced_s1` rc 3 with one FATAL line; POOL_LOG=27 at 1e9 size 1 does not trip the
  guard, VERIFY OK). Relaunched as job 21258 with `--begin=07:42` (CDT), any node, logs in `~/C314/` (`summary.txt`,
  `mnaccept.out`); self-cancels (trap + `-t 0:45:00`).
- **Job 21258 (s24-26, 07:42-08:05 CDT)**: the A3 and E12 runs (Tests below, all as intended) and mnaccept
  unit,e9,mn: **15 passed, 0 failed** (1274 s, 17b5e375).
- **Job 21262 (s24-26, 08:06-08:16 CDT)**: `mnaccept.sh $J --full --only full` (the full step needs `--full` as well as
  `--only full`): 4e10 **identical** to `~/ntt/ecalc/results/e_4e10.out` (wall total 62.95 s) + RECHECK OK: 2 passed.
- **Done.** Nothing running; no job held. Ready to merge: `p14-C3` (code at 17b5e375; later commits are results and the
  batch scripts).

#### Design

##### A3 — one fatal path (`ecalc/fatal.c`, `fatal.h`)
- `ec_fatal(code, fmt, ...)`: the first caller in the process (an atomic claim) prints one line
  `ecalc: FATAL [rank r/n host h, APU d, thread t (tid), after <phase>, rc c]: <message with the sizes>` (APU from the calling
  thread's `hipGetDevice`, rank from COMM_RANK/COMM_SIZE, phase = the last `mem_report` boundary), flushes stdout/stderr and
  leaves with `_exit(code)`: no atexit handler, no destructor of the HIP runtime runs while the other threads compute.
  Later callers block (`pause()` forever) and print nothing; a re-entry from the reporting thread exits at once; a detached
  watchdog `_exit`s after `ECALC_FATAL_WAIT` s (60) if the report blocks on a lock a parked thread holds.
- `ec_fatal_begin` / `ec_fatal_end`: the same with a report in between — the two in-phase pool guards (`binsplit.c`
  region pools, `rns_mul.c rns_dpool` plane pools) and `mem_oom` print `mem_report("GROW"/"OOM")` + the summary once.
- Codes: 3 (`EC_RC_FATAL`: a failed check, a HIP call, a transport — was exit(1)/abort()), 6 (`EC_RC_OOM`: an
  allocation failed — HIP's out-of-memory in every HIP_CHECK macro, host allocations — or a pool would grow inside a
  phase), 8 (`EC_RC_BUDGET`). `ECALC_FATAL_ABORT=1` aborts instead (a core file).
- Converted: every `exit(1)` / `abort()` in `ecalc/*.c` (176 single-line `fprintf(stderr, ...); exit/abort` sites by a
  script whose argument pattern admits no `;` outside strings, 11 by hand). Left: the main thread's usage errors
  (`exit(2)` in `ecalc.c` / `binsplit.c`, before any worker), `exit(0)` of BS_LAYOUT_ONLY, `mn_plan.c` (MN_PLAN_ONLY, no
  threads), `mn.c`'s collective BS_RESTART refusal (`exit(4/5)`, main thread, every rank together), SOS's
  `shmem_global_exit`.
- Test hook `ECALC_FATAL_TEST=1`: right after bs's seeds the four worker threads meet a barrier and call `ec_fatal`
  together.

##### E12 — `ECALC_BUDGET_CHECK=1` (`ecalc.c budget_check`, after `mem_report("init")`)
Per rank: peak = max(device in use now, model planes + arena at the run's cap) + max(model host terms, VmRSS);
to take = peak − (device in use + VmRSS). The ranks allgather (host hash, peak, device, host, to take, MemAvailable,
share override) and each applies the same rule: a node is over when its ranks' peaks sum above `ECALC_NODE_GB` (480) or
their "to take" exceeds the node's MemAvailable; a rank is over when its peak exceeds its share (`ECALC_NODE_GB` / ranks on
the node, or `ECALC_BUDGET_RANK_GB=<r>:<GB>`, a test hook). Any over → every rank prints the figures (rank 0 all over
ranks, each rank its line + one `ecalc: BUDGET CHECK FAILED ... rc 8` line), barrier, `_exit(8)`. Default off.

#### Tests (job 21258, aac6-spx-b, 07:42 CDT; all measured; logs `~/C314/*.log` on aac6)

rc per rank from a wrapper around each task (`echo C3RC rank $COMM_RANK rc $?`); "FATAL lines" counts
`ecalc: FATAL` in the whole log (all ranks); no log has `Segmentation`, `corrupted` or `core dumped`.

| run | configuration | result |
|---|---|---|
| a3_forced_s1 | `ECALC_FATAL_TEST=1` 1e8, size 1 (the four bs threads call `ec_fatal` at one barrier) | **rc 3**, 7 s, **1** FATAL line: `[rank 0/1 host aac6-spx-b, APU 0, thread 0, after init, rc 3]: ECALC_FATAL_TEST=1: a forced failure in bs's worker 0 of 4` |
| a3_forced_s4 | the same at 1e9 over 4 processes (POOL_LOG=29) | every rank **rc 3**, **4** FATAL lines (one per rank; APU 1/1/1/0 -- whichever thread won) |
| a3_guard_s4_920e7 | `POOL_LOG=27` at 9.2e9 over 4 processes, no RNS_POOL_GROW (G13d's c1: every run rc 139, `corrupted unsorted chunks`) | every rank **rc 6**, 7 s, **4** FATAL lines, one per rank: `rns_dpool: plane pool 0 on APU 0 would grow inside a phase, 0.81 -> 1.07 GB ...` followed by the GROW memory report |
| a3_guard_s4_e9 (x2) | `POOL_LOG=27` at 1e9 over 4 processes | does **not** trip the guard at 1e9 (VERIFY OK, rc 0 on all ranks, twice): the tree's leaves need the growth only at G13d's 2.3e9 per process |
| a3_guard_s1_e9 | `POOL_LOG=27` at 1e9, size 1 | no guard either (VERIFY OK) |
| e12_low_s4 | `ECALC_BUDGET_CHECK=1 ECALC_NODE_GB=10`, 1e9 over 4 | every rank **rc 8** after init, 6 s; every rank's figures: peak 84.2 GB (device 62.6 + host 21.6), node of 4 ranks 336.8 of 10 GB -- "the node's peaks exceed ECALC_NODE_GB"; one `BUDGET CHECK FAILED` line per rank |
| e12_rank2_s4 | `ECALC_BUDGET_CHECK=1 ECALC_BUDGET_RANK_GB=2:1` (only rank 2 over) | every rank **rc 8**, 5 s; each names rank 2: "1 rank over, the first rank 2 (peak 84.2 GB: device 62.6 + host 21.6; over its share of the node budget)" |
| e12_ok_s4 | `ECALC_BUDGET_CHECK=1`, default budget, 1e9 over 4 | rc 0 on all ranks, "ECALC_BUDGET_CHECK ok: 4 ranks, largest rank peak 84.2 GB, node budget 480 GB"; node 336.8 of 480 GB, to take 164.8 of MemAvailable 362.9 GB; all 4 VERIFY OK, digits **identical** to ref |
| e12_ok_s1 | `ECALC_BUDGET_CHECK=1`, 1e9, size 1 | rc 0, peak 124.6 GB (device 109.1 + host 15.6), to take 8.2 of MemAvailable 412.0 GB; VERIFY OK, digits **identical** |

Observed: at size 4 on one node each rank's plane pools are the whole of its device figure (62.6 GB at POOL_LOG 29), so
the model's sum (336.8 GB) is what four ranks on one node would claim at their peaks.

##### Gates (mnaccept on 17b5e375 = p14-C3 before the RESUME-only commits)

| step | result |
|---|---|
| unit (t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid 2 procs) | 8 PASS |
| e9 size 1, LIMB_BASE=10 / LIMB_BASE=2 | PASS identical (11.37 s / 21.83 s total) |
| mn e8 sizes 2, 3, 4; e9 sizes 2, 4 | 5 PASS, identical, all nodes VERIFY OK |
| full: 4e10 vs `~/ntt/ecalc/results/e_4e10.out` (job 21262) | PASS identical, wall total 62.95 s (152 s elapsed with the write); RECHECK OK in 48 s |
