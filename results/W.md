# W — verification and housekeeping (Phase 12, PLAN.md §27 row W; DECISIONS2 §8)

Branch `w12` (from `main` @ 7aded87; the aac6 clone `~/ntt-w`). Files: `ecalc/ecalc.c` (the output tail), `ecalc/mn_out.c/.h`,
`ecalc/mnaccept.sh` (the `recheck` step and the `--full` step's recheck — one block each, R's `--stress` block is separate),
`ecalc/README.md`, `.gitignore` (unchanged: verified). Touched outside my list, minimal and commented `Phase 12 W`:
`ecalc/binsplit.c` / `.h` — two globals (`bs_ckpt_own_buf`: the checkpoint DMA buffers malloc'd instead of the pinned
staging; `bs_ckpt_tree_pdone`: the count of a tree set's files whose P part is written, one atomic increment in
`tree_io`) — 6 lines, no change to the file format or to any existing path.

## 1. `ECALC_CKPT_TOP` on by default above 10¹⁰ digits

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

## 2. The recheck as a regression step (`mnaccept.sh recheck`)

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

## 3. A 4 × 10¹⁰ run rechecked from its files

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

## 4. The host-flow stand-ins (`MN_DM=host`, `MN_COMBINE=host`) — **kept**

Checked at the end of the session (`ls /home/machinus/apucode/ntt/.claude/worktrees/*/results/R.md`): one `R.md`
(agent-a50c987e…, 575 bytes, 19:33) — the header names a one-line fix in `mem_dev_copy_on` (`mem.c`) and the stress
step, and says "(filled in below as the batches complete)"; no cause statement, no 40/40 count, and R's job was still
running (20904) when I closed. The rule was "delete only if R.md says the cause is fixed and 40/40 passed": it does not
yet, so the stand-ins stay — `ecalc.c`'s `MN_COMBINE=host` combine and `MN_DM=host` gather + host division, `mn_out.c`'s
`mn_out_scatter_standin` (the host X scattered into shares), `mn.c`'s `mn_gather_host` (M3's end: the shares assembled on
node 0's host) and the host-`X` path of `newton_db_divmod` that only `MN_DM=host` reaches at size > 1 — untouched, listed in the README's switch table as
*stand-in … kept while DECISIONS2 #1 is open*. Deleting them is a 20-minute edit for the integrator once R's numbers are
in: the three `MN_COMBINE`/`MN_DM` tests in `ecalc.c` (lines `host_combine`, `mn_dist`, `mn_dm`), the `else` branches
they guard (the M2 combine loop, the `mn_gather_host` branch), `mn_out_scatter_standin` and its declaration,
`mn_gather_host` in `mn.c`; then the regression (no step uses them).

## 5. README and the switch list

`ecalc/README.md`: the header at the Phase 11 numbers; the two old switch paragraphs and the multi-node variable table
replaced by pointers; the recheck paragraph and the regression paragraph in the Phase 12 form; a **Switches** section at
the end — every `getenv` in `ecalc/*.c *.h` (108 variables), grouped `ECALC_` / limbs and pools / transform and tiers
(`NTT_`, `RNS_`, `DBIG_`) / `BS_` / `NEWTON_` / `COMM_`, `MN_` / `DIST_`, one line each with the default, debug and
test switches marked; the five test-only variables named. Verified by grep (both directions):

    grep -ho 'getenv("[A-Z0-9_]*")' *.c *.h tests/*.c | sed 's/getenv("//;s/")//' | sort -u > live
    grep -o '`[A-Z][A-Z0-9_]*' README.md | tr -d '`' | sort -u > listed
    comm -23 live listed      # live, not listed: (empty)
    comm -13 live listed      # listed, not live: the prefixes, the three names listed as deleted, ECALC_REF / ECALC_REF_4E10 (mnaccept.sh's), words

`results/*.md` tracked: `.gitignore` has `results/*` + `!results/*.md`; `git check-ignore -v results/W.md` reports the
negation rule (not ignored), `git ls-files results` lists the 21 write-ups.

## Gate runs

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

## Open issues

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
