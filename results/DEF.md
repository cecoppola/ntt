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

PENDING

## 3. 6-node A/B of DM_MN_WRAP with the P24 default (m)

PENDING

## Verdict

PENDING

## Open issues

- `BS_LAYOUT_ONLY` reads `ECALC_NP` once per process: lay out multi-node points with `ECALC_NP=auto` (as the launch line
  does); with it unset a multi-node layout point shows three planes although the run would take four.
- P24 at a non-power-of-two group below 32 nodes (e.g. 3 nodes in `mnaccept.sh`) is not gated here.

## RESUME

Branch `def`; aac7 worktree `~/ntt-wt/def`; login-node outputs `~/def/login/`.
