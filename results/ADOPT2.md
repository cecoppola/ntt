# ADOPT2 - `P24_CRT_SCAN=1` (D24) and `NEWTON_CHAIN_GUARD=1` (D25) as code defaults; two helper merges

**Change.** Both switches default to 1 (`=0` stays the fallback). `p24_crt_scan_on()` (p24_crt.c) and `newton_chain_guard()`
(newton_db.c) are the readers; the startup `config:` line shows `p24_crt_scan:%d,chain_guard:%d`. `mem_model.py` adds the guard's
+1 limb to the last doubling's start (`CHAIN_GUARD_CODE`, read from `NEWTON_CHAIN_GUARD`, as `binsplit.c dm_layout`).
Helper merges (SH3 follow-ups): `dmod128`/`dmod192` once in `rns_int.h` (rns_mul.c, p24_crt.c use it; was a copy), `db_powmod_h` once in
`dbig.c` (declared in `dbig.h`; `newton_db.c` had `mn_powmod`). Same behaviour.

**Checks (aac7, 2-node hold, SHMEM build; all measured).**
- `tools/gate.sh --build --nodes 2` PASS (VERIFY OK, digits identical, P24 in use, ecalc total 20.66 s); `--nodes 1` PASS (9.64 s).
  The `config:` line shows `p24_crt_scan:1,chain_guard:1`.
- `tests/t_newton 20` VERIFY OK (2756 checks); `mnaccept.sh --only unit` 9 passed (1556 s, t_mn_grid included, no hang: the unit step
  alone takes ~1500 s); `--only e9,mn` 6 passed (108 s), digits identical.
- `MN_PLAN_ONLY=38200000000000:576` runs (rc 0). `mem_model.py --check-c` exact (0 terms not exact) against `BS_LAYOUT_ONLY` at 1e10, 1e11 (1 node),
  6.441e10 x 2, and the 576-node share, with the guard 0 and 1.
- ABBA, 1 node, 1e10 digits, no digit output, A = guard 0, B = guard 1, 4 rounds (8 pairs of runs): B - A = +0.25 s,
  95% CI [-1.00, +1.50] s (n.s.; runs 15.9-19.4 s): no measurable cost.
- Not re-measured here: `P24_CRT_SCAN` has no effect at 1 node (multi-node only); its timing is results/K4c.md.

## RESUME
Done; merged. Raw logs on the cluster under `~/adopt2/` (removed by the coordinator when no longer needed).
