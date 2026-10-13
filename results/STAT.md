# STAT - static-analysis triage (L1, results/NIGHT.md section 6) and SH4 leftovers

Date 2026-10-13. Base: main b1e4d56. Gates ran on aac7 (SHMEM build, 2 nodes then 1 node, one queue item).

## 1. Analyzer findings (7 reports, 5 locations) and misleading-indentation (8)

| location | verdict | action |
|---|---|---|
| `ecalc/bigint.c:100` NULL `r` in `limb_mul_school` | false positive: the path needs `bi_mul_school` to reach `bi_reserve` with `a->n + b->n == 0`, but both are non-zero there (early return); the analyzer loses that across the recursive call | none in the code (still reported by gcc 13 `-O2 -fanalyzer`). Real nearby defect fixed: the `malloc` of the column scratch (`w1`) was unchecked; now `ec_fatal(EC_RC_OOM, ...)` |
| `ecalc/memsample.c:73`, `:78` `live[3]` uninitialised | false positive: the loop `d < DB_NQ` (4) fills all four | `live[DB_NQ] = {0}` (silences it; no behaviour change) |
| `tools/test_pack60.c:16` possibly-NULL `l` | real but harmless (test tool, unchecked `malloc`) | allocation checks in both loops |
| `tools/unpack_digits.c:149` possibly-NULL `p` | real but harmless (unchecked `calloc`) | check, message, exit 1 |
| `bigint.c:10` unused `oldn` | harmless (signature kept) | `(void)oldn` |
| `binsplit.c` unused `total0` (795), `tl0` (1406) | dead locals | removed |
| `binsplit.c` 8 misleading-indentation (234, 351, 438, 717, 1228, 1251, 1272, 1328) | all false positives: two statements on one line, each meant to be independent (for 1251/1272 the `t_wait_dma +=` after the loop is intended) | split onto separate lines, no semantic change |

Re-run (gcc 13.3, `-O2 -fanalyzer -Wall -Wextra`, and `-Wshadow -Wconversion` on binsplit.c): memsample.c, unpack_digits.c, test_pack60.c,
binsplit.c, and bigint.c except the one false positive above have no remaining analyzer, indentation or unused warnings (measured).

Conversion warnings (244: 148 sign-conversion, 96 conversion): none narrows an integer. 148 are `int` to `size_t`/`unsigned` sign changes
(indices, node/device/prime counts, never negative), 90+ are `size_t` to `double` (exact below 2^53; the largest counts at 3.82e13 digits,
about 2^41 limbs and 2^44 bytes, are far below). Noise; nothing to fix.

## 2. SH4 shrink

- `ecalc/rns_dist.c`: the `flat` member of `struct acc` and its dead branches (the four accessors, `cache_find`, `b_choose`, `dist_core` including the
  host-side spill merge) removed: `flat` was never set (`acc_db` is the only constructor). 17 lines net.
- `ecalc/rns_mul.h`: `rns_stats` loses `t_repack, t_h2d, t_fwd, t_inv, t_d2h, t_crt, n_mdev, n_split, n_school, points_mdev` (always 0 since TREF).
- Printed summary lines changed (no script or test parses them; grep of tools/, tests/, ecalc/*.sh found none): `recip` line loses "`, N mdev`",
  `10dP` line loses "`(N mdev, N splits)`", `dm` line loses "`; N mdev`"; `t_newton` divmod line loses "`N mdev, N splits`".
  (The task named three prints; `t_newton.c` had a fourth.)

## 3. Gates (aac7, measured)

| step | result |
|---|---|
| `tools/gate.sh --build --nodes 2` (SHMEM build, `-DCOMM_SHMEM`, no compiler warnings) | PASS, VERIFY OK, digits identical (1e9), P24 in use |
| `tools/gate.sh --nodes 1` | PASS, VERIFY OK, digits identical |
| `t_dbig 0` / `t_bs` / `t_newton 20` / `t_dist` | VERIFY OK (555 / 10 / 2756 / 55 checks) |
| `test_pack60` | PASS |

## RESUME
Done; merged to main with this file. Open: bigint.c:100 stays as a documented analyzer false positive.
