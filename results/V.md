# V — verification and cleanup (Phase 11, PLAN.md §26 row V: D5, ECALC_RECHECK, E1 (c), E2)

Branch `v11` (from `main` @ 72aa2e9; the aac6 clone `~/ntt-v`). Files: `ecalc/verify.c`, `ecalc/mn_out.c/.h`, the
residue kernel of `ecalc/dbig.c` (+ `dbig.h`), the output tail and the T1 flow of `ecalc/ecalc.c`, `ecalc/tests/t_dist.c`,
`ecalc/ntt_dist.c/.h` (the two deleted functions), the seeds of `ecalc/binsplit.c` (the deleted path) plus a
commented instrumentation block there, `.gitignore`, `ecalc/README.md`, the scripts `ecalc/v11_d5.sh`, `ecalc/v11_recheck.sh`.
Touched outside my list, minimal and commented `Phase 11 V`: `ecalc/newton_db.c` (five lines of logging in
`mdb_mod_qs`), `ecalc/mem.c` (`MEM_DPOOL_FILL`, a test knob in `dpool_get`), `ecalc/binsplit.c` (the level loop calls
`bs_res_check` once per level; the leaf hand-over check).

## D5 — the intermittent size > 1 verification failure

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
level's scatter kernel are the one CPU-to-GPU hand-over there), under the forced growth of plane pool 1 — the configuration A-mem's rule excludes (`results/A-mem.md`: pool 1 at the full pool for
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

## `ECALC_RECHECK=1` — the standalone recheck (mn_out.c)

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

## Formats that changed

- The T1 moduli (`t1_q`): every residue printed by T1/RES lines and stored in the `.t1` sidecar changes; the digits and
  every file format the checkpoints use do not (the moduli never enter them). A sidecar records its moduli and the
  recheck refuses one written with others.
- New: `<outfile>.t1` (node 0 writes it after the checks; text, ≈ 400 bytes). New, optional: the size-1 top-level set
  `tree_000.{hdr,r0..r3}` in `BS_CKPT_DIR` with `ECALC_CKPT_TOP=1` (level 0 is never seen by the restart logic, which
  scans levels ≥ 1). The multi-node tree sets are unchanged.
- `mn_out` gained `tail[24], ntail` (the computed digits after d_out); `struct pq_bg` a `joined` flag.

## E1 (c) — deleted

`DIST_PLANE2` (`dist_fwd2`, `dist_inv2` in ntt_dist.c/.h and `t_dist`'s `plane2` mode), `BS_SEED_DIRECT=0` (the per-chunk
DMA path of `seeds_stream`: a buffered chunk is now always copied once the regions exist, the direct stores are the
only path) and the `ECALC_OVERLAP_COPY` remnant (a comment; its code was already gone). The one-line switches stay.
**The host-flow stand-ins `MN_DM=host` / `MN_COMBINE=host` are kept**: D5's residual fault is in bs, not closed, and
`MN_DM=host` remains the only cross-check of the sharded division against the host one (PLAN §23-1 (a)).

## E2

`.gitignore`: `results/*` + `!results/*.md` (the write-ups tracked by rule; `results/wp6/` etc. still ignored).

## Gate runs

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

## Open issues

- The leaf-level race under in-phase pool growth (above): open, owner P / N-kernel; `v11_d5.sh` reproduces it. Until
  it is closed the host-flow stand-ins stay (the `MN_DM=host` cross-check) and A-mem's no-growth rule is the guard.
- The recheck at size 1 needs `ECALC_CKPT_TOP=1` at run time (2 × the top-level P, Q written: 35 GB at 4 × 10¹⁰);
  without it P, Q come from the sidecar and only the digits, X and the recurrence are recomputed independently.
- The recheck reads the part files where they were written (node-local `/tmp` at size > 1 means the same nodes).
- `ECALC_RES_LOG=1` copies every reduced number to the host for the cross-check — a debug mode (seconds at 10¹⁰,
  ≈ 30 s of bs per run with the level probe).
