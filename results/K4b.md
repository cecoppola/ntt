# K4b — `k_crt_batch`: the per-chunk carry chain as a parallel carry-lookahead

**Question.** Does resolving the CRT kernel's per-chunk carry chain in parallel (instead of a 256-step serial loop run by
one thread) shorten the 1-node run at 6.441e10 digits? Baseline: `k_crt_batch` takes 4.5–4.8 s per APU at 1 node (m,
`archive:results/O7b.md` §D, `results/S39.md` row 3); K4 (B-form occupancy, `results/S51.md`) gave nothing.

**Change / branch.** Branch `k4b`. Switch `RNS_CRT_SCAN` (0 = the serial chain, default; 1 = parallel). Files:
`ecalc/rns_mul.c` (`k_crt_batch`, `rns_gconst`), `ecalc/rns_int.h` (`struct gconst` field `scan`), new unit test
`ecalc/tests/t_crtk.c` (in `TESTS`), registry rows. No call site changes: the flag rides in `struct gconst`, which every
launch already passes by value.

How it works (`RNS_CRT_SCAN=1`). Per chunk of 256 coefficients, thread j forms w_j = V_j + CY_{j−1} (V_j < B the chunk
sum's low digit, CY ≤ 4 its quotient, CY_{−1} the chunk's carry-in ≤ 5), so w_j ≤ B + 4. The serial chain's overflow is
o_j = gen_j | (prop_j & o_{j−1}) with gen = w ≥ B, prop = w = B − 1. Two wave ballots give the gen / prop masks of each
64-thread wave; every thread then adds A + G over 256 bits (A = gen | prop, G = gen; four words) and reads its carry-in as
bit j of (A + G) ^ A ^ G. The next chunk's carry-in is CY_{n−1} + o_{n−1}. The tails and the spill are unchanged.
Barriers per chunk: 3 instead of 5. The serial path is the reference: `t_crtk` compares every limb and spill word.

**Method.**
- Microbenchmark `tests/t_crtk 29` and `27` on one aac7 node (APU 0): part 1 compares scan 0 against scan 1 on random
  residues, carry ripples (every coefficient B − 1, generators 2B − 1 at about 1 in 997, an added operand of B − 1 limbs)
  and all residues p_i − 1, three and four primes, C form, ragged stripes, nc < S, a B-form batch with added operands;
  part 2 times the C-form launch of the 1-node run (q = 2^29 coefficients in 65536 stripes of 8192), median of 7.
- Gates (`tools/gate.sh --build --nodes 2`, then `--nodes 2`, `--nodes 1` twice; 10⁹ digits, aac7 launch line, P24 forced
  at 2 nodes) with the switch off and with `--env RNS_CRT_SCAN=1`; after the rebase on main, the same four gates again.
- Unit tests with `RNS_CRT_SCAN=0` and `=1`: `t_mul 20` (invoked as `mnaccept.sh`'s unit step), `t_strategy 24 2 V`.
- Timing: paired ABBA at 1 node, 6.441e10 digits, aac7 launch line (`tools/profile_load.py aac7 --line`) through
  `mnrun.sh 1`, A = `RNS_CRT_SCAN=0`, B = `RNS_CRT_SCAN=1`, same binary (branch commit e9505b97), same node per round;
  stop rule p < 0.05 / CI half-width ≤ 1.5 s / 4 rounds (driver adapted from `archive:drivers/s51/s51_abba.sh`, kept as
  `archive:drivers/k4b/`). The wall is ecalc's own `total` line.

**Results.**

| check | result |
|---|---|
| `t_crtk 29`, `t_crtk 27` equivalence (48 cases, 3 and 4 primes) | all identical, VERIFY OK (m) |
| C form 2^29 / 65536, three primes (the 1-node case) | scan 0 30.18 ms → scan 1 13.65 ms median, 2.21× (569 → 1259 GB/s) (m) |
| C form 2^29 / 65536, four primes | 49.85 → 34.11 ms, 1.46× (m) |
| C form 2^27 / 16384, three / four primes | 8.02 → 3.95 ms (2.03×) / 12.54 → 8.56 ms (1.47×) (m) |
| gates before the rebase: 2 n off / on, 1 n off / on | PASS ×4: VERIFY OK, digits identical, segv 0 (ecalc total 18.67 / 17.54 s at 2 n, 12.39 / 12.15 s at 1 n; single runs, not timings) (m) |
| gates after the rebase on main 89318ae (branch 0b3c36e), 2 n off / on, 1 n off / on; `t_crtk` | PASS ×4: VERIFY OK, digits identical, segv 0; `t_crtk` VERIFY OK (48 checks) (m) |
| gates after the second rebase on main f232085 (DEF: P24 for every multi-node product; branch e385355), same four + `t_crtk` | PASS ×4, digits identical, segv 0 (2 n: P24 in use); `t_crtk` VERIFY OK (m) |
| `t_mul 20`, scan 0 / scan 1 | VERIFY OK (169 checks) both, 727 / 731 s (m) |
| `t_strategy 24 2 V`, scan 0 / scan 1 | VERIFY OK (12 checks) both (m) |
| **ABBA 1 n, 6.441e10, 4 rounds (16 runs)** | **B − A = −2.52 s, 95 % CI [−4.19, −0.85]** (per-round deltas −3.6, −1.4, −3.2, −1.9 s; p < 0.05), all 16 runs VERIFY OK; arm means A 71.47 s, B 68.95 s (−3.5 %) (m) |

Modelled saving at 1 node: the kernel's 4.5–4.8 s per APU (m, S39/O7b) at the three-prime ratio 2.21 is 2.4–2.6 s per
APU of kernel time (modelled; the wall gain depends on how much of the kernel is on the critical path).

**Verdict.** Adopt candidate: −2.52 s [−4.19, −0.85] at 1 node (m), above the 2.0 s assumed in OPPORTUNITIES and in
line with the modelled 2.4–2.6 s of kernel time. Digits identical in every gate and unit test. Merged with the switch off;
the user decides adoption (a default flip is one line in `rns_gconst`).

**Open issues.** The four-prime C form gains less (1.46× against 2.21×, m): it is bound by `ec_words_to_dec4`'s sixteen
divisions, not by the chain; a cheaper four-word-to-decimal conversion is the next step there. Multi-node timing not
measured (the 2-node gates only show digits). `k_crt24` (`p24_crt.c`, the multi-node four-prime 24-digit CRT) has the same serial chain; not touched.

**RESUME.** Branch `k4b` merged to main with the switch off; worktrees removed. Raw logs: aac7 `~/k4b/{ubench,gates,gates2,gates3,tmul,abba}`
(`abba/pairs.tsv`, `abba/stats.txt`). Next (for the user): adopt `RNS_CRT_SCAN=1` as the default, then remove the serial path;
optionally the same carry-lookahead in `k_crt24` and a faster `ec_words_to_dec4`.
