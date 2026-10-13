# K4c — `k_crt24`: the P24 CRT's per-chunk carry chain as a parallel carry-lookahead

**Question.** Does K4b's carry-lookahead (`results/K4b.md`), applied to `k_crt24` (the P24 CRT, `ecalc/p24_crt.c`, the
default for every multi-node product since D18, `results/DEF.md`), shorten the 2-node run at 6.441e10 digits per node?

**Change / branch.** Branch `k4c`. Switch `P24_CRT_SCAN` (0 = the serial chain, default; 1 = parallel).

(in progress)

**RESUME.** Branch `k4c`; aac7 worktree `~/ntt-wt/k4c`; outputs `~/k4c/`.
