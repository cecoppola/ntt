# TESTING_VERIF — verification, correctness sweeps and test design

Scope: verification, correctness sweeps and test design: per-phase verification, inter-node correctness, weak-memory audit, t_mn_grid over SOS. Condensed 2026-10-19 from the per-item reports (originals: `archive:results_merged_2026-10-19/`; raw data and drivers: `archive:results_raw_2026-10-19/`). Old item IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`. Labels: m measured, mod modelled, a assumed.

Sections moved to `archive:results_merged_2026-10-24/TESTING_VERIF.md` (2026-10-24 cleanup): W; T; WP6; CX; WM15; N414; FIX117.

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
