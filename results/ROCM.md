# ROCM - ROCm 7.14.0 against 7.2.4 at 1 node (OPPORTUNITIES M7d)
**Question.** The target runs ROCm 7.14.0. Does ecalc build and run correctly with it, and is it faster or slower than the aac7 default 7.2.4?
**Change / branch.** None (docs only). Commit 39346d03 of `main`, built twice with `SHMEM_CRAY=1`, once under each ROCm module.
**Method.** aac7, 1 node, 1e10 digits, default settings (aac7 profile launch line), digits written and compared with the prefix of the
GMP reference; `VERIFY` read from each log; the log's HIP runtime path checked per run. ABBA-style, 4 rounds (B A, A B, B A, A B), A = rocm 7.2.4,
B = rocm 7.14.0. Unload every ROCm module before loading the wanted one (see Open issues).
**Results.** All 8 runs: VERIFY OK, digits identical, no segfault (measured).

| round | A 7.2.4 (s) | B 7.14.0 (s) | B - A (s) |
|---|---|---|---|
| 1 | 35.68 | 60.65 (init 33.8 s, cold first launch) | +24.97 |
| 2 | 38.49 | 34.40 | -4.09 |
| 3 | 38.28 | 35.24 | -3.04 |
| 4 | 36.14 | 37.25 | +1.11 |

- All pairs: B - A = +4.74 s, 95 % CI [-17.02, +26.49] (t, n = 4 pairs) (measured).
- Without round 1 (post hoc): B - A = -2.01 s, 95 % CI [-8.62, +4.59] (t, n = 3 pairs) (measured). Stage split (bs, recip, dm, init) equal in the two arms.
**Verdict.** 7.14.0 builds and gives identical digits; no speed difference is detectable at this size (CIs contain 0). Keep 7.2.4 as the aac7 default; no change.
**Open issues.**
- Cold first launch: the first 7.14.0 run took 60.65 s (`init` 33.8 s against about 9 s later). In an earlier attempt the first 7.14.0 run did not
  finish the reciprocal stage within a 240 s timeout (killed). Cause not established (assumed first-use code-object build); give the first target run a long timeout.
- Not measured: 1e11 digits, multi-node, 7.14.0 with the SHMEM-only path.
- Method trap: if another ROCm module is already loaded, `module load rocm/7.14.0` silently fails (conflict) and the run uses the old runtime;
  set `MNRUN_UNLOAD` to all ROCm modules and check the libamdhip64 line in the log.
- A 7.14.0 binary needs the 7.14.0 module at run time (its RPATH names only the llvm library directory).
**RESUME.** Done. Next, if wanted: one 7.14.0 gate at 2 nodes and a 1e11 ABBA; raw logs stay on aac7.
