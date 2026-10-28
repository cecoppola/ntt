# ROUTE.md - the stages (adaptive; the goal is digits per session time)

Scripts: `remote/run/*.sh`; all end with a summary (contract in AGENT.md). Machine facts, planner limits, time model,
output glossary, known problems: `FACTS.md` (sections of those names). Paste `bash remote/run/<script> ...` from the
repository root. `chain.sh "<script args>" ...` runs several scripts in one paste and stops at the first FAIL.
Budgets are for the session clock excluding queue waits (permissive queue: expect short ones). Time per stage = budget.

| stage | budget | round trips |
|---|---|---|
| S0+S1 check, facts, allocation | 15 min | 1 |
| S2+S3 build, gates | 30 min | 1-2 |
| S4 tuning at 8 nodes | 30 min | 2-3 |
| S5 scale-up 8 > 64 > 576 | 40 min | 3-4 |
| S6 record attempts | 60+ min | 3-5 |
| S7 report | 5 min | 1 |

Total 12-20 round trips. Tell the user the allocation time you ask for at S1: (session hours) minus 15 min.

## S0 + S1 (one paste)

```
bash remote/run/chain.sh "s0_check.sh" "alloc.sh --nodes 576 --time <H:MM:SS>" "s1_facts.sh"
```
Holding all 576 nodes costs the same wait as 64 if the queue is idle; if alloc does not start within 5 min (the summary
says PENDING) re-run `alloc.sh` with `--nodes 64`, later with 8: a PENDING hold of another size is cancelled and replaced
by the new one, a RUNNING hold of another size is refused (`alloc.sh --release`, then `alloc.sh --nodes N`); subsets are
chosen inside the allocation, scale-up then uses what was granted). Add `--partition/--account` after the first error says which are needed (s0 lists partitions).
Go on if: s0 PASS (manifest clean; python3, make, sbatch, srun, sinfo, squeue, scontrol present (any missing = FAIL); a compiler, `module` and ROCm/SMA/libfabric modules found, salloc optional (any missing = PARTIAL, read `tools_missing_optional` and the `module.*` lines)) and s1 PASS (checked:
4 GPUs and 8 CXI NICs per node = `gpu_count`, `nics_cxi`; SHMEM test rc 0 in `shmem_test_rc`; `site.env` written). s1 PARTIAL
with `advisory: NICs per node = N`: the NIC count is not 8, read FACTS known problems before going on. s0 PARTIAL with
`exec_bits=missing`: run the `chmod +x ...` printed in `next=` (the copy lost the execute bits), then s0 again.
s0 FAIL on changed files: have the user unpack again. Module not found: ask for `module avail <name>` only.

## S2 + S3 (one paste, build then gates)

```
bash remote/run/chain.sh "s2_build.sh --path both --jobs 32" "s3_gate.sh --nodes 1,2 --path both --digits 1e9"
```
Go on if: build PASS for both paths (`-DXSHMEM_OFI` and `-DXSHMEM_CRAY` in the flags seen) and every gate `VERIFY OK` with
hash bf9f4b08758b40d9 at 1e9. A path that fails to build or gate is dropped (write it in PROBLEMS) - one working path is
enough to continue. When only one path builds, `s2_build.sh --path both` ends PARTIAL (not FAIL), `chain.sh` goes on, and `s3_gate.sh --path both` skips the missing path (`SKIP (no path N binary)`, PARTIAL) and gates the built one: no flag is needed in that paste; the summaries say `carry_on`, and every later command uses `--path <built path>`. The S6 route needs path 1; with path 1 missing, take the largest planner-approved size on path 2. Both fail: failure handling (AGENT.md); no digits-producing patch without a gate re-run.
Optional second gate paste when time allows: `s3_gate.sh --nodes 2 --digits 1e10` (hash 3fc025473586ed22).

## S4 short tuning at 8 nodes (digits 6.4e10 = 8e9 per node; each paste = one A/B experiment, ABBA)

1. Path first (both paths gated): one chain paste with `s5_run.sh --nodes 8 --digits 64e9 --path 1` and the same with
   `--path 2`. If the walls differ by more than 8 %, take the faster; else prefer path 2 (the target's shape).
   Path 2 is refused by memory at s2/s3 (S6), so the S6 route uses path 1 whatever S4 says; keep both numbers.
2. Then at most 2-3 A/B settings, only those with a payoff worth the 6-10 min each. Candidates (source: bench
   `QUESTIONS.md` rows TQ19, TQ47, TQ49, TQ81, TQ82; `ladder/ADJUST.md` section 3 rows 5-7 and 12):
   - `ECALC_PLANE_CAP` 2^30 (default) vs `2^31` (TQ82; on aac7 2^30 saved ~2.7 s at 8e9 per node);
   - `ECALC_NP` auto vs 3 vs 4 (TQ47; compute-bound `bs` phase);
   - `MN_T_CHUNK_MB` 2048 vs 1024 (dm phase, row 6; also the memory edge, row 7);
   - path 2 only: `XSHMEM_LANES` 1 vs 2 (both NICs of an APU);
   - `ECALC_BG_THREADS` 24 vs 48 (TQ49) when `init` dominates.
   Example: `bash remote/run/s4_ab.sh --nodes 8 --digits 64e9 --path 2 --a "XSHMEM_LANES=1" --b "XSHMEM_LANES=2" --rounds 2`
   Combine two experiments per paste with chain.sh. Run `--aa` once if the first difference is small, to see the noise.
3. Adopt a B arm only if its interval excludes 0 and digits are identical. Collect adopted settings into one `--env "..."`
   string for all later stages. Stop S4 at its budget even with ideas left (list them as open questions).
Go on if: a path chosen, digits identical in all arms, and the 8-node wall within 1.5 x the model
(ADJUST.md gate 3; model L1 22.6 s). Wall growing a lot above the model: ADJUST.md section 3 rows 9-10 (placement; `--exclude`).

## S5 scale-up (node screen, 64 n, 576 n at 8e9 digits per node)

```
bash remote/run/chain.sh "s5_run.sh --nodes 64 --digits 512e9 --path <P> --screen --env \"<adopted>\"" \
                         "s5_run.sh --nodes 576 --digits 4608e9 --path <P> --env \"<adopted>\" --write <dir>"
```
(drop `--write` when disk space is unknown; the write step costs ~1-2 s modelled, TB of space.) Check each summary:
`VERIFY OK` (hash when a reference exists), measured/model, slow hosts. Model growth per step (ADJUST.md section 1):
8 > 64 +14 %, 64 > 576 +24 % of wall; a step far above it = fabric, launch or placement trouble.
Decision rules: `slow_hosts` listed or spread across nodes large > the screen's list goes to `--exclude` on the next run
(rerun the step once, not more); `rc` 8/9 or `plan REFUSED` > the printed largest fitting digits, or `MN_T_CHUNK_MB=1024`;
rc 124 (timeout) > tail of the last phase (`show.sh latest tail 40`), check the registration setting in FACTS.md.
Go on if: 576 n s1 (4.608e12) VERIFY OK on the chosen path, no unexplained growth above 1.5 x the model (32 s without write).
If the 64-node step fails for placement reasons, try 8 > 576 directly once.

## S6 record attempts (staircase to the 3.82e13 headline)

Always run the planner first (s5_run does: `--fit` is its first step; on refusal it prints the largest digits that fit).

| step | digits | nodes | path | env | model (no write) |
|---|---|---|---|---|---|
| s1 | 4.608e12 | 576 | chosen (done in S5) | adopted | 32 s |
| s2 | 1e13 | 576 | 1 (path 2 refused by memory) | adopted + the S6 s2 env words | 52 s |
| s3 | 3.82e13 | 576 | 1 | adopted + the S6 s3 env words | 187 s |

The two env lines below are the ONLY place that gives these words; every other mention (table, commands, Minimal route, step 7d, FACTS.md) refers to them.
Q132 (user decision pending: `BS_MDEV_LOGL=28 DIST_CHUNKS=16` instead of 29 at s3) is a one-line edit of the "S6 s3 env" line; FACTS.md "Per-APU record edge" has the numbers.

S6 s2 env: `BS_MDEV_LOGL=29 XSHMEM_HANG_S=3600`
S6 s3 env: `BS_MDEV_LOGL=29 XSHMEM_HANG_S=3600 MN_NONPOW2_CAP=0`

Command forms (paste the words of the env line in place of `<S6 sN env>`):
```
bash remote/run/s5_run.sh --nodes 576 --digits 10e12 --path 1 --env "<adopted> <S6 s2 env>"
bash remote/run/s5_run.sh --nodes 576 --digits 38.2e12 --path 1 --env "<adopted> <S6 s3 env>"
```
Fallback when a slow or bad host appears at 576 nodes and the allocation has no spare nodes: `s5_run.sh` refuses to start with fewer than N usable nodes
(`only M usable nodes in the allocation, N asked`; after `--screen` or `--exclude` it fails with `fewer than N nodes remain`). Choices, in this order: (a) re-run the same step
with the largest node count the planner accepts on the usable nodes, digits scaled to it at the same per-node share (576 > 575 or 512: `--nodes 512 --digits <512/576 x D>`; the planner still prints the largest fitting
digits on a refusal; the exclusion list is applied automatically, `alloc.sh --status` shows the usable count); the result is a smaller headline, say so in the REPORT; (b) drop the slow host and re-plan:
`--exclude <host>` (comma list; a Slurm range like `nid[001-003]` is accepted) on the run plus `MN_GROUPS` for the new node count (FACTS.md, the `MN_GROUPS` row of the candidate table: the default schedule is tuned for 576 only), or
release and request more nodes (`alloc.sh --release`, then `alloc.sh --nodes <more> --exclude <host>`). Prefer (a) when time is short.

`BS_MDEV_LOGL` and `XSHMEM_HANG_S=3600` (code default 900 stays; user decision 2026-10-27, Q130) on every run from 1e13 digits (BS_MDEV_LOGL: user decision 2026-10-27; at 29): smaller device-0 arena levels, so the 4 APUs of a node
peak equally (aac7 1e11 on 1 node, m: APU 0 109.9 -> 90.5 GB, all four 90.5, digits identical, no slower; planner per APU at 2e13 on
576 nodes 89.7 -> 74.7 GB, mod; no change at 3.82e13).
(or both in one chain paste once s1 passed and time is short; chain stops at FAIL, so s3 is skipped when s2 fails).
Source of the numbers: `target/RESTART.md` section 8, `target/ladder/levels/L3.conf` (device 358.96 GB at s3 with cap 0, mod).
Without cap 0 the planner refuses s3 (407 GB vs 373.44). A step that fails is not repeated blindly: ADJUST.md section 3
rows 1-2, 7, then at most one re-run (another node group via `--exclude`, or a smaller `MN_T_CHUNK_MB`). If s3 will
not fit or fails twice, take the largest planner-approved digit count (about 2.67e13 fits without cap 0) as the record.
No `--write` at s2/s3 unless the file system has room for 17 TB (user decides; the stripe step, part files and time budget are in "Record-size output" below). Spend leftover time on a verified repeat
of the best size with the best settings, not on new experiments.

## Record-size output (the 3.82e13 run with `--write`; also 2e13 and 3e13)

Sizes (packed v2 = 0.41667 B per digit + a 4096 B header per part; m from the format, totals mod): 2e13 = 8.3 TB, 3e13 = 12.5 TB, 3.82e13 = 15.9 TB.
Path 1 (layout D, one process per node) writes 576 part files `<out>.part<k>` of 27.6 GB each (k = size-1-rank); path 2 / layout P writes 2304 parts of
6.9 GB. Each part has one writer thread per process: O_DIRECT, 256 MB chunks (`MN_OUT_CHUNK_MB`) cut over `MN_OUT_THREADS` (8) `pwrite` threads. The part
starts at the division's hook (`MN_OUT_EARLY=1`, default), so most of the write hides behind the low product; the exposed part is the `dc` wait and
`s after the checks` in the `wrote <file>.partNNNN` line. The target has 1.7 PB free, no quota (FACTS Target site); 16 TB needs the user's go (S6).

Striping (the `lfs setstripe` step; the target's default directory layout is PFL with 4 components, the first extent 0-1 GiB = ONE OST):
- Every write measured so far (0.42 GB at 1 node, 1.04 GB parts at 2 nodes: 3.4-5.3 GB/s) fits inside the first 1 GiB extent, so they are single-OST rates;
  how the later PFL components (component count and widths unknown) behave for a 27.6 GB file, and the aggregate of 576 files, are unmeasured.
- Do not set a layout blind. First measure (io03 below). Candidates: `default` (PFL, no setstripe), `-c 4 -S 32M`, `-c 16`, `-c -1`. Reasoning for `-c 4 -S 32M`: a file of 27.6 GB on one OST
  makes the slowest of the busiest OSTs set the end of the run (576 files hash onto the OSTs unevenly: the run waits for the most loaded one); 4 OSTs per file
  average that out with only 2304 stripe objects, while `-c -1` makes every file touch every OST (576 x all objects, lock traffic); `-S 32M` equals one
  `pwrite` thread's slice of a 256 MB chunk (256 MB / 8), so each call goes to one OST.
- Rule for the record run: keep `default` unless io03 at 64 nodes shows an explicit arm at least 15 % faster in aggregate (then use that arm); the larger the better only up to the
  point where the aggregate stops growing. Where to apply: the output directory BEFORE the run, `lfs setstripe -c <C> -S <S>M <dir>` then `lfs getstripe -d <dir>` (the run's
  subdirectory and the part files inherit it); `--write <dir>` passes that directory. Alternative that needs no directory step: `--env "MN_OUT_STRIPE=<C>:<S>:<OSTs>"`
  (each part created with that layout; `<OSTs>` = the OST count from `lfs df <dir> | grep -c OST` makes the first OST `(part x C) mod OSTs`, so the parts start on distinct OSTs);
  it needs `lfs` on the compute nodes' PATH (`command -v lfs` in one srun; a failed call prints `mn_out: ... FAILED: the file takes its directory's layout`).
- Keep the output directory on the one file system used by io01-io03 (home's file system; the other two are not user-writable or unused, FACTS Target site).

Time budget for the write (modelled = bytes / aggregate rate; the 5 GB/s per part is measured at 1-2 nodes only):

| aggregate write rate | 576 files, 15.9 TB | per node (27.6 GB) | basis |
|---|---|---|---|
| 2.9 TB/s (5 GB/s per node, nothing shared) | 5.5 s | 5.5 s | upper bound of the measured single-stream rate (mod) |
| 1 TB/s | 16 s | 27.6 s per node at 1.7 GB/s (!) | the file system's claimed cumulative rate into one file (u) |
| 0.58 TB/s (1 GB/s per node) | 27 s | 27.6 s | the earlier model's assumption (a) |
| 0.25 TB/s | 64 s | 64 s | pessimistic (a) |
| 0.1 TB/s | 159 s | 159 s | bad: another OST-limited layout (a) |

Read it as: write time about 6-60 s (mod), exposed time smaller because of `MN_OUT_EARLY`; the s3 run's model without write is 187 s, so plan 190-250 s with the
write and say so before the run. Warning limit: if io02 at 64 nodes gives less than 0.5 GB/s per node (a write over 55 s), report to the user before the 3.82e13 attempt
(decision: write with a longer budget, or attempt without `--write`). Digit hash: `s5_run --write` hashes the parts through ONE node after the run; at 16 TB that would
read for hours (read rate 0.8 GB/s on aac7's old file system, m), so the script skips the hash above `TGT_HASH_MAX_GB` (default 300) and the run's own VERIFY (T1/T2 windows, written by the writer) is the check;
an optional read-back of one part per node (`ECALC_RECHECK`) costs about 35 s per node at 0.8 GB/s (mod; io04 gives the real rate).

Which bench results change the record-run settings (benches in "Next target session" step 6a, all on the same file system):
- io01 (1 node, `tools/wbench` (built from `tools/wbench.c` by the io bench items on first use: `make wbench` in `tools/`) direct 8 vs 32 threads, buffered 8): 32 threads at least 1.3 x of 8 -> `MN_OUT_THREADS=32` in the record run; else keep 8.
- io02 (64 nodes, per-node files vs one shared name): per-node aggregate x 9 (576/64) = the modelled 576-node rate for the table above, if it keeps scaling (check the per-node
  rate fell by less than 30 % from io01 to io02, else the file system saturates: take the io02 aggregate itself as the 576-node rate). The shared-file figure is information only
  (ecalc writes per-node parts; a shared-file writer would be new code).
- io03 (64 nodes, `IO03_ARMS="default 1 4 16 -1 4:32M"`, 16 GB per rank): the stripe rule above.
- io04 (read-back, 1 node): the time of `ECALC_RECHECK` / `unpack_digits` over a part (27.6 GB per node) for the after-run check.

## S7 report

Stop when time is below the next step's budget. Run `bash remote/run/show.sh latest files` only if the user wants the
run dirs listed, then write the REPORT block (AGENT.md). Remind the user: the report goes into a local Claude Code session.

## Minimal route (time short, or the start went badly)

1. S0+S1 paste. 2. `chain.sh "s2_build.sh --path 1" "s3_gate.sh --nodes 1,2 --path 1 --digits 1e9"`.
3. `s5_run.sh --nodes 576 --digits 4608e9 --path 1` (s1; planner decides), then, if the wall is within 1.5 x model,
`s5_run.sh --nodes 576 --digits 38.2e12 --path 1 --env "<S6 s3 env>"` (or the largest the planner approves; the 1e13 step is skipped here, so the s3 words are the only ones needed).
4. REPORT. About 5-6 round trips, 60-90 min plus queue. Skipped: S4 tuning, the 64-node step, path 2.

## Next target session (after the 2026-10-26 runs; settings words are plain env words; arms run one after the other inside the hold)

How this relates to S0-S7: these steps are the optional experiments between S3 and S6; they do not replace the stages. Order: S0 + S1, S2 + S3 (this section's step 1 is the S2 + S3 gate with the extra 1e10 case, so do it instead of the plain S3 paste), then
the steps below in their order, but each only while the remaining session time still leaves the S6 budget (60 min) plus the next step's own time plus 20 %; S4 and S5 (30 + 40 min) are skipped or cut to what the steps here already measured. Steps 2-5b are the ones to take first (about 10-20 min each);
6a only if the record run will write; 6b and 7 only if the user asks for them (7 = failure hunt, long runs). Before S6 starts, stop and say what remains. Time short: Minimal route.

Facts behind it: `FACTS.md` "Measured runs on the target". New switches are off by default unless stated: `ECALC_OMP_TEAM=mask` (team sizes from the
affinity mask), `ECALC_RESIDUE=shoup` (digit residue), `XSHMEM_COLL=tree` (collectives), `ECALC_INIT_TL=1` (init timeline lines),
`MN_WAIT_STATS=1`, `NEWTON_MN_RECIP_TS=1`, `COMM_HANG_DETAIL=1`, `MEM_REPORT_DEVS=1`; `P24_CHECK=1|2` (operand limb checks; 2 also switches the P24 transform cache off), `COMM_SEQ_CHECK=1` (collective call-sequence check), `XSHMEM_HANG_S=<s>` (handshake / exchange HANG timeout, 900), `XSHMEM_HS_BATCH=k` (paced, rotated flat handshake), `XSHMEM_SPARSE=1` (exchanges skip zero-byte peers); `BS_MDEV_LOGL=29` on runs from 1e13 digits (S6); `mnrun.sh` now gives every task `srun -c <CPUs per task>` by default
(`MNRUN_CPUS_PER_TASK=` empty restores the old line). Settings a script cannot take as an option are written as `--a/--b/--env` words.
The multi-node changes shipped off by default, so the FIRST step is the gate at 1 AND 2 nodes (user decision); stop if it fails.

1. Gate (1 paste): `bash remote/run/chain.sh "s2_build.sh --path both --jobs 32" "s3_gate.sh --nodes 1,2 --path both --digits 1e9" "s3_gate.sh --nodes 2 --path both --digits 1e10"`
   (hashes bf9f4b08758b40d9 and 3fc025473586ed22; allocation as in S0+S1 with 64 nodes at least, ask for 576 for step 5).
2. Threads and residue, 2 nodes, 8e9 per node (1 paste, expected dc 14 s -> under 1 s, total about 39 s):
   `bash remote/run/s4_ab.sh --nodes 2 --digits 16e9 --path 1 --a "BS_SEED_THREADS=144" --b "ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup ECALC_INIT_TL=1" --rounds 2`
   Check in the B log: the rank-0 team line (team sizes and affinity count: 192 expected), the `tl` init lines; the self-test seconds: keep the
   self-test while it stays under 15 s (note the number in the report); digits identical in both arms. Optional, old line for comparison (3 min):
   `MNRUN_CPUS_PER_TASK= bash remote/run/s5_run.sh --nodes 2 --digits 16e9 --path 1` (expect the old 182.7 s).
3. Round size, 64 nodes, 8e9 per node, three arms (the planner check of each arm runs first; `XSHMEM_ROUND_MB=0` stages the whole exchange, the planner may refuse it):
   `bash remote/run/chain.sh "s5_run.sh --nodes 64 --digits 512e9 --path 1 --env \"BS_SEED_THREADS=144 ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1 XSHMEM_ROUND_MB=128\"" "s5_run.sh --nodes 64 --digits 512e9 --path 1 --env \"BS_SEED_THREADS=144 ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1 XSHMEM_ROUND_MB=1024\"" "s5_run.sh --nodes 64 --digits 512e9 --path 1 --env \"BS_SEED_THREADS=144 ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1 XSHMEM_ROUND_MB=0\""`
   The three round sizes at once (allocation of >= 192 nodes): `bash remote/run/arms.sh --nodes 64 --digits 512e9 --path 1 --arm "r128:BS_SEED_THREADS=144 ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1 XSHMEM_ROUND_MB=128" --arm "r1024:BS_SEED_THREADS=144 ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1 XSHMEM_ROUND_MB=1024" --arm "r0:BS_SEED_THREADS=144 ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1 XSHMEM_ROUND_MB=0"`
   Model prediction (`results/TIME_MODEL.md` §target_fit): small at 64 nodes (bs 21.5 / 20.8, dm 26.8 / 25.7 s);
   the staging costs 7.5 GB (1024) and 38 GB (0) more host memory per node at 3.82e13 (`results/MEMORY.md`). A/B with intervals when time allows: `s4_ab.sh --nodes 64 --digits 512e9 --path 1 --a "XSHMEM_ROUND_MB=128" --b "XSHMEM_ROUND_MB=1024"`.
4. Collectives and layout, 64 nodes: `s4_ab.sh --nodes 64 --digits 512e9 --path 1 --a "" --b "XSHMEM_COLL=tree"`, then layout D vs P:
   `bash remote/run/chain.sh "s5_run.sh --nodes 64 --digits 512e9 --path 1 --env \"<best>\"" "s5_run.sh --nodes 64 --digits 512e9 --path 2 --env \"<best>\""` (path 2 = layout P with `ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`, added by the script; its planner may refuse at 64 nodes: record it; layout P never ran on the target).
   (`arms.sh` takes one path per call; for D and P side by side run two calls from two shells of the same allocation, or keep the chain.)
4b. Shifts, 64 nodes: `XSHMEM_SPARSE=1` skips peers with zero bytes in every exchange. `mdb_shift_gt` already passes exact zero counts, so a shift's cost was the transport handshaking all g - 1 peers (124 shifts x ~122 ms at 576 nodes, m); sparse leaves about 2 neighbours per shift:
   `bash remote/run/s4_ab.sh --nodes 64 --digits 512e9 --path 1 --a "MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1" --b "MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1 XSHMEM_SPARSE=1" --rounds 2`. Digits identical in both arms (stop otherwise). Read the `shifts n/s` field of the `recip(mn)` line (and of the `divmod(mn ...)` line):
   Both arms side by side instead (one allocation of >= 128 nodes, about 3 min instead of two sequential runs; a screening look, not the ABBA pairing; confirm a win with the s4_ab line above):
   `bash remote/run/arms.sh --nodes 64 --digits 512e9 --path 1 --arm "dense:MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1" --arm "sparse:MN_WAIT_STATS=1 NEWTON_MN_RECIP_TS=1 XSHMEM_SPARSE=1"` (one table; the `shifts` figure of each arm is in `raw/arm_<name>.out`).
   n = shift count, s = their seconds; expect n unchanged and s far lower with sparse. Adopt with an interval that excludes 0 on the total. Goes after step 4 (same 64-node allocation), before 576.
5. 576 nodes, 4.608e12, best settings of 2-4b, TWO runs in one allocation (first = cold, second = warm; compare init: the 111 s anomaly):
   `bash remote/run/chain.sh "s5_run.sh --nodes 576 --digits 4608e9 --path 1 --env \"<best> ECALC_INIT_TL=1 COMM_HANG_DETAIL=1 MEM_REPORT_DEVS=1\"" "s5_run.sh --nodes 576 --digits 4608e9 --path 1 --env \"<best> ECALC_INIT_TL=1 COMM_HANG_DETAIL=1 MEM_REPORT_DEVS=1\""`
   Report both inits and the `tl` lines of the slowest node; the level lines `mn: node R level L ... in X s` (Fit A of the analysis predicts 2.1, 2.9, 3.3, 3.7, 4.1, 4.8 s for levels 1-6; compare).
5b. 576-node init-only run, the full startup timeline (right after step 5, same allocation, about 3 min; the anomaly of step 5): `bash remote/run/s5_run.sh --nodes 576 --digits 4608e9 --path 1 --env "<best> ECALC_INIT_ONLY=1 ECALC_INIT_TL=2 COMM_HANG_DETAIL=1"`.
   `ECALC_INIT_ONLY` ends the run after the pools and arena are mapped, so no VERIFY line is expected (the summary may say PARTIAL: read the log with `show.sh latest tail 80`). `ECALC_INIT_TL=1` prints, on rank 0, `process age at main` (process start to `main`, from `/proc/self/stat`), an `init = plan/fit + mn_init + rns_init + mn_selftest + region pools + other` split that sums to the init figure, and an ofi split
   (PMI init and topology, endpoints incl. the first HIP call and kernel, PMI address exchange, `fi_av_insert`); `=2` prints on every node leader, and a process whose age at main exceeds 30 s prints `LATE START` (names the slow nodes). The lines that cover the 111 s are the point (the engine's own lines covered only part of it).
   Compare the three inits (cold run of step 5, warm run, init-only): a large process age at main or `LATE START` = shared-image start (library loads by 2304 processes); a warm init far below the cold one the same; equal inits and a large PMI / address-exchange / `fi_av_insert` term = the engine's steps.
6a. IO benches (before any run that writes; skip when the record run will not write; each at the sizes of "Record-size output"; needs the login-node builds as bench items first: `bash target/bench/run.sh --jobid none --only bld01,bld02`; `<J>` = the allocation's job id from `alloc.sh --status`):
   `IO02_SZ=16 IO03_SZ=16 IO03_NODES=64 IO03_ARMS="default 1 4 16 -1 4:32M" ASSUME=cor01 bash target/bench/run.sh --jobid <J> --only io01,io02,io03,io04 --worker w1`
   (about 30 min worst case; io01 1 node, io02 64 nodes, io03 64 nodes, io04 1 node; results in `target/results/bench/<id>/summary.txt`, the `q.TQ54` ... `q.TQ57` lines). The result table changes the record-run settings: see "Record-size output".
6b. Fabric and memory benches (when time remains; one worker per allocation, `target/RESTART.md` §7; `ASSUME=cor01,scl01 bash target/bench/run.sh --jobid <J> --only mem01,net02,net04,net05 --worker w2`): `net02` (put rate, latency, 1 vs 2 lanes), `net04` / `net05` (all-to-all scaling),
   `mem01` (VMM edge per APU: is the 93.36 GB hipMalloc edge the real limit? aac7 ran 109.9 GB on APU 0).
7. Failures, only after 1-5b (every run here with `BS_MDEV_LOGL=29`, as in S6; each paste below is its own long step, not one chain; `--tmo` is the step's wall limit in minutes):
   a. 2e13 on 576 nodes, flat handshake with a long timeout. Code reading (results/XSHMEM.md T2) says: most likely a straggler past the fixed 900 s (both failing PEs were in node range 512-575, the last level-6/7 group), then a call-order divergence, then a flat burst of signals. One run separates the first two:
      `bash remote/run/s5_run.sh --nodes 576 --digits 20e12 --path 1 --tmo 80 --env "<best> BS_MDEV_LOGL=29 P24_CHECK=1 XSHMEM_HANG_S=3600 COMM_SEQ_CHECK=1 COMM_HANG_DETAIL=1"`
      Outcomes: it completes (VERIFY OK; look for HANG-free waits well over 900 s in the level lines) = imbalance, the fixed 900 s was the defect: use `XSHMEM_HANG_S=3600` on every larger run and stop the hang hunt. A `COMM_SEQ_CHECK` fatal = divergence: the two call sites it names are the result, take them to the code lane.
      A HANG after 3600 s (`COMM_HANG_DETAIL` names the members that never arrived) = a real hang: run b. A `P24_CHECK` fatal = the CRT-bound defect (product number and kind, piece, node, PE, sizes, first bad position; "received" = bad exchange data, "CRT" = the bound); the note "non-normalised share" on stderr is only a note.
   b. Only after a real hang in a: the burst question, 2 runs, each `--tmo 40` with `XSHMEM_HANG_S=1800` and the same checks: `XSHMEM_COLL=tree` (12 signals per member), and the flat handshake paced with `XSHMEM_HS_BATCH=64`. Tree passes and paced passes = the burst (keep tree for larger runs); both hang like a = not the handshake: report the `COMM_HANG_DETAIL` lines.
      The two runs are 576-node runs: side by side they need an allocation of 1152 nodes, so they stay sequential; `arms.sh` is for arms of <= half the allocation (`--nodes 64` ... `--nodes 288` in a 576-node allocation: two 288-node arms).
   c. 3e13 on 576 nodes (with the settings that passed in a / b): `bash remote/run/s5_run.sh --nodes 576 --digits 30e12 --path 1 --tmo 40 --env "<best> BS_MDEV_LOGL=29 MN_NONPOW2_CAP=0 P24_CHECK=1 XSHMEM_HANG_S=3600 ECALC_FIT_PER_APU=1"`; a planner refusal means the per-APU edge is the cause (the per-APU line says by how much).
      3e13 is above 2.592e13, where `MN_NONPOW2_CAP` auto turns on (the planner refuses 3e13 with it: 385.93 GB > 373.44, so the failed run most likely used `MN_NONPOW2_CAP=0`, FACTS failure table, modelled) and above 4.95e12 (plane cap 2^31). The all-shift CRT flag is now the default (`P24_CRT_ANY=1`), so any flag names the product kind, piece and group in the line: compare the node with the mixed-piece groups (3e13 levels 1-6: 4 pieces from node 158/160 on, 2 below; level 7 groups [192,384) at 10 pieces; FACTS). A cheaper target check (planner, aac7 2026-10-27, target limits, m): the mixed shape (some level-1/2 groups at 4 pieces, others at 2) appears at a per-node share of 2.9e13/576 to 3.6e13/576 digits at every node count tried, and not at 2e13, 2.2e13-2.8e13 or 3.82e13 (576 n). On 64 nodes `--digits 3.333e12` (3e13's per-node share) gives 23 of 32 level-1 groups at 4 pieces (first [18, 20)), fits with `MN_NONPOW2_CAP` auto (device 352.36 GB, 88.09 GB per APU); 128 n `6.667e12` and 256 n `13.33e12` likewise. So run 64 nodes at 3.333e12 with `P24_CHECK=1` before c: a flag there reproduces the failure at one ninth of the nodes; a clean pass points at what only 576 nodes add (level 7-8 groups, non-power-of-two groups): `bash remote/run/s5_run.sh --nodes 64 --digits 3.333e12 --path 1 --tmo 20 --env "<best> BS_MDEV_LOGL=29 P24_CHECK=1 XSHMEM_HANG_S=3600"`. `P24_CHECK=1` checks only operands that miss the transform cache; after a flag-free run that still fails the digits, repeat with `P24_CHECK=2`.
   d. then the 3.82e13 attempt (S6 s3: `<adopted>` + the S6 s3 env words) with the best round size, `--write` only after 6a and the user's go (see "Record-size output").

Script features (A6): `arms.sh --nodes N --digits D --path P --arm "NAME:K=V ..." --arm ...` runs K arms of N nodes at once in one allocation (each arm is an `s5_run.sh --hosts <its block>`; one table: arm, nodes, rc, total_s, init_s, verify, last digits, settings; `--cmd` = per-arm shell command for allocations without NICs; `--dry-run` prints the plan). Needs N x arms usable nodes; concurrent arms share the fabric, so their times are for screening (confirm with `s4_ab.sh`). `s5_run.sh` and `s4_ab.sh` print, per run, `layout`, `init_split`, `shifts`, `p24_check` and `device_peak_apu_gb` lines (NA when ecalc did not print them; `s4_ab.sh` per arm as means).
