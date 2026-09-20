# T — tests, checkpoints and docs (Phase 10, PLAN.md §21: D3, D2, D4, D5, C6, E3)

Branch `t10` (from `main` @ 4aca721 = 38ed61b + the PLAN §20–22 commits; the aac6 clone `~/ntt-t`).
Files: `ecalc/mnaccept.sh` (new), `ecalc/t10_test.sh`, `t10_d5.sh`, `t10_d5b.sh` (new: this report's runs), `ecalc/accept.sh`,
`ecalc/variance.sh`, `ecalc/mn.c` (the tree checkpoint loop and `mn_finalize`), `ecalc/binsplit.c`
(`bs_ckpt_tree_remove_below` and a comment), `ecalc/README.md`. Nothing else touched; the numerics are
unchanged (the C6 change moves a barrier and a set removal).

## D3 — the standing regression: `ecalc/mnaccept.sh <jobid> [--full] [--only unit,e9,mn,ckpt,full]`

Run from `ecalc/` of any clone on the login node against a one-node allocation. Steps, in order, each
with its own log under `results/mnaccept/<jobid>/` and one `PASS`/`FAIL` line:

| step | what | pass when |
|---|---|---|
| unit | `t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`, `t_newton 20`, `t_verify`, `t_out` on the node; `t_mn_grid 1 28` at 2 node-processes through `mnrun.sh` | exit 0, a `VERIFY OK` line, no `VERIFY FAILED` |
| e9 | 10⁹ at size 1, `LIMB_BASE=10` and `2` | exit 0, `VERIFY OK`, `cmp` identical to `ref/e_1000000000.txt` |
| mn | 10⁸ at sizes 2, 3, 4 (`POOL_LOG=27`), 10⁹ at sizes 2, 4 (`POOL_LOG=29`), one node | exit 0, `mn: all n nodes: VERIFY OK`, `cat` of the part files identical to the reference |
| ckpt | 10⁸ size 2: `BS_CKPT_EVERY=2 BS_CKPT_MIN_LEVEL=2 BS_CKPT_ABORT=6` (both nodes exit after their leaf level 6 set), then `BS_RESTART=1` | 2 nodes exited; the restart exit 0, all nodes VERIFY OK, identical |
| full (`--full`) | 4 × 10¹⁰ at size 1, `results/e_4e10.out` evicted from the page cache first; the `total` line and the elapsed time with the write are printed | exit 0, `VERIFY OK`, identical to `results/e_4e10.out` |

The summary line counts passes and failures and the exit status is the number of failures. The digits
go to the node's `/tmp/mnaccept_<jobid>` (removed at the end); the references are the clone's `ref/`
when present, else `ECALC_REF` (`~/ntt/ecalc/ref`; the files are linked into the clone's `ref/` so
`t_bs` finds its `.sha256` files) and `ECALC_REF_4E10`. Duration ≈ 31 min with `--full` (t_ntt 24
alone ≈ 10 min, t_mul 20 ≈ 5 min; the 4 × 10¹⁰ run 87 s + 90 s of writing 40 GB to the node's NVMe).

**Runs.** Job 20761 (s24-30, `main` @ 38ed61b + the script, 2026-09-20 02:01), `./mnaccept.sh 20761 --full`,
31 min: t_ntt 24 (565 checks), t_mul 20 (189), t_dbig 0 (555), t_newton 20 (620), t_verify (334), t_out (1 line,
2 860 checks inside), t_mn_grid at 2 procs (120 on each rank), 10⁹ base 10 (12.2 s) and base 2 (21.5 s) identical,
ckpt 10⁸ size 2 (both nodes exited at leaf level 6, restart from level 6, 453 nodes, identical), **4 × 10¹⁰: total
86.64 s (bs 38.4, dm 34.8, init 13.3), VmHWM 48.8 GB, VERIFY OK, identical to `results/e_4e10.out`** (174 s
elapsed with the 40 GB write to the node's NVMe). Two steps failed on script defects, both fixed in 138c5a5:
`t_bs` compares SHA-256 against `ref/e_<d>.sha256`, absent in a fresh clone (now linked from `ECALC_REF`), and
the multi-process tags carried spaces into the file names (the runs themselves were VERIFY OK). The five `mn`
steps rerun in the same job with the fixed script and the same binary: 10⁸ sizes 2/3/4 (8.1 / 9.0 / 8.0 s) and
10⁹ sizes 2/4 (27.4 / 20.3 s), all identical, all nodes VERIFY OK. **The whole script again on the branch's final build (mn.c with C6), job 20771 (s24-30, 03:43): 17 passed,
0 failed in 1828 s** — the same checks, 10⁹ 13.1 / 21.4 s, 10⁸ sizes 2/3/4 8.0 / 8.7 / 8.0 s, 10⁹ sizes 2/4 27.1 /
19.9 s, ckpt restart from leaf level 6, 4 × 10¹⁰ total 89.49 s (bs 38.3, dm 34.4, init 16.7) VmHWM 48.8 GB identical.
So the script is green on `main`'s binary (job 20761 + the mn rerun) and on the branch (job 20771); its only
dependency outside the tree is `~/ntt/ecalc/ref` and `results/e_4e10.out`.

## C6 — the multi-node checkpoints: `BS_CKPT_TREE_EVERY`, the barrier off the write

Before: `mn_tree` wrote a tree set at every level, then `mn_barrier()`, then removed the superseded set
— every node waited for the slowest node's write at every tree level. Now (`mn.c`):

- `BS_CKPT_TREE_EVERY=k` (default 1): sets at tree levels k, 2k, … and always at the top level L.
- The set of level l is written with no barrier after it. The barrier is taken at the *next* set level,
  after that level's product and before its write, and then everything below the previous set (the
  lower tree sets, the leaf sets: `bs_ckpt_tree_remove_below` now clears every lower tree level, since
  `EVERY` leaves gaps) is removed. Every node wrote the previous set before entering the next level's
  product, so that barrier waits for the nodes' compute, never for a write. The last set's predecessors
  are removed at `mn_finalize`, which the driver calls after the run's final `mn_barrier()` on every path.
- The restart rule is unchanged (`mn_ckpt_tree_level`: the lowest, over the nodes, of each node's
  highest complete tree level; sets above it cleared) and still sound: a node removes the sets below
  level m only after a barrier that every node reached having completed its write of m, so the lowest
  "highest complete level" over the nodes is present on all of them at every instant. Between a write
  and the next barrier a node holds two tree sets (or the leaf sets plus one tree set): at 4 × 10¹⁰
  and size 2 that is 2 × 17 GB on disk for the top level's duration.
- `BS_CKPT_ABORT_TREE=l` exits right after the write, as before (before any barrier).

**Runs** (job 20765, s24-16, `t10_test.sh 20765 c6`; 10⁸, `POOL_LOG=27 BS_CKPT_EVERY=2 BS_CKPT_MIN_LEVEL=2`,
sizes 4 and 3; every restart `cmp` identical to `ref/e_100000000.txt` and to the uninterrupted run (a), every node
VERIFY OK):

| run | size 4 | size 3 |
|---|---|---|
| a uninterrupted: leaf sets 8 / 9 per node, tree sets at levels 1, 2 (0.021–0.029 GB each) | identical | identical |
| b `BS_RESTART=1` from the top tree set (leaf skipped, straight to the division) | identical (level 2) | identical (level 2) |
| e `BS_CKPT_ABORT_TREE=1` (every node exits after writing tree level 1; leaf sets still present — no barrier has passed) → f restart | identical (from tree level 1) | identical |
| g node 1 exits after tree level 1, the others blocked in level 2's product and killed by `timeout` → h restart | identical (all nodes have level 1) | identical |
| t `timeout 6` (killed while tree level 1 was being written) → u restart | identical (from tree level 1) | identical |
| v `BS_CKPT_TREE_EVERY=2 BS_CKPT_ABORT_TREE=2`: only the top set (level 2) is written, level 1 skipped; the leaf sets are removed at level 2's barrier → w restart | identical (from level 2) | identical |

At 10¹⁰ size 4 (D2 below) the tree sets are 2.1–2.3 GB per node written at 0.7–1.2 GB/s (four processes on one
NVMe), the removed barrier after each write would have cost the slowest write (2–3 s) per level.

## D2 — restart at 10¹⁰ from tree-level sets at size 4

Job 20765, `t10_test.sh 20765 d2`: `POOL_LOG=29 BS_CKPT_DIR=/tmp/... ./mnrun.sh 4 ./ecalc 10000000000 <out>`
(the leaf sets at the defaults, levels 16 and 20: 4 per run, 8.5 GB, 3.4 s each; tree sets at levels 1 and 2
per node, 2.1–2.3 GB, 1.8–3.1 s each). Parts concatenated and `cmp`'d against `results/e_1e10.out` and against
each other:

| run | result |
|---|---|
| d2a uninterrupted with checkpoints | identical; total 130.5 s (bs 48.6 incl. 13.7 s of leaf sets and 5 s of tree sets; the distributed division over loopback TCP), all 4 nodes VERIFY OK |
| d2c `BS_CKPT_ABORT_TREE=1`: all 4 nodes exit after writing tree level 1 (leaf level 20 and tree level 1 sets on disk) | 4 nodes exited at 37 s |
| d2d `BS_RESTART=1`: every node has level 1 → restart from the tree level 1 shares (P, Q 268 M limbs, shares 134 M), level 2 and the division | identical to d2a and to the reference; total 105.2 s (bs 20.8); the leaf sets and level 1 removed at level 2's barrier / mn_finalize, only tree_002 remains |
| d2b `BS_RESTART=1` from d2a's top-level sets (level 2): the leaf tree and the tree skipped, straight to the division | identical; total 80.3 s (bs 0.4, dm 67) |

## D4 — the binary path at 10¹⁰

Job 20765, `LIMB_BASE=2 ./ecalc 10000000000 <out>` at size 1 (the paper's binary limbs, the radix conversion
on the device): VERIFY OK, **identical to `results/e_1e10.out`** (the decimal path's file) — total 56.2 s (bs 6.1,
recip 4.8, 10dP 2.4, dm 6.8, T1 2.3, dc 23.7, T2 0.3; init 13.0), VmHWM 124.9 GB. 10⁹ in both bases is in the
regression (e9 step).

## D5 — the stale-plane-pointer suspicion (A-mem open issue 1)

**A-mem's observation**: at 10¹⁰ size 4 with `POOL_LOG=29` and pool 1 at 3 q (3 GiB, the batch-local tier's 4 GiB tile
grows it inside the phase), two runs failed T1; with pool 1 at the full pool nothing grows and every run is identical.
Their suspicion: a stale plane pointer in the batch tier after the growth.

**What the code says** (read-only, `rns_mul.c`): in both batch tiers the `rns_dpool` growth calls precede every
pointer taken from the pools inside the same parallel region (`da`, `db` at lines 687 and 839; `k_crt_batch` in
the striped tier reads `D[0..3].da.p` after the `omp barrier` that follows every thread's growth call; pool 0 never
grows there — the tile is bounded by it), and no pointer into a plane pool survives a call: the mdev tiers, the
dist tier (`rns_dist.c` 207, 578) and the `dist_plan` slabs re-request the pools on every call. The one cached
pointer into pool 1 is the `dist_plan`'s `sbuf`/`rbuf` (`rns_dist.c` 213 / 585: the plan is rebuilt only when
`logR`, `logC` or the communicator change, not when pool 1 moved) — a hazard if pool 1 grows *between* two dist
products of the same shape; the dist tier's own requests at `POOL_LOG` 29 (≤ 3 q + 16 limbs) fit the 3 GiB pool,
so that path is not exercised in this configuration either.

**Reproduction** (`t10_test.sh d5`, `t10_d5.sh`, `t10_d5b.sh`; `POOL_LOG=29 RNS_POOL1_GB=3.2213` = pool 1 at exactly
3 GiB, `RNS_VERBOSE=1` shows the pools at 4.00 + 3.00 GiB and the batch-local tiles at 2²⁹ limbs, so the growth
happens at the first batch level of every run):

| run | result |
|---|---|
| d5a 10¹⁰ size 4 | identical, VERIFY OK on all nodes (133 s) |
| **d5b 10¹⁰ size 4** | **VERIFY FAILED on all 4 nodes, digits differ from the reference**; T1: `P BAD, Q BAD, T(P+Q) == XQ+R BAD` at q0, q1, q3, q4, q5, q7 and `ok` at q2, q6; `digits == X mod q ok` at all 8 |
| d5c 10⁹ size 4 `POOL_LOG=27 RNS_POOL1_GB=0.8054` (the same growth at 2²⁷) | identical |
| d5d 10⁹ size 1 `POOL_LOG=29`, pool 1 at 3 GiB | identical |
| `t10_d5.sh`: 8 runs of 10¹⁰ size 4 with the growth, exiting after tree level 1 (4 runs) and after tree level 2 (4 runs) with the tree sets kept | **every node's P and Q shares after tree level 1 and after level 2 are byte-identical across the runs** (32 + 64 file comparisons) |

So in 9 of 10 complete or bs-complete runs with the growth forced, the bs phase (the batch tier included) was
right, and the 8 kept sets show bs is deterministic under the growth — the batch tier does not produce a wrong
P from the growth in any run observed here. The one failure has exactly the T1 signature of A-mem's two
(`amem_bench/batch4/e1e10_s4.log`, `batch5/amem_s4_nb.log`: BAD at the same six primes, ok at q2 and q6): a
wrong big number has wrong residues at every 62-bit prime with overwhelming probability, so this per-prime
pattern is not the signature of a wrong P or X — it points at the T1 path itself at size > 1 (the residues of
P, Q by `mdb_mod_qs`, or the joined term recurrence `mn_out_pq_combine`, small vectors all-gathered through
`mn_out_allgather_u64`, which allocates and frees its device buffers per call), with the digit difference a
second symptom of the same run.

**Series b** (`t10_d5b.sh`, job 20785, 11:23): 8 more full runs of 10¹⁰ at size 4 with the growth forced, the digits
kept — **8 of 8 identical to the reference, VERIFY OK on every node** (109–134 s each).

**Verdict.** With the growth deliberately forced at every run (pool 1 at 3 GiB, the batch-local tier's first 2²⁹-limb
tile grows it to 4 GiB — 18 runs of 10¹⁰ at size 4, plus 10⁹ at sizes 1 and 4 with the same growth): the batch tier
gave the right P, Q in every run whose bs output could be checked (8 of 8 kept tree sets identical across runs, 17 of
17 digit files identical), so **a stale plane pointer in the batch tier after a pool growth is not reproducible**;
the code reading (above) finds no pointer into a plane pool that outlives the growth on that path — no line to
name, no fix. The one failure in 18 (d5b) carries A-mem's exact T1 signature (`P BAD, Q BAD` and the identity BAD
at q0, q1, q3, q4, q5, q7 and ok at q2, q6 — three failures, three times the same six primes), which a wrong
big number cannot produce: the residues of a wrong P would differ at every 62-bit prime. That signature lives in
the size > 1 verification path (`mdb_mod_qs` in `newton_db.c`, the joined recurrence in `mn_out.c`) or in whatever
produced it together with the wrong digits, is intermittent (once in 18 here, twice in A-mem's few), and is A-div's /
A-out's code — recorded as an open issue with the log (`~/ntt-t/ecalc/results/t10/20765/d5b.log`), not chased
further. It is not exercised at the defaults' pool sizes any more than the growth is: all three failures were in
runs with the growth, none of the ≈ 15 default-configuration runs at 10¹⁰ size 4 failed (A-mem's 3, the
integrator's, D2's 4, C6's) — so the growth (a 3 GiB `hipFree` + 4 GiB `hipMalloc` per APU inside bs, moving every
later allocation) most likely exposes a use of freed or uninitialised device memory elsewhere rather than in the
tier that grows the pool. `results/A-mem.md`'s rule stands: pool 1 at the full pool for `POOL_LOG` ≤ 30, so nothing
grows inside a phase.

## E3 — `ecalc/README.md`

The header paragraph carries the Phase 9 defaults and numbers (86.4 s / 48.8 GB at 4 × 10¹⁰; 7 × 10¹⁰);
the file table gained `mn.c`/`mdb.h`, `mn_out.c`, `mem_report`, the scripts; new sections: the
multi-node run (`mnrun.sh`, the `COMM_*`/`MN_*`/`DIST_*`/`MEM_REPORT_DEVS`/`POOL_LOG` knobs, the part
files, the per-node checkpoints with the C6 rules) and the standing regression. `accept.sh` and
`variance.sh` run from their own clone (`cd "$(dirname "$0")"` instead of `~/ntt/ecalc`); `accept.sh`
adds `t_dbig 0` and `t_out` to its unit list.

## Open issues

- **D5's one failure (d5b) is unexplained**: 1 of 18 forced-growth runs at 10¹⁰ size 4 failed with A-mem's T1
  signature (six primes BAD, q2 and q6 ok, both P and Q) and different digits; not in my files (the residue path of
  `newton_db.c` / `mn_out.c`), not reproducible on demand (8 + 8 + 1 runs after it were right). A run with the
  residues printed per node before the combine (`mdb_mod_qs`'s `v[]` per node and the recurrence's per-node
  (P_r, Q_r)) would say which side of the T1 comparison is wrong the next time it happens; the growth
  configuration (`RNS_POOL1_GB=3.2213 POOL_LOG=29` at size 4) is the way to provoke it.
- C6: between a tree set's write and the next set level's barrier a node holds two sets (≈ 2 × 35 GB / size at
  4 × 10¹⁰); with `BS_CKPT_TREE=1` and one node per real node this is the leaf set plus the top set on local disk
  until the end of the run (the leaf sets are removed at the first tree set's barrier, which at size 2 is
  `mn_finalize`).
- `BS_CKPT_TREE_EVERY` at sizes beyond 4 (three or more tree levels, so a level is actually skipped between two
  sets) is exercised only in the size-4 `v/w` runs (level 1 skipped, the top written); the removal logic is the
  same at any depth.
- The regression's `t_ntt 24` takes 10 of its 31 minutes (the O(n²) reference convolution runs single-threaded
  under `srun`); `--only` skips steps when the time matters.
- The multi-node checkpoint at 10¹⁰ size 4 wrote the leaf sets at the defaults (levels 16 and 20, 8.5 GB in
  13.7 s of bs) — at 4 × 10¹⁰ per node those are ≈ 35 GB / size each; `BS_CKPT_MIN_LEVEL` above the leaf's top
  level leaves only the tree sets.
