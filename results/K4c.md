# K4c — `k_crt24`: the P24 CRT's per-chunk carry chain as a parallel carry-lookahead

**Question.** Does K4b's carry-lookahead (`results/K4b.md`), applied to `k_crt24` (the P24 CRT, `ecalc/p24_crt.c`, the
form of every multi-node product since D18, `results/DEF.md`), shorten the 2-node run at 6.441e10 digits per node?

**Change / branch.** Branch `k4c`. New switch `P24_CRT_SCAN` (0 = the serial chain, default; 1 = parallel). Files:
`ecalc/p24_crt.c` (`k_crt24` and `p24_crt_launch`, which reads the switch per launch and passes it as a kernel argument),
new unit test `ecalc/tests/t_crt24k.c` (in `TESTS`), registry and review rows, `ALGORITHM.md` §4 (P24). No call-site
change. A new switch rather than `RNS_CRT_SCAN`: that one became the default by the user's decision D21 (on 1-node data,
`k_crt_batch` only) while this branch was open; reusing it would have switched `k_crt24` on by default without its own
measurement and adoption decision.

How it works (`P24_CRT_SCAN=1`). A chunk of 256 points covers up to 342 limbs; each limb sums the digits of at most three
points (< 3B), so its quotient CY ≤ 2, the chunk's carry-in ≤ 3, and w_i = V_i + CY_{i−1} ≤ B + 2. As in K4b the serial
overflow is o_i = gen_i | (prop_i & o_{i−1}), gen = w ≥ B, prop = w = B − 1. Each thread owns limbs t and t + 256; four wave
ballots give the masks of 512 limbs (8 words); every thread adds A + G (A = gen | prop, G = gen) over the chunk's words and
reads its carry-ins as bits of (A + G) ^ A ^ G; the next chunk's carry-in is CY_{nl−1} + o_{nl−1}. Same barrier count (5) as
the serial path; the 342-step one-thread loop is gone. The tails and the spill are unchanged.

**Method.**
- `tests/t_crt24k 30` on one aac7 node (APU 0): part 1 runs `k_crt24` with the switch 0 and 1 on the same planes and compares
  every out-run limb, every column spill and the device flag: coefficients random below 10^60, long carry ripples
  (10^24 − 1 everywhere, all limbs B − 1, with generators 2·10^24 − 1 at about 1 in 997), one generator at each column's first
  point (the ripple crosses every chunk into the spill), the bound 10^60 − 1 everywhere, all residues p_i − 1 (past the bound:
  flag raised), a mix; shapes rows 1, 3, 128, 333/334 (uneven ranks of R = 1000), 428/429 (R = 3000, 7 ranks), 512, 777, 2048,
  8192, every a mod 3. Part 2 times the kernel at 2^30 points per rank (rows 4096 / 8192 / 16384, 2-node rank shape
  rows = R / 8), median of 7.
- `tests/t_p24` (k_crt24 against the GMP host model) with `P24_CRT_SCAN=0` and `=1`.
- Gates `tools/gate.sh --build --nodes 2`, `--nodes 2`, `--nodes 1` ×2, switch off and `--env P24_CRT_SCAN=1` (10⁹ digits,
  aac7 launch line, P24 in use at 2 nodes); again after the rebase on main 4ec5ab5 (SIMP, ADOPT: `RNS_CRT_SCAN=1` default).
- Profiling round (one ABBA round with `RNS_VERBOSE=1`): node 0's sum of the `dist_mn` lines' `crt` times per run.
- Timing: paired ABBA at 2 nodes, 6.441e10 digits per node, aac7 launch line through `mnrun.sh 2`, A = `P24_CRT_SCAN=0`,
  B = `=1`, same binary (branch commit 5d54994), same nodes per round, no digit output; stop rule p < 0.05 / CI half-width
  ≤ 1.5 s / 4 rounds. Driver `archive:drivers/k4c/k4c_abba.sh` (from K4b's); queue items `archive:drivers/k4c/6*_k4c_*.sh`.
  The wall is ecalc's own `total` line; bs and dm are its phase fields.

**Results.**

| check | result |
|---|---|
| `t_crt24k` equivalence (279 checks: 9 shapes × 6 coefficient kinds × up to 3 ranks) | all identical, VERIFY OK (m) |
| `t_p24` against the GMP model, switch 0 / 1 | VERIFY OK (78594 checks) both (m) |
| kernel, 2^30 points per rank, rows 4096 / 8192 / 16384 | scan 0 157.96 / 158.39 / 158.76 ms → scan 1 101.95 / 101.95 / 101.26 ms median; 1.55–1.57× (290 → 450 GB/s) (m) |
| gates on 5d54994: 2 n off / on, 1 n off / on | PASS ×4: VERIFY OK, digits identical, segv 0, P24 in use at 2 n (m) |
| gates after the rebase on main 4ec5ab5 (branch ba34ba2): 2 n off / on, 1 n off / on; `t_crt24k 0`, `t_p24` with the switch on | PASS ×4: VERIFY OK, digits identical, segv 0, P24 in use at 2 n; `t_crt24k` VERIFY OK (276 checks), `t_p24` VERIFY OK (78594) (m) |
| profiling round (RNS_VERBOSE=1), node-0 `crt` sum per run | A 4.13 / 4.25 s → B 2.95 / 2.94 s: −1.24 s per run (m, 1 round); total in that round −2.5 s (m, one round, not a result) |
| **ABBA 2 n, 6.441e10/node, 4 rounds (16 runs)** | **total B − A = −1.89 s, 95 % CI [−7.51, +3.73]** (per-round −2.8, +2.0, −6.3, −0.5 s; not significant); arm means A 126.28 s, B 124.39 s; all 16 VERIFY OK (m) |
| phases, same 4 rounds | dm −0.55 s [−3.00, +1.90]; bs −0.43 s [−1.67, +0.82] (m) |

Modelled: node 0's P24 CRT time of 4.19 s per run (m) at the kernel ratio 1.55 (m) gives −1.49 s of kernel time per run
(modelled); the profiling round measured −1.24 s. The wall saving is at most that (the CRT is on the critical path of each
product), below the ABBA's resolution at 4 rounds: the A arm had two slow runs (131.0, 131.4 s) against a B range of
122.7–125.9 s.

**Verdict.** Needs more data for a wall claim; the kernel gain is clear. Kernel 1.55× and −1.24 s of CRT time per 2-node
run (m); wall −1.89 s [−7.51, +3.73] at 4 rounds (n.s.). Digits identical in every gate and test. Merged with the switch off;
the user decides adoption (on the kernel evidence, or after more rounds: at the observed per-round spread of 3.5 s about 40 rounds would be
needed to resolve −1.2 s, modelled).

**Open issues.** The four-prime conversion `ec_words_to_dec4` now dominates the kernel (K4d). The share of the CRT at
larger node counts is not measured (the per-rank plane shrinks with g while the per-node digit count is fixed, so the CRT time
per node stays about the same, modelled).

**RESUME.** Branch `k4c` merged to main with the switch off; worktrees removed. Raw logs: aac7 `~/k4c/{gates,gates2,gates3,prof,abba}`
(`abba/pairs.tsv`, `abba/s51stat.tsv` = bs, dm per run, `abba/stats.txt`). Next (for the user): adopt `P24_CRT_SCAN=1`
or ask for more rounds; K4d.
