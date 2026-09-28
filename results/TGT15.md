# TGT15 — the target moved to 5.1 × 10¹³ digits on 576 nodes (Phase 15, agent TGT, 2026-09-28)

Branch `p15-TGT` from `main` f184d51. Desk work: the docs and the models' target constants. No node jobs. The plan and layout
checks ran on aac6's login node against `~/ntt-NP15` at 994e43a, used read-only. Times are Eastern.

**The user's decision (2026-09-27, 23:50 EDT):** the target is **5.1 × 10¹³ digits on 576 nodes**. It was 4.25 × 10¹³. The runtime
one step below, 4.74 × 10¹³, is to be tested after the headline run.

## 1. What changed

| file | change |
|---|---|
| `ecalc/mem_model.py` | `TARGET_DIGITS = 5.1e13`, `TARGET_BELOW = 4.74e13`, `TARGET_NODES = 576`, with dated history comments. `pool_target()` and the `--p15` target block now use them (they had 4.25e13 hard-coded). The historical reports stay as they were: `savings` at 7.38e10 and `e10_report`. |
| `ecalc/mn_model.py` | re-exports `TARGET_DIGITS`, `TARGET_BELOW`, `TARGET_NODES`. |
| `ecalc/estimate.py` | `--target` prints these rows: the target, 5.11 (node 0 steps), 5.12 (the critical path steps), 5.17, 4.74 (one step below), 4.75 (its step), and 4.25 (the previous target). It gives a by-phase line for 5.1 and 4.74 (new `by_phase()`). The help text notes that `ECALC_NP=auto` is pending. |
| `ecalc/design_table.py` | `COMMON = M.TARGET_DIGITS`. Column (e) is at 5.1 × 10¹³. |
| `results/DESIGN_TABLE.md` | regenerated at 364c727 (215 s, all modelled, no M-run log). |
| `docs/TARGET.md` | Header. §1: a new block with the table of steps, the memory, the top node, the one-node rehearsal, the disk and the off-the-clock times; the DOC block is marked as history. §3: the pool, `ECALC_NP`, `MN_T_CHUNK_MB` and `ECALC_OUT_PACKED` rows. §4: the launch line is now `ecalc 51000000000000`, plus the part sizes, RECHECK and conversion times, and the plan-only line. §5: step 5's byte fraction, step 6 (the target), 6b, the new **6c (4.74 × 10¹³)** and step 7. §6: items 4, 5 and 7. §7: the read-back times, and a warning on the `cat`. §8: traps 12 and 13 updated, new **trap 14** (the Lustre capacity). |
| `docs/TARGET_TASKS.md` | "Where things stand"; T0, T3, T4, T4b, T11; the new **T4c: the runtime one step below, 4.74 × 10¹³, after the headline**. |
| `ecalc/README.md` | the header's launch-line sentence names the target; the `ECALC_NP` and `MN_PLAN_ONLY` rows get their 5.1 × 10¹³ plan results. |
| `docs/APUMULT_STUDY.md` | a dated note (2026-09-28). |
| `results/TGT15/` | `sweep576.txt` (the plan at 4.25–5.17 × 10¹³), `plan576_51e13.txt`, `plan576_474e13.txt` (the full plans), `layout.txt` (`BS_LAYOUT_ONLY` at 5.1e13/576, 4.74e13/576 and 9.169e10/1, and the size-1 plan sweep). |

Every dated history line was kept, and the old figures are still recorded. `PLAN.md` and `RESULTS.md` were not touched. No C code changed,
so the digits are unaffected.

## 2. The numbers (labels: measured = a login-node run of the code's own plan or layout; modelled = `estimate.py` / `mem_model.py`; assumed = the fabric and the 576-writer disk rate)

### Plan (measured on aac6's login node)
Environment: `ECALC_NP=4 COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576`, run as
`MN_PLAN_ONLY=<d>:576 ./ecalc`. Each run takes < 0.1 s.

| digits | node 0 pieces (tree + recip + div) | critical path | plan check |
|---|---|---|---|
| 4.25e13 | 86 + 66 + 28 = 180 | 182 | OK |
| 4.70e13 / **4.74e13** | 100 + 68 + 34 = 202 | 226 | OK (largest piece 1.097e12 limbs) |
| 4.75e13 | 100 + 73 + 36 = 209 | 233 | OK |
| 5.05e13 / **5.10e13** | 108 + 74 + 40 = 222 | 242 | OK, 1240 products, largest piece 1.096e12 limbs (dist_mn level 8), transforms 2⁴⁰ of 2⁴⁴ |
| 5.11e13 | 112 + 74 + 40 = 226 | 242 | OK |
| 5.12e13 | 116 + 74 + 40 = 230 | 246 | OK |
| 5.17e13 | 230 | 266 | OK |

- `plan pool` is 9472 MiB at 5.1e13 and at 4.74e13, the same as at 4.25e13. The heap needs ≥ 9984 MiB.
- With `ECALC_NP=3` at 5.1e13 the plan is `plan REFUSED`: 85 of 1240 products are over the term bound, the first at tree level 5, and it exits with rc 3.
- **Step margin.** 5.10 × 10¹³ is the last size, in steps of 0.01 × 10¹³, at 222 / 242 pieces. At 5.11 node 0 steps; at 5.12 the
  critical path steps (+5.2 s, modelled). So the margin is 0 at that resolution, and 0.02 × 10¹³ to the step that costs time.
- `mn_model.plan` gives the same counts at all nine sizes.

### Walls (modelled; `estimate.py --target`, `ECALC_NP=4`, packed, early writer; 100 GB/s per APU and 2 µs assumed)

| digits | no write | @2.0 | @0.8 | @0.6 GB/s | node GB |
|---|---|---|---|---|---|
| **5.10e13** | **344.7 s (5.75 min)** | 339.3 | 359.8 | **376.2 s (6.27 min)** | **455.4** |
| 5.12e13 | 349.9 | 344.4 | 365.1 | 381.6 | 456.4 |
| **4.74e13** | **320.3 s (5.34 min)** | 315.3 | 336.3 | **351.5 s (5.86 min)** | 439.2 |
| 4.75e13 | 328.5 | 323.5 | 343.4 | 358.6 | 439.7 |
| 4.25e13 (history) | 256.0 | 251.5 | 272.0 | 285.7 | 416.0 |

- **5.1e13 by phase:** init 19.3, seed wait 12.9, batch 30.8, top 30.5, distributed levels 150.1, reciprocal 43.4, division 49.7, other 0.2,
  residues 5.4, exit 2.4.
- **ASCII instead of packed:** 355.2 / 421.6 / 458.5 s at 2.0 / 0.8 / 0.6 GB/s.
- **`ECALC_NP=auto`:** the integrator's figures on `p15-NP` are 322.6 s without the write and 354.1 / 337.7 / 317.2 s with it at 0.6 / 0.8 / 2.0 GB/s. It is
  pending Batch 2, so the docs are written for `ECALC_NP=4`.
- **Design table:** the recommended row is unchanged (auto / 2³¹ / both / d2): 5.75 min without the write and 6.27 min with it. The fastest row is
  now "shift" (`MN_T_CHUNK_MB=0`) at 5.41 min, but it holds only 4.88 × 10¹³ at 480 GB, so it cannot hold the target inside 480.

### Memory
- **C layout (measured, login node).** `BS_LAYOUT_ONLY=88541666666.6667:576` gives planes 120.88 + arena 284.14 = device 405.02 GB, and node
  426.61 GB with the layout's own host figure. At 4.74e13: device 388.83 GB, node 410.42 GB.
- **`mem_model.py --p15` (modelled).** Node 455.4 GB = max(init 405.0 + 44.4, bs 411.0 + 44.4, dm 417.0 + 28.8). That is 24.6 GB below 480.
- **Settings that now decide whether the target fits in 480 GB (modelled).** Changing any one of these puts the node over 480 GB:

  | setting | node |
  |---|---|
  | `COMM_SHMEM_ROUND_MB` off | 496.0 GB (pool 48128 MiB) |
  | `MN_T_CHUNK_MB=0` | 490.5 GB (it would be −19.6 s) |
  | `MN_TREE_EARLY_FREE=0` | 482.5 GB |
  | `DM_TIGHT=0` | 502.8 GB |

- AS's pending `BS_ARENA_ROOM=0.16` adds ≈ 9 GB, which gives ≈ 464 GB (the integrator's figure).

### Top node and the one-node rehearsal
- **Top node's share.** The code gives each node the same number of terms, so node 575's share is
  `lf(N) − lf(N·575/576)` with N = 4 184 661 447 309 (the plan's N; `mem_model.e_terms` gives the same). That is **9.169 × 10¹⁰ digits = 1.0356 × the average
  8.854 × 10¹⁰**. Node 0 holds 6.849 × 10¹⁰ (0.774 ×). At 4.74e13 the top node holds 8.523 × 10¹⁰ (1.0357 ×), and at 4.25e13 it held 7.643 × 10¹⁰, which checks the old 1.036.
- **The one-node run for the top node:** `./ecalc 91694091804`.

  | configuration | layout: device / node (measured) | `mem_model` node (modelled) | wall without the write (modelled) |
  |---|---|---|---|
  | defaults (three primes) | 362.93 / 378.52 GB | 396 GB | 174.2 s |
  | `ECALC_NP=4` (the target's primes) | — / 395.70 GB | 413 GB | 194.0 s |

  - `MN_PLAN_ONLY=91694091804:1` gives plan check OK and 113 pieces. Sizes 8.8–9.2e10 plan at 113–114 pieces and 9.3e10 at 123.
  - **It fits one aac6 node** with ≥ 67 GB below 480 in either configuration, and it is under the measured 524 GB edge. It is the Phase-13-style
    rehearsal of the new target's top node. Run it with `ECALC_NP=4` to match the target's planes.

### Disk and off the clock (modelled from measured rates)
- **Part files:** 39.35 GB per node packed (22.67 TB in all). ASCII is 88.54 GB per node (51.0 TB). At 4.74e13: 21.07 TB packed and 47.4 TB ASCII.
- **Lustre, 122 TB shared:** packed and ASCII together are **73.7 TB (60 %)**. Adding one concatenated `e.txt` makes 124.7 TB, **more than the file
  system holds**. This is new trap 14: keep the parts and do not concatenate on the same file system. The free space on the day is assumed, not
  known; check with `lfs df -h`.
- **Per node, all 576 in parallel:**
  - RECHECK of the packed part: 46–50 s at 0.78–0.86 GB/s read.
  - Conversion: **111–148 s (1.8–2.5 min)** at 0.8–0.6 GB/s write. IO215 measured 0.58–0.59 GB/s per stream, write-bound. The 39.35 GB read (24 s at
    1.67 GB/s) and the formatting hide under it.
  - RECHECK of the ASCII part: 103–114 s.
  - One stream over all the parts would take ≈ 24 h.
- **Assumed:** the aggregate carries 576 streams (≈ 346 GB/s). If it does not, the time is 51.0 TB divided by the aggregate rate.

## 3. Tests

| what | command | result |
|---|---|---|
| plan sweep | `results/TGT15/sweep576.txt`: the header line has the environment, run at 2026-09-27 23:52 EDT | as in §2 |
| full plans | `… MN_PLAN_ONLY=51000000000000:576 ./ecalc`, `…47400000000000:576` | rc 0, OK |
| three primes | `ECALC_NP=3 … MN_PLAN_ONLY=51000000000000:576 ./ecalc` | REFUSED, rc 3 |
| layouts | `results/TGT15/layout.txt` | as in §2 |
| models | `./estimate.py --target` (16 s); `./mem_model.py --p15`; `./mem_model.py --pool`; `./estimate.py --g 576 --D 7.7e10` (smoke) | as in §2; nothing broken |
| design table | `./design_table.py` (215 s) | 96 rows, written |
| TARGET §9 variable check | the script in §9 | only the pre-existing non-code names (model constants, library variables), plus `BS_ARENA_ROOM` (AS's branch, labelled pending) |

## 4. Open issues
- The 0 margin to the node-0 step at 5.11 × 10¹³ is fine only if the argument is exactly `51000000000000`, and the docs say so.
  Node 0's step at 5.11 does not move the critical path (242), so it would not cost wall time by the model.
- The fastest design row (`MN_T_CHUNK_MB=0`) no longer fits 480 GB at the target. T11 now says so.
- `ECALC_NP=auto` (−22 s modelled) waits on the user's Batch 2 decision. When it is adopted, update TARGET §1 and §4, TASKS T3 and T4, and
  `estimate.py --np-mn`'s default.
- `tests/*.py`: old phase scripts still name 4.25e13. They are historical and were left alone.
- The top set size at 5.1e13 (≈ 42 GB per node) is scaled from 35 GB, not computed.

## RESUME
Done: every step above, committed on `p15-TGT` (364c727 … the last commit). Nothing is running and there are no jobs; aac6's home
has no leftover files (the `tgt15_*` files were removed). If resumed: nothing is left except the integrator's merge. On merge,
`results/DESIGN_TABLE.md` should be regenerated if `main` has moved in the models.
