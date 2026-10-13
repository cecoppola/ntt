# DEF — P24 as the multi-node default (D18) and DM_MN_WRAP in the memory model + 6-node A/B (D19 b)

Labels: **m** measured, **mod** modelled, **a** assumed. Times Eastern. Branch `def` (from `main` d1d19bf).

**Question.** D18: make the four-prime 24-digit product form P24 the default for every multi-node product, keeping the
three-prime multi-node path selectable. D19 (b): count `DM_MN_WRAP`'s extra block-pool memory in `dm_layout` /
`mem_model.py`, and time `DM_MN_WRAP=0` against `=1` at 6 nodes with the new default.

**Change / branch.** Branch `def`.
- `crt.c ec_np_planes`: under `ECALC_NP=auto`, pool 0 holds four planes at every g > 1, so `mn_p24_of()` (`rns_dist.c`) is
  true for every multi-node product. New setting **`ECALC_NP_MN3=1`** (off by default; review row: fallback) keeps the rule
  before D18: four planes only where the run's largest group can exceed the switch-over (from 32 node-processes).
- `ecalc.c`: `ECALC_NP` unset becomes `auto` at size > 1 (still 3 at size 1), so a multi-node run without a launch line also
  takes P24.
- `tools/gate.sh`: `--p24 auto|on|off` (forced P24 through `ECALC_NP_AUTO_TERMS=1000000`) is replaced by
  `--mn-primes p24|3`: at `--nodes` > 1 the gate checks the run log for `plane pool 0 for 4 primes` (default) or, with `3`,
  adds `ECALC_NP_MN3=1` and checks `plane pool 0 for 3 primes`.
- `mem_model.py np_planes` follows (reads `ECALC_NP_MN3` from the environment, as the C does).
- D19 (i): `binsplit.c dm_wrap_bytes` / `mem_model.py wrap_bytes`: with `DM_MN_WRAP` set and the wrap used
  (`newton_db.c mn_wrap_plane`'s rule), the mn division's low-product moment gains 2 quarters of P + 4 limbs per APU
  (Q·B^(−h) folded, held over both steps, and the folded A′ beside the assembly's Rc + Tt, where Aw + xql were). The
  `layout:` line prints ` wrap <bytes>` (absent when off: unchanged). `rns_dist.c`: `rns_mul_dist_mn_logcap_g`,
  `rns_mul_dist_mn_logmin_g` (by group size, for the layout).
- `DM_MN_WRAP`'s default is unchanged (0).

## 1. Planner and memory model (login node, m)

Build `def` (SHMEM), aac7 launch line (`tools/profile_load.py aac7 --line`), output `~/def/login/` on aac7.

| check | result |
|---|---|
| `MN_PLAN_ONLY=3.82e13:576` (default) | `plan check … OK -- 1236 products`; `plan primes`: tree 42 of 42, recip 49 of 49, div 18 of 18 pieces at four primes (P24) |
| the same with `ECALC_NP_MN3=1` | identical output (only the timestamp line differs): at 576 the old rule already gave four planes |
| the same with `DM_MN_WRAP=1` | plan check OK; the layout's node 380.84 → 406.61 GB (+25.77 GB), with v-slots 388.51 → 414.28 GB |
| `BS_LAYOUT_ONLY` at 6.441e10 × {1, 2, 4, 6} and 3.82e13/576 × 576, four variants (default, `ECALC_NP_MN3=1`, `DM_MN_WRAP=1`, both) | `mem_model.py --check-c FILE 31 2048`: **0 terms not exact, largest arena difference 0.0000 %** in all four (20 plane figures each) |
| pool 0 at 6.441e10 per node, cap 2^31 (`planes:` line) | g 2, 4, 6: planes 120.88 GB per node (P24 default) against 103.70 with `ECALC_NP_MN3=1` (+17.18 GB per node, +4.3 GB per APU); g 576: 120.88 both |

`DM_MN_WRAP=1`'s term per APU (`wrap / dev`, C = model, mod): 8.59 GB at 2 and 4 nodes, 5.73 GB at 6, 7.64 GB at
3.82e13 on 576. At 3.82e13 on 576 the arena grows 214.75 → 240.52 GB per node and the room's node 386.84 → 412.61 GB:
**fits** the 480 GB budget (`room: … fits 1`, aac7 launch line; the target's own line is private and was not used).
At 2 nodes the model's dm need grows 8.6 GB per APU against the 4.3 GB per APU of pool growth measured in P1 (3-prime
form): the term is an upper bound (the assembly moment has no product scratch beside it, which the model's `div` keeps).

## 2. Gates (aac7 queue, m)

`tools/gate.sh` at 10⁹ digits, aac7 launch line, digits compared with the reference. Items `84_def_gates2` (build `def`
ee85f5e, 20:15 ET), `85_def_gate4` (exclusive, build 5c84efb on the login node) and `86_def_regate` (after the rebase on
main 89318ae: `--build`, 5c84efb, 20:19 ET). Outputs `~/def/{gates,gate4,gates3}/` on aac7.

| gate | command | result | log check | device per APU, max (driver) |
|---|---|---|---|---|
| p24n2 (ee85f5e) | `--build --nodes 2` | PASS: rc 0, VERIFY OK, digits identical, 18.79 s | `plane pool 0 for 4 primes` (P24 in use) | 33.1 of 137.4 GB |
| p24n1 (ee85f5e) | `--nodes 1` | PASS, 12.27 s | `plane pool 0 for 3 primes` (size 1, unchanged) | 30.3 GB |
| mn3n2 (ee85f5e) | `--nodes 2 --mn-primes 3` (`ECALC_NP_MN3=1`) | PASS, 17.09 s | `plane pool 0 for 3 primes` | 28.8 GB |
| p24n4 (5c84efb) | `--nodes 4` | PASS, 20.30 s | `plane pool 0 for 4 primes` | 33.1 GB (in use 32.2: planes 120.3 GB per node + block pool 8.6) |
| p24n2 (5c84efb, rebased) | `--build --nodes 2` | PASS, 20.96 s | P24 in use | 33.1 GB |
| mn3n2 (5c84efb, rebased) | `--nodes 2 --mn-primes 3` | PASS, 17.03 s | three-prime path | 28.8 GB |

Memory headroom at 4 nodes: at 10⁹ digits 33.1 of 137.4 GB per APU (m). At the standard 6.441 × 10¹⁰ digits per node the
model gives the 4-node node peak 423.3 GB with P24 (`mem_per_node`, `ECALC_NP=auto`, shmem; mod), inside 480 GB;
MEAS-P24 measured 88.1 GB per APU in use at init at 2 nodes (m).

## 3. 6-node A/B of DM_MN_WRAP with the P24 default (m)

Method (TESTING.md §6): queue item `87_def_abba6` (6 nodes, exclusive, user-approved D19 b), driver `~/def/def_abba.sh`,
2026-10-12 20:55–21:50 ET. Build `main` f232085 (P24 default, SHMEM) in `~/ntt-wt/defm`; 6 × 6.441 × 10¹⁰ = 3.8646 × 10¹¹
digits, no digit output; the aac7 launch line (`tools/profile_load.py aac7 --line`) + `COMM_XSTATS=1 MN_WAIT_STATS=1`;
A = `DM_MN_WRAP=0`, B = `DM_MN_WRAP=1`; rounds A B B A, cap 4 rounds, 60 min budget. All 16 runs rc 0 and VERIFY OK
(7 lines per log); every log `plane pool 0 for 4 primes`; B logs `DM_MN_WRAP: X Q from the low 12880065414 limbs + one
cyclic plane of 2^33 (w 21470000006)`. Runs took 181–229 s of `total`.

| quantity | A mean (8 runs) | B mean (8 runs) | B − A per round | **B − A mean, 95 % CI, 4 rounds** |
|---|---|---|---|---|
| `total` (s) | 199.40 | 196.54 | −4.13, −2.62, −6.89, +2.19 | **−2.86 [−8.92, +3.19], p 0.23** (round cap reached, stop rule not met) |
| division phase dm (s, `RESULT ecalc dm s`) | 84.88 | 78.63 | −3.46, −8.92, −9.26, −3.36 | **−6.25 [−11.47, −1.03], p 0.032** |

Device memory per APU (m, `mem[…] [init]` and `live:` lines, all runs alike per arm):

| | A (`DM_MN_WRAP=0`) | B (`=1`) | B − A |
|---|---|---|---|
| in use at init (planes 30.1 + arena + block pool) | 81.6 GB | 88.1 GB | +6.5 (the arena request now counts the wrap: `dm_layout`) |
| block pool at the division | 51.54 GB | 57.98 GB (60.13 on some APUs) | +6.44 (+8.59) |
| the division's window peak (step 2 X_lo Q) | 46.88 GB | 52.25 GB | **+5.37** (model's wrap term 5.73 GB, mod) |
| driver-reported peak used (of 137.4 GB) | 84.4 GB | 92.9 GB | +8.5 |

## Verdict

Needs the user's decision; the switch stays off. At 6 nodes with the P24 default `DM_MN_WRAP=1` saves **6.25 s of the
division (95 % CI 1.0–11.5 s, p 0.032, 4 rounds, m)** but the `total` difference, −2.86 s [−8.92, +3.19], is not
significant after the 4-round cap (bs and init vary ±15 s between runs in both arms). It costs +5.4 GB per APU at the
division's peak (+6.4 to +8.6 GB of block pool, m), which the memory model now counts (5.73 GB per APU, mod; at 3.82 × 10¹³
on 576 the node with the room 386.8 → 412.6 GB, fits 480 GB, mod/m layout). D18 (P24 the multi-node default) is merged.

## Open issues

- `BS_LAYOUT_ONLY` reads `ECALC_NP` once per process: lay out multi-node points with `ECALC_NP=auto` (as the launch line
  does); with it unset a multi-node layout point shows three planes although the run would take four.
- P24 at a non-power-of-two group below 32 nodes (e.g. 3 nodes in `mnaccept.sh`) is not gated here.

- The 6-node `total` difference is not significant after 4 rounds; a further 4 rounds (another 6-node hour) could settle it.
- Measured at 6 nodes only with P24; the model's wrap term was checked against one measured peak (6 nodes: 5.37 vs 5.73 GB
  per APU) and P1's 2-node pool growth (4.3 GB per APU against 8.59 modelled, 3-prime form).

## RESUME

Done: merged to `main` (D18 default on; `DM_MN_WRAP` off), worktrees removed. Raw outputs on aac7: `~/def/login/` (plans,
layouts, `--check-c`), `~/def/{gates,gate4,gates3}/` (gates), `~/def/abba/` (`totals.tsv`: round, arm, seq, total, ok, dm,
rc, VERIFY lines, driver peak GB, pool GB, window peak GB; `stats.txt`, `log/`; driver `~/def/def_abba.sh <budget-min>`).
Next (user's call): adopt `DM_MN_WRAP=1` or not; optionally 4 more 6-node rounds for `total`.
