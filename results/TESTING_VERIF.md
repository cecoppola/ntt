# TESTING_VERIF — verification, correctness sweeps and test design

Scope: verification, correctness sweeps and test design: per-phase verification, inter-node correctness, weak-memory audit, t_mn_grid over SOS. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

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

## V — verification and cleanup (Phase 11, PLAN.md §26 row V: D5, ECALC_RECHECK, E1 (c), E2)

(former file: V.md)

Branch `v11` (from `main` @ 72aa2e9; the aac6 clone `~/ntt-v`). Files: `ecalc/verify.c`, `ecalc/mn_out.c/.h`, the
residue kernel of `ecalc/dbig.c` (+ `dbig.h`), the output tail and the T1 flow of `ecalc/ecalc.c`, `ecalc/tests/t_dist.c`,
`ecalc/ntt_dist.c/.h` (the two deleted functions), the seeds of `ecalc/binsplit.c` (the deleted path) plus a
commented instrumentation block there, `.gitignore`, `ecalc/README.md`, the scripts `archive:drivers/ecalc/v11_d5.sh`, `archive:drivers/ecalc/v11_recheck.sh`.
Touched outside my list, minimal and commented `Phase 11 V`: `ecalc/newton_db.c` (five lines of logging in
`mdb_mod_qs`), `ecalc/mem.c` (`MEM_DPOOL_FILL`, a test knob in `dpool_get`), `ecalc/binsplit.c` (the level loop calls
`bs_res_check` once per level; the leaf hand-over check).

#### D5 — the intermittent size > 1 verification failure

**What the signature was.** Three failures (A-mem ×2, T's d5b) printed `P BAD, Q BAD, T(P+Q) == XQ+R BAD` at q0, q1,
q3, q4, q5, q7 and `ok` at q2, q6, and the digits differed from the reference each time. PLAN §23-6 read the
per-prime pattern as "the residue path's fault, not a wrong number". It is neither: **seven of the eight "primes"
in `verify.c` were composite.** The table held 2⁶² + {135, 179, 183, 247, 315, 319, 349, 397}; only 2⁶² + 135 is
prime (RESULTS.md's own table lists the first eight primes above 2⁶² as 2⁶² + {135, 169, 177, 187, 189, 193, 253,
277} — the code never matched it). Two of the composites, q2 = 2⁶² + 183 = 11 · 1847 · 41641 · 41813 · 130367 and
q6 = 2⁶² + 349 = 1187 · 3911 · 40583 · 24478063, have every factor below 2.5 × 10⁷, so every node's
Q(a, b) = ∏ k over ≥ 2.9 × 10⁸ consecutive integers is **0 modulo q2 and q6** (the first `ECALC_RES_LOG` run showed
`Q mod q2 = 0`, `Q mod q6 = 0` on all four nodes). At those two moduli T1 is blind: `Q == recurrence` is 0 == 0,
the identity T (P + Q) ≡ X Q + R reduces to T P ≡ R (X drops out), and P = P_A Q_B + P_B ≡ P_B sees only the top
node's range. So a wrong P or Q from any node **below the top** is caught at the six moduli that can see it and
passes at q2, q6 — exactly the observed pattern, three times. The signature is that of a genuinely wrong bs result
from node 0, 1 or 2, correctly flagged by the checker; the "checker fault" reading was wrong.

**Fix (verify.c).** `t1_q` is now the true first eight primes above 2⁶² (2⁶² + 135 … 277), with the history in a
comment. Consequences: the T1 numbers, the `RES` logs and the `.t1` sidecar residues change; the digits do not
(the moduli only enter the checks) — 10⁹ and 4 × 10¹⁰ identical below. With prime moduli larger than N, Q ≢ 0 at
every one of them, so T1 sees P, Q and X at all eight, and a wrong P now shows as `BAD` at all eight (batch 2 on).

**The real fault, reproduced and localised (the instrumentation `ECALC_RES_LOG=1`).** Every residue the checks use
is printed and cross-checked: the kernel `db_mod_qs` against a host Horner over a copy of the number (dbig.c), the
background recurrence against a main-thread recomputation, each node's share before the cross-node sum
(`mdb_mod_qs`), the joined P/Q, X's share and sum, the digit residues; then (batches 2–5) each node's **leaf P_r, Q_r
against the recurrence over its own term range** before the tree, the leaf hand-over copy, and from
`ECALC_RES_LOG_LEVEL` (17) on every node of every leaf level (`bs_res_check` in binsplit.c). `ECALC_LEAF_DUMP=<dir>`
writes the leaf P_r, Q_r as raw limbs (and, with the level check, the first wrong node and its four children).
T's recipe (10¹⁰ at size 4, `POOL_LOG=29 RNS_POOL1_GB=3.2213`: pool 1 at 3 GiB, grown to 4 GiB by the first
batch-local level) reproduces the failure at ≈ 1 in 5 here, not 1 in 18:

| batch (job) | runs | result |
|---|---|---|
| 1 (20803, old moduli, RES log) | 5 (shard/host alternating) | 4 identical; **shard5 failed**: digits differ from byte 2 287 655 016 on, T1 BAD at the six non-degenerate moduli, ok at q2/q6; every kernel residue agreed with the host Horner, the background recurrence with the main thread, digits == X at all eight — a wrong P **and** Q (every share's residue differs from a good run's) |
| 2 (20803, new moduli, leaf check) | 4 | 2 identical; **b2shard3: node 0's leaf P/Q BAD** at all 8 moduli (digits differ from byte 2 288 684 254); **b2shard4: node 1's leaf BAD** (from byte 4 786 297 548) — the other three nodes' leaves agree with the recurrence in both |
| 3 (20803, level checks ≥ 17) | 5 | 5 identical, every level ≥ 17 of every node ok |
| 5 (20811, no level checks, `RNS_BATCH_LOCAL_MIN=1`, leaf dumps) | 13 | 11 identical; **b5shard11: node 1's leaf wrong** — vs a good run's leaf: P differs from limb 131 584 to the top (138 758 021), Q from limb 4 092 871 to the top; **b5shard12: node 0's leaf wrong** — P from limb 131 072, Q from limb 4 092 358 to the top; the other nodes' leaves byte-identical to the good run's |

| 6 (20820, level checks ≥ 11: every node of levels 11–21 reduced by the kernel after the level, i.e. a stream synchronisation and a host copy per level) | 12 | 12 identical, every level of every node ok |

So without per-level checks: 21 identical and 5 failed of 26 forced-growth runs, every failure a **wrong leaf P_r and Q_r of node 0 or node 1**
(never 2 or 3), wrong from a limb of the order 2¹⁷ (P) / 2²² (Q) to the top, the tree, the division, the residues
and the digits all consistent with that wrong leaf; the hand-over copy (region → device number) agrees each time.
With the per-level checks on (batches 3 and 6): 17 of 17 identical — at the 5/26 rate the chance of 17 clean runs is
0.81¹⁷ ≈ 3 %, so the check's synchronisation between levels most likely **hides the fault: a timing-dependent (race)
error, not a deterministic one**. Its shape — the product wrong from one limb position to its top, both P and Q of the
same node, the position of the order of a stripe boundary of the children's products — is what a single wrong operand
limb (or a stale read of one) at that position produces. The fault is inside the leaf's level loop (`binsplit.c` +
`rns_mul.c`'s batch tiers: the CPU `spill_merge` writes into the device regions between the CRT kernel and the next
level's scatter kernel are the one CPU-to-GPU hand-over there), under the forced growth of plane pool 1 — the configuration A-mem's rule excludes (`results/MEMORY.md §A-mem`: pool 1 at the full pool for
`POOL_LOG` ≤ 30, so nothing grows inside a phase); none of the ≈ 15 default-configuration runs at 10¹⁰/4 and none
of the regressions failed. Batch 7 (job 20830, `ECALC_RES_LOG_LEVEL=11 ECALC_RES_LOG_CPU=1`: the per-level check reads the regions with the CPU
only — no kernel, no stream synchronisation — and would dump the first wrong node with its four children):
**14 of 14 identical**. So 31 of 31 runs with a per-level check (any kind: a kernel reduction or a CPU read of the
regions between levels, ≈ 2 s per level) against 21 of 26 without — a 0.15 % chance if the check were neutral.
The fault is a **timing-dependent error in the leaf's level loop under the in-phase growth of plane pool 1**, hidden
by anything that separates one level's end from the next level's start. It is not in my files (binsplit.c's level
loop and rns_mul.c's batch tiers: P / N-kernel); the reproducer is `v11_d5.sh <job> 14 shard` (≈ 1 failure in 5, the
wrong node named by the leaf check, the wrong limb range by the leaf dump against a good run's), and the first
thing to look at is the one CPU-to-GPU hand-over inside the tier, the CPU `spill_merge` writes into the device
regions at the stripe boundaries followed by the next level's scatter kernel: "wrong from one stripe-boundary-like
limb to the top" is what a stale or late operand limb there produces. I did not change that code.

**Verdict for §23-6, plainly.** (1) The checker was not at fault and is now stronger: the digits were wrong in every
failing run (T.md records it; PLAN §23-6's "the digits were right in every failing case" was mistaken), the checker
caught them, and the per-prime signature that suggested a checker fault was the composite-moduli artefact — seven
composite moduli, two of which divide every Q — now replaced by the eight true primes (commit 66ee599). (2) There
**is** a second, real fault: a timing-dependent corruption of a leaf-level product on node 0 or 1 of 10¹⁰/4 under the
forced in-phase growth of plane pool 1 (5 of 26 unperturbed runs; 0 of 31 with a per-level probe), in the batch tier's
level loop — **open**, not in my files, reproducer and localisation tools delivered; it does not occur at the
defaults (A-mem's rule: no growth inside a phase; ≈ 15 default runs at 10¹⁰/4 and every regression clean). The gate
"20 forced-growth runs all VERIFY OK" is therefore met only with the probe on (31 of 31) and not met without it
(21 of 26) — I report both rather than the one that passes.

#### `ECALC_RECHECK=1` — the standalone recheck (mn_out.c)

The run writes `<outfile>.t1` (node 0): N, d, d_out, size, the moduli, the residues of X, R, P, Q it checked with, and
the d − d_out computed digits after d_out (captured in `mn_out_run` from the last chunk). `ECALC_RECHECK=1 ./ecalc
<digits> <outfile>` (through `mnrun.sh <size>` at size > 1, with the run's `BS_CKPT_DIR`) then, without pools or
computation (it returns right after `mn_init`): reads each node's part file in 256 MB chunks (the digit residues by
Horner, the counts and 49-char tails all-gathered → the nodes' digit ranges and the T2 heads), forms X mod q =
D · 10^(d − d_out) + tail from the file, checks the T2 windows over the file, takes P and Q mod q from the
checkpointed top-level shares (`bs_ckpt_tree_read` of tree level ⌈log₂ size⌉; at size 1 the level-0 tree set that
a run writes with `ECALC_CKPT_TOP=1` — new: after bs, `bs_ckpt_tree_write(0, …)` of `bs_Pd`, `bs_Qd`), each share's
`db_mod_qs` placed at its offset and summed over the nodes, recomputes the term recurrence per node and joins it,
and runs T1 with R's residues from the sidecar (the one value that cannot be recomputed without redoing the
division) plus `digits → X mod q == the run's X residues` and `P, Q from the checkpoint == the run's`. Prints
`RECHECK OK` / `FAILED` per node and `mn: all n nodes: …`. Without a checkpoint it says so and takes P, Q from the
sidecar (then only the file and the recurrence are independent).

Tests: `v11_recheck.sh` — 10⁸ at sizes 1, 2 and 10⁹ at sizes 1, 2: the run with checkpoints, the recheck, and the
recheck of a file with one digit flipped (must fail). Results below.

#### Formats that changed

- The T1 moduli (`t1_q`): every residue printed by T1/RES lines and stored in the `.t1` sidecar changes; the digits and
  every file format the checkpoints use do not (the moduli never enter them). A sidecar records its moduli and the
  recheck refuses one written with others.
- New: `<outfile>.t1` (node 0 writes it after the checks; text, ≈ 400 bytes). New, optional: the size-1 top-level set
  `tree_000.{hdr,r0..r3}` in `BS_CKPT_DIR` with `ECALC_CKPT_TOP=1` (level 0 is never seen by the restart logic, which
  scans levels ≥ 1). The multi-node tree sets are unchanged.
- `mn_out` gained `tail[24], ntail` (the computed digits after d_out); `struct pq_bg` a `joined` flag.

#### E1 (c) — deleted

`DIST_PLANE2` (`dist_fwd2`, `dist_inv2` in ntt_dist.c/.h and `t_dist`'s `plane2` mode), `BS_SEED_DIRECT=0` (the per-chunk
DMA path of `seeds_stream`: a buffered chunk is now always copied once the regions exist, the direct stores are the
only path) and the `ECALC_OVERLAP_COPY` remnant (a comment; its code was already gone). The one-line switches stay.
**The host-flow stand-ins `MN_DM=host` / `MN_COMBINE=host` are kept**: D5's residual fault is in bs, not closed, and
`MN_DM=host` remains the only cross-check of the sharded division against the host one (PLAN §23-1 (a)).

#### E2

`.gitignore`: `results/*` + `!results/*.md` (the write-ups tracked by rule; `archive:results_raw_2026-10-19/wp6/` etc. still ignored).

#### Gate runs

**Regression** (job 20818, `./mnaccept.sh 20818 --full` on the branch @ 45ef12e — the new moduli, the recheck code, the
deletions): **17 passed, 0 failed in 1904 s**: t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify (334), t_out,
t_mn_grid at 2 procs; 10⁹ size 1 base 10 (14.3 s) and base 2 (25.1 s) identical; 10⁸ sizes 2/3/4 and 10⁹ sizes 2/4
identical, all nodes VERIFY OK; ckpt restart identical; **4 × 10¹⁰ size 1: identical to `results/e_4e10.out`, VERIFY OK,
total 86.21 s** (171 s with the write). So the moduli change left the digits bit-identical at 10⁹ and 4 × 10¹⁰.

**Recheck mode** (job 20818, `v11_recheck.sh`; each: the run with `BS_CKPT_DIR`, the recheck, the recheck of the file
with one digit flipped):

| case | run | recheck | corrupted file |
|---|---|---|---|
| 10⁸ size 1 | VERIFY OK, identical | RECHECK OK: 100 000 001 digits read in 0.1 s, 2 windows, digits → X == the run's, P, Q == the recurrence, T1 ok (P, Q from the sidecar: the level-0 set is written only by the device flow's leaf in that build; a568823 adds the host-flow leaf — below 4 × 10¹⁰ the size-1 leaf ends on the batch tier — not re-run) | RECHECK FAILED (`digits == X BAD`) |
| 10⁸ size 2 | 3 VERIFY OK, identical | 3 RECHECK OK: P, Q **from the checkpoint** == the recurrence == the run's; T1 ok | 3 RECHECK FAILED |
| 10⁹ size 1 | VERIFY OK, identical | RECHECK OK (1.3 s for the file; P, Q from the sidecar, as above) | RECHECK FAILED |
| 10⁹ size 2 | 3 VERIFY OK, identical | 3 RECHECK OK, P, Q from the checkpoint | 3 RECHECK FAILED |

**Forced-growth runs, the exact count** (10¹⁰ at size 4 on one node, `POOL_LOG=29 RNS_POOL1_GB=3.2213`, this branch):
57 runs — 26 without a per-level probe: 21 identical + VERIFY OK on every node, 5 VERIFY FAILED with a wrong leaf
(batches 1, 2, 5: shard5, b2shard3, b2shard4, b5shard11, b5shard12; the `MN_DM=host` runs of batch 1, 2 of 2, were
identical); 31 with a per-level probe (batches 3, 6, 7): 31 identical, VERIFY OK on every node. Logs under
`~/ntt-v/ecalc/results/v11/<job>/` on aac6, summaries in `summary_d5.txt`.

#### Open issues

- The leaf-level race under in-phase pool growth (above): open, owner P / N-kernel; `v11_d5.sh` reproduces it. Until
  it is closed the host-flow stand-ins stay (the `MN_DM=host` cross-check) and A-mem's no-growth rule is the guard.
- The recheck at size 1 needs `ECALC_CKPT_TOP=1` at run time (2 × the top-level P, Q written: 35 GB at 4 × 10¹⁰);
  without it P, Q come from the sidecar and only the digits, X and the recurrence are recomputed independently.
- The recheck reads the part files where they were written (node-local `/tmp` at size > 1 means the same nodes).
- `ECALC_RES_LOG=1` copies every reduced number to the host for the cross-check — a debug mode (seconds at 10¹⁰,
  ≈ 30 s of bs per run with the level probe).

## W — verification and housekeeping (Phase 12, PLAN.md §27 row W; DECISIONS2 §8)

(former file: W.md)

Branch `w12` (from `main` @ 7aded87; the aac6 clone `~/ntt-w`). Files: `ecalc/ecalc.c` (the output tail), `ecalc/mn_out.c/.h`,
`ecalc/mnaccept.sh` (the `recheck` step and the `--full` step's recheck — one block each, R's `--stress` block is separate),
`ecalc/README.md`, `.gitignore` (unchanged: verified). Touched outside my list, minimal and commented `Phase 12 W`:
`ecalc/binsplit.c` / `.h` — two globals (`bs_ckpt_own_buf`: the checkpoint DMA buffers malloc'd instead of the pinned
staging; `bs_ckpt_tree_pdone`: the count of a tree set's files whose P part is written, one atomic increment in
`tree_io`) — 6 lines, no change to the file format or to any existing path.

#### 1. `ECALC_CKPT_TOP` on by default above 10¹⁰ digits

**What it was.** Phase 11 V wrote the size-1 top-level P, Q as a level-0 tree set only with `ECALC_CKPT_TOP=1` and a
`BS_CKPT_DIR`, synchronously between bs and the reciprocal (2 × 17.8 GB at 4 × 10¹⁰), through the pinned staging.

**What it is now (ecalc.c, the output tail).**
- Default: on when the run has an outfile, decimal limbs and d_out > 10¹⁰; `ECALC_CKPT_TOP=0` off, `=1` on at any size.
- Directory: `BS_CKPT_DIR` when set, else `<outfile>.top/` (created; the recheck looks there when `BS_CKPT_DIR` is unset).
  Delete it after the recheck.
- Size 1, the device flow (the top leaf level on the device tier, every run ≥ 10¹⁰): a background thread runs
  `bs_ckpt_tree_write(0, …)` on `bs_Pd`, `bs_Qd` — started right after bs, before the reciprocal. The four region
  files each hold P's quarter then Q's (binsplit.c `tree_io`), so P is on disk when `bs_ckpt_tree_pdone` reaches 4:
  the driver waits for that (`ckpt_top_wait_p`) before `S = P + Q` overwrites P in place — i.e. P's 17.8 GB are
  written under the reciprocal (16 s at 4 × 10¹⁰) — and joins the thread (`ckpt_top_join`) before Q's block is
  released after the division (Q's 17.8 GB under the division's 17 s). Both waits are printed on the
  `checkpoint: the top-level P, Q -> …` line and reported as `RESULT ecalc ckpt_top_wait`. The thread's DMA
  buffers are its own (`bs_ckpt_own_buf`): the staging is released for the dm phase while it runs.
- Size 1, the host flow (a leaf that ended on the batch tier: ≤ 10⁹): synchronous through device copies, as before
  (0.8 GB at 10⁹).
- Size > 1: `mn_tree` writes the top tree set as before when `BS_CKPT_DIR` is set; with the default on and no
  `BS_CKPT_DIR`, the directory is `<outfile>.top` and `BS_CKPT_TREE_EVERY=64` (the top set only, no leaf sets: the
  directory is set after the leaf tree).

**Measured at 4 × 10¹⁰ (job 20881, node s24-26, the reference evicted; `results/mnaccept/20881/full_4e10.log` and
`results/w12/20881/base_4e10.log` on aac6).** The same binary, one run each:

| | with the top-level set (the default) | `ECALC_CKPT_TOP=0` |
|---|---|---|
| init | 15.0 s | 18.2 s |
| bs | 35.29 s | 34.64 s |
| recip | 14.16 s | 13.10 s |
| dm (incl. recip) | 31.51 s | 28.09 s |
| **total (to VERIFY OK)** | **81.87 s** | **80.99 s** |
| the digit file's write (the writer thread, overlapped) | 95.92 s | 78.33 s |
| digits | identical to `results/e_4e10.out` | identical |

The set: 35.56 GB in 31.57 s in the background (1.13 GB/s to the node's `/tmp`); the driver waited **0.00 s for P** before
S = P + Q (P's 17.8 GB were on disk before the 14 s reciprocal ended) and **2.15 s for Q** after the division. So on the
wall to VERIFY the cost is +0.9 s (the run-to-run spread is ± 1.4 s; by phases dm is +3.4 s: the 2.15 s wait plus ≈ 1.2 s
of a slower reciprocal and division under the concurrent DMA + writes). Two more runs: job 20891 (same node) waited
3.23 s for Q, 85.25 s total with a slow init (17.8 s); job 20912 on node s24-16, whose disk took the set at 1.59 GB/s,
waited 0.00 s for both — 82.42 s. So the wait is the disk's rate against the division's 15–17 s: hidden entirely at
≥ 1.5 GB/s, 2–3 s exposed at 1.1 GB/s. What is not hidden is the disk: the digit file's
40 GB write, which overlaps the division and runs on past VERIFY, finishes ≈ 18 s later because the same disk absorbed
35.6 GB more (171 s elapsed with the write, against ≈ 155 s). Verdict: the top-level set costs ≈ 1 s of the run's wall
and ≈ 30 s of disk time on a 1.1 GB/s local disk — default on above 10¹⁰ as DECISIONS2 §8 recommends; `ECALC_CKPT_TOP=0`
turns it off (or `BS_CKPT_DIR` points it at another disk). The 2.15 s Q wait could be hidden too by deferring Q's
release past the output stage; not done — the process's end is the digit write in any case.

#### 2. The recheck as a regression step (`mnaccept.sh recheck`)

In the default run (not only `--full`), after `ckpt`: 10⁹ at size 1 (`POOL_LOG=29 ECALC_CKPT_TOP=1`, the level-0 set into
`<outfile>.top`) and 10⁸ at size 2 (`POOL_LOG=27 ECALC_CKPT_TOP=1`, the top tree set into `<outfile>.top` — the size > 1
default path), each: the run (VERIFY OK, digits identical to `ref/`), `ECALC_RECHECK=1` on its files (RECHECK OK on
every node, P and Q *from the checkpoint* — the step fails if they came from the sidecar), then one digit flipped at
byte 1000 of the (last) part file and the recheck again (RECHECK FAILED wanted, no RECHECK OK). Runtime 46 s for the step (job 20881): 10⁹ size 1 —
run 9.0 s (the level-0 set 0.89 GB in 1.93 s, the host flow, synchronous), recheck 1.8 s for the file, RECHECK OK with
P, Q from the checkpoint, the corrupted copy RECHECK FAILED (`digits == X BAD` at all eight moduli, the T1 identity BAD);
10⁸ size 2 — run 8.3 s (the top tree set 2 × 0.044 GB into `<outfile>.top` by the size > 1 default path), 3 RECHECK OK
(both nodes + `mn: all 2 nodes`), P, Q from the tree level 1 set on both, the corrupted copy 3 RECHECK FAILED.
`--full` gained a second line: the 4 × 10¹⁰ run's files rechecked (`full 4e10 recheck`, its time printed).

#### 3. A 4 × 10¹⁰ run rechecked from its files

Job 20881, `./mnaccept.sh 20881 --full --only full` (2668ef4): the run above (81.87 s to VERIFY OK, 171 s with the
write, identical), then `ECALC_RECHECK=1 ./ecalc 40000000000 /tmp/mnaccept_20881/e4e10.txt` — no `BS_CKPT_DIR`, the
recheck found `e4e10.txt.top` itself:

    recheck: P (2222222227 limbs), Q (2222222227 limbs) from the tree level 0 set in /tmp/mnaccept_20881/e4e10.txt.top
    recheck: 40000000001 digits read from /tmp/mnaccept_20881/e4e10.txt in 84.2 s; windows ok (4 checked); digits -> X mod q == the run's X residues; P, Q from the checkpoint: == the recurrence (2.2 s), == the run's; T1 identity with the run's R residues ok
    RECHECK OK

**98 s** end to end (the 40 GB file 84.2 s in the first pass, the 35.6 GB set read and reduced by the kernels, the
recurrence 2.2 s) against the run's 81.9 s — the recheck of a run is as long as the run, and it is all disk: 76 GB
read from `/tmp`. In that build the file was read twice (the residues, then the windows); commit 36afd71 folds the
windows into the residue pass and takes the counts and tails from `stat` and the file's end, so the recheck reads the
file once: 72.9 s for the file, 82 s end to end (job 20891). What remained was the serial non-digit scan and the
serial read; fd82fb8 runs the scan over the OpenMP team and reads the next 256 MB chunk in a thread while the current
one is reduced: **36.3 s for the file, 46 s end to end** (job 20912) — 0.56 of the run's wall, and the floor is now the
disk (40 GB + 35.6 GB read; the file was in the page cache in these runs, the set mostly not).

#### 4. The host-flow stand-ins (`MN_DM=host`, `MN_COMBINE=host`) — **kept**

Checked at the end of the session (`ls ~/apucode/ntt/.claude/worktrees/*/results/CRASHES_HANGS.md §R`): one `R.md`
(agent-a50c987e…, last written 00:30). It names the transition (a stale operand: the odd-node copy at a level boundary
issued by `mem_dev_copy_on` without a wait on the caller's stream, read by the next level's Q₁₇ operand before it had
landed — the failing leaves are wrong from limb 2¹⁷ + a few pages, as V measured), has the one-line fix in `mem.c` and the
no-growth invariant (`RNS_POOL_GROW`), and reports the unfixed witness runs (1 wrong leaf in 13, twice); its **"Gate
runs" section still reads "(filled in below)"** — no 40/40 count at the time I closed. The rule was "delete only if R.md
says the cause is fixed and 40/40 passed": the second half is not there yet, so the stand-ins stay — `ecalc.c`'s `MN_COMBINE=host` combine and `MN_DM=host` gather + host division, `mn_out.c`'s
`mn_out_scatter_standin` (the host X scattered into shares), `mn.c`'s `mn_gather_host` (M3's end: the shares assembled on
node 0's host) and the host-`X` path of `newton_db_divmod` that only `MN_DM=host` reaches at size > 1 — untouched, listed in the README's switch table as
*stand-in … kept while DECISIONS2 #1 is open*. Deleting them is a 20-minute edit for the integrator once R's numbers are
in: the three `MN_COMBINE`/`MN_DM` tests in `ecalc.c` (lines `host_combine`, `mn_dist`, `mn_dm`), the `else` branches
they guard (the M2 combine loop, the `mn_gather_host` branch), `mn_out_scatter_standin` and its declaration,
`mn_gather_host` in `mn.c`; then the regression (no step uses them).

#### 5. README and the switch list

`ecalc/README.md`: the header at the Phase 11 numbers; the two old switch paragraphs and the multi-node variable table
replaced by pointers; the recheck paragraph and the regression paragraph in the Phase 12 form; a **Switches** section at
the end — every `getenv` in `ecalc/*.c *.h` (108 variables), grouped `ECALC_` / limbs and pools / transform and tiers
(`NTT_`, `RNS_`, `DBIG_`) / `BS_` / `NEWTON_` / `COMM_`, `MN_` / `DIST_`, one line each with the default, debug and
test switches marked; the five test-only variables named. Verified by grep (both directions):

    grep -ho 'getenv("[A-Z0-9_]*")' *.c *.h tests/*.c | sed 's/getenv("//;s/")//' | sort -u > live
    grep -o '`[A-Z][A-Z0-9_]*' README.md | tr -d '`' | sort -u > listed
    comm -23 live listed      # live, not listed: (empty)
    comm -13 live listed      # listed, not live: the prefixes, the three names listed as deleted, ECALC_REF / ECALC_REF_4E10 (mnaccept.sh's), words

`results/*.md` tracked: `.gitignore` has `results/*` + `!results/*.md`; `git check-ignore -v results/TESTING_VERIF.md §W` reports the
negation rule (not ignored), `git ls-files results` lists the 21 write-ups.

#### Gate runs

- **Job 20881** (2668ef4, batch 1: `./mnaccept.sh 20881 --only recheck,full`, the `ECALC_CKPT_TOP=0` baseline, then
  `--only unit,e9,mn,ckpt`): recheck 2/2 (46 s), full 2/2 (the 4 × 10¹⁰ run 81.87 s identical + its recheck 98 s), the
  baseline 80.99 s identical, unit 8/8, e9 2/2, mn 5/5, ckpt 1/1 — 20 of 20 steps passed across the three invocations.
- **Job 20891** (36afd71, the one-pass recheck: `./mnaccept.sh 20891 --full`, one invocation): **20 passed, 0 failed in
  2018 s** — unit 8, e9 2 (10⁹ 14.70 s / 24.97 s identical), mn 5 (10⁸ at 2/3/4, 10⁹ at 2/4, all nodes VERIFY OK,
  identical), ckpt 1, recheck 2 (10⁹ size 1: file read 1.5 s; 10⁸ size 2: 0.2 s; both RECHECK OK from the checkpoint,
  both corrupted copies RECHECK FAILED), full 2: **4 × 10¹⁰ 85.25 s** (init 17.8 — a slow init this time — bs 34.8,
  recip 14.1, dm 32.5; the set 35.56 GB in 32.61 s, waited 0.00 s for P, 3.23 s for Q; 160 s with the write; identical)
  and **its recheck 82 s** (the file 72.9 s in one pass, the set 35.6 GB, RECHECK OK, P, Q from the checkpoint).
- **Job 20912** (fd82fb8, node s24-16, the reader thread + parallel scan: `--only recheck` then `--full --only full`):
  recheck 2/2 (44 s; 10⁹ file read 0.7 s), full 2/2 — **4 × 10¹⁰ 82.42 s** identical (init 17.3, bs 35.5, recip 14.3,
  dm 29.6; the set 35.56 GB in 22.43 s at 1.59 GB/s on this node's disk, **waited 0.00 s for P and 0.00 s for Q** — the
  write fully hidden; 162 s with the digit write) and **its recheck 46 s** (the 40 GB file **36.3 s**, RECHECK OK, P, Q
  from the checkpoint == the recurrence == the run's, T1 ok).

Commands: `sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J W --wrap "sleep 2700"`, then from `~/ntt-w/ecalc`
`./mnaccept.sh <job> [--full] [--only recheck,full]`; the logs on aac6 under `~/ntt-w/ecalc/results/mnaccept/<job>/`
(`full_4e10.log`, `full_4e10_recheck.log`, `recheck_*.log`, `*_recheck_bad.log`) and `results/w12/<job>/summary.txt`.

Size 1 digits untouched: 10⁹ identical in both bases and 4 × 10¹⁰ identical to `results/e_4e10.out` in every run above
(the checkpoint write reads P, Q; the recheck computes nothing the run uses).

#### Open issues

- The stand-ins (section 4): delete after R's verdict.
- The 2–3 s Q wait after the division at 4 × 10¹⁰ (the disk at 1.1 GB/s): hide it by releasing Q's block after the
  output stage instead of before (memory allows it at 4 × 10¹⁰, not checked at 10¹¹); or point `BS_CKPT_DIR` at a faster
  disk. The digit file's own write, which runs on after VERIFY, is the larger disk consumer (40 GB) and now shares the disk
  with 35.6 GB more: 160–171 s to the last byte against ≈ 155 s without the set.
- The recheck of a 4 × 10¹⁰ run: 46 s (0.56 of the run) with the file in the page cache; from a cold disk the floor is
  the disk (40 GB + 35.6 GB at 1.1–1.6 GB/s ≈ 50–70 s). The set's read (`bs_ckpt_tree_read`, four threads through 1 GiB
  chunks) is the other half; not measured separately.
- At size > 1 the default top tree set is written synchronously by `mn_tree` (each node's share: 35 GB / size per node
  at 4 × 10¹⁰), as the Phase 10 tree checkpoints always were; a background write there would be the same pattern as the
  size-1 one (the tree's P, Q shares are read-only until the division re-shards them) — not done, not measured beyond
  10⁸ at size 2.
- `<outfile>.top` is left on disk by design (the recheck needs it); nothing deletes it. On the target (576 nodes, ≈ 4 × 10¹³
  digits) the top set is ≈ 2 × 20 TB on the parallel file system — `ECALC_CKPT_TOP=0` if that is not wanted.

## T — tests, checkpoints and docs (Phase 10, PLAN.md §21: D3, D2, D4, D5, C6, E3)

(former file: T.md)

Branch `t10` (from `main` @ 4aca721 = 38ed61b + the PLAN §20–22 commits; the aac6 clone `~/ntt-t`).
Files: `ecalc/mnaccept.sh` (new), `archive:drivers/ecalc/t10_test.sh`, `t10_d5.sh`, `t10_d5b.sh` (new: this report's runs), `ecalc/accept.sh`,
`ecalc/variance.sh`, `ecalc/mn.c` (the tree checkpoint loop and `mn_finalize`), `ecalc/binsplit.c`
(`bs_ckpt_tree_remove_below` and a comment), `ecalc/README.md`. Nothing else touched; the numerics are
unchanged (the C6 change moves a barrier and a set removal).

#### D3 — the standing regression: `ecalc/mnaccept.sh <jobid> [--full] [--only unit,e9,mn,ckpt,full]`

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

#### C6 — the multi-node checkpoints: `BS_CKPT_TREE_EVERY`, the barrier off the write

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

#### D2 — restart at 10¹⁰ from tree-level sets at size 4

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

#### D4 — the binary path at 10¹⁰

Job 20765, `LIMB_BASE=2 ./ecalc 10000000000 <out>` at size 1 (the paper's binary limbs, the radix conversion
on the device): VERIFY OK, **identical to `results/e_1e10.out`** (the decimal path's file) — total 56.2 s (bs 6.1,
recip 4.8, 10dP 2.4, dm 6.8, T1 2.3, dc 23.7, T2 0.3; init 13.0), VmHWM 124.9 GB. 10⁹ in both bases is in the
regression (e9 step).

#### D5 — the stale-plane-pointer suspicion (A-mem open issue 1)

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
tier that grows the pool. `results/MEMORY.md §A-mem`'s rule stands: pool 1 at the full pool for `POOL_LOG` ≤ 30, so nothing
grows inside a phase.

#### E3 — `ecalc/README.md`

The header paragraph carries the Phase 9 defaults and numbers (86.4 s / 48.8 GB at 4 × 10¹⁰; 7 × 10¹⁰);
the file table gained `mn.c`/`mdb.h`, `mn_out.c`, `mem_report`, the scripts; new sections: the
multi-node run (`mnrun.sh`, the `COMM_*`/`MN_*`/`DIST_*`/`MEM_REPORT_DEVS`/`POOL_LOG` knobs, the part
files, the per-node checkpoints with the C6 rules) and the standing regression. `accept.sh` and
`variance.sh` run from their own clone (`cd "$(dirname "$0")"` instead of `~/ntt/ecalc`); `accept.sh`
adds `t_dbig 0` and `t_out` to its unit list.

#### Open issues

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

## WP6 — inter-node correctness on aac6 (2026-09-17/18)

(former file: WP6.md)

Branch `wp6-internode` (from `wp1-decimal-base` at 8a43393). Code under
test, unchanged from that base: `ecalc/comm_tcp.c` (TCP full mesh, one
process per rank), `ecalc/ntt_dist.c` (the distributed four-step
transform), `ecalc/tests/t_dist.c` (one process per rank over TCP when
`COMM_RANK` is set, rank r on APU r mod 4), `ecalc/wp6run.sh` (one process
per APU across the nodes of a Slurm allocation, `COMM_HOSTS` from
`scontrol show hostnames`, port base 27000). One addition on this branch:
`ecalc/tests/t_comm.c` also runs as one process per rank when `COMM_RANK`
is set, so the communicator alone can be run across nodes with
`wp6run.sh <nodes> ./tests/t_comm` (all-to-all of 1 B … 3 MiB slabs
checked word-for-word against the senders, barrier, max and mod-q
reductions). Correctness only: aac6 has one 1 GbE NIC per node.

Build on the login node (`module load rocm; make -s tests/t_dist
tests/t_comm`) in `~/ntt-wp6` (a clone of `~/ntt` with this branch fetched
from a bundle). All runs were launched by a nohup watcher on the login node
the moment a job named `wp6` was RUNNING, so that a short window could not
be missed; logs `~/wp6run-<jobid>.log`, `~/wp6extra-20656.log`, snapshots
in `~/wp6status.log`.

#### Cluster state

Partition `PPAC_MI300A_SPX`, 4 usable-in-principle nodes: s24-30 and s24-35
**down** for the whole session (so 3 nodes / 12 ranks was impossible);
s24-16 held by the main session's `wp1d` jobs (20644 until 02:59 aac6
time, then 20655 for 4 h); s24-26 held by `admin`'s `cdash-nightly`
(20645_2, 3 h) until ≈ 00:55, then "planned" for the pending 2-node `wp6`
request 20649. Request 20649 (`-N2 --gpus-per-node=4 -t 1:00:00 -J wp6`,
submitted by the main session) started at 02:59:31 when 20644 ended and
was cancelled 22 s later (from the main session, to let 20655 in), which
was enough for two of the three planned runs. A second 2-node request
(20657, 2 h) was queued at 03:00; at 04:33 another user's job (20662,
`rocprofsys-ctest`, 8 h limit) was backfilled onto the idle s24-26, and
when 20655 ended early at 05:58 s24-26 was still held, so no second
2-node window appeared before the session's ~6 h budget ran out; 20657
was cancelled at 06:00 (it was holding s24-16 as "planned"). Snapshots
in `archive:results_raw_2026-10-19/wp6/status-snapshots.txt` and `~/wp6status.log` on aac6.
The only piece not demonstrated across nodes is `t_dist 24` with 8
ranks on two nodes; its two ingredients were: `t_dist 24` with 8 ranks
on one node over TCP, and 3 MiB-per-peer slab all-to-alls across the
two nodes (t_comm), so a repeat on the next free 2-node window
(`salloc -N2 --gpus-per-node=4 -J wp6 --no-shell`, then
`SLURM_JOB_ID=<id> ./wp6run.sh 2 bash -lc "module load rocm; cd
~/ntt-wp6/ecalc; ./tests/t_dist 24"`) is expected to pass; `~/ntt-wp6`
on aac6 is built and ready (`~/wp6watch.sh` automates the run).

#### Runs

| job | nodes | ranks | test | result |
|---|---|---|---|---|
| 20649 | s24-16 + s24-26 | 8 (4 per node, one per APU) | `wp6run.sh 2 ./tests/t_comm` | **VERIFY OK on all 8 ranks** (5 slab sizes, barrier, reductions) — 7 s |
| 20649 | s24-16 + s24-26 | 8 | `wp6run.sh 2 … ./tests/t_dist 20` | **VERIFY OK on all 8 ranks** (4 checks each: 2¹⁰ × 2¹⁰, all four primes) — 3 s |
| 20649 | s24-16 + s24-26 | 8 | `t_dist 24` | not run: job cancelled before it started (`srun: error: Slurm job 20649 has expired`) |
| 20656 | s24-26 | 4 (TCP, one process per APU) | `wp6run.sh 1 ./tests/t_comm` | VERIFY OK on all 4 ranks |
| 20656 | s24-26 | 4 | `t_dist 20`, `t_dist 24`, `t_dist 26` | VERIFY OK on all 4 ranks (4, 52, 65 checks; up to 2²⁶ points) |
| 20656 | s24-26 | 4 | `t_dist 20` again immediately after, same ports | VERIFY OK (port reuse after a clean exit is fine) |
| 20656 | s24-26 | 8 (two processes per APU, `srun --ntasks-per-node=8`, `COMM_PORT=27100`) | `t_dist 24` | **VERIFY OK on all 8 ranks** (52 checks each) |
| login node | aac6-login | 4 and 8 forked | `t_comm` (host-only build) | VERIFY OK |
| 20657 | (2 nodes, requested 03:00) | 8 | `t_dist 24` across nodes | **did not run**: no second window in the ~6 h session (see below); request cancelled at 06:00 |

So: the communicator and the distributed transform are correct **across
two nodes with 8 ranks** (t_comm and t_dist 20), the transform is correct
with **size 8 up to 2²⁴ points** (8 ranks on one node — the ntt_dist code
does not know where a rank lives, only the socket path differs) and with
size 4 up to 2²⁶ points over TCP. Every rank verifies the full
convolution result of its own block-cyclic rows against the one-rank
engine (`ntt_fwd/ntt_pw/ntt_inv`) with the same random input.

#### Bugs found

None. No source change was needed in `comm_tcp.c`, `t_dist.c` or
`wp6run.sh`; `t_comm.c` gained the per-rank mode (commit fa3e603).

#### Observations on the TCP communicator across nodes

- **Connection ordering.** Rank r listens on `base + r` (INADDR_ANY), then
  connects upward to every rank > r (retrying for up to 60 s while the
  peer's process starts), then accepts from every rank < r and reads the
  8-byte hello that identifies the connecting rank. Because every rank
  listens before it connects and `connect` only needs the peer to be
  listening (backlog 64), the order in which srun starts the 8 processes
  on the two nodes does not matter; the start-up skew across nodes was
  well under a second (all META lines carry the same second). With 4 ranks
  per node the ports are `27000+r`, distinct per rank, so two nodes never
  clash; two processes per APU (8 ranks on one node) work the same way.
- **Port reuse.** The listening socket has `SO_REUSEADDR` and is closed
  after the accepts; consecutive runs 0–5 s apart on the same ports
  (t_comm → t_dist 20 → t_dist 24 → 26 → 20) all connected at once
  — TIME_WAIT sockets from the previous run did not block the bind.
- **Host names.** `scontrol show hostnames` gives `aac6-ppac-NN`, which
  resolves (via `/etc/hosts`) to the node's <ip> address on the
  1 GbE interface `enp129s0` — no IP list is needed in `COMM_HOSTS`.
- **No deadlock in the all-to-all.** One sender thread per peer writes
  the slab while the caller's thread reads from every peer in rank order;
  with 8 ranks and 3 MiB slabs (t_comm round 5, larger than the socket
  buffers) and 2²⁴-point planes in t_dist (slabs of 2 MiB per peer) the
  blocking sockets never stalled, as the design argues (every send has an
  independent reader).
- **Speed (for the record only).** `t_dist 20` on 8 ranks over two nodes:
  3 s wall including process start; the 1 GbE link is irrelevant at these
  sizes.
- **Slurm.** `srun --ntasks-per-node=4 --gpus-per-node=4` exposes all four
  APUs to every task, so `hipSetDevice(rank % 4)` is what places the
  ranks (the 8-rank step used `--overlap` and `COMM_PORT=27100`).

## CX: correctness sweep of the 2026-10-11/12 branches on aac6's CPX nodes

(former file: CX.md)

Armed 2026-10-12 (Eastern, early morning). Driver `tools/cx_aac6.sh` (copied to `~/cx/driver.sh` on aac6). Collected; see Results.

#### What runs
- One git bundle of the seven branches (`o2 o5 k6 k1b k8b o3 g6`, tips of GitHub after tonight's rewrite) -> clone + `make -j12` per
  branch in `~/cx/<branch>` (module rocm), three builds at a time.
- Three slots, each with one 45-min hold on a CPX node (`SH5_MI300A_CPX,PPAC_MI300A_CPX`; the 4-APU SPX nodes are not used),
  taking branches from a queue (o5 k6 k1b o2 k8b o3 g6); a hold is reused while >= 31 min remain, else cancelled and a new one taken.
  Only its own holds are cancelled (also on exit); `~/cx/STOP` stops after the current branch.
- CPX environment on every run: `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30` (S44, T11, o4); CPX tops out near 4e9 digits.
- Per branch, arm `off` then arm `on` (o3, g6: one arm `std`, no switch):
  1. `mnaccept.sh --only e9 --skip-grid`: 1e9 decimal and LIMB_BASE=2, digits cmp'd against the reference.
  2. `ecalc 4000000000` (VERIFY OK); the digit files of `off` and `on` kept on the node and compared with `cmp` (all sibling part files).
  3. `mnaccept.sh --only unit --skip-grid` (t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_patch; `t_mn_grid`
     skipped: it needs two node-processes).
- Switches (arm `on`): o2 `DBIG_SMALL_BOUNCE=1`; o5 `RNS_STRATEGY_FORM=B5`; k6 `RNS_DIST_OVERLAP=1` and k1b `DIST_GATHER_RAW=1`
  (both arms with `RNS_STRATEGY=C` so the changed path runs at these sizes); k8b `BS_SEED_PULL=1`.
- Extras: o5 (arm on) `LIMB_BASE=10 ECALC_NP=3 tests/t_strategy {24,26} 1 V` and ecalc at 1e9 and 4e9 with `RNS_STRATEGY_CHECK=1
  RNS_VERBOSE=1` (the number of check lines is reported: 0 means the B form was not chosen, so B5 was not exercised);
  g6 `mnaccept.sh --only sched` with `MNACCEPT_SCHED_PROCS=4` (4 node-processes on one CPX node: may fail for lack of devices, which
  would be an environment limit, not a G6 defect).
- Known CPX baseline: `t_ntt 24` fails only its speed check (S44: a CPX XCD has 1/6 of the APU bandwidth); compare against the `off` arm.

#### Outputs
`~/cx/summary.txt` (branch | arm | test | PASS/FAIL | s | note), marker `~/cx/DONE`, `~/cx/driver.txt` (holds, slot timeline),
logs `~/cx/<branch>/out/`, build logs `~/cx/<branch>.build.log`.

#### Results
Collected 2026-10-12 from `~/cx/summary.txt` and `driver.txt` (all measured). Driver ran 02:37-03:40 ET (three slots, one 45-min hold each, reused or renewed; no leftover steps). 7 of 7 builds PASS. "off/on" = the switch off/on; o3 and g6 have no switch (std). Times are the ecalc total of the run.

| branch | switch (on) | mnaccept e9 (1e9, LIMB_BASE 10 and 2) off / on | ecalc 4e9 off / on | 4e9 digit files cmp off vs on | unit sweep off / on |
|---|---|---|---|---|---|
| o5 | RNS_STRATEGY_FORM=B5 | PASS (2/2) / PASS (2/2) | PASS 20.43 s / PASS 19.73 s | PASS (identical) | t_ntt 24 FAIL (CPX speed check), 7 others PASS / same |
| k6 | RNS_DIST_OVERLAP=1 (RNS_STRATEGY=C) | PASS / PASS | PASS 21.15 / PASS 22.43 s | PASS | same as o5 |
| k1b | DIST_GATHER_RAW=1 (RNS_STRATEGY=C) | PASS / PASS | PASS 21.01 / PASS 20.63 s | PASS | same as o5 |
| o2 | DBIG_SMALL_BOUNCE=1 | PASS / PASS | PASS 18.98 / PASS 18.45 s | PASS | same as o5 |
| k8b | BS_SEED_PULL=1 | PASS / **FAIL (2/2)** | PASS 19.56 s / **FAIL** | FAIL (on run produced no digits) | off: same as o5; on: t_ntt 24 FAIL, t_patch rc 143 (see below), others PASS |
| o3 | none (std) | PASS (2/2) | PASS 19.11 s | n/a | same as o5 |
| g6 | none (std) | PASS (2/2) | PASS 19.00 s | n/a | same as o5; `--only sched` 3 of 3 FAIL (environment, below) |

o5 extras (arm on): `t_strategy 24 1 V` and `t_strategy 26 1 V` (LIMB_BASE=10 ECALC_NP=3) PASS; ecalc with RNS_STRATEGY_CHECK=1 at 1e9 (10.93 s) and 4e9 (30.02 s) VERIFY OK, 4 check lines each, so the B form was chosen and exercised at both sizes (no digit change).

Failures, classified:
- **t_ntt 24 (every branch, both arms, 12 of 12 unit runs): CPX limit, not a code defect.** "VERIFY FAILED (4 of 2360 checks)"; the same failure is on the `off` arms, and S44 shows it is the speed check (a CPX XCD has 1/6 of the APU bandwidth). All other 7 unit tests (t_mul 20 185 checks, t_bs, t_dbig 0 555, t_newton 20 620, t_verify 334, t_out, t_patch) PASS on every branch and arm except k8b/on t_patch.
- **k8b with BS_SEED_PULL=1: FAIL, a possible defect (or a CPX-specific hang), needs a look.** Both mnaccept e9 runs and ecalc 4e9 exit rc 3 after init with `ecalc: FATAL ... after init, rc 3: db_pull_wait: APU0's pull kernel did not finish copy 1 in 600 s` (the pull kernel's own 600 s watchdog), so each failing run cost 600 s (the e9 step took 1211 s, 4e9 607 s). The off arm of the same build is clean, so the switch is the cause. Not known whether it is CPX-only: the pull kernel waits on the XCD devices of one APU; run it on a 4-APU node before judging. In the unit sweep with the switch on, t_patch was killed (rc 143, SIGTERM, after printing "VERIFY OK (1 checks)"; cause not determined).
- **g6 `--only sched` (MNACCEPT_SCHED_PROCS=4 on one CPX node): 3 of 3 FAIL, environment limit.** "sets written by 0 nodes", "0 refusal lines", restart step "Exited with exit code" from the launcher: 4 node-processes need 4 nodes; not a G6 defect (expected in the plan above).

Summary: digits identical on o5, k6, k1b, o2 (switch on vs off, 4e9 files cmp, 1e9 mnaccept) and o3, g6 (std); the only switch failure is k8b BS_SEED_PULL=1. Speed on CPX (4e9, off to on): o5 -0.70 s, k1b -0.38 s, o2 -0.53 s, k6 +1.28 s: single runs, not timing claims.


#### RESUME
COLLECTED 2026-10-12 (Results). Open: k8b BS_SEED_PULL=1 pull-kernel 600 s timeout on CPX (re-run on a 4-APU node; ask the k8b owner). Remove the `~/cx/<branch>` clones. Re-run one branch: `CX_BRANCHES=<b> CX_SLOTS=1 bash ~/cx/driver.sh`.

## WM15 — the weak-memory audit (Phase 15, task 0b)

(former file: WM15.md)

Agent **WM**, branch `p15-WM` from `int15i` @ ab8daa2. Times Eastern. Numbers labelled **measured / modelled / assumed**.

#### Hazard table

| # | where | writer → reader | class | fix |
|---|---|---|---|---|
| **H1** | `ntt_dist.c` chunk pipeline (`dist_fwd_pre`, `inv_pre_y`) over **comm_xgmi** — the single-node dist tier (`rns_dist.c dist_core`, three transforms per prime: fwd A, fwd B, inv) | the sender's **push kernel on its transfer stream** stores into the **receiver's `rbuf`** (XGMI peer write) ← the receiver's **unpack kernels of the previous transform** (its compute stream) still reading the same chunk region | **hazard (WAR)** — nothing on the GPU orders the sender's `ts` after the receiver's `s`; the exchange's `pthread_barrier` orders only the host threads, which enqueue asynchronously. Needs the receiver's GPU to lag the sender's by ≥ a column pass + a row pass (improbable, not excluded: nothing forbids it) | **committed** (0a10a99): event `evr` recorded on `s` at each transform's start; the host waits for it before the transform's first post; the exchange's barrier then holds every sender until every receiver has passed it. No GPU idle (the wait is for work queued *before* the row pass already enqueued) |
| — | everything else (≈ 60 sites, below) | | **safe** (reasons below) or **unclear** (U1–U4: runtime assumptions, none a code hazard) | none |

No hazard found in the files being edited now (EW: `mn_out.c`, `ecalc.c`, `newton_db.c`'s hook; DL: `binsplit.c`; PC: `rns_dist.c`'s
cache): **no patch for the integrator to apply**. Two robustness notes for them (not hazards today) are in §3.

#### 1. Method and the rule applied

Every HIP call, kernel launch, stream / event, host thread hand-off (`pthread_create/join`, barriers, semaphores, mutex/cond,
atomics, OpenMP regions) in `ecalc/*.c` (≈ 19,700 lines) was read in context. For each place where the CPU or another device reads
memory a kernel / copy / peer store wrote, and each place where a kernel or DMA reads memory the CPU wrote, the ordering chain was
written down. The rule (apumult A27 §3.3 / §4.3, and the HIP contract):

- **device → CPU**: a *completed synchronisation of the stream (or device) that ran the writer*, **done by the host thread that
  reads, or by a thread that then hands over with release/acquire** (join, barrier, mutex, semaphore, seq_cst atomic). A sync of a
  different stream / device orders nothing.
- **device A → device B** (peer store, or B reading A's memory): the same completed sync on A's stream observed by the host thread
  that *then launches* B's reader, or a GPU-side event wait (`hipStreamWaitEvent` on A's event).
- **CPU → device**: the CPU stores happen-before the launch/enqueue of the reader (same thread, or a hand-off as above), and the CPU
  does not rewrite a buffer before the copy/kernel reading it is synchronised (WAR).
- **WAR across devices**: a peer write must not overtake the target device's pending reads of the same memory.

**Assumed** (the HIP contract, not measured here): `hipStreamSynchronize` of the writing stream is equivalent, for that stream's work,
to A27's `hipDeviceSynchronize` on the writing die (both end in a system-scope release); a kernel dispatched after the host observed
that sync sees the data (system-scope acquire at dispatch). See U1–U2.

The codebase's invariant (true at every site read): **every operation synchronises all the streams it used, in the thread that
drove each device, before it returns** (dbig ops: `hipStreamSynchronize(0)` per quarter thread; rns tiers: `hipStreamSynchronize(v->s)`
per device thread, then an `omp barrier` / region end; mdb ops: per-APU stream sync then the region's join). Most of the "safe"
classifications below rest on it.

#### 2. The inventory

##### 2.1 Device → CPU reads

| site | writer (device, stream) | reader | ordering | class |
|---|---|---|---|---|
| `dbig.c addsub_core2` carry flags `g_flags[d]` (pinned host) | `k_addsub` on APU d, null stream | main thread's carry scan | quarter thread syncs null stream d, then the `omp parallel for` join | safe |
| `dbig.c maxidx` (`g_red[d]` → `hred`) | `k_maxidx`, null stream d | the same thread | `hipMemcpy` D2H on the same null stream (the Phase 14 A1 race was the shared host buffer, fixed) | safe |
| `dbig.c db_mod_qs` (`g_mq_out[d]`) | `k_modq`, null stream d | same thread | stream sync + `hipMemcpy` on d; `g_mq_mx` against the writer thread's concurrent call | safe |
| `dbig.c db_to_bi` (bounce → host) | the source's writers (any tier) | quarter loop, `g_bs[d]` non-blocking | D2H on `g_bs[d]` + sync before each `par_memcpy`; the source complete by the invariant (the comment "its kernels ran on the null stream" is not the reason: a source written by `rns_dist`'s `k_scatter_runs` on another APU is complete because that op synchronised before returning) | safe |
| `dbig.c db_limb`, `db_set_*` (`mem_dev_copy_on`) | any | caller | synchronous `hipMemcpy` + `hipStreamSynchronize(0)` on the quarter's APU; writer complete by the invariant | safe |
| `rns_mul.c rns_mul_batch` striped: `g_spill` (registered host), outputs `Q[i].c` merged on the CPU (VMM off) | `k_crt_batch` on each APU's `D[d].s`, outputs possibly on another APU (peer store) | main thread after the region | each APU thread syncs its own stream (the writing die), then the second `omp barrier` / the region's end — A27's rule exactly | safe |
| `rns_mul.c rns_mul_batch_local` (`v->spill`, `v->len`, host merge) | `k_crt_batch`/`k_norm` on `D[d].s` | the same APU thread | same stream sync; D2H of `len` on the same stream | safe |
| `rns_mul.c mdev_gpu_crt` (`D[0].hstage` = the product, `g_spill`) | `k_crt_batch` on all four APUs (stores into APU 0's registered staging) | thread 0's spill merge, then `par_copy` | each APU syncs its own stream, then `omp barrier` | safe |
| `rns_mul.c mdev_core` host CRT (`hstage` planes, `k_store` incl. `D[q].hstage` from APU d) | `k_store` on `D[d].s` | host CRT after the region | own-stream sync per APU thread + region end | safe |
| `rns_mul.c rns2_*` (engine 2, not default) | `k_store` | host `crt2_carry` | own-stream sync + barrier / region end | safe |
| `rns_dist.c b_core` (`spall` pinned host; `Cd` quarters on other APUs) | `k_crt_batch` on `RS[d].s` | host spill reshuffle, `db_add_spills` | own-stream sync per rank thread, region end | safe |
| `rns_dist.c dist_core` (`Cw.flat`: `RS[r].spill` → host) | `k_crt_batch` | main thread | region end, then synchronous `hipMemcpy` on r | safe |
| `rns_dist.c` P24 CRT error flag (`p24_crt_err`) | `k_crt24` on s | same thread | D2H on s + sync | safe |
| `newton_db.c mdb_to_host_all`, `mn_out.c mn_out_allgather_u64` | the collective's pushes | caller | transport completes (xgmi: sender's stream sync + barrier), then synchronous `hipMemcpy` | safe |
| `mn_out.c dev_fetch` (writer thread, X on the device while the low product runs; `g_fs[d]` non-blocking) | X's writers (the division's ops) | writer thread | X complete by the invariant before the hook (`newton_db.c`: the hook follows `mdb_shift`/`db_*` ops that synced); the hook → `pthread_create` (release/acquire); D2H on `g_fs[d]` + sync before the CPU reads `lbuf`. `mn_out_early_start` also does `sp_dev_sync_all()` (after `mn_out_boundaries`' fetch — harmless, see §3 note N1) | safe |
| `ecalc.c x_bg_run` → `db_mod_qs` from the background thread | X's writers | background thread | as above; its null-stream kernels serialise with the main thread's blocking streams (performance, not ordering) | safe |
| `spill.c` writer (`spf_write_dev`, own non-blocking streams) | X / P / Q writers | spill threads | `sp_dev_sync_all()` (device sync of every APU) before the threads start; each chunk D2H + stream sync before `pwrite` reads the pinned bounce | safe |
| `binsplit.c` checkpoint writer (`ckpt_dbig_io`; background top set `bs_ckpt_bg_*`) | P, Q (bs top level) | writer thread | P, Q complete by the invariant (and `sp_dev_sync_all` in the O_DIRECT form); DMA + sync per run; cancel contract `dma_active`/`cancel` seq_cst: the owner overwrites P only after `dma_active == 0` | safe |
| `todec.c` leaf (legacy dc) | `k_leaf` into registered host | main thread | `hipDeviceSynchronize` per APU thread + region end | safe |
| `comm_tcp.c` (host staging) | pack kernels / D2H on s | sender threads | `DEV_TO_HOST` = async + sync of s before `exchange_start` creates the threads | safe |
| `comm_shmem.c` staging (pool, registered host / fine-grained) | pack kernels on the caller's stream s | NIC puts | `SYNC(s)` before `publish` / `start_push` | safe |
| `comm_xgmi.c h4_collect`, `rns_mul.c` snap / copy probe (instruments) | kernels / copies | same thread | stream sync / `hipMemcpy` on the stream's device (the copy probe polls an event on purpose, report only) | safe |
| `mem.c` reports, `memsample.c`, `db_pool_live` | host counters only | sampler thread | relaxed reads of sizes (a report) | safe |

##### 2.2 Device → device (peer stores and cross-device reads)

| site | writer | reader | ordering | class |
|---|---|---|---|---|
| **`ntt_dist.c` pipeline over `comm_xgmi`** (`x_alltoall` pushes into peers' `rbuf`) | sender's `k_push3` on its `ts` | receiver's `k_unpack*` on its `s` (**next** exchange's write vs **this** one's read) | RAW safe (`x_wait`: sender's stream sync + barrier, then the receiver enqueues `hipStreamWaitEvent` + unpack). **WAR unordered** | **H1, fixed** |
| `rns_dist.c dist3_fwd/inv` (DIST_R3) and all other `comm_xgmi` / `G->all` users | pushes | unpacks | the caller syncs its stream before `comm_alltoall(v)` (every rank, before the barrier) | safe |
| `comm_layered.c` intra stage (non-minor: pushes into the peers' own scratch slot `e->tmp` / `x1`) | peer pushes | owner's transpose / reorder | the owner's previous reads of that slot are synchronised (`complete_oldest` / `v_stages`) before it can reach the next intra barrier | safe |
| `comm_layered.c` minor form (pushes into `rb`) | peer pushes | caller's unpacks | `block_transpose` + `hipStreamSynchronize(s)` before the intra exchange, and `s` = the plan's `ts`, which waited on `ev` (after the caller's earlier unpacks) | safe |
| `comm_shmem.c` (NIC puts into `rb` or staging) | remote PE | caller's kernels | receiver publishes its offset only after `SYNC(s)` (the file's own comment names the WAR); arrival by signal + quiet, then the copy / launch | safe |
| `comm_tcp.c` | receiver thread's H2D on the caller's stream | caller | `hipStreamSynchronize(s)` before the exchange starts; upload on s | safe |
| `rns_mul.c` striped batch / `mdev_gpu_crt`: `k_crt_batch` on APU d reads all four APUs' planes `D[c].da` | NTTs on each `D[c].s` | CRT kernel on APU d | each thread syncs its stream, `omp barrier`, then launches ("all four planes complete"); a second barrier before planes are overwritten (WAR) | safe |
| `rns_mul.c batch_local`: `k_scatter4` reads operands in other APUs' regions; outputs in the other parity's regions | previous level's CRT (any APU) | this level's scatters | previous level's per-APU syncs + region end; inputs and outputs in different parities (no WAR) | safe |
| `rns_dist.c` `k_gather`, `k_bload`, `k_bsplit` read dbig quarters on all APUs; `k_scatter_runs`, `k_crt_batch` (b_core), `k_scatter_mn`, `k_unpack_rng`, `k_mn_scatter` (`newton_db.c`) store into other APUs' quarters | per-rank streams | later ops on any APU | writer's thread syncs its stream; region end / `omp barrier` before the next op | safe |
| `dbig.c` every op: kernels on APU d read all quarters (`dget`) | previous op's kernels | this op | previous op synced all four null streams + join | safe |
| `mem.c mem_dev_copy_on` (D2D on the destination APU's null stream) | copy engine | other APUs' kernels | `hipStreamSynchronize(0)` inside (the Phase 12 R fix) | safe |
| `dbig.c` pool reuse (`q_free`/`q_alloc`, VMM remap) | kernels of a finished op | the next owner (any stream) | blocks are freed only after the op's syncs (every `db_pool_free` site read: after `hipStreamSynchronize`) | safe (U4 for the remap) |

##### 2.3 CPU → device (pinned / registered host, CPU stores into device memory)

| site | CPU writer | device reader | ordering | class |
|---|---|---|---|---|
| seeds (`binsplit.c seeds_stream`), VMM path: pinned buffers → `k_copy_h2d` on `g_h2d_s[dev]` | the seed thread's OpenMP team | copy kernel, then the level kernels | team's implicit barrier → launch in the seed thread; buffer reused only after `db_copy_h2d_wait` (stream sync); main thread `pthread_join`s the seed thread before level 1 | safe |
| seeds, direct path (VMM off): CPU stores into the region (coarse-grained device memory) | seed team | level kernels | stores → join → launch (assumed contract U2) | safe |
| `dbig.c db_from_bi` (bounce) | `par_memcpy` | H2D on `g_bs[d]` | enqueue after the stores; bounce reused after the stream sync | safe |
| `rns_mul.c` staging (`node_copy`, staged batch into `D[0].hstage`) | main / APU threads | `ntt_load`, scatters | stores before the launch in the same thread or before the region; previous readers synced | safe |
| descriptors (`hipMemcpyAsync` from pageable `hd`, `hpk`, `ht`, stack `hd`) | host | kernels | the source stays unmodified until the stream sync (every site checked) | safe |
| `comm_layered.c ctab` (pinned table read by `k_bcopy`) | host | kernel | "every use is followed by a stream synchronise before the table is refilled" — checked in `v_stages`, `complete_v`, `y_alltoallv2` | safe |
| `spill.c` reader (restore) | `pread` into pinned bounce | H2D on own stream | double buffer, each reused after `stage_wait`; the target's blocks reserved before, used after `spill_restore_wait` (join) | safe |
| checkpoint restart (`ckpt_read`) | `fread` | `mem_dev_copy` | synchronous | safe |

##### 2.4 Host threads handing buffers over

| hand-off | primitive | class |
|---|---|---|
| seed thread ↔ main (`pools_ready`) | mutex + cond | safe |
| VMM background mapper ↔ pool (`v->mapped`) | `g_pool_mx` + cond; chunks zeroed and synced before `mapped` advances | safe |
| `pq_bg` (`db_pregrow`: `hipMemset` + sync, then `db_donate_ext` under the mutex) ↔ main's `q_alloc` | mutex | safe |
| `x_bg` writer ↔ main (`res_ready`) | `sem_post` / `sem_wait` (POSIX: synchronise memory) | safe |
| `mn_out` writer thread (`job_ready`, `job_taken`, `buf_free`) | semaphores | safe |
| `mn_out` recheck reader (`empty`/`full`) | semaphores | safe |
| bs checkpoint writer (`pdone`/`qdone` release/acquire, `done` release/acquire, `cancel`/`dma_active` seq_cst) | atomics | safe |
| spill writers (`ndone` acq_rel / acquire), `spill_wait` join | atomics + join | safe |
| `comm_xgmi` (`G.rb`, tables, `G.red`) | `pthread_barrier_wait` | safe |
| `comm_shmem` helper threads, words | `pthread_create/join`; SHMEM wait/quiet order the words | safe |
| `fatal.c` | seq_cst CAS | safe |
| lazy first-use allocations inside the four-thread regions (the Phase 14 A1 class) | all per-device (`g_flags[d]`, `g_red[d]`, `g_desc[d]`, `v->len`, `H.d_ts[me]`, `g_gev[dev]`), under a mutex (`bounce_init`, `g_mq_*`, `bounce_get`), `pthread_once` (`fs_init`), or done before the region (`g_spill`, `spall`, `rank_init`); remaining function-static `getenv` caches are benign same-value races | safe |

#### 3. Unclear items (runtime assumptions, no code change) and notes

- **U1** `hipStreamSynchronize` (writer's stream) is used everywhere A27 used `hipDeviceSynchronize` (writer's die). Assumed equivalent for
  the synchronised stream's work (both are a CPU wait on a system-scope-released marker in CLR). Not measured here.
- **U2** CPU stores into coarse-grained device memory (the seeds' direct path, VMM off) read by a kernel launched afterwards: assumed
  visible (the dispatch's acquire). Default path (VMM on) does not do this.
- **U3** `MEM_COPY_WAIT_POLL=1` (off by default) completes the seed DMA by `hipStreamQuery` instead of a sync; the readers are later
  kernels, not the CPU, so no CPU-side acquire is needed; assumed fine.
- **U4** VMM remap (`vmm_make_room`: `hipMemUnmap`/`hipMemMap` of wholly free chunks) may run while background threads' copies or
  kernels are in flight on other blocks. Not a memory-ordering question (no freed block is in use), but the driver's behaviour under
  concurrent kernels is not established (the node rule "never map VMM memory while GPU kernels run" is a test-safety rule).
- **N1** (EW, `mn_out.c`, not a hazard): `mn_out_early_start` calls `mn_out_boundaries` (a `dev_fetch` on the non-blocking `g_fs`
  streams) *before* its `sp_dev_sync_all()`. Correct today because X is complete by the invariant at the hook; if the hook ever moves
  to before a synchronising op, move the sync first.
- **N2** (`dbig.c`, not changed): `db_from_bi` / `db_to_bi` justify their non-blocking copies by "the source's kernels ran on the null
  stream"; the real reason is the invariant (writers on `RS[d].s` / `g_ms[d]` / other APUs are not ordered by a null-stream sync).

#### 4. The fix (H1)

`ecalc/ntt_dist.{c,h}` (0a10a99): `dist_plan.evr`; `dist_fwd_pre` records it on `s` before the first row pass and waits for it
(`hipEventSynchronize`) after enqueuing that row pass, before `chunk_post(p, 0)`; `inv_pre_y` records it before the column inverse and
waits after enqueuing the packs, before the first post. Behind nothing (a correctness fix, digits identical: an ordering, no
arithmetic). Transports that already synchronise (SHMEM, TCP, layered minor) are unaffected (their own sync waits for more).

#### 5. Tests

All on aac6, clone `~/ntt-WM15` @ 7baec40 (the fix + the two test switches, off by default), `archive:drivers/ecalc/wm15_job.sh`.

**H1 demonstrated (measured, job 21802, s24-30, 2026-09-29 01:43–01:45 EDT).** `tests/t_dist 24` with `DIST_XGMI=1` (four APUs, four
threads, 52 cyclic convolutions 2²⁰–2²⁴ points, primes 0–3: fwd x, sync, fwd y, pw, inv) and `DIST_TEST_LAG_MS=200` (rank 1's GPU
spins 200 ms before each forward's first unpack, its host runs on):

| `DIST_RB_WAIT` | run 1 | run 2 |
|---|---|---|
| 0 (the code before the fix) | **VERIFY FAILED 52 of 52** (every case has points differing) | **FAILED 52 of 52** |
| 1 (the fix, default) | VERIFY OK 52 | VERIFY OK 52 |

Plain `DIST_XGMI=1 t_dist 24` (no lag): VERIFY OK 52. So the WAR is real on this hardware whenever a receiver's GPU lags: the peers'
pushes of the inverse overwrite the receive slab before rank 1's unpack of the forward reads it; with the fix the same lag is harmless.
In production the lag needed (a whole column pass + row pass) has not been observed — every 10¹¹ run so far was identical — but nothing
prevented it.

**Gates (measured, logs in `archive:results_raw_2026-10-19/wm15/`).** `mnaccept.sh 21802 --stress --only unit,e9,mn,stress` (s24-30, 01:45–02:12 EDT):
**17/17 PASS** (unit incl. t_ntt, t_mul, t_newton, t_dbig; e9 identical in both bases; mn e8 sizes 2/3/4, e9 sizes 2/4; stress 10/10).
Job 21806 (s24-26): **4 × 10¹⁰ identical** (`total` 57.19 s); **10¹¹ identical** (fix1, digcmp against `e_1e11.out`), all four VERIFY OK.

| 10¹¹ (paired, s24-26, reference evicted) | base1 (ab8daa2) | fix1 | base2 | fix2 |
|---|---|---|---|---|
| `total` s | 167.72 | 166.96 | 168.02 | 165.33 |
| elapsed s | 170.0 | 169.2 | 170.2 | 167.6 |

Fix − base: −1.7 s mean `total` (measured, 2 each) — within the noise; **no measurable cost**. The C-form dist products
(71 at 10¹¹, 57.8 s) are the ones that run the pipeline with the wait.

#### RESUME

- Step 0: branch `p15-WM` from ab8daa2; skeleton.
- Step 1 (2026-09-29 01:20 EDT): audit complete (§1–§3); H1 fixed and committed (0a10a99). aac6 clone `~/ntt-WM15` @ 0a10a99 built;
  baseline worktree `~/ntt-WM15-base` @ ab8daa2 built (for the paired 10¹¹).
- Step 2 (01:25 EDT): test switches `DIST_RB_WAIT` (0 = the H1 wait off) and `DIST_TEST_LAG_MS` (rank 1's GPU lags) added, off by
  default, README rows (dbce20d, f036410); `wm15_job.sh` A = the H1 demonstration (t_dist over xGMI) + mnaccept, B = 4 × 10¹⁰ + 10¹¹
  base/fix/base/fix (7baec40). aac6 `~/ntt-WM15` @ 7baec40 rebuilt; driver `./wm15_job.sh A; ./wm15_job.sh B` started 01:24 EDT under
  setsid nohup (job 21802 pending behind PC / SX / DL; cdash holds s24-26/30). Logs: `~/ntt-WM15/ecalc/archive:results_raw_2026-10-19/wm15/<A|B>_<job>/driver.log`,
  mnaccept's in `results/mnaccept/<job>/`.
- Step 3 (02:50 EDT): both batches done (jobs 21802, 21806, cancelled by the driver); logs copied to `archive:results_raw_2026-10-19/wm15/`; §5 filled.
  **DONE.** Branch `p15-WM`: 0a10a99 (the fix), dbce20d / f036410 (test switches, README), wm15_job.sh. Nothing owed; no patches for
  EW / DL / PC files (no hazard found there). aac6 clones `~/ntt-WM15`, `~/ntt-WM15-base` (worktree) can be removed.

## N414 — `t_mn_grid` on real nodes over SOS, `mnrun.sh`'s SHMEM detection, the dist tier's load speed (Phase 14, PLAN §33 A4, A5)

(former file: N414.md)

Branch `p14-N4` (from `main` @ a839418). aac6 clones: `~/ntt-N414` (plain build, A5) and `~/ntt-N414-sos`
(`make SHMEM_HOME=~/sos`, A4), both at the branch head. Scripts and logs on aac6 in `~/N414/` (`a4.sh`, `a5.sh`,
`a5g.sh`, `props.sh`, `thptouch.c`; `a4*/`, `a5/<batch>/`, `a5/summary.txt`). Labels: **measured** (aac6, node named),
**modelled** (`BS_LAYOUT_ONLY`), **assumed**.

#### A5 — the answer: the memory state (free memory in 2 MiB blocks), not the node's hardware. Fix: map the plane pools first

**Cause (measured on all three nodes).** The dist tier's per-call load (the gather of both operands into the plane
pools) and its transforms (on the plane pools) run 4–10 × slower when the run's device total exceeds the node's free
memory in blocks of ≥ 2 MiB (buddy orders ≥ 9, `/proc/buddyinfo`). `rns_init` maps the region arenas first (the seeds
stream into them) and the plane pools **last**, so near the edge the planes are built from what is left: free pieces
below 2 MiB. Mid-run buddyinfo at 1.42 × 10¹¹ shows the ≥ 2 MiB supply exhausted on every node (0–3 GB per NUMA node,
19 GB free in all). That the GPU then maps the planes with small page-table fragments and loses on every gather and
transform pass (TLB reach) is the mechanism **assumed** from this; what is measured is the ordering effect below.

The supply differs between the nodes and is what separates them (all three: the same kernel 6.8.0-134, THP `always` /
defrag `madvise`, the same amdgpu parameters (md5 of the parameter dump identical), the same clocks, 518–519 GB free,
page cache ≤ 0.6 GB):

| node (uptime) | free in ≥ 2 MiB blocks at start (per NUMA node) | 1.42 × 10¹¹ probe, default order: load / ntt s per call | the same with `RNS_PLANES_FIRST=1` |
|---|---|---|---|
| s24-16 (8 days) | **447–449 GB** (101 + 115 + 113 + 118) | **0.94–1.04 / 1.15–1.25** (six runs: b1 p1–p4, b2 d1, b7 dc) | **0.095–0.096 / 0.250** (b2 pf1, pf2) |
| s24-26 (19 days) | **464 GB** (110 + 117 + 120 + 117) | **0.696 / 0.882** (b6 d) | **0.096 / 0.236** (b6 pf) |
| s24-30 (2 days) | **474–476 GB** (114 + 120 + 121 + 119) | **0.099 / 0.266** (b5 d) | **0.099 / 0.259** (b5 pf) |

(Probes: `ECALC_PLANE_CAP=2^30 RNS_STRATEGY=C RNS_VERBOSE=1 ./ecalc 142000000000`, the first 240 s of the run, 55–196
dist calls at 2³⁰ each; device 501.4 GB measured = modelled; free memory ≥ 500 GB before each.) The default order is
fast only where the supply comes close to the need (s24-30), half-slow a little below it (s24-26), fully slow below
that (s24-16); with the planes mapped first, all three nodes run at the same 0.095–0.099 s. The same grading on one
node by size (s24-16, default order):

| size (s24-16, same probe) | device GB (modelled = measured) | 2³⁰ calls in 240 s | load s / call | ntt s / call |
|---|---|---|---|---|
| 1.20 × 10¹¹ (b2 s120) | 433.1 (< the 447 GB supply) | 214 | **0.100** | 0.250 |
| 1.30 × 10¹¹ (b2 s130) | 464.1 | 117 | 0.513 | 0.697 |
| 1.42 × 10¹¹ (b2 d1) | 501.4 | 62 | 0.978 | 1.187 |
| 2 × 10¹⁰ (b1 s1, whole run, 19 calls at 2³⁰) | ≈ 70 | — | 0.06–0.11 | 0.22–0.29 |
| for reference: G13d's C 1.42 × 10¹¹ on s24-16 / P13b's on s24-30 (their logs) | 501.4 | — | 0.92 / 0.094 | 1.07 / 0.24 |

So G13d's 0.89 s against P13b's 0.094 s per call is this: s24-16's smaller 2 MiB supply (P13b's s24-26 run at 1.44 ×
10¹¹, 0.6 s per call, sits between, as s24-26 does here).

**What the supply is, and why a user cannot raise it (measured, s24-16).** 518 GB are free, but only 448 GB of it in
≥ 2 MiB blocks; the other ≈ 70 GB is scattered in orders 0–8 (node 0 worst: 22425 order-10 blocks against 28231 on
s24-30). It is not the page cache (`Cached` 0.2–0.6 GB on every node throughout; a 200 GB cache fill was attempted but
the node's root file system is not writable, so it was not tested) and not reclaimable: a process that touches 420 GB
of `MADV_HUGEPAGE` memory gets 100 % huge pages, one that touches 490 GB gets 450.7 GB (92 %) — direct compaction
under THP defrag creates no new 2 MiB blocks — and after it the supply is unchanged (101 + 116 + 113 + 118 GB) and the
next default probe is as slow (b7 dc: 0.969 s). Four back-to-back 1.42 × 10¹¹ runs (alloc/free cycles) do not change
it either. The scattered free pieces sit between unmovable kernel pages; the supply falls with uptime and use (s24-30:
2 days, the most; s24-16: 8 days, the least; s24-26 at 19 days is between, so use matters more than time) — **assumed**:
only a reboot (or an administrator's compaction, which the measurement above suggests would not help much) restores it.
`peer access` time (0.75–20 s) does not track the load.

**Fix**: `RNS_PLANES_FIRST=1` (commit 487458a, `rns_mul.c` `rns_init`, off by default): the plane pools are mapped
before the region arenas, and the seeds start after them. The arenas then take the sub-2 MiB remainder; their
contents are read once per product, and no slowdown of the tree or the division shows (the whole runs below).

##### The switch: gates and whole runs (job 21284, s24-16, measured; `~/N414/a5/b3/`)

| test | command | result |
|---|---|---|
| e9, both bases | `RNS_PLANES_FIRST=1 ./mnaccept.sh $J --only e9` | **PASS ×2**: LIMB_BASE=10 identical, 11.06 s; LIMB_BASE=2 identical, 21.91 s |
| 4 × 10¹⁰ | `RNS_PLANES_FIRST=1 ./ecalc 40000000000 /tmp/n4_4e10.out`, sha1 vs `~/e_4e10.sha1` | **IDENTICAL**, VERIFY OK, total 65.19 s (phases 53.0, init 12.2) — the default's 63.5 ± 1.5 s: within ≈ 1 s (one run) |
| `t_newton 20`, `t_mul 20` | `./tests/t_newton 20`, `./tests/t_mul 20` (the switch is not read by the tests; the default path is the pre-change code) | VERIFY OK, 0 failed, each |
| **1.42 × 10¹¹, 2³⁰, C, the switch** | `ECALC_PLANE_CAP=2^30 RNS_STRATEGY=C RNS_PLANES_FIRST=1 RNS_VERBOSE=1 ./ecalc 142000000000` | **VERIFY OK, total 672.03 s** (bs 223.9, dm 397.1 (recip 179.6), phases 621.2; init 50.9), wall 11:16; 705 dist calls at 2³⁰: load 0.105, ntt 0.253 s per call; recip: 510 calls, load 46.8 s, ntt 115.2 s |
| (G13d, the same configuration without the switch, same node) | `~/g13d/b_230C_142e9.log` | 1349.66 s (phases 1306.2; init 43.5); recip load 452 s, ntt 544 s |
| (P13b, s24-30, without the switch) | P13b b3 log | 677.2 s (phases 642.8; init 34.4) |

**1.42 × 10¹¹ at 2³⁰ on s24-16: 1349.7 → 672.0 s (2.01 ×)**, the s24-30 speed of P13b (−5 s), for +7 s of init (the
seeds start after the plane pools are mapped instead of alongside them).

##### The default configuration near the edge, and `auto` (job 21285, s24-16, measured; `~/N414/a5/b4/`)

| run | switches | result | total (phases; init) | dist calls at the cap: load / ntt s per call | recip: load / ntt s |
|---|---|---|---|---|---|
| 1.30 × 10¹¹, **main's defaults** (2³¹, auto; 507 GB device modelled) | — | VERIFY OK | **497.65 s** (439.1; 58.5) | 43 at 2³¹: 1.636 / 1.934 | 87.0 / 142.8 (202 calls) |
| 1.30 × 10¹¹, defaults + the switch | `RNS_PLANES_FIRST=1` | VERIFY OK | **364.72 s** (326.0; 38.7) | 43 at 2³¹: 0.272 / 0.455 | 27.5 / 44.5 |
| (P13b, s24-30, 1.30 × 10¹¹ at 2³¹, then-default C) | — | VERIFY OK | 353.1 s (314.5; 38.6) | — | — |
| 1.42 × 10¹¹, 2³⁰, auto + the switch | `ECALC_PLANE_CAP=2^30 RNS_PLANES_FIRST=1` | VERIFY OK | **863.16 s** (817.6; 45.5) | 11 at 2³⁰: 0.108 / 0.282 | 75.0 / 93.5 (829 calls) |
| (G13d, s24-16, the same without the switch) | `ECALC_PLANE_CAP=2^30` | VERIFY OK | 1080.90 s (1020.9; 60.0) | — | — |

So main's own defaults lose 1.36 × at 1.30 × 10¹¹ on this node (497.7 → 364.7 s) — any run whose device total passes
the node's ≥ 2 MiB supply (≈ 447 GB on s24-16) is affected, whatever the cap or strategy. And G13d's conclusion that
`auto` beats C at 1.42 × 10¹¹ / 2³⁰ (1021 vs 1306 s of phases) was an artefact of the slow planes: with the planes in
2 MiB blocks C is the faster (621.2 vs 817.6 s of phases) — for B3 (`auto`'s cost model).

#### A4(a) — `t_mn_grid` on real nodes over Sandia OpenSHMEM: VERIFY OK at 2 and 3 nodes (measured)

The SOS build `~/ntt-N414-sos/ecalc` (`make SHMEM_HOME=~/sos`), `COMM_TRANSPORT=shmem`, host heap (the default pool
8192 MiB), launched through wrappers with **no** `COMM_SHMEM_IMPL` (the new detection chooses SOS):
`COMM_TRANSPORT=shmem SLURM_JOB_ID=$J ./mnrun.sh <p> stdbuf -oL ./tests/t_mn_grid <scale> 25`. One PE (= one
node-process driving four APUs) per node; the only inter-node link is the 1 GbE. Logs `~/N414/a4/`.

| nodes | test | result | launch-to-exit | product times on node 0 (one plane / 2 pieces / 2×3 / 3×1 / subgroups) |
|---|---|---|---|---|
| 2 (s24-26 + s24-30, job 21282) | `t_mn_grid 0.1 25` (unit 3.36 M limbs, plane cap 2²⁵) | **VERIFY OK (200 checks) on both PEs** | 219.9 s | 0.90 / 1.24 / 2.14 / 2.09 / 2.14 s |
| 2 (s24-26 + s24-30, job 21282) | `t_mn_grid 0.25 25` (unit 8.39 M limbs) | **VERIFY OK (200 checks) on both PEs** | 583.6 s | 2.43 / 3.97 / 7.78 / 4.22 / 4.35 s |
| 3 (s24-16 + s24-26 + s24-30, job 21283) | `t_mn_grid 0.1 25` (3 × 4 ranks, the general map) | **VERIFY OK (200 checks) on all three PEs** | 340.0 s | 1.74 / 1.82 / 3.52 / 3.33 / 3.47 s |
| 2 (job 21282) | `t_comm` behind `stdbuf -oL` / `timeout 250` / `numactl --interleave=all` / `env X=1 stdbuf -oL timeout 250` | VERIFY OK on both PEs, each | 2.1–2.3 s | — |
| 3 (job 21283) | `t_comm` behind `env X=1 stdbuf -oL timeout 250` | VERIFY OK × 3 | 7.2 s | — |

This closes S13d open issue 2 (the multi-node product tier's shapes test at 2 and 3 real nodes over SOS; `0.5 27`
remains too slow for the 1 GbE, and is not needed: the shapes are the same at every scale).

#### A4(b) — `mnrun.sh`'s SHMEM detection (commit 278daf9)

Every word of the command that resolves (`command -v`) to an executable ELF file is checked (`readelf -d` NEEDED,
then `ldd`) for `libsma` / `liboshmem`; the first that links either decides; wrapper options (`-x`) and assignments
(`X=Y`) are skipped; none found: oshmem as before, with a note on stderr. `COMM_SHMEM_IMPL` must be `sos` or `oshmem`
(anything else exits 1). `MNRUN_SHOW_IMPL=1` prints the choice, `=only` prints and exits (test hook).

Tested on aac6 (job 21267, `~/N414/a4_try1/summary.txt`), the SOS build `~/ntt-N414-sos/ecalc/tests/t_comm` and the
OSHMEM build `~/ntt-S13d-osh/ecalc/tests/t_comm`, behind each wrapper: none, `env X=1`, `stdbuf -oL`,
`timeout 600`, `numactl --interleave=all`, `nice -n 0`, `setarch x86_64 -L`, and all of `env stdbuf timeout numactl`
chained — **sos and oshmem detected correctly in all 16 cases**; `bash -c ./tests/t_comm` → sos (the path word
resolves); `COMM_SHMEM_IMPL=bogus` → refused. The four real launches through the wrappers reached SOS's
`shmem_init` / `shmem_malloc` on both PEs (the SOS launch form); they then failed on the 8 GiB pool because the batch
exported `COMM_SHMEM_DEVHEAP=1`, which `t_comm` (no HIP) cannot use. Rerun without it (job 21282, 2 nodes, and 21283,
3 nodes, table above): every wrapped launch VERIFY OK. (Before the change, a wrapper word made `mnrun.sh` pick OSHMEM's
`--mpi=pmix` + `setarch -L` form for the SOS binary — S13d §3.1's failed 3-node launch.)

#### Recommendations (the user's decision)

1. **`RNS_PLANES_FIRST=1` as the default.** Measured: 1.42 × 10¹¹ / 2³⁰ / C 1349.7 → 672.0 s and main's own defaults
   at 1.30 × 10¹¹ 497.7 → 364.7 s on s24-16; 4 × 10¹⁰ 65.2 s (one run, within the series' spread); output identical
   (e9 both bases, 4 × 10¹⁰). Cost: the seeds start after the plane pools' mapping instead of during it (init +7 s at
   1.42 × 10¹¹ in one run, −20 s at 1.30 × 10¹¹ in another: init varies more than that). At the target's share
   (7.64 × 10¹⁰, 354 GB) the device total is below every node's supply here, so the default order would be fast there
   too — unless the target's nodes have a smaller supply; the switch makes that irrelevant.
2. **A preflight line**: print the node's free memory in ≥ 2 MiB blocks (buddyinfo orders ≥ 9, per NUMA node) at init
   next to the modelled device total, so a slow node is visible before the run (not implemented).
3. For B3: rerank `auto` against C with the switch on (C 621.2 vs auto 817.6 s of phases at 1.42 × 10¹¹ / 2³⁰).

#### Open issues

- The mechanism below the ordering (the GPU page-table fragment size of TTM-allocated memory) is inferred, not
  measured (no user-level view of the GPU page tables).
- The page-cache variant of the memory state was not tested (the nodes' root file system is not writable to users;
  a cache fill needs a writable local disk or a large NFS read over the 1 GbE).
- Whether the region arenas' small-page tail costs anything is only bounded by the whole runs (672 s vs P13b's 677 s).

#### Files outside mine

`ecalc/rns_mul.c` (the switch, 8 lines in `rns_init`), `ecalc/mnrun.sh` (the detection), `ecalc/README.md` (three rows:
`COMM_SHMEM_IMPL`, `MNRUN_SHOW_IMPL`, `RNS_PLANES_FIRST`). Scripts in `archive:results_raw_2026-10-19/n414/` (copies of `~/N414/*.sh`,
`thptouch.c`). No default changed.

#### RESUME

**Done (12:00 CDT).** A4(a) VERIFY OK at 2 (0.1 and 0.25) and 3 (0.1) real nodes over SOS; A4(b) detection fixed and
tested behind every wrapper; A5 cause found on all three nodes, the fix `RNS_PLANES_FIRST=1` gated (e9 ×2, 4 × 10¹⁰,
`t_newton`, `t_mul`) and measured in whole runs. Jobs 21267, 21270, 21272, 21277, 21280, 21282–21285, 21287–21289 all
finished or cancelled; no job of N4 running or pending. Nothing left to resume.

## FIX117 — FIX1 17 — t_edge vmm, mn_selftest power-of-two>=128, MN_TOPO_GROUP under SHMEM

(former file: FIX117.md)

Branch `p17-fix1` (pushed, not merged). Commits: c686267 (the three fixes), c764c91 (NTT_LOGN_MIN clamp on the
debug knob). Built/tested on aac7, `~/fix17a` (a clone of this branch).

#### Design

1. `tests/t_edge.c`: added a `vmm` form -- `hipMemCreate` (pinned, `location.type = Device`, round-robin over
   the 4 APUs) + `hipMemAddressReserve` + `hipMemMap` + `hipMemSetAccess`, touched with `hipMemset`, freed with
   `hipMemUnmap`/`hipMemAddressFree`/`hipMemRelease`. Same step/cap/report format as host/dev/malloc.
2. `mn.c` `mn_selftest`: grows `logR` (mirroring `mn_selftest_layered`'s `MN_SELFTEST_GROW`) when `R/ranks < 32`
   -- the real bug at >=128 ranks (`k_twpack`'s grid is `rk/32` blocks, 0 blocks at `R/ranks` = 16, a silent
   no-op, not a HIP error). Added `MN_SELFTEST_LOGR` (debug: force a small starting `logR`) and clamped it to
   `NTT_LOGN_MIN` (10) since the column pass (`ntt_fwd(..., p->logR, ...)`, `ntt_dist.c:327`) needs `logR` itself
   to be a valid transform size, independent of the rows-per-rank rule. `ntt_dist.c` `fwd_prod`: added
   `HIP_CHECK(hipGetLastError())` after the pack kernel launch.
3. `mn.c` `mn_init`: `MN_TOPO_GROUP > 1` under `COMM_TRANSPORT=shmem` is now fatal at init with a named-cause
   message (B9: the cross-group mesh id collides with the next level's `all[d]` id, `comm_shmem.c`'s
   "communicator id used twice" abort). `README.md`'s `MN_TOPO_GROUP` row updated.

#### Why the >=128-rank case cannot be reproduced end to end on this hardware

`R >= 2^NTT_LOGN_MIN = 1024` always (the column transform's own floor). At 2 ranks, `R/ranks >= 512`, far above
the 32 threshold that triggers growth -- no real rank count reachable on aac7 (13 nodes) or any test allocation
gets near the >= 33 ranks (at the floor) / >= 65 ranks (at the production default `logR=11`) needed to hit
`R/ranks < 32`. Checked instead by direct arithmetic (matches `mn_selftest_layered`'s already-shipped, identical
growth formula) and by confirming the debug knob + growth code path runs cleanly at 2 ranks once clamped to the
transform floor (`mn: self-test, four meshes over 2 nodes at 2^21 points: ok`, `MN_SELFTEST_LOGR=4`,
`POOL_LOG=27`).

#### Tests on aac7 (job 12294, `srun --jobid=12294 --overlap -N1`)

- `t_edge vmm 8 560` and `t_edge vmm 2 100` run alone on the node (checked `rocm-smi --showpids` first: no KFD
  PIDs). **Measured** (step 2 GB, apples to apples): `vmm` edge 82.0 GB (41 steps before `hipMemCreate`
  out-of-memory), `dev` edge 88.0 GB (44 steps before `hipMalloc` out-of-memory) -- VMM shares dev's edge within
  ~7%, confirming A2's open question for this job's environment. `host` edge (hipHostMalloc, step 8 GB, cap 460):
  416 GB before the node OOM-killed the step (`rocm-smi --showmeminfo vram` reads 22.9 GB "VRAM Total" per
  logical GPU here -- this job's node is in a 4-way GPU partition, `lspci`/`rocm-smi --showbus` shows all 4
  "devices" as PCI functions `.0`-`.3` of one `02:00` device, not 4 separate MI300A dies; this caps `dev`/`vmm`
  far below the ~96 GiB/APU `amdttm.pages_limit` noted elsewhere, while `host` draws from the much larger
  node-wide unified pool and is comparatively unaffected). **This partition state, not the code, is also why
  `mnaccept`'s device-heavy unit tests (`t_mul`, `t_bs`, `t_dbig`, `t_newton`, `e9`) OOM on this job** -- they
  request 12-17 GB per logical GPU, right at this job's ~22.9 GB cap once a couple of allocations stack up.
- `mn_selftest`: default behavior unchanged (`mn: self-test, four meshes over 2 nodes at 2^22 points: ok`,
  `POOL_LOG=27`, no override). Forced small-rows path (`MN_SELFTEST_LOGR=4`): clamps to `logR=10`, runs clean
  (`2^21 points: ok`) instead of crashing ("ntt: logn 6 out of range") as it did before the clamp commit.
- `mnaccept.sh 12294 --only unit,e9`: run **twice**, confirmed and collected. (1) Concurrently with the 416 GB
  host-edge test running on the same node: 4 passed, 7 failed, and left two orphaned `ecalc` KFD processes
  holding ~18.7 GB each (cleared before run 2; `rocm-smi --showpids` confirmed "No KFD PIDs currently running").
  (2) Solo, nothing else on the node (checked `rocm-smi --showpids` immediately before launch, and via the
  detached watcher at `~/fix17a/R/watcher.log` / `~/fix17a/R/acc_summary.txt`, collected with
  `ssh aac7 'cat ~/fix17a/R/acc_summary.txt'`): **identical result, 4 passed, 7 failed**
  (`t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`, `t_newton 20`, `e9 size 1` both limb bases; `t_verify`, `t_out`,
  `t_patch`, `t_mn_grid (2 procs)` passed both times). Identical results with and without contention rules out a
  transient-contention explanation and confirms the ~22.9 GB/logical-GPU partition cap: e.g. `t_dbig`'s log shows
  a *single, first* `hipMalloc` of 17.18 GB failing out-of-memory on a fresh process (`device 0.0 GB in use`
  beforehand) -- not a cumulative/fragmentation effect of other tests, a hard ceiling below what `t_mul`/
  `t_bs`/`t_dbig`/`t_newton`/`e9` need to even start. None of the three fixes touch these tests' code paths
  (`e9` at size 1 doesn't call `mn_init` at all; `t_mn_grid` does and passed). **11/11 is not achievable on this
  job as currently partitioned; the 4 that do not need > ~15 GB on one logical GPU pass cleanly both times.**

#### RESUME

- Branch pushed through 497476b (c686267 the three fixes, c764c91 the `NTT_LOGN_MIN` clamp, 497476b this file).
  Nothing uncommitted in the worktree.
- Open / handoff: `mnaccept --only unit,e9` cannot reach 11/11 on job 12294 while it exposes each MI300A as a
  4-way GPU partition (~22.9 GB/logical-GPU VRAM, confirmed via `rocm-smi --showmeminfo vram` and `--showbus`
  showing all 4 "devices" as PCI functions `.0`-`.3` of one `02:00` device). This is the node's current
  partition state, not a code regression -- re-run `mnaccept.sh 12294 --only unit,e9` once the node is
  repartitioned to 4 full MI300A dies (or on a fresh non-partitioned allocation) for a true 11/11 gate.
- Never touched 12287. Used 12294 via `--overlap` only; checked `rocm-smi --showpids` before every exclusive
  (`vmm` alone, and both `mnaccept`) run.
