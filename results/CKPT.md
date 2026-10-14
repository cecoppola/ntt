# CKPT — the recheck could not read the top-level set of the 6-node 3.8646 × 10¹¹ run

**Question.** After the 6-node 3.8646 × 10¹¹ run with `ECALC_CKPT_TOP=1` (VERIFY OK, first 10¹¹ digits identical),
`ECALC_RECHECK=1` on its files printed `bs: checkpoint: short read from <out>.top/n00X_tree_002.rK` (16 of the 24 files,
all 6 nodes, tree level 2), fell back to the sidecar's P, Q residues on node 1, and was killed at its 1800 s timeout. Is
the set written incompletely, read with the wrong layout, or is it the file system?

**Change / branch.** Branch `ckpt` (from `main` at 39346d0): (1) the failed read is named (errno, offset, bytes got /
needed, file size); (2) `ecalc/spill.c` `pread_need`: (in progress)

**Method.** (in progress)

**Results.** (in progress)

**Verdict.** (in progress)

**Open issues.** (in progress)

**RESUME.** Branch `ckpt`, worktree `../ntt-wt-ckpt`. Queue item `40_ckpt_repro.sh` (2 nodes: write with
`ECALC_CKPT_TOP=1` at 10¹⁰, then 2 × 10¹⁰ if not reproduced; file sizes vs headers; recheck with the diagnostic build,
then with the fix build on a short read). Next: collect its summary, then the gate item (1 and 2 nodes + write/recheck).
