# TREF — a test reference product, and the host product tier removed

**Question.** Build the test reference designed in `results/SH3.md` so the host product tier (`rns_mul`: mdev, the Karatsuba /
chunked split, the CPU schoolbook, `rns_mul_low`, `rns_mul_pair`; no production caller) can go; prove the reference first
(it must catch injected faults and agree with the old reference on every existing `t_mul` case), then move the tests and
remove the tier. No switch, no default, no digit changed.

**Change / branch.** Branch `tref` (from main b80ee2e). Commits (after the final rebase on 740bfc6): d465b08 + 06978b9 (the reference and `t_mul`'s proof mode,
against the old tier), 14eb7c6 (the removal and the tests moved), then docs. Files: `ecalc/tests/tref.h` (new),
`ecalc/tests/harness.h`, `t_mul.c`, `t_dbig.c`, `t_newton.c`, `t_mn_grid.c`, `t_primes.c`; `ecalc/rns_mul.c/h`,
`ecalc/rns_dist.c`, `ecalc/ecalc.c` (three `rns_free_scratch()` calls dropped), the registry (a count).

## The reference (`ecalc/tests/tref.h`)

- **Exact, up to 2^21 product limbs:** GMP. The decimal-limb conversions are now a product tree (value = lo + hi B^h, h = 2^k,
  the powers B^(2^k) kept; OpenMP tasks over the halves), `harness.h`'s `mpz_from_limbs` / `mpz_to_limbs`. SH3 assumed the old
  18-digit string bridge was quadratic above 2^20 limbs; it is not (GMP's `mpz_set_str` / `mpz_get_str` are subquadratic):
  single-threaded the tree and the string form take the same time (2^20 limbs: 2.2 s in, 4.5 s out on a 4-core
  workstation, m); the tree's gain is the parallel halves (1.5x on 4 cores, m). The string forms stay as its check (`t_mul` §0).
- **Residue form, any size:** (1) every limb < B and the normalised length na + nb or na + nb − 1; (2) c ≡ a b modulo three
  62-bit primes (`t1_q[0..2]`, `vf_limbs_mods`: O(n), parallel); (3) the low 64 limbs exactly (a b mod B^64 from the low 64
  limbs, GMP); (4) the top limbs: with A = Ah B^ka + Al (Ah the top 64 limbs) and B likewise, floor(c / B^(ka+kb)) must lie in
  [Ah Bh, Ah Bh + Ah + Bh] (exact bounds, not an estimate). One wrong limb changes c by d B^k, 0 < |d| < B, never a multiple of
  a prime q > B, so (2) alone always catches it.
- `tref_mul` (a reference *value*, used by `t_newton`, `t_mn_grid`, `t_dbig`): GMP up to 2^21 limbs, else the one-node device
  product (`rns_mul_dist_db`) at the default plane — the tests' grids run under `DIST_LOGN_TEST`, so the value comes from a
  different plane size — checked by the residue form (a failure is a VERIFY failure). `tref_faults` tests the reference itself.

## Method

aac7 queue items (2 nodes; logs on aac7 under `~/tref/q1/`, `~/tref/q2/`, raw, not in git; drivers in `archive:drivers/tref/`):
- **Proof** (the old tier still present): `t_mul 20 tref` at commit 3e1b35b (06978b9 after the rebase). On every product of the old sections 1 (pool 2^20:
  tiers and boundaries, 13 shapes x 5 generators), the low products, 2 (2^16 .. 2^30 limbs per operand) and the pair section:
  `tref_mul`'s value == `rns_mul`'s; the old check (GMP / three residues) accepts `tref_mul`'s value; `tref_check` accepts
  `rns_mul`'s (exact, and also the residue form at small sizes); one limb changed at the low end, the middle and the top must
  fail the new check (both forms) and the old one.
- **Before / after** on one node of the same hold: `t_mul 20` at main b80ee2e and at the branch.
- **New tests:** `t_mul 20` (§1 device path vs GMP at two plane caps, low products; §2 to 2^30 by the residue form, plane caps
  agreeing at 2^20, 2^24, 2^26; every product of §1-§2 also fault-tests the reference), `t_dbig 24 prod`, `t_primes gpu 26`.
- **Gates:** `tools/gate.sh --build --nodes 2`, `--nodes 1` (10⁹ digits, aac7 line); `ecalc/mnaccept.sh <hold> --only unit`.

## Results (all measured)

| item | result |
|---|---|
| proof, run 1 (sections 1, low products, 2 up to 2^24 x 2^24) | 72 products: value `tref_mul` == `rns_mul` on all 72; 0 VERIFY failures; every injected fault caught (new check, residue form, old check). The run was OOM-killed at 2^26 x 2^26 (one process holding the host tier's pinned staging and pools and the device tier's; VmHWM of the old `t_mul` alone 114.6 GB, of the new 128.1 GB) |
| proof, run 2 (sections 2 + pair, value agreement to 2^25 product limbs, above it cross-acceptance) | 17 products (11 by value: 2^16 .. 2^24 x 2^24 and the pair cases at 2^12, 2^20; 6 by cross-acceptance: 2^26, 2^27, 2^28, 2^30 and pair 2^27+5): 11 of 11 agree, the new check accepts all 17 of `rns_mul`'s products; 51 faults injected: 0 missed by the new check, 0 by the residue form (9 small cases), 0 by the old check; VERIFY OK (150 checks), 330 s |
| `t_mul 20` before (main b80ee2e) | 737 s, VERIFY OK (169 checks) |
| `t_mul 20` after (branch) | **187 s** (−550 s, −75 %), VERIFY OK (260 checks); 23 fault sets x 3 injected, 0 missed |
| `t_dbig 24 prod` (products to 2^27 against the reference) | 67 s, VERIFY OK (565 checks) |
| `t_primes gpu 26` (both plane sizes, closed form) | 45 s, VERIFY OK (89 checks) |
| gate `--build --nodes 2` / `--nodes 1` | PASS / PASS (item 2, branch a5ad2cd: 10⁹ digits identical, VERIFY OK; ecalc total 21.84 s at 2 nodes with P24, 9.76 s at 1 node) |
| `mnaccept.sh --only unit` (item 1, branch 3e8665c, 14eb7c6 after the rebase) | 9 passed, 0 failed: t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20 (2756 checks), t_verify, t_out, t_patch, t_mn_grid at 2 processes (244 checks; its reference is now `tref_mul`) |

Reference cost inside `t_mul` §2 (with the three injected faults): exact 2^20 x 2^20 8.2 s; residue form 2^24 x 2^24 0.07 s,
2^30 x 2^30 2.7 s (m). The device product at 2^30 x 2^30 took 2.9 s.

**Lines** (`git diff --numstat b80ee2e..tref`, m): production code −404 / +10 (`rns_mul.c` −341 / +3, `rns_mul.h` −32 / +5,
`rns_dist.c` −27, `ecalc.c` −4 / +2); tests +393 / −148 (`tref.h` +148 new, `harness.h` +93 / −6, `t_mul.c` +102 / −105, the
other four tests +49 / −37); docs +10 / −1. Net over code, tests and central docs: −141.

Removed (byte-identical pre-change files in `archive:removed_2026-10-13/tref/ecalc/`): `rns_mul`, `rns_mul_low`, `rns_mul_mdev`,
`rns_mul_pair`, `mdev_core`, `mul_karatsuba`, `mul_chunked`, `mul_rec`, `rns_free_scratch`, `mdev_gpu_crt`, `mdev_pts`,
`node_copy`, `par_copy`, `par_zero`, `x_inv_pw`, `rns_school_max`, `rns_crt_threads`, `rns_mdev_gpucrt`; `rns_mul_dist` (host
bigints), `acc_flat` and its staging in `rns_dist.c`. `t_mul`'s sections 1-3 (the host tier's tests and timings, args `big`,
`big32`) are replaced by the device-path sections; `t_primes` tier 0 (`rns_mul`) is now the device path at half the plane.

**Verdict.** The reference catches every injected fault (0 missed of all sets, both forms) and agrees with the old reference on
every case: 83 products by value, 6 large ones by cross-acceptance; the host tier is removed. `t_mul 20` is 3.9x faster.

**Open issues.**
- `struct acc`'s `flat` branches in `rns_dist.c` (the host-result spill merge, `A.flat` tests) are now dead: about 25 lines, left
  for a cleanup pass (kernel code; not touched here).
- `rns_stats` keeps the host tier's fields (`n_mdev`, `n_split`, `t_repack` ...), always 0 now; `ecalc.c` still prints
  `n_mdev` / `n_split` in three summary lines. Dropping them changes ecalc's output lines (left to the coordinator).
- The tests lost the host tier as an independent code path; the replacement independence is the residue form (no product)
  and the device path compared at two plane sizes (`t_mul` §1, §2; `t_primes`; `t_dbig` grid vs one plane).

## RESUME

State: done; merged to main. Nothing open but the open issues above.
