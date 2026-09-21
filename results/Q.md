# Q — the target plan: the models completed for 576 nodes, `estimate(g, D)`, the runbook (Phase 12, PLAN.md §27 row Q; 2026-09-21)

Branch `q12` (from `main` @ 7aded87). Files: `ecalc/mn_model.py` (X's fabric model, extended), `ecalc/mem_model.py`
(M's memory model, extended), NEW `ecalc/estimate.py`, NEW `docs/TARGET.md`. Nothing outside my files. No node time
used (the calibration is against the recorded walls; the phase splits were read from the earlier batches' logs on
aac6: `~/l11/out{1,2}`, `~/x11/b{1,2}`, `~/ntt-m11/ecalc/results/m11`, `~/ntt-s/ecalc/results/mnaccept/20817`).

## HEADLINE (the standing rule, PLAN §26) — 576 nodes

| code form | max digits per node | digits total | per-node wall | label |
|---|---|---|---|---|
| **Phase 12 (G's gridded top product + S's pool-resident slabs), the ceiling (502 GB)** | **6.7 × 10¹⁰** | **3.9 × 10¹³** | **4.0 min** | modelled |
| **Phase 12, the safe size (480 GB)** | **6.1 × 10¹⁰** | **3.5 × 10¹³** | **3.8 min** | modelled |
| Phase 12, every level product one piece | 3.8 × 10¹⁰ | 2.2 × 10¹³ | 2.0 min | modelled |
| G's grid, the transport's staging freed per exchange (a small `comm_shmem.c` change, not written) | 5.6 × 10¹⁰ | 3.2 × 10¹³ | 3.3 min | modelled |
| **the code at `main` 7aded87 (`estimate.py --as-is`)** | **9.5 × 10⁹** | **5.5 × 10¹²** | 0.7 min | modelled |

Labels: the per-node compute is **measured** on one node (4 × 10¹⁰ 81.5 s, 7 × 10¹⁰ 153.5, 8 × 10¹⁰ 195.5, 10¹¹ 262.9;
memory 253 / 369 / 431 GB); the distributed levels, the sharded division and the memory at g > 1 are **modelled**
(the arithmetic below, calibrated on 16 aac6 multi-process points within 6 %); the fabric's 100 GB/s per APU, 2 µs per
message, the part file's 2 GB/s per node and the general map's exchange overlap are **assumed** (PLAN §25; nothing on
aac6 measures them). Phase 11's headline (7.7 × 10¹⁰ per node, 4.4 × 10¹³) does not survive M's accounting of the top
level's scratch (86 GB per node at the cap) and the sharded exchange (69 GB): 541 GB per node.

Two findings that change the target plan (both in `docs/TARGET.md` §8, traps 6 and 11):

1. **The arena request at size > 1 is uncapped.** `binsplit.c`'s `tree_need_dev` sizes the top level as one
   transform over the level's whole product (q = n / 4g, two g × C × 4-limb spill buffers) although `rns_dist.c`'s
   `mn_grid` would form pieces at the cap 2^(31 + ⌊log₂ g⌋). The arena the code *asks for* at 576 × 4 × 10¹⁰ is
   ≈ 440 GB per node; the run fails at init above ≈ 1.9 × 10¹⁰ per node whatever the pieces do. Agent G's gridded tree
   must re-derive that formula (the `grid` form of `mem_model.tree_need_dev` is the target: piece-sized slabs ≤ 3q/2,
   cx + tmp 2q, O(C) spills, the general map's v-scratch 3q/4).
2. **The SHMEM transport's staging is kept per communicator** (`comm_shmem.c` `staging()`: grown to the largest
   exchange seen, freed at `comm_destroy`; the level meshes live to the end). At 576 nodes every level's mesh holds
   the result exchange's q × 8 B send + receive per APU thread (8.6 GB), ten levels × four threads ≈ **345 GB per
   node** — the 8 GiB default pool would abort at the first level, and no pool of that size fits beside the arena.
   S's Phase 12 form (the callers' slabs resident in the pool) removes it; freeing the staging in `wait()` bounds it
   to one exchange (54 GB per node at 6 × 10¹⁰); `COMM_SHMEM_POOL_MB` must be sized from `estimate.py`'s `pool` column.

Also: `MN_GROUPS` is parsed (`mn_groups_parse`, rns_dist.c) but at 7aded87 has no caller — `mn_tree` walks binary
levels 2^l clipped to the size (the parser's default schedule, `2,4,…,512,576`); the k-way combine is G's `tree_level`.

## 1. L's real schedule in the model (`mn_model.tree_cost`, `mem_model.level_children`)

`mem_model.mn_groups(g, spec)` is `mn_groups_parse` in Python (the same rules: increasing, multiples, anything ≥ g
ends the list at g; default the powers of two then g). `level_children(g, spec)` gives per level the group size S and
its children — the previous level's groups `[k P, min((k+1) P, S))` — so the default at 576 is ten levels
`2, 4, …, 512, 576` whose top joins a 512-group and a 64-group, `2,4,8,16,32,64,576` is a 9-way level of nine 64-groups,
`…,64,192,576` two 3-way levels. A k-way level is costed as the tree forms it (results/L.md: "k − 1 products over
the level's group, each balanced over all its nodes"): the fold `(P, Q) ← (P Q_i + P_i, Q Q_i)` for i = 2 … k —
2 (k − 1) products over the level's S nodes, the accumulated operand growing from m₁ to S − m_k leaf shares, the P
product with the shifted add. Each product is `product_cost` as X wrote it, with three corrections for L's map: every
node of the group transforms (q = pts / 4g, not 4gt), the cap is `mn_logn_cap` (2^(31 + ⌊log₂ g⌋), the plane minimum
`mn_shape`'s), and a group that is not a power of two runs the general map with one v-exchange in flight, so only
`GEN_HIDE` = ½ (assumed) of the xGMI stage hides the fabric stage. The spill all-gather is now costed per form
(flat: g × C × 32 B received per APU per piece — 19 GB at 576; grid: O(C)).

**9-way vs 3·3 (`./mn_model.py --schedules`)**, the distributed levels' per-node wall at 576 nodes, tree form grid:

| MN_GROUPS | 4 × 10¹⁰: levels / exposed / pieces / global TB / msgs per APU | 6 × 10¹⁰ | 7.7 × 10¹⁰ |
|---|---|---|---|
| default `2,4,…,512,576` (512 + 64 join) | 46.5 s / 11.0 / 42 / 2.2 / 318 k | 82.9 / 20.4 / 76 / 3.9 / 419 k | 84.5 / 20.8 / 78 / 4.1 / 459 k |
| `2,4,8,16,32,64,576` (9-way) | 49.9 / 13.3 / 48 / 2.8 / 665 k | 80.7 / 21.2 / 72 / 3.9 / 681 k | 86.6 / 23.2 / 78 / 4.8 / 802 k |
| **`2,4,8,16,32,64,192,576` (3·3)** | **44.3** / 11.1 / 40 / **1.9** / **262 k** | **78.2** / 20.2 / 78 / 3.2 / 426 k | 89.0 / 23.9 / 90 / 4.6 / 614 k |

**Decision: `MN_GROUPS=2,4,8,16,32,64,192,576`** — the cheapest at the safe size and at 4 × 10¹⁰ (−5 % of the levels
against the 9-way, −2…−6 % against the default), the fewest global-link bytes and messages (the 9-way's single
576-node level is the general map with 575 peers per exchange and a 2⁴⁰ cap — 8 products of 16 pieces each at
6 × 10¹⁰), and its six doublings stay inside a 64-node dragonfly group. The three are within the model's own error
(±6 %) of each other at every size, and at 7.7 × 10¹⁰ the default is 5 % cheaper (its levels' grids happen to cut
better) — the choice is one environment variable and TARGET.md §5 step 4 measures both explicit schedules at 576 × 10⁹.
The distributed levels are 33–36 % of the 576-node wall at every D (X's finding stands); the fabric's exposed part
is 16–19 %.

## 2. The memory model at g in both forms (`mem_model.py --tree`/`form`)

`tree_need_dev(nq_leaf, g, …, form, pool_log, groups)`:

* **`flat`** = `tree_need_dev_flat`: M's port of binsplit.c's formula, unchanged (binary levels, one uncapped transform,
  q = n / 4g, two g × C × 4-limb spill buffers): the arena the code requests today. The task's "planes n/(4g)" is M11's
  reading of it (the pool growth it implies never happens in `rns_dist.c` — the cap — but the arena request does).
* **`grid`** = G's form as PLAN §27 specifies it: the levels from `level_children`, each product's piece at the level's
  cap (`mn_shape` of min(n_c, 2^cap) → q ≤ 2^29 limbs per rank: the plane pools never grow), the slabs sb/rbA/rbB
  sized to the piece's operand quarter (≤ 3q/2 per APU; B7's exact sequences), cx + tmp = 2q (the general map has no
  tmp), the spills 2 × C × 4 limbs per APU (own + received, O(C)), the general map's v-exchange scratch 3q/4 (L's open
  issue), the window temporary of the piece's share; the live shares are whole shares (P, Q in and out, + 1/8).

Also new: the SHMEM transport's staging (`shmem_staging`: `cached` / `per_exchange` / `resident`, §HEADLINE finding 2),
the SHMEM pool on the host (the larger of `COMM_SHMEM_POOL_MB` and the staging), `alltoallv` default on (merged),
`transport` and `groups` options. The size-1 calibration (M11's table: 4 / 7 / 8 × 10¹⁰ within 1 %, 10¹⁰/4's 20.3 GB
arena) is unchanged (`python3 mem_model.py`).

Per node at 576 (GB; `python3 mem_model.py`, `estimate.py --verbose`):

| D per node | form | planes | arena (= max of bs regions, dm need, tree need) | top scratch | exchange | host (SHMEM pool) | node peak | fits |
|---|---|---|---|---|---|---|---|---|
| 4 × 10¹⁰ | flat, cached | 428 | 709 | 556 | 36 | 366 (345) | 1515 | no |
| 1.9 × 10¹⁰ | flat, cached | 121 | 318 | 244 | 17 | 207 (185) | 659 | no |
| 9.5 × 10⁹ | flat, cached | 121 | 240 | 199 | 9 | 114 (93) | 479 | yes |
| 4 × 10¹⁰ | grid, resident | 121 | 219 (104 / 219 / 193) | 86 | 36 | 30 (9) | 402 | yes, margin |
| 6.1 × 10¹⁰ | grid, resident | 121 | 277 (152 / 277 / 237) | 86 | 54 | 30 | 479 | yes (480) |
| 6.7 × 10¹⁰ | grid, resident | 121 | 294 (173 / 294 / 250) | 86 | 60 | 30 | 501 | yes (502) |
| 7.7 × 10¹⁰ | grid, resident | 121 | 325 | 86 | 69 | 30 | 541 | no |
| 6.1 × 10¹⁰ | grid, per_exchange | 121 | 277 | 86 | 54 | 76 (54) | 524 | no |

**Safe and ceiling per node at 576 with the margins** (`estimate.py --max`, form grid + resident): **502 GB → 6.7 × 10¹⁰
(3.86 × 10¹³ digits, 4.0 min); 480 GB → 6.1 × 10¹⁰ (3.53 × 10¹³, 3.8 min)**. Flat + cached (the code as is): 9.5 × 10⁹
(5.5 × 10¹²); flat alone or cached alone: 1.9 × 10¹⁰ (1.1 × 10¹³) each; grid + per_exchange: 5.6 × 10¹⁰ / 5.1 × 10¹⁰.
At g = 4 and 64 the grid ceiling is 6.3 × 10¹⁰ / 5.7 × 10¹⁰ (the top scratch at a power-of-two cap is 101 GB, 15 more
than at 576's 2⁴⁰ over 2 304 ranks). Size 1: 1.19 × 10¹¹ / 1.12 × 10¹¹ (M11's number).

## 3. `estimate(g, D)` — `ecalc/estimate.py`

`estimate(g, D, tree, groups, fabric, rule, transport, staging) → dict`; the CLI prints the table with every column
labelled measured / modelled / assumed (`--g --D --tree --staging --groups --bw --lat --group --layers --taper
--write-bw --max --verbose --as-is`). The standing table (`./estimate.py`; form grid + resident, the default schedule):

| g | D per node | digits | wall s | min | exposed s (%) | device GB | host GB | pool | node GB | NIC per node | per NIC | global | fits 502 / 480 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 × 10¹⁰ | 4.0 × 10¹⁰ | 81.4 | 1.4 | 0 | 253 | 16 | 0 | 265 | 0 | 0 | 0 | yes / yes |
| 1 | 7.7 × 10¹⁰ | 7.7 × 10¹⁰ | 174.6 | 2.9 | 0 | 360 | 16 | 0 | 372 | 0 | 0 | 0 | yes / yes |
| 1 | 10¹¹ | 10¹¹ | 262.6 | 4.4 | 0 | 431 | 16 | 0 | 443 | 0 | 0 | 0 | yes / yes |
| 4 | 4 × 10¹⁰ | 1.6 × 10¹¹ | 97.8 | 1.6 | 5.6 (6) | 390 | 30 | 9 | 416 | 4.3 TB | 541 GB | 0 | yes / yes |
| 4 | 7.7 × 10¹⁰ | 3.1 × 10¹¹ | 188.4 | 3.1 | 12.4 (7) | 529 | 30 | 9 | 556 | 9.7 TB | 1.2 TB | 0 | NO / no |
| 4 | 10¹¹ | 4.0 × 10¹¹ | 284.5 | 4.7 | 20.5 (7) | 621 | 30 | 9 | 648 | 16.4 TB | 2.0 TB | 0 | NO / no |
| 64 | 4 × 10¹⁰ | 2.56 × 10¹² | 119.4 | 2.0 | 13.1 (11) | 390 | 30 | 9 | 417 | 8.4 TB | 1.0 TB | 0 | yes / yes |
| 64 | 7.7 × 10¹⁰ | 4.93 × 10¹² | 228.0 | 3.8 | 27.8 (12) | 530 | 30 | 9 | 556 | 17.8 TB | 2.2 TB | 0 | NO / no |
| 64 | 10¹¹ | 6.4 × 10¹² | 358.1 | 6.0 | 47.8 (13) | 621 | 30 | 9 | 648 | 31.1 TB | 3.9 TB | 0 | NO / no |
| **576** | **4 × 10¹⁰** | **2.30 × 10¹³** | **140.4** | **2.3** | 22.6 (16) | 375 | 30 | 9 | **402** | 11.3 TB | 1.4 TB | 6.0 TB | **yes / yes** |
| 576 | 7.7 × 10¹⁰ | 4.44 × 10¹³ | 279.3 | 4.7 | 51.4 (18) | 515 | 30 | 9 | 541 | 25.5 TB | 3.2 TB | 15.2 TB | NO / no |
| 576 | 10¹¹ | 5.76 × 10¹³ | 437.6 | 7.3 | 84.2 (19) | 607 | 30 | 9 | 633 | 43.2 TB | 5.4 TB | 24.2 TB | NO / no |

(Size 1 at 7.7 × 10¹⁰ and 10¹¹: the wall is the measured table interpolated; the 4-node rows at 7.7 × 10¹⁰ do not fit
because of the top scratch at a 2³³ cap and the exchange — the single node has neither.) The same at the sizes that
matter: 576 × 3.8 × 10¹⁰ = 2.19 × 10¹³ in 2.0 min (395 GB); **576 × 6.1 × 10¹⁰ = 3.51 × 10¹³ in 3.8 min (479 GB)**;
576 × 6.7 × 10¹⁰ = 3.86 × 10¹³ in 4.0 min (501 GB). With `--as-is` (flat + cached): 576 × 9.5 × 10⁹ = 5.5 × 10¹² in
0.7 min (479 GB); 576 × 1.9 × 10¹⁰ → 659 GB, does not fit.

Sensitivities at 576 × 6.1 × 10¹⁰ (3.8 min): `--lat 2e-5` 4.3 min (2.5 M messages per APU), `--bw 50` 4.6, `--write-bw 1`
4.3, `--taper 0.5` 4.1, `--layers 3` 4.5 (the NIC bytes double: 21 → 44 TB per node), `--layers 3 --lat 2e-5` 4.7 —
the third layer does not pay below ≈ 20 µs per message (X's conclusion stands with L's schedule).

## 4. Calibration against every recorded aac6 point (`./mn_model.py --calib`)

The substitute fabric: g node-processes on one node over a loopback transport, each driving the four APUs (the local
passes shared g-way), the leaves concurrent (0.5 × the single-node bs of the total digits, measured), init the
process's own (measured: shmem_init and the 8 GiB pool's registration make it 11–15 s under OSHMEM). The transport
constants per (transport, g) — bandwidth per process, a fixed cost per chunk exchange, the small-exchange exponent —
are **fitted** (`aac6_fabric`): TCP 3.0 GB/s with 0.5 / 2.0 / 1.5 / 8 / 32 ms per chunk exchange at 2 / 3 / 4 / 6 / 9
processes (the fixed cost grows with the process count: g × 4 meshes × (g − 1) peer threads contend on one node, and
the general map's `alltoallv` at 6 and 9 allocates its scratch per call); OSHMEM 1.5 GB/s (the staging on both sides)
with 0.5 / 1.0 / 0.25 ms. So each (transport, g) has 2–3 parameters for 1–3 points: the only genuine check is the
consistency across D at one g (2: 10⁸–10¹⁰, 4: 10⁸–10¹⁰, 3 and 6: 10⁸–10⁹), and the per-phase split (tree, reciprocal,
division), which the fit does not see. None of these constants applies to the target (two parameters: `--bw`, `--lat`).

| digits | g | transport | wall measured | modelled | error | tree meas / model | recip | div | source |
|---|---|---|---|---|---|---|---|---|---|
| 10⁸ | 2 | TCP | 8.5 | 8.8 | +3 % | 0.6 / 0.8 | 3.7 / 3.7 | 1.0 / 1.3 | X.md b2, L.md, M11 (8.37–8.61 over five runs) |
| 10⁸ | 3 | TCP | 9.3 | 9.1 | −3 % | 1.1 / 0.9 | 4.0 / 4.1 | 0.9 / 0.9 | X.md 9.13 / L.md 9.41 |
| 10⁸ | 4 | TCP | 8.4 | 8.5 | +1 % | 0.9 / 0.7 | 3.6 / 3.9 | 0.7 / 0.6 | L.md 8.28 / X.md 8.09 / M11 8.98 |
| 10⁸ | 6 | TCP | 25.8 | 26.1 | +1 % | 4.4 / 5.3 | 14.5 / 14.3 | 3.0 / 2.9 | L.md batch 1 (the general map) |
| 10⁸ | 9 | TCP | 62.2 | 65.7 | +6 % | 14.6 / 18.1 | 37.9 / 37.5 | 4.9 / 6.0 | L.md batch 1 (the general map) |
| 10⁹ | 2 | TCP | 27.4 | 29.1 | +6 % | 3.0 / 3.1 | 12.2 / 12.7 | 5.8 / 6.8 | X.md 27.66 / 27.41, L.md 26.14 |
| 10⁹ | 3 | TCP | 24.5 | 24.9 | +2 % | 5.1 / 3.9 | 9.4 / 9.6 | 3.5 / 4.8 | L.md batch 1 (26.5 with the grids forced) |
| 10⁹ | 4 | TCP | 19.7 | 19.5 | −1 % | 3.2 / 2.2 | 8.0 / 7.7 | 2.7 / 3.4 | X.md 19.69 / 20.38, L.md 18.92 |
| 10⁹ | 6 | TCP | 55.8 | 55.7 | −0 % | 9.3 / 13.1 | 36.4 / 28.2 | 4.2 / 8.3 | L.md batch 2 (the general map) |
| 10¹⁰ | 2 | TCP | 146.4 | 140.7 | −4 % | 22.3 / 20.8 | 64.9 / 59.0 | 40.3 / 42.6 | X.md job 20802 |
| 10¹⁰ | 4 | TCP | 108.0 | 109.6 | +1 % | 28.0 / 26.4 | 36.0 / 38.0 | 30.0 / 32.3 | X.md 110.1 / 105.7 / 100.6, L.md 99.3, M11 116.5 |
| 10⁸ | 2 | OSHMEM | 22.4 | 22.1 | −1 % | 0.6 / 0.9 | 4.3 / 3.9 | 1.7 / 1.7 | S.md job 20817 |
| 10⁸ | 3 | OSHMEM | 19.3 | 19.3 | −0 % | 1.4 / 1.6 | 4.9 / 4.8 | 1.7 / 1.8 | S.md job 20817 |
| 10⁸ | 4 | OSHMEM | 18.5 | 19.0 | +2 % | 1.2 / 1.4 | 4.1 / 4.5 | 1.5 / 1.6 | S.md job 20817 |
| 10⁹ | 2 | OSHMEM | 43.9 | 44.7 | +2 % | 5.1 / 4.6 | 18.6 / 16.9 | 8.1 / 11.1 | S.md job 20817 |
| 10⁹ | 4 | OSHMEM | 43.8 | 44.7 | +2 % | 7.3 / 6.7 | 16.8 / 16.0 | 7.8 / 9.7 | S.md job 20817 |

**Gate: every point within 10 % (worst 6.1 %).** Where the phases are off although the wall is not: 10⁹ at 6
processes (the general map on loopback: the reciprocal's 36 s against 28 modelled, the division 4 against 8 — the
per-call `hipMalloc` of the v-scratch and 24 threads' contention are one fixed cost in the model, and at 10⁹ the
reciprocal's exchanges are many and small while the division's are few and large); 10⁸ at 9 (+6 %: the same, at the
size where everything is fixed cost — 62 s for 10⁸ digits is the loopback, not the code: the per-rank work is 1/g of
the plane); the 10¹⁰/4 point is the mean of five runs spread 99–117 s on the shared node (RESULTS §68's per-node
spread, and X1's two runs) — the model's 109.6 is inside it. The shared-node effects the model does not separate: the
g processes' CPU threads (4 APU threads + helper + 48 background each) on 96 cores at g = 9, the loopback's memory-bus
share, and init's registration under OSHMEM (taken as measured).

## 5. `docs/TARGET.md` — the runbook

The build (Makefile's `SHMEM=1` with `SHMEM_CFLAGS`/`SHMEM_LIBS` for Cray `cc` or SOS), the environment (every
`COMM_*`, `MN_*`, `DIST_*`, `RNS_*`, `NEWTON_MN_*`, `ECALC_*`, `BS_CKPT_*` variable that matters, the target's value and
why), the `srun` line (one process per node, `COMM_RANK/SIZE` from Slurm; the tests to run on 2, 4, 9 nodes first),
`MN_GROUPS` and `MN_TOPO_GROUP` (off until the per-message cost is measured), the sizes in order (10⁸ at 2/3/4/9, 10⁹
at 2/4/64 with `cmp`, 10¹⁰ per node at 64, 10¹² over 576 with both schedules, then 3.8 × 10¹⁰, the safe 6.1 × 10¹⁰, the
ceiling 6.7 × 10¹⁰), checkpoints (`BS_CKPT_DIR`, `BS_CKPT_TREE_EVERY=3`, `ECALC_CKPT_TOP` at size 1), the recheck
(`ECALC_RECHECK=1`), what to measure first (the injection bandwidth from `DIST_STATS`, the per-message cost from
`t_comm`, the part-file and checkpoint bandwidths, the mapping rate, the taper) and how each feeds `estimate.py`
(`--bw --lat --write-bw --taper`, `NEWTON_MN_BW/LAT`, the constants at the top of `mn_model.py`), the traps (setarch -L
on OSHMEM, fence vs quiet, the serial lock, the port base, the page-cache rule, the uncapped arena request, the
unwired `MN_GROUPS`, the communicator ids, the slow node, the missing reference, the SHMEM staging).

**Every variable named in TARGET.md exists in the code**: checked by the script in its §9 (`grep -oE` of the
back-quoted names against `ecalc/*.c ecalc/tests/*.c ecalc/*.sh ecalc/Makefile`); the names the check flags are
the model's own constants (`GEN_HIDE`, `HIDDEN_XGMI`, `PHASES`), the libraries' variables (`XT_SYMMETRIC_HEAP_SIZE`,
`SHMEM_THREAD_MULTIPLE`, `OMPI_MCA_*`) and agent R's `RNS_POOL_GROW` of this phase — each stated as such in the text.

## 6. Tests run

No node jobs. `python3 mn_model.py --calib` (16/16 within 10 %, worst 6.1 %), `python3 mn_model.py --schedules`,
`python3 mn_model.py` (the full tables, ≈ 40 s), `python3 mem_model.py` (the size-1 calibration table unchanged: 4 / 7 /
8 × 10¹⁰ within 1 %), `python3 estimate.py` / `--max` / `--as-is` / `--staging per_exchange` / `--verbose` / `--lat 2e-5`
/ `--bw 50` / `--layers 3` / `--groups 2,4,8,16,32,64,192,576` — all run; the numbers above are their output.

## 7. Open issues

* The model's `grid` form is a specification of G's code, not a measurement of it; G's gate ("the memory model's
  g-terms updated and matching") is where the two meet — the 10¹⁰ at size 4 forced-grid run's `mem_report` against
  `mem_per_node(2.5e9, 4, dict(form='grid', pool_log=29))` (8.6 GB of top scratch per node modelled).
* The SHMEM staging accounting (`shmem_staging`) is arithmetic on `comm_shmem.c` as merged; S's Phase 12 branch decides
  which of `resident` / `per_exchange` / `cached` the target sees. At 10¹⁰/4 over OSHMEM on aac6 the `cached` form
  needs 8.6 GB — above the 8 GiB default pool, which is presumably why S ran 10⁸ and 10⁹ only.
* The general map's exchange overlap (`GEN_HIDE` = ½) and its one-in-flight pipeline: an assumption until L's open
  issue (two in flight on the v-exchange) or a measurement on the target; at 576 every product over the full group is
  on the general map (576 is not a power of two), so it is 20–30 % of the exposed time.
* `PHASES[1e11]`: M11 v4's log (init 25.9, bs 117.0 = seeds 10.9 + batch 54.6 + mdev 51.4, dm 119.8); the seeds appear
  in bs at that size — the interpolation above 8 × 10¹⁰ uses it as the top level's share.
* The 9-way and 3·3 schedules assume the fold (`tree_level`'s form for k = 2 applied k − 1 times); a pairwise
  combine inside the level (2 + 2 + 2 + 2 + 1 → …) would cost ≈ 25 % fewer plane points at the 9-way level — G's
  choice; the model's `tree_cost` is one function to change.
* The per-node spread of the aac6 nodes (s24-30 50 % slower at the large sizes) is in the 10¹⁰/4 calibration point's
  ±8 s; the model takes the mean.
