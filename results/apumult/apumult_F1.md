# F1 — apumult §0, Category A (SSD spill/stream), Category D (trim/free/tight-size), §7 (ranked ports), §8 (architecture) against ntt/

Source read: `/home/machinus/apucode/apumult.md` (all 130 lines). Our side: `ecalc/{binsplit.c, newton_db.c, dbig.c, mem.c, rns_dist.c, mn.c, todec.c, ecalc.c, mem_model.py}`, `RESULTS.md` §71, §77–§82, `results/{M13,P13b,G13d,WP7,A-ckpt,N13}.md`.
Labels: **measured** (aac6 runs, file named), **modelled** (`mem_model.py`, which equals the C layout request to the byte and the measured device totals to ±0.05 % — M13), **assumed** (mine). No code was changed.

Context from the integrator (arrived mid-task): apumult's numbers were taken on the target's own MI300A nodes with a node-local `/ssd0`, so its 235e9-per-node figure is target-node evidence; the goal is adoption, so every item below ends in "how to port it here and what it gives".

---

## Decision table — digits against time, per option (the user's criterion: %Δtime per %Δdigits; rejected before: ≈ 1.6 %/1 %; the compute floor ≈ 1.1 %/1 %)

Method (all **modelled** unless marked): ceiling = the largest size whose modelled node peak fits the measured 524 GB one-node edge (P13b) or the 480 GB target budget; wall at the ceiling = in-core compute by d log² d from the nearest measured point + the spill I/O that is **not** hidden under compute; elasticity = (wall/today's wall − 1)/(digits/today's digits − 1). The writes hide under compute (the background writer exists: `bs_ckpt_bg_*`, N13 measured it hidden at ≳ 1.1 GB/s at 4e10); the **reads are exposed** (P before S = P + Q, Q before the low product) because at the ceiling there is no free space to prefetch into. The hidden writes need ≈ 0.6 GB/s (P under the reciprocal at 1.7e11), ≈ 1.3 GB/s (Q under the second product + high product), ≈ 0.8 GB/s at 576 (the P share): satisfied at every rate below.

**One node, cap 2³¹ (default).** Today: 1.306e11 (measured 1.30), wall 432 s (d log² d from the measured 413 s at 1.252e11, G13d).

| option | ceiling | Δdigits | compute at ceiling | wall / elasticity at 2 GB/s | 5 GB/s | 10 GB/s | exposed read | passes the 1.6 criterion? |
|---|---|---|---|---|---|---|---|---|
| V2: P spill under the reciprocal + tail policy | **1.678e11** | +28.5 % | 566 s (el 1.09) | 604 s, **1.39** | 581 s, **1.21** | 574 s, **1.15** | 75 GB (P) | yes at every rate |
| V3: V2 + Q spill (2nd product → high product), r2 late, division reorder | **1.697e11** | +29.9 % | 573 s (el 1.09) | 649 s, 1.67 | 603 s, **1.32** | 588 s, **1.21** | 151 GB (P, Q) | at ≥ 5 GB/s; marginal at 2 |
| D: trim the 10 % bound margin on Q, S (no I/O) | 1.332e11 | +2.0 % | 442 s | 442 s, 1.08 | same | same | 0 | yes (small) |

**One node, cap 2³⁰ (the largest-size cap).** Today: 1.443e11 (measured 1.44), wall 1120 s (measured 1086–1139 s at 1.42–1.44e11, G13d).

| option | ceiling | Δdigits | compute | 2 GB/s | 5 GB/s | 10 GB/s | exposed read | passes? |
|---|---|---|---|---|---|---|---|---|
| V2 | **1.794e11** | +24.3 % | 1416 s | 1456 s, **1.23** | 1432 s, **1.15** | 1424 s, **1.12** | 80 GB | yes |
| V3 | **1.882e11** | +30.4 % | 1491 s | 1575 s, **1.33** | 1525 s, **1.19** | 1508 s, **1.14** | 167 GB | yes |
| D trim | 1.472e11 | +2.0 % | 1144 s | 1144 s, 1.08 | same | same | 0 | yes |

**576 nodes** (the design's switches; the tree's top level binds at 294 GB per node once the dm need drops — section 6). Today's 480 GB ceiling 8.105e10 per node = 4.67e13, wall 259 s by d log² d from the modelled 234 s at 7.38e10 (`estimate.py --target`). **The grid steps are excluded from the compute column**: 4.90e13 lies past the modelled steps at 4.30e13 (+5 %) and 4.40e13 (+12.6 %) (RESULTS §81), which by themselves are elasticity ≈ 9 — the same objection the user raised before applies to any ceiling past 4.29e13, spill or not.

| option | per node | total | Δdigits | compute | 2 GB/s | 5 GB/s | 10 GB/s | exposed read per node |
|---|---|---|---|---|---|---|---|---|
| V2 (P share) | 8.51e10 | **4.90e13** | +5.0 % | 273 s | 292 s, 2.54 | 280 s, **1.67** | 277 s, **1.37** | 38 GB |
| V3 (P, Q shares) | 8.51e10 | 4.90e13 | +5.0 % (the tree caps it) | 273 s | 311 s, 4.00 | 288 s, 2.25 | 280 s, 1.67 | 76 GB |
| V3 + tree-level spill (**assumed** cold set ≈ 60 GB per top level) | ≈ 9.6e10 | ≈ **5.5e13** | +18.4 % | 311 s | 413 s, 3.24 | 352 s, 1.95 | 331 s, **1.52** | ≈ 205 GB |
| for comparison, built and off: `MN_T_CHUNK_MB=1024` | 9.24e10 | 5.32e13 (M13, 502 GB) | +14 % | — | +0.24 min modelled (§79: ≈ +6 %), i.e. el ≈ 0.5 + compute 1.1 | | | 0 |

Reading: on one node the P spill with the tail policy (V2) is the only option that is both large and cheap — +28 % digits at 1.15–1.39 %/1 %, inside the user's criterion at any NVMe rate ≥ 2 GB/s; V3 adds 1–5 % more digits for a second exposed read and is worth it only at ≥ 5 GB/s (2³¹) or at the 2³⁰ cap. At 576 the spills buy 5 % (the tree's top level is the next wall) at 1.4–2.5 %/1 %, worse than the built chunking switch; at the chosen 4.25e13 target they buy margin (452 → 437 GB per node), not digits. Nothing reaches apumult's 2.35e11 per node: the V3 floor is ≈ 2.3 bytes/digit (planes + host + 4.2 B), and a run at 2e11+ per node takes 15–30 min.

---

## 0. The one fact everything hangs on: what sets our one-node peak

Our device memory is mapped **at init** and does not move: planes (103.7 GB at cap 2³¹, 60.7 at 2³⁰; measured P13b) + **one arena per APU** = `max(bs regions, dm need)` (`binsplit.c binsplit_pregrow` → `arena_get`; `dm_layout` lines 318–339) + 0.6 GB tables, beside a 12–17 GB host HWM. Above ≈ 5e10 digits the arena is the **dm need**, which is the larger of two formulas per device (in units of B = 8·n_Q bytes = the bytes of Q ≈ 0.444 bytes per digit; n_Q = Q's limbs):

| term in `dm_layout` | what it is | size |
|---|---|---|
| **v2** (the reciprocal's peak) | Q (+10 % bound margin) + S=P+Q (same) + r + r2 (k+4 limbs each, k ≈ n_Q) + t1 (max(n_Q+k, 2k)+8 ≈ 2 n_Q: the reserved "hole"/tail) + one grid piece (2^pool_log limbs: 17.2 GB/node at 2³¹, 8.6 at 2³⁰) + slack min(1/8, 1 GiB) | ≈ 6.2 B + piece + slack |
| **v3** (the bs top level beside the tail) | the last bs level's inputs (2 P, 2 Q of n_Q/2, +10 %) + outputs (P, Q of n_Q, +10 %) + 1/8 fit **+ the hole (t1's 2 B, reserved so the outputs do not fragment it)** | ≈ 4.95 B + 2 B ≈ 6.95 B |

At 1e11 (modelled, = measured 431/414 GB within 0.1 %): v2 = 298 GB, **v3 = 310 GB binds**; arena 310, planes 104, host 14 → 428 GB. The measured edge is device + host HWM ≲ 524 GB (P13b), so the ceiling is (524 − 15 − planes)/(≈ 3.1 bytes/digit): **1.30e11 at 2³¹, 1.44e11 at 2³⁰ (measured, P13b)**; the model at 524 GB gives 1.306 / 1.443e11 (this run of `mem_model.py`).

Consequences for every spill item:
1. A buffer that is cold during the reciprocal frees bytes **only if the arena formula is shrunk by it** (the block returns to the pool's extent list; the OS never sees anything; there is no madvise on hipMalloc'd HBM). The port is therefore always: spill + `db_free` + a smaller `dm_layout` + restore, not a spill alone.
2. **Spilling P alone gives zero**, because v3 (the bs top level + reserved hole) binds, not v2. The hole policy has to change with it (below, "V2").
3. Beyond the dm phase the tree's top level binds at 576 nodes (tree need 294 GB per node at 7.38e10 vs dm need 309: modelled below), so the same spills cap out at +5 % there unless the tree level is spilled too.

The variants I modelled (each includes the previous; all are changes to `dm_layout`'s formula plus the spill code):

| variant | what is spilled / changed | v2 (recip/div peak) | v3 (bs top) |
|---|---|---|---|
| V0 | today | 6.2 B + piece | 4.95 B + hole |
| V1 | P spilled under the reciprocal, restored before S = P + Q | 5.1 B + piece | unchanged |
| V2 | V1 + the tail policy: t1's contiguous block = the parity half that held the top level's dead inputs, so no hole is reserved beside the top level | 5.1 B + piece | 4.95 B |
| V3 | V2 + r2 reserved only after the first product; Q spilled from the last iteration's second product through the division's high product, restored for the low product; the window Aw formed and S freed before the low product | max(Q+r+t1+piece, r+d+t1, S+µ+t, Q+X+xq+Aw) ≈ 4.2 B + piece | 4.95 B |
| V4 | V3 + the bs top level frees each input as consumed | same | ≈ 4.33 B |
| V5 | V4 + the fresh Q and P₂ spilled during T = P₁Q₂ (apumult M56) | same | ≈ 2.5 B |

**One-node ceilings (modelled at the measured 524 GB edge; 502 GB in brackets), three primes, host fitted:**

| cap | V0 | V1 (P only) | V2 (P + tail policy) | V3 (+Q, r2, div reorder) | V4/V5 (bs top) |
|---|---|---|---|---|---|
| 2³¹ (default) | **1.306e11** (1.236; measured 1.30 / OOM-free 1.252, fragmentation fail at 1.245) | 1.306e11 (no change) | **1.678e11** (1.583) | **1.697e11** (1.683) | 1.697e11 (no change: v2 binds) |
| 2³⁰ | **1.443e11** (1.373; measured 1.44, 1.46 OOM) | 1.443e11 | **1.794e11** (1.739) | **1.882e11** (1.794) | 1.882e11 |

Per-size node totals (modelled, 2³¹; the arena in brackets): 1e11: V0 428 (310) → V2 373 (256) → V3 361 (243); 1.4e11: 553 (434) → 466 (347) → 447 (328); 2e11: 741 (621) → 598 (478) → 585 (465); 2.35e11: 851 (729) → 685 (563) → 676 (554). At 2³⁰ subtract 43 GB. So 2e11 and 2.35e11 on one node need ≈ 585 / 676 GB even with V3: **not reachable by spilling P/Q/µ** on our layout; the floor with V3 is the reciprocal's own r + r2 + t1 (4 B) and the low product's Q + X + xq + Aw (4.2 B). apumult's 2.02 bytes/digit (475/235e9) is below our V3 floor of ≈ 2.3 bytes/digit at 2³⁰ (planes 61 + host 15 + 4.4 B·0.444 d); reaching it needs the two further changes listed under "V6" (band-sized product outputs, the window in chunks) that apumult's `mul_dispatch_hi/lo/band` embodies.

GB freed from the node peak by V3, at each size (modelled): 1e11 −67 GB, 1.4e11 −106, 2e11 −156, 2.35e11 −175 (2³¹); 2³⁰ within 1 GB of the same.

---

## 1. Category A — SSD spill/stream, item by item

### A1. `DM_Q_SPILL` (M29e): Q spilled after bs, restored for the low product (~98 GB at d235)
*What it does.* Q (= N! in the run's base, n_Q limbs, 0.444 bytes/digit) is written to the SSD once bs is over and its host buffer released; it is read back only for the low product X·Q of the division. In apumult's host flow the reciprocal is fed from a "prerecip" (their divisor-cache µ) so Q is not needed in memory meanwhile; the restore is a full sequential read.
*Our analogue.* `bs_Qd` (the top-level dbig, `ecalc.c` line ~496; the mdb share `Qm.sh` at size > 1). In our reciprocal (`newton_db.c recip_db2`, lines 62–117) Q is an operand of the **first product of every iteration** (Q_t = the top `take = 2j+2` limbs, `db_view(Qd, nq − take, take)`; at the last iteration take = n_Q: all of Q), so Q cannot leave during the reciprocal; it is dead from the last iteration's second product (r·d → t1) through the final add and the division's high product A_h·µ (`newton_db_divmod_shifted` line 243), and needed again at the low product (line 251) and the window corrections (lines 257–268: `db_cmp(&Rd, Qd)`, `db_sub(&Rd, &Rd, Qd)`).
*Port.* Spill Q right after the last iteration's first product (`recip_db2` after line 82, condition `take == nq`), `db_free` it; restore into a fresh block before `rns_mul_low_db` (line 251). Transfer: `ckpt_dbig_io` (`binsplit.c:596`) already moves any dbig range device ↔ file through the 1 GiB pinned chunk per APU (`CKPT_CHUNK`, `rns_hstage`); wrap it as `db_spill/db_restore`. Size > 1: the same on `Q->sh` in `newton_mn_divmod` (`newton_db.c` ~706–746; the existing `newton_mn_pq_hook(1, Q)` at line 746 is where the top-set writer already takes Q's share).
*GB and time.* 1.1 B: 44 GB at 1e11, 62 at 1.4e11, 75 at 1.7e11 (per node). It lowers v2 only together with V2's tail policy and the r2 change (part of V3): V2 → V3 is +1 % at 2³¹, +5 % at 2³⁰ (1.79 → 1.88e11). Traffic 2 × Q per run: at 1.7e11 150 GB → 150 / 75 / 50 s at 1 / 2 / 3 GB/s; the write can hide under the second product + high product (needs ≳ 2 GB/s), the read is exposed before the low product (no free space to prefetch into at the ceiling).
*Verdict:* worth testing as part of V3, not alone.

### A2. `DM_P_SPILL` (M32): P spilled around the reciprocal (~98 GB)
*What it does.* P is dead from the end of bs until S = P + Q (their 10dP); it goes to disk under the reciprocal and comes back for the add.
*Our analogue.* Exactly the same dead interval: `bs_Pd` from the residues (`ecalc.c` ~491) to `db_add(&bs_Pd, &bs_Pd, &bs_Qd)` (line 513). **The write half already exists**: `ECALC_CKPT_TOP` runs `bs_ckpt_bg_start` (`binsplit.c:815`) writing P under the reciprocal and Q under the division, with `bs_ckpt_bg_release(topbg, 0)` before P is overwritten (`ecalc.c:512`); at size > 1 `mn_pq_hook` (`mn.c:351`) does the same for the shares. What is missing: `db_free(&bs_Pd)` once the writer's P part is done (`bs_ckpt_bg_done(b, 0)`), the read-back into a new block before line 513, and the smaller arena (v2 − 1.1 B).
*GB and time.* P = 1.1 B (with the bound's margin): 49 GB at 1e11, 68 at 1.4e11. **Alone it changes the ceiling by 0** (v3 binds; table above). With the tail policy (V2) it is the single biggest step: **1.31 → 1.68e11 (2³¹), 1.44 → 1.79e11 (2³⁰)**. Traffic 2 × P: 89 GB at 1e11, 149 GB at 1.7e11 → read exposed 75 / 37 / 25 s at 1 / 2 / 3 GB/s on a run of ≈ 580–840 s at 1.7e11 (compute extrapolated from 413 s at 1.25e11: d log² d gives 575 s, the measured local exponent 2.3 from 7.64e10 → 1.25e11 gives 838 s) = +3–13 %; the write hides under the reciprocal if the SSD sustains ≈ 0.6 GB/s (75 GB over the ≈ 120 s reciprocal; the aac6 NVMe wrote 0.8–1.5 GB/s with fsync, WP7/A-ckpt measured; 0.31 GB/s on its slow path, §77).
*Verdict:* **worth testing first**, as V2 (P spill + tail policy); it is the one that moves the ceiling.

### A3. `DM_MU_SPILL` (M33a): µ spilled around 10dP (~98 GB)
*What it does.* In apumult µ is computed before A = 10^d (P + Q) is formed (their 10dP is a real host phase with a large A), so µ is cold during it.
*Our analogue.* None: the decimal device flow has no 10dP (A = S·B^dl is implicit, RESULTS §70 I3; `10dP 0.0 s`); µ is produced by the reciprocal (it takes r's block, `recip_db2` line 108) and consumed by the high product right after S = P + Q, an add of 1–2 s. Its only cold interval is that add, which is not a peak.
*Verdict:* not applicable.

### A4. `DM_MU_STREAM` (M49p): the high product preads µ in chunks, no full restore (~33 GB)
*What it does.* µ never comes back whole; their `mul_dispatch_hi` reads µ's chunk for each band from disk.
*Our analogue.* The high product is a piece grid over fixed planes (`rns_mul_high_db` → `rns_dist.c` grid, ka × kb pieces of 2^pool_log limbs; at 1.44e11 the division's grids reach 18 × 22, G13d). A streamed operand piece is read **once per row it is combined with** unless the other operand's transforms are cached: kb pieces × ka rows = ka × |µ| bytes of reads per product (10–20× µ at the top sizes: 0.5–1.5 TB per product at 1–3 GB/s = hours). The transform cache (`RNS_DIST_CACHE`, 17 GB per slot per APU, off at size 1) would have to hold a whole row of transforms to make streaming read each piece once, which costs the memory the stream saves. Not the same product structure as apumult's dispatch.
*GB.* Would remove µ (1 B) from the high product's set (S + µ + t = 4.1 B) — a phase that does not bind (the reciprocal's 4 B + piece and the low product's 4.2 B do).
*Verdict:* not applicable to the grid; superseded by restoring µ whole (it is never spilled here).

### A5. `DM_Q_STREAM` (M52k): the low product streams Q from the spill (~20 GB)
*Same structure as A4*: the low product `rns_mul_low_db(&xq, Xp, Qd, w)` is a grid with the pieces above w skipped; streaming Q's pieces reads them ka times. The window corrections need Q whole (`db_cmp`, `db_sub` over quarters) — those could be chunked (the add kernel is already chunked, `dbig.c addsub_core2`, CH = 4096 limbs with a host carry scan), which is the part of this item that would port: a `db_sub_streamed(R, R, file)`.
*GB.* Q (1.1 B) out of the low-product set Q + X + xq + Aw = 4.2 B → 3.1 B, but then the reciprocal's r + r2 + t1 + piece ≈ 4.4 B binds: the ceiling moves ≈ 0.
*Verdict:* not applicable now; only relevant after a band-sized t1 (V6).

### A6. `DM_RECIP_RSPILL` (M51) + M51-fix2: the final iteration spills r after r², the shift-left streams from the spill (~30 GB)
*What it does.* In the r' = 2r − Q r² form, r is dead between forming r² and the final 2r − …; they spill it and stream it into the final shifted subtraction.
*Our analogue.* Our form (k-anchored, `recip_db2`): t1 = Q_t·r; d = |B^2j − u| → r2; t1 = r·d; r' = (r << j) ± corr with corr a view of t1. r is an operand of both products and of the final add: **no dead r at the peak**. The doc's "T2 stream-shl" would apply to `db_sub_shifted(&r2, &r, j, &corr)` (line 95), whose kernel is chunked and could read r from a bounce buffer, but r is live for r·d one step earlier anyway.
*What does apply from the same iteration:* **r2 is reserved for the whole reciprocal** (`db_reserve(&r2, k + 4)` at line 72) though it is only written after the first product; reserving it after `rns_mul_dist_db(&t1, &qt, &r)` makes the first product's live set Q + r + t1 instead of Q + r + r2 + t1 (−1 B = −44 GB at 1e11). That is the item's real counterpart here, included in V3. Risk: the block must be found again each iteration (the pool's best-fit with the reserved tail, `dbig.c ext_take`); G13d's 1.245e11 failure (a 27.67 GB request refused with 41 GB free in two extents) shows the edge is fragile.
*Doc's numbers.* "block-pool 121.5 GB @ 4e10" is not in our results: the measured 4e10 arena is 132.3 GB (M13, RESULTS §77/§78), the Phase 10 pool was 141.0 (`mem_model.py MEASURED`). "+2–3e10": not transferable — the r-spill is 0 here; the r2 change is worth ≈ +1 % at 2³¹, +5 % at 2³⁰ on top of V2.
*Verdict:* the r-spill itself: not applicable (different Newton form); the r2 late reservation: worth testing inside V3.

### A7. M56 (designed): the bs top level spills the fresh Q and P₂ between MUL1 and MUL2 (~19 GB at d240)
*What it does.* Top level: Q = Q₁Q₂ (MUL1), T = P₁Q₂ (MUL2), P = T + P₂. After MUL1 the new Q and P₂ are cold until after MUL2.
*Our analogue.* The size-1 top level runs on device numbers in the region pools of the two parities (`binsplit.c` level loop; inputs in one parity's pool, outputs in the other; `node_p/node_q` views, `pool_view`). The pools are bump allocators donated whole (`donate_pools`), so nothing is freed per node: the level's live set is inputs + outputs = 4.4 B (+1/8) — the v3 term. Per-node freeing (V4) and this spill (V5) cut v3 to ≈ 4.33 B / 2.5 B, but **at size 1 v2 binds once V2/V3 are in** (identical ceilings in the table), so they buy < 1 %.
*At 576 nodes* the counterpart is the tree level (`mn.c tree_level_k`, line 462: Horner (P, Q) ← (P_i Q + P, Q_i Q) over the group): the live set per node is the child pair (74 GB at 7.38e10) + the running pair (49) + the new pair (74) + the top product's scratch (80) = 277 → 294 GB with the 1/16 (modelled = the C formula `tree_need_dev`). During P_i × Q_run the child's Q_i and the running P are cold (≈ 60 GB): spilling them is what would move the 576-node ceiling after V2 (section 6).
*Verdict:* size 1: not worth it after V2/V3; 576: worth modelling in `tree_need_dev` and testing after V2.

**Category A total.** The doc's "~200 GB freed at d235" is apumult's host-resident sum; on our layout the reachable sum is V3's −67 GB at 1e11 … −175 GB at 2.35e11 (modelled), and the binding number is the ceiling: **1.30 → 1.70e11 (2³¹), 1.44 → 1.88e11 (2³⁰)**, +30 %, not 3.4×.

---

## 2. Category D — trim / free / tight-size

| item | apumult | ours | GB | verdict |
|---|---|---|---|---|
| `DC_TRIM*` (M31/M48d): free the divisor cache g_dc[h ≤ d/N] before the reciprocal, rebuild later | binary dc | the decimal path formats 10¹⁸-limbs directly (`mn_out.c`); `todec.c`'s `struct divisor` (T = 10^h, µ) exists only on the binary test path (`LIMB_BASE=2`, RESULTS §67 kept it as a check, not the code) | 0 on the default path | not applicable |
| `DM_TRIM`: dm buffer trim | trims host buffers | `recip_db2` reserves r, r2 at k+4 and t1 at tcap exactly ("no other temporaries"); the margins left in `dm_layout` are the +10 % on Q and S (n_Q/10), the +1/8 level fit and the min(1/8, 1 GiB) slack. Trimming the 10 % to the true bound (Q is exactly n_Q limbs by construction; the +10 % is the bound's margin for P > Q) would free ≈ 0.2 B = 9 GB at 1e11 | ≈ 2 % of the ceiling | already mostly equivalent; a 2 % item, after V2 |
| `DEC_AXPOOL_FREE_N` | binary dc pool | no dc pool | 0 | not applicable |
| `DEVDAB_MID_FREE` (M15b): free the device planes between phases | host-resident: planes only needed per multiply | our planes are pools for the whole run (`rns_init` → `rns_shutdown`); every peak phase (reciprocal, division, bs top) multiplies, so the planes are needed exactly when the peak is; the output stage needs none but is past the peak | 0 | already equivalent (`rns_dist_cache_release` at the last grid product; `db_release_pools` after the output stage) |
| `MDEV_RELEASE_DC` | release mdev buffers at dc | region pools donated to the block pool (`donate_pools`), pool released after output | 0 | already equivalent |
| `BATCH_DEVDAB_TIGHT` (M42h): planes sized to a G-multiple, not pow2 | fine-grained plane size | `ECALC_PLANE_CAP` = {2³⁰, 3·2²⁹, 2³¹, 3·2³⁰, fit} (P13b): four steps, each 17–52 GB for 6–9 s at 4e10 measured; at the ceiling the top products exceed the cap and are gridded, so the plane is fully used — sizing below the cap only adds pieces (the 2³⁰ row: −43 GB for 1.5–2.7× the wall at 1.42e11, P13b/G13d measured) | 0 beyond the cap switch | superseded by `ECALC_PLANE_CAP` |
| `DC_NXT_TIGHT`, `P10I_STORE_CAP/EARLY_CLEAR` | pow10 intermediates | no pow10 (A = S·B^dl implicit) | 0 | not applicable |
| `DEC_BFS_TOP_N`, `DEC_LEAF_THRESH` (M55) | binary dc tuning | `todec.c dec_leaf_u_max = 256` is a constant on the binary path (no env) | 0 | not applicable (decimal) |

---

## 3. §7 — the ranked port list, checked

| # | doc's item and claim | verdict here | what it gives (modelled) |
|---|---|---|---|
| 1 | SSD spill P/Q/µ between phases: "7e10 → 15–20e10", "dm overflows the block pool @ 7e10 (§71)", "frees ~200 GB" | The premise is stale (§71 is Phase 8, 2026-09-18; since M11's tail arena the in-phase hipMalloc is 0 at 4e10/8e10/1e11, M13 measured; the ceiling is 1.30/1.44e11 measured). The port is V2 (+V3): the P spill needs the tail policy change to count; µ has no cold interval. | **1.30 → 1.68e11 (2³¹) / 1.44 → 1.79e11 (2³⁰) for V2; 1.70 / 1.88e11 for V3.** Effort: the transfer exists (`ckpt_dbig_io`, `bs_ckpt_bg_*`); the work is `dm_layout` + the tail policy (`arena_get`, `db_pool_set_tail`) + `mem_model.py`, and the fragmentation risk at the edge. |
| 2 | Recip r-spill + stream-shl: "+2–3e10"; "block-pool 121.5 GB @ 4e10 scales" | Not applicable to our Newton form (A6); the counterpart is r2's late reservation (inside V3). The 121.5 GB figure is not ours (132.3 measured). | ≈ +1–5 % on top of V2 |
| 3 | bs-mdev madvise/spill: "+3–5e10", "their bs top levels overflow @ 8e10" | "Overflow @ 8e10" is wrong: 8e10 runs at 369.1 GB with hipMalloc 0 (M11/M13), and G13d ran every size to 1.163e11 clean on the defaults (the region pools' in-phase growth aborts since Phase 12 R unless `RNS_POOL_GROW=1`, and no run needed it). Size 1: v3 stops binding after V2, so V4/V5 give < 1 %. 576: the tree level's cold shares are the second lever. | size 1 ≈ 0; 576 ≈ +1.5e10/node after V2 (section 6, assumed cold set) |
| 4 | pollv3 phase-aware RSS watchdog | Not in my categories; note the facts: our edge runs died by the kernel OOM killer in bs (P13b jobs 21064, 21069) and by a refused hipMalloc (G13d 1.245e11); `ECALC_INIT_ONLY=1` and `BS_LAYOUT_ONLY` predict the init peak without a run (P13b) and `mem_report` prints per phase. A watchdog is cheap and independent. | safety |
| 5 | `DC_TRIM_D8_PRE` + rebuild: "+2–3e10 (bin only)" | not applicable (decimal; no divisor cache) | 0 |
| 6 | madvise(DONTNEED) between dbig ops: "+1–2e10" | Not applicable to hipMalloc'd HBM: our device blocks live in one arena mapping per APU (`arena_get`) and a `db_free` only returns the extent to our allocator; the OS is not involved and the arena's size is fixed at init by `dm_layout`. The equivalent lever is the formula (V2/V3), which is item 1. | 0 by itself |
| 7 | hwm-poller + [pk] probes | observability; `mem_report` per phase + `MEM_REPORT_DEVS=1` exist; a continuous poller is a shell script | — |
| 8 | `BATCH_DEVDAB_TIGHT`: "+0.5–1e10" | superseded by `ECALC_PLANE_CAP` (Category D) | 0 beyond the cap steps |
| 9 | `ENV_L3/ENV_DMAX` split | workflow; `estimate.py --target`, `design_table.py`, the M-run blocks serve it | — |
| 10 | `mul_dispatch_band` + operand madvise for the host-resident 10dP: "+2–3e10" | no 10dP in the decimal flow. The band idea's device counterpart is "V6": size the reciprocal's t1 to the top band actually used (u = t1 >> (take − j), corr = t1 >> j: the top ≈ n_Q limbs of a 2 n_Q product; `rns_mul_high_db` already skips the low pieces' computation but the output dbig is full length) → the reciprocal's floor 4 B → 3 B; then the low product's Q + X + xq + Aw (4.2 B) binds until the window is done in chunks (A5). Together they are what would take the floor from ≈ 2.3 to apumult's ≈ 2.0 bytes/digit. | after V3: ≈ 1.9 → 2.1–2.3e11 (assumed) |

"Projected combined 7e10 → 20–25e10 with items 1–6": on our layout items 1–6 reach **1.70e11 (2³¹) / 1.88e11 (2³⁰)** modelled; 2.0–2.3e11 needs V6 as well, and the run at that size takes 15–30 min per node (section 5).

---

## 4. §8 — the architectural note, checked

- Right: numbers are device-resident dbig quarters (one per APU; mdb shares at size > 1 are dbigs too, `x->sh`), so the spill path is device → pinned bounce → SSD.
- The bounce path: `dbig.c` `BOUNCE` = 2 × 1 GiB pinned per APU (`hipHostMalloc`, line 244; the comment quotes pinned `hipMemcpy` ≈ 50 GB/s) — it serves `db_from_bi/db_to_bi`. The checkpoint transfer is a different buffer, the 1 GiB `rns_hstage` per APU (`CKPT_CHUNK`, `ckpt_buf` line 557). The "1.6–2 GB/s" the doc attributes to dbig.c is WP7's measured NVMe DMA rate (write with fsync ≈ 1 GB/s sustained; A-ckpt: 0.8–1.5 GB/s single node, 0.4–0.7 per node when four processes share the disk).
- "Generalize BS_CKPT into a per-buffer spill primitive": **`ckpt_dbig_io(FILE*, dbig*, lo, hi, buf, write, io, part)` (`binsplit.c:596`) already is a per-dbig-range primitive in both directions**, and `bs_ckpt_bg_start/done/release` (lines 815–831) is a background writer over a P and a Q with per-part completion. What has to be added: (i) `db_free` on completion and `db_reserve` + read-back, four threads on their NUMA-local staging (`mem.c mem_hstage_alloc`); (ii) the callers' order changes in `recip_db2`, `newton_db_divmod_shifted`, `newton_mn_divmod`, `ecalc.c` (P restore before line 513); (iii) the formula: `dm_layout` v2 minus the spilled terms and v3 without the hole, the tail moved to the dead parity's half (`arena_half_range`, `db_pool_set_tail`), mirrored in `mem_model.py dm_layout`; (iv) a spill directory switch (the target's `/ssd0`; aac6's `BS_CKPT_DIR` disk) and the layout report (`BS_LAYOUT_ONLY`) printing the spilled variant so the M-run can compare before running.
- Not in the note but decisive: the memory is freed by the formula, not by the spill (section 0), and the pool's fragmentation at the edge (G13d) is the risk the tail policy must handle; `ECALC_INIT_ONLY=1` + `BS_LAYOUT_ONLY` give the check before a 20-minute run.

---

## 5. Time cost (one node)

Traffic per run (P, Q ≈ 0.444 bytes/digit each; writes + reads):

| digits | V2 (P w+r) | V3 (P, Q w+r) | compute (measured / extrapolated) |
|---|---|---|---|
| 7.64e10 | 68 GB → 68 / 34 / 23 s at 1 / 2 / 3 GB/s | 136 GB → 136 / 68 / 45 s | 133–138 s measured (§80, G13d) |
| 1e11 | 89 GB → 89 / 44 / 30 s | 178 GB → 178 / 89 / 59 s | 207 s (3·2³⁰, P13b), 225 s (defaults, G13d) measured |
| 1.4e11 | 124 GB → 124 / 62 / 41 s | 249 GB → 249 / 124 / 83 s | 1086–1139 s at 2³⁰ measured (G13d); 2³¹ does not fit today |
| 1.7e11 (V3 ceiling, 2³¹) | 151 GB → 151 / 75 / 50 s | 302 GB → 302 / 151 / 101 s | 575 s (d log² d) – 838 s (local exponent 2.3) extrapolated from 413 s at 1.252e11 |
| 2e11 | 178 GB → 178 / 89 / 59 s | 356 GB → 356 / 178 / 119 s | 685–1218 s extrapolated; does not fit even with V3 (585 GB at 2³¹, 548 at 2³⁰) |
| 2.35e11 | 209 GB → 209 / 104 / 70 s | 418 GB → 418 / 209 / 139 s | 815–1766 s extrapolated; 676 / 641 GB with V3: does not fit |

Exposed vs hidden: the writes go under the reciprocal (P; the writer thread exists and W measured it hidden at ≳ 1.1 GB/s at 4e10, N13) and under the second product + high product (Q); the reads are exposed (P before S = P + Q, Q before the low product; at the ceiling there is no free space to prefetch into). So V2 costs ≈ one P read = **+3–13 % of the wall at 1.7e11** (25–75 s at 3–1 GB/s), V3 ≈ two reads = **+6–26 %**. aac6's rate is a lower bound (the integrator's note); at the target's `/ssd0` rate (unknown to us; another agent measures aac6) the exposed part scales inversely.

---

## 6. 576 nodes

Per node at the target (7.38e10 average, 7.64e10 top node; modelled with the design's switches, this run of `mem_model.py`): **452 GB = planes 103.7 + arena 309.2 + exchange 12.3 + host 30.2** (SHMEM pool 8.6), where the arena = max(bs regions 185, **dm need 309 = 229 (the shares' v2/v3, same structure as size 1 with n_Q/576) + 80 top-product scratch**, tree 294).

| variant | dm need per node | arena (= max with tree 294) | node at 7.38e10 | 576 ceiling at 480 GB (502) | total digits |
|---|---|---|---|---|---|
| V0 | 309 | 309 | 452 | 8.11e10 (8.62) | 4.67e13 (4.97) |
| V1 (P share spilled) | 309 (v3 binds) | 309 | 452 | 8.11e10 | 4.67e13 — **no change** |
| V2 (+ tail policy) | 270 | **294 (the tree binds)** | 437 | 8.51e10 (9.13) | 4.90e13 (5.26) |
| V3 / V5 | 243 / 237 | 294 | 437 | 8.51e10 | 4.90e13 — the tree caps it |
| + tree-level spill (the child's Q_i and the running P cold during P_i·Q_run, ≈ 60 GB; assumed) | 243 | ≈ 240 | ≈ 385 | ≈ 9.6e10 (assumed: −65 GB at the marginal 4.2 bytes/digit of the 452 → 463 GB step §82) | ≈ 5.5e13 |

So: spilling does **not** raise the 576-node ceiling the way it raises the one-node one. The per-node budget at 576 also carries the top product's scratch (80 GB, which `MN_T_CHUNK_MB=1024` — built, off — cuts to ≈ 50: M13), the exchange and the SHMEM pool, and the tree's top level (294 GB) is the second wall right behind the dm need. The cheaper lever there is the built chunking switch (5.32e13 modelled, M13/§78) before any spill. At the chosen target (4.25e13, 7.38e10/node) a V2 spill converts to **margin** (452 → 437 GB per node; with the tree spill → ≈ 385), which is relevant for the open SHMEM-pool sizing (T0) and the exchange-stacking question (M13 open issue 2), not to digits.

Time at 576: V2 = P share 36 GB written under the reciprocal (hidden if `/ssd0` ≥ ≈ 0.7 GB/s over the ≈ 50 s reciprocal; assumed) + 36 GB read exposed = **+12–36 s on the 234 s wall (+5–15 %)**; V3 doubles it; the tree spill adds ≈ 2 × 60 GB per node exposed at each top level: +40–120 s. All per node, in parallel over the nodes, each on its own `/ssd0` (assumed).

---

## 7. The doc's claims about ntt/ — current / stale / wrong

| claim | status | evidence |
|---|---|---|
| single-node d_max 7e10 "in-core, ~440 GB" | **stale** (Phase 8, §71 of 2026-09-18) | measured ceilings at three primes: 1.44e11 (2³⁰), 1.30e11 (2³¹), 1.14e11 (3·2³⁰) — P13b jobs 21072/21069/21059, VERIFY OK; the defaults run 1.252e11 (G13d) |
| µ @ 4e10 = 73.1 s | **stale** | 63.5 ± 1.5 s (five runs, §80 job C13c); 67.8 ± 1.9 with three primes alone (§78) |
| bytes/digit 6.3 (440/70) | **stale** | 340.7/7.64e10 = 4.46 (§80); 507.6/1.44e11 = 3.52 (2³⁰), 507.1/1.30e11 = 3.90 (2³¹) (P13b) |
| "7e10/node × 576 = 4.0e13" | **mixed**: 4.0e13 is the modelled 576 ceiling at 6.95e10/node (M13/§78), coincidentally ≈ 7e10; the one-node ceiling is not what limits 576 | current: target 4.25e13 (7.38e10/node) in 234 s at 452 GB (§82); design-table ceilings 4.66e13 (defaults, 480 GB), 5.5e13 (recommended), 6.4e13 (largest) (§79/§80) |
| "235e9/node × 576 = 1.35e14" | **wrong** on two counts | (i) the 576 per-node ceiling is 55–70 % of the one-node one (tree top level, top scratch, exchange, SHMEM pool: section 6); (ii) time: apumult is 3.7× slower per digit at 4e10 (234.8 vs 63.5 s) and our own run at 1.44e11 takes 18 min per node (G13d); a 2.35e11-per-node run is ≥ 30 min per node by either extrapolation, against a 234 s target |
| "dm phase overflows the block pool @ 7e10 (§71)" | **stale** | zero in-phase hipMalloc at 4e10, 8e10, 1e11 since M11's tail arena (M13 calibration table); the only in-phase failure is fragmentation at 1.245e11 (G13d) |
| "their bs top levels overflow @ 8e10" | **wrong** | 8e10: 369.1 GB, hipMalloc 0 (M11/M13); every default run 5.12e10 … 1.163e11 clean (G13d); region growth aborts unless `RNS_POOL_GROW=1` (Phase 12 R) and none was needed |
| "recip peak block-pool 121.5 GB @ 4e10" | **not ours** | measured arena 132.3 GB at 4e10 (M13/§77); Phase 10's pool 141.0 (`mem_model.py MEASURED`) |
| "zero spill/stream/trim of working set; the only disk write is BS_CKPT_*" | **current**, with a nuance | `ECALC_CKPT_TOP` (off by default) writes P under the reciprocal and Q under the division and releases P before S = P + Q (§77, N13): the write half of a P spill exists; the buffers are never freed for memory, and Q is *held* +17.8 GB when the writer lags (§78) |
| "1 madvise (HUGEPAGE), disabled" | **roughly current** | four `MADV_HUGEPAGE` sites (`bigint.c:17`, `mem.c:134/230/252`); all off the default path (`BI_HUGE=0` measured harmful; the mem.c ones are in the non-default mmap/hreg forms) |
| "dbig.c 1 GB pinned chunks, 1.6–2 GB/s" | **half right** | dbig.c's bounce is 2 × 1 GiB per APU for host↔device copies (≈ 50 GB/s pinned); the checkpoint path uses the 1 GiB `rns_hstage` chunk; 1.6–2 GB/s is WP7's NVMe DMA figure, the write with fsync ≈ 1 GB/s, aac6's slow path 0.31 GB/s |
| "ntt/ divisor cache has no trim/rebuild" | **misleading** | no divisor cache on the decimal path at all |
| "their planes are fixed 2³¹/3·2³⁰" | **stale** | `ECALC_PLANE_CAP` with four caps and `fit` (P13b); 2³¹ the default (§80) |
| "ntt/ has no OOM guard" | **current** | edge runs were killed by the kernel (P13b) or a refused hipMalloc (G13d); the guards are pre-run (`BS_LAYOUT_ONLY`, `ECALC_INIT_ONLY`) |
| "§66 grid-split, no madvise" | current (the grid is `rns_dist.c` mn/mul grids; no madvise on device memory is possible) | — |

---

## 8. What to test, ranked

1. **V2 — P spill under the reciprocal + the tail policy** (`ecalc.c` 491–513, `bs_ckpt_bg_*` with `db_free` on part 0 done, restore via `ckpt_dbig_io`; `dm_layout` v2 − 1.1 B and v3 − hole; tail = the dead parity's half). Gate: `BS_LAYOUT_ONLY` shows the new arena; `ECALC_INIT_ONLY` maps at 1.5e11 (2³¹); a real 1.5e11 run VERIFY OK on aac6 (today's edge is 1.30e11). Expected: **1.30 → ≈ 1.68e11 at 2³¹, 1.44 → ≈ 1.79e11 at 2³⁰ (modelled)** for one exposed P read (25–75 s). This is also the one that pays at 576 (+5 % digits, or −15 GB per node of margin).
2. **V3 — r2 reserved after the first product; Q spilled from the last iteration's second product through the high product; Aw formed and S freed before the low product** (`recip_db2`, `newton_db_divmod_shifted`, `newton_mn_divmod`). Expected +1 % (2³¹) / +5 % (2³⁰) over V2; the reciprocal's r + r2 + t1 + piece and the low product's Q + X + xq + Aw are then the floor (≈ 4.2 B + piece).
3. **576: `MN_T_CHUNK_MB=1024` first** (built, off; −30 GB per node of top scratch, 5.3e13 modelled, M13) — cheaper than any spill there; then the tree-level spill (child's Q_i, running P during P_i·Q_run in `tree_level_k`) modelled in `tree_need_dev` before writing it; expected ≈ 9.6e10/node (assumed).
4. **The 10 % bound margin on Q and S in `dm_layout`** (≈ 2 %): trivial, verify the bound first.
5. **V6 (band-sized t1, the window in chunks)** only if 2e11+ per node is wanted: the remaining ≈ 15 % to apumult's 2.0 bytes/digit, at 15–30 min per node.
6. Not worth porting: µ spill/stream, Q stream into the grid products, the r-spill/stream-shl, madvise(DONTNEED), the DC/pow10 trims, `BATCH_DEVDAB_TIGHT` (superseded by `ECALC_PLANE_CAP`).
