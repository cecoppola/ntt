# SH3 — shrink pass 3: rns_dist.c dead items, duplicated helpers, the host product tier (2026-10-12)

**Question.** Finish the SHR2 follow-ups: remove the `rns_dist.c` items SHR2 skipped, merge duplicated helpers where safe, and
decide whether the host product tier can go. No switch added, no default changed, no digit changed.

**Branch.** `sh3` from main 5081105. Other open branches while this ran: adopt (rns_mul.c, binsplit.c, newton_db.c), k4c
(p24_crt.c), recip (newton_db.c, t_newton.c): hunks kept small.

## What was removed (lines, measured: `git diff --numstat`)

| item | result | lines (-/+) |
|---|---|---|
| `gen_pack_bench`, `gb_cmp`, `gb_dcmp`, the plain reference kernels `k_twpack_g` / `k_unpacktw_g`, the `DIST_GBENCH` block of `t_dist`, rows `DIST_GBENCH` / `DIST_PREPS` (only read by it) | removed | rns_dist.c -99 (38 plain kernels + 61 bench), t_dist.c -8, registry -3 (README row edited) |
| `pow_table` (duplicate of ntt_dist.c `dev_pow_table`; only the bench used it), `part_owner` (only the plain kernels used it) | removed | rns_dist.c -7 and -1 |
| `rns_mul_dist_hd` (self-recursive only, no caller) | removed | rns_dist.c -18, rns_mul.h -1 |
| `acc_flat`, `rns_mul_dist` | **kept**: `rns_mul_dist` has test callers (`t_mul`, `t_primes` tier 1: the host-bigint path of the distributed tier); `acc_flat` is its helper | 0 |
| `k_store` (rns_mul.c) = `k_copy_h2d` (dbig.c) | merged: one kernel, `db_copy_kernel` (dbig.c) launches it for rns_mul.c (same grid, block, stream, so the launch is unchanged) | rns_mul.c -9/+3, dbig.c/h +2 |
| `make_mdb` (t_mn_grid.c = cx_grid.c) | merged by archiving `tests/cx_grid.c` (no Makefile rule since H4) and `cx_shapes_c25.txt`, `cx_shapes_c27.txt` to `archive:removed_2026-10-12/sh3/` (byte-identical) | tests -115 (97 + 9 + 9) |
| `mn_powmod` (newton_db.c) = `powmod_h` (dbig.c) | **skipped**: branch `recip` edits newton_db.c (11 lines); do after RECIP merges (one line each: export `db_powmod_h`, drop the copy) | 0 |
| `dmod128` / `dmod192` (rns_mul.c vs p24_crt.c) | **waits** for K4c (p24_crt.c is not to be touched) | 0 |
| host product tier (≈270 lines) | **kept**, design below | 0 |

Total: rns_dist.c 2284 -> 2155 lines (-129 net, +1 comment line edited); all files together -261 net (9 added, 270 removed, before this file) (`git diff --shortstat main..sh3`).

## Host product tier: why it stays, and the design that would let it go

Users of `rns_mul` as a reference product: `t_mul` (also benchmarks the tier itself and tests `rns_mul_low`, `rns_mul_pair`),
`t_dbig` (grid products up to 30e6 x 7e6 limbs), `t_mn_grid` (up to ~0.9 x 2^cap limbs per operand), `t_newton`, `t_primes`
(tier 0). Replacing it by GMP is not small: the decimal limbs go to GMP through a string (`harness.h`) which is quadratic-ish
above 2^20 limbs, and `mpz_mul` of 5e8-digit operands is tens of seconds per product, times the dozens of shapes. So a
GMP reference cannot cover the large grid tests; and `t_mul`'s tier-specific tests would be deleted, not ported.

Design (proposed, not done; ~1 task, needs its own gate run):
1. **Residue check as the large-size reference.** For C = A B check `C mod q = (A mod q)(B mod q) mod q` for 3 fixed primes q
   (61-bit) using the existing `vf_digits_mod` / `db_mod_q` (O(n), no product), plus the exact low 64 limbs (schoolbook) and
   the top limbs (from a floating or GMP product of the leading 64 limbs). A wrong digit survives only with probability about
   2^-60 per prime; structured carry errors (a limb moved by B^k) change the residue unless q divides B^k - 1.
2. **GMP below 2^20 limbs** through a product-tree base conversion (O(M(n) log n)) in `harness.h`, for the small and
   boundary cases (x * 0, x * 1, all-ones, 2^k +- 1) where an exact reference is cheap.
3. Then `rns_mul`, `rns_mul_low`, `rns_mul_mdev`, `rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`, `mul_rec`,
   `mdev_gpu_crt` go; `t_mul`'s timing section is dropped (its numbers live in results/); `t_primes` tier 0 moves to the
   device path at two transform sizes (`DIST_LOGN_TEST`).
Risk: the tests lose an independent code path (the host tier shares the primes and CRT but not the distributed kernels), so
step 1's residue check should run on the device path against a second device run at another plane size before the tier goes.

## Gates (aac7 queue, measured)

On the sh3 branch (aac7 queue item, 2 nodes, hold released by the runner; logs on aac7 under `~/sh3q/`, raw, not in git):
`tools/gate.sh --build --nodes 2` PASS (SHMEM build, P24, 1e9 digits identical, ecalc 21.91 s); `--nodes 1` PASS (identical, 10.49 s);
`mnaccept.sh --only unit,e9,mn`: 15 passed, 0 failed (unit incl. t_ntt 24, t_mul 20, t_dbig, t_newton 20, t_mn_grid 2 procs; e9;
mn 1e8 at 2/3/4, 1e9 at 2/4), digits identical. doccheck 0 problems.

## RESUME

State: see the gates above; merge, push, TASKS row SH3, remove worktrees. Open: `mn_powmod`/`powmod_h` (after RECIP),
`dmod128`/`dmod192` (after K4c), the host tier (design above).
