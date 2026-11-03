# CORRECTNESS — Correctness: crashes and hangs, checkpoint, detect-and-retry

Scope: crash and hang tracks, checkpoint and recheck, the detect-and-retry (DR) mechanism. Current conclusions and live numbers (labeled m measured / mod modeled / a assumed) condensed from the former per-topic files; the removed text of each is unchanged in `archive:cleanup_1103/results/<OLD>.md`. Every former section keeps its `§ID` heading.

## CRASHES_HANGS — crashes, hangs and intermittent defects

Scope: the t_mn_grid "hangs", the multi-node segfault and hang track (ROCr vs libfabric memhooks), the startup segfault under Cray SHMEM, the leaf-length race, the batch-tier copy race, `DIST_LOGN_TEST` corrections, clean-failure and budget checks. Old item IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`; raw data and drivers are in `archive:results_raw_2026-10-19/` and `archive:drivers/`. Labels: m measured, mod modeled, a assumed, i inferred.

### Topic summary

- **Current state (check ecalc/README.md; main wins on any difference).**
  - `FI_MR_CACHE_MONITOR=userfaultfd` is the `mnrun.sh` default (S46, adopted 2026-10-11): it removed the ROCr <-> libfabric lock-order deadlock (base 22/105 hangs vs 0/104). `FI_MR_CACHE_MONITOR=disabled` gives silently WRONG exchanges and is refused by `mnrun.sh` and `target_kit.sh`.
  - `ECALC_VMM_BG` (S34): `0` maps the arena synchronously (+20 s per 2-node run, m), `2` (S35, process-wide HIP lock) was dropped as a switch value (W2A A1; it hung 9/9 in the reproducer); default stays the background mapper. The production segfault in `hipMemMap` of `vmm_bg_map` (about 1 % of 2-node runs, 4/422 m) is not reproduced; the uffd default is the working fix hypothesis (a: same cause), watch 4-node real-size runs.
  - `COMM_INIT_EARLY` (S16) was adopted as built-in and its switch removed (W2A A2): the transport is opened before any device mapping.
  - `ECALC_SEGV_TRACE=1` prints a signal-safe trace (S18W); `ECALC_FATAL_*` one fatal path and `ECALC_BUDGET_CHECK` (C314) exist, off by default.
  - Fixed defects: garbage leaf length (A114, `maxidx` race, per-thread buffer), batch-tier odd-node copy race (R, `mem_dev_copy_on` now waits), `mdb_add_shifted` round sizing (T23: the "65 corrections" of `DIST_LOGN_TEST=23` at 4 nodes was this, fixed in 9a746c8).
- **Not defects.** The t_mn_grid 2-node "hang" (A01, T13) was the TCP mesh over 1 GbE Ethernet; the mnaccept `HANG` item was the queue item's own `timeout 1500` (unit step needs about 1500 s). TCP transport no longer exists (W2B B4), so neither can recur; `t_mn_grid` now runs over xshmem.
- **Closed 2026-10-25.** E1 (7.7e10 start-up hang, 1 in 31, single sighting; the 2026-10-24/25 soaks on current code saw 0 hangs in 301 runs at 7.7e10 (95 % bound 1.0 %) and 0 in 1127 at 1e9, §A114; not reproduced, excluded at its old 3.2 %); reopen rule: a hang caught with `ECALC_INIT_TL=1` plus `eu-stack` reopens it.
- **Open.** S30's 20 s stall of node 1 (host vs rank, cause of the draw unknown, see `results/TRANSPORT.md`); G2 soak lost (superseded, §G2); the 1 % segfault is still unproven fixed.
- **Superseded.** T13's lock-order suspicion for the t_mn_grid hang (refuted by A01); G12's attribution of "65 corrections" to the sharded division (refuted by T23).

### §HANG — t_mn_grid "hang" inside mnaccept --only unit,e9 (2026-10-13)

**Verdict: not a hang.** ADOPT's two queue items ran `timeout 1500 ./mnaccept.sh <hold> --only unit,e9` and recorded rc 124; both summaries show the eight single-process unit tests passed and no `t_mn_grid` line (the 9th and last unit step, its own limit 1200 s). A repeat on a 2-node hold (main b80ee2e): `--only unit` ran all 9 steps, 9 passed, `t_mn_grid` VERIFY OK (180 checks), **1491 s total** (99 % of a 1500 s limit; `e9` adds about 1 min). Standalone `t_mn_grid` is 100-130 s (m). Rule: queue items running `mnaccept.sh --only unit[,...]` need `timeout >= 2400` and EST_MIN >= 45 (daemon cap is 1.5 x EST_MIN + 5 min); `--only grid` alone is about 2 min. Optional: print each unit step's elapsed time in mnaccept.sh.

### §S46 — crash + hang track (batch 2026-10-11, track T1)

**Results (collected 2026-10-11 09:00, R1 on aac7, 322 runs, arms interleaved; aac6 soak 223 runs; Wilson 95 % CI).**

**Recommendation for the target line (the user decided: uffd adopted).** (a) `ECALC_VMM_BG=0`: 0/104 hangs, +20 s per 2-node 1e9 on aac7 (m earlier), removes the mapper itself. (b) uffd: 0/104 hangs, wall 44.6 vs 44.7 s, removes libfabric's mmap intercept (the lock in the cycle), keeps the MR cache, digits identical. (c) both: no gain shown. Recommended (b) alone. Residual risk: the reproducer produces hangs, not the segfault, and 0 segfaults in 322 + 223 runs cannot test the shared-cause hypothesis; confirming (b) at 4 nodes and real size (rate about 1 in 26) needs 80+ clean runs (a, rule of three). Not tested: (b) at 4 nodes, real size.

### §S34 — where the rc 139 segfault is (2026-10-09)

**Finding (m, 3 independent traces, identical offsets):** the multi-node segfault is inside `hipMemMap`, called from `vmm_map_run` (`ecalc/dbig.c`) in `vmm_bg_map`, the background VMM mapper thread (one pthread per APU, started by `db_vmm_arena_alloc` when `first < arena`, released by `db_vmm_bg_release` from `rns_init`). Symbolised: `libamdhip64.so.7` (ROCm 7.2.4) fault at `+0x3f8624`, via `+0x403b0d, +0x3cbd39, +0x8cf57/+0x8d21d, +0x36bb8e`; at about 42 s wall, when the mapper runs against init / level 1 work and SHMEM/libfabric registration. Consequence: `ECALC_VMM_SAFE=2` (a tail-of-thread fix) cannot help.

### §S30 — the reciprocal's late node, and today's two segfaults (2026-10-08; analysis only)

Late node (m): `NEWTON_MN_CHAIN_BCAST` (only node 0 runs the one-node chain) was null in a 7-round ABBA at 10 nodes (-2.70 s, CI [-13.3, +7.9]); the slow chain stage follows node 1, not the algorithm (switch removed 2026-10-11). Two start-up segfaults at the end of the init-time VMM mapping, no backtrace; they lead to §S34 and §S46.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/CRASHES_HANGS.md`.)

### §S16 — the intermittent startup segfault of ecalc under Cray OpenSHMEMX on aac7 (Phase 16 S)

**Symptom (m).** 2 nodes, `COMM_TRANSPORT=shmem`, 1e10 digits: task 1 segfaults inside Cray's `shmem_init_thread` in 3 of 23 launches (after `bs: arenas 45.1 GB allocated`); 0 of 24 at 1e9 / `t_comm`; PE 0 then hangs in PMI. At 1e10 the init runs in a process with about 45 GB of VMM address ranges mapped and a busy seed thread; at 1e9 the seeds are done.
**Switch `COMM_INIT_EARLY=1`** (b658922): pool rule and `mn_init` before `mem_sampler_start` / `rns_init`; self-tests stay after. Adopted later as built-in (W2A A2 removed the switch).
**Trap for any SHMEM-on-cxi setup.** Cray OpenSHMEMX 11.8 / libfabric 2.3.1 (cxi) without `/dev/kdreg2` uses memhooks, which patches glibc's mmap family in place during `shmem_init_thread`; a thread executing those functions then can continue at a garbage address (about 10 % at 1e10 x 2 nodes, 0 at 1e9). Initialise SHMEM before any device mapping, or use `userfaultfd` (see §S46); never call `shmem_init` from a process with live HIP worker threads. The PE hangs in PMI afterwards (launch under `timeout`).
Other findings: `mnaccept.sh`'s `MNRUN_CPUS_PER_TASK=auto` broke every unit step on >= 2 nodes (`scontrol show node` printed one `CPUTot` per node; fixed in bc17d32 with `head -1`); the "killed step poisons the node" trap did not reproduce in 5 killed steps (`scancel <stepid>` after `timeout 300`); a ROCm 7.2.4 build needs `module unload rocm; module load rocm/7.2.4` at launch and the explicit `HIPCC` path (RUNPATH loses to the login's `LD_LIBRARY_PATH`).

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/CRASHES_HANGS.md`.)

### §G2 — hang bound for the `DBIG_QSEL=1` start-up hang (aac6 CPX soak)

TASKS item 2: `DBIG_QSEL=1` (now default) stalled once in 64 aac6 runs (1-node 4e9, after init), not reproduced in 44 repeats (`=0`: 0/14). The soak (armed 2026-10-12 06:30 ET on idle CPX nodes, main a448d73, `ecalc 4000000000`, `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30`, about 20-26 s per run, timeout 3x median, hang capture with gdb through a `PR_SET_PTRACER_ANY` wrapper) measures the 1-node no-network case; the multi-node S46 mechanism is different and not exercised. With zero hangs in n runs the one-sided 95 % upper bound is 3/n. **Results were never filled in** (pending at collection); the aac6 outputs are gone and the soak is superseded by the 2026-10-24 aac7 soak of §A114 E1 (main 2d54d6b, the same 1-node no-network case at 1e9 and 7.7e10: 0 hangs in 1127 + 45 runs). It is not repeated. Classes: OK, HANG, SEGV, VFAIL, OTHER.

### §A114 — A1 (Phase 14, W3): the intermittent leaf / seed-stage defect — root cause, fix, soak

**Reading of the current seed path (2026-10-24).** (a) `mem_dev_copy_wait` (mem.c) is reached only in the direct (non-VMM) seed path and calls `hipStreamSynchronize` on the stream stored for `dev`; HIP does not need the current device to match, so the missing `hipSetDevice` is not a defect found by reading. (b) The default (VMM) path uses the persistent pull kernels: `db_pull_stop_all` runs at the end of `seeds_stream` before `mem_hstage_free` (hipHostUnregister), so the ordering that hung the dropped branch is not present; the pull streams are non-blocking, so null-stream syncs do not wait on them, but `hipDeviceSynchronize` in `mem_dev_alloc` / `dpool_fill` / dbig.c (pool growth) would wait for the pull kernels until the seed thread quits them: a delay while the seed thread works, a deadlock only if the seed thread ever waited on the thread that sits in that sync (it waits only for `pools_ready`, set before the kernels start). `db_pull_wait` aborts after 600 s instead of hanging. No code change made.

## CHECKPOINT — checkpoint, restart and the verification record

Scope: checkpoint and restart of the binary-splitting phase (per-node checkpoints, hardening, restart under another MN_GROUPS) and how the project proves its digits right (§V: T1 moduli, the standalone recheck; the former TESTING_VERIF, merged 2026-10-25). | Owner of the items below (condensed 2026-10-19 from the merged per-item reports; originals in `archive:results_merged_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers are in `archive:results_raw_2026-10-19/`.

### Topic summary

- Current state on main (ecalc/README.md): only the **top-level set** exists (`ECALC_CKPT_TOP`, off by default; `ECALC_CHECKPOINT=1` makes it `=2`, the budgeted set). It holds each node's P and Q after the last tree level (`n<rank>_tree_LLL.*`, size 1 `tree_000.*`) in `BS_CKPT_DIR` or `<outfile>.top` and exists for `ECALC_RECHECK`. **There is no restart inside a run** (user decision D11, 2026-10-12): the leaf-level and tree-level restart sets of WP7 / A-ckpt (`BS_CKPT_EVERY`, `BS_CKPT_MIN_LEVEL`, `BS_RESTART`, `BS_CKPT_TREE`) were removed and archived. WP7, A-ckpt and G6 are therefore history (design and evidence for what was removed); the schedule check of N13 / G6 went with the restart.
- Live material: the top-set writer (N13 §4.1 / §1.4: background writer, budgeted `=2`, `BS_CKPT_BG_CHUNK_MB`, `BS_CKPT_BG_MBS` test knob) and the reader fix of CKPT (64 MiB reads with retries, `sp_file` in `ecalc/spill.c`).
- Restart design that was shown correct (m): snapshot of the level node table plus the used limbs of the four region pools every few levels, atomically (`.tmp`, fsync, rename, header last); restarted runs bit-identical to uninterrupted runs and to the GMP reference at 10⁸ and 10⁹, sizes 1, 2, 4 (WP7, A-ckpt). Disk-bound: ≈ 1 GB/s per node with fsync.
- `ECALC_CKPT_TOP=1` always completes the set and can stall the division: P (17.8 GB at 4 × 10¹⁰) must be on disk before S = P + Q, the reciprocal hides only ≈ 16 s, so P alone needs ≳ 1.1 GB/s (mod). `=2` drops the set when the projected wait exceeds the slack (1 s) and never stalls. On aac7's file system the writer waited 499 s for P at 3.8646 × 10¹¹ (57 GB at 56 MB/s per node, m).
- aac7 compute nodes mount the home file system NFS soft; a huge single O_DIRECT `pread` can fail with EIO under load (CKPT). The reader now reads ≤ 64 MiB per call with retries; confirmed at scale 2026-10-14 (0 retries).
- A recheck at 6 nodes / 3.8646 × 10¹¹ takes 7380 s (m); use a timeout of ≥ 3 h (TESTING.md §3). The part-file pass at 12.5 MB/s per node is the recheck's main cost (TASKS "Proposed" candidate).
- Never run at 10¹⁰ / 4 × 10¹⁰ for the removed restart; extrapolations there were by the measured per-node rate only.

### Verification summary (from the former TESTING_VERIF)

- **The "intermittent size > 1 verification failure" (D5) is closed.** (a) §V found seven of the eight T1 moduli composite (two divide every Q, so T1 was blind there) and replaced them by the first eight primes above 2^62 (digits unchanged); (b) the real fault was a timing-dependent wrong leaf P, Q on node 0 or 1 under forced growth of plane pool 1, localised in §V and fixed in Phase 12 R (the batch tier's odd-node copy `mem_dev_copy_on` did not wait for its copy; `archive:results_merged_2026-10-24/CRASHES_HANGS.md §R`).
- Standing tools: `mnaccept.sh` (the gate per TESTING.md owns the step list); `ECALC_RECHECK=1` standalone recheck of a finished run from its part files and the top-level P, Q set (§V); per-level check switches `ECALC_RES_LOG`, `ECALC_RES_LOG_LEVEL`, `ECALC_LEAF_DUMP` (debug).
- `RNS_PLANES_FIRST`: 2.01x at 1.42 x 10^11 on a node with a small 2 MiB-block supply (1349.7 -> 672.0 s, m); an adopted default. `mnrun.sh` detects SOS vs OSHMEM from the binary's ELF dependencies.
- Weak-memory audit (Phase 15): about 60 sites read, one real hazard (WAR between a sender's xGMI push and the receiver's unpack of the previous transform), fixed by an event wait (`DIST_RB_WAIT`, default on), demonstrated failing 52/52 under an induced lag (`DIST_TEST_LAG_MS`), no measurable cost.
- `mnaccept` unit tests cannot pass on a GPU-partitioned (CPX) node (about 22.9 GB per logical GPU); `t_ntt 24` always fails its speed check on CPX (hardware).

### N13 — Phase 13a agent N — checkpoint hardening (TASKS 1.7, 4.1, 1.4)

**Open issues.** Holding Q past the division keeps 17.8 GB in the block pool through the output stage at 4 × 10¹⁰ (fits; not checked at 10¹¹). Size > 1 background top set tested only at 10⁸ / 10⁹ (write 0.1 s; waits unexercised at scale). Behavior changes outside the default: a fresh run with `BS_CKPT_DIR` at size > 1 deletes its own earlier tree sets; a failed synchronous tree-set write removes partial files; the size > 1 top set is always written in the background (`BS_CKPT_TREE_BG=0` restores synchronous). Done at `6392f78`/`a7884f0`; nothing live.

### CKPT — the recheck could not read the top-level set of the 6-node 3.8646 × 10¹¹ run

**Results** (m unless marked):

**Verdict.** A reader bug (one huge read on a soft-mounted NFS), not a bad set; fixed by smaller reads plus retries, merged as a correctness fix (last code commit 544b863). A large run's recheck needs a timeout of ≥ 3 h on aac7 (TESTING.md §3).

**Open issues.** The part-file pass at 12.5 MB/s per node (a quarter of the set's O_DIRECT rate) is the recheck's main cost (TASKS "Proposed" candidate). The writer waited 499 s for P before S = P + Q at 3.8646 × 10¹¹ (57 GB at 56 MB/s per node): `=1` costs run time on this file system, `=2` drops the set instead. Nothing else open.

### V — verification and cleanup (Phase 11, PLAN.md §26 row V: D5, ECALC_RECHECK, E1 (c), E2)

##### D5 — the intermittent size > 1 verification failure
**The checker's signature was a composite-moduli artefact.** Three failures (A-mem x2, T's d5b) printed `P BAD, Q BAD, T(P+Q) == XQ+R BAD` at q0, q1, q3, q4, q5, q7 and ok at q2, q6, with wrong digits each time. PLAN §23-6 read this as "the residue path's fault, not a wrong number": wrong. **Seven of the eight "primes" in `verify.c` were composite** (2^62 + {135, 179, 183, 247, 315, 319, 349, 397}; only 2^62 + 135 prime). q2 = 2^62 + 183 and q6 = 2^62 + 349 have all factors below 2.5 x 10^7, so every node's Q(a, b) (a product of >= 2.9 x 10^8 consecutive integers) is 0 mod q2 and q6; there T1 is blind (0 == 0, X drops out, P sees only the top node's range). So a wrong P or Q from any node below the top is caught at the six other moduli and passes at q2, q6: exactly the observed pattern, i.e. a genuinely wrong bs result correctly flagged.
**Fix (verify.c, commit 66ee599):** `t1_q` is now the true first eight primes above 2^62 (2^62 + 135 ... 277). T1 numbers, `RES` logs and `.t1` sidecar residues change; digits do not (10^9 and 4 x 10^10 identical). A wrong P now shows BAD at all eight.
**The real fault, localised** (instrumentation `ECALC_RES_LOG=1`, `ECALC_RES_LOG_LEVEL`, `ECALC_LEAF_DUMP=<dir>`; reproducer 10^10 at size 4, `POOL_LOG=29 RNS_POOL1_GB=3.2213`: pool 1 at 3 GiB grows to 4 GiB at the first batch-local level; `v11_d5.sh <job> 14 shard`):

| batches | runs | result (m) |
|---|---|---|
| 1 (old moduli) | 5 | 4 identical, shard5 failed (digits differ from byte 2 287 655 016; wrong P and Q) |
| 2 (new moduli, leaf check) | 4 | 2 identical; node 0's leaf P/Q BAD in one, node 1's in another; other leaves agree with the recurrence |
| 5 (no level checks, leaf dumps) | 13 | 11 identical; two wrong leaves (node 1: P differs from limb 131 584 up, Q from 4 092 871; node 0: P from limb 131 072, Q from 4 092 358) |
| 3, 6, 7 (per-level checks from level 17 / 11; batch 7 CPU-only read) | 5 + 12 + 14 | 31 of 31 identical, every level ok |

## DR — detect and retry (C3): a faulty product repeats itself instead of ending the run

### Summary (final, 2026-11-01; branch `c3check`, tested commit 9129ac52; all numbers measured on aac7 unless marked modeled)

`ECALC_DR` (default 0 = off, the unchanged code paths) selects what is checked inside the run; a failed check repeats only the faulty piece or product, once, inside the product call (no extra memory beyond the first attempt's peak, no change of digits):

### Design summary (planning agent, 2026-11-01; code read at main c86e5fcc)

Kind (1), the CRT flag, already detects the fault; missing is recovery: the flag becomes a collective, per-piece decision inside `mn_core`, taken before any result limb leaves the PE (the scatter and the chunked rounds write C), and the piece repeats. Kind (2) needs an independent expected value (a residue computed at consumption from corrupted storage proves nothing): the 62-bit prime `t1_q[0]` (`DR_Q`) and tracked residues with exact end-to-end identities: tree products (tracked residues seeded by the T1 recurrence), the leaf (per-node recurrence), each division step (A' = X Q + R), and the flagless cyclic product (modulo B+1 and B-1; B^2-1 divides B^P-1 for even P). Rejected: coefficient sums at x = +-1 (misses most kind-1 faults, whole-limb input faults and delivery faults).

### Counting rule (step 2)

`failures` counts the PEs that failed the check of a piece, group-wide, so it is the same on every process; `injected` is local (the injections done by this process's threads); `recovered` counts those PEs once the repeat passes; the gate sums `injected` over the processes and compares: injected = failures = recovered. `checks` counts piece decisions (one `grp_max` each).

### Steps

| step | content | status | commit | gates |
|---|---|---|---|---|
| 1 | `dr.c/h`, `t_dr`, registry, this file | done | 4af77c91 | t_dr VERIFY OK (9351 checks, measured); 1-node and 2-node gates with `ECALC_DR` unset PASS, digits identical (measured) |
| 2 | A in `mn_core` (flag retry, fatal-to-flag conversions, injection `flag`) | done | 14926757 | see Measured, step 2 |
| 3 | B + C (tree residue identity, leaf check, division-start compare): `rns_mul_dist_mn_chk`, tracked residues in `mn.c`, injection `deliver`, `store`, `leaf` | done | 489e38ee, 583b6479, 1d266195 | see Measured, step 3 |
| 4 | D + D' (division `mn_dkm_low` steps 1-2, the wrap, the cyclic product; injection `xql`, `cyc`, test modifier `leafslow`) | done | df4caf7e, e7274f10, (this commit) | see Measured, step 4 |
| 5 | full regression with `ECALC_DR=3` | done | 9129ac52 | see Measured, step 5 |
| 6 | timing (ABBA), docs (ALGORITHM §10, TESTING §4, TASKS) | done | this commit | see Measured, step 6 |

### Deviations from the design

- Step 2 injection positions: `P24_CHECK_TEST_S` (the existing 0|6|12 row-class switch) positions the `flag` injection; no new switch.
- The `_PE` of `flag` is the global PE number (rho); `-1` injects on every PE of every process.
- Step 3, retry: the design re-zeroed C's share in place (`db_zero_fill`); the code frees the bad result and re-runs `mn_grid`, which allocates C again. Peak memory: the bad C is freed before the repeat allocates its replacement, so the repeat's peak equals the first attempt's (one C share plus the planes; the planes are reused, the transform cache is dropped); an in-place zero fill would have saved only the allocator round trip. Unmeasured at the memory edge (the runs here are far below it).
- Step 3, leaf: the tracked residues are seeded from the leaf by `db_mod_qs` (one extra pass over the node's P_r, Q_r at the start of the tree) and the independent check is the node's recurrence (`pq_bg_run`, the T1 values of its term range), compared as soon as the recurrence is done (non-blocking after every level and at the division's start; a recurrence still running at the division's start is not waited for, so it keeps overlapping the division: the compare is deferred to the join in `out_stage` (`mn_dr_leaf_final`, before T1), runs exactly once, and is counted in the summary line as `leaf checks deferred`; a recurrence that never started is logged and counted as `skipped`; `pq_bg.started` is stored with release), not the other way round (the design seeded from the recurrence). `ends` counts the leaf compare and the division-start compare of the tree's final P, Q (2 per node).
- Step 3, local-only fatals (leaf check, division-start compare) call `ec_fatal` on the failing node only; the other nodes are ended by the launcher's `srun --kill-on-bad-exit=1` (`mnrun.sh`, D34), the same path as every other multi-node fatal. The bench launcher `ecalc_srun` (`target/bench/items/_ecalc.sh`) lacked the option (peers would have waited for the item timeout); it now passes it (`ECALC_SRUN_KILL=0` switches it off for the independent per-node probes of `mem02.sh`). Other multi-node srun launches checked: `target_kit.sh` node check (independent probes, no collective), `remote/run/lib.sh` snapshot and monitor (independent, monitor deliberately `=0`). Not tested with a launcher without that option; the `ecalc_srun` change was checked with `bash -n` only (no bench item run).
- The persistent case (`_N=2`) prints the exit summary twice per process (the repeat's failure and the fatal path); harmless.

### Open issues

- Step 2 covers only the flag (kind 1) of the P24 piece; kind 2 and delivery faults are steps 3-4.
- Not exercised: PEs of one group with different failure counts (the cap decision is collective, so it cannot diverge, but no test builds such a history; the counts here are equal on all PEs).

### Measured, steps 1-2 (aac7, 2026-11-01; items 60_dra_x1, 63_dra_x2b, 62_dra_x3 of the DR-A series; all counts measured)

Setup names: 2p = `t_mn_grid 1 28` on 2 processes; 4p = 1 node, 4 PEs (`ECALC_PE_PER_GPU=1 COMM_TRANSPORT=xshmem XSHMEM_LOOPBACK_PES=1 ECALC_NP=4`); 4 APU threads per process, so `_PE=-1` injects 8 (2p) or 4 (4p) at once. Counts are the DR summary lines (injected summed over processes, failures and recovered as the group-wide value).

### RESUME

### Review fixes and the cap test (aac7, 2026-11-01, commit e22efde; item 70_drfix; all counts measured)

Fixes in `dr_piece_decide`: (1) the `who[640]` text is clamped after every append (a long PE list used to push `sizeof who - k` below zero); (2) the `ECALC_DR_TOTAL` decision is collective: the first `grp_max` now carries the verdict (bit 32) and the process's count of failed pieces (low 32 bits), so every PE of the group adopts the group's maximum count and takes the same branch; no collective is added, and with `ECALC_DR` unset nothing changes. New injection setting `ECALC_DR_TEST_M` (default 1): corrupt M consecutive P24 products from the K-th (each product has its own latch), so two distinct products fail in one run.

### Measured, step 3 (aac7, 2026-11-01, 16:17-16:39 ET; items 81_drb_t1, 82_drb_n2, 83_drb_n1; commits 583b647 and 1d26619; all counts measured)

`t_mn_grid 1 28`, `ECALC_DR=2` (shapes: product, product + x, subgroups, mixed). Setup 4p = 1 node, 4 PEs (`ECALC_PE_PER_GPU=1 COMM_TRANSPORT=xshmem XSHMEM_LOOPBACK_PES=1 ECALC_NP=4`).

### Measured, step 4 (aac7, 2026-11-01, 16:00-17:30 ET; items 90_drc_x1 (commit df4caf7), 91_drc_n1 (df4caf7), 92_drc_x2 (e7274f10); all counts measured, single runs)

Code (`ECALC_DR >= 3`): D around `mn_dkm_low` (`newton_db.c` `mn_dkm_low_d`, division steps 1 and 2): E = (R(S) B^e - R(X) R(Q)) - (R(Aw) - R(xql)) must be 0 or +-B^w; a mismatch frees xql, drops the transform cache and recomputes it once; a second failure, or the `ECALC_DR_TOTAL` cap, is fatal with the location. D in the wrap (`mn_dd_wrap`): exact E = 0, fatal on a mismatch (no stored input to recompute from; U faults are caught earlier by D'). D' in `rns_mul_dist_mn_cyc_v`: U == A B modulo B+1 and B-1 (the plane has an even size, so B^2 - 1 divides B^P - 1), one 6-word host all-gather, retry once. Each residue is an all-gather (`mdb_trk_make`) summed alike on every node: the verdict is collective. Cost: 3 gathers per low product, 2 per wrap step plus one for R(R1), 1 per cyclic product. Test injections: `xql` (K = the process's low product number: 1 step 1, 2 step 2), `cyc` (K = cyclic product number); `leafslow` is a modifier in the `ECALC_DR_TEST` comma list (`leaf,leafslow`): the recurrence waits for the division's start (at most 120 s), so the deferred leaf compare runs.

### Measured, step 5 (aac7, 2026-11-01, fixed commit 9129ac52; all measured, single runs)

Fixes of the review (all `ECALC_DR >= 2` only): the division flag `g_dr_div_seen` is stored before the `!g_dr.on` return; at level 3 a D that cannot be enabled (no `DR_Q` residue) prints one line on rank 0 and counts `DR_C_DIV_OFF` ("D NOT ACTIVE" in the summary); an independent check of R(R1) right after division step 1 (fatal naming the step; clean 2-node runs now show 3 division checks, not 2); `dd_step(1)` runs before the step-1 A mu product; the context is cleared at both ends of the division. Local: unittest 57 OK, switch registry 0 problems, doccheck 0.

### Measured, step 6 (aac7, 2026-11-01; 2 nodes, 128.82e9 digits, no digit output)

ABBA: one warm-up pair discarded, then 4 rounds A B B A; A = `ECALC_DR` unset, B = `ECALC_DR=3`; all runs rc 0, VERIFY OK.

### Measured, step 6 repeat: paired timing in the aac7 campaign (M6, 2026-11-02 00:33-00:48 ET, main d7c7f28, item 64_m6_dr; 2 nodes, 6.441e10 digits, measured)

ABBA, 6 pairs, A = `ECALC_DR` unset, B = `ECALC_DR=3`, all runs rc 0 and VERIFY OK. Mean A 65.18 s, mean B 64.62 s: **B - A = -0.56 s, 95 % interval [-3.52, +2.40] s** (-0.9 %, interval -5.4 % to +3.7 %). Together with the step-6 table above (+0.48 s, 4 round differences, 128.82e9 digits), the overhead of `ECALC_DR=3` is not distinguishable from zero at 2 nodes at either size; the interval is about 3 s wide, so a cost of 0.1-0.2 % (modeled) is far below what aac7 can resolve, and the 576-node figure stays modeled.
