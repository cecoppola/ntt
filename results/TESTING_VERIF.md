# TESTING_VERIF — verification, correctness sweeps and test design

Scope: verification, correctness sweeps and test design: per-phase verification, inter-node correctness, weak-memory audit, t_mn_grid over SOS. Condensed 2026-10-19 from the per-item reports (originals: `archive:results_merged_2026-10-19/`; raw data and drivers: `archive:results_raw_2026-10-19/`). Old item IDs resolve through `results/INDEX.md`. Labels: m measured, mod modelled, a assumed.

| item | title |
|---|---|
| V | verification and cleanup (Phase 11, PLAN.md §26 row V: D5, ECALC_RECHECK, E1 (c), E2) |
| W | verification and housekeeping (Phase 12, PLAN.md §27 row W; DECISIONS2 §8) |
| T | tests, checkpoints and docs (Phase 10, PLAN.md §21: D3, D2, D4, D5, C6, E3) |
| WP6 | inter-node correctness on aac6 (2026-09-17/18) |
| CX | correctness sweep of the 2026-10-11/12 branches on aac6's CPX nodes |
| WM15 | the weak-memory audit (Phase 15, task 0b) |
| N414 | `t_mn_grid` on real nodes over SOS, `mnrun.sh`'s SHMEM detection, the dist tier's load speed (Phase 14, PLAN §33 A4, A5) |
| FIX117 | FIX1 17 — t_edge vmm, mn_selftest power-of-two>=128, MN_TOPO_GROUP under SHMEM |

## Topic summary

What this is: how the project proves its digits right (T1/T2 verification, the standalone recheck, the standing regression `mnaccept.sh`, checkpoint restarts), what the correctness sweeps found, and the audits (weak memory, inter-node transports, SHMEM).

Current state (cross-checked with `ecalc/README.md`, `results/CRASHES_HANGS.md`, RESULTS.md):
- **The "intermittent size > 1 verification failure" (D5) is closed.** Two causes, in order: (a) §V found that seven of the eight T1 moduli in `verify.c` were composite (two divide every Q, so T1 was blind there) and replaced them by the first eight primes above 2^62 (digits unchanged; T1 numbers and sidecar residues changed); (b) the real fault was a timing-dependent wrong leaf P, Q on node 0 or 1 under forced growth of plane pool 1, localised in §V and fixed in Phase 12 R (the batch tier's odd-node copy `mem_dev_copy_on` did not wait for its copy; the fix waits), see `results/CRASHES_HANGS.md §R`. §T's "stale plane pointer" suspicion was not reproducible and is superseded by §V.
- Standing tools: `mnaccept.sh <job> [--full] [--only unit,e9,mn,ckpt,recheck,full,...]` (§T D3, extended in §W with the `recheck` step; the gate per TESTING.md owns the current step list); `ECALC_RECHECK=1` standalone recheck of a finished run from its part files + the top-level P, Q set (§V, §W); per-level check switches `ECALC_RES_LOG`, `ECALC_RES_LOG_LEVEL`, `ECALC_LEAF_DUMP` (debug); multi-node checkpoints with `BS_CKPT_TREE_EVERY` and the barrier off the write (§T C6).
- `ECALC_CKPT_TOP`: §W made it default-on above 10^10 digits (cost ~1 s of 82 s at 4 x 10^10, m); current README says **off by default** (record timing runs; user decision 2026-09-27), `=1` writes it at any size. The README row owns this.
- `RNS_PLANES_FIRST` (§N414 A5): measured 2.01x at 1.42 x 10^11 on a node with a small 2 MiB-block supply (1349.7 -> 672.0 s, m); now an adopted default (1). A4: `t_mn_grid` VERIFY OK at 2 and 3 real nodes over SOS; `mnrun.sh` detects SOS vs OSHMEM from the binary's ELF dependencies.
- §WM15 audit: ~60 sites read, one real hazard (H1: WAR between a sender's xGMI push and the receiver's unpack of the previous transform), fixed by an event wait (`DIST_RB_WAIT`, default on), demonstrated failing 52/52 under an induced lag (`DIST_TEST_LAG_MS`), no measurable cost.
- §CX: CPX-node sweep found one switch failure (`BS_SEED_PULL=1`, a 600 s pull-kernel timeout) which was investigated later (BS_SEED_PULL is now adopted: RESULTS.md); `t_ntt 24` always fails its speed check on CPX (hardware, not a defect).
- §FIX117: three small fixes (`t_edge vmm`, `mn_selftest` rows-per-rank growth, fatal `MN_TOPO_GROUP>1` under SHMEM); `mnaccept` unit tests cannot pass on a GPU-partitioned node (~22.9 GB per logical GPU).
- Superseded: §V's "open" D5 fault and the W "stand-ins kept" (`MN_DM=host`, `MN_COMBINE=host`) item (kept only while D5 was open; the README switch table no longer lists them, check before relying); §T D5's conclusion.

## V — verification and cleanup (Phase 11, PLAN.md §26 row V: D5, ECALC_RECHECK, E1 (c), E2)

Branch `v11` from `main` 72aa2e9. Files: `verify.c`, `mn_out.c/.h`, the residue kernel of `dbig.c`, the output tail of `ecalc.c`, `t_dist.c`, deletions in `ntt_dist.c` and `binsplit.c`, `.gitignore`, README; minimal touches in `newton_db.c`, `mem.c` (`MEM_DPOOL_FILL` test knob), `binsplit.c` (`bs_res_check` per level).

#### D5 — the intermittent size > 1 verification failure
**The checker's signature was a composite-moduli artefact.** Three failures (A-mem x2, T's d5b) printed `P BAD, Q BAD, T(P+Q) == XQ+R BAD` at q0, q1, q3, q4, q5, q7 and ok at q2, q6, with wrong digits each time. PLAN §23-6 read this as "the residue path's fault, not a wrong number": wrong. **Seven of the eight "primes" in `verify.c` were composite** (2^62 + {135, 179, 183, 247, 315, 319, 349, 397}; only 2^62 + 135 prime). q2 = 2^62 + 183 and q6 = 2^62 + 349 have all factors below 2.5 x 10^7, so every node's Q(a, b) (a product of >= 2.9 x 10^8 consecutive integers) is 0 mod q2 and q6; there T1 is blind (0 == 0, X drops out, P sees only the top node's range). So a wrong P or Q from any node below the top is caught at the six other moduli and passes at q2, q6: exactly the observed pattern, i.e. a genuinely wrong bs result correctly flagged.
**Fix (verify.c, commit 66ee599):** `t1_q` is now the true first eight primes above 2^62 (2^62 + 135 ... 277). T1 numbers, `RES` logs and `.t1` sidecar residues change; digits do not (10^9 and 4 x 10^10 identical). A wrong P now shows BAD at all eight.
**The real fault, localised** (instrumentation `ECALC_RES_LOG=1`, `ECALC_RES_LOG_LEVEL`, `ECALC_LEAF_DUMP=<dir>`; reproducer 10^10 at size 4, `POOL_LOG=29 RNS_POOL1_GB=3.2213`: pool 1 at 3 GiB grows to 4 GiB at the first batch-local level; `v11_d5.sh <job> 14 shard`):

| batches | runs | result (m) |
|---|---|---|
| 1 (old moduli) | 5 | 4 identical, shard5 failed (digits differ from byte 2 287 655 016; wrong P and Q) |
| 2 (new moduli, leaf check) | 4 | 2 identical; node 0's leaf P/Q BAD in one, node 1's in another; other leaves agree with the recurrence |
| 5 (no level checks, leaf dumps) | 13 | 11 identical; two wrong leaves (node 1: P differs from limb 131 584 up, Q from 4 092 871; node 0: P from limb 131 072, Q from 4 092 358) |
| 3, 6, 7 (per-level checks from level 17 / 11; batch 7 CPU-only read) | 5 + 12 + 14 | 31 of 31 identical, every level ok |

Without per-level checks 21 identical and 5 failed of 26 forced-growth runs, every failure a **wrong leaf P_r, Q_r of node 0 or 1** (never 2 or 3), wrong from a limb ~2^17 (P) / 2^22 (Q) to the top, everything downstream consistent with it; the hand-over copy agrees. With any per-level check 0 of 31 failed (0.15 % if the check were neutral): a **timing-dependent error in the leaf's level loop under in-phase growth of plane pool 1**, hidden by anything separating one level's end from the next level's start. Default-configuration runs (~15 at 10^10/4) never failed (A-mem's rule: pool 1 at full pool for `POOL_LOG` <= 30). Localisation delivered: the CPU-to-GPU hand-over in the batch tier (CPU `spill_merge` writes, then the next level's scatter). It was later found and fixed in Phase 12 R (`results/CRASHES_HANGS.md §R`).
Verdict at the time: (1) the checker was not at fault and is now stronger (the digits were wrong in every failing run; PLAN §23-6's "digits right" was mistaken); (2) a second, real fault exists (5/26 unperturbed runs), then open. The gate "20 forced-growth runs all VERIFY OK" was met only with the probe (31/31), not without (21/26): both reported.

#### `ECALC_RECHECK=1` — the standalone recheck (mn_out.c)
The run writes `<outfile>.t1` (node 0: N, d, d_out, size, moduli, residues of X, R, P, Q, and the d - d_out computed digits after d_out). `ECALC_RECHECK=1 ./ecalc <digits> <outfile>` (through `mnrun.sh <size>` at size > 1, same `BS_CKPT_DIR`) then, with no pools and no computation: reads each node's part file in 256 MB chunks (digit residues by Horner; counts and 49-char tails all-gathered), forms X mod q from the file, checks the T2 windows, takes P and Q mod q from the checkpointed top-level shares (`bs_ckpt_tree_read` of level ceil(log2 size); size 1: the level-0 set written with `ECALC_CKPT_TOP=1`), recomputes the term recurrence per node, and runs T1 with R's residues from the sidecar (the one value not recomputable without redoing the division) plus `digits -> X mod q == the run's` and `P, Q from the checkpoint == the run's`. Prints `RECHECK OK` / `FAILED` per node. Without a checkpoint it says so and takes P, Q from the sidecar (only the file and the recurrence are then independent).
Formats: T1 moduli changed (sidecar records them; the recheck refuses others); new `<outfile>.t1` (~400 B) and optional `tree_000.{hdr,r0..r3}`; `mn_out` gained `tail[24]`, `pq_bg` a `joined` flag.
Also E1 (c) deleted: `DIST_PLANE2` (`dist_fwd2/inv2`, see `results/EXCHANGE.md §C`), `BS_SEED_DIRECT=0`, the `ECALC_OVERLAP_COPY` remnant; **`MN_DM=host` / `MN_COMBINE=host` stand-ins kept** (the only cross-check of the sharded division). E2: `.gitignore` `results/*` + `!results/*.md`.

#### Gate runs (m)
Regression (`mnaccept.sh --full`, 45ef12e): **17 passed, 0 failed, 1904 s**; 10^9 size 1 both bases identical (14.3 / 25.1 s); 10^8 sizes 2/3/4 and 10^9 sizes 2/4 identical; ckpt restart identical; **4 x 10^10 size 1 identical to the stored reference, VERIFY OK, total 86.21 s** (171 s with the write). Recheck mode (`v11_recheck.sh`; run, recheck, recheck of a file with one digit flipped): 10^8 and 10^9 at sizes 1 and 2 all RECHECK OK (10^8: 100 000 001 digits read in 0.1 s; size 2: P, Q from the checkpoint == recurrence == the run's) and every corrupted copy RECHECK FAILED (size 1 P, Q from the sidecar in that build). Forced-growth runs in total: 57 (26 without probe: 21 identical, 5 VERIFY FAILED with a wrong leaf; 31 with probe: 31 identical; the `MN_DM=host` runs identical).
Open at the time: the leaf race (now fixed); size-1 recheck needs `ECALC_CKPT_TOP=1` at run time (2 x top-level P, Q: 35 GB at 4 x 10^10); the recheck reads part files where written (node-local `/tmp` means the same nodes); `ECALC_RES_LOG=1` is a debug mode (~30 s of bs per run with the level probe).

## W — verification and housekeeping (Phase 12, PLAN.md §27 row W; DECISIONS2 §8)

Branch `w12` from `main` 7aded87. Files: output tail of `ecalc.c`, `mn_out.c/.h`, `mnaccept.sh` (`recheck` step and the `--full` recheck), README; minimal `binsplit.c/.h` (`bs_ckpt_own_buf`, `bs_ckpt_tree_pdone`).

#### 1. `ECALC_CKPT_TOP` on by default above 10^10 digits (as built then; README now says off by default, see summary)
Default then: on with an outfile, decimal limbs and d_out > 10^10 (`=0` off, `=1` any size); directory `BS_CKPT_DIR` else `<outfile>.top/`. Size 1 device flow: a background thread writes the static `tree_write` (was `bs_ckpt_tree_write`, removed 2026-10-23) of P, Q right after bs; the driver waits for P (`ckpt_top_wait_p`) before `S = P + Q` overwrites P and joins before Q's block is released after the division; DMA buffers are the thread's own. Size > 1: `mn_tree` writes the top set as before; default directory `<outfile>.top` and `BS_CKPT_TREE_EVERY=64`.
Measured at 4 x 10^10 (same binary, one run each, identical digits):

| | top-level set (then default) | `ECALC_CKPT_TOP=0` |
|---|---|---|
| init / bs / recip / dm | 15.0 / 35.29 / 14.16 / 31.51 s | 18.2 / 34.64 / 13.10 / 28.09 s |
| **total (to VERIFY OK)** | **81.87 s** | **80.99 s** |
| digit file write (overlapped) | 95.92 s | 78.33 s |

The set: 35.56 GB in 31.57 s in the background (1.13 GB/s local disk); waits 0.00 s for P and 2.15 s for Q. Two more runs: 85.25 s total (3.23 s wait for Q, slow init), and on a node with 1.59 GB/s disk 82.42 s, 0.00 s waits. So the wait is the disk rate against the division's 15-17 s: hidden at >= 1.5 GB/s, 2-3 s exposed at 1.1 GB/s; the unhidden cost is disk: the 40 GB digit write ends ~18 s later (160-171 s to the last byte vs ~155 s). Verdict then: ~1 s of wall and ~30 s of disk time; run-to-run spread +-1.4 s.

#### 2-3. The recheck as a regression step; a 4 x 10^10 run rechecked
`mnaccept.sh recheck` (default run, not only `--full`): 10^9 size 1 (`ECALC_CKPT_TOP=1`) and 10^8 size 2: run (VERIFY OK, digits identical), `ECALC_RECHECK=1` (RECHECK OK on every node, P, Q *from the checkpoint*, the step fails if from the sidecar), then one digit flipped at byte 1000 and recheck again (must fail). 46 s for the step (m). `--full` rechecks the 4 x 10^10 files too. Recheck time at 4 x 10^10 (m): first build 98 s (file read twice, 84.2 s for the 40 GB file), one-pass reader 82 s (file 72.9 s), OpenMP non-digit scan + a reader thread for the next 256 MB chunk **46 s end to end, file 36.3 s** (0.56 of the run's wall; floor is the disk: 40 GB + 35.6 GB read, file in page cache).

#### 4-5. Stand-ins and README
`MN_DM=host` / `MN_COMBINE=host` stand-ins **kept** at the time: R's report named the transition fix but had no 40/40 count; deletion was a 20-minute edit once R's numbers were in (the three `host_combine`/`mn_dist`/`mn_dm` tests in `ecalc.c`, `mn_out_scatter_standin`, `mn_gather_host`). The README gained a Switches section (every `getenv`, 108 variables then, verified in both directions by grep); `results/*.md` verified tracked.

#### Gate runs (m)
Job A (2668ef4): recheck 2/2 (46 s), full 2/2 (4 x 10^10 81.87 s identical, its recheck 98 s), baseline 80.99 s, unit 8/8, e9 2/2, mn 5/5, ckpt 1/1: 20 of 20 across three invocations. Job B (36afd71, one-pass recheck): **20 passed, 0 failed, 2018 s**; 4 x 10^10 85.25 s (init 17.8, bs 34.8, recip 14.1, dm 32.5), recheck 82 s. Job C (fd82fb8): recheck 2/2 (44 s), 4 x 10^10 82.42 s identical, **both waits 0.00 s**, 162 s with the digit write, recheck **46 s**. Size 1 digits untouched (10^9 both bases, 4 x 10^10 identical).
Open: stand-ins (above); 2-3 s Q wait (release Q after the output stage, or a faster `BS_CKPT_DIR`); recheck from cold disk is disk-bound (~50-70 s at 1.1-1.6 GB/s); size > 1 top set is written synchronously by `mn_tree` (35 GB / size per node at 4 x 10^10); `<outfile>.top` is never deleted (on the target ~2 x 20 TB: use `ECALC_CKPT_TOP=0` if unwanted).

## T — tests, checkpoints and docs (Phase 10, PLAN.md §21: D3, D2, D4, D5, C6, E3)

Branch `t10` from `main` 4aca721. Files: `mnaccept.sh` (new), `accept.sh`, `variance.sh`, `mn.c` (tree checkpoint loop, `mn_finalize`), `binsplit.c` (`bs_ckpt_tree_remove_below`), README. Numerics unchanged (C6 moves a barrier and a set removal).

#### D3 — the standing regression: `mnaccept.sh <jobid> [--full] [--only unit,e9,mn,ckpt,full]`
Run from `ecalc/` on the login node against a one-node allocation; one log and one PASS/FAIL line per step under `results/mnaccept/<jobid>/`; exit status = number of failures.

| step | what | pass when |
|---|---|---|
| unit | `t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`, `t_newton 20`, `t_verify`, `t_out`; `t_mn_grid 1 28` at 2 node-processes | exit 0, a `VERIFY OK` line, no `VERIFY FAILED` |
| e9 | 10^9 at size 1, `LIMB_BASE=10` and `2` | exit 0, VERIFY OK, `cmp` identical to `ref/` |
| mn | 10^8 at sizes 2, 3, 4 (`POOL_LOG=27`), 10^9 at sizes 2, 4 (`POOL_LOG=29`) on one node | `mn: all n nodes: VERIFY OK`, part files identical to reference |
| ckpt | 10^8 size 2 with `BS_CKPT_EVERY=2 BS_CKPT_MIN_LEVEL=2 BS_CKPT_ABORT=6`, then `BS_RESTART=1` | both nodes exit, restart exit 0, identical |
| full | 4 x 10^10 at size 1 (reference evicted from page cache first) | exit 0, VERIFY OK, identical to the stored reference |

~31 min with `--full` (`t_ntt 24` alone ~10 min: the O(n^2) reference runs single-threaded). Runs (m): first job 17 steps incl. t_ntt 24 (565 checks), t_mul 20 (189), t_dbig 0 (555), t_newton 20 (620), t_verify (334), t_mn_grid at 2 procs (120 per rank), 10^9 base 10 / 2 12.2 / 21.5 s, ckpt restart from leaf level 6 (453 nodes), **4 x 10^10 total 86.64 s** (bs 38.4, dm 34.8, init 13.3), VmHWM 48.8 GB, identical; two steps failed on script defects fixed in 138c5a5 (t_bs needs `ref/e_<d>.sha256`, space-containing tags in file names). Final build: **17 passed, 0 failed in 1828 s**, 4 x 10^10 89.49 s (bs 38.3, dm 34.4, init 16.7).

#### C6 — the multi-node checkpoints: `BS_CKPT_TREE_EVERY`, the barrier off the write
Before: `mn_tree` wrote a set at every level, then a barrier (everyone waited for the slowest write), then removed the superseded set. Now: `BS_CKPT_TREE_EVERY=k` (default 1) writes sets at levels k, 2k, ... and always the top level; no barrier after a write; the barrier is taken at the next set level, after its product and before its write, then everything below the previous set is removed (`bs_ckpt_tree_remove_below` clears every lower tree level); the last set's predecessors at `mn_finalize`. The restart rule (lowest over nodes of each node's highest complete tree level) stays sound: a node removes sets below m only after a barrier every node reached having completed its write of m. A node holds two sets between a write and the next barrier (2 x 17 GB at 4 x 10^10, size 2). `BS_CKPT_ABORT_TREE=l` exits right after the write.
Tests (m, 10^8, sizes 4 and 3; all restarts `cmp` identical, every node VERIFY OK): uninterrupted; restart from the top set; abort after tree level 1 (leaf sets still present); node 1 exits and the others killed; killed during a write; `BS_CKPT_TREE_EVERY=2 BS_CKPT_ABORT_TREE=2` (only the top set, level 1 skipped, leaf sets removed at level 2's barrier). At 10^10 size 4 tree sets are 2.1-2.3 GB per node at 0.7-1.2 GB/s: the removed barrier would have cost the slowest write (2-3 s) per level.

#### D2 and D4
D2 (restart at 10^10, size 4, loopback; `POOL_LOG=29`): uninterrupted with checkpoints identical, total 130.5 s (leaf sets 13.7 s, tree sets 5 s); abort after tree level 1 then `BS_RESTART=1` identical, total 105.2 s (bs 20.8); restart from the top set (level 2) identical, total 80.3 s (bs 0.4, dm 67). D4: `LIMB_BASE=2` 10^10 at size 1 VERIFY OK, identical to the decimal path's file, total 56.2 s (bs 6.1, recip 4.8, 10dP 2.4, dm 6.8, T1 2.3, dc 23.7, T2 0.3; init 13.0), VmHWM 124.9 GB (m).

#### D5 — the stale-plane-pointer suspicion (A-mem open issue 1) — superseded by §V
A-mem saw two T1 failures at 10^10 size 4 with pool 1 at 3 GiB growing inside bs. Code reading (`rns_mul.c`): growth calls precede every pointer taken from the pools in the same region, no plane-pool pointer survives a call, the only cached one is `dist_plan`'s `sbuf/rbuf` (a hazard only if pool 1 grows between two dist products of the same shape; not exercised). Reproduction (m): 18 forced-growth runs of 10^10 at size 4 (plus 10^9 at sizes 1 and 4): 17 identical, 1 failure (d5b, A-mem's exact T1 signature); 8 kept tree sets byte-identical across runs (32 + 64 file comparisons). T concluded a stale pointer in the batch tier is not reproducible and that the per-prime signature pointed at the T1 path; §V then showed the signature was the composite moduli and the fault a real wrong leaf. `results/MEMORY.md §A-mem`'s rule (pool 1 at the full pool for `POOL_LOG` <= 30) stands.

#### E3 — `ecalc/README.md`
Header with the Phase 9 defaults (86.4 s / 48.8 GB at 4 x 10^10); file table and sections for the multi-node run and the standing regression; `accept.sh` / `variance.sh` run from their own clone and `accept.sh` adds `t_dbig 0`, `t_out`.
Open at the time: the d5b failure (now explained, §V); C6 two-set disk use; `BS_CKPT_TREE_EVERY` beyond size 4 exercised only at size 4; `t_ntt 24` takes 10 of 31 minutes (`--only` skips); leaf sets at the defaults are ~35 GB / size each at 4 x 10^10 (`BS_CKPT_MIN_LEVEL` above the leaf level leaves only tree sets).

## WP6 — inter-node correctness on aac6 (2026-09-17/18)

Branch `wp6-internode` from `wp1-decimal-base` 8a43393. Code under test unchanged: `comm_tcp.c` (TCP full mesh, one process per rank), `ntt_dist.c`, `t_dist.c` (one process per rank when `COMM_RANK` is set, rank r on APU r mod 4), `wp6run.sh`; added: `t_comm.c` per-rank mode. Correctness only (one 1 GbE NIC per node).

Cluster state: of 4 nodes two were down, one held by the main session, one by another job; only two short 2-node windows, so the 3-node case and a `t_dist 24` run with 8 ranks across two nodes did not happen (its two ingredients were run: 8 ranks on one node, and 3 MiB slab all-to-alls across two nodes).

Runs (m):

| nodes / ranks | test | result |
|---|---|---|
| 2 nodes, 8 ranks (4 per node) | `t_comm` (5 slab sizes 1 B-3 MiB, barrier, reductions) | VERIFY OK on all 8, 7 s |
| 2 nodes, 8 ranks | `t_dist 20` | VERIFY OK on all 8 (4 checks each, 2^10 x 2^10, all four primes), 3 s |
| 1 node, 4 ranks (TCP, one process per APU) | `t_comm`, `t_dist 20 / 24 / 26` | VERIFY OK (4, 52, 65 checks; up to 2^26 points); immediate re-run on the same ports OK |
| 1 node, 8 ranks (two per APU) | `t_dist 24` | VERIFY OK on all 8 (52 checks each) |
| login node, 4 and 8 forked | `t_comm` (host-only) | VERIFY OK |
| 2 nodes, 8 ranks | `t_dist 24` | did not run (no second window) |

So the communicator and the distributed transform are correct across two nodes with 8 ranks (`t_comm`, `t_dist 20`), correct at size 8 up to 2^24 points (one node) and at size 4 up to 2^26 over TCP; every rank verifies its block-cyclic rows against the one-rank engine. **Bugs found: none**; no source change in `comm_tcp.c`, `t_dist.c`, `wp6run.sh`.
Observations on the TCP communicator: rank r listens on `base + r`, connects upward to every higher rank (retrying up to 60 s), accepts from lower ranks and reads an 8-byte hello; start order across nodes does not matter (skew < 1 s). `SO_REUSEADDR`: consecutive runs 0-5 s apart on the same ports connect at once. Host names from `scontrol show hostnames` resolve to the 1 GbE interface. No deadlock in the all-to-all (one sender thread per peer, caller reads in rank order; verified with 3 MiB slabs and 2 MiB-per-peer planes). `srun --ntasks-per-node=4 --gpus-per-node=4` exposes all four APUs to every task, so `hipSetDevice(rank % 4)` places ranks (the 8-rank step used `--overlap` and `COMM_PORT=27100`).

## CX: correctness sweep of the 2026-10-11/12 branches on aac6's CPX nodes

Driver `tools/cx_aac6.sh`, 2026-10-12 02:37-03:40 ET. Seven branches (o2 o5 k6 k1b k8b o3 g6) built (7/7 PASS) and run on CPX nodes (three slots, 45-min holds; CPX env `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30`, CPX tops out near 4e9 digits). Per branch, arm off then on (o3, g6: one arm std): (1) `mnaccept --only e9 --skip-grid` (1e9, both limb bases); (2) `ecalc 4000000000`, off and on digit files `cmp`'d; (3) `mnaccept --only unit --skip-grid`. Switches (on): o2 `DBIG_SMALL_BOUNCE=1`; o5 `RNS_STRATEGY_FORM=B5`; k6 `RNS_DIST_OVERLAP=1` and k1b `DIST_GATHER_RAW=1` (both with `RNS_STRATEGY=C`); k8b `BS_SEED_PULL=1`.

Results (m):

| branch | e9 off / on | ecalc 4e9 off / on | 4e9 digits off vs on |
|---|---|---|---|
| o5 | PASS / PASS | 20.43 / 19.73 s | identical |
| k6 | PASS / PASS | 21.15 / 22.43 s | identical |
| k1b | PASS / PASS | 21.01 / 20.63 s | identical |
| o2 | PASS / PASS | 18.98 / 18.45 s | identical |
| k8b | PASS / **FAIL (2/2)** | 19.56 s / **FAIL** | no digits on the "on" run |
| o3 (std) | PASS | 19.11 s | n/a |
| g6 (std) | PASS | 19.00 s | n/a |

o5 extras: `t_strategy 24 / 26 1 V` PASS; ecalc 1e9 (10.93 s) and 4e9 (30.02 s) with `RNS_STRATEGY_CHECK=1` VERIFY OK, 4 check lines each (the B form was exercised). Failures, classified: **`t_ntt 24` fails in 12 of 12 unit runs, both arms: CPX limit** ("VERIFY FAILED 4 of 2360 checks", the speed check; S44: a CPX XCD has 1/6 of an APU's bandwidth); all other 7 unit tests PASS on every branch and arm except k8b/on `t_patch` (killed rc 143 after "VERIFY OK (1 checks)", cause not determined). **k8b with `BS_SEED_PULL=1`: FAIL**: both e9 runs and ecalc 4e9 exit rc 3 after init with `db_pull_wait: APU0's pull kernel did not finish copy 1 in 600 s` (the kernel's own watchdog; each failing run cost 600 s); the off arm is clean so the switch is the cause; unknown whether CPX-only (re-run on a 4-APU node). **g6 `--only sched` (`MNACCEPT_SCHED_PROCS=4` on one CPX node): 3 of 3 FAIL, environment** (4 node-processes need 4 nodes). Speed on CPX (4e9, off to on, single runs, not timing claims): o5 -0.70 s, k1b -0.38 s, o2 -0.53 s, k6 +1.28 s.
Later: BS_SEED_PULL is now an adopted default (RESULTS.md, `results/SINGLE_NODE.md §CAND`), so the CPX timeout did not block adoption on 4-APU nodes.

## WM15 — the weak-memory audit (Phase 15, task 0b)

Agent WM, branch `p15-WM` from `int15i` ab8daa2.

**Result: one hazard, fixed; ~60 other sites safe; no patch needed for the files being edited elsewhere (`mn_out.c`, `ecalc.c`, `newton_db.c`, `binsplit.c`, `rns_dist.c` cache).**
- **H1 (WAR)**: `ntt_dist.c` chunk pipeline over `comm_xgmi` (`dist_fwd_pre`, `inv_pre_y`; the single-node dist tier `dist_core`, three transforms per prime): the sender's push kernel on its transfer stream stores into the receiver's `rbuf` while the receiver's unpack kernels of the previous transform (its compute stream) may still read the same chunk region. Nothing on the GPU orders the sender's `ts` after the receiver's `s`; the exchange's `pthread_barrier` orders only host threads. Needs the receiver's GPU to lag by >= a column pass + a row pass (improbable, not excluded). **Fix (0a10a99)**: `dist_plan.evr` event recorded on `s` at each transform's start; the host waits for it before the first post (`dist_fwd_pre` after enqueuing the first row pass, `inv_pre_y` after enqueuing the packs); the barrier then holds every sender until every receiver has passed it. No GPU idle; digits identical (an ordering, no arithmetic); SHMEM, TCP, layered-minor transports already sync more.

Method and rule: every HIP call, launch, stream/event, thread hand-off in `ecalc/*.c` (~19 700 lines) read in context; the rule (apumult A27 §3.3/§4.3): device -> CPU needs a completed sync of the stream that ran the writer, done by the reading thread or handed over by release/acquire; device A -> B the same sync observed before launching B's reader or a `hipStreamWaitEvent`; CPU -> device stores happen-before the enqueue and no rewrite before the copy is synchronised; peer writes must not overtake the target's pending reads. Codebase invariant: every operation synchronises all streams it used, in the thread driving each device, before returning (dbig ops `hipStreamSynchronize(0)` per quarter thread; rns tiers per device thread then `omp barrier`; mdb ops per-APU sync then join); most "safe" classifications rest on it. Assumed, not measured: `hipStreamSynchronize` of the writing stream is equivalent for its work to A27's `hipDeviceSynchronize`.
Inventory (all classified safe unless noted): device -> CPU reads (~22 sites: dbig carry flags / `maxidx` / `db_mod_qs` / `db_to_bi`, rns_mul striped and local batch, `mdev_gpu_crt`, `rns_dist` `b_core` / `dist_core` / P24 flag, `mdb_to_host_all`, `mn_out` `dev_fetch`, `x_bg_run`, `spill.c` writer, checkpoint writer, `todec`, TCP / SHMEM staging, instruments); device -> device (~11 sites; the xGMI pipeline RAW is safe via `x_wait` and WAR was **H1**; layered intra stage, SHMEM puts, striped batch CRT, `k_scatter*`, `mem_dev_copy_on`, pool reuse); CPU -> device (~8: seeds, bounce buffers, staging, descriptors, `ctab`, spill restore, checkpoint restart); host-thread hand-offs (~12: mutex/cond, semaphores, release/acquire atomics, barriers, joins; lazy first-use allocations inside four-thread regions are per-device or mutex/once).
Unclear (runtime assumptions, no code change): **U1** stream-sync vs device-sync equivalence (a); **U2** CPU stores into coarse-grained device memory (seeds' direct path, VMM off) visible to a later kernel (dispatch acquire, a; default VMM path does not do this); **U3** `MEM_COPY_WAIT_POLL=1` (off) completes seed DMA by `hipStreamQuery` (readers are kernels, a fine); **U4** VMM remap (`hipMemUnmap/Map` of free chunks) while other kernels run: driver behaviour not established (test-safety rule). Notes: **N1** `mn_out_early_start` fetches before its `sp_dev_sync_all()` (correct today, move the sync first if the hook moves); **N2** `db_from_bi/db_to_bi` justify non-blocking copies by "null stream" but the real reason is the invariant.

Tests (m, aac6, `wm15_job.sh`): **H1 demonstrated**: `t_dist 24` with `DIST_XGMI=1` (four APUs, 52 cyclic convolutions 2^20-2^24 points) and `DIST_TEST_LAG_MS=200` (rank 1's GPU spins before each forward's first unpack): `DIST_RB_WAIT=0` (the code before the fix) **VERIFY FAILED 52 of 52** in two runs; `DIST_RB_WAIT=1` (the fix, default) VERIFY OK 52 in both; plain `t_dist 24` OK. The lag needed in production (a column + row pass) has not been observed (every 10^11 run identical) but nothing prevented it. Gates: `mnaccept.sh --stress --only unit,e9,mn,stress` **17/17 PASS**; 4 x 10^10 identical (`total` 57.19 s); 10^11 identical, paired:

| 10^11 (s24-26, reference evicted) | base1 | fix1 | base2 | fix2 |
|---|---|---|---|---|
| `total` s | 167.72 | 166.96 | 168.02 | 165.33 |

Fix - base: -1.7 s mean (2 each), within noise: **no measurable cost** (the 71 C-form dist products at 10^11, 57.8 s, run the pipeline with the wait). Test switches `DIST_RB_WAIT` and `DIST_TEST_LAG_MS` (off by default, README rows). Branch commits: 0a10a99 (fix), dbce20d / f036410 (switches, README), 7baec40.

## N414 — `t_mn_grid` on real nodes over SOS, `mnrun.sh`'s SHMEM detection, the dist tier's load speed (Phase 14, PLAN §33 A4, A5)

Branch `p14-N4` from `main` a839418. Labels: m (node named), mod (`BS_LAYOUT_ONLY`), a.

#### A5 — the answer: the memory state (free memory in 2 MiB blocks), not the node's hardware. Fix: map the plane pools first
**Cause (m on three nodes).** The dist tier's per-call load (gather of both operands into the plane pools) and its transforms run 4-10x slower when the run's device total exceeds the node's free memory in blocks of >= 2 MiB (buddy orders >= 9). `rns_init` mapped the region arenas first and the plane pools last, so near the edge the planes are built from sub-2 MiB pieces. That the GPU then maps the planes with small page-table fragments and loses on TLB reach is the mechanism assumed (a); the ordering effect is measured. The nodes share kernel, THP settings, amdgpu parameters, clocks, ~518 GB free and page cache <= 0.6 GB; the 2 MiB supply separates them:

| node (uptime) | free in >= 2 MiB blocks at start | 1.42 x 10^11 probe, default order: load / ntt s per call | with `RNS_PLANES_FIRST=1` |
|---|---|---|---|
| s24-16 (8 d) | 447-449 GB | **0.94-1.04 / 1.15-1.25** (six runs) | **0.095-0.096 / 0.250** |
| s24-26 (19 d) | 464 GB | **0.696 / 0.882** | **0.096 / 0.236** |
| s24-30 (2 d) | 474-476 GB | **0.099 / 0.266** | **0.099 / 0.259** |

(Probe: `ECALC_PLANE_CAP=2^30 RNS_STRATEGY=C RNS_VERBOSE=1 ./ecalc 142000000000`, first 240 s; device 501.4 GB m = mod.) The same grading by size on s24-16 (default order): 1.20 x 10^11 (433.1 GB, below the supply) load 0.100 / ntt 0.250 s per call; 1.30 x 10^11 (464.1 GB) 0.513 / 0.697; 1.42 x 10^11 (501.4 GB) 0.978 / 1.187. This explains G13d's 0.89 s vs P13b's 0.094 s per call (s24-16's smaller supply). The supply cannot be raised by a user (m, s24-16): the ~70 GB outside >= 2 MiB blocks sits between unmovable kernel pages; touching 420-490 GB of `MADV_HUGEPAGE` memory (100 % / 92 % huge pages) creates no new 2 MiB blocks and leaves the supply unchanged; four back-to-back runs do not change it; the supply falls with uptime and use; only a reboot would restore it (a).
**Fix**: `RNS_PLANES_FIRST=1` (commit 487458a, `rns_mul.c rns_init`): the plane pools are mapped before the region arenas, seeds start after them; arenas take the sub-2 MiB remainder (read once per product; no slowdown of tree or division). **Now an adopted default (1)** (`results/A11.md` registry row).
Gates and whole runs on s24-16 (m): e9 both bases PASS (11.06 / 21.91 s); 4 x 10^10 identical, total 65.19 s vs default 63.5 +- 1.5 s (one run); `t_newton 20`, `t_mul 20` OK. **1.42 x 10^11 at 2^30, C: 1349.7 -> 672.0 s (2.01x)** (VERIFY OK; bs 223.9, dm 397.1, phases 621.2, init 50.9; 705 dist calls load 0.105 / ntt 0.253 s per call; recip 510 calls load 46.8 s, ntt 115.2 s), equal to s24-30 (P13b 677.2 s), for +7 s init. Main's own defaults near the edge: 1.30 x 10^11 at 2^31 497.65 s -> **364.72 s** with the switch (load 1.636 -> 0.272 s, ntt 1.934 -> 0.455 s per call; P13b on s24-30 353.1 s): any run whose device total passes the node's >= 2 MiB supply is affected. 1.42 x 10^11, 2^30, auto + switch 863.16 s vs G13d without 1080.90 s. G13d's conclusion that `auto` beats C at 1.42 x 10^11 (1021 vs 1306 s of phases) was an artefact of the slow planes: with planes in 2 MiB blocks C is faster (621.2 vs 817.6 s of phases): input for B3 (`auto`'s cost model).

#### A4(a) — `t_mn_grid` on real nodes over Sandia OpenSHMEM: VERIFY OK at 2 and 3 nodes (m)
SOS build, `COMM_TRANSPORT=shmem`, host heap, launched through wrappers without `COMM_SHMEM_IMPL` (the new detection chooses SOS); one PE per node, 1 GbE only.

| nodes | test | result | launch-to-exit |
|---|---|---|---|
| 2 | `t_mn_grid 0.1 25` | **VERIFY OK (200 checks) on both PEs** | 219.9 s |
| 2 | `t_mn_grid 0.25 25` | **VERIFY OK (200 checks)** | 583.6 s |
| 3 | `t_mn_grid 0.1 25` (3 x 4 ranks, general map) | **VERIFY OK (200 checks) on all three** | 340.0 s |
| 2 / 3 | `t_comm` behind `stdbuf` / `timeout` / `numactl` / `env` wrappers | VERIFY OK, each | 2.1-2.3 s / 7.2 s |

Closes S13d open issue 2 (`0.5 27` remains too slow for the 1 GbE and is not needed).

#### A4(b) — `mnrun.sh`'s SHMEM detection (commit 278daf9)
Every command word that resolves to an ELF file is checked (`readelf -d` NEEDED, then `ldd`) for `libsma` / `liboshmem`; first match decides; wrapper options and `X=Y` are skipped; none found: oshmem with a note. `COMM_SHMEM_IMPL` must be `sos` or `oshmem` (else exit 1); `MNRUN_SHOW_IMPL=1` prints the choice, `=only` exits. Tested (m): sos and oshmem detected correctly in all 16 wrapper cases; `bash -c ./tests/t_comm` -> sos; `COMM_SHMEM_IMPL=bogus` refused. Before the change a wrapper word made `mnrun.sh` pick OSHMEM's `--mpi=pmix` + `setarch -L` form for the SOS binary (S13d's failed 3-node launch). First real launches failed only because the batch exported `COMM_SHMEM_DEVHEAP=1`, unusable by the HIP-free `t_comm`.

Recommendations at the time (user's decision): (1) `RNS_PLANES_FIRST=1` default (done); (2) a preflight line printing the node's free memory in >= 2 MiB blocks next to the modelled device total (not implemented); (3) rerank `auto` against C with the switch on. Open: the page-table-fragment mechanism is inferred; a page-cache variant of the memory state was not tested (root file system not writable); whether the region arenas' small-page tail costs anything is bounded only by whole runs (672 s vs 677 s). No default changed by the branch (since adopted); files: `rns_mul.c` (8 lines in `rns_init`), `mnrun.sh`, README (three rows).

## FIX117 — FIX1 17 — t_edge vmm, mn_selftest power-of-two>=128, MN_TOPO_GROUP under SHMEM

Branch `p17-fix1` (c686267 the three fixes, c764c91 `NTT_LOGN_MIN` clamp on the debug knob), tested on aac7.
1. `tests/t_edge.c`: a `vmm` form (`hipMemCreate` round-robin over the 4 APUs + `hipMemAddressReserve` + `hipMemMap` + `hipMemSetAccess`).
2. `mn.c mn_selftest`: grows `logR` (as `mn_selftest_layered`'s `MN_SELFTEST_GROW`) when `R/ranks < 32`: the real bug at >= 128 ranks (`k_twpack`'s grid is `rk/32` blocks, 0 blocks at `R/ranks` = 16, a silent no-op). New debug knob `MN_SELFTEST_LOGR`, clamped to `NTT_LOGN_MIN` (10) because the column pass needs `logR` itself to be a valid transform size; `ntt_dist.c fwd_prod` gains `HIP_CHECK(hipGetLastError())` after the pack launch.
3. `mn.c mn_init`: `MN_TOPO_GROUP > 1` under `COMM_TRANSPORT=shmem` is fatal with a named cause (B9: the cross-group mesh id collides with the next level's `all[d]` id).
The >= 128-rank case cannot be reproduced end to end (`R >= 1024` always; needs >= 33 ranks at the floor, >= 65 at the default `logR=11`); checked by arithmetic and by running the knob at 2 ranks once clamped (`self-test ... 2^21 points: ok`, before the clamp it crashed with "ntt: logn 6 out of range").
Tests (m, aac7, one node in a 4-way GPU partition: ~22.9 GB VRAM per logical GPU): `t_edge` edge: vmm 82.0 GB (41 steps) vs dev 88.0 GB (44 steps): VMM within ~7 % of dev (A2's open question); host edge 416 GB before node OOM. `mn_selftest` default unchanged. `mnaccept.sh --only unit,e9` run twice (with and without a concurrent 416 GB host test): identical **4 passed, 7 failed**: `t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`, `t_newton 20`, `e9` both bases fail because a first `hipMalloc` of 12-17 GB already exceeds the partition cap (not contention, not code; none of the fixes touch those paths); `t_verify`, `t_out`, `t_patch`, `t_mn_grid` (2 procs) pass. 11/11 needs a non-partitioned allocation.
