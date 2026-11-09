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

### §S30 — the reciprocal's late node, and today's two segfaults (2026-10-08; analysis only)

Late node (m): `NEWTON_MN_CHAIN_BCAST` (only node 0 runs the one-node chain) was null in a 7-round ABBA at 10 nodes (-2.70 s, CI [-13.3, +7.9]); the slow chain stage follows node 1, not the algorithm (switch removed 2026-10-11). Two start-up segfaults at the end of the init-time VMM mapping, no backtrace; they lead to §S34 and §S46.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/CRASHES_HANGS.md`.)

### §S16 — the intermittent startup segfault of ecalc under Cray OpenSHMEMX on aac7 (Phase 16 S)

**Symptom (m).** 2 nodes, `COMM_TRANSPORT=shmem`, 1e10 digits: task 1 segfaults inside Cray's `shmem_init_thread` in 3 of 23 launches (after `bs: arenas 45.1 GB allocated`); 0 of 24 at 1e9 / `t_comm`; PE 0 then hangs in PMI. At 1e10 the init runs in a process with about 45 GB of VMM address ranges mapped and a busy seed thread; at 1e9 the seeds are done.
**Switch `COMM_INIT_EARLY=1`** (b658922): pool rule and `mn_init` before `mem_sampler_start` / `rns_init`; self-tests stay after. Adopted later as built-in (W2A A2 removed the switch).
**Trap for any SHMEM-on-cxi setup.** Cray OpenSHMEMX 11.8 / libfabric 2.3.1 (cxi) without `/dev/kdreg2` uses memhooks, which patches glibc's mmap family in place during `shmem_init_thread`; a thread executing those functions then can continue at a garbage address (about 10 % at 1e10 x 2 nodes, 0 at 1e9). Initialise SHMEM before any device mapping, or use `userfaultfd` (see §S46); never call `shmem_init` from a process with live HIP worker threads. The PE hangs in PMI afterwards (launch under `timeout`).
Other findings: `mnaccept.sh`'s `MNRUN_CPUS_PER_TASK=auto` broke every unit step on >= 2 nodes (`scontrol show node` printed one `CPUTot` per node; fixed in bc17d32 with `head -1`); the "killed step poisons the node" trap did not reproduce in 5 killed steps (`scancel <stepid>` after `timeout 300`); a ROCm 7.2.4 build needs `module unload rocm; module load rocm/7.2.4` at launch and the explicit `HIPCC` path (RUNPATH loses to the login's `LD_LIBRARY_PATH`).

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/CRASHES_HANGS.md`.)

### §A114 — A1 (Phase 14, W3): the intermittent leaf / seed-stage defect — root cause, fix, soak

**Evidence.** E1: job 21105, 7.70e10, size 1: a hang right after init (197 threads in `futex_wait_queue`, 2 in `kfd_wait_on_events`; the next line would have followed the `pthread_join` of the seed thread, binsplit.c 1228), 1 in 31 runs, not reproduced in 5. E2: job 21222, `mn e8 size 4` (1e8, 4 node-processes): node 3 printed `leaf P 18385101070989787659 limbs, Q 1466159 limbs` and the level-1 product refused it; the step passed in three other jobs. The garbage length 0xff2500040000020b is uninitialised memory read as a `size_t`.

**Root cause of E2 (i, the value fits).** At size > 1 a batch-tier leaf is copied to device numbers (`db_copy` -> `shift_into` -> `db_norm` -> `maxidx`, dbig.c); `maxidx` scans the four quarters with one OpenMP thread each. The host buffer for the per-block results was a process-wide `g_hred`, allocated on first use inside `flags_reserve` by `if (!g_hred) g_hred = malloc(...)`, unguarded, and `maxidx` read the global twice (as `hipMemcpy` destination, then as scan source). Two threads both seeing `g_hred == 0` both `malloc`; one then scans the other, never-written buffer: heap garbage as a length. It is P not Q and size > 1 because the leaf hand-over's `db_copy(&bs_Pd)` is the first `dbig` operation of a process (first-use), and it is rare because the preceding serialized allocations stagger the threads; at size 1 the same race is possible with a wrong top-level length as symptom (not seen). Not the Phase 12 copy-order race (the D2H copy is host-synchronous).

**Fix (`ecalc/dbig.c`).** `maxidx` copies each quarter into a per-thread stack buffer (`size_t hred[228 * 8]`, 14.6 KB); `g_hred` is gone; `maxidx` also aborts loudly if its result exceeds the limbs scanned. No computed value changes (bit-identical by construction).

**Mechanism measured (m, job 21230, 4 APUs; `tests/t_hred_race`, 20 000 rounds of the old pattern).** Old pattern: **9 884 of 20 000 rounds with more than one allocation, 29 402 garbage reads**; per-thread buffer: **0**, VERIFY OK. In ecalc the race needs a preemption (a scheduling event: idle-node soaks never show it, a loaded batch did).

**Soak (m).** Before-fix binary on s24-26: mn 1e8 size 4 x100, size 3 x30, size 2 x10: **140/140 ok** (so the idle-node rate is < 1/100; T1's failure was 1 in about 4 executions of the mnaccept step under load); after the fix: **140/140 ok**, 0 DIFFERS. E1 (hang after init) is **open**: hypothesis (a): a lost completion of the seed thread's last DMA wait (`mem_dev_copy_wait` does not `hipSetDevice(dev)` before waiting); the gdb-launch soak `h1` (1e9 size 1 x60, 7.7e10 size 1 x8) was running at write-up and its results were lost with the cluster. **Status: closed 2026-10-25** (not reproduced on current main: 0 hangs in 301 runs at 7.7e10, one-sided 95 % bound 1.0 %, measured); a hang caught with `ECALC_INIT_TL=1` plus `eu-stack` reopens it.

**E1 re-soak, 2026-10-24 (main 2d54d6b, aac7, 1 node, default launch, layout D, `./mnrun.sh 1 env ./ecalc <digits>`, no digit file, `ECALC_INIT_TL=1` on every second run; timeout 3x the running median with `eu-stack` capture armed, never fired).** Eight node-streams of independent 1-node runs on six nodes (1e9, 15 runs per 1 run of 7.7e10): **1e9: 1127 of 1127 OK (about 12 s each); 7.7e10: 45 of 45 OK (98-127 s each, plan fit 316 GB device of 373); HANG 0, SEGV 0, VFAIL 0, OTHER 0** (measured; classes by exit code and the run's own VERIFY line). One-sided 95 % upper bounds 3/n: **0.27 % per run at 1e9, 6.7 % at 7.7e10** (measured, from zero events). E1's old rate (1/31 = 3.2 %) is not excluded at 7.7e10: with that rate 45 clean runs have probability 0.23 (modeled). Note the nodes of four of the eight streams had no working NIC (single-process runs, no network use). E1 is not reproduced on the code after the seed-pull rework; its cause is unknown.

**E1 soak extension, 2026-10-24 19:23-20:28 ET (main 1e7c781, same code as 2d54d6b; item 9zv4_soak5).** 7.7e10 only, same launch, four parallel 1-node streams on the four NIC-less nodes: **129 of 129 OK (33 + 33 + 33 + 30), HANG 0** (measured). With the 45 above: **0 hangs in 174 runs at 7.7e10, one-sided 95 % bound 1.7 % per run** (3/n, measured from zero events). E1's old rate 3.2 % would leave 174 clean runs with probability 0.0035 (modeled): **E1 is excluded at its old rate on current code.**

**E1 soak, 2026-10-25 01:39-02:40 ET (main 614267d; items a0e1_soak6, a0e2_soak7).** 7.7e10 only, same launch: two 1-node streams on the two NIC nodes (30 min, **20 of 20 OK**) and four on the four NIC-less nodes (60 min, **107 of 107 OK**, 26 + 27 + 28 + 26), HANG 0 (measured). With the 174 above: **0 hangs in 301 runs at 7.7e10, one-sided 95 % bound 1.0 % per run** (3/n, measured from zero events); E1 stays a watch item.

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

### Design summary (planning agent, 2026-11-01; code read at main c86e5fcc)

Kind (1), the CRT flag, already detects the fault; missing is recovery: the flag becomes a collective, per-piece decision inside `mn_core`, taken before any result limb leaves the PE (the scatter and the chunked rounds write C), and the piece repeats. Kind (2) needs an independent expected value (a residue computed at consumption from corrupted storage proves nothing): the 62-bit prime `t1_q[0]` (`DR_Q`) and tracked residues with exact end-to-end identities: tree products (tracked residues seeded by the T1 recurrence), the leaf (per-node recurrence), each division step (A' = X Q + R), and the flagless cyclic product (modulo B+1 and B-1; B^2-1 divides B^P-1 for even P). Rejected: coefficient sums at x = +-1 (misses most kind-1 faults, whole-limb input faults and delivery faults).

| product | check | kind 1 | kind 2 |
|---|---|---|---|
| leaf, node-local | C: residues of P_r, Q_r vs the per-node recurrence | n/a | detected; fatal naming the node |
| tree, full P24 +- X | A flag -> piece retry; B R(C) = R(A) R(B) + R(X) from tracked residues -> product retry | recovered | production / redistribution / delivery faults recovered; operand corrupted at rest -> fatal naming node and APU |
| reciprocal, division estimate | A only | recovered | cannot make wrong digits (corrections after the exact remainder fix any error) |
| division low product X Q mod B^w | A + D before `mn_dkm_corr` | recovered | recovered (xql recomputed once) |
| wrap: L (low_basis), U (cyclic) | D on Rc; D': U = X~ Q'' mod (B+1), (B-1) | L: flag; U: D' | U recovered; L or assembly -> fatal naming the step |
| backstop | existing T1 at the end | | |

Retry rules: every retry runs inside the product call (inputs are freed right after it returns); no extra memory (planes reused, C's share re-zeroed in place); the decision is collective (A by `grp_max` on mesh 0, B and D by an allgather summed identically on every node); the repeat runs with the transform cache off (`cache_drop()`); a second failure at the same place is fatal with the location; `ECALC_DR_TOTAL` caps the failures per run. Localization: the log line carries product number, size, piece i,j of ka x kb, cause, attempt, first flagged point and, from an allgather of per-node masks, the PEs. Exit summary per process: `DR: <checks> checks, <injected> injected, <failures> failures, <recovered> recovered (pieces, products, division), <fatal> fatal`.

Injection: `ECALC_TEST="dr=<kind>,dr_k=<k>,dr_pe=<pe>,dr_n=<n>,dr_m=<m>"` (`dr_k` k-th P24 product of the process, `dr_pe` -1 = all, `dr_n` consecutive attempts, 2 = persistent, `dr_m` consecutive products injected from the k-th; for xql and cyc the PE is the node). Kinds: flag (step 2), deliver, store, xql, cyc, leaf (steps 3-4).

Small tests of the untested paths (2026-11-08, 2 nodes on aac7, `main` b7ea4e52):
- Different per-PE failure histories: PASS. `ECALC_DR=1`, only node 1 injected (flag, PE 7): 32 injected, 32 failures on both nodes, 32 pieces recovered, VERIFY OK at 1e10 digits. With `ECALC_DR_TOTAL=3` both nodes stop together on the 4th failure (rc 3, no hang).
- Retry peak memory at the memory edge (planner device at 99 % of the edge, 6.441e10 digits): PASS for the tree retries and the division low product. Every-PE flag retry (144 injected, 285 failures recovered, VERIFY OK) and the xql retry (4 recovered): no pool growth, no hipMalloc inside a phase, no remap; pool peak-live per phase equal to the uninjected run on the compared rows (node 1 rows within 0.4 GB, run-to-run). The wrap (cyc) arm could not run: see below. Two of the runs stopped before the first product on start-up (one 900 s exchange wait, one hipMalloc out of memory at the 99 % edge), not in the DR code.
- D' (cyclic wrap product): PASS at 1e10 digits, 2 nodes, `DM_MN_WRAP=28` (a value of 20 or more sets the wrap plane to 2^n, so the wrap runs below the cap; `DM_MN_WRAP=1` means the cap plane and never reaches the wrap at this size). Recovery: one injected fault on node 1 (`dr=cyc`), both nodes record 1 failure and recover 1 in the division, VERIFY OK, the wrap line printed. Persistent (`dr_n=2`): both nodes stop fatal naming `rns_mul_dist_mn_cyc_v`, rc 3, no hang. The memory arm for the wrap (b4) is still not measured at the edge. The fault is injected after `mdb_norm`, which only trims leading zero limbs, so judged equivalent to injecting before it.

Cost model (modeled, 3.8e13 digits, 576 nodes): A 0.05-0.2 s, B about 60 ms, C about 5 ms, D about 12 ms, D' 0.1-0.15 s; total 0.25-0.4 s = 0.1-0.2 % of a 200 s run.

### Measured, step 6 repeat: paired timing in the aac7 campaign (M6, 2026-11-02 00:33-00:48 ET, main d7c7f28, item 64_m6_dr; 2 nodes, 6.441e10 digits, measured)

ABBA, 6 pairs, A = `ECALC_DR` unset, B = `ECALC_DR=3`, all runs rc 0 and VERIFY OK. Mean A 65.18 s, mean B 64.62 s: **B - A = -0.56 s, 95 % interval [-3.52, +2.40] s** (-0.9 %, interval -5.4 % to +3.7 %). Together with the step-6 table above (+0.48 s, 4 round differences, 128.82e9 digits), the overhead of `ECALC_DR=3` is not distinguishable from zero at 2 nodes at either size; the interval is about 3 s wide, so a cost of 0.1-0.2 % (modeled) is far below what aac7 can resolve, and the 576-node figure stays modeled.
