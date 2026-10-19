# TIME_MODEL — the time model and the 576-node estimates

Scope: the time model and the 576-node estimates: refits to measured runs, scaling studies, the model's inputs measured on aac7. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| Q | the target plan: the models completed for 576 nodes, `estimate(g, D)`, the runbook (Phase 12, PLAN.md §27 row Q; 2026-09 |
| X | cross-fabric traffic and the 576-node model (Phase 11, PLAN.md §26 row X) |
| L4 | refit of the time model to all of 2026-10-11's multi-node aac7 runs; what it got wrong and why (wave L, agent L4) |
| T06 | layeq / layv rate in the time model, and the 576-node estimate redone |
| M2a | time-model refit, scaling study, 1- and 2-node points (aac7 queue items 77 / 78) |
| S48 | scaling study (1/2/4/6 nodes at 6.441e10 digits per node) and refit of the aac7 model profile |
| SCALE35 | scaling points at 3 and 5 nodes |
| S19B | profile analysis of S19A (2 → 10 nodes) and the S19C switch A/B, with a 576-node projection |
| D213d | D2 13d — the model: steps for G, the 13c recalibration, the 576 piece counts, the estimate (Phase 13d, PLAN.md §32 row D |
| C16 | Phase 16 C: the model's inputs measured on aac7 (Slingshot-11, Cray OpenSHMEMX 11.8.0) and `mn_model.py` recalibrated |
| F16 | the 576-node figures re-estimated from the inputs measured on aac7 (Phase 16 C), with bands |
| TARGET_HW_REVIEW | the target's microbenchmark report against the model (desk review, 2026-10-03, 22:52 EDT) |

## Q — the target plan: the models completed for 576 nodes, `estimate(g, D)`, the runbook (Phase 12, PLAN.md §27 row Q; 2026-09-21)

(former file: Q.md)

Branch `q12` (from `main` @ 7aded87). Files: `ecalc/mn_model.py` (X's fabric model, extended), `ecalc/mem_model.py`
(M's memory model, extended), NEW `ecalc/estimate.py`, NEW internal target notes. Nothing outside my files. No node time
used (the calibration is against the recorded walls; the phase splits were read from the earlier batches' logs on
aac6: `~/l11/out{1,2}`, `~/x11/b{1,2}`, `~/ntt-m11/ecalc/results/m11`, `~/ntt-s/ecalc/results/mnaccept/20817`).

#### HEADLINE (the standing rule, PLAN §26) — 576 nodes

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

Two findings that change the target plan (both in internal target notes §8, traps 6 and 11):

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

#### 1. L's real schedule in the model (`mn_model.tree_cost`, `mem_model.level_children`)

`mem_model.mn_groups(g, spec)` is `mn_groups_parse` in Python (the same rules: increasing, multiples, anything ≥ g
ends the list at g; default the powers of two then g). `level_children(g, spec)` gives per level the group size S and
its children — the previous level's groups `[k P, min((k+1) P, S))` — so the default at 576 is ten levels
`2, 4, …, 512, 576` whose top joins a 512-group and a 64-group, `2,4,8,16,32,64,576` is a 9-way level of nine 64-groups,
`…,64,192,576` two 3-way levels. A k-way level is costed as the tree forms it (results/GRID_GROUPS.md §L: "k − 1 products over
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

#### 2. The memory model at g in both forms (`mem_model.py --tree`/`form`)

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

#### 3. `estimate(g, D)` — `ecalc/estimate.py`

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

#### 4. Calibration against every recorded aac6 point (`./mn_model.py --calib`)

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

#### 5. internal target notes — the runbook

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

#### 6. Tests run

No node jobs. `python3 mn_model.py --calib` (16/16 within 10 %, worst 6.1 %), `python3 mn_model.py --schedules`,
`python3 mn_model.py` (the full tables, ≈ 40 s), `python3 mem_model.py` (the size-1 calibration table unchanged: 4 / 7 /
8 × 10¹⁰ within 1 %), `python3 estimate.py` / `--max` / `--as-is` / `--staging per_exchange` / `--verbose` / `--lat 2e-5`
/ `--bw 50` / `--layers 3` / `--groups 2,4,8,16,32,64,192,576` — all run; the numbers above are their output.

#### 7. Open issues

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

## X — cross-fabric traffic and the 576-node model (Phase 11, PLAN.md §26 row X)

(former file: X.md)

Branch `x11` (from `main` @ 72aa2e9). Files: `ecalc/mn_model.py` (new), `ecalc/newton_db.c` (`recip_mn`, `newton_mn_divmod`,
`mdb_shift_g`), nothing else touched. Clone `~/ntt-x` on aac6; batches `~/x_b1.sh` (calibration, job 20802, s24-26),
`~/x_b2.sh` (the X1 gates); logs `~/x11/b1/`, `~/x11/b2/`.

#### X2 — the fabric model (`mn_model.py`)

`./mn_model.py` prints, for D ∈ {4e10, 8e10, 1e11} digits per node and g ∈ {4, 64, 576} nodes, the per-node wall by phase,
the bytes on the fabric (per node, per NIC, on the dragonfly's global links), the exposed communication, the memory per
node and the total digits; `--calib` reproduces the aac6 multi-process runs; `--group G --layers 3 --taper t --lat s
--bw GB/s --write-bw GB/s --rule full` vary the target's parameters. The script is the document: every constant is in
the table at its top with its RESULTS section.

##### Inputs (measured)

| item | value | where |
|---|---|---|
| single-node phases at 1e9, 1e10 (this session), 4e10, 7e10, 8e10: init / batch / top levels / reciprocal / division | 9.3/1.5/0.1/2.8/0.4 · 10.8/7.1/1.1/4.8/3.0 · 16.4/23.0/13.1/15.2/14.9 · 22.2/39.5/25.7/35.9/36.6 · 25.1/46.6/38.8/49.6/49.5 s | job 20802; RESULTS §75, §74, results/MEMORY.md §M |
| the 2^31-point piece product (4 primes, 2^29 points per APU) | 1.11 s (load 0.33, ntt 0.67, crt/out 0.11); 0.72 s with the B operand's transform cached | results/GRID_GROUPS.md §G |
| the four-step transform's parts at 2^31 over the four APUs (per three transforms of one prime) | rows 0.0624, cols 0.0429, packs 0.0385, xGMI exchange 0.0571 s, exposed 0.0089 (84 % hidden) | results/EXCHANGE.md §C (DIST_STATS) |
| memory per node (size 1) | planes 120.3 GB; regions 2.3 GB and the dm pool's peak live 2.9 GB per 10⁹ digits; host 3.1 + 0.215 GB per 10⁹ | results/MEMORY.md §M, RESULTS §75 |
| the target | 576 nodes, 100 GB/s per APU (two 400 Gb/s NICs), dragonfly groups of `MN_TOPO_GROUP` = 64 nodes, ~2 µs per message, the global tier limited by the group's aggregate injection to each peer group (taper 1.0) | PLAN §25 |

##### The model

Phases per node at size g and D digits per node: init(D), batch(D), top levels(D) from the single-node table
(log-log interpolation; above 8e10 extrapolated on the 7e10–8e10 slope); then every distributed product is costed the
way the code forms it — the tree's levels over groups 2, 4, …, 64 and the two 3-way steps to 192 and 576 (L's
C3-576 schedule; a 3-way level = 4 products: Q₂Q₃, Q₁(Q₂Q₃), P₂Q₃+P₃, P₁(Q₂Q₃)+…), the reciprocal's anchored chain
(k, ⌈k/2⌉, … to 2¹⁶; per step Q_t·r and r·d, six shifts, two adds, the small collectives) and the division's A_h·μ
(low cut) and X·Q (high cut), each product as `split_grid_cap`'s grid at the group's plane cap 2^(31+log₂ gt) with the
2-slot transform cache over shares (B's piece held across A's pieces, A's piece 0 across B's).

A piece of `pts` points over g' nodes (q = pts/(4 g') points per APU): local = 1.11 s × q/2²⁹ (the measured piece, its
xGMI exchanges included) plus, per transform (EC_NP × (fwd + inv) = 12 layered all-to-alls of 8q bytes per APU),
the fabric stage's excess over the xGMI stage it runs beside (`comm_layered` keeps two exchanges in flight, so
exposed = max(0, t_fabric − t_xGMI); t_xGMI = 0.019 s × q/2²⁹); the operand redistributions and the result exchange
(plain all-to-alls of the operands' bytes, exposed); three small collectives. An all-to-all over g' nodes with B bytes
per APU: two-layer (APU × node) — B (g'−1)/g' on the NIC, K (g'−1) messages per APU (K = 4 slab chunks);
three-layer (APU × node-in-group × group, g' > G) — B ((G−1) + 3 (g'−G))/g' on the NIC (the cross-group data is
relayed in and out of the group), K (2 (G−1) + g'/G − 1) messages; the bytes that cross a global link are
B (g'−G)/g' either way, timed against the group's aggregate injection to each peer group × taper.

Memory per node at the dm peak = planes 120.3 + max(regions, dm pool)(D) + the distributed scratch (4 q per APU at the
cap plane = 69 GB, M3) + the shift buffers (2 × share/4 per APU with `alltoallv` — L's B7; g × that padded, as today)
+ host. The transform cache over shares (2 × 16 GiB per APU) is counted as 0 (the group takes what the pool allows).

##### Calibration on aac6 (`--calib`; g node-processes sharing one node over loopback TCP, job 20802, s24-26)

The substitutions: the loopback transport at 3.3 GB/s per node-process (g = 2; its single stream per mesh is slower
below 1 GiB per APU, exponent 0.3) / 3.7 GB/s (g = 4, exponent 0.1) — fitted on the products' `dist_mn` lines
(2³¹ over 2 × 4 ranks 15.7–16.6 s, over 4 × 4 ranks 9.4–11.2 s; 2²⁶ 1.2 / 0.55 s) — 1 ms fixed per chunk exchange, the
local passes g × slower (the processes share the four APUs), the leaves at 0.5 × the single-node bs of the total digits
(measured: four 2.5 × 10⁹-digit leaves take 3.9 s beside bs(10¹⁰) = 8.2 s), init as measured (a shared-node artefact).

| digits | g | measured wall | modelled | error | measured bs+tree / dm | modelled leaf+tree / dm |
|---|---|---|---|---|---|---|
| 10⁹ | 2 | 27.7 s | 28.1 | +2 % | 3.8 / 18.0 | 3.7 / 18.5 |
| 10⁹ | 4 | 19.7 s | 19.3 | −2 % | 3.8 / 10.6 | 3.1 / 11.0 |
| 10¹⁰ | 4 (POOL_LOG 29) | 110.1 s | 98.5 | −11 % | 32.6 / 68.9 | 27.4 / 62.5 |
| 10¹⁰ | 2 (POOL_LOG 30) | 146.4 s | 131.1 | −10 % | 26.8 / 105.2 | 23.1 / 93.7 |

(size 1 for reference: 10⁹ 14.4 s, 10¹⁰ 29.0 s, identical to the references.) Within the 20 % gate; the model is
10 % optimistic at 10¹⁰ — the TCP transport's mid-size exchanges (2²⁸–2³⁰) run at 60–80 % of its large-exchange rate.

##### The target tables (`./mn_model.py`, defaults: G = 64, two layers, taper 1, 2 µs, part files at 2 GB/s per node)

Per-node wall (s) = init + batch + top levels + distributed levels + reciprocal + division + the part file's exposed
tail; "exposed" = the communication not hidden by the pipeline; NIC = bytes through the node's eight NICs (per NIC =
1/8); global = bytes over the dragonfly's global links per node.

| D per node | g | total digits | wall | init | batch | top | dist. levels | recip | division | output | exposed | NIC / global per node | node GB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4e10 | 4 | 1.6e11 | 99.2 s (1.7 min) | 16.4 | 23.0 | 13.1 | 8.4 | 11.6 | 13.3 | 13.3 | 5.6 (6 %) | 4.3 TB / 0 | 352 |
| 4e10 | 64 | 2.56e12 | 120.8 (2.0) | 16.4 | 23.0 | 13.1 | 27.7 | 13.2 | 14.9 | 12.6 | 13.1 (11 %) | 8.4 TB / 0 | 352 |
| 4e10 | 576 | 2.30e13 | 138.9 (2.3) | 16.4 | 23.0 | 13.1 | 47.6 | 13.3 | 11.0 | 14.5 | 18.4 (13 %) | 10.7 TB / 5.5 TB | 352 |
| 8e10 | 4 | 3.2e11 | 218.8 (3.6) | 25.1 | 46.6 | 38.8 | 21.6 | 26.7 | 39.8 | 20.1 | — | — | 512 ✗ |
| 8e10 | 64 | 5.12e12 | 274.4 (4.6) | 25.1 | 46.6 | 38.8 | 71.7 | 30.0 | 44.4 | 17.8 | 34.6 (13 %) | 22.6 TB / 0 | 512 ✗ |
| 8e10 | 576 | 4.61e13 | 317.6 (5.3) | 25.1 | 46.6 | 38.8 | 127.4 | 24.3 | 30.8 | 24.6 | 46.3 (15 %) | 28.1 TB / 14.4 TB | 512 ✗ |
| 1e11 | 4 | 4.0e11 | 311.2 (5.2) | 30.8 | 61.4 | 77.2 | 28.5 | 34.0 | 58.4 | 20.8 | 20.5 (7 %) | 16.4 TB / 0 | 593 ✗ |
| 1e11 | 64 | 6.4e12 | 384.7 (6.4) | 30.8 | 61.4 | 77.2 | 94.5 | 38.2 | 65.1 | 17.5 | 47.8 (12 %) | 31.1 TB / 0 | 593 ✗ |
| 1e11 | 576 | 5.76e13 | 446.1 (7.4) | 30.8 | 61.4 | 77.2 | 168.0 | 36.1 | 45.1 | 27.5 | 63.7 (14 %) | 38.7 TB / 20.2 TB | 593 ✗ |

(✗: does not fit 502 GB by the memory model — the size-1 profile plus the distributed scratch and the shift buffers;
the size-1 run fits 8e10 at 393 GB because it has neither.)

**Where the time is.** The fabric is not the bottleneck: at 576 nodes the exposed communication is 13–15 % of the wall
at every D (the transforms' exchanges are 84 % hidden behind the row passes on xGMI and the fabric stage at 100 GB/s
per APU costs about what the xGMI stage costs, as PLAN §25 anticipated; what stays exposed is the operand
redistributions and the result exchange of every piece, ≈ 0.1 s per 2³¹-point piece, and the latency term of the
small products). The distributed levels are: at 576 nodes there are 8 of them (2, 4, …, 64, then 3-way to 192 and
576) and each costs every node the same local work — two products of a full plane per APU — so they are 34 % of the
wall at 4e10 and 40 % at 8e10, against the fabric's 13 %. Their cost steps with the grid: at D per node just below a
power of two of limbs (n_Q ≤ 2³¹: D ≤ 3.86e10) each level product is ONE piece; above it two, then four, then six:

| D per node | total digits (576) | wall | dist. levels | pieces per level product | node GB |
|---|---|---|---|---|---|
| 2.0e10 | 1.15e13 | 1.3 min | 27 s | 1 | 272 |
| 3.0e10 | 1.73e13 | 1.7 | 31 | 1 | 312 |
| **3.8e10** | 2.19e13 | **2.0** | 33 | 1 | 344 |
| 4.0e10 | 2.30e13 | 2.3 | 48 | 2 | 352 |
| 5.0e10 | 2.88e13 | 2.8 | 50 | 2 | 392 |
| 6.0e10 | 3.46e13 | 3.6 | 84 | 4 | 432 |
| 7.0e10 | 4.03e13 | 4.2 | 94 | 4 | 472 |
| **7.7e10** | **4.44e13** | **4.6** | 96 | 4 | 500 |
| 8.0e10 | 4.61e13 | 5.3 | 127 | 6 | 512 (does not fit) |
| 1.0e11 | 5.76e13 | 7.4 | 168 | 8 | 593 (does not fit) |

**The global tier.** With the layered (two-layer) all-to-all, every exchange over a group larger than the dragonfly
group sends (g'−G)/g' of its bytes over global links: 5.5 TB per node at 4e10 (of 10.7 TB on the NICs), 14.4 of
28.1 TB at 8e10 — the top two tree levels, the reciprocal's last steps and the division. Under the uniform all-to-all
the group's aggregate injection to each peer group is spent evenly, so with taper 1 the global tier is never the
limit; at taper 0.5 with 10 µs messages and 1 GB/s part files (`--taper 0.5 --lat 1e-5 --write-bw 1`) the 576-node
walls become 2.9 / 6.6 min at 4e10 / 8e10 (exposed 36 / 89 s) — still the levels' local work first.

**What the third layer buys** (`--layers 3`, G = 64): messages per APU at 4e10 × 576 fall from 925 k to 322 k, but the
cross-group data is relayed twice inside the groups, so the NIC bytes double (10.7 → 21.6 TB) and the exposed
communication goes 18.4 → 44.7 s (wall 139 → 161 s). The third layer pays only for exchanges below ≈ 200 MB per APU
(latency-bound: the reciprocal's early steps), which X1 sends to small groups instead — so at 576 nodes the two-layer
form with X1 is the better pair; the third layer is worth having only if the message rate, not the bandwidth, turns
out to be the limit of the real fabric (a per-message cost above ≈ 15 µs makes it win at 4e10).

**X1 in the model**: the reciprocal at 4e10 × 576 on the model's groups 13.3 s vs 16.0 s with every product on the
full group (messages per APU 548 k vs 1.53 M; exposed 4.5 vs 6.8 s); at 64 nodes 13.2 vs 13.3 s (nothing: 64 nodes'
latency term is small). A product on the full 576 costs at least a 2³² plane (`logmin = 2 (7 + log₂ gt)`) and 15
exchanges of 575 messages per APU ≈ 0.2 s however small it is; on two nodes the same step costs 10 ms.

##### HEADLINE (576 nodes; modelled from the measured per-node profile; extrapolated where marked)

* **The largest digit count: 576 × 7.7 × 10¹⁰ = 4.4 × 10¹³ digits in ≈ 4.6 min per-node wall** (4.2 min without the
  part file's exposed tail) — the largest D per node that fits the memory model (500 of 502 GB: planes 120 + the dm
  pool 223 + distributed scratch 69 + shift buffers 68 + host 20) and just under 2³² limbs of Q, where the levels'
  products are 2 × 2 grids (at 8e10 they are 2 × 3: 4.61 × 10¹³ digits in 5.3 min, but 512 GB — does not fit
  without M's memory work or a smaller distributed scratch). Assumptions: the single-node phases as measured (7e10 /
  8e10 interpolated: measured), the fabric at 100 GB/s per APU and 2 µs per message with the two-layer all-to-all
  (modelled), L's block-cyclic map for the 3-way levels (modelled as 4 products each), B7's `alltoallv` shifts
  (required: the padded buffers are 39 TB per node at 576), the part files at 2 GB/s per node (assumed).
* The safe per-node size: **576 × 3.8 × 10¹⁰ = 2.2 × 10¹³ digits in 2.0 min** (344 GB per node; every level product
  one piece) — measured single-node profile, modelled fabric.
* 576 × 4 × 10¹⁰ = 2.3 × 10¹³ digits in 2.3 min; 576 × 8 × 10¹⁰ = 4.6 × 10¹³ in 5.3 min (memory-limited, see above).

#### X1 — the group per product size in the reciprocal and the division (`newton_db.c`)

`recip_mn`: each sharded step j → jn runs on the smallest prefix group [0, 2^L) of the full group G whose modelled
cost is least (`x1_level`: the pieces at the group's cap × (local 1.11 s × q/2²⁹ × the APU sharing + 15 exchanges of
8q (g'−1)/g' bytes at `NEWTON_MN_BW` GB/s per APU with 4 (g'−1) messages of `NEWTON_MN_LAT` s and `NEWTON_MN_FIXED`);
defaults = the target fabric (100 GB/s, 2 µs, 0); `MN_MODEL_TCP=1` = aac6's loopback (0.8 GB/s per APU thread, 1 ms
per exchange, the APUs shared size-way); `NEWTON_MN_GROUPS=0` = every product on G as before). The groups are the
tree's (`mn_group_at(L)`, created by `mn_tree` on every node — no new meshes; node 0's group at level L is [0, 2^L)).
Mechanics: r lives on the current step's group; Q stays on G and Q_t is cut out of it by one exchange over G into the
step's group (`mdb_shift_g`: `mdb_shift` with the target group's sharding — every node of G takes part, the pieces of
nodes outside the target are empty) once per step instead of once per repeat; the step (both products, the shifts, the
adds, the compares, the overshoot path) runs on the group's members only, the other nodes go to the next step's
Q_t exchange and wait there; when the next step's group is larger, r is re-sharded onto it (`x1_regroup`: r's
descriptor from node 0 by an all-gather over the new group, one exchange); after the last step μ is re-sharded onto G.
`newton_mn_divmod`: the two products go through the same rule with the re-sharding of A, B and C counted
(`mn_prod_cut_x1`) — on the target's sizes they always take G (they are ≥ 2 n_Q points); on aac6 they take a subgroup
only at 10⁸ with the TCP constants.

The digits cannot depend on the choice (every sharded operation is exact; the products are the same integers over any
group); size 1 does not enter this code.

##### Measurements (batch 3 = `~/x_b2.sh`, job 20815, s24-26, one node, the node-processes sharing it; every run's part files
concatenated and `cmp`'d against `ref/e_<digits>.txt` / `results/e_1e10.out`, VERIFY OK on every node)

| run | env | groups taken by the reciprocal | digits | wall |
|---|---|---|---|---|
| 10⁹ size 1 | — | (size 1: none of this code) | identical | 13.7 s |
| 10⁸ size 2 | `POOL_LOG=27` | full (g = 2 has no subgroup) | identical | 8.5 |
| 10⁸ size 3 | `POOL_LOG=27 MN_MODEL_TCP=1` | full | identical | 9.0 |
| **10⁸ size 4** | `POOL_LOG=27` (target constants) | **[0, 2) for j ≤ 3.5 × 10⁵, then the four** | identical | 8.4 |
| 10⁸ size 4 | `MN_MODEL_TCP=1` | full | identical | 8.4 |
| 10⁸ size 2 | `NEWTON_MN_GROUPS=0` | full (the switch off) | identical | 8.1 |
| 10⁹ size 2 | `POOL_LOG=29` | full | identical | 27.3 |
| **10⁹ size 4** | `POOL_LOG=29` (target constants) | **[0, 2) for j ≤ 4.3 × 10⁵ (4 steps), then the four** | identical | 20.6 |
| 10⁹ size 4 | `MN_MODEL_TCP=1` | full | identical | 20.2 |
| 10¹⁰ size 4 | `POOL_LOG=29 NEWTON_MN_GROUPS=0` (X1 off) | full | identical | **114.6** (dm 73.6, recip 45.1) |
| **10¹⁰ size 4** | `POOL_LOG=29` (target constants) | **[0, 2) for j ≤ 5.4 × 10⁵ (4 steps, 0.55 s together), then the four** | identical | **123.0** (dm 78.4, recip 48.2) |
| **10¹⁰ size 4** | `POOL_LOG=29 MN_MODEL_TCP=1` (aac6's constants: the rule keeps the full group, as the model says it should on a shared node over loopback) | full | identical | **100.6** (dm 60.1, recip 33.4) |

The same 10¹⁰/4 configuration without X1 ran 110.1 s (job 20802) and 105.7 s (job 20809): the node's spread is
± 5 s, and the four subgroup steps cost 0.55 s in the X1 run against 0.57 s for the same steps on the full group (the
loopback transport has no latency term to save, and the local passes on two node-processes instead of four are no
cheaper on a shared node). So on aac6 X1 with the target's constants is neutral on the steps it changes and the wall
difference is noise; with aac6's own constants (`MN_MODEL_TCP=1`, which the calibration says is the rule for this
machine) the rule leaves every product on the full group and the wall is 100.6 s. The first version of the batch
(job 20809) found the bug the size-4 runs exist for: a non-member's `mn_group_at(L)` is its own group [2, 4), so its
pieces of Q_t went to the wrong nodes and the step repeated without end — the target group is now named as
[0, 2^L) on every node (`mdb_shift_g(…, tg0, tg)`).

#### X3 — the transform cache's reach

Not started (the time went to the model's calibration and the X1 gate); the mechanism (results/GRID_GROUPS.md §G: `rns_dist_cache_hold`
pins Q's pieces from the reciprocal's top step to the division's X Q; `RNS_DIST_CACHE_MN` slots as the pool allows) is in
place and off by default — what X3 would add is a per-product slot budget from the pool's free bytes (M's accounting)
so that A_h μ's pieces do not evict Q's.

#### Open issues

* The model's distributed levels assume L's block-cyclic map spreads a 3-way level over all 3m nodes with the
  per-rank points of a binary level; the 9-way top as two 3-way levels costs 4 products each (Q₂Q₃ shared) — 8
  products against 4 for two padded binary levels, at 2/3 the points per rank each; the schedule "…, 64, 576" with
  one 9-way step would be different again. The tables use 192, 576.
* The memory at size > 1 is the size-1 profile plus 69 GB of distributed scratch (M3's 4 q per APU) and the shift
  buffers; M's per-node accounting at size > 1 should replace these two lines (they decide 7.7e10 vs 8e10 per node).
* The per-message cost of a SHMEM put with signal from a host thread is not 2 µs on every implementation; `--lat`
  moves it, and at ≥ 15 µs the third layer starts to pay at 576 (the reciprocal's early steps are X1's already).
* The part file: at size > 1 the `dc` tail is exposed on aac6 (2.4 s at 10¹⁰/4, 5.0 s at 10¹⁰/2: the writer to /tmp),
  hidden at size 1; the model charges max(0, D/write_bw − half the division) per node — the target's file system
  decides.
* X3 (the transform cache's reach across the division's products) is not started.

## L4: refit of the time model to all of 2026-10-11's multi-node aac7 runs; what it got wrong and why (wave L, agent L4)

(former file: L4.md)

Date 2026-10-11 (Eastern). Base main a1569d5, branch `l4` (worktree `../ntt-wt-l4`). CPU only: local analysis plus the aac6 MI250 CPU nodes for the schedule search; no aac7 job (ssh reads of logs only).
Labels: **measured** = parsed from run logs (`total`, stage times, xstats); **modelled** = `ecalc/mn_model.py` / `estimate.py` / statistics on measured values; **assumed** = the target fabric (100 GB/s per APU, 2 us) and the part-file rate, as in every standing estimate.
Files: `ecalc/mn_model.py`, `ecalc/estimate.py`, `ecalc/refit_s48.py` (`--l4`), `ecalc/aac7_l4_fit.json` (new), `tools/groups_table.py` (`GT_PROFILE`, `GT_RELEASE_GB`, `GT_LAYV`), `archive:drivers/l4/l4_search.sh`, this file. The run table is `archive:results-raw/L4_runs.tsv` (148 rows); raw refit output `archive:results-raw/L4_refit.out`; schedule searches and plan checks `archive:results-raw/L4_groups/`.

#### 0. Answer in short
1. **One bookkeeping bug and one mis-fitted rate parameter explain the misses; a third effect (the 6-node default's variance) is not modelled.**
   - **Bug (fixed; affects every uneven MN_GROUPS result):** a tree level was costed through its *last* (clipped, smaller) group. The real level ends when its slowest group ends, and with an uneven last group that is a full-size group. C09's "model-best" forms (`2,4,8,16,80,560,576`: 146.5 s) rest on this: costed at the slowest group the same form is 196.2 s, 12.5 s *worse* than the standing default `2,4,8,16,32,64,192,576` (183.7 s). Schedules whose levels divide the node count (all of B04's, the 576 default) are unchanged (outputs bit-identical). Old behaviour behind `MN_MODEL_TREE_TOPONLY=1`.
   - **Rate (refit):** the layv factor 0.655 was fitted on the 6-node default `2,6` alone. Fitted on all non-power-of-2 classes (3, 5, 6 nodes, 7 schedules) it is **0.92** (assumes the unknown NIC launch group is the fast one B; 0.73 / 1.0 move the 576 estimate by +-3 s), and the bulk rate depends on the NIC launch group: **10.63 GB/s per APU thread in the fast group B, x0.818 in the slow group A** (S48's single 10.31 was a mix of the two modes).
   - **Not modelled:** the 6-node default `[2,6]` is slower on average (256-277 s vs 243 s for `[6]`) and far more variable (223-336 s, sd 22 s vs 10 s) than the model says (240 s, 9 s *faster* than `[6]`); the per-node timers show one 2-node group of three occasionally taking 2x as long (an expected maximum over groups). Unidentified with 5-7 runs per class.
2. **Residuals** (model vs measured total, rms of log ratio over 12 run classes): before 0.156, after **0.062** (group B for classes of unknown group). Power-of-2 classes -11..+5 % -> -3..+3 % (mode-adjusted); 3 nodes +18 % -> -3 %; 5 nodes +7..+40 % -> +5..+12 %; 6 nodes `[6]` +27 % -> +2 %, `[3,6]` +17 % -> -6 %, but `[2,6]` +6 % -> -13 %. Per-class table in 3.1.
3. **576 nodes (modelled, fabric assumed, T2048, no part file):** the standing default `2,4,8,16,32,64,192,576` moves 183.7 -> **176.3 s** at 3.71e13 and 189.5 -> **181.7 s** at 3.82e13 (layv 0.655 -> 0.919); with RELEASE the margin at 3.82e13 is 13.0 GB (was 4.4). The best explicit schedule at 576 is the standing form within 0.2 s: C09's 13-21 % "model-best" gain (`80/560` forms) and its 19-28 s "round down to 569" advice were artefacts of the level bug. For N < 576 explicit forms with balanced tops (`2,4,8,40,200,N`, `5,35,140,N`, ...) are 3-25 s better than the standing form `2,..,64,192,N`; the code's default schedule is 8 s to 2000 s worse at almost every N (section 4).

#### 1. One table of all multi-node runs (calib_table.py --keep-stalled)
Command: `python3 -I ecalc/calib_table.py $(find <s48 b06 sess1011c batch1011 s47 logs> -name '*.log') --keep-stalled --tsv calib_all.tsv` (about 580 logs read; 148 multi-node rows at 6.441e10 digits/node kept in `archive:results-raw/L4_runs.tsv`, including f3's single-level pairs collected 20:55 EDT, with stage times, r, mode, flags, xstats GB per node). Class means (clean rows = no cold start; stalled runs kept, so the 4 n means include chain-spike runs: chain-free fast mean 181.5 s, slow 208.0 s); seconds, measured:

<!--CLASS-->
| nodes | MN_GROUPS | mode | clean runs | total | init | bs | tree | recip | dm | r GB/s | GB/node bs / dm / recip (xstats) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | default | fast | 2 | 138.9 | 26.9 | 49.0 | 18.5 | 17.4 | 58.6 | 14.0 | 629 / 1560 / 549 |
| 4 | 4 | fast | 2 | 191.9 | 33.5 | 69.2 | 41.4 | 27.6 | 83.7 | 13.1 | 1447 / 2324 / 816 |
| 4 | 4 | mid | 1 | 205.7 | 34.0 | 71.5 | 41.6 | 32.0 | 96.8 | 12.0 | 1447 / 2324 / 816 |
| 4 | 4 | slow | 2 | 209.3 | 32.4 | 75.8 | 46.6 | 29.6 | 97.3 | 11.1 | 1447 / 2324 / 816 |
| 4 | default | fast | 78 | 188.5 | 30.3 | 71.3 | 42.2 | 27.5 | 82.7 | 13.7 | 1541 / 2324 / 816 |
| 4 | default | mid | 1 | 207.7 | 34.5 | 74.8 | 45.7 | 31.6 | 94.0 | 12.1 | 1541 / 2324 / 816 |
| 4 | default | slow | 27 | 211.4 | 30.8 | 77.1 | 48.4 | 29.8 | 99.1 | 10.9 | 1541 / 2324 / 816 |
| 5 | 2,5 | - | 1 | 226.2 | 25.8 | 96.1 | 61.4 | 32.4 | 100.4 | 9.6 | - |
| 5 | 4,5 | - | 1 | 232.2 | 25.5 | 98.7 | 61.3 | 33.0 | 104.0 | 9.6 | - |
| 5 | default | - | 1 | 213.2 | 32.9 | 78.9 | 47.6 | 31.3 | 96.3 | 10.2 | - |
| 6 | 3,6 | - | 2 | 264.0 | 29.0 | 105.8 | 76.1 | 32.7 | 125.0 | 8.3 | 2492 / 3267 / 941 |
| 6 | 6 | - | 5 | 243.3 | 28.0 | 93.0 | 61.9 | 35.6 | 117.6 | 9.2 | 2204 / 3096 / 941 |
| 6 | default | - | 6 | 273.9 | 28.9 | 107.5 | 77.6 | 45.5 | 132.4 | 9.2 | 2250 / 3267 / 941 |
<!--/CLASS-->

Not in the table (cold start, init >= 36 s, or no rate): 3 n `[3]` 210.2 s (r 10.46, init 41.8; B06) and the single-6-node A arm 304.4 s (init 56.5); they enter the fit cold-adjusted. 1 node: 75.5 s (44 runs, init 13.5 s).

Reading (measured): (a) at 4 n the NIC launch group is the whole story of the bimodality (T09, A13): same bytes, rate x0.8 (fast 13.7 / slow 10.9 GB/s per thread). (b) **Bytes per node follow the model within 5-20 %** (section 2), so the model's errors at 3/5/6 n are rates and structure, not volumes. (c) Per stage, time = local compute + bytes / (4 r) holds for dm and tree at every size (dm x r = 1000-1060 for 3..6 n, 818 at 2 n), i.e. the 6-node dm is *not* slower per byte than the 4-node dm once r is the run's own rate.

#### 2. Diagnosis
**2.1 Bytes per node, model vs measured (GB per node; measured = sum of xstats kinds, S48/C09/f3 logs).**

| class | tree: model / measured (bs) | recip: model / measured | dm division: model / measured (dm) |
|---|---|---|---|
| 2 n `[2]` | 512 / 629 | 520 / 549 | 1581 / 1560 |
| 4 n `[2,4]` | 1283 / 1541 | 783 / 816 | 2371 / 2324 |
| 4 n `[4]` | 1502 / 1447 | 783 / 816 | 2371 / 2324 |
| 6 n `[2,6]` | 2214 / 2250 | 878 / 941 | 3188 / 3267 |
| 6 n `[6]` | 2527 / 2204 | 878 / 941 | 3188 / 3096 |
| 6 n `[3,6]` | 2499 / 2492 | 878 / 941 | 3188 / 3267 |
| 3 n, 5 n | no xstats in the B06 logs (model: 3 n tree 1359, 5 n `[5]` 1944, `[2,5]` 1826, `[4,5]` 2161) | | |

The measured `bs` also holds the leaf's redistributions; the model's tree is the levels only. The 2-node level is under-counted by about 20 % (operand redist / result / spill kinds), the 6-way single level over-counted by 15 %.

**2.2 Critical path of a level (the bug).** `tree_cost` used `level_top_group(..., 'top')`: the group holding the last node. For `MN_GROUPS=4,5` that is a 1-node group (cost 0) and level 4 cost nothing; for `2,4,8,16,80,560,576` the level of 80 was costed as a 16-node group and the level of 560 as one child. Measured per-node level timers (`mn: node N level L [..) in X s`): `4,5` level 1 = 42.9-45.7 s on nodes 0-3 and level 2 = 15.6 s on every node, while node 4 (not in a 4-node group) shows 57.0 s for level 2 because it waits for the 4-node level first (critical path = the full group, then the top level; tree 61.3 s measured). Fix: a level costs `max(top group, last full group)` (`mn_model.tree_cost`, `TREE_CRIT`). Changes nothing when S divides g.
**2.3 Rate per kind (xstats, measured, GB/s per APU thread = GB / t_post).** layeq 16.0 / 18.4 (2 / 4 n, fast); layv 8.2-9.3 at 6 n (t_post double counts the INTER2 overlap, C02: span rate 13.3-15.7); the other kinds are slow and fall with group size: redist 8.9 / 6.0 / 4.6, result 10.6 / 8.9 / 7.0 (2 / 4 / 6 n), addsh 9-10; they carry 29-38 % of the exchange time but 15-20 % of the bytes. A `k_other` rate factor for them (new hook in `Fabric`, default 1.0) was tested at 1.0, 0.8, 0.6, 0.45 with bw refit each time (on the first, smaller class set): the totals' rms is 0.124-0.126 for all of them (no information in the totals), so it stays 1.0.
**2.4 Per-exchange fixed cost.** The per-message latency moves the walls < 0.5 % over 2-68 us (S48); not identifiable from totals; unchanged (8.5 us).
**2.5 Level structure vs measured tree time x r** (tree volume in GB/s x s, measured; the tree timer also holds up to ~10 s of waiting for the slowest leaf, so read differences, not levels):
single level `[3]` 492, `[4]` 505-542, `[5]` 488, `[6]` 551-615; two levels `[2,4]` 528-578, `[2,5]` 590, `[4,5]` 587, `[3,6]` 630, `[2,6]` 590-855. A single k-way level costs about the same for k = 3..6 (the model's fold grows it with k; its volume is within 15 %). Rate-adjusted, the f3 pairs (section 5) give **`[2,6]` - `[6]` = +22.7 s, SE 11.8** at 6 n and no difference at 4 n; the model says `[2,6]` is 9 s *faster*. At 5 nodes the raw gaps (`[2,5]` +13 s, `[4,5]` +19 s vs `[5]`) are inside what the rates alone explain (B06: 9.6 vs 10.3 GB/s), so the 5-node order is **not established** (one run per schedule) and the model's flat order is not contradicted. Per-node timers show where `[2,6]` loses time: level 1 is three independent 2-node products whose times differ by up to 2x within a run (14.7 / 30.2 / 18.1 s; 14.1 / 16.3 / 51.7 s in C09 r1_A) and level 2 waits for the slowest pair: the cost is an expected maximum over groups, not a mean. That term is **not modelled** (unidentified, 3.3).

#### 3. Refit (`refit_s48.py --l4 --write-profile` -> `ecalc/aac7_l4_fit.json`)
Fit (modelled on the measured totals; 12 classes, each weighted equally, runs of a class averaged): `bw_apu` (group B) from the three pow2 fast classes (2 n, 4 n `[2,4]`, 4 n `[4]`) by mean-log residual; `alpha` = A/B from the two slow classes; layv factor by mean-log residual over the seven non-pow2 classes priced as group B (a 3/5/6 n run's group is unknown except f3's; pricing them as A would give layv >= 1.1, so 0.92 is the low end of the totals' range). Node and init scales stay the S48 one-node fit (0.757, 0.968). Profiles: `aac7_l4` (B), `aac7_l4a` (A), `target_l4` (target fabric, layv 0.919); `aac7_s48` and all others unchanged and selectable.
| parameter | S48 | L4 |
|---|---|---|
| bulk rate per APU thread | 10.31 (mix) | **10.63 (B)**, x0.818 = 8.69 (A) |
| layv factor | 0.655 | **0.919** (C02 span-based 0.73-0.86) |
| tree level = slowest group | no | yes |
| node / init scale | 0.757 / 0.968 | same |

**3.1 Residuals per run class** (model - measured, %, total without digit write; "?" = group of the run unknown: both groups shown; the rms uses B for "?").

<!--RESID-->
| class | group | measured s | before (S48 profile, top-group level) | after (L4) |
|---|---|---|---|---|
| 2 n [2] fast | B | 138.9 | 136.6 (-1.7%) | B 134.7 -3.1% |
| 4 n [2,4] fast | B | 181.5 | 185.0 (+1.9%) | B 181.7 +0.1% |
| 4 n [2,4] slow | A | 208.0 | 185.0 (-11.1%) | A 204.9 -1.5% |
| 4 n [4] fast | B | 181.3 | 190.3 (+5.0%) | B 186.8 +3.1% |
| 4 n [4] slow | A | 208.1 | 190.3 (-8.6%) | A 211.2 +1.5% |
| 3 n [3] | ? | 200.2 | 237.2 (+18.5%) | B 194.5 -2.8% A 220.4 +10.1% |
| 5 n [5] | ? | 213.2 | 297.9 (+39.7%) | B 239.5 +12.4% A 274.6 +28.8% |
| 5 n [2,5] | ? | 226.2 | 273.0 (+20.7%) | B 236.5 +4.6% A 270.7 +19.7% |
| 5 n [4,5] | ? | 232.2 | 247.8 (+6.7%) | B 244.2 +5.2% A 279.9 +20.5% |
| 6 n [2,6] | ? | 276.7 | 293.5 (+6.1%) | B 239.7 -13.4% A 274.7 -0.7% |
| 6 n [6] | ? | 243.3 | 309.6 (+27.2%) | B 248.3 +2.0% A 285.2 +17.2% |
| 6 n [3,6] | ? | 264.0 | 309.6 (+17.3%) | B 248.7 -5.8% A 285.5 +8.2% |
<!--/RESID-->

<!--RMS-->rms log residual: before 0.156  after 0.062 (all 12 classes; after = group B for the "?" classes)<!--/RMS-->

**3.2 What is fixed and what is not.** Fixed: 3 n (+18 -> -3 %), the 4 n slow/fast modes by group (-11 / +2 -> -2 / 0 %), 5 n (+7..+40 -> +5..+12 % in group B), 6 n `[6]` (+27 -> +2 %) and `[3,6]` (+17 -> -6 %). Not fixed: (a) the 6-node default `[2,6]`: measured 223-336 s (mean 277), model 240 s (B) / 275 s (A); it got *worse* against the S48 runs the old profile was fitted to (+6 -> -13 %), and the sign of `[2,6]` vs `[6]` is wrong (3.2: measured +23 +- 12 s, model -9 s); (b) 5 nodes is still 5-12 % high in group B and has one run per schedule, so its order is untestable; (c) the leftover scatter inside a class (4 n: +-10 s after the group is fixed; 6 n `[2,6]`: sd 22 s) is not in any term.
**3.3 A level penalty was tried and not adopted.** A multiplier on tree levels whose group is not a power of two and whose children are multi-node (`MN_MODEL_MIX_PEN`, default 0) lowers the residual monotonically (sum of squares over the non-pow2 classes 0.043 at 0 -> 0.029 at 0.6, the edge of the scan) while the layv factor rises to compensate (0.92 -> 1.01): the two are not separable with 3-7 runs per class. It is kept only as a sensitivity (k4 below).

#### 4. groups_table re-run, N = 576 .. 554, 3.71e13 and 3.82e13 (T2048 + X2 + VSLOT_RELEASE; modelled; fabric ASSUMED)
Method: `tools/groups_table.py search` on aac6 MI250 CPU nodes (one exclusive node per model variant, ~1 min each; driver `archive:drivers/l4/l4_search.sh`), `GT_PROFILE=target|target_l4`, `GT_RELEASE_GB=8.59` (the user's RELEASE figure, results/GRID_GROUPS.md §C06, subtracted from the device peak; margin rule unchanged: device + 0.5 + 2.1 GB <= 373.44 - 10), `GT_LAYV` for the sensitivities, candidates as C09 (~440 per N: pow2 prefix + one or two further levels, ratio <= 12 / 8). **Plan check (measured):** every best row of every variant below (46 / 46 each) passes `MN_PLAN_ONLY=<digits>:<N> MN_GROUPS=<g> ... plan check OK` (447 distinct candidate lines; the shared aac6 clone's host binary built 2026-10-10 22:00, run read-only; the second batch of 429 ran on the aac6 login node, ~20 s; the planner is assumed unchanged since; release switch set). k0 reproduces C09 exactly (576: 146.5 s; 569: 127.3 s). Raw: `archive:results-raw/L4_groups/`.

##### 4.1 The 576-node row and the best explicit schedule, by model variant
| digits | model | default `2,4,8,16,32,64,192,576` no-write s | best explicit schedule (s) | margin GB |
|---|---|---|---|---|
| 3.71e13 | k0: C09 model (layv 0.655, level = top group), no release | 183.7 | `2,4,8,16,80,560,576` (146.5 s) | 12.7 |
| 3.71e13 | k1: k0 + VSLOT_RELEASE | 183.7 | `5,30,180,576` (143.6 s) | 13.0 |
| 3.71e13 | k2: k1 + level = slowest group | 183.7 | `2,4,8,16,64,192,576` (183.5 s) | 21.5 |
| 3.71e13 | **k3: k2 + L4 layv 0.919 (the L4 model)** | 176.3 | `2,4,8,16,64,192,576` (176.1 s) | 21.5 |
| 3.71e13 | k5 (sensitivity): layv 0.73 (C02 span rate, low end) | 181.0 | `2,4,8,16,64,192,576` (180.9 s) | 21.5 |
| 3.71e13 | k6 (sensitivity): layv 1.0 (no layv penalty) | 174.8 | `2,4,8,40,200,576` (174.2 s) | 21.5 |
| 3.71e13 | k4 (sensitivity): k3 + level penalty 0.6 | 191.8 | `2,4,8,16,32,128,512,576` (184.3 s) | 20.2 |
| 3.82e13 | k0: C09 model (layv 0.655, level = top group), no release | 189.5 | `5,40,160,576` (167.4 s) | 10.6 |
| 3.82e13 | k1: k0 + VSLOT_RELEASE | 189.5 | `5,20,140,576` (152.8 s) | 12.7 |
| 3.82e13 | k2: k1 + level = slowest group | 189.5 | `2,4,8,16,64,192,576` (189.4 s) | 13.0 |
| 3.82e13 | **k3: k2 + L4 layv 0.919 (the L4 model)** | 181.7 | `2,4,8,16,64,192,576` (181.6 s) | 13.0 |
| 3.82e13 | k5 (sensitivity): layv 0.73 (C02 span rate, low end) | 186.7 | `2,4,8,16,64,192,576` (186.6 s) | 13.0 |
| 3.82e13 | k6 (sensitivity): layv 1.0 (no layv penalty) | 180.2 | `2,4,8,48,192,576` (179.7 s) | 13.0 |
| 3.82e13 | k4 (sensitivity): k3 + level penalty 0.6 | 197.3 | `2,4,8,16,32,128,512,576` (189.8 s) | 13.0 |

Reading (modelled, no part-file write, T2048): **(i) RELEASE** adds 8.59 GB of margin: at 3.82e13 N = 572..554 become feasible (k0: infeasible); the 576 margin of the default schedule goes 4.4 -> 13.0 GB. **(ii) The level-cost fix** removes the whole C09 "model-best" advantage: the best explicit schedule at 576 is the standing one within 0.2 s (`2,4,8,16,64,192,576` 183.5 s vs the default 183.7 s). **(iii) The layv factor 0.655 -> 0.919** lowers the standing 576 estimate by 7.4 s: 3.71e13 183.7 -> **176.3 s**, 3.82e13 189.5 -> **181.7 s** no-write (range over the layv factor 0.73..1.0: 181.0..174.8 and 186.7..180.2 s: the factor matters by +-3 s). With the part file (rate 1.0 GB/s ASSUMED; refit_s48's T1024 basis, +5.4 s over T2048) the old rows were 3.71e13 203.8 s and 3.82e13 208.7 s; with the S48 one-node local scales (node 0.757, init 0.968, from the 1-node run) 177.3 and 181.7 s; apply the same -7.4 s for the new layv factor (`estimate.py --fabric target_l4` prints them). **(iv) Sensitivity k4** (an unidentified level penalty 0.6 on non-pow2 levels with multi-node children, 3.3, layv left at 0.919, i.e. a pessimistic bound): 576 default 191.8 s (+15.5 s), best `2,4,8,16,32,128,512,576` 184.3 s: a 512 level removes all but one non-pow2 level.

##### 4.2 Best-schedule table (the L4 model, k3)

**3.71e13 digits** (modelled no-write seconds, T2048 + X2 + RELEASE; "C09 best" = k0, the old model, no release; "k5/k6" = best wall at layv 0.73 / 1.0)

| N | L4 best MN_GROUPS | wall s | margin GB | plan | std form `2,..,64,192,N` s | default s | C09 best (old model) s | k5 / k6 best s |
|---|---|---|---|---|---|---|---|---|
| 576 | `2,4,8,16,64,192,576` | 176.1 | 21.5 | OK | 176.3 | 176.3 | 146.5 (`2,4,8,16,80,560,576`) | 180.9 / 174.2 |
| 575 | `6,24,96,575` | 182.3 | 21.5 | OK | 202.8 | 227.9 | 155.0 (`2,4,8,16,80,560,575`) | 189.1 / 179.7 |
| 574 | `2,4,8,40,200,574` | 178.7 | 21.4 | OK | 199.9 | 278.9 | 149.6 (`2,4,8,16,80,560,574`) | 185.8 / 176.5 |
| 573 | `2,4,8,40,200,573` | 176.7 | 21.4 | OK | 195.6 | 781.8 | 140.6 (`2,4,8,16,80,560,573`) | 183.2 / 174.7 |
| 572 | `2,4,8,40,200,572` | 176.9 | 21.4 | OK | 192.8 | 205.3 | 143.4 (`2,4,8,16,80,560,572`) | 183.3 / 174.9 |
| 571 | `2,4,8,40,200,571` | 178.9 | 21.1 | OK | 189.9 | 2283.2 | 138.1 (`2,4,8,16,80,560,571`) | 185.9 / 176.7 |
| 570 | `2,4,8,40,200,570` | 177.3 | 20.7 | OK | 188.4 | 216.6 | 132.8 (`2,4,8,16,80,560,570`) | 183.8 / 175.3 |
| 569 | `2,4,8,16,48,192,569` | 177.8 | 20.8 | OK | 187.1 | 2281.0 | 127.3 (`2,4,8,16,80,560,569`) | 183.8 / 175.9 |
| 568 | `2,4,8,16,48,192,568` | 178.0 | 20.4 | OK | 185.6 | 370.8 | 140.4 (`2,4,8,16,80,560,568`) | 183.7 / 176.0 |
| 567 | `4,16,96,567` | 183.3 | 21.4 | OK | 193.0 | 194.6 | 143.3 (`2,4,8,16,80,560,567`) | 190.0 / 181.3 |
| 566 | `2,4,8,16,48,192,566` | 180.6 | 19.6 | OK | 186.8 | 1124.9 | 137.1 (`2,4,8,16,80,560,566`) | 186.9 / 178.4 |
| 565 | `2,4,8,16,48,192,565` | 178.7 | 19.3 | OK | 185.4 | 518.0 | 144.8 (`2,4,8,16,80,400,565`) | 184.7 / 176.7 |
| 564 | `2,4,8,32,128,564` | 178.9 | 12.7 | OK | 185.0 | 299.0 | 153.5 (`2,4,8,16,80,160,564`) | 183.2 / 176.9 |
| 563 | `2,4,8,32,128,563` | 180.9 | 12.7 | OK | 187.3 | 2263.0 | 150.3 (`2,4,8,16,80,160,563`) | 185.8 / 178.8 |
| 562 | `2,4,8,32,128,562` | 179.2 | 12.7 | OK | 186.0 | 1123.8 | 147.2 (`2,4,8,16,80,160,562`) | 183.9 / 177.4 |
| 561 | `2,4,8,32,128,561` | 179.9 | 12.5 | OK | 184.6 | 223.9 | 141.6 (`2,4,8,16,112,336,561`) | 184.3 / 178.1 |
| 560 | `2,4,8,32,192,560` | 179.6 | 12.7 | OK | 182.7 | 185.1 | 170.0 (`3,18,144,560`) | 184.4 / 177.9 |
| 559 | `4,24,96,559` | 191.0 | 12.7 | OK | 202.8 | 308.7 | 170.9 (`3,18,144,559`) | 199.7 / 188.2 |
| 558 | `2,4,8,48,192,558` | 192.7 | 12.7 | OK | 202.5 | 264.0 | 186.2 (`5,25,150,558`) | 199.4 / 190.3 |
| 557 | `2,4,8,32,192,557` | 193.3 | 12.7 | OK | 200.4 | 2250.5 | 177.2 (`2,4,24,144,557`) | 197.8 / 191.0 |
| 556 | `4,16,96,556` | 192.7 | 12.7 | OK | 202.4 | 618.7 | 202.1 (`2,4,8,16,112,556`) | 198.0 / 190.3 |
| 555 | `2,4,8,32,192,555` | 194.4 | 12.6 | OK | 202.3 | 286.6 | 190.3 (`7,14,112,555`) | 200.6 / 192.2 |
| 554 | `2,4,8,32,192,554` | 194.3 | 12.6 | OK | 202.0 | 1122.6 | 181.8 (`7,14,112,554`) | 199.8 / 192.5 |

**3.82e13 digits** (modelled no-write seconds, T2048 + X2 + RELEASE; "C09 best" = k0, the old model, no release; "k5/k6" = best wall at layv 0.73 / 1.0)

| N | L4 best MN_GROUPS | wall s | margin GB | plan | std form `2,..,64,192,N` s | default s | C09 best (old model) s | k5 / k6 best s |
|---|---|---|---|---|---|---|---|---|
| 576 | `2,4,8,16,64,192,576` | 181.6 | 13.0 | OK | 181.7 | 181.7 | 167.4 (`5,40,160,576`) | 186.6 / 179.7 |
| 575 | `4,24,96,575` | 191.5 | 13.0 | OK | 217.0 | 241.5 | 173.6 (`2,14,98,575`) | 200.2 / 188.8 |
| 574 | `2,4,8,48,192,574` | 193.6 | 12.8 | OK | 213.7 | 299.5 | 178.6 (`5,30,120,574`) | 201.2 / 191.2 |
| 573 | `4,24,96,573` | 195.3 | 12.8 | OK | 210.7 | 807.7 | 185.2 (`2,4,8,48,144,573`) | 203.3 / 192.8 |
| 572 | `4,24,96,572` | 195.6 | 12.8 | OK | 210.2 | 225.0 | infeasible | 203.6 / 193.1 |
| 571 | `4,16,96,571` | 197.1 | 12.8 | OK | 210.8 | 2309.4 | infeasible | 204.8 / 194.6 |
| 570 | `2,4,8,48,192,570` | 197.5 | 12.8 | OK | 207.1 | 240.2 | infeasible | 204.7 / 195.0 |
| 569 | `2,4,8,48,192,569` | 197.8 | 12.8 | OK | 208.0 | 2305.6 | infeasible | 204.9 / 195.1 |
| 568 | `2,4,8,48,192,568` | 198.0 | 12.8 | OK | 206.6 | 392.0 | infeasible | 205.2 / 195.3 |
| 567 | `5,20,100,567` | 198.4 | 12.4 | OK | 208.0 | 212.6 | infeasible | 206.4 / 195.5 |
| 566 | `2,4,8,32,192,566` | 198.3 | 12.8 | OK | 207.3 | 1145.2 | infeasible | 204.9 / 195.7 |
| 565 | `5,20,100,565` | 198.7 | 12.1 | OK | 206.8 | 537.1 | infeasible | 204.8 / 195.8 |
| 564 | `5,20,100,564` | 199.2 | 12.7 | OK | 207.6 | 322.4 | infeasible | 205.8 / 196.3 |
| 563 | `5,40,200,563` | 199.5 | 12.7 | OK | 210.2 | 2291.5 | infeasible | 206.7 / 196.6 |
| 562 | `5,40,200,562` | 199.7 | 12.7 | OK | 210.0 | 1145.2 | infeasible | 207.6 / 196.8 |
| 561 | `5,40,200,561` | 199.9 | 12.3 | OK | 208.1 | 240.3 | infeasible | 207.8 / 196.9 |
| 560 | `5,35,140,560` | 199.4 | 12.7 | OK | 208.4 | 209.2 | infeasible | 208.0 / 196.4 |
| 559 | `5,35,140,559` | 200.2 | 12.5 | OK | 216.5 | 318.3 | infeasible | 209.7 / 197.3 |
| 558 | `5,20,100,558` | 200.3 | 11.5 | OK | 212.5 | 271.9 | infeasible | 209.2 / 197.3 |
| 557 | `5,20,100,557` | 200.8 | 10.9 | OK | 212.9 | 2278.7 | infeasible | 209.3 / 197.8 |
| 556 | `5,20,140,556` | 202.2 | 12.5 | OK | 212.0 | 635.8 | infeasible | 211.7 / 199.2 |
| 555 | `5,35,140,555` | 202.7 | 12.4 | OK | 211.7 | 295.0 | infeasible | 212.3 / 199.7 |
| 554 | `5,35,140,554` | 203.0 | 12.4 | OK | 211.3 | 1138.3 | infeasible | 212.6 / 200.0 |

What changes against C09's table (modelled): (1) the best forms no longer have unbalanced tops (80/560, 112/336 ...): they are `2,4,8,{16,32,40,48,64},{128,192,200},N`, a few `5,35,140,N` / `4,24,96,N` / `8,40,200,N` (levels of ratio 3-5 with a balanced top), i.e. the standing shape with a different split of the 3-way top; they are 0.2 s better than the standing form at N = 576 and 3-25 s better than `2,..,64,192,N` at N <= 575 (575: 182.3 vs 202.8 s; 573: 176.7 vs 195.6 s) because the standing form's top level then joins children of 192, 192 and 189 nodes. (2) The default (code) schedule remains far worse at almost every N != 576 (185 s at 560, 224 s at 561, 217-1125 s elsewhere): an explicit MN_GROUPS is required for any exclusion. (3) The walls are flat: 176-182 s for N = 576..560 at 3.71e13 (+0.6..+6 s over 576), 191-194 s for 559..554. **"Round down" advice shrinks to 0-6 s** (575 -> 573: -5.6 s; the 19-28 s "round down to 569" of C09 is an artefact of the fixed bug), i.e. cost of excluding k nodes is about +1 s per node for the first 15 and ~+15 s once N <= 559 (the 192-node level no longer fits). (4) At 3.82e13 the best schedules are 181.6-203 s over N = 576..554, all memory-feasible with RELEASE (margins 10.9-13.0 GB). Both tables are modelled on an assumed fabric; no 5-way/7-way or 200-node level was ever run, only the planner accepted them.

#### 5. f3 single-level pairs on aac7 (queue items 80-83; collected 20:55 EDT; measured, rate-adjusted with the 4 n fit total = 81.1 + 1384 / r)
Runs: single4 (6), single4b (6), single6 (5), single6b (5); arm A = default schedule, B = single level (`4` / `6`); NIC group per run from the counters (drv.log). 4 n, residual to the fit line (s; chain-spike runs included, cold starts excluded): `[4]` (B) +5.3 +- 6.5 (n 5), default (A) +8.4 +- 5.6 (n 3): **no gain from a single 4-node level** (B - A = -3 +- 5 s, mod). 6 n (b = 1024 GB/s x s fitted on the `[6]` runs; residual total - 1024 / r): `[6]` 132.3 +- 9.9 s (n 4, totals 231.3 / 234.3 / 238.2 / 252.8), `[2,6]` 155.0 +- 21.5 s (n 4, totals 223.6 / 271.5 / 258.0 / 271.5): `[2,6]` - `[6]` = **+22.7 s, SE 11.8** (mod): the single 6-way level is not slower than the default, probably faster, and `[2,6]` is far more variable (sd 22 vs 10 s; one run was as fast as any `[6]` run). The group (A / B) does not order the 6-node runs: `[6]` in group A 238.2 s, in group B 231-253 s. The model (L4, group B) says `[2,6]` 239.7 s < `[6]` 248.3 s, i.e. -9 s: wrong in sign by about 30 s. Everything in the fit uses these runs (6 n `[2,6]`: 7 runs, `[6]`: 5 runs).

#### RESUME
Done: model fixes, `aac7_l4` / `aac7_l4a` / `target_l4` profiles (`aac7_l4_fit.json`), refit (12 classes), groups_table re-run on aac6 (7 variants, 46 best rows each, all plan-OK), f3 pairs (items 80-83) ingested. Branch `l4` (worktree `../ntt-wt-l4`), not merged. aac6 `~/l4`, `~/l4src.tgz`, `~/l4_search.sh` removed at the end; no aac7 job, no hold.
To redo after more aac7 runs: add the class totals to `L4_CLASSES` in `ecalc/refit_s48.py`, `python3 ecalc/refit_s48.py --l4 --write-profile`, repack `ecalc/*.py *.json`, `results/S13_e0.txt`, `results/D13b_strategy_e0.txt`, `tools/groups_table.py`, unpack on an aac6 MI250 node and run `archive:drivers/l4/l4_search.sh <tag> "GT_PROFILE=target_l4 GT_RELEASE_GB=8.59"` (~1 min on an exclusive node), then `tools/groups_table.py plancheck` on its `plan_cmds.txt`.
Open: (1) the 6-node default `[2,6]` variance (expected maximum over groups) is not in the model; a paired ABBA `[2,6]` vs `[6]` with 8 or more runs per arm would settle sign and size (+23 +- 12 s now); (2) layv factor 0.92 assumes group B for the 3/5/6 n runs of unknown group: probe the NIC group in every multi-node run (`tools/envprobe.sh` bracket, as f3 did) to separate it from the rate; (3) the 5-node order needs rate-paired repeats; (4) the target fabric (100 GB/s, 2 us) and the part-file rate are still ASSUMED; the layv factor on the dragonfly is the aac7 Slingshot-11 one; (5) the 559..554 counts lose the neat 192-node level (about +15 s): a search with deeper or wider chains is possible but unmeasured; (6) the `E[max over groups]` effect would grow with the number of groups at the 576 levels (288 pairs at level 1): modelled nowhere; the k4 sensitivity (+15 s) is one crude bound.

## T06: layeq / layv rate in the time model, and the 576-node estimate redone

(former file: T06.md)

Labels: m measured, mod modelled, a assumed. Times Eastern. 2026-10-11. CPU only (no cluster job). Data: the S48 logs (results/TIME_MODEL.md §S48; local copy of aac7 `~/s48/log/n*_r*.log`, 8 runs).
Separate file (not appended to S48) because it changes the model code and corrects one S48 number.

#### Which exchange kind runs where (from the code)
- `comm_layered.c`: the equal-slab inter stage is tagged `XK_LAYEQ` (`inter_post`), the unequal one `XK_LAYV` (`v_post`).
  `rns_dist.c` runs the general transform (unequal parts, `v` exchanges) exactly when the group size g is not a power of 2 (`gen = !is_pow2(g) || DIST_GEN`, line ~1617); a power-of-2 g uses ntt_dist's equal-slab pipeline.
  redist / result / spill / addsh are `alltoallv` of their own kinds at every g (not layeq / layv).
- The group sizes per tree level are `mem_model.mn_groups`: 1 n: none; 2 n: [2]; 4 n: [2, 4]; 6 n: [2, 6]; **576 n: [2, 4, 8, 16, 32, 64, 192, 576]**.
  So at 576 the levels 2..64 are **layeq** (6 of 8) and the levels **192 and 576 are layv** (2 of 8). 6 n likewise: level 2 layeq (bs 498 GB at 16.8 GB/s per thread, m), level 6 layv (1317 GB at 7.5, m).

#### Model change (all behind a default-off factor; digits not involved)
- `mn_model.py`: `Fabric.v_factor` (default 1.0), `Fabric.rate/a2a(..., v=)`; `piece_cost` passes `v=True` for the transform exchange of a general-map group (`not is_pow2(g) or force_gen`). The product-cost memo key includes `v_factor`.
  Only profile `aac7_s48` reads a factor (`layv_factor` in `aac7_s48_fit.json`); every other profile keeps 1.0, so their results are unchanged.
- `estimate.py --layv-factor F` (default 1.0 = off) sets it on the target fabric.
- `refit_s48.py`: fits `layv_factor` on the non-power-of-2 totals with bw, lat, scales fixed (writes it to the json); prints residuals, the 576 scenarios and the per-level split. Selftest passes.
- **Correction of an S48 number**: the product-cost memo (`mn_model._PC`) has no NODE_SCALE / INIT_SCALE in its key, so S48's "target + refit locals" 576 row read entries cached by earlier calls with other scales. Clean values: 152.4 s (3.71e13) and 157.3 s (3.82e13) no-write, not 169.9 / 175.3 (mod). `refit_s48.model()` now clears the memo. The fit itself (bw 10.31, node 0.757, init 0.968) and the "as is" rows (180.2 / 186.0) are unchanged.

#### Fit (mod)
- layv_factor **0.655** (layv exchange at 65.5 % of the layeq rate, in the model's units). The raw per-thread xstats ratio is **0.41-0.42** (m: 7.5 / 8.0 / 7.0 vs 18.3 / 19.1 / 17.3 GB/s), lower because the model's single bw also carries overlap and the redist / result exchanges, which do not slow by that ratio.

| nodes | measured total (s) | model (s) | residual |
|---|---|---|---|
| 1 | 76.3 | 76.3 | 0.0 % |
| 2 | 139.0 | 136.6 | -1.7 % |
| 4 | 181.8 | 185.0 | +1.8 % |
| 6 | 293.5 | 293.5 | 0.0 % (fitted, one parameter on one point; was -20.0 %) |

#### 576 nodes, no-write wall, fabric 100 GB/s per APU and 2 us (a), part file 1.0 GB/s (a)  (mod)
| locals | digits | layv = layeq (a) | layv factor 0.655 (as fitted on aac7) | factor 0.41 (raw xstats ratio, sensitivity) |
|---|---|---|---|---|
| refit (node 0.757, init 0.968) | 3.71e13 | 152.4 | 161.3 (+8.9) | 176.7 (+24.3) |
| refit | 3.82e13 | 157.3 | 166.7 (+9.4) | 182.9 (+25.6) |
| standing target (1.0 / 1.0) | 3.71e13 | 180.2 | 189.1 (+8.9) | 204.5 (+24.3) |
| standing target | 3.82e13 | 186.0 | 195.3 (+9.3) | 211.6 (+25.6) |
With the part file (+15.8..17.3 s): refit locals 169.6 / 177.3 / 190.6 s at 3.71e13, 173.7 / 181.7 / 195.4 s at 3.82e13; standing 196.1 / 203.8 / 217.1 s and 200.8 / 208.7 / 222.4 s.
- The layv penalty at 576 (factor 0.655): **+8.9 s at 3.71e13, +9.4 s at 3.82e13** (all layv levels together). Per level, each alone: level 192 +2.2 s, level 576 +6.7 s (3.71e13) / +7.2 s (3.82e13). At the raw 0.41: +24 / +26 s (192: +5.9, 576: +18.4 / +19.7).
- Reading: the crude "~219 s if the whole 6 n penalty carries" is an upper bound that scales the whole wall; the per-level model prices the penalty only on the 2 of 8 levels that are layv, and at 100 GB/s per APU most of an exchange is hidden, so it costs 9-26 s (a, depends on the layv ratio carrying to the target fabric; unmeasured).
  Range to quote: **152-157 s (layv = layeq) .. 161-167 s (aac7 ratio) .. 177-183 s (raw ratio)** with refit locals; 180-186 .. 189-195 .. 205-212 s with the standing target locals.

#### Caveats
- 0.655 is one parameter on one point (6 n, two runs at 304 / 283 s, spread 21 s) and absorbs everything the 6 n run has beyond the 2/4 n curve (not only layv). The data cannot say whether the ratio holds at 192 / 576 nodes or on the target fabric.
- bw / lat / node scales of the target are a, as in S48. Nothing here measures the layv rate on the target; a 3- or 5-node run (also non-power-of-2) on aac7 would test whether 0.655 is the kind or the size.

#### Adoption 2026-10-11 (user decision; branch a02)
- The user adopted the layv factor 0.655 into the standing 576-node estimate. Code: `mn_model.TARGET_LAYV_FACTOR = 0.655`, set as the `v_factor` of the
  `target` and `target-m` profiles (so `estimate.py --target` and the other target estimates use it by default). `estimate.py --layv-factor F` now defaults
  to the profile's factor (None -> profile); `aac7_s48` takes its json fit (0.6547), `aac7` and `aac7_s18` stay 1.0; `--layv-factor 1.0` restores the old numbers.
  `target-w2` (historical reports) stays at 1.0. Digits not involved (time model only).
- Standing 576-node no-write walls after adoption (modelled; `estimate.py --g 576 --D <T/576>`, the same numbers as `--target`, 100 GB/s, 2 us; the refit
  locals via `MN_MODEL_NODE_SCALE=0.757 MN_MODEL_INIT_SCALE=0.968`):

| locals | digits | before (layv = layeq) | after (factor 0.655) |
|---|---|---|---|
| standing target (1.0 / 1.0) | 3.71e13 | 180.2 s | **189.1 s** |
| standing target | 3.82e13 | 186.0 s | **195.3 s** |
| refit (0.757 / 0.968) | 3.71e13 | 152.4 s | **161.3 s** |
| refit | 3.82e13 | 157.3 s | **166.7 s** |

  The part file (write at 1.0 GB/s per node) adds 14.3-17.3 s as before (modelled). The refit-locals rows reproduce the T06 table.
- The `--target` run took about 1 minute on the workstation (no timeout); the single-point runs were used for the 3.82e13 row.

#### RESUME
Done: code, json, this file; branch `t06` pushed. Adopted 2026-10-11 (above; branch `a02`). Open: optional 3/5-node aac7 timing to test the kind-vs-size reading; the 0.655 is one parameter on one point (unmeasured on the target).

## M2a - time-model refit, scaling study, 1- and 2-node points (aac7 queue items 77 / 78)

(former file: M2a.md)

Status: armed 2026-10-12 03:34 ET (item 77 started at once on the 2 free nodes; item 78 queued behind it). Labels: measured / modelled / assumed.

#### Goal
Opportunity M2: refit the time model (`ecalc/mn_model.py`, `calib_table.py` inputs; current fit target_l4, `results/TIME_MODEL.md §L4`) to current code with a 1/2/4/6-node scaling study taken with `COMM_XSTATS` and `MN_WAIT_STATS`. This item takes the 1- and 2-node points while 2 nodes are otherwise idle (4 busy until about 04:40, then a 4-node soak to 08:30).

#### Design
- Two queue items, `77_m2a_scale12.sh` and `78_m2a_scale12b.sh` (same file; the file name selects tag `a` / `b`), each NODES 2, EST_MIN 60, pre-approved.
- Binary: main's SHMEM build in the existing aac7 worktree `main` baseline (commit 0298fe2, not modified; the binary is copied per item and checked for SHMEM).
- Line: the `LINE=` of `e16_headline.sh` plus `COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1 ECALC_SEGV_TRACE=1`; no digit writes. Size 6.441e10 digits per node (1 node 6.441e10, 2 nodes 1.2882e11).
- Order 1 2 2 1 1 2 2 1 ... (ABBA-style alternation of node count), up to 12 runs, no new run after 48 min. Page cache of the references evicted before each run; one network program per node; every step with `--overlap --mem=0`, no `-w`; `mnrun.sh` with `MNRUN_NODES=n` runs the 1-node points on the first node of the 2-node hold.
- Per run recorded: rc, wall, total, bs / recip / dm stage times, init, VERIFY, xstats GB and `t_wait` of PE 0, NIC MCU group (counter sets `oxe_mcu_meas_18..25` = A vs `28..33` = B on every cxi device of the used nodes, snapshot before and after, same method as C02). Full logs (xstats and wait-stats lines) are kept.

#### Arm / outputs
Arm: file moved into `~/aac7q/queue/` (daemon already running). Outputs `~/m2a/a/` (item 77) and `~/m2a/b/` (item 78): `log/<run>.log|.plain|.wait`, `nic/`, `summary.tsv`, `summary.txt`. Done-markers `~/m2a/DONE` (77) and `~/m2a/DONE_b` (78); the marker holds `SUCCESS|FAILED ok=<n> bad=<n> <epoch>`. Item `.rc` and `.log` sit next to the item in the queue dir.
Dry run (measured, stub): both items through the daemon in STUB mode (no Slurm): syntax, binary SHMEM check, line extraction OK, rc 0 each. The real measurement path follows the 51_h4_abba2 / 61_c03_time items; it was not run before arming.

#### Collection (cheap; no large agent)
`cat ~/m2a/DONE ~/m2a/DONE_b; cat ~/m2a/a/summary.txt ~/m2a/b/summary.txt`. Then feed the `.log` files to `python3 -I ecalc/calib_table.py <logs> --tsv m2a.tsv` (calib_table reads xstats and stage times) and compare the 1- and 2-node rows with `ecalc/mn_model.py` / target_l4 predictions; raw data to `archive:results-raw/`.

#### Results, 1- and 2-node points (collected 2026-10-12; all numbers measured)
Setup: main's SHMEM build, e16 aac7 launch line, COMM_XSTATS and MN_WAIT_STATS, no digit writes, 6.441e10 digits per node (1 node 6.441e10, 2 nodes 1.2882e11), order 1 2 2 1 1 2 2 1 1 2 2 1. Item 78 (`~/m2a/b/`) collected from its summary.tsv. Item 77's summary files in `~/m2a/a/` were overwritten by a later item (7a, same output dir); item 77 was rebuilt from its saved raw logs (`~/m2a/a77_saved/log/*.plain`, all 12 runs) with the item script's own parsing (`^total`, `^bs`, `^recip`, `^dm`, `init` in the total line, xstats GB / t_wait of PE 0). 24 of 24 runs rc 0 and VERIFY OK. Item 7a will write to `~/m2a/a/` again (marker `~/m2a/DONE`).

Mean (sd) in seconds, per item. "excl r1" drops run 1 of the 1-node arm: in item 77 it ran 87.44 s (bs 48.8, init 17.5), in item 78 80.68 s (bs 43.4, init 16.5), a cold-start run (page cache / first launch after the allocation); the other five 1-node runs agree to under 1 s.

| item | nodes | n | total | bs | recip | dm | init |
|---|---|---|---|---|---|---|---|
| 77 | 1 (all) | 6 | 76.57 (5.36) | 41.45 (3.60) | 5.12 (0.11) | 20.79 (0.12) | 14.17 (1.72) |
| 77 | 1 (excl r1) | 5 | 74.39 (0.64) | 39.98 (0.11) | 5.09 (0.09) | 20.75 (0.09) | 13.50 (0.60) |
| 77 | 2 | 6 | 136.41 (2.69) | 48.31 (0.97) | 16.90 (2.75) | 55.69 (2.92) | 28.70 (1.68) |
| 78 | 1 (all) | 6 | 75.30 (2.72) | 40.62 (1.36) | 5.04 (0.09) | 20.66 (0.12) | 13.85 (1.38) |
| 78 | 1 (excl r1) | 5 | 74.23 (0.75) | 40.07 (0.26) | 5.03 (0.10) | 20.66 (0.14) | 13.32 (0.54) |
| 78 | 2 | 6 | 137.97 (4.18) | 48.04 (1.49) | 17.27 (1.97) | 57.30 (3.68) | 28.87 (4.51) |

- Pooled over both items (excl r1): 1 node 74.31 s (n 10), 2 nodes 137.19 s (n 12). The two items agree within 1.6 s at 2 nodes and 0.2 s at 1 node (excl r1): reproducible.
- Per-node efficiency (weak scaling, same digits per node): E = T1 / T2 = 74.31 / 137.19 = **0.54** (77: 0.545, 78: 0.538, excl r1), i.e. at the same digits per node the 2-node run takes 1.85 x the 1-node time, so each node delivers 54 % of its 1-node digit rate. Part is the digit count itself (2 x the digits cost slightly more than 2 x the work, n log n), most is communication: the 1 -> 2 node step adds bs +8.1 s (+20 %), recip +12.0 s (3.4 x), dm +35.8 s (2.7 x), init +15.3 s (2.1 x).
- Comm: XSTATS GB of PE 0 is 0 at 1 node and 2739.3 at 2 nodes (identical in all 12 runs); summed t_wait of PE 0 is 139.8 s (77) / 142.3 s (78) at 2 nodes (sd 5.4 / 4.8). The NIC counters show the traffic on one MCU group per run (item 78: A in 9 of 12 runs, B in 2, 1 unknown; B runs 135.7 and 135.0 s vs 2-node A runs 132.7-143.6 s: no separation, n too small).
- Run-to-run spread at 2 nodes is dominated by dm and recip (item 77 run 11: recip 22.5, dm 61.6 s; item 78 run 7: recip 20.0, dm 63.3 s), bs is stable (sd 1.0-1.5 s).

How this feeds the model refit (not refitted here): (1) the 1-node anchor (74.3 s at 6.441e10) fixes the single-node stage terms bs 40.0, recip 5.0, dm 20.7; the cold first run must be excluded or flagged. (2) The 2-node point is the first multi-node anchor at the target per-node size: it supplies the per-stage communication step (bs +8.1, recip +12.0, dm +35.8 s) that the refit of `ecalc/mn_model.py` / target_l4 needs to separate fixed latency from per-GB volume (XSTATS 2739 GB, t_wait about 141 s). (3) Compare these three stage steps with the model's 2-node predictions before using the 4- and 6-node points; the 2-to-4 and 4-to-6 steps are still needed to fit the node-count dependence (do not extrapolate to 576 nodes from one step). Feed the `.log` files to `ecalc/calib_table.py` as planned above.

#### Third batch: item 7a (collected 2026-10-12, aac7; all numbers measured)
Item 7a: same line, binary (main 0298fe2), order 1 2 2 1 1 2 2 1 1 2 2 1, 6.441e10 digits per node, marker `SUCCESS ok=12 bad=0`, 05:45-06:09 ET. It wrote `~/m2a/a/summary.txt` and `summary.tsv` (item 77's data are the saved logs in `~/m2a/a77_saved`, section above). 12 of 12 runs rc 0 and VERIFY OK.

Mean (sd) in seconds. "excl r1" drops 1-node run 1 (81.42 s, bs 46.2, init 14.5: cold first run, as in 77 and 78); "excl first 2" drops 2-node run 2 (167.85 s, init 42.6, t_wait 226.5 s: the first 2-node run, the slowest of all 30 runs of the three items).

| nodes | n | total | bs | recip | dm | init |
|---|---|---|---|---|---|---|
| 1 (all) | 6 | 75.70 (2.86) | 41.25 (2.48) | 5.11 (0.06) | 20.59 (0.07) | 13.70 (0.49) |
| 1 (excl r1) | 5 | 74.56 (0.67) | 40.25 (0.48) | 5.13 (0.05) | 20.60 (0.08) | 13.54 (0.33) |
| 2 (all) | 6 | 143.69 (12.64) | 49.29 (2.41) | 18.20 (3.18) | 58.70 (5.26) | 31.67 (5.50) |
| 2 (excl first) | 5 | 138.85 (4.95) | 48.55 (1.77) | 17.20 (2.27) | 57.07 (3.82) | 29.48 (1.38) |

- Agrees with items 77 and 78: 1 node 74.56 s (77: 74.39, 78: 74.23, excl r1), 2 nodes 138.85 s excl first (77: 136.41, 78: 137.97; 143.69 s with all runs). Pooled 1 node (15 runs excl r1): 74.4 s; 2 nodes, the 18 runs of the three items: 139.4 s (item means 136.4 / 138.0 / 143.7).
- Step 1 to 2 nodes, 7a excl first/r1: total +64.3 s, bs +8.3, recip +12.1, dm +36.5, init +15.9 (77/78: bs +8.1, recip +12.0, dm +35.8, init +15.3): the same stage steps. E = T1/T2 = 74.56 / 138.85 = 0.54.
- Comm: XSTATS GB of PE 0 2739.3 at 2 nodes in all 6 runs (as before); summed t_wait of PE 0 is 133-226 s, median 137 s (mean 158 s with the first run, 138 s without); the five later runs are 133.1-144.2 s, 77/78 had 139.8 / 142.3 s mean. NIC counters: A group in 4 of the 6 two-node runs and B in 2 (runs 2 and 11); the B runs were 167.85 s (first run) and 135.07 s, so no separation.
- Reading: a first run after an allocation is slow at both node counts (1 node +7 s, 2 nodes +29 s over the later runs); keep it out of the fit or flag it. The three batches give 16 steady-state-quality points for the model refit; the 4- and 6-node points are still open.

#### 4- and 6-node points (item m2b, collected 2026-10-12; all numbers measured unless marked)
Item 7d (`~/m2b/`, hold of 6 nodes, 08:18-09:06 ET), main 0298fe2, same line and per-node size (6.441e10 digits per node: 4 nodes 2.5764e11, 6 nodes 3.8646e11 digits), COMM_XSTATS and MN_WAIT_STATS, sequence 4 6 6 4 4 6 6 4 4 6 6 4. **12 runs, all rc 0, VERIFY OK** (marker "12 runs ok, 0 not ok").

Mean (sd) in seconds; "excl first" drops the first run of that node count (4 nodes: run 1, 314.97 s, init 61.5 s; 6 nodes: run 2, 271.42 s), as for the 1- and 2-node batches.

| nodes | n | total | bs | recip | dm | init | PE 0 t_wait (summed) |
|---|---|---|---|---|---|---|---|
| 4 (all) | 6 | 209.29 (52.55) | 75.69 (10.85) | 30.22 (17.38) | 90.60 (26.07) | 37.43 (11.83) | 317.5 (91.1) |
| 4 (excl first) | 5 | 188.15 (10.08) | 71.41 (3.13) | 23.17 (1.96) | 80.31 (7.49) | 32.62 (1.03) | 280.5 (11.5) |
| 6 (all) | 6 | 250.53 (16.18) | 100.75 (6.68) | 28.26 (3.56) | 114.91 (9.17) | 30.60 (7.37) | 427.7 (18.7) |
| 6 (excl first) | 5 | 246.35 (14.01) | 99.57 (6.73) | 27.57 (3.52) | 114.74 (10.24) | 27.62 (1.17) | 423.7 (17.8) |

Individual totals: 4 nodes 314.97, 186.70, 184.00, 182.84, 181.38, 205.84 s (the last, run 12, is 24 s above the four steady runs: bs +5, dm +13 s); 6 nodes 271.42, 263.61, 250.88, 246.49, 246.01, 224.74 s (falls over the batch, 47 s from first to last: not converged). Bytes sent by PE 0: 4493.3 GB (4 nodes), 6051.4 GB (6 nodes), vs 2739.3 GB at 2 nodes (constant across runs). NIC groups alternate A/B within each count; the A/B counters are in `summary.tsv`.

##### All node counts, steady state (1 node: 15 runs excl r1 of items 77/78/7a; 2 nodes: item 7a excl first; 4 and 6 nodes: excl first)
| nodes | total | bs | recip | dm | init | E = T1/Tn: total | bs | recip | dm | init |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 74.4 | 40.1 | 5.08 | 20.67 | 13.45 | 1 | 1 | 1 | 1 | 1 |
| 2 | 138.85 | 48.55 | 17.20 | 57.07 | 29.48 | 0.54 | 0.83 | 0.30 | 0.36 | 0.46 |
| 4 | 188.15 | 71.41 | 23.17 | 80.31 | 32.62 | 0.40 | 0.56 | 0.22 | 0.26 | 0.41 |
| 6 | 246.35 | 99.57 | 27.57 | 114.74 | 27.62 | 0.30 | 0.40 | 0.18 | 0.18 | 0.49 |

(Weak scaling: per-node digits constant, so ideal Tn = T1, E = 1. Stage columns for 1 node are pooled means of the batches; E computed here from the table values, modelled arithmetic on measured means.)

##### Stage-time growth over 1 node (s)
| nodes | total | bs | recip | dm | init |
|---|---|---|---|---|---|
| 2 | +64.5 | +8.5 | +12.1 | +36.4 | +16.0 |
| 4 | +113.8 | +31.3 | +18.1 | +59.6 | +19.2 |
| 6 | +171.9 | +59.5 | +22.5 | +94.1 | +14.2 |

- dm and bs carry most of the growth (at 6 nodes dm +94 s and bs +60 s of +172 s); recip jumps 1 -> 2 nodes (+12 s) then adds only +5-6 s per extra 2 nodes; init steps up once (1 -> 2 nodes, +16 s) and stays at 28-33 s (the 4-node init is higher than the 6-node init by 5 s, steady runs).
- PE 0 summed t_wait is about 70 s per node (137 s at 2 nodes, 281 s at 4, 424 s at 6) and bytes sent grow 2739 -> 4493 -> 6051 GB: the communication wait grows about linearly in nodes while per-node work is constant (rate per stage from this table is the input, no fit done here).
- Where the step from 2 to 4 and to 6 comes from is bs (+23 s, then +28 s) and dm (+23 s, then +34 s): the two stages with the all-to-all exchanges.

##### What the refit needs next (no refit done here)
- Fit points: 1 node 15 steady runs, 2 nodes 17 (18 minus first), 4 nodes 5 steady-state-quality runs (4 close together, one at 205.8 s), 6 nodes 5 runs that still drift down (246 -> 225 s). The 4- and 6-node means are thinner than the 1/2-node ones; item 7g (4 nodes, cand) and the later 4/6-node runs of the queue add points.
- Decide per node count the rule for the first run after an allocation (4 nodes +127 s over steady, 6 nodes +25 s, 2 nodes +29 s, 1 node +7 s): flag, not fit.
- Decide whether the 4-node run 12 (205.8 s) and the 6-node drift are NIC / group effects (compare the group columns in `summary.tsv`, 2 groups, 12 runs: too few to say) before using run-to-run sd as the fit's noise.
- The model needs a communication term per stage with an n-dependence (t_wait about 70 s x n, bytes per PE per stage), not a fixed per-step cost; 8+ nodes cannot be measured (QOS limit 6 nodes per user), so extrapolation beyond 6 is a model call.

#### Open issues
- 1-node at 6.441e10 digits: memory fit not yet confirmed on this build (4e10 ran at 1 node); a failed run shows as rc != 0 in the summary.
- Slow host exclusion is done by the daemon (`-x`).

#### RESUME
Items 77, 78, 7a and m2b (4- and 6-node points) COLLECTED; nothing of M2a runs. Next: the refit (inputs above), after item 7g adds 4-node cand points. Was: wait for `~/m2a/DONE` (about 04:35) and `~/m2a/DONE_b`; collect, fill the numbers here, then plan the 4/6-node items.

## S48: scaling study (1/2/4/6 nodes at 6.441e10 digits per node) and refit of the aac7 model profile

(former file: S48.md)

Labels: m measured, mod modelled, a assumed. Times Eastern. Status 2026-10-11 09:10: all 8 timed runs done (1/2/4/6 nodes x 2), all VERIFY OK; refit done (section Results). Sections Design/Refit below are the pre-run plan; the fit procedure changed (see Results, 'Fit').
Base: main daeb0f9, binary built from it by `tools/gate.sh --build` machinery (Cray SHMEM, `-DCOMM_SHMEM` present in the log), copied to aac7 `~/s48/ecalc_s48` (md5 3d0fb4a2a425...).

#### Design
- Per size N in {1, 2, 4, 6}: 2 timed runs of `ecalc` at N x 6.441e10 digits, no digit output, the aac7 launch line (LINE of `ecalc/e16_headline.sh`, parsed at run time)
  + `COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1`, current defaults. One retry per item after an early death with no `total` (Cray shmem_init failure). Runs of
  N nodes use the first N nodes of the hold. A run that cannot end by D (epoch 1791723386) is not started. Reference page-cache eviction is done for ~/ref files
  (none is read by a no-output run, so it is a protocol no-op).
- Driver `archive:drivers/s48/s48_run.sh <nodes> <runs>` (also at aac7 `~/s48/s48_run.sh`). Outputs `~/s48/log/n<N>_r<k>[b].log` (raw), `~/s48/summary.tsv`
  (nodes, label, rc, wall s, VERIFY, total s, time), marker `~/s48/done_n<N>` (DONE or PARTIAL, written last).
- Job B queue items (T2's hold, dropped atomically in `~/batch1011/jobB/queue/`, owner T3):
  `10_1_gate2.sh` (EST 6 min: 2-node 1e9 gate of the s48 binary, `tools/gate.sh --nodes 2 --hold`; result `~/s48/done_gate2`, `~/s48/gate2/gate.txt`),
  `10_2_n1.sh` (EST 8), `10_3_n2.sh` (EST 12), `10_4_n4.sh` (EST 14).
  Stub dry-runs (DRY=1) of the s48_run.sh logic for N = 1, 2, 4, 6 and of the three item wrappers passed; the gate item was syntax-checked only
  (it needs a live hold; it is the documented `gate.sh` usage).
- J6: Slurm job **12475**, `-N6 -J b1011w --begin=2026-10-11T02:56:26 (aac7 Pacific = W 05:56 EDT) --deadline=04:36:26 PDT (W+100 min) -t 60`, exclusions
  aac7-n01,aac7-n61, script `~/s48/s48_j6.sbatch` (runs `s48_run.sh 6 2`, then writes `~/s48/J6_DONE`, exits by itself; also `~/s48/J6_START`, `~/s48/j6.out`).
  Expected length about 12-25 min (2 runs of about 300-500 s mod) inside the 60-min limit.

#### Refit (ecalc/refit_s48.py; defaults untouched)
- Fits INIT_SCALE / NODE_SCALE on the 1-node `total` (and init), then bw_apu and lat on the 2/4/6-node totals (grid + bisection, log-error least squares),
  HIDE_POW2, GEN_HIDE, T_ROUND, MAP_RATE kept at AAC7_CONSTS. Prints measured exchange rates (xstats GB / t_post per APU thread), wait-stats and the init
  timeline mark as independent checks, and the 576-node estimate (3.71e13 digits) with the unchanged target locals and with the refit local scales (mod; target
  fabric 100 GB/s per APU and the file rate are a, not fitted). `--write-profile` writes `ecalc/aac7_s48_fit.json`, read by the new profile
  `estimate.py --fabric aac7_s48` (mn_model.py: `aac7_s48` added, nothing else changed; refused while the json is absent).
- Test on existing data (the S43 4-node and 2-node arm-B walls and the 1-node 78 s; logs of S43 no longer exist on aac7, so the test uses their numbers, walls
  incl. about 6 s start-up, 1 node 77.9 s (m)): `./refit_s48.py --table 1:77.9 2:155.7 4:220.3` -> node_scale 0.80 (AAC7 1.12), init_scale 0.85 (AAC7 1.20), bw 7.9 GB/s,
  lat 34 us (grid edge: bw and lat are not separated by two multi-node points), residuals -0.7..+0.7 % (mod); the old AAC7 profile is +39 % / +76 % / +84 % off at 1/2/4 nodes.
  576-node standing estimate 180 s no-write (target as is) vs 170 s with the refit local scales (mod, a fabric). `./refit_s48.py --selftest` passes (parser on a synthetic log).
  The real fit needs the 6-node point and the stats logs; expect to read bw/lat as a pair.

#### Results (2026-10-11 09:10 EDT; collected by T3)

Data: aac7 `~/s48` (summary.tsv, log/n*_r*.log, done_n*, J6_DONE, j6.out); local copy `/tmp/claude-1000/s48log` (scratch). The 1/2/4 n logs were first written to `~/s47/run` by an
env leak (OUT inherited from the Job B queue driver) and copied to `~/s48` (`NOTE_OUT_LEAK`); the 6 n logs are in `~/s48/log` (J6 job 12475, nodes c1s3b0n0..c1s5b1n0). 6.441e10 digits per node, no digit output.

##### Measured (ecalc `total`, seconds; init = slowest process; bs, dm, recip = RESULT lines)
| nodes | run 1 total | run 2 total | mean | init (r1/r2) | bs (r1/r2) | dm (r1/r2) | recip (r1/r2) |
|---|---|---|---|---|---|---|---|
| 1 | 76.73 | 75.87 | 76.30 | 13.5 / 13.2 | 40.5 / 40.2 | 22.6 / 22.3 | 6.1 / 5.8 |
| 2 | 144.12 | 133.85 | 138.99 | 27.3 / 26.6 | 51.0 / 47.0 | 61.3 / 55.9 | 18.4 / 16.5 |
| 4 | 181.97 | 181.62 | 181.80 | 30.1 / 28.9 | 70.9 / 72.3 | 76.9 / 76.9 | 22.0 / 22.0 |
| 6 | 304.12 | 282.82 | 293.47 | 41.6 / 31.3 | 112.6 / 112.0 | 145.9 / 135.7 | 40.3 / 38.5 |
(measured; recip is inside dm.) Work per node is constant (weak scaling), so the ideal is a flat 76 s.

##### Fit (modelled; `ecalc/refit_s48.py`, profile `estimate.py --fabric aac7_s48` reads `ecalc/aac7_s48_fit.json`)
- First attempt (bw and lat fitted on 2/4/6 n together, one bw): residuals +4 / +10 / -13 % at 2/4/6 n and lat at its grid edge: the data cannot do it. Two findings changed the fit:
  1. **lat is not identifiable**: over 2..68 us the modelled walls move < 0.5 % (few messages), so lat is now FIXED at the profile 8.5 us; bw and lat were never separable (and are not now).
  2. **6 n is not on the 2/4 n curve**: it needs ~28 % less effective bandwidth (see xstats below). bw is therefore fitted on the power-of-2 sizes only (2, 4 n) and 6 n is reported as an implied bw / factor.
- Result: node_scale **0.757** (AAC7 profile 1.120), init_scale **0.968** (1.200), bw_apu **10.31 GB/s** (AAC7 3.60), lat 8.5 us (fixed). Implied bw per size (lat fixed): 2 n 9.93, 4 n 10.62, 6 n 7.44 GB/s per APU
  (non-pow2 factor 0.72). Fast/slow-mode runs imply 4 n 10.6 / 8.5 and 6 n 7.8 / 7.1.
- Residuals (model wall_nowrite vs mean measured): 1 n 0.0 %, 2 n -1.7 %, 4 n +1.8 %, **6 n -20.0 %** (234.9 vs 293.5 s). Old AAC7 profile: +42 / +97 / +122 / +88 % (it was calibrated on the older, slower code).
  The 6 n residual is a model gap, not noise: the model prices the non-pow2 (general map) exchange with the same bw, while the code runs a slower exchange kind there.
- Test of the earlier 3-point table fit (S43 walls) is superseded by these measured points.

##### Where 6 n is slower than the 4 n trend (xstats / wait stats, measured)
- 4 n -> 6 n adds 111.7 s: init +7.1, bs +40.7, dm +63.9 (of which recip +17.4), other ~0. Per-node compute is the same, so it is exchange time.
- xstats, GB per node / rate per APU thread (GB / t_post): at 2 and 4 n the bulk exchange is `layeq` (equal slabs): bs 515 GB at 17.3 (2 n), 1263 GB at 18.3 (4 n); dm 1245 GB at 17.5, 1868 GB at 19.1.
  At 6 n the bulk exchange is `layv` (variable layout, the non-power-of-2 general map): bs 1317 GB at **7.5**, dm 2625 GB at **8.0**, recip 744 GB at 7.0 GB/s: **2.3-2.4x slower per thread** for the same kind of traffic.
  redist/result exchanges also fall with size (redist 9.1 -> 6.0 -> 4.8 GB/s at 2/4/6 n; result 10.6 -> 8.9 -> 6.9).
- wait stats (blocked time, summed over the 4 APU threads, per node): bs 48 / 115 / 202 s, dm 95 / 150 / 282 s, recip 32 / 49 / 77 s at 2 / 4 / 6 n. As a share of (4 x phase time): bs 25 / 40 / 45 %, dm 40 / 49 / 48 %, recip ~48 / 56 / 48 %.
  So at 6 n about half of every distributed phase is blocked waiting on the exchange (same share as 4 n, but the exchange itself is 2.4x slower).
- init: 26.9 / 29.4 / 36.5 s mean at 2 / 4 / 6 n (r1 41.6, r2 31.3 at 6 n); last init timeline mark 35.6 / 37.4 / 47.7 s. The init growth (+7 s 4->6 n) is about 6 % of the 4->6 n gap; the 10 s run-to-run swing at 6 n is init (10.3 s) plus dm (10.2 s).
- Reading: the 6 n penalty is the `layv` exchange kind (non-pow2 node count), not the fabric getting slower per link and not init. HIDE_POW2 / GEN_HIDE cover only the xGMI overlap, not this rate difference.

##### 4 n bimodality (S47 ABBA in `~/s47/run`, 42 usable 4 n runs, same four nodes c1s1b0n0 c1s1b1n0 c1s2b0n0 c1s2b1n0 throughout; measured)
- Totals fall in two modes: fast **180.4-185.6 s** (about 24 runs) and slow **207.3-213.2 s** (11 runs), with a few in between (189.9, 193.9, 201.9) and cold outliers (224.7, 231.0, 231.0, 280.7 in the first rounds, init 39-47 s). S48's two 4 n runs (181.97, 181.62) are fast mode.
- The slow mode is the same code, same nodes, both arms (A and B alike; e.g. rounds 5 and 7 have both arms slow): it is **not an A/B effect**. It comes in blocks (rounds 4-8 mostly slow, 9-11 fast), so it follows time/fabric state.
- Where: dm +20 s (76.8 -> 96.6-103 s; recip 22.0 -> 27.0 s of that +5 s), bs +3..+9 s (71 -> 74-80 s), init unchanged (~30 s). That is +25 % in the exchange-bound phases. The S47 logs have no xstats/wait lines, so the cause inside the exchange cannot be separated further from these logs
  (the same mode also shows at 2 n: 144.1 vs 133.8 s, dm 61.3 vs 55.9 s). Candidate cause (unproven): fabric/NIC contention or a different path state; this is a pointer for a rerun with `COMM_XSTATS=1` at 4 n, not a finding.
- Consequence for fits: a 4 n mean over a mixed set is ~190 s; the fit above uses the S48 pair (fast mode, 181.8 s). With the slow mode (208 s) the implied 4 n bw would be 8.5 GB/s instead of 10.6 (about -20 %).

##### 576-node estimate (modelled; fabric 100 GB/s per APU and 2 us ASSUMED, not fitted; part file 1.0 GB/s ASSUMED)
| digits | locals | no-write | with the part file |
|---|---|---|---|
| 3.71e13 | standing target (aac6-calibrated) | 180.2 s (3.00 min) | 196.1 s (3.27 min) |
| 3.71e13 | refit locals (node 0.757, init 0.968) | 169.9 s (2.83 min) | 185.7 s (3.10 min) |
| 3.82e13 | standing target | 186.0 s (3.10 min) | 200.8 s (3.35 min) |
| 3.82e13 | refit locals | **175.3 s (2.92 min)** | **190.1 s (3.17 min)** |
Caveats (all modelled/assumed): (1) the target fabric is ASSUMED and 576 is a non-power-of-2 count; the measured 6 n `layv` exchange is 2.4x slower per thread than `layeq` and the model under-predicts 6 n by 20 %. If a similar gap
(+25 % of the modelled wall) applied at 576 n the no-write wall would be about 219 s at 3.82e13 (assumed, crude upper marker, not a model output); lowering only the target bw by the 0.72 factor does not move the model (the model is not
monotone there, within 3 %), so the shortfall is in the exchange kind, not in bw. (2) 4 n showed a 25 % slow mode in dm/bs; the 576-node dragonfly has more such state. (3) The refit scales come from 1 n (node 0.757 = 1 n total 76.3 s) and carry over to the target only if its MI300A nodes behave alike; the standing target locals were calibrated on aac6.
The refit tightens the local scales (-5.7 % wall at 576 n) but the open risk is the non-pow2 exchange, not the local compute.

##### Adoption (user's decision)
Adopt `aac7_s48` as the aac7 model profile: cost = one more profile to keep (default untouched, selected only by `--fabric aac7_s48`); benefit = aac7 predictions within 2 % at 1/2/4 n instead of +42..+122 %, but it still under-predicts 6 n by 20 %, so it is not a 576-node predictor without a non-pow2 exchange term.

> **Correction (2026-10-11, results/TIME_MODEL.md §T06):** the "170 s" 576-node row (refit locals) read stale memo entries; clean values are 152.4 s (3.71e13) / 157.3 s (3.82e13) no-write (mod), and with the fitted layv factor 0.655: 161.3 s / 166.7 s (mod, fabric a). Standing target locals: 180.2 s unchanged at 3.71e13.

#### RESUME
- DONE 2026-10-11 09:15: results above, `ecalc/aac7_s48_fit.json` committed, `s48_run.sh` now reads `S48_OUT` (not `OUT`). Nothing armed; no job of mine. Open: merge decision, a 6 n/4 n rerun with xstats to look at the `layv` rate and the 4 n slow mode. Original arm notes follow.
- (history) Armed: Job B items (run by the driver of T2 on its hold, serially, through W, continuing on B2 until D) and J6 (starts at W). Nothing else of mine runs;
  no hold of mine; no job except 12475 pending.
- One-line collect (after D): `sshpass -p <see private protocol> ssh aac7 'cd ~/s48; cat done_gate2 done_n1 done_n2 done_n4 done_n6 J6_DONE; cat summary.tsv' < /dev/null`
- Refit command after D (copy the logs first):
  `mkdir -p /tmp/s48log && scp <aac7>:~/s48/log/*.log /tmp/s48log/ && cd ~/apucode/ntt-wt-s48/ecalc && ./refit_s48.py --logs /tmp/s48log --write-profile`
  then `estimate.py --fabric aac7_s48 --g 1 2 4 6 --D 6.441e10`, copy numbers here, commit `aac7_s48_fit.json` with this file, push `s48`.
- Missing items (queue skipped past D, J6 dropped by Slurm): the refit works on whatever sizes have a DONE/PARTIAL marker; report which are missing.
- Cleanup after use: remove aac7 `~/ntt-wt/s48` (git worktree remove) and local `../ntt-wt-s48` after merge; `~/s48` holds raw logs (keep until archived).

## SCALE35 - scaling points at 3 and 5 nodes

(former file: SCALE35.md)

**Question.** What are the totals at 3 and 5 nodes at 6.441e10 digits per node, to complete the 1/2/4/6-node scaling curve of PLAN.md section 2?
**Change / branch.** None (docs only). Commit 39346d03 of `main`, SHMEM build, defaults (`comm_ofi`).
**Method.** aac7, 3 runs per count, no output written, `VERIFY` of every node read from the log; aac7 profile launch line.
**Results.** All 6 runs: VERIFY OK, no segfault (measured). Digits not compared (no write).

| nodes | digits | totals (s) | mean +- sd (s) | bs / recip / dm / init (mean s) |
|---|---|---|---|---|
| 3 | 1.9323e11 | 147.98, 143.11, 152.06 | 147.72 +- 4.48 | 52.7 / 15.7 / 57.4 / 34.0 |
| 5 | 3.2205e11 | 175.90, 177.18, 193.25 | 182.11 +- 9.67 | 68.0 / 21.0 / 77.9 / 32.1 |

With the 1/2/4/6-node means of PLAN.md section 2 (67.6 / 119.4 / 172.2 / 204.6 s): 1 to 6 nodes read 67.6, 119.4, 147.7, 172.2, 182.1, 204.6 s;
efficiency T1/Tn 1.00, 0.57, 0.46, 0.39, 0.37, 0.33 (measured / derived). The curve is monotone; 3 and 5 nodes show no slow mode.
**Verdict.** Data point only; nothing to adopt.
**Open issues.** Three runs per count; the 5-node spread (sd 9.7 s) is the largest.
**RESUME.** Done.

## S19B: profile analysis of S19A (2 → 10 nodes) and the S19C switch A/B, with a 576-node projection

(former file: S19B.md)

Analysis only, read-only on aac7 (job 12287; no launches). Code: `ntt-acc` main @ 3e508e76. Inputs: `~/s19a/log/*`,
`~/s19a/summary.txt`, `~/s19a/drive.sh`, `~/s19c/ab.tsv`, `~/s19c/log/*`. S19A ran 2026-10-07 09:52–10:39 EDT, S19C
10:40–12:11 EDT (the `rd_*` log times). Labels: **(m)** measured, **(mod)** modelled, **(a)** assumed.

**Configuration (both drivers):** the 3.71e13 target's per-node share, 6.441e10 digits/node, no write, `COMM_TRANSPORT=shmem`
with comm_ofi carrying the device exchanges (code default; 4 comm_ofi pools of 2304 MiB in the log), `ECALC_NP=auto
RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 COMM_SHMEM_ROUND_MB=1024 MN_TOPO_GROUP=0
ECALC_MEM_GUARD_GB=6 MEM_REPORT_DEVS=1`. S19A adds `MN_COMM_MARK=1 COMM_LAYER_STATS=1 DIST_STATS=1` (but not p10n).
**`DM_MN_LEAN=1` was not set, although the target's launch line has it (internal target notes §4, the user's decision of 2026-10-05).
So "base" here is the target line without LEAN, and the LEAN arm is the line the target will actually run.**

All 19 runs rc 0, VERIFY OK on every node (m).

#### 1. Per-phase walls, 2 → 10 nodes (m)

`total` line of each run. "tree" is the largest node's distributed tree levels inside bs. "leaf" is bs minus tree (seed
wait + batch + mdev).

| n | total | init | bs | (leaf) | (tree) | dm | (recip) | (division) | dc | tree levels (group sizes) |
|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 202.2 | 25.5 | 67.9 | 38.3 | 29.6 | 104.6 | 39.6 | 65.0 | 3.2 | 2 |
| 4 | 283.2 | 27.2 | 111.9 | 35.6 | 76.3 | 140.4 | 39.5 | 101.0 | 2.7 | 2, 4 |
| 6 | 412.3 | 24.4 | 160.2 | 37.5 | 122.7 | 223.9 | 39.7 | 184.2 | 3.0 | 2, 6 (3-way, general map) |
| 8 | 362.5 | 27.3 | 162.2 | 35.9 | 126.3 | 168.5 | 48.2 | 120.3 | 3.2 | 2, 4, 8 |
| 10 | 465.0 | 25.6 | 185.4 | 36.1 | 149.3 | 250.0 | 65.1 | 184.9 | 2.8 | 2, 10 (5-way, general map) |
| 10 (p10b) | 416.0 | 26.0 | 167.5 | 34.7 | 132.8 | 217.3 | 48.5 | 168.9 | 3.4 | same |
| 10 (p10n, no stats) | 467.8 | 25.0 | 184.1 | 37.2 | 146.9 | 253.3 | 67.8 | 185.5 | 4.1 | same |

- **The stats cost nothing measurable:** p10n (no stats) 467.8 s sits between p10 465.0 and p10b 416.0 (m).
- **What stays flat as n grows:** init (24–27 s), the leaf (35–38 s) and dc (3–4 s).
- **What grows:** the tree levels (≈ 30 s for the 2-node level, then ≈ 48 s per further binary level) and the dm.
- **Non-power-of-two node counts cost more** (n = 6 and 10 use the general map): the division takes 1.4–1.5× as long as at
  the power-of-two n = 8, and the n = 6 run is slower than the n = 8 run.
- **Each 10-node division step** (`divmod(mn, DKM)`, p10 / p10b): step 1 (A μ, X_hi Q) 88.3 / 80.4, step 2 26.4 / 31.2,
  X_lo Q 61.3 / 56.4. The reciprocal's products take 45.3 / 37.4.

#### 2. Compute vs exchange per phase; bytes moved (m)

**Where the numbers come from.** `comm-mark` (node 0) gives each phase's wall, GB received and post-to-completion seconds
summed over the 4 APU threads. `layer-stats` gives the per-APU-thread **span**, the union of exchange lifetimes, i.e. the time
any exchange was in flight. The post sums count the depth-2 pipeline twice: Σpost/4 = 277 s against a span of 151 s at
n = 10. So the span is split across phases in proportion to their post seconds ("exch"); "other" = phase wall − exch, which
is local passes plus waiting for peers plus serial steps.

"exch" is an **upper bound on exposed exchange**: the overlap of local passes with in-flight exchanges is not instrumented.
The fabric was busy 95 % of the span, and exchanges queued 84–90 s per APU thread behind the previous one ("held"). So
whenever exchanges are in flight the distributed phases are fabric-bound, and most of "exch" is exposed.

| n | phase | wall s | exch s | other s | GB recv (node 0) | GB/s per exchange per APU thread |
|---|---|---|---|---|---|---|
| 2 | L1 / recip / div | 29.6 / 39.6 / 65.0 | 9.7 / 7.9 / 22.9 | 19.8 / 31.8 / 42.1 | 631 / 574 / 1650 | 10.2 / 11.5 / 11.4 |
| 4 | L1 / L2 / recip / div | 32.5 / 43.8 / 39.5 / 101.0 | 8.9 / 17.6 / 10.4 / 44.6 | 23.6 / 26.2 / 29.1 / 56.4 | 588 / 903 / 825 / 2314 | 9.5 / 7.4 / 11.4 / 7.4 |
| 6 | L1 / L2(3-way) / recip / div | 29.4 / 93.3 / 39.7 / 184.2 | 6.0 / 31.9 / 12.1 / 69.2 | 23.4 / 61.4 / 27.6 / 115.0 | 586 / 1519 / 931 / 3007 | 11.4 / 5.5 / 8.9 / 5.1 |
| 8 | L1 / L2 / L3 / recip / div | 29.4 / 40.5 / 57.4 / 48.2 / 120.3 | 9.2 / 14.4 / 22.8 / 13.8 / 57.1 | 20.2 / 26.1 / 34.6 / 34.4 / 63.2 | 585 / 828 / 1052 / 951 / 2684 | 9.2 / 8.3 / 6.7 / 10.0 / 6.8 |
| 10 | L1 / L2(5-way) / recip / div | 30.0 / 120.8 / 65.1 / 184.9 | 7.5 / 44.6 / 19.5 / 79.8 | 22.6 / 76.2 / 45.6 / 105.1 | 584 / 1949 / 1078 / 3091 | 10.7 / 6.0 / 7.6 / 5.3 |
| 10b | same | 29.5 / 103.6 / 48.5 / 168.9 | 8.0 / 38.0 / 16.3 / 73.7 | 21.5 / 65.6 / 32.1 / 95.2 | same | 9.7 / 6.8 / 8.8 / 5.6 |

Totals over the distributed phases:

| n | exch s | other s | fabric GB per APU thread | node fabric | node 0 received |
|---|---|---|---|---|---|
| 2 | 40 | 94 | 573 | 2.3 TB | 2.9 TB |
| 4 | 82 | 135 | 976 | 3.9 TB | 4.6 TB |
| 6 | 119 | 227 | 1289 | 5.2 TB | 6.0 TB |
| 8 | 117 | 178 | 1305 | 5.2 TB | 6.1 TB |
| 10 | 136–151 | 215–250 | 1425 | 5.7 TB | 6.7 TB |

The model's NIC bytes per node (2.6 / 4.4 / 6.3 / 5.9 / 7.6 TB) are 13–33 % above the measured fabric bytes.

**Findings:**
- **The fabric stage runs at the transport's rate:** ≈ 10.2 GB/s per APU thread (m, layer-stats buckets ≥ 537 MB/exchange),
  the same as §117's raw comm_ofi rate.
- **The rate per exchange falls with group size:** about 11 GB/s at g = 2, 5.3–6.8 at g = 8–10 (division and the large tree
  levels).
- **"Other" doubles from 2 to 10 nodes** (94 → 215–250 s) while each node's share of arithmetic barely changes. That is the
  largest unexplained block. It includes waiting for peers at each exchange (skew), and that wait is not instrumented today.
- **General-map levels move about 2× the bytes of a binary level** (n = 10 L2: 1949 GB; n = 6 L2: 1519 GB; binary levels
  585–1052 GB). Per binary-level equivalent (log₂ 5 = 2.32 levels) the n = 10 L2 costs 45–52 s, the same as a binary level
  (40–57 s), so the general map is not less efficient per unit of tree work. The **division over a general-map group,
  however, is 1.4–1.5× the power-of-two division** (n = 6/10 vs n = 8).
- **The reciprocal's single-node chain** (to 34126 limbs) takes **2.28 s at minimum but 6.8–22.2 s in most runs** (15
  ten-node runs: mean 12.4 s). The excess, about 10 s per run on average, is a wait, not arithmetic (§5 item 3).

#### 3. Memory per phase and the top node's margin (m unless marked)

Node = Σ over 4 APUs of driver-used + host RSS at the `MEM_REPORT_DEVS` report points (`tools/parse_mem.py`). The figures are
**identical to 0.1 GB across rounds** (deterministic). Layout = `BS_LAYOUT_ONLY` node_with / device_with; the LEAN and DC8
layouts are mem_model (mod; `--check-c` reproduces the C layout exactly with the run's environment).

| config (10 nodes) | rank 0 at init | tree, every node | rank 0 at bs (before dm) | rank 0 device / top APU | layout node_with | top-node margin | other nodes' margin |
|---|---|---|---|---|---|---|---|
| base | 262–281 | 363.0–363.6 | **389.1–389.6** | 369.2 / **92.3** | 391.49 | **−1.9 to −2.4** | ≈ −28 |
| LEAN (`DM_MN_LEAN=1`) | 268–273 | 354.2–355.2 | **362.9–363.5** | 343.2 / 85.8 | 382.90 (mod) | **≈ −19.7** | ≈ −27.8 |
| DC8 (`DIST_CHUNKS=8`) | 262–285 | 356.0–356.7 | 381.8–382.1 | 362.0 / 90.5 | 384.61 (mod) | ≈ −2.6 | ≈ −28 |
| T2048 (`MN_T_CHUNK_MB=2048`) | 266–273 | 362.7–363.4 | 389.0–389.3 | 369.2 / 92.3 | 391.49 (mod: arena unchanged) | ≈ −2.3 | ≈ −28 |

Notes:
- **"bs" is rank 0's report after the tree, with the dm arena reserved.** Only the top node prints it, and it is the base
  run's peak point. Host RSS: 28 GB at init (21.5 GB pinned seed staging), 11 GB in the tree, 20 GB at bs.
- **The top APU in base reaches 92.3 GB driver-used** (m) against the target's measured 93.36 GB per-APU hipMalloc edge
  (TGTBENCH2). This compares directly only if the target's driver accounting matches aac7's, so treat it as a flag and not as
  a fit test.
- **LEAN takes the top node from 389.4 to 363.2 GB (−26.2 GB)** and every other node by −8.4 GB in the tree phase. The
  model's arena drops 223.34 → 214.75 GB (−8.6 GB, mod); once the dm need shrinks, the tree need binds.
- **DC8 halves the general-map v-slots:** 3.44 → 1.72 GB per APU, −6.87 GB per node (mod), against −7.0 GB measured on every
  node (m).
- **v-slots grow with the node count:** 13.8 GB per node at 10, 26.8 at 576 (mod, `mem_model.vslot_resident`; it matches
  TARGET.md's 405.61 − 378.79). Per-node memory is therefore **not O(1) in g** for this term. DC8 at 576: 26.83 → 13.46 GB per
  node (−13.4 GB, mod), so the device layout would go 363.53 → ≈ 350.2 GB and the margin to the 373.44 GB edge 9.91 → ≈ 23.3
  GB (mod; assumes DIST_CHUNKS changes nothing else in the layout; confirm with `BS_LAYOUT_ONLY` at `:576`).
- **LEAN + DC8 at 10 nodes:** node_with 376.02 GB (mod), −15.5 GB against base.

#### 4. Scaling model and the 576 projection (mod)

**Model:** `estimate.py --fabric aac7_s18 --bw B` (mn_model DEFAULT15C on the launch line; the code's own level and piece
plan), with one fitted parameter, the **effective** per-APU fabric bandwidth B.

**Assumptions:**
- the per-node compute terms as calibrated in mn_model;
- B absorbs everything the fabric phases lose to skew, queuing and per-round costs;
- the effective rate stays constant from 10 to 576 nodes (a); the falling per-exchange rate with group size in §2 says this
  is optimistic;
- MN_GROUPS = 2,4,8,16,32,64,192,576 at 576.

**Fit:** **B = 6 GB/s per APU thread (fitted)**, against the 10.2 GB/s raw stage rate (m) and the profile's 4.25.

| n | measured (m) | model, B = 6 | residual | model, aac7_s18 (B = 4.25) |
|---|---|---|---|---|
| 2 | 202.2 | 210.0 | +3.9 % | 254.8 (+26 %) |
| 4 | 283.2 | 292.0 | +3.1 % | 368.1 (+30 %) |
| 6 | 412.3 | 376.8 | −8.6 % | 484.5 (+18 %) |
| 8 | 362.5 | 357.1 | −1.5 % | 457.6 (+26 %) |
| 10 | 462.0 (mean of 6 base-like runs, 416–522) | 435.1 | −5.8 % | 564.8 (+22 %) |

At 10 nodes, B = 6 matches the measured phases one by one: tree 130.8 (m 132.8–149.3), reciprocal 54.1 (m 48.5–65.1),
division 177.9 (m 168.9–184.9).

**576 nodes, 3.71e13, no write** (mod):

| fabric | wall | per-phase |
|---|---|---|
| aac7-class, B = 6 | **737 s** (656–851 s for B = 7–5) | init 20.6 + seed wait 9.3 + batch 19.8 + top 16.2 (66 s); **distributed tree levels 412 s (56 %)**; division 181 s (25 %); reciprocal 72 s (10 %); exposed exchange 583 s (79 %) |
| target, standing estimate (`--fabric target-m --bw 47`) | 216.5 s | |
| target, B derated by aac7's effective/raw ratio (6 / 10.2 ≈ 0.59; assumed to carry) | **≈ 266 s** | |
| target at 100 GB/s/APU (a) | 178 s | |

- **The 576 tree's largest steps are its 3-way levels:** at B = 6, the 192 level costs 97 s and the 576 level 66 s; the
  binary levels cost 27–48 s each.
- **Fabric volume at 576:** 13.7 TB per node (mod), or ≈ 10.3 TB scaled by the measured/model byte ratio at 10 nodes
  (0.75). That is 1.8–2.4× the 10-node volume.
- **Message count:** 1.46 M messages per APU (mod) against 30 k at 10 nodes.
- **Per-node time** at a fixed share grows **≈ 1.7×** from 10 to 576 nodes at aac7-class fabric (435 → 737 s, mod).

#### 5. Ranked opportunities

Seconds and GB at 10 nodes are from the measurements. Values at 576 are modelled as in §4.

| # | opportunity | 10 nodes | 576 nodes (mod) | status / next step |
|---|---|---|---|---|
| 1 | **Raise the effective fabric rate** (6 → closer to the 10.2 raw; on the target, 2 NICs per APU, `COMM_OFI_NICS`) | ≈ −40 s per +1 GB/s/APU (mod); 2-NIC +2–6 % on aac7's 1-NIC hardware (m, §116) | ≈ −80 s per +1 GB/s/APU (737 → 656 at B = 7, → 595 at B = 8) | the dominant term. Target A3/A4 (kit) are unmeasured; the per-exchange rate falling with g (§2) is the 576 risk |
| 2 | **Find and cut the "other" time in the distributed phases** (it grows 94 → 215–250 s from 2 to 10 nodes at the same per-node work) | up to ≈ 120–155 s if it is waiting for peers (upper bound; not instrumented) | grows with the level count (unquantified) | diagnostic first: a per-exchange "waiting for peers" stat (ready → all peers posted). No code in this task |
| 3 | **`MN_T_CHUNK_MB=2048`** (half the rounds) | −60 s mean paired (−139 … +49; §6) | model −4.6 s (it prices only staging) | promising, unproven; memory 0 at 10 nodes (m). Retest |
| 4 | **The reciprocal chain's wait** (2.28 s minimum vs a 12.4 s mean) | ≈ −10 s mean (m) | unknown | timestamps (`ECALC_LOG_CLOCKS`) around `recip(mn)` to find what it waits on |
| 5 | **`DIST_CHUNKS=8`: memory** | −7.0 GB on every node (m) | **−13.4 GB per node** (v-slots halved; device margin 9.9 → 23 GB) | time-neutral at 10 nodes (m). At 576, 2× the messages per APU: +3–12 s upper bound at 2–8.5 µs per message (mod) |
| 6 | **`DM_MN_LEAN=1`** | −26.2 GB top node, −8.4 GB others (m); time neutral | already in the target's layout (363.53 GB) | already on the launch line; put it in every aac7 base line |
| 7 | init: the seeds wait 4.5 s for the region map; arenas take 8.5 s | ≤ 5 s | ≤ 5 s (3 %) | low priority |
| 8 | Run-to-run fabric noise (±12 % at 10 nodes: 416–522 s, same configuration) | 50–100 s between "fast" and "slow" jobs | n/a | environment; same bytes, longer fabric time (p10 vs p10b: span 151 vs 136 s). Pair every A/B |

#### 6. A/B verdicts (S19C, 10 nodes, 3 rounds; m)

- **Noise caveat:** the order within every round was fixed: base, T2048, LEAN, DC8. **Base always ran first**, so an order
  effect cannot be separated from a config effect. Base itself spans 424–522 s, and the 6 base-like 10-node runs of S19A+C
  give mean 462.0 s, sd 38.6 s. No single paired delta below ≈ 50 s means anything, and 3 rounds cannot reach significance
  (an all-same-sign test needs ≥ 6 rounds for two-sided p < 0.05).
- **Phase timings move together.** "Slow" runs are slow in every fabric phase (tree, reciprocal, division) at once, which
  points to the fabric and not to the switch.

| config | r1 | r2 | r3 | mean ± sd | paired Δ vs base (r1, r2, r3) | mean Δ | top-node peak | verdict |
|---|---|---|---|---|---|---|---|---|
| base | 476.8 | 424.1 | 522.2 | 474.4 ± 49.1 | — | — | 389.1–389.4 | — |
| T2048 | 385.5 | 473.0 | 383.5 | 414.0 ± 51.1 | −91.3, +48.9, −138.6 | **−60.3** | 389.0–389.3 | **promising, not proven.** Its two fast runs (384–385 s; tree 120 s, division step 1 72–73 s) are below every one of the 6 base-like runs (min 416); its third was in the slow band. Memory unchanged at 10 nodes |
| LEAN | 417.4 | 442.4 | 452.7 | 437.5 ± 18.1 | −59.4, +18.4, −69.5 | −36.8 | **362.9–363.5** | **adopt in every aac7 base line** (already the target line): time neutral within noise, −26.2 GB top node, deterministic |
| DC8 | 436.9 | 426.1 | 435.8 | 432.9 ± 5.9 | −39.9, +2.0, −86.4 | −41.4 | 381.8–382.1 | **memory win, time neutral at 10 nodes** (tightest spread of the four, inside the base range). −7.0 GB per node (m), −13.4 GB per node at 576 (mod) |

**Recommendation:**
1. Treat `DM_MN_LEAN=1` as part of the base: it is already the target's line, and every future aac7 profile and A/B should
   carry it.
2. Adopt `DIST_CHUNKS=8` for its memory, subject to the user's decision (§7). Before it goes on the target line, confirm
   `BS_LAYOUT_ONLY=37100000000000:576` with it and a 10-node LEAN+DC8 run.
3. Do **not** adopt `MN_T_CHUNK_MB=2048` yet. Rerun it as LEAN vs LEAN+T2048 in ABBA order over ≥ 6 rounds (≈ 90 min at
   10 nodes).

#### 7. Decisions for the user

| decision | options | cost / benefit |
|---|---|---|
| D1 `DIST_CHUNKS=8` | (a) adopt on the target line after a `:576` layout check; (b) keep 4 | (a) −13.4 GB per node at 576 (mod), device margin ≈ 23 GB, time neutral at 10 nodes (m); risk: 2× the messages at 576 (+3–12 s, mod). (b) no risk, 9.9 GB margin |
| D2 `MN_T_CHUNK_MB=2048` | (a) a focused ABBA retest, ≥ 6 rounds (≈ 90 min hold); (b) adopt now; (c) drop | (a) settles a possible −60 s (−13 %) at 10 nodes; (b) unproven, the model does not explain the gain; (c) loses a candidate |
| D3 instrument "other" (waiting for peers) and the reciprocal chain's wait | (a) a stats-only switch (off by default, digits unchanged); (b) not now | (a) targets the largest unexplained block (≈ 120–155 s at 10 nodes, upper bound); small code. (b) nothing |
| D4 `aac7_s18` profile | (a) refit bw_apu to the effective 6 GB/s (fit residuals −8.6 … +3.9 % on 2–10 nodes); (b) keep 4.25 (+18 … +30 %) | (a) a better aac7 predictor; no effect on the target profile |

#### Reproduce

Bounded greps on the logs:
- `^total`, `^bs  `, `^comm-mark`, `^divmod(mn, DKM)`, `tree levels`, `recip(mn): the single-node chain`;
- `layer-stats .*passive\)` (span, xGMI, fabric, both), `block transposes` (fabric bytes per APU), `one-at-a-time: held`;
- `mem[r] [phase]` → `tools/parse_mem.py`.

Models:
- `estimate.py --fabric aac7_s18 --bw {5,6,7,8} --D 6.441e10 --g 2 4 6 8 10 576 [--verbose] [--chunk-mb 2048]`;
- `estimate.py --fabric target-m --bw {47,28}`;
- `mem_model.py --check-c <layout> 31 <T>` with the run's environment (`DIST_CHUNKS`, `MN_T_CHUNK_MB`) and `lean 1` added to
  the layout line;
- `mem_model.vslot_resident` at `DIST_CHUNKS` 4 / 8.

## D213d — D2 13d — the model: steps for G, the 13c recalibration, the 576 piece counts, the estimate (Phase 13d, PLAN.md §32 row D2)

(former file: D213d.md)

Branch `p13d-D2` (from main 566291d). Files: `ecalc/mn_model.py`, `ecalc/design_table.py` (one line), `ecalc/estimate.py`
(`--target`, labels), `ecalc/tests/plan_vs_model.py` (new), `archive:results-raw/loose/D2_13d_steps1.txt`, `archive:results-raw/loose/D2_13d_plan1_C.txt`,
`archive:results_raw_2026-10-19/D2_13d_plan576.txt`, `results/DESIGN_TABLE.md` (regenerated), this file. No GPU job. One login-node build of L's
branch in `~/ntt-D213d` on aac6, used only to run `MN_PLAN_ONLY` at size 1. Every number is labelled measured / modelled /
assumed.

#### Headline — the 576 estimate for the target; 4.4 × 10¹³ is above the step

| total digits (576 nodes) | wall (modelled) | pieces: tree_max + recip + div | node (device + host) |
|---|---|---|---|
| **4.25 × 10¹³ (proposed target, 1.2 % below the step)** | **234 s = 3.90 min** | 88 + 69 + 28 = 185 | 452 GB (425 + 30) |
| 4.29 × 10¹³ (the last size below the step) | 235 s = 3.91 min | 88 + 69 + 28 = 185 | 455 GB |
| 4.30 × 10¹³ (the step's first size) | 247 s = 4.12 min | 100 + 69 + 28 = 197 | 455 GB |
| **4.4 × 10¹³ (the Phase 13c target)** | **275 s = 4.59 min** | 124 + 71 + 34 = 229 | 463 GB (436 + 30) |

(`cd ecalc && ./estimate.py --target`. Modelled: the per-node compute is recalibrated on measured one-node runs, and the piece
counts are the C code's own. The fabric is **assumed**: 100 GB/s per APU, 2 µs per message, 2 GB/s part files. All sizes
fit inside 480 GB.)

**4.4 × 10¹³ does not sit below the step.** The C code's own plan (agent L, `archive:results_raw_2026-10-19/L13d_plan576.txt`) puts the step at
**4.29 → 4.30 × 10¹³**. There the tree levels' largest groups go from 88 to 100 pieces, and they reach 124 by 4.39; at 4.30
the top node's leaf also crosses the one-node step S4. From 4.29 to 4.34 × 10¹³ the wall rises 13 % (235 → 267 s) for
1.2 % more digits. The model now matches L's table in every column at all 401 sizes from 2.0 to 6.0 × 10¹³.

**Proposed new target: 4.25 × 10¹³ digits.** It sits on the flat stretch from 4.04 to 4.29 × 10¹³, where tree_max stays at
88 and recip / div stay at 69 / 28 in the C plan. Its top node's leaf is 7.64 × 10¹⁰ digits, the size of the measured share
run. Going to 4.4 × 10¹³ costs +41 s for +3.5 % digits.

**Why the step moved**: the code gives every node the same number of **terms** (mn.c: node r computes [1 + N r/g,
1 + N (r+1)/g)). A node's digits are the sum of log10 k over its terms, so they are not equal. At 576 nodes the **top node holds
1.0358 × the average** and node 0 holds 0.772. The binary levels' top groups hold 1.034–1.036 × the average. Every tree
level waits for its largest group, and the wall waits for the largest leaf. §80's model costed node 0's group at the
average: its 214 pieces at 4.4 × 10¹³ are exactly L's node-0 `total`. L's `total_max` is 229.

#### 1. The step list for G (task 1) — delivered 20:15, updated 20:24 / 20:45 / 20:53

`archive:results-raw/loose/D2_13d_steps1.txt` (also `~/d2_steps1.txt` on aac6). Size 1, the default design, 5 × 10¹⁰–1.3 × 10¹¹. The major
steps below are **confirmed by the C code's own plan** (L's `MN_PLAN_ONLY=<D>:1` swept at 10⁸ resolution,
`archive:results-raw/loose/D2_13d_plan1_C.txt`):
- 5.155e10
- 5.795e10
- 6.625e10
- **7.735e10 (S4: leaf 24 → 32 pieces, recip 73 → 92, div 26 → 40)**
- 9.275e10
- 1.0305e11
- 1.0545e11
- 1.1595e11
- 1.2485e11

The C plan also has steps *down*, where auto's grid moves to B-form pieces: 6.445e10, 7.375e10, 1.0435e11 and 1.2275e11.

**G measured (job 21105, s24-16, every run VERIFY OK)** the phases jumping at every pair it ran:

| step | phases below → above | jump |
|---|---|---|
| S3 | 87.8 → 98.1 s | +11.7 % for +1.1 % digits |
| S5 | 149.3 → 156.5 s | +4.8 % |
| S6 | 165.7 → 170.9 s | +3.1 % |
| S9 | 276.3 → 285.3 s | +3.3 % |

S4's lower run (7.70e10) hung after init (G's issue; rc 143).

Size 4 with `DIST_LOGN_TEST=27 NEWTON_MN_GROUPS=0` is the test of the term layout: the S = 2 level's top pair (nodes 2, 3)
splits at 9.337e9 total digits, the S = 4 level at 9.664e9, and the bottom pair at 1.0013e10. `POOL_LOG=29` puts no
mn-tier step in 1e9–2e10.

#### 2. The recalibration (task 2), and what the 6.2 % was

**The 6.2 %**: §80's 129.9 s at 7.64e10 was node_phases (133.2 = init 20.6 + bs 57.5 + dm 55.0) × the design table's
leaf_scale. The leaf_scale (0.949) was fitted at 4e10 as measured / modelled and applied to bs only. Against the measured
19.5 + 57.5 + 60.8:
- **dm was 9.5 % low.** The model priced its big products with S13's *isolated* t_strategy times. In the pipeline, G's logs
  show every product slower (below).
- **bs was scaled 5 % low.** At 4e10 the model's bs was 12 % high, because its top levels lacked the auto grid's B-form
  3·2²⁹ pieces. Scaling to fix that point took 3 s off the target's share.
- **The model's structure also differed from the code's**, in the table below.

##### Fixes to the model's structure

Each fix is checked against L's printer or a real log.

| fix | before | after | check |
|---|---|---|---|
| term shares (`SHARES='terms'`) | every node D digits | the top group's real lengths per level; the top node's leaf | L's tree_max at 401/401 sizes |
| k-way level (tree_level_k) | fold from the bottom child | Horner from the top child (A = child i, B = the running Q) | L's per-level counts |
| A_h in the division | 2 dl + 1 limbs | S >> (nq − 1 − dl) = dl + 1 limbs (the product was 2× too long) | 1.42e11 log: 7888888890 × 7888888891 |
| the division's lengths | nq = D/18 | the code's dl, Q, P, S = Q e; k_mu = P + 1 + dl − nq + 1 (`exact_sizes`) | L's div at 401/401 |
| size-1 top levels | nq/2 × nq/2, nq/4 × nq/4 | binsplit's span tree: every mdev level's pairs (`leaf_shapes`) | L's leaf lengths to the limb |
| auto's grid / form | B pieces at the C plane length | b_len (2^k or 3·2^(k−2)), b_fits, the 0.1 % tie, mul_grid's one-plane rule | L's leaf + div at 801/801 sizes 5e10–1.3e11 |

##### The pipeline's per-product law

**Measured**, fitted by `fit_pipe` on G's 11 logs: 303 products ≥ 2²⁷ limbs, 1381 pieces.
- A one-plane product takes 1.150 × (B form) or 1.221 × (C form) S13's isolated time.
- A grid piece adds 0.075 s (B) per 2³¹ points, plus **0.079 s per 2³¹ limbs of the whole product**. The last term is the
  same for both forms: each piece's add passes over the product's C.

This is the mechanism behind the steps: a 7 × 10 grid of 3·2²⁹ pieces costs 0.94 s per piece where the law said 0.46.

##### The rests and the refit

- **dm's rest** is 4.00 s × D/4e10: the small reciprocal steps, the shifts, the adds and the corrections. Before, it was the
  old phase table's rest, which extrapolated 2.5 × high beyond 10¹¹.
- **bs's rest** is 0.753 (D/4e10)^−0.071 × the phase table's rest.
- **init** is 1.047 (D/4e10)^−0.105 × the model's.

These are fitted (`fit13`) on the 13c runs: 4e10 (n = 5), 7.64e10, and G's ten.

The recalibration is a layer: `run()` under `CAL13`. node_phases' default is still the Phase 13b model, so
`design_table.py --calibrate` keeps its gate. That gate now fails on 2 of its items: the loopback rows at 10¹⁰ / 4 with
both chunkings, at −10.2 % against its 10 % limit. On main they were −9.2 %. The model's division product is now its real
size, so the modelled loopback wall is 1 s lower.

##### The gate — `./mn_model.py --calib13`

| digits | measured wall | model | err | source |
|---|---|---|---|---|
| 4.00e10 | 63.5 | 65.4 | +2.9 % | 13c series, s24-26 (n = 5) |
| 6.59e10 | 109.8 | 109.5 | −0.3 % | G S3 lo |
| 6.66e10 | 121.0 | 117.6 | −2.8 % | G S3 hi |
| 7.20e10 | 126.4 | 127.0 | +0.5 % | G mid |
| 7.64e10 | 137.9 | 136.6 | −1.0 % | share run, s24-30 (× 1.042, the node factor measured by P13b's same-run pair) |
| 7.77e10 | 150.5 | 146.0 | −3.0 % | G S4 hi |
| 8.56e10 | 172.6 | 172.6 | +0.0 % | G S5 lo |
| 8.61e10 | 178.9 | 179.7 | +0.5 % | G S5 hi |
| 9.24e10 | 189.8 | 194.5 | +2.5 % | G S6 lo |
| 9.31e10 | 191.7 | 198.5 | **+3.6 %** | G S6 hi (init 1.8 s below the model's; phases +2.9 %) |
| 1.156e11 | 307.9 | 303.1 | −1.6 % | G S9 lo |
| 1.163e11 | 313.9 | 316.3 | +0.8 % | G S9 hi |
| 1.30e11 | 353.1 | 387.3 | +9.7 % | P13b edge: **RNS_STRATEGY=C, K off**, s24-30 (check, not fitted) |

Eleven of the twelve fitted walls are within 3 %; S6 hi is at +3.6 % (phases +2.9 %). The 1.3e11 point is a different design:
strategy C, K's kernels off, one node slower. Its dm big products, priced by the pipeline law, match its log product by
product within 2 %: 156.5 against about 154 s. Its miss comes from converting its bs and dm to K-on with ratios measured at
4 × 10¹⁰. G's sweep has no run at 1.3e11 on the default. **The gate is not met above 1.163e11.**

Leave-one-out (walls):
- The 4e10 point, left out, comes back +19 %. Every other run of the fit lies at 6.6e10 or above, so 4e10 is an
  extrapolation.
- The rest are within ±4.4 %.

The logs show S9's products 3–7 % slower than the law, and S6's 5 % faster; that variation is the fit's floor.

**A finding for the code (not changed here)**: auto's grid weighs B pieces at 0.70 per point and ignores the per-piece pass
over C. At ≥ 10¹¹ it picks 7 × 10 grids of 3·2²⁹ B pieces. dm at 1.16e11 is 162–171 s on auto; P13b's C at the 3·2³⁰ cap
took 90 s at 1.14e11 (K off). The one-node default may be far from the best above 10¹¹. The 576 target is not affected,
because its distributed products are C-form in the mn tier.

#### 3. Piece counts at 576 against L (task 3)

`./tests/plan_vs_model.py archive:results_raw_2026-10-19/L13d_plan576.txt archive:results_raw_2026-10-19/D2_13d_plan576.txt`: **tree (node 0), tree_max, recip, div and
the per-level counts agree at 401/401 sizes** (2.0–6.0 × 10¹³, step 0.01), and so do the step lists. The earlier
disagreements, and why:
- **tree_max** (the step at 4.30 instead of 4.45): the term layout.
- **the 3-way levels** (16 vs 18 pieces): Horner order.
- **div** (48 vs 28): A_h was 2 dl + 1 limbs.
- **div ±1–2** at 34 sizes: the exact lengths decide which low-cut / high-cut pieces are skipped.

#### 4. The design table and the estimate (task 4)

`results/DESIGN_TABLE.md` is regenerated (`design_table.py --mrun mrun_13b.log`). The default row (auto, 2³¹, shift,
depth 2) now gives **4 × 10¹³ in 3.75 min** (was 3.64). The Pareto front and ranking are unchanged in kind. `estimate.py
--target` prints the headline table above.

**Assumed / not modelled**:
- The mn tier's pieces carry the one-node C factor (1.22, **assumed** to carry over). They get no per-piece pass over C,
  because mn_grid normalises once per product.
- The fabric (as in §79).
- init on the target.

#### RESUME

- 20:15 task 1 delivered; 20:24–20:53 three updates to G.
- 20:35–20:57 the model's structure was brought into line with the C code; L's table matched (401/401).
- 21:05–21:45 G's sweep came in: the pipeline law was fitted on its logs, and CAL13 was refitted (11 of 12 fitted walls within 3 %).
- 21:50 the design table and the estimate were regenerated.
- The standing estimate, target and proposal are in the headline above.
- Open:
  - a default-design run at 1.2–1.3 × 10¹¹, to close the gate's top end;
  - S4's lower point (7.70e10), which hung on G's first try;
  - the 576 C-factor carry-over (1.22), to be measured on the target's first multi-node run.

## C16 — Phase 16 C: the model's inputs measured on aac7 (Slingshot-11, Cray OpenSHMEMX 11.8.0) and `mn_model.py` recalibrated

(former file: C16.md)

Agent C, branch `p16-C` from `origin/p16-A` 26671c6 (2026-10-04). Times **Eastern** (aac7 prints PDT = EDT − 3 h).
Every number is **measured** unless marked modelled / assumed. Outputs on aac7 under `~/p16/C/` (NFS); jobs in `~/p16/C/jobs.txt`.

#### 1. Design

| item | how | code |
|---|---|---|
| 1 injection bandwidth per APU | `tests/t_comm --bw [threads] [reps] [max MiB] [sym\|host] [pairs]` (new, behind the argument): slab sizes 64 KiB … 256 MiB per destination through `comm_alltoall` + `comm_wait`, one communicator (= one SHMEM context) per thread as ecalc's four APU threads have; `sym` = pool-resident buffers (unstaged: the put + signal path alone), `host` = malloc'd (staged through the pool by memcpy; the host-only build has no device); `pairs` on 8 PEs = every PE with the PE n/2 away only (four inter-node pairs, no intra-node traffic). Forms: (a) default (one NIC per PE), (b) `SHMEM_OFI_NUM_NICS=4` (+ `SHMEM_OFI_NIC_POLICY` ROUND-ROBIN / NUMA), (c) 4 PEs per node (t_comm only: ecalc is one process per node by design — `mn.c` drives the four APUs with threads; `mnrun.sh 8` on 2 nodes places 4 PEs per node), (d) sym vs host | `ecalc/tests/t_comm.c` (04d23d6) |
| 2 per-message cost | `tests/t_comm --lat [threads] [reps]`: 8 B … 4 KiB, the exchange's median time / (n − 1) | same |
| 3 overlap | 10¹⁰ on 2 nodes, `COMM_LAYER_STATS=1 COMM_XGMI_STATS=1 COMM_ALLTOALLV_DEPTH=1\|2`, 2 runs each, interleaved | — |
| 4 `T_ROUND` | TARGET_TASKS T11: `MN_T_CHUNK_MB` 1024 / 2048 / 512, 2 runs each, interleaved, same nodes, `RNS_VERBOSE=1`; least squares on the `divmod(mn …) shifts n/t` + `recip(mn) … shifts n/t` figures against the round counts | — |
| 5 mapping rate | `tests/t_alloc 50 hipmalloc hostnuma hostnc` (`T_ALLOC_NTT=0 T_ALLOC_SEED=0`) and init's `region pools` line against `mem init`'s device total, at ROCm 7.0.3 and 7.2.4 | — |
| 6 memory edge | `tests/t_edge host\|dev 8 560` (new): 8 GB steps, touched, until the first failure, then freed; alone on the node | `ecalc/tests/t_edge.c`, a Makefile rule (on demand, not in TESTS) |
| 7 ROCm 7.2.4 A/B | a second clone `~/ntt-C724` built under `module load rocm/7.2.4`; 10¹⁰ ×2 and 10¹¹ ×1 per toolchain on the same node, interleaved; packed outputs compared byte for byte (`cmp`) and the 10¹⁰ against `ref_1e10.txt` (`digcmp.sh`) | — |
| 8 the model | a fabric profile `AAC7` in `mn_model.py` + `estimate.py --fabric aac7` (and env overrides for HIDE / GEN_HIDE / T_ROUND / MAP_RATE), the TARGET profile unchanged | `ecalc/mn_model.py`, `ecalc/estimate.py`, README rows |

Batches (unattended, `setsid nohup`, `_DONE` markers): `~/p16/C/n1.sh` (J1 = 12179, 1 node aac7-n60, 4 h: items 5–7; log `n1.log`, outputs `n1/`),
`~/p16/C/n2.sh` (J2 = 12180, 2 nodes, 5 h: items 1–4; log `n2.log`, outputs `n2/`). Build: `~/p16/C/build.sh` (= A's line in `~/ntt-C`).

#### 2. Measurements

Nodes: J2 = 12180 on aac7-n20 + aac7-n21 (the pair A used); J1 = 12179 on aac7-n60. Logs `~/p16/C/n2/tc_<tag>.log`
(sweeps), `~/p16/C/n2/e10_<tag>.log` (ecalc), `~/p16/C/n1/*.log`. Launch of a sweep: `SLURM_JOB_ID=$J MNRUN_PLAN_POOL=0 ./mnrun.sh <PEs> env
COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_POOL_MB=8192 [SHMEM_OFI_*] ./tests/t_comm --bw <threads> 5 256 [host] [pairs]` (03:25–03:28 EDT).

##### 2.1 Item 1 — injection bandwidth per APU (2 nodes, the put + signal path on pool-resident buffers; `t_comm bw: pe 0` lines, GB/s received)

| form (tag) | 64 KiB | 256 KiB | 1 MiB | 4 MiB | 16 MiB | 64 MiB | 256 MiB per slab |
|---|---|---|---|---|---|---|---|
| (a) default, 1 thread = 1 context (`bw_def_t1_r1`) | 2.6 | 6.6 | 8.9 | **13.8** | 9.6 | 4.7 | 7.9 |
| (a) default, 4 threads, per thread / aggregate (`bw_def_t4_r1`, `_r2`) | 1.7–1.9 / 6.0–6.3 | 2.9–3.1 / 10.8–11.8 | 4.5 / 14.7–17.0 | 4.7–4.8 / **18.7–19.1** | 3.8–4.0 / 15.0–15.8 | 3.4–3.6 / 13.5–14.4 | **3.4–3.7 / 13.5–14.8** |
| (b) `SHMEM_OFI_NUM_NICS=4`, 4 threads (`bw_nic4_t4_r1`, `_r2`) | 1.7–2.1 / 4.4–6.9 | 3.0–3.8 / 11.9–14.0 | 4.5 / 14.5–14.7 | 4.8–4.9 / 19.0–19.2 | 4.0 / 16.0–16.1 | 3.8 / 15.0–15.2 | 3.7–3.8 / 12.5–14.7 |
| (b) `NUM_NICS=4 NIC_POLICY=ROUND-ROBIN`, 4 threads (`bw_nic4rr_t4_r1`) | 1.6 / 4.9 | 3.7 / 14.0 | 4.5 / 14.7 | 4.9 / 19.4 | 3.9 / 15.5 | 3.9 / 12.9 | 3.9 / 12.3 |
| (b) `NUM_NICS=4`, 1 thread (`bw_nic4_t1_r1`) | 2.6 | 6.5 | 9.9 | 14.3 | 10.9 | 7.5 | 7.7 |
| (b) `NIC_POLICY=NUMA` | **refused**: `LIBSMA ERROR: Unable to use a NIC_POLICY of 'NUMA'. Rank 0 cannot determine numa node distances for numa node -1` (`bw_nic4numa_t4_r1`) | | | | | | |
| (d) malloc'd buffers, staged by memcpy through the pool, 4 threads (`bw_def_host_t4_r1`) | 0.8 / 2.1 | 1.8 / 4.2 | 3.4 / 5.9 | 2.2 / 7.9 | 2.2 / 7.8 | 2.0 / 7.9 | 1.7 / 6.7 |
| (c) 4 PEs per node (8 PEs, inter-node pairs, 1 context each), default NICs (`bw_8pe_def_pairs_r1`), per PE at 64 / 128 MiB | | | 12.5 | 14.8 | 8.5 | 6.3 | 6.9–8.2 (128 MiB; the four PEs of a node 6.9, 8.2, 7.5, 8.2) |
| (c) 8 PEs in pairs, `NUM_NICS=4` (`bw_8pe_nic4_pairs_r1`, `_r2`) | | | 12.3–12.5 | 14.3–14.8 | 10.3–10.4 | 7.7–8.2 | 7.6–8.0 per PE (the four PEs of a node 7.6, 7.9, 8.0, 7.9: ≈ 31 GB/s per node, the pairs' loops overlapping but not barrier-aligned; the barrier-aligned repeat is in n3) |

Reading (measured, 2 nodes; the theoretical maximum of one Cassini is 25 GB/s):
- **One context moves at most ≈ 14 GB/s (56 % of a NIC) and only at 4 MiB puts; a 64–256 MiB put runs at 4.7–7.9 GB/s (19–32 %).** The
  transport sends one `putmem_signal_nbi` per pair per exchange, so ecalc's 2²⁸–2²⁹ B slabs sit in the slow regime.
- **Four contexts of one PE reach 13.5–14.8 GB/s aggregate at 256 MiB (54–59 % of one NIC) = 3.4–3.7 GB/s per APU thread** — exactly A's
  post-to-completion 3.7 GB/s per APU thread in ecalc: the staging copies and the readiness wait are not the limit, the put path is.
  At 4 MiB puts the same four contexts reach 18.7–19.4 GB/s (75–78 % of one NIC).
- **`SHMEM_OFI_NUM_NICS=4` changes nothing for one PE per node** (12.5–14.7 vs 13.5–14.8 aggregate; the man page's "each PE is assigned to
  exactly one NIC" holds: the variable spreads *PEs*, not a PE's contexts); ROUND-ROBIN the same; NUMA refuses.
- **4 PEs per node**: every PE keeps the single-context rate (6.9–8.2 GB/s at 128 MiB, ≈ 31 GB/s per node summed) — more than one NIC's 25 GB/s,
  so the PEs do land on different NICs (with and without `NUM_NICS=4`: the default BLOCK policy already spreads 4 PEs over cxi0–3). But ecalc is
  **one process per node by design** (`mn.c`: four APU threads, one communicator each; `mnrun.sh 8` on 2 nodes would give 4 node-processes
  sharing a node's APUs, the aac6 correctness form, not an APU per process) — a 4-PE-per-node ecalc is a redesign, not a switch.
- The staged (host) form is 2× slower than the pool-resident one (6.7–7.9 vs 13.5–14.8 aggregate) — in t_comm's host-only build the staging is a
  memcpy on the sending thread; ecalc's D2H/H2D staging runs on the stream and is not this number (A's 3.7 GB/s equals the pool-resident rate).
- No device-resident symmetric heap exists on Cray OpenSHMEMX 11.8.0 (A16 §1: libsma has no HIP dependence, no device hint in
  `shmem_malloc_with_hints`); `FI_HMEM` is libfabric's, unreachable through libsma. The pool is HIP-registered host hugepages, which on the APU
  is the same HBM — nothing to switch.

**What the launch line should use**: nothing new from (b)/(c) — leave `SHMEM_OFI_*` unset. The lever the sweep found is the **put size**:
`COMM_SHMEM_PUT_MB=4` (new, 4a4e21c, off by default: a put larger than this goes as pieces of this size — `putmem_nbi` ×(k−1), a
`shmem_ctx_fence`, the last piece with the signal) is measured in n3 (§2.9) through t_comm and ecalc: it does not help (the regime is the bytes in flight per context, not the put size).

##### 2.2 Item 2 — per-message cost (`t_comm --lat`, 2 nodes, pool-resident 8 B – 4 KiB, 200 reps, median µs per message = per exchange over 2 PEs)

| form | 8 B | 64 B | 512 B | 4 KiB |
|---|---|---|---|---|
| 1 context (`lat_def_t1_r1`, `_r2`) | **16.9–17.9** | 16.0–17.2 | 16.9–18.1 | 17.9–18.8 |
| 4 contexts, each its own exchange (`lat_def_t4_r1`) | 25.8 (min 18.8) | 22.0 | 22.5 | 21.9 |
| `NUM_NICS=4`, 1 context (`lat_nic4_t1_r1`) | 19.1 | 19.1 | 20.0 | 20.0 |
| `NUM_NICS=4`, 4 contexts (`lat_nic4_t4_r1`) | 23.0 | 23.1 | 22.2 | 23.0 |

One all-to-all over 2 PEs is a publish (a put of the landing offset), a put + signal and a wait on the signal word on each side, so the
measured ≈ 17 µs is **≈ 2 round trips of the transport's handshake (≈ 8.5 µs per put + signal + wait, measured)** — against the model's 2 µs
assumed and TARGET_HW_REVIEW's 3.75–6.33 µs proxy. With four contexts busy at once each exchange costs 22–26 µs (the progress thread serves all).
Fed to the model as `lat = 8.5e-6` per message (the model counts one message per pair per exchange; the handshake's second trip is in it).

##### 2.3 Item 3 — the overlap of the two fabrics (10¹⁰ on 2 nodes, `COMM_LAYER_STATS=1 COMM_XGMI_STATS=1`, the outputs on the nodes' `/tmp`)

Launch: `SLURM_JOB_ID=12180 ./mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_CHECKPOINT=1 ECALC_VERBOSE=2
COMM_LAYER_STATS=1 COMM_XGMI_STATS=1 COMM_ALLTOALLV_DEPTH=<d> ./ecalc 10000000000 /tmp/C/e10_<tag>.out` (03:38–03:41 EDT; logs `n2/e10_ov_<tag>.log`).

| run | `total` | init | dm | all-to-all per PE (5508 exchanges) | span per APU thread | xGMI stage | fabric stage | BOTH | xGMI link-active | BOTH link+fabric / link time |
|---|---|---|---|---|---|---|---|---|---|---|
| depth 2, r1 | 35.27 s | 16.1 | 13.5 | 50.0 s summed, 186.6 GB, **3.73 GB/s per APU thread** (pe 1: 3.62) | 10.72 s | 2.98 s | 10.27 s | 2.63 s | 0.267 s (9.0 %, 217 GB/s per APU) | **71.3 %** (node 1: 70.9) |
| depth 2, r2 | 36.73 s | 17.3 | 13.8 | 51.0 s, 3.66 (3.57) | 10.91 | 2.96 | 10.47 | 2.62 | 0.266 s | **71.8 %** |
| depth 1, r1 | 35.59 s | 16.2 | 13.7 | 50.6 s, 3.69 (3.65) | 10.68 | 2.97 | 10.23 | 2.61 | 0.326 s (11.0 %, 178 GB/s) | **76.2 %** |
| depth 1, r2 | (below) | | | | | | | | | |

- **`HIDE_POW2` on aac7 = 0.71–0.72 at depth 2 (the default), 0.76 at depth 1** (measured, 2 nodes, the equal-slab path) — the same 0.75 class
  as aac6's (X13: 0.74–0.76). The fabric stage is 96 % of the span (10.3 of 10.7 s per APU thread; xGMI-only 0.35 s, fabric-only 7.6–7.9 s):
  at 3.6 GB/s per APU thread the xGMI leg (217 GB/s per APU, link-active 0.27 s) is noise under the Slingshot leg, so the depth choice is
  worth < 0.1 s here; `COMM_ALLTOALLV_DEPTH=2` stays.
- The general map (`GEN_HIDE_DEPTH`, a node count that is not a power of two) needs 3 nodes; not run (the integrator's node priority to B's
  8-node job) — aac6's 0.011 / 0.74 stay in the profile, **assumed** for aac7.
- **The 2-node 1e10 wall without NFS in the way is 35.3–36.7 s** (init 16.1–17.3 — 10 s of it the communicator handshake, A16 §5; bs 5.5;
  dm 13.5–13.8) against A's 54–62 s with the part file on NFS (A's `dm` 19.3–21.8: `MN_OUT_EARLY` streams the file during the division and
  NFS at ≈ 0.1 GB/s held it). The model's 2-node check (§2.8) uses these.

##### 2.4 Item 4 — `T_ROUND` (10¹⁰ on 2 nodes, `RNS_VERBOSE=1 MN_T_CHUNK_MB=1024 | 2048 | 512`, interleaved; logs `n2/e10_tr_<mb>_r<n>.log`, n3 for the repeats)

| run | `total` | dm | `recip(mn)` shifts n/t | `divmod(mn, DKM)` shifts n/t | window temporaries (`scratch(mn)`) |
|---|---|---|---|---|---|
| 1024 r1 (parts on NFS: dc 32.5 s, `total` 66.97) | 66.97 | 13.8 | 58 / 0.63 s | 71 / 1.29 s | 2.222 GB |
| 2048 r1 | 39.94 (recip's single-node chain 7.68 s: a 5 s hiccup there, not in the shifts) | 18.8 | 58 / 0.55 s | 71 / 1.20 s | 2.222 GB |
| 512 r1 | 34.16 | 14.1 | 58 / 0.59 s | 71 / 1.23 s | 4.295 GB |
| 1024 (the depth-1 overlap run, `ov_d1_r2`) | 36.04 | 13.6 | — | 71 / 1.09 s | 2.222 GB |
| r2 of each | (n3, below) | | | | |

At 10¹⁰ on 2 nodes every window is one round whatever the chunk (the counts stay 58 / 71), so the chunk size cannot separate a per-round
fixed cost here — T11's procedure needs the 64-node size. What the series does give is **the whole cost of one shift round, data included:
divmod 1.09–1.29 s / 71 = 15–18 ms, recip 0.55–0.63 s / 58 = 9.5–11 ms per round** (measured), and the spread between the chunk sizes
(≤ 0.2 s over 71 rounds) bounds the extra fixed cost of a round at **≤ 3 ms**. So `T_ROUND` on aac7 is **≤ 0.015 s (upper bound: the full
round, measured) and the fixed part ≤ 0.003 s** — below the assumed 0.03 s; the profile takes 0.015 s (the bound) and the sensitivity
0.003–0.03 s is shown in §2.8. `COMM_SHMEM_ROUND_MB` 1024 vs 512: §2.6.

##### 2.5 Item 5 — the mapping rate at ROCm 7.0.3 (and 7.2.4), one node aac7-n60 (J1 12179)

`T_ALLOC_NTT=0 T_ALLOC_SEED=0 ./tests/t_alloc 50 hipmalloc hostnuma hostnc` (50 GB per APU on all four at once; `s/GB*` = the slowest thread's
wall / the bytes of all APUs; logs `n1/t_alloc_703.log`, `n1/t_alloc_724.log`):

| form | ROCm 7.0.3 alloc s / s/GB* | ROCm 7.2.4 alloc s / s/GB* |
|---|---|---|
| hipmalloc | 13.96 / **0.070** | 13.96 / **0.070** |
| hostnuma (`hipHostMalloc` NumaUser, mem.c's pool form) | 39.33 / 0.197 | 39.06 / 0.195 |
| hostnc | 39.1 / 0.196 | 39.13 / 0.196 |

- **`MAP_RATE` = 0.070 s/GB on aac7 at both toolchains** (aac6: 0.057–0.072, the model's 0.065) — the `hipMalloc` mapping rate is not the
  7.0.3 difference.
- **ecalc's init is**: 1e11 at 7.0.3 `region pools 16.52 / 18.30 s`, `rns_init 22.17 / 24.16 s` (mine / A's) against **11.02 / 16.21 s at 7.2.4**;
  1e10: `region pools 2.53–2.61`, `rns_init 8.3–8.9` at both. So the 387 GB of 1e11 pools map at 0.043–0.047 s/GB under 7.0.3 and 0.028 s/GB under
  7.2.4 (the VMM arena path, `DB_POOL_VMM`; measured, one run each) — the toolchain costs init ≈ 6 s at 1e11 (`INIT_SCALE` 1.37 at 7.0.3
  against the model's 18.4 s; 0.88 at 7.2.4).

##### 2.6 Item 6 — the memory edge (one node, alone; `tests/t_edge <form> 8 560`, logs `n1/edge_host.log`, `n1/edge_dev.log`)

| form | result |
|---|---|
| `hipHostMalloc` NumaUser (the pool form), 8 GB steps, touched | **504 GB committed (63 steps, MemAvailable 18.7 GB left) — the 64th step (512 GB) was OOM-killed** (`Detected 1 oom_kill event in StepId=12179.7`; the step died, the node stayed up, MemAvailable back to 525 GB) |
| `hipMalloc`, 8 GB steps, `hipMemset` | **520 GB committed (65 steps); the 66th (528 GB) failed cleanly: `out of memory`**; freed, MemAvailable 523.8 GB |

The node (RealMemory 500,000 MiB = 524 GB, MemAvailable 524–525 GB idle) holds **≈ 520 GB of device allocations or ≈ 504–512 GB of host-pool
allocations** before the kill — aac6's 524 GB class; the 480 GB budget keeps a 24–40 GB margin. (`ECALC_MEM_GUARD_GB`'s named stop was not
exercised: it needs a run whose modelled peak is 515–525 GB on one node ≈ 1.3 × 10¹¹, 5 min; not spent.)

##### 2.7 Item 7 — ROCm 7.2.4 against 7.0.3, the same node (aac7-n60), interleaved; clone `~/ntt-C724` built under `module load rocm/7.2.4`

| run | ROCm 7.0.3 `total` (bs / dm / init) | ROCm 7.2.4 `total` (bs / dm / init) | digits |
|---|---|---|---|
| 10¹⁰ (`/tmp`) ×3 / ×2 | 26.92 (9.0 / 6.3 / 8.9), 25.98 (8.5 / 6.3 / 8.3), 27.71 (9.5 / 6.3 / 9.0) | 26.18 (8.8 / 6.1 / 8.6), 26.27 (9.4 / 6.2 / 8.1) | packed outputs byte-identical across the toolchains (`cmp`, `E10_703_724_IDENTICAL`); 7.0.3 r1 `digcmp.sh` against `ref_1e10.txt`: **identical** |
| 10¹¹ (NFS) ×1 each | **205.92** (95.1 / 88.0 / 22.2); A's 206.73 (96.8 / 85.3 / 24.2) | **168.45** (79.0 / 72.9 / 16.2) | `cmp` of the two 44 GB packed files: (n1's last step; below) |

**The 7.0.3 slowdown is the toolchain, not the node**: 7.2.4 on the same node gives 168.5 s = aac6's 166.7 ± 1.2 s class (RESULTS §94), and
7.0.3 costs **+37 s (+22 %) at 10¹¹** — bs +16 s, dm +15 s, init +6 s — while 10¹⁰ is unchanged (26–28 s both). The target's ROCm is 7.0.3
(TARGET_HW_REVIEW §2 row 9), so the number to report for the target is the 7.0.3 one: `NODE_SCALE` 1.22 at 10¹¹ (the 7.2.4 build would recover
it if the target's module set allowed it — a question for the target's admins, not a switch of ours).

##### 2.8 Item 8 — the model recalibrated: the `AAC7` profile (`estimate.py --fabric aac7`, `MN_MODEL_PROFILE=aac7`; 9dbb2c7)

`mn_model.AAC7` = Fabric(bw_apu 3.6 GB/s, lat 8.5 µs, write_bw 0.116 GB/s) and `AAC7_CONSTS` = HIDE_POW2 0.72, GEN_HIDE 0.011 / 0.74
(aac6's, assumed), T_ROUND 0.015, MAP_RATE 0.070, NODE_SCALE 1.12, INIT_SCALE 1.20 (fitted on the 7.0.3 one-node 10¹¹: 205.9 = 1.12 ×
183.8; init 22.2 = 1.20 × 18.4). The TARGET profile and every constant stay the defaults; `MN_MODEL_HIDE_POW2` / `GEN_HIDE1` / `GEN_HIDE2` /
`MAP_RATE` / `NODE_SCALE` / `INIT_SCALE` override either (README rows). `./estimate.py --fabric aac7 --g <g> --D 1e10` (no-write wall) against
the measured walls (10¹⁰ per node, outputs on `/tmp`, `ECALC_CHECKPOINT` as noted):

| g | model, profile bw 3.6 | model at the measured bw of that g | measured wall (`total`) | error | the measured fabric rate per APU thread |
|---|---|---|---|---|---|
| 1 | 23.9 (1e10), 207.0 (1e11) | — | 26.0–27.7 (1e10, ×3); 205.9 (1e11) | −10 % / +0.5 % (1e11 fitted) | — |
| 2 | **50.7** | 50.7 | **35.3–36.7** (ov_d*, 4 runs; ckpt on) | **+40 %** | 3.6–3.7 GB/s (C16 2.3) |
| 4 | 68.0 | **77.8** (`--bw 2.9`) | **87.3** (B's e10d, ckpt off, `~/p16/B/n4/e10d.log`) | **−11 %** | 2.89 GB/s (B) |
| 8 | 81.3 | **219.2** (`--bw 1.0`) | **327.1** (B's 8-node e10d: bs 118.6, dm 187.6 — B's open issue) | **−33 %** | 0.99 GB/s (B) |
| 12 | 87.9 | — | (no run) | — | — |

Where the model is off and why (measured against the model's `--verbose` breakdown):
- **g = 2, +40 %**: the model moves 86.7 GB per APU over the NIC (346.8 GB per node: level 62.7 + reciprocal 94.0 + division 190.2) and prices
  it at 3.6 GB/s = 24.2 s exposed; the code moved **38.7 GB per APU thread** (`layer-stats … fabric bytes 38.68 GB`, 10.3 s of fabric stage).
  The byte law of the 2-node division / reciprocal pieces is 2.2× the code's — a structural term of the model at small g (its calibration is
  the 576-node plan), not a fabric input; with the code's bytes the 2-node wall comes out at 36.6 s (= measured).
- **g = 4, −11 % with B's measured 2.9 GB/s** (−22 % at the profile's 3.6): within the ± 10 % goal only once the fabric rate of that g is given.
- **g = 8**: B's run at 0.99 GB/s per APU thread (8× worse than 2 nodes: 6944 exchanges, 825 s summed) is the thing to explain before the model
  is judged there; at `--bw 1.0` the model still sits 33 % under it.
- **The fabric rate falls with the node count: 3.6 (2) → 2.9 (4) → 1.0 (8) GB/s per APU thread** — one NIC per PE shared by (g − 1) peers'
  puts and the single-context rate at large messages (§2.1). The profile's constant 3.6 is the 2-node value; use `--bw` with the measured
  rate of the node count until the transport's put size is fixed (`COMM_SHMEM_PUT_MB`, n3) and the rate re-measured at 4–8 nodes.
- `T_ROUND` sensitivity at the target (`MN_MODEL_T_ROUND=0.003 | 0.015 | 0.03`, `--target`): the gain of `MN_T_CHUNK_MB=2048` scales with it (int15k:
  −10.9 s at 0.03 → ≈ −5.5 s at 0.015, ≈ −1 s at 0.003, modelled).

##### 2.9 n3 — `COMM_SHMEM_PUT_MB`, the barrier-aligned 8-PE pairs, the T_ROUND repeats (job 12194, aac7-n41 + aac7-n50, 12:05–12:14 EDT; clone `~/ntt-C2` at 4a4e21c; logs `~/p16/C/n3/`; 8 ecalc launches, 0 failures)

`t_comm --bw 4 20 256` (4 contexts, pool-resident; aggregate GB/s per node at 4 / 16 / 64 / 256 MiB per slab):

| form (tag) | 4 MiB | 16 MiB | 64 MiB | 256 MiB |
|---|---|---|---|---|
| default puts (`bw_def_t4_r3`) | 19.5 | 15.3 | 13.6 | 13.4 |
| `COMM_SHMEM_PUT_MB=4` (`bw_put4_t4_r1`, `_r2`) | 19.6 | 17.0–19.6 | 14.1–14.6 | 13.2–14.7 |
| `=1` (`bw_put1_t4_r1`) | **21.0** | 17.5 | 14.6 | 13.9 |
| `=16` / `=64` | 19.5 / 19.4 | 15.2 / 15.1 | 13.9 / 13.4 | 13.2 / 13.5 |
| `=4`, `SHMEM_OFI_NUM_NICS=4` (`bw_put4_nic4_t4_r1`) | 19.6 | 15.8 | 14.0 | 13.9 |
| `=4`, 1 context (`bw_put4_t1_r1`) | 14.8 | 9.0 | 7.6 | 7.2 |
| `=4`, malloc'd (staged) buffers | 8.7 | 10.3 | 9.4 | 9.9 |

- **Splitting the puts does not recover the 4 MiB rate**: a 256 MiB slab sent as 64 puts of 4 MiB runs at 13.2–14.7 GB/s aggregate (default
  13.4), as 256 puts of 1 MiB at 13.9; one context 7.2 GB/s either way. The regime is set by the bytes in flight per context, not by the size of one
  `putmem_signal_nbi` — the NIC/progress path pipelines the pieces as it did the whole. `NUM_NICS=4` at 4 MiB pieces: unchanged (19.6). The
  switch stays in (off by default) as measured: **not worth the launch line**.
- **ecalc 10¹⁰ on 2 nodes, interleaved**: `COMM_SHMEM_PUT_MB=4` 36.00 / 35.39 s (3.85 / 3.89 GB/s per APU thread post-to-completion), default
  35.45 / 35.36 s (3.72 / 3.70), `=16` 36.56 s (3.68) — **+4 % on the exchange rate, 0 on the wall** (within the spread); digits identical (the
  parts' sha1 over all 8 runs: 2 distinct hashes, one per node).
- **4 PEs per node, barrier-aligned (`pairs`, 20 reps, 128 MiB)**: every PE 7.9–8.2 GB/s, the four pairs' walls 0.326–0.340 s **overlapping —
  ≈ 32 GB/s per node received, over four NICs** (default policy and `NUM_NICS=4` alike; `PUT_MB=4` the same). So the hardware gives a node
  4 × 8 GB/s with one context per NIC against 13–15 GB/s for one PE's four contexts on one NIC: **the one-NIC-per-PE assignment costs the
  node 55 % of its injection** — the lever is a design one (one PE per APU, or a library that spreads a PE's contexts over NICs), not a switch.
- T_ROUND repeats (r2): 1024 → recip 58 / 0.59 s, divmod 71 / 1.24 s; 2048 → 0.60 / 1.24; 512 → 0.57 / 1.20 (`total` 36.3 / 35.5 / 33.2): the
  chunk size makes no difference at this size (§2.4 stands: the whole round 15–18 ms, the fixed part ≤ 3 ms).

#### 2.10 The inputs (the table for the model; measured unless marked)

| input | value | spread | command / log |
|---|---|---|---|
| injection per APU thread, ecalc's exchange sizes, 1 PE per node | **3.6 GB/s** (13.5–14.8 per node) | 3.3–3.9 | `t_comm --bw 4 5 256`, `n2/tc_bw_def_t4_r*`; ecalc 3.59–3.89 (`n2/e10_ov_*`, `n3/e10_*`) |
| the same at 4 MiB puts / one context's peak | 19.5 per node / 14 | 18.7–21.0 / 13.8–14.8 | `n2/tc_bw_def_t*`, `n3/tc_bw_put*` |
| node injection with 4 PEs (one per NIC) | 32 GB/s per node | 31.6–32.5 | `n3/tc_bw_8pe_*_bar_*` |
| per-message cost | 8.5 µs per put + signal + wait (17 µs per 2-PE exchange) | 16–19 µs | `t_comm --lat 1 200`, `n2/tc_lat_def_t1_r*` |
| `HIDE_POW2` | 0.72 at depth 2 (0.76 at depth 1) | 0.709–0.718 / 0.758–0.771 | `n2/e10_ov_d2_r*`, `_d1_r*` |
| `GEN_HIDE_DEPTH` | aac6's 0.011 / 0.74 **assumed** (no 3-node run) | — | — |
| `T_ROUND` | ≤ 0.015 s (the full round; fixed part ≤ 0.003) | 15–18 ms per divmod round | `n2/e10_tr_*`, `n3/e10_tr_*` |
| `MAP_RATE` | 0.070 s/GB (hipMalloc), both toolchains | 0.070 | `n1/t_alloc_703.log`, `_724.log` |
| init at 1e11 | 22.2 s (7.0.3) / 16.2 s (7.2.4) | A's 24.2 | `n1/e11_703.log`, `n1/e11_724.log` |
| memory edge | 520 GB device (hipMalloc) / 504 GB host pool (then OOM-kill) | — | `n1/edge_dev.log`, `n1/edge_host.log` |
| ROCm 7.2.4 vs 7.0.3 at 1e11 | **168.5 vs 205.9 s** (bs 79.0 / 95.1, dm 72.9 / 88.0, init 16.2 / 22.2); 1e10 26.2 vs 26.0–27.7 | 1 run each at 1e11 | `n1/e11_*.log`; `E11_703_724_IDENTICAL` (cmp of the 44 GB packed files), `E10_703_724_IDENTICAL`, `digcmp` vs ref_1e10 identical |
| part file on NFS | 0.116 GB/s (A16) | — | A's 1e11 write |


#### 3. Open issues
- **The comm_shmem_init segfault + PE-0 hang (A16 §4)** hit n2's first 1e10 launch (`n2/e10_ov_d2_r1.hang1.log`: `inet_recv: unexpected socket
  EOF` flood from 00:28 PDT); n2.sh now runs every launch under a watcher (the log's EOF/segfault lines → the steps cancelled, stray `ecalc`
  killed by exact name on the two exclusive nodes, up to 3 launches per run) and counts launches / failures.
- **n2's launch count**: 3 genuine failures of 10 launches (two `shmem_init` segfaults with the EOF flood, one silent PE-0 death at init
  with PE 1 alive, `n2/e10_tr_1024_r2.try2.log`); 7 further launches were killed by my own login-side stall watcher, which compared the
  NFS file mtime (the file server's clock is ≈ 6.8 h behind the login node's) — removed; n3.sh's watcher uses log size, not time.
- **A cancelled SHMEM step breaks the node's HSN for the rest of the job**: from 01:02 PDT every launch on aac7-n20 failed with
  `srun: error: task 0 launch failed: Error configuring interconnect` (then PE 1's `PMI2_Init failed` after 300 connect attempts) — 16 launches
  in a row (n2's `rm_*`, `nic4_r1`; n3's first run on the same job), after my watcher's `scancel -s KILL` of a step + `pkill -KILL -x ecalc`.
  A fresh allocation is the cure; the switch plugin's VNI state is the suspect. n2's genuine failures before that: 3 of 10 launches (A16's
  segfault class, two with the EOF flood, one silent PE-0 death at init). Digits: `digcmp.sh` of `tr_1024_r1` against `ref_1e10.txt` identical;
  the parts' sha1 over all 10 completed n2 runs: 2 distinct hashes (one per node) — every run identical.
- `SHMEM_OFI_NIC_POLICY=NUMA` refuses on these nodes (numa node −1 for the rank).

#### RESUME
- 2026-10-04 12:25 EDT: **done.** n3 on job 12194 (8 launches, 0 failures; the job cancelled by `n3wrap.sh`); §2.9–2.10 written; no job of
  mine left on aac7 (`squeue -u <user> | grep C16` empty). Branch p16-C pushed. Not done: a 3-node run for `GEN_HIDE` (assumed aac6's).
- 2026-10-04 12:05 EDT: n1 done (`N1_DONE`, 12179 expired), n2 done (`N2_DONE`; 12180 expired — the nodes refused launches after 01:02 PDT:
  `Error configuring interconnect`, §3); n3 rerun on a fresh 2-node job (`~/p16/C/jobs.txt` J2=<id>, `n3wrap.sh` cancels it after `N3_DONE`):
  the `COMM_SHMEM_PUT_MB` sweep, the barrier-aligned 8-PE pairs, ecalc with PUT_MB 4 / 16, the T_ROUND repeats. §2.1–2.8 written, the
  profile in (9dbb2c7), README rows added (uncommitted → commit). Left: §2.9 from n3, the final table, cancel the job if n3 fails.
- 2026-10-04 03:50 EDT: §2.1–2.2 written (sweeps done). n1 (J1 12179): e10 ×2, e11 7.0.3 (205.9 s) done, the edge probe running, then the
  7.2.4 clone/build and runs → `N1_DONE`. n2 (J2 12180, relaunched with the watcher, `SKIP_TC=1`; the sweeps' logs kept in `n2/`): the
  overlap / T_ROUND ecalc runs → `N2_DONE`; then `n3.sh` (waits for N2_DONE; clone `~/ntt-C2` at 4a4e21c): `COMM_SHMEM_PUT_MB` sweeps + ecalc,
  the 8-PE barrier-aligned pairs → `N3_DONE`. Then: §2.3–2.7, item 8 (the model), README rows, cancel 12179 / 12180.
- 2026-10-04 03:15 EDT: worktree + branch p16-C (04d23d6 t_comm sweep, t_edge); aac7 clone `~/ntt-C` built (`~/p16/C/build.log`);
  J1 = 12179 (1 node, running `n1.sh` → `N1_DONE`), J2 = 12180 (2 nodes, pending; `n2.sh` waits for it → `N2_DONE`). **Cancel both at the
  end** (`scancel 12179 12180`). Next: read `n1.log` / `n2.log`, fill §2, then item 8 (the model) and the README rows.

## F16 — the 576-node figures re-estimated from the inputs measured on aac7 (Phase 16 C), with bands

(former file: F16.md)

Agent F, phase 16 (2026-10-04). Branch `p16-F` from `main` dbde0c7, worktree `~/apucode/wt-p16-F`. Local python only
(`ecalc/estimate.py`, `ecalc/mn_model.py`); no cluster. Every number below is **modelled** (the B3 model on the launch line, `MN_OUT_DKM_HI=1`,
`RNS_DIST_CACHE_PARTIAL=1`: the pool rule, P24 cached, the loop along the longer axis) from inputs labelled **measured** (aac7, C16 §2.10) or
**assumed** (the target). Two walls per cell: **without the part file / with it packed at 1.0 GB/s per node** (the write rate: the user's one-node
Lustre test, 2026-09-29, **assumed** to hold with 576 writers). Both test sizes: 5.276 × 10¹³ and 5.167 × 10¹³ on 576 nodes.

#### 1. Design

- The model's knobs for C's inputs already existed as environment overrides (`MN_MODEL_HIDE_POW2`, `MN_MODEL_T_ROUND`, `MN_MODEL_MAP_RATE`,
  `MN_MODEL_NODE_SCALE`, `MN_MODEL_INIT_SCALE`). Added to `estimate.py` (defaults `None` = unchanged): `--hide-pow2`, `--t-round`, and
  **`--local-factor F`** — one factor on the local (non-fabric) terms: `NODE_SCALE` (bs, the leaf, the pieces' local passes, the seed wait) and
  `INIT_SCALE` (init) together (`MN_MODEL_INIT_SCALE` in the environment splits init off). `mn_model.py`: env **`MN_MODEL_LOCAL_F`** = the same.
  Default 1.0 everywhere; `./estimate.py --target` is byte-identical at the standing row (284.2 / 299.3 s).
- `estimate.py --target`'s partial-cache block now also prints the **second test size's row** (5.167 × 10¹³; one added line).
- Each row below is one process (`NODE_SCALE` is not in the product cache's key, so the rows must not share a process); the driver
  (scratchpad `row.py`) calls `mn_model._run_cache` with the pool rule's primes and `estimate.estimate` for the node memory — the same
  arithmetic as `estimate.py --target`'s partial row (checked: 284.2 / 299.3 and, with the E inputs, 316.9 / 329.2 both ways).

#### 2. The inputs

| input | standing (TARGET profile) | this re-estimate | label |
|---|---|---|---|
| injection per APU | 100 GB/s | **100 GB/s** (PLAN §25: two 400 Gb/s NICs per APU) | **assumed** — the target has no network measurement; aac7 measured 3.6 GB/s per APU thread at one PE per node (one NIC per PE), 32 GB/s per node at 4 PEs |
| per-message cost | 2 µs | **8.5 µs** per put + signal + wait | **measured** (Slingshot-11 + Cray OpenSHMEMX, 2 nodes; 16–19 µs per 2-PE exchange) |
| `T_ROUND` | 0.030 s (fitted on aac6 loopback ± 100 %) | **0.015 s** | **measured** upper bound (15–18 ms per divmod round, fixed part ≤ 3 ms) |
| `HIDE_POW2` | 0.75 (aac6) | **0.72** | **measured** (depth 2, 0.709–0.718; depth 1 0.76) |
| `GEN_HIDE_DEPTH` | 0.011 / 0.74 | 0.011 / 0.74 | **assumed** (aac6's; no 3-node run on aac7) |
| `MAP_RATE` | 0.065 s/GB (aac6) | **0.070 s/GB** | **measured** (t_alloc, 7.0.3 = 7.2.4) |
| the local factor (toolchain) | 1.0 (aac6 at ROCm 7.2.4 = the model's calibration) | **1.0 for the ROCm 7.2.4 stack; 1.22 for ROCm 7.0.3** | **measured** at 10¹¹ on one aac7 node: bs 95.1 / 79.0 = 1.204, dm 88.0 / 72.9 = 1.207, init 22.2 / 16.2 = 1.37, total 205.9 / 168.5 = 1.22; aac7 at 7.2.4 is aac6's wall (168.5 vs 166.7 ± 1.2) |
| part file | 1.0 GB/s per node | 1.0 GB/s per node | **assumed** (one node measured; the aggregate with 576 writers open: T1) |
| dragonfly group / taper | 64 / 1.0 | 64 / 1.0 | **assumed** (no fabric data for the target) |

Two stacks are reported in every table (the user's directive): **7.2.4** = the best aac7 stack, local factor 1.0 (the model's aac6 calibration,
which aac7 at 7.2.4 reproduces within 1 %); **7.0.3** = the target's installed version, local factor 1.22 (measured). The AAC7 profile's fitted
split (`NODE_SCALE` 1.12, `INIT_SCALE` 1.20, fitted on the one-node 10¹¹ against the model's 183.8 s) gives 316.9 / 329.2 s and 312.6 / 323.0 s on
the E inputs — between the two columns; it is the model-relative factor, the 1.22 column the measured ratio (the model's one-node 10¹¹ at 7.2.4 is
183.8 s against 166.7–168.5 measured, so 1.22 on the model over-prices 7.0.3 by ≈ 8 %: the 7.0.3 column is a ceiling).

#### 3. The re-estimate (modelled; s without the write / with the packed write at 1.0 GB/s per node; `MN_OUT_DKM_HI=1`)

| row | inputs | 5.276 × 10¹³, 7.2.4 (×1.0) | 5.276 × 10¹³, 7.0.3 (×1.22) | 5.167 × 10¹³, 7.2.4 | 5.167 × 10¹³, 7.0.3 | node GB |
|---|---|---|---|---|---|---|
| **S** the standing row | 100 GB/s, 2 µs, T_ROUND 0.03, HIDE 0.75, MAP 0.065 | **284.2 / 299.3** (reproduced) | 330.0 / 342.6 | 280.2 / 293.8 (§94 said 278.5 / 293.0: see §6) | 325.5 / 336.3 | 471.9 / 463.3 |
| **E** the estimate: the design fabric with C's inputs | 100 GB/s (assumed), 8.5 µs, T_ROUND 0.015, HIDE 0.72, MAP 0.070 (all measured) | **290.2 / 303.9** | **336.3 / 347.4** | **286.2 / 298.1** | **331.7 / 340.9** | 471.9 / 463.3 |
| E − lat only (8.5 µs on S) | | 299.7 / 311.8 | — | 295.5 / 306.0 | — | |
| E − T_ROUND only (0.015 on S) | | 274.4 / 291.0 | — | 270.6 / 285.6 | — | |
| E − HIDE only (0.72 on S) | | 284.5 / 299.6 | — | 280.6 / 294.1 | — | |
| **(a)** E at 50 GB/s per APU | one 400 Gb/s NIC per APU | 348.2 / 355.6 | 394.0 / 398.9 | 343.5 / 349.0 | 388.7 / 391.5 | unchanged |
| **(a)** E at 25 GB/s per APU | one 200 Gb/s NIC per APU (the aac7 NIC class) | 463.5 / 458.6 | 509.4 / 503.8 | 457.6 / 452.1 | 502.8 / 497.4 | unchanged |
| **(a)** E at 8 GB/s per APU | 4 PEs per node, one per NIC: 32 GB/s per node (measured on aac7) | 953.7 / 948.2 | 999.5 / 994.0 | 942.5 / 937.0 | 987.6 / 982.2 | unchanged |
| **(a)** E at 3.6 GB/s per APU thread | 1 PE per node, one NIC per PE: what the code does today on a Cray with 4 NICs per node (measured on 2 aac7 nodes) | 1834.1 / 1828.5 | 1879.9 / 1874.3 | 1813.1 / 1807.6 | 1858.3 / 1852.8 | unchanged |
| **(b)** E at 2 µs | the latency band's low end (S's assumption) | 274.7 / 291.3 | 320.6 / 334.6 | 270.9 / 285.9 | 316.2 / 328.5 | unchanged |
| **(c)** E + the APU-binning tail | +0 … +10 s (assumed, TARGET_HW_REVIEW §2 item 10; not in the model) | 290.2 … 300.2 / 303.9 … 313.9 | 336.3 … 346.3 / 347.4 … 357.4 | 286.2 … 296.2 / 298.1 … 308.1 | 331.7 … 341.7 / 340.9 … 350.9 | unchanged |
| **(d)** E at 1.0 GB/s per APU thread | the 8-node fall-off (C §3 a: 3.6 → 2.9 → 1.0 GB/s at 2 / 4 / 8 nodes): **an open risk, not the estimate** | 5994 / 5989 | 6040 / 6035 | 5928 / 5922 | 5973 / 5967 | unchanged |
| **(e)** E with `MN_T_CHUNK_MB=2048` | at T_ROUND 0.015 | 276.6 / 291.5 (−13.6 / −12.4) | 321.6 / 334.0 (−14.7 / −13.4) | 270.8 / 285.0 (−15.4 / −13.1) | 314.9 / 326.6 (−16.8 / −14.3) | 471.9 / 471.9 (+8.6 GB at 5.167: the larger chunk's pool) |
| (e) at S (T_ROUND 0.03, for reference) | RESULTS §94 gave −10.9 s on the base row | 267.0 / 283.8 (−17.2 / −15.5) | — | 261.5 / 277.6 | — | |
| taper 0.5 (the global links at half the injection) | not asked; the 576-node taper is the other assumed input | 311.8 / 320.6 | 357.8 / 364.1 | 307.4 / 314.3 | 352.8 / 357.0 | unchanged |

Phase breakdown of E at 5.276 × 10¹³ (7.2.4 / 7.0.3): init 20.6 / 25.2, seed wait 12.4 / 15.1, bs 25.1 / 30.6, top 28.8 / 35.1, distributed
levels 135.2 / 155.4, reciprocal 17.1 / 18.8, division 43.0 / 48.0; the fabric's exposed share 72.0 / 72.5 s (S: 66.3; lat alone +15.1 s,
T_ROUND alone −9.8 s, HIDE −0.3 s). At 3.6 GB/s the exposed share is 1616 s — the fabric is the wall by 6 ×.

**The node memory** (modelled, unchanged by the fabric and the toolchain): 471.9 GB at 5.276 × 10¹³ (427.6 device + 44.4 host; pool 9.9),
463.3 GB at 5.167 × 10¹³, against the **480 GB budget**; the aac7 edge **measured** at 520 GB device (hipMalloc) / 504 GB host pool before the
OOM killer (C16 §2.6; aac6 524 GB) — 32–48 GB above the budget, 8–16 GB less than aac6's edge was taken to be.

#### 4. Commands (from `ecalc/`, all with `MN_OUT_DKM_HI=1`; the partial-cache rows of `--target`)

- S: `./estimate.py --target` → 284.2 / 299.3 (5.276) and 280.2 / 293.8 (5.167).
- E, 7.2.4: `MN_MODEL_MAP_RATE=0.070 ./estimate.py --target --lat 8.5e-6 --hide-pow2 0.72 --t-round 0.015` → 290.2 / 303.9, 286.2 / 298.1.
- E, 7.0.3: the same with `--local-factor 1.22` → 336.3 / 347.4, 331.7 / 340.9 (or env `MN_MODEL_LOCAL_F=1.22`).
- The AAC7 fit: `MN_MODEL_MAP_RATE=0.070 MN_MODEL_INIT_SCALE=1.20 ./estimate.py --target --lat 8.5e-6 --hide-pow2 0.72 --t-round 0.015 --local-factor 1.12`
  → 316.9 / 329.2, 312.6 / 323.0 (checked against the env-only form `MN_MODEL_LOCAL_F=1.12 MN_MODEL_INIT_SCALE=1.20 MN_MODEL_HIDE_POW2=0.72
  MN_MODEL_T_ROUND=0.015 MN_MODEL_MAP_RATE=0.070 ./estimate.py --target --lat 8.5e-6`: identical).
- The bands: add `--bw 50 | 25 | 8 | 3.6 | 1.0`, `--lat 2e-6`, `--chunk-mb 2048`, `--taper 0.5` to the E line (the `--chunk-mb` row through the
  driver: `--target`'s partial row prices `DEFAULT15C`'s chunk; `./estimate.py --g 576 --D 9.16e10 --chunk-mb 2048 …` gives the base row).

#### 5. Recommendation

**The estimate is row E on the 7.2.4 stack: 290.2 / 303.9 s at 5.276 × 10¹³ and 286.2 / 298.1 s at 5.167 × 10¹³** (modelled) — the design fabric
(100 GB/s per APU, assumed) with every per-node and per-message input measured on aac7 (8.5 µs, T_ROUND 0.015, HIDE 0.72, MAP_RATE 0.070). It is
+6.0 / +4.6 s over the standing row: the measured per-message cost costs +15 s, the measured round cost gives −10 s back. On the target's
installed **ROCm 7.0.3 the same run is 336.3 / 347.4 s** (+46 s: the toolchain's +22 % on the local passes, measured; the model's one-node
over-prediction makes this a ceiling, the AAC7 fit 316.9 / 329.2 the mid value) — the first thing to ask of the target is a 7.2.4 module.
What the bands mean: (b) the latency is now measured, its band (2 → 8.5 µs) is 15 s and the estimate sits at its top; (c) the APU-binning tail is
≤ +10 s and unmeasured; (e) `MN_T_CHUNK_MB=2048` is worth −13 … −15 s at the measured T_ROUND (the user's T11 call, still off); (a) is the
one that decides the run: **the injection bandwidth per APU is the only input that moves the wall by more than ±20 s, and it remains assumed**
— at one NIC per APU of aac7's class the wall is 460 s, and at what the code does today on a Cray node (one PE, one NIC, 3.6 GB/s per APU thread,
measured) it is 1 830 s, at the 8-node fall-off 6 000 s (d: an open risk — the fabric rate per thread fell 3.6 → 2.9 → 1.0 GB/s from 2 to 8 nodes
on aac7, cause open, B16/C16). Still **assumed**: the target's injection bandwidth per APU and NIC count (PLAN §25's two 400 Gb/s), the 576-node
taper and dragonfly group (taper 0.5 would add +22 s), the Lustre aggregate with 576 writers (1.0 GB/s per node from one node's test), `GEN_HIDE`
at depth 1 / 2 (aac6's), and the APU binning.

#### 6. Open items

- RESULTS §94's 278.5 / 293.0 s at 5.167 × 10¹³ is not what main dbde0c7 prints for the pool rule (280.2 / 293.8: the rule gives tree 1 /
  division 0 primes at that size; the forced tree 1 / division 1 row is 278.3 / 292.7). The 1.7 / 0.8 s is inside the model's noise; the row now
  printed by `--target` is the pool rule's.
- The model's 2-node byte law is 2.2 × the code's bytes (C §3); if that factor carried to 576 nodes the fabric's exposed share (72 s at E) would
  shrink, not grow — the E row is conservative on bytes, not on rate.
- The 3.6 → 1.0 GB/s fall-off with the node count (d) has no model; the aac7 4- and 8-node runs that would separate the fabric from the code
  (B16) are the open measurement, with the 4-PE-per-node design (32 GB/s per node) the open design question.

#### RESUME

- 2026-10-04: **done** — the knobs (`--hide-pow2`, `--t-round`, `--local-factor`, `MN_MODEL_LOCAL_F`), the second test size's partial row, this
  report, TARGET.md §1's new paragraph, TARGET_TASKS T1's note, README rows; `./estimate.py --target` unchanged at the standing row.

## TARGET_HW_REVIEW — the target's microbenchmark report against the model (desk review, 2026-10-03, 22:52 EDT)

(former file: TARGET_HW_REVIEW.md)

Source: `~/apucode/TARGET_HARDWARE.txt` (564 lines; one node, `baryon-cn0059`, 4 × MI300A, ROCm 7.0.3,
XNACK off; 157 binaries, 13,160 measurements). Compared with `ecalc/mn_model.py`, `ecalc/mem_model.py`,
internal target notes §6, internal target task list T1, `archive:results/AAC7_survey.md` and the memory file of measured MI300A facts.
No code was read or edited; the only runs were `ecalc/estimate.py` on this workstation (main a24ef3c).
Every number is labelled **measured** (the report, or our aac6 runs), **modelled** (`estimate.py`) or **assumed**.

#### 1. What the report is, and what it is not

The report is a **single-node, in-node** characterization: launch overhead, the memory hierarchy (L2 / Infinity
Cache / HBM), MFMA throughput, xGMI P2P via RCCL, precision conversion, kernel fusion, atomics, per-APU binning,
thermals. Its coverage matrix lists "MPI collectives", "I/O buffering", "PCIe bandwidth", "NUMA effects" and
"Advanced (persistent/UM)" as benchmarked categories, but the report **prints no number for any of them**.

Therefore it says **nothing** about the inputs the 576-node estimate is most sensitive to: the Slingshot injection
bandwidth per APU, the per-message cost of a SHMEM put, the NIC count and NUMA affinity, libfabric / cxi behavior,
GPU-initiated transfers, the chunk-round cost `T_ROUND`, Lustre (per-node rate, aggregate with 576 writers, OSTs,
striping), the device-memory mapping rate, page sizes, or the node's memory edge. Of TARGET_TASKS T1's list, the
report closes **none** of items 1–9 outright; it confirms the silicon-level facts the model already carries from aac6.

#### 2. Assumption by assumption

| # | input | ours (source) | the target report | effect on 5.276 × 10¹³ on 576 (284.2 / 299.3 s standing) |
|---|---|---|---|---|
| 1 | Fabric injection per APU | **100 GB/s assumed** (`mn_model.TARGET`, two 400 Gb/s NICs per APU, PLAN §25) | not measured (no network benchmark) | unchanged. Sensitivity, modelled on the base row (292.3 / 306.5 s without the partial cache): 50 GB/s → 352.3 / 360.1 s (+60 s); 25 GB/s → 472.2 / 467.0 s (+180 s). One 200 Gb/s NIC per APU (aac7's class) on the target would cost +180 s |
| 2 | Per-message cost | **2 µs assumed** (`--lat`) | not measured for the NIC; xGMI P2P latency 3.75 µs (<1 KB) … 6.33 µs (>256 KB), measured, RCCL | if the NIC's put cost resembled xGMI's: `--lat 3.75e-6` → +4.5 s (296.8 / 310.2 s on the base row), `--lat 6.33e-6` → +10.9 s (303.2 / 315.3 s). Applied to the standing figure: ≈ 288.7 / 303.8 and 295.1 / 310.2 s (modelled; the `RNS_DIST_CACHE_PARTIAL` row of `estimate.py --target` does not take `--bw` / `--lat`, see §5) |
| 3 | `T_ROUND` (one chunk round) | **0.030 s fitted on aac6 loopback ± 100 %** (`MN_MODEL_T_ROUND`) | not measured; the report's launch figures bound the launch part of it: async 0.91 µs, steady sync 7–8 µs (measured) — launches are ≪ 1 ms of the 30 ms | unchanged. `MN_MODEL_T_ROUND=0.01` → **271.1 / 288.2 s**; `0.1` → **329.7 / 338.1 s** (modelled, the standing row). Still the largest open input after the fabric; aac7 can measure it (T11's procedure at 8–12 nodes) |
| 4 | Overlap / `GEN_HIDE` | `HIDE_POW2` 0.75, `GEN_HIDE_DEPTH` {1: 0.011, 2: 0.74} measured on aac6 (xGMI under loopback / 1 GbE) | not measured; the report's "P2P" is RCCL inside the node, no overlap test | unchanged; aac7 measures it at Slingshot rates (T1 item 2, `COMM_LAYER_STATS=1 COMM_XGMI_STATS=1`) |
| 5 | Part-file write | **1.0 GB/s per node assumed** to hold with 576 writers (`TARGET_WRITE_BW`; the user's Lustre test 2026-09-29) | nothing on Lustre or I/O | unchanged: 299.3 s at 1.0, 326.4 at 0.6, 278.9 at 2.0 (modelled). Aggregate, striping, waves (T1 item 5b/c, T10) stay target-only; aac7 has no Lustre |
| 6 | xGMI per APU | 3 × 91 GB/s links (RESULTS 11); all-to-all push kernel **909 GB/s per node, 64-bit stores** (measured aac6); `T_XGMI_31` 0.019 s per transform per prime measured; 84 % hidden | **1.6 TB/s aggregate, RCCL ring AllReduce 1 MB** (bus-bandwidth convention, × 1.5 over algorithm bandwidth at 4 ranks → ≈ 1.07 TB/s of data ≈ 267 GB/s per APU); topology symmetric, < 5 % pair variance; `hipMemcpyPeerAsync`-class copies not reported | none: our push reaches ≈ 227 GB/s per APU = 83 % of the report's link class, on identical silicon; the model's xGMI terms are measured kernel times. The symmetric topology confirms no pair-specific placement is needed |
| 7 | HBM / LDS / compute | NTT kernels calibrated on aac6 MI300A (2325 Gbfly/s; `hipHostMalloc` streams 14.0 TB/s per node = 3.5 TB/s per APU) | HBM 3.2 TB/s sustained per APU (STREAM Triad, 60 % of peak), 3.96 TB/s max at 64 MB; IC 1.2 TB/s, 256 MB; L2 800 GB/s; sclk 1,595–1,626 MHz, mclk 1,300 MHz | none to the calibrated terms (same silicon, same ROCm major). The report's per-APU spread (U4) is the one new risk: see row 10 |
| 8 | Node memory | `NODE_GB` 502, budget **480 GB**; edge **524 GB measured on aac6** (Slurm RealMemory 514,000 MiB); node 471.9 GB at the target (modelled) | "501 GB HBM3, shared CPU+GPU, 4 NUMA × ~128 GB, no host DRAM; hipMalloc carves from the same pool" (the unit is loose: 4 × 128 GiB = 549.8 GB; aac7's `free -g` shows 501 GiB = 538 GB, RealMemory 500,000 MiB = 524 GB) | unchanged: 480 GB holds under every reading. The target's edge (T1 item 9) is still unmeasured; aac7 (same RealMemory class as the report's node, probably) can stand in for it |
| 9 | Mapping rate / VMM / pages | `MAP_RATE` 0.065 s/GB measured on aac6 (ROCm 7.2.4) | no VMM, page-size or allocation numbers; ROCm **7.0.3**, XNACK off | unchanged in the model; the ROCm version is the one target fact we can match: aac7's default is 7.0.3 |
| 10 | Per-APU variance | the critical path = the top node's leaf (1.036 × average digits), APUs taken as equal | sclk spread 5 %, HBM 2,780–2,971 GB/s (7 %), FP64 86–92.5 TF across the four APUs (measured, one node) | **assumed +0 … +10 s**: the local terms (≈ 200 s of the 284) × up to 5 % if every level's barrier waits on a slow-bin APU among 2,304; not in the model. Worth a `rocm-smi --showclocks` line in every log |
| 11 | Thermal / power | no throttling in 60 s continuous NTT (measured aac6) | 416 W of 2,200 W, 28–33 °C, 62–65 °C headroom (measured; bursts) | none |
| 12 | Launch overhead | async launches throughout | 0.91 µs async; first launch 354 ms cold, sync 8–22 ms cold then 7–8 µs | none (init absorbs the cold start) |

##### Re-run of `estimate.py` with the report's values (modelled, 5.276 × 10¹³ on 576, `MN_OUT_DKM_HI=1`)

| run | no write | with the write @1.0 GB/s | note |
|---|---|---|---|
| standing (`./estimate.py --target`, the `RNS_DIST_CACHE_PARTIAL` row) | **284.2 s** | **299.3 s** | base row without the partial cache: 292.3 / 306.5 s |
| `--lat 3.75e-6` (the report's small-message xGMI latency, as a proxy) | 296.8 (base row) ⇒ ≈ 288.7 | 310.2 ⇒ ≈ 303.0 | +4.5 / +3.7 s |
| `--lat 6.33e-6` (its large-message latency) | 303.2 ⇒ ≈ 295.1 | 315.3 ⇒ ≈ 308.1 | +10.9 / +8.8 s |
| `--bw 50` | 352.3 ⇒ ≈ 344 | 360.1 ⇒ ≈ 353 | the fabric's exposed share at 100 GB/s is ≈ 60 s |
| `--bw 25` (one 200 Gb/s NIC per APU) | 472.2 ⇒ ≈ 464 | 467.0 ⇒ ≈ 460 | the aac7 class of NIC on the target |
| `MN_MODEL_T_ROUND=0.01` | 271.1 | 288.2 | the standing row takes the env |
| `MN_MODEL_T_ROUND=0.1` | 329.7 | 338.1 | |
| `--write-bw 0.6` / `2.0` | 284.2 | 326.4 / 278.9 | |

No input in the report moves the standing estimate; the report's only numeric effect is a latency proxy worth
+4 … +11 s if the NIC put behaved like xGMI P2P, and an unmodelled APU-binning tail of +0 … +10 s (assumed).

#### 3. What in the report touches code design

1. **NIC count / affinity: nothing.** The report has no NIC. The design facts stay PLAN §25's (two 400 Gb/s per APU);
   aac7 has one 200 Gb/s Cassini per APU socket (`cxi0..3`, PCI domains 0000–0003), so on aac7 the thread-to-NIC
   affinity (APU i's host thread and staging buffers on NUMA node i, the endpoint on `cxi<i>`) can be checked and the
   model's `--bw` set to 25. T8 (two endpoints per APU) cannot be measured as designed on aac7 (one NIC per APU); a
   two-contexts-on-one-NIC run only says whether one context saturates 200 Gb/s.
2. **SHMEM / libfabric: nothing.** Cray OpenSHMEMX over cxi is unmeasured anywhere; aac7 is the first chance.
3. **GPU-initiated vs host-initiated: indirectly host.** The report's `atomicCAS_i32` at **34 µs** (measured, A-table)
   argues against any device-side polling or signaling loop; our transport is host-initiated SHMEM (T6's rocSHMEM
   put-with-signal stays conditional). Keep the host path; do not add device-side waits.
4. **xGMI: confirmed, no change.** Symmetric topology, 1.6 TB/s RCCL bus bandwidth ≈ our push kernel's class. A cheap
   A/B on aac7 (`rccl-tests/7.0.3` `all_to_all_perf` against `t_comm`'s push) says whether RCCL beats 909 GB/s; only if
   it does by > 1.2× is a transport change worth a switch.
5. **Infinity Cache 256 MB, 16–64 MB sweet spot (1.3× over HBM, measured).** The NTT row kernels stream 2²⁹-point
   planes; the transposes (16 % of the multiply, measured aac6) could be tiled to 16–64 MB working sets. Upper bound,
   modelled: 16 % × (1 − 1/1.3) ≈ 3.7 % of the local passes ≈ −5 … −7 s at the target; a single-node code test (aac6 or
   aac7), behind a switch.
6. **HBM / page sizes / VMM / hipMalloc: nothing measured.** `MAP_RATE` and the 460 GiB `hipHostMalloc` trick remain
   aac6 facts; the only actionable match is **ROCm 7.0.3** — build on aac7 with the system 7.0.3 (the target's version),
   not the 7.2.4 module the survey proposed for aac6 parity, and record init's `pools … s` line at 7.0.3 as the target's
   mapping rate.
7. **Lustre: nothing**, and aac7 has none. Items 5a–d and T10 stay target-only; aac7 can only exercise the writer's
   mechanics (`ECALC_OUT_MODE`, `MN_OUT_THREADS`, waves) over NFS, which says nothing about Lustre.
8. **Memory limits: consistent with 480 GB.** 501 GB/GiB shared pool, no host DRAM, `/tmp` is tmpfs on aac7 (counts
   against the pool: TARGET §6 item 10 holds). The edge is unmeasured on the target; aac7's edge is the proxy.
9. **APU binning (5–7 %)**: log `rocm-smi --showclocks` and the per-APU `dist_mn` times on every run; if the slow APU
   is systematic, the leaf layout's 1.036× top-node factor is the place a per-APU weight would go (design only; no
   change proposed now).

#### 4. The revised aac7 plan

| phase | keep / change / drop / new | why | time (EDT, elapsed) |
|---|---|---|---|
| A — port to Cray OpenSHMEMX | **keep, change the toolchain**: ROCm **7.0.3** (the target's, the report's; aac7's default), `module load cray-dsmml cray-openshmemx`, `-lsma`, `srun`; a 7.2.4 build only as a one-off A/B | the report fixes the target's ROCm; nothing else in A changes | 1 session (2–4 h) + bundle copy |
| B — bring-up 2 → 12 nodes | **keep**; add `-c 192 --exclusive`, and `rocm-smi --showclocks` + NIC affinity (`cxi<i>` per APU thread) in every log | nothing in the report pre-empts it; the clocks line prices the binning tail (§2 row 10) | 1 session (modelled walls at 25 GB/s: 10¹⁰ per node 27.5–34.1 s at 2–12 nodes; 9.16 × 10¹⁰ per node 190.9 s at 2, 323.3 s at 12) |
| C — measure the model's inputs on Slingshot | **keep, narrowed**: injection GB/s per APU (expect ≈ 25 on aac7), per-message cost, overlap at depth 1 / 2, `T_ROUND` (T11's procedure), the mapping rate at 7.0.3, the memory edge; **drop from C** nothing the report measured — it measured none of these | the report closes no T1 item; aac7 is the first fabric that can | 1–2 sessions |
| D — A/B at 8–12 nodes | **keep** `MN_GROUPS`, `MN_T_CHUNK_MB` 1024 / 2048, overlap stats, the cache, EW / PC; **change** "two SHMEM endpoints per APU" to "two contexts on one NIC" (aac7 has one NIC per APU); **new** rccl-tests all-to-all against the push kernel | the endpoint test as designed needs two NICs per APU | 1–2 sessions |
| E — scaled headline at 12 nodes at the target per-node share | **keep**: 1.099 × 10¹² digits, modelled 323.3 s without / 335.8 s with the write at 25 GB/s and 0.6 GB/s (216.8 / 253.2 s at 100 GB/s); node 445 GB (modelled) + the verification chain | unchanged by the report | 1 session |
| F — re-estimate | **keep, extend**: feed `--bw`, `--lat`, `MN_MODEL_T_ROUND`, `--hide-pow2`, `--gen-hide2` from C; add the binning tail as an explicit assumed term; note the `RNS_DIST_CACHE_PARTIAL` row's missing `--bw` / `--lat` (§5) | the report adds the latency proxy (+4 … +11 s) and the binning tail (+0 … +10 s) as bands, not inputs | 0.5 session |
| G — the target package | **keep, paused** | unchanged | — |
| **new N1** — IC-tiled transposes | single-node code test behind a switch (aac6 or aac7) | the report's 1.3× IC figure; −5 … −7 s upper bound, modelled | 1 session |
| **new N2** — RCCL vs push kernel | `rccl-tests` on aac7, 30 min | the report's 1.6 TB/s is RCCL; only a > 1.2× gap would justify a transport switch | 0.5 h |
| **new N3** — clocks and NIC affinity in the logs | a log line per run, no algorithm change | the 5–7 % APU spread is the one unmodelled term the report introduces | in B |

Phases the report makes unnecessary: **none** (it measured nothing off the node). Phases it changes: A (ROCm 7.0.3),
D (the endpoint test), F (two new bands). New: N1–N3.

#### 5. Open items and model notes

- `estimate.py --target`'s standing row (the `RNS_DIST_CACHE_PARTIAL` line, 284.2 / 299.3 s) is computed by
  `mn_model.cache_partial` on `mn_model.TARGET` and **ignores `--bw` and `--lat`** (it does take `MN_MODEL_T_ROUND`
  and `--write-bw`); the sensitivities above are therefore taken on the base row and transferred as deltas. Worth a
  one-line fix in F so the row follows the CLI fabric.
- `estimate.py`'s "per NIC" column divides by 8 NICs; on aac7 (4 NICs) read it × 2.
- The report's "501 GB" node memory is unit-ambiguous (GB vs GiB); nothing in the budget depends on which.
- Items still only the target can give: the aggregate Lustre rate with 576 writers, striping and waves (T10), the
  576-node global-link taper, two NICs per APU (T8), the real per-message cost on Slingshot-2 (aac7 is Slingshot-11).
