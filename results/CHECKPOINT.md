# CHECKPOINT — checkpoint and restart of the binary-splitting phase

Scope: checkpoint and restart of the binary-splitting phase: per-node checkpoints, hardening, restart under another MN_GROUPS. | Owner of the items below (condensed 2026-10-19 from the merged per-item reports; originals in `archive:results_merged_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers are in `archive:results_raw_2026-10-19/`.

Sections moved to `archive:results_merged_2026-10-24/CHECKPOINT.md` (2026-10-24 cleanup): WP7; A-ckpt; G6 (TASKS 1.6 / 1.7).

| item | title |
|---|---|
| WP7 | checkpoint and restart of the binary-splitting phase |
| A-ckpt | M6, per-node checkpoints of the binary-splitting phase (PLAN.md §19) |
| N13 | Phase 13a agent N — checkpoint hardening (TASKS 1.7, 4.1, 1.4) |
| G6 | (TASKS 1.6 / 1.7): checkpoint restart under another MN_GROUPS / node count |
| CKPT | the recheck could not read the top-level set of the 6-node 3.8646 × 10¹¹ run |

## Topic summary

- Current state on main (ecalc/README.md): only the **top-level set** exists (`ECALC_CKPT_TOP`, off by default; `ECALC_CHECKPOINT=1` makes it `=2`, the budgeted set). It holds each node's P and Q after the last tree level (`n<rank>_tree_LLL.*`, size 1 `tree_000.*`) in `BS_CKPT_DIR` or `<outfile>.top` and exists for `ECALC_RECHECK`. **There is no restart inside a run** (user decision D11, 2026-10-12): the leaf-level and tree-level restart sets of WP7 / A-ckpt (`BS_CKPT_EVERY`, `BS_CKPT_MIN_LEVEL`, `BS_RESTART`, `BS_CKPT_TREE`) were removed and archived. WP7, A-ckpt and G6 are therefore history (design and evidence for what was removed); the schedule check of N13 / G6 went with the restart.
- Live material: the top-set writer (N13 §4.1 / §1.4: background writer, budgeted `=2`, `BS_CKPT_BG_CHUNK_MB`, `BS_CKPT_BG_MBS` test knob) and the reader fix of CKPT (64 MiB reads with retries, `sp_file` in `ecalc/spill.c`).
- Restart design that was shown correct (m): snapshot of the level node table plus the used limbs of the four region pools every few levels, atomically (`.tmp`, fsync, rename, header last); restarted runs bit-identical to uninterrupted runs and to the GMP reference at 10⁸ and 10⁹, sizes 1, 2, 4 (WP7, A-ckpt). Disk-bound: ≈ 1 GB/s per node with fsync.
- `ECALC_CKPT_TOP=1` always completes the set and can stall the division: P (17.8 GB at 4 × 10¹⁰) must be on disk before S = P + Q, the reciprocal hides only ≈ 16 s, so P alone needs ≳ 1.1 GB/s (mod). `=2` drops the set when the projected wait exceeds the slack (1 s) and never stalls. On aac7's file system the writer waited 499 s for P at 3.8646 × 10¹¹ (57 GB at 56 MB/s per node, m).
- aac7 compute nodes mount the home file system NFS soft; a huge single O_DIRECT `pread` can fail with EIO under load (CKPT). The reader now reads ≤ 64 MiB per call with retries; confirmed at scale 2026-10-14 (0 retries).
- A recheck at 6 nodes / 3.8646 × 10¹¹ takes 7380 s (m); use a timeout of ≥ 3 h (TESTING.md §3). The part-file pass at 12.5 MB/s per node is the recheck's main cost (TASKS "Proposed" candidate).
- Never run at 10¹⁰ / 4 × 10¹⁰ for the removed restart; extrapolations there were by the measured per-node rate only.

## N13 — Phase 13a agent N — checkpoint hardening (TASKS 1.7, 4.1, 1.4)

Branch `p13-N` (base 203733b); checkpoint functions of `binsplit.c/.h`, `mn.c` tree-set write and restart level, output tail of `ecalc.c`, `archive:drivers/ecalc/n13_ckpt_test.sh`; minimal changes in `newton_db.c`/`newton.h` (two-call hook `newton_mn_pq_hook` in `newton_mn_divmod`), `mn_out.c`, `mn.h`, `README.md`. Defaults unchanged: all paths sit behind `BS_CKPT_DIR`, `BS_RESTART`, `ECALC_CKPT_TOP` (off).

**1.7 — the restart trap (`MN_GROUPS`, node count).** Tree sets are indexed by schedule level, so a restart under another `MN_GROUPS` read old level l as new level l (2,4 → 4 at size 4: level-1 shares went straight into the division). Fix: tree sets get magic `ECBSCKP3` (v2 header plus `struct ckpt_sched {size, nlev, gs[32]}` from `mn_groups_parse`); leaf sets stay v1/v2; old v2 tree sets restart with a warning. `bs_ckpt_restart_scan` reads every header without aborting and returns the highest complete tree level and a verdict (0 fine, 1 another run, 2 another schedule); `mn_ckpt_tree_level` all-gathers (level, verdict) and all nodes exit together (rc 5 schedule mismatch, rc 4 another run) with node 0 printing `mn: BS_RESTART refused: k of n nodes hold checkpoint sets …`. A fresh run clears its old tree sets (`bs_ckpt_tree_clear(0)`). The recheck in `mn_out.c` takes the highest complete set via `bs_ckpt_tree_find` instead of assuming ⌈log₂ size⌉.

**4.1 — the checkpoint's wait on the division.** The Phase 12 size-1 writer waited for P's parts before S = P + Q and joined (fsync included) before Q was released, 113 s at 0.31 GB/s. Now one writer serves both sizes (`bs_ckpt_bg_start / release(part) / done(part) / join`): own DMA buffers (`BS_CKPT_BG_CHUNK_MB` 256), byte counters for the measured rate, a cancel flag, dbig descriptors held by value. Q is released after the output stage (VERIFY OK, phase lines and `total` are printed first; the join prints `waited … for P …, … for Q …, … at the join`). `ECALC_CKPT_TOP=2` sizes the write from the measured rate: at each release, if the projected remainder exceeds `ECALC_CKPT_TOP_SLACK` (1 s) less the time already waited, the set is dropped (cancel, wait for the one DMA in flight, delete temporaries and renamed files; no header, no partial set). `=1` keeps its meaning (always complete). No ordering can hide the set on a slow disk: P (17.8 GB) must be on disk before S = P + Q and the reciprocal gives ≈ 16 s, so P needs ≳ 1.1 GB/s; at 0.31 GB/s P alone needs 57 s (mod); an extra 17.8 GB copy is forbidden by the memory budget. A rewritten set loses its old header first. Test knob `BS_CKPT_BG_MBS` caps the writer at an emulated rate.

**1.4 — synchronous top set at size > 1.** `mn_tree` starts the same background writer for the top level (`mn_ckpt_bg_mode`; `BS_CKPT_TREE_BG=0` restores synchronous). `newton_mn_pq_hook(0,P)` before S = P + Q, `(1,Q)` before `mfree(Q)` (the hook takes `Q->sh` if the writer still needs it); `mn_ckpt_top_finish()` after `out_stage` releases Q, joins the writer and all-gathers whether every node has the set; only then does `mn_finalize` remove superseded sets.

**Tests** (m; `n13_ckpt_test.sh`, job 21017, s24-26, `6392f78`, node-local NVMe; later `a7884f0` changes only a printed rate):

| test | result |
|---|---|
| 1.7 before (base commit), 10⁸ size 4, abort tree 1, restart `MN_GROUPS=4` | **segfault on 3 of 4 nodes (rc 139), digits DIFFER** |
| 1.7 same sets, this branch | refused on all 4 nodes, rc 5, 4 s, both schedules named |
| 1.7 other node count (size 4 sets at size 2) | refused rc 4, 3 s, no hang |
| 1.7 matching restart | identical to `e_100000000.txt`, 4 nodes VERIFY OK |
| 1.7 gate, 10⁹ size 2 (`POOL_LOG=29`, abort tree 1, restart at size 4 refused rc 4, matching restart) | identical, restart from tree level 1, total 23.6 s |
| 1.4 background top set, `recheck e8 size 2` | identical; 3 RECHECK OK from the set on both nodes; corrupted copy RECHECK FAILED; 0.04 GB, waits 0.00 s |
| regression `./mnaccept.sh 21017 --only unit,e9,ckpt,recheck` | 13 passed, 0 failed (1263 s) |

**4.1 timing at 4 × 10¹⁰, size 1, disk emulated at 310 MB/s** (effective 0.22 GB/s with the real disk; digits identical to `results/e_4e10.out` in every run; reference evicted from the page cache; m):

| configuration | total to VERIFY | dm | wait for P | wait for Q | set | exit |
|---|---|---|---|---|---|---|
| Phase 12, `=1` at 0.31 GB/s (RESULTS §77, job 20964) | 164 s | 113 s | — | division waited for the whole set | kept | — |
| this branch `=1` at 0.22 GB/s | 128.2 s | 25.8 s | 45.6 s | 0.00 s after dm; 10.65 s at the join, after VERIFY | kept (35.56 GB in 162.9 s) | 222 s |
| this branch `=2` (slack 1 s) at 0.22 GB/s | 79.6 s | 26.3 s | 0.00 s: dropped at the P release (projected 56.5 s more) | 0.00 s | dropped, no files | 160 s |

Reading: the Q wait is gone (10.7 s remain at the join after VERIFY); P's wait is structural at this rate (≈ 45 s, `=1` costs 128 s against ≈ 80 s); `=2` never stalls (79.6 s vs the ≈ 80 s default series) but loses the set on a slow disk. The two disk rates differ (0.22 emulated vs 0.31 real). Not measured: the real disk (W measured 1.1–1.6 GB/s, `=1` expected to wait ≈ 0 s), `ecalc.base` vs branch on the same disk, the `budget` and `bg` sections at 10⁹. Recommendation: `ECALC_CKPT_TOP=2` as the safe default candidate (keeps the set where the disk can take it, ≳ 1.1 GB/s at 4 × 10¹⁰ (mod), costs ≤ 1 s elsewhere); the default stays off.

**Open issues.** Holding Q past the division keeps 17.8 GB in the block pool through the output stage at 4 × 10¹⁰ (fits; not checked at 10¹¹). Size > 1 background top set tested only at 10⁸ / 10⁹ (write 0.1 s; waits unexercised at scale). Behaviour changes outside the default: a fresh run with `BS_CKPT_DIR` at size > 1 deletes its own earlier tree sets; a failed synchronous tree-set write removes partial files; the size > 1 top set is always written in the background (`BS_CKPT_TREE_BG=0` restores synchronous). Done at `6392f78`/`a7884f0`; nothing live.

## CKPT — the recheck could not read the top-level set of the 6-node 3.8646 × 10¹¹ run

**Question.** After the 6-node 3.8646 × 10¹¹ run with `ECALC_CKPT_TOP=1` (2026-10-13, VERIFY OK, first 10¹¹ digits identical), `ECALC_RECHECK=1` printed `bs: checkpoint: short read from <out>.top/n00X_tree_002.rK` for 16 of 24 files (all 6 nodes), fell back to the sidecar's P, Q residues and was killed at its 1800 s timeout. Incomplete set, wrong size or layout, or the file system? Why so slow?

**Change / branch.** Branch `ckpt` (from main 39346d0); only the top-set writer and reader use `sp_file` (`ecalc/spill.c`). `pread_need`: one `pread` asks for at most 64 MiB (`SP_READ_MAX`; it asked for up to 512 MiB); EIO / ETIMEDOUT / EAGAIN / ENOMEM are retried after 1, 4, 16 s (3 failures in a row fail it, said on stderr); an O_DIRECT read that comes back short at a partial block before the bytes needed is re-read from that block. `pwrite_full`: the same retry. `binsplit.c`: a failed read names errno, offset, bytes got / needed, file size. No switch (reads that came back whole return the same bytes).

**Method.** Logs: the writing run printed `checkpoint: the top-level P, Q ... 57.25 GB in 1027 s` on every node (the header comes only after all four files close cleanly), and the recheck found the set by `ckpt_find_kind`, which checks file sizes against the header, so the files were complete. Queue items `40_ckpt_repro` (2 nodes, main + diagnostics; write then recheck at 10¹⁰ and 2 × 10¹⁰), `41_ckpt_mount` (mount options), `42_ckpt_gate` (16 MiB cap) and `43_ckpt_gate2` (64 MiB, merged) with `tools/gate.sh --build --nodes 2` and `--nodes 1` plus the 2-node 2 × 10¹⁰ write + recheck.

**Results** (m unless marked):

| run | set per node | files vs header | recheck | part-file pass | top-set read |
|---|---|---|---|---|---|
| 2 nodes, 10¹⁰, main + diagnostics | 4.44 GB | 8/8 equal | OK, 0 short reads, 281 s | 164–170 s (12.5 MB/s per node) | 102–108 s (41–44 MB/s) |
| 2 nodes, 2 × 10¹⁰, main + diagnostics | 8.89 GB | 8/8 equal | OK, 0 short reads, 527 s | 332–336 s (12.5 MB/s) | 180–185 s (48–49 MB/s) |
| 2 nodes, 2 × 10¹⁰, 16 MiB cap | 8.89 GB | — | OK, 0 retries, 679 s | 317–370 s | 300–353 s |
| 2 nodes, 2 × 10¹⁰, 64 MiB cap (merged) | 8.89 GB | — | OK, 0 retries, 693 s | 480–481 s | 203–204 s |
| 6 nodes, 3.8646 × 10¹¹ (2026-10-13) | 57.25 GB | equal | 16 of 24 files short read, killed at 1800 s | | |
| 6 nodes, recheck's need (mod) | | | ≈ 1 h at 2-node rates (26.8 GB parts at 12.5 MB/s ≈ 2140 s + 57.25 GB at 48 MB/s ≈ 1190 s); slower when 6 nodes share the server | | |

Gates 16 MiB (e339948) and 64 MiB (544b863): 2 nodes PASS (10⁹, VERIFY OK, identical, P24 in use) and 1 node PASS (m). Single runs on a shared file system (the unchanged part-file pass took 317–481 s across the three 2 × 10¹⁰ rechecks), so read times compare only roughly: 16 MiB read the set ≈ 1.7 × slower than 512 MiB, 64 MiB ≈ 1.1 × (m, one run each, no CI).

Cause: compute nodes mount the home file system NFS soft (`timeo=600,retrans=2`, NFSv3, rsize 64 KiB; the login node hard). A soft mount fails an RPC with EIO when it gets no answer within its timeouts. The reader had four threads per node each with one O_DIRECT `pread` of up to 512 MiB in flight: 2 GiB per node, 12 GiB over 6 nodes, against ≈ 0.3 GB/s delivered in all (6-node write 56 MB/s per node, m), ≈ 40 s of queue (mod), so the tail passed the 60 s timeout; the old message called the EIO a short read. At 2 nodes the queue is a third of that and nothing failed; with 64 MiB requests the 6 nodes have 1.5 GiB in flight, ≈ 5 s of queue (mod). The large run's nodes were rebooted, so kernel logs could not be checked: the mechanism is inferred. The 30 minutes: the recheck reads every part file (26.8 GB per node, packed) then the set (57.25 GB per node); the recurrence over a node's terms takes ≈ 1.1 s at 2 × 10¹⁰ (m), ≈ 7 s at 3.86 × 10¹¹ (mod); 1800 s could not have been enough.

**Verdict.** A reader bug (one huge read on a soft-mounted NFS), not a bad set; fixed by smaller reads plus retries, merged as a correctness fix (last code commit 544b863). A large run's recheck needs a timeout of ≥ 3 h on aac7 (TESTING.md §3).

**Proof at scale (2026-10-14, commit 34175cf, 6 nodes, 3.8646 × 10¹¹, `ECALC_CKPT_TOP=1`, one run, m).** Whole run 1473.26 s, VERIFY OK, first 10¹¹ digits identical; standalone `ECALC_RECHECK=1`: RECHECK OK on all nodes (7 OK, 0 FAILED), 0 `spill: read at offset` retries, wall 7380 s (2 h 3 min) against ≈ 3330 s modelled (2140 s parts + 1190 s set): the model was low by ≈ 2.2 ×, as 6-node file-system rates are below the 2-node rates (mod). The 16 short reads did not recur. TESTING.md §3 keeps ≥ 3 h for the 6-node large run (twice the model, 2 h, would be below the measured 2 h 3 min).

**Open issues.** The part-file pass at 12.5 MB/s per node (a quarter of the set's O_DIRECT rate) is the recheck's main cost (TASKS "Proposed" candidate). The writer waited 499 s for P before S = P + Q at 3.8646 × 10¹¹ (57 GB at 56 MB/s per node): `=1` costs run time on this file system, `=2` drops the set instead. Nothing else open.
