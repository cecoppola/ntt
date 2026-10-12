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
- Gates, unit tests, ABBA: see Results (filled in as they complete).

**Results.**

| check | result |
|---|---|
| `t_crtk 29`, `t_crtk 27` equivalence (48 cases, 3 and 4 primes) | all identical, VERIFY OK (m) |
| C form 2^29 / 65536, three primes (the 1-node case) | scan 0 30.18 ms → scan 1 13.65 ms median, 2.21× (569 → 1259 GB/s) (m) |
| C form 2^29 / 65536, four primes | 49.85 → 34.11 ms, 1.46× (m) |
| C form 2^27 / 16384, three / four primes | 8.02 → 3.95 ms (2.03×) / 12.54 → 8.56 ms (1.47×) (m) |

Modelled saving at 1 node: the kernel's 4.5–4.8 s per APU (m, S39/O7b) at the three-prime ratio 2.21 is 2.4–2.6 s per
APU of kernel time (modelled; the wall gain depends on how much of the kernel is on the critical path).

**Verdict.** Pending the gates and the ABBA.

**Open issues.** `k_crt24` (`p24_crt.c`, the multi-node four-prime 24-digit CRT) has the same serial chain; not touched.

**RESUME.** Branch `k4b` (local worktree `../ntt-wt-k4b`, aac7 worktree `~/ntt-wt/k4b`). Done: kernel, `t_crtk`,
registry rows, microbenchmark. Next: queue item gates (2 n and 1 n, switch off and on) + `t_mul 20` / `t_strategy` /
`t_crtk` with the switch on and off; then the 1-node ABBA at 6.441e10 (≤ 4 rounds).
