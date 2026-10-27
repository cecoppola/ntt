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
says PENDING) re-run `alloc.sh` with `--nodes 64`, later with 8 (subsets are chosen inside the allocation; scale-up then
uses what was granted). Add `--partition/--account` after the first error says which are needed (s0 lists partitions).
Go on if: s0 PASS (manifest clean, python3, compiler, srun/salloc, ROCm/SMA/libfabric modules found) and s1 PASS (4 GPUs
and 8 NICs per node, SHMEM test rc 0, `site.env` written). s1 PARTIAL: read what is missing (FACTS known problems).
s0 FAIL on changed files: have the user unpack again. Module not found: ask for `module avail <name>` only.

## S2 + S3 (one paste, build then gates)

```
bash remote/run/chain.sh "s2_build.sh --path both --jobs 32" "s3_gate.sh --nodes 1,2 --path both --digits 1e9"
```
Go on if: build PASS for both paths (`-DXSHMEM_OFI` and `-DXSHMEM_CRAY` in the flags seen) and every gate `VERIFY OK` with
hash bf9f4b08758b40d9 at 1e9. A path that fails to build or gate is dropped (write it in PROBLEMS) - one working path is
enough to continue. Both fail: failure handling (AGENT.md); no digits-producing patch without a gate re-run.
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
| s2 | 1e13 | 576 | 1 (path 2 refused by memory) | adopted | 52 s |
| s3 | 3.82e13 | 576 | 1 | adopted + `MN_NONPOW2_CAP=0` | 187 s |

```
bash remote/run/s5_run.sh --nodes 576 --digits 10e12 --path 1 --env "<adopted>"
bash remote/run/s5_run.sh --nodes 576 --digits 38.2e12 --path 1 --env "<adopted> MN_NONPOW2_CAP=0"
```
(or both in one chain paste once s1 passed and time is short; chain stops at FAIL, so s3 is skipped when s2 fails).
Source of the numbers: `target/RESTART.md` section 8, `target/ladder/levels/L3.conf` (device 358.96 GB at s3 with cap 0, mod).
Without cap 0 the planner refuses s3 (407 GB vs 373.44). A step that fails is not repeated blindly: ADJUST.md section 3
rows 1-2, 7, then at most one re-run (another node group via `--exclude`, or a smaller `MN_T_CHUNK_MB`). If s3 will
not fit or fails twice, take the largest planner-approved digit count (about 2.67e13 fits without cap 0) as the record.
No `--write` at s2/s3 unless the file system has room for 17 TB (user decides). Spend leftover time on a verified repeat
of the best size with the best settings, not on new experiments.

## S7 report

Stop when time is below the next step's budget. Run `bash remote/run/show.sh latest files` only if the user wants the
run dirs listed, then write the REPORT block (AGENT.md). Remind the user: the report goes into a local Claude Code session.

## Minimal route (time short, or the start went badly)

1. S0+S1 paste. 2. `chain.sh "s2_build.sh --path 1" "s3_gate.sh --nodes 1,2 --path 1 --digits 1e9"`.
3. `s5_run.sh --nodes 576 --digits 4608e9 --path 1` (s1; planner decides), then, if the wall is within 1.5 x model,
`s5_run.sh --nodes 576 --digits 38.2e12 --path 1 --env "MN_NONPOW2_CAP=0"` (or the largest the planner approves).
4. REPORT. About 5-6 round trips, 60-90 min plus queue. Skipped: S4 tuning, the 64-node step, path 2.
