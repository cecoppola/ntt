# TARGET — the target (digits and 576 nodes)

Scope: the target (digits and 576 nodes): facts, plans, device edge, scouting, documentation on the user's decisions, steps on real hardware. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| TGT | target facts (user-supplied 2026-10-12) and what they change |
| TGT15 | the target moved to 5.1 × 10¹³ digits on 576 nodes (Phase 15, agent TGT, 2026-09-28) |
| TGT17 | the 4.08 × 10¹³ target: set to fit the device-memory edge comfortably (2026-10-06) |
| TGTBENCH2 | the user's second set of target tests, assessed against the ecalc design (2026-10-06, branch `tgtbench2`) |
| CAP17 | the device-memory edge at the target (373 GB/node) against the layout (2026-10-06) |
| SC15 | scouting Batch 3 for the target 5.1 × 10¹³ digits on 576 nodes (Phase 15, agent SC; desk work) |
| MS15 | the node memory at 5.1–5.7 × 10¹³ on 576 nodes, and the largest target that fits 480 GB (Phase 15, agent MS) |
| DOC15 | the documentation and the models on the user's decisions of 2026-09-27 (Phase 15, agent DOC) |
| DOC215 | the documentation and the models on the user's decisions of 2026-09-28 (Phase 15, agent DOC2) |
| L13d | the plan printer (PLAN §32, row L) |
| G13d | the steps on real hardware (Phase 13d, PLAN §32 row G) |
| S13d | SHMEM across real nodes (TASKS 1.5), Phase 13d agent S (2026-09-23) |

## TGT — target facts (user-supplied 2026-10-12) and what they change

(former file: TGT.md)

**Question.** The user supplied facts about the target machine. Where do they go, which backlog rows (OPPORTUNITIES.md)
change, what does the digit write cost against the file system's measured rate, and does any planned launch value change?

**Change / branch.** Branch `tgt`. No code or default changed. Model constants only: `ecalc/mn_model.py` gains the
user-supplied target facts (`TARGET_NICS_PER_APU=2`, `TARGET_NIC_GBS=50`, so `TARGET_BW_APU` = 100 GB/s, the same value
as before; `TARGET_FS_SINGLE_FILE_GBS=1000` at `TARGET_FS_WRITERS_PER_NODE=32`, `TARGET_WRITE_BW_FS` = 1.736 GB/s per
node, now added to the write rates every target estimate prints). `TARGET_WRITE_BW` (1.0 GB/s per node, ecalc's writer)
is unchanged, so the standing estimate does not move. `profiles/target.conf.template`: the keys these facts determine are
filled. PLAN.md §1 lists what is known.

**Facts (user-supplied, a).** 576 nodes × 4 MI300A = 2304 APUs; 8 Slingshot NICs per node at 400 Gb/s (2 per APU,
100 GB/s injection peak per APU, 400 GB/s per node); Rocky Linux 9.6; Slurm 25.11.8; ROCm 7.14.0 (may be upgraded later);
cray-shmem 12.1.0; CCE 21.0.0; libfabric 2.3.1; file system ≈ 1 TB/s cumulative write into one file at 32 processes per
node. Compared with aac7 (m): CCE 21.0.0 and libfabric 2.3.1 are the same, Slurm is the same 25.11 series (25.11.4),
OpenSHMEMX is 11.8.0 there (11.7.4 the only other), and aac7 has one NIC per APU.

**Method.**
- ROCm 7.14.0 build: aac7 login node, a scratch clone of `main`, `module unload rocm/7.0.3; module load rocm/7.14.0
  cray-dsmml cray-openshmemx`, `make -B -j16 SHMEM_CRAY=1 GMP_HOME=~/gmp ecalc`, then `make all tests/t_comm tests/t_edge`.
- Write time: `ecalc/estimate.py --fabric target_l4 --g 576 --D 6.632e10 --write-bw W` for W = 0.434, 0.6, 1.0, 1.736,
  2.0 GB/s per node; `--bw 50` for one effective NIC per APU.
- Plan values: `tools/plan_values.py --profile <the template with the site keys filled by placeholders> 38200000000000
  576` on the aac7 login node (`~/ntt-acc` build), also with `COMM_OFI_NICS` unset and with `COMM_OFI=0`.
- ecalc's writer read from `ALGORITHM.md` §9 and `ecalc/mn_out.c`.

**Results.**

ROCm 7.14.0 (m, 2026-10-12): `ecalc` and `make all` (tests, tools) build with `-DCOMM_SHMEM`, linked against the 7.14.0
`libamdhip64`; with or without `-fPIC` the `ld.lld` relocation failure of `results/V16.md` does not reproduce. Not run on
a compute node: no digits and no time yet.

ecalc's writer today: one process per node; a background writer thread with 8 pwrite threads per 256 MB chunk
(`MN_OUT_THREADS=8`, `MN_OUT_CHUNK_MB=256`), O_DIRECT on Lustre (`ECALC_ODIRECT=auto`); two part files per node
(`MN_OUT_DKM_HI`: X_hi from the division's step 1, then X_lo), 1152 files in all; striping by the directory unless
`MN_OUT_STRIPE`. So 8 concurrent writers per node into per-node files, not 32 writers into one file.

| write rate per node | aggregate | raw write of 15.92 TB (27.63 GB per node) | wall with the write (no-write 185.5 s) | exposed write |
|---|---|---|---|---|
| 1.736 GB/s, the file system's 1 TB/s (u) | 1000 GB/s | 15.9 s | 186.5 s | 1.0 s (mod) |
| 1.0 GB/s, the model's prior for ecalc's writer (a) | 576 GB/s | 27.6 s | 198.3 s | 12.8 s (mod) |
| 0.6 GB/s, the old single-stream prior (a) | 346 GB/s | 46.1 s | 216.7 s | 31.2 s (mod) |
| 0.434 GB/s, 8 of 32 writers if the rate scales with writers (a) | 250 GB/s | 63.7 s | 234.3 s | 48.8 s (mod) |

Bytes: 3.82 × 10¹³ digits × 7.5 B / 18 digits = 15.92 TB (packed 60-bit form). The writer design gap is 11.8 s against the
1.0 GB/s prior and up to 47.8 s against the writer-scaled case (mod); new row N8.

Fabric: 100 GB/s per APU was the assumed design value and is now the user-supplied peak, so the estimate is unchanged
(185.5 s without the write at 3.82 × 10¹³ on 576 nodes in this run, mod). With one effective NIC per APU (50 GB/s): 222.7 s,
+37.2 s (mod). `comm_ofi` takes the cxi devices on each APU's NUMA node and stripes over them when `COMM_OFI_NICS` is unset,
so two NICs per APU are used without a setting; the template no longer sets `COMM_OFI_NICS`.

Plan values (m, planner on the aac7 login node): unchanged by these facts. `COMM_SHMEM_POOL_MB` 2304, heap 2816 MiB,
`FI_UNIVERSE_SIZE` 4096, `MN_GROUPS` 2,4,8,16,32,64,192,576, pool_log 31, `plan check` OK with 1236 products. With
`COMM_OFI=0` (SHMEM carries the device exchanges): pool 10240 MiB, heap 10752 MiB, plan check OK.

OPPORTUNITIES changes: M7c re-scored (the two-NIC path is the code's default; the open item is to verify it, +37 s at stake);
M7d re-worded (ROCm 7.14.0 builds; measure its speed on aac7, re-measure at target upgrades); M7b folded into N8's test;
N4 notes the Slurm series; new N7 (SHMEM as the primary transport, cray-shmem 12.1.0 untested) and N8 (writer design gap).
Top 5 for the target: M7c, N8, N7, N1, M7d.

**Verdict.** No default changes. Three items for the target work: verify two NICs per APU (M7c), benchmark ecalc's writer
shape against the file system's measured mode (N8), measure the SHMEM path and 12.1.0 (N7). One aac7 item is new: ROCm
7.14.0 against 7.2.4 at 1 node (M7d).

**Open issues.**
- The module name behind "cray-shmem 12.1.0" is assumed to be `cray-openshmemx/12.1.0`; the kit's auto-detection looks
  only for `cray-openshmemx/*`.
- Fixed (FIX1): the loader accepts `TARGET_PARTITION`, `TARGET_NODES` and `ECALC_OUT_DIR` as profile meta keys (never exported), and
  `estimate.py --target` / `mem_model.TARGET_DIGITS` now use 3.82 × 10¹³ (3.71 × 10¹³ stays as a history row).

**RESUME.** Branch `tgt` merged to `main`. Next: the M7d ABBA on aac7 (1 node, `rocm/7.14.0` vs `rocm/7.2.4`), the N7
`COMM_OFI=0` A/B at 2–6 nodes, and for the target kit a `tools/wbench` run at scale with `-t 8` and `-t 32`.

## TGT15 — the target moved to 5.1 × 10¹³ digits on 576 nodes (Phase 15, agent TGT, 2026-09-28)

(former file: TGT15.md)

Branch `p15-TGT` from `main` f184d51. Desk work: the docs and the models' target constants. No node jobs. The plan and layout
checks ran on aac6's login node against `~/ntt-NP15` at 994e43a, used read-only. Times are Eastern.

**The user's decision (2026-09-27, 23:50 EDT):** the target is **5.1 × 10¹³ digits on 576 nodes**. It was 4.25 × 10¹³. The runtime
one step below, 4.74 × 10¹³, is to be tested after the headline run.

#### 1. What changed

| file | change |
|---|---|
| `ecalc/mem_model.py` | `TARGET_DIGITS = 5.1e13`, `TARGET_BELOW = 4.74e13`, `TARGET_NODES = 576`, with dated history comments. `pool_target()` and the `--p15` target block now use them (they had 4.25e13 hard-coded). The historical reports stay as they were: `savings` at 7.38e10 and `e10_report`. |
| `ecalc/mn_model.py` | re-exports `TARGET_DIGITS`, `TARGET_BELOW`, `TARGET_NODES`. |
| `ecalc/estimate.py` | `--target` prints these rows: the target, 5.11 (node 0 steps), 5.12 (the critical path steps), 5.17, 4.74 (one step below), 4.75 (its step), and 4.25 (the previous target). It gives a by-phase line for 5.1 and 4.74 (new `by_phase()`). The help text notes that `ECALC_NP=auto` is pending. |
| `ecalc/design_table.py` | `COMMON = M.TARGET_DIGITS`. Column (e) is at 5.1 × 10¹³. |
| `results/DESIGN_TABLE.md` | regenerated at 364c727 (215 s, all modelled, no M-run log). |
| internal target notes | Header. §1: a new block with the table of steps, the memory, the top node, the one-node rehearsal, the disk and the off-the-clock times; the DOC block is marked as history. §3: the pool, `ECALC_NP`, `MN_T_CHUNK_MB` and `ECALC_OUT_PACKED` rows. §4: the launch line is now `ecalc 51000000000000`, plus the part sizes, RECHECK and conversion times, and the plan-only line. §5: step 5's byte fraction, step 6 (the target), 6b, the new **6c (4.74 × 10¹³)** and step 7. §6: items 4, 5 and 7. §7: the read-back times, and a warning on the `cat`. §8: traps 12 and 13 updated, new **trap 14** (the Lustre capacity). |
| internal target task list | "Where things stand"; T0, T3, T4, T4b, T11; the new **T4c: the runtime one step below, 4.74 × 10¹³, after the headline**. |
| `ecalc/README.md` | the header's launch-line sentence names the target; the `ECALC_NP` and `MN_PLAN_ONLY` rows get their 5.1 × 10¹³ plan results. |
| internal apumult study | a dated note (2026-09-28). |
| `archive:results_raw_2026-10-19/TGT15/` | `sweep576.txt` (the plan at 4.25–5.17 × 10¹³), `plan576_51e13.txt`, `plan576_474e13.txt` (the full plans), `layout.txt` (`BS_LAYOUT_ONLY` at 5.1e13/576, 4.74e13/576 and 9.169e10/1, and the size-1 plan sweep). |

Every dated history line was kept, and the old figures are still recorded. `PLAN.md` and `RESULTS.md` were not touched. No C code changed,
so the digits are unaffected.

#### 2. The numbers (labels: measured = a login-node run of the code's own plan or layout; modelled = `estimate.py` / `mem_model.py`; assumed = the fabric and the 576-writer disk rate)

##### Plan (measured on aac6's login node)
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

##### Walls (modelled; `estimate.py --target`, `ECALC_NP=4`, packed, early writer; 100 GB/s per APU and 2 µs assumed)

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

##### Memory
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

##### Top node and the one-node rehearsal
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

##### Disk and off the clock (modelled from measured rates)
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

#### 3. Tests

| what | command | result |
|---|---|---|
| plan sweep | `archive:results_raw_2026-10-19/TGT15/sweep576.txt`: the header line has the environment, run at 2026-09-27 23:52 EDT | as in §2 |
| full plans | `… MN_PLAN_ONLY=51000000000000:576 ./ecalc`, `…47400000000000:576` | rc 0, OK |
| three primes | `ECALC_NP=3 … MN_PLAN_ONLY=51000000000000:576 ./ecalc` | REFUSED, rc 3 |
| layouts | `archive:results_raw_2026-10-19/TGT15/layout.txt` | as in §2 |
| models | `./estimate.py --target` (16 s); `./mem_model.py --p15`; `./mem_model.py --pool`; `./estimate.py --g 576 --D 7.7e10` (smoke) | as in §2; nothing broken |
| design table | `./design_table.py` (215 s) | 96 rows, written |
| TARGET §9 variable check | the script in §9 | only the pre-existing non-code names (model constants, library variables), plus `BS_ARENA_ROOM` (AS's branch, labelled pending) |

#### 4. Open issues
- The 0 margin to the node-0 step at 5.11 × 10¹³ is fine only if the argument is exactly `51000000000000`, and the docs say so.
  Node 0's step at 5.11 does not move the critical path (242), so it would not cost wall time by the model.
- The fastest design row (`MN_T_CHUNK_MB=0`) no longer fits 480 GB at the target. T11 now says so.
- `ECALC_NP=auto` (−22 s modelled) waits on the user's Batch 2 decision. When it is adopted, update TARGET §1 and §4, TASKS T3 and T4, and
  `estimate.py --np-mn`'s default.
- `tests/*.py`: old phase scripts still name 4.25e13. They are historical and were left alone.
- The top set size at 5.1e13 (≈ 42 GB per node) is scaled from 35 GB, not computed.

#### RESUME
Done: every step above, committed on `p15-TGT` (364c727 … the last commit). Nothing is running and there are no jobs; aac6's home
has no leftover files (the `tgt15_*` files were removed). If resumed: nothing is left except the integrator's merge. On merge,
`results/DESIGN_TABLE.md` should be regenerated if `main` has moved in the models.

## TGT17 — the 4.08 × 10¹³ target: set to fit the device-memory edge comfortably (2026-10-06)

(former file: TGT17.md)

Login-node and local work only (aac7 `aac7-login`, no GPU, no compute jobs), on `main` fast-forwarded to `eebb0da` in the
`~/ofimem17` clone (rebuilt, `source ecalc/aac7env.sh && make -s -j16`), the launch line of internal target notes §4
(`DM_MN_LEAN=1`, `comm_ofi` default on cxi). This note follows **the user's decision of 2026-10-06**: set the target
digit count so the run fits COMFORTABLY in the target's available memory, not razor-thin against a bar; it can be
raised later if the memory configuration changes. It replaces results/TARGET.md §CAP17's 4.452 × 10¹³ answer (margin 0.53
GB to CAP17's 363 GB bar) with a size chosen against tighter bars (353 GB device, 480 − 30 GB node) and a larger
margin. Every number is **measured** (the C binary's own `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` print, no HIP calls)
unless marked **modelled** or **assumed**.

#### 1. The bars

- (a) device layout ≤ 373 − 20 = **353 GB/node** (the target's `hipMalloc` edge, A6 **measured**, minus a 20 GB
  margin — tighter than CAP17's 10/5 GB margins).
- (b) node total + 30 GB (the B7 v-exchange-slot allowance, **assumed**: STD17 found ≈ 13 GB/node of general-map
  v-exchange slots outside the layout at 10 nodes, 15–30 GB estimated at 576) ≤ **480 GB**, i.e. node total ≤ 450 GB.
- (c) `plan check` OK, and the size sits **just below** a grid step, not on it.

#### 2. Method

Same as CAP17 §2: `BS_LAYOUT_ONLY=<D per node>:576 ./ecalc <D> /dev/null` and `MN_PLAN_ONLY=<D>:576 ./ecalc` with
the launch line's environment: `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto
RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 DM_MN_LEAN=1
COMM_SHMEM_ROUND_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6
COMM_OFI_PLAN_CXI=1` (the last substitutes for a real cxi NIC, absent on the login node, per CAP17's trap 20 —
without `COMM_TRANSPORT=shmem` the `room:` line silently drops the SHMEM and `comm_ofi` pools). `device` = `room
planes` + `room arena_with_room` + `room ofi_pool` (bytes; CAP17's definition, excludes the SHMEM symmetric pool and
the host-side terms, which the code buckets under `host`). `node` = the C binary's own `node` figure (planes + arena
+ bs_grow + host, which includes the SHMEM pool, the OFI pools and the seed buffers). Every figure cross-checked
with `python3 mem_model.py --check-c`: **exact (0.0000 %) at every point tested**.

#### 3. The sweep

At the default `ECALC_PLANE_CAP=2^31`, `ECALC_NP=auto`, sweeping `D` (total digits on 576 nodes):

| D range | arena | device (planes 120.877 + arena + ofi_pool 9.664) | vs 353 GB | node | vs 450 GB |
|---|---|---|---|---|---|
| 4.0–4.0815 × 10¹³ | 214.748 GB | **345.29 GB** | **fits, +7.71** | 387.37 GB | fits, +62.63 |
| 4.0816–4.0899 × 10¹³ | 223.338 GB | 353.88 GB | fails, −0.88 | 395.96 GB | fits, +54.04 |
| 4.1–4.44 × 10¹³ | 223.338 GB | 353.88 GB | fails | 395.96 GB | fits |
| 4.452 × 10¹³ (CAP17's boundary) | 231.928 GB | 362.47 GB | fails, −9.47 | — | — |

The step from 214.748 GB to 223.338 GB — the one that matters for the 353 GB bar — was bisected on the login node
(`BS_LAYOUT_ONLY`/`MN_PLAN_ONLY`, 576 nodes) to **1 × 10⁹ digit resolution**:

| D | arena (bytes) |
|---|---|
| 40,815,000,000,000 | 214,748,364,800 (the lower tier) |
| 40,816,000,000,000 | 223,338,299,392 (the step) |

**The grid step is exact at 4.0815 × 10¹³ → 4.0816 × 10¹³.** Both device bound (a) and node bound (b) are flat
across the whole lower tier (4.0–4.0815 × 10¹³) — device 345.289514112 GB, node 387.374957984 GB — because planes,
arena and the ofi pool are all unchanged in this range (the same mechanism CAP17 found: the grid/plane structure is
flat across narrow ranges). Bound (b) (node total + 30 ≤ 480) is **not binding anywhere in this sweep** — even the
failing 4.0816–4.44 × 10¹³ range sits at 395.96 + 30 = 425.96 GB, 54 GB under 480. Bound (a) (device ≤ 353 GB) is
the one that decides the answer, exactly as CAP17 found for its own bars.

#### 4. The answer

**4.08 × 10¹³ digits** (`ecalc 40800000000000`) — a clean round number inside the lower tier, **1.6 × 10¹⁰ digits
(0.039 %) below the grid step at 4.0816 × 10¹³**, not on it.

- **Device layout: 345.289514112 GB** (planes 120,877,472,896 B + arena_with_room 214,748,364,800 B + ofi_pool
  9,663,676,416 B) — **7.71 GB of margin to the 353 GB bar** (bar (a)).
- **Node total: 387.374957984 GB**; **+ 30 GB (B7) = 417.37 GB — 62.63 GB of margin to 480 GB** (bar (b)).
- **`plan check` OK — 1238 products**, the largest piece 1,416,666,666,673 limbs (dist_mn j 566,666,666,668 →
  1,133,333,333,335 Q_t r, g 576; P24 piece 1,133,333,333,338 + 283,333,333,335 limbs = 850,000,000,004 +
  212,500,000,002 points), bound per product (four primes over 58,424,467,928 terms: **119 of 155 pieces**); the
  longest transform 2^40 of 2^44.
- **Distance to the next step: 16,000,000,000 digits** (4.0816 × 10¹³ − 4.08 × 10¹³), where the arena steps 214.748
  → 223.338 GB and device jumps to 353.88 GB (−0.88 GB, fails bar (a)).
- `mem_model.py --check-c`: **exact, 0.0000 %** (0 terms not exact).

This is a **22.7 % cut from the 5.276 × 10¹³ former headline** and **21.1 % from the 5.167 × 10¹³ former second
test size** — a larger cut than CAP17's 4.452 × 10¹³ answer (15.6 %/13.8 %), in exchange for 7.71 GB of margin
(bar a) instead of CAP17's razor-thin 0.53 GB (against its looser 363 GB bar). Raise the target later (closer to
CAP17's 4.452 × 10¹³, or back to the 5.276 × 10¹³ headline with `ECALC_PLANE_CAP=2^30`, or to the full headline
outright) if the memory configuration — the 373 GB/node `hipMalloc` edge, or the B7 v-exchange-slot allowance — is
lifted or found larger than assumed.

#### 5. Modelled walls (`estimate.py`, archive:results/EST17.md's flag set)

Command (per-node D = 4.08 × 10¹³ / 576 = 70,833,333,333.33): `DM_MN_LEAN=1 MN_OUT_DKM_HI=1
MN_MODEL_MAP_RATE=0.070 python3 estimate.py --g 576 --D 70833333333.33 --np-mn auto --lat 8.5e-6 --hide-pow2 0.72
--t-round 0.015 --bw <BW> [--local-factor 1.22]`. The target fabric profile's default `write_bw` is already 1.0
GB/s/node (matching EST17's "write" column; confirmed by varying `--write-bw` explicitly: 0.6 GB/s gives a visibly
different write column, 1.0 GB/s matches the no-flag default).

| case (`--bw`) | ROCm 7.2.4 no-write / write | ROCm 7.0.3 (`--local-factor 1.22`) no-write / write |
|---|---|---|
| (i) 4 NICs, aac7-measured efficiency (`--bw 11`, measured aac7 / assumed on the target) | 606.7 / 602.4 s | 642.3 / 638.0 s |
| (ii) 8 NICs, same efficiency (`--bw 47`, assumed) | **282.4 / 282.9 s** | 318.0 / 316.0 s |
| (iii) line rate (`--bw 100`, assumed, old) | 229.6 / 236.6 s | 265.4 / 270.0 s |

All **modelled**, labels as archive:results/EST17.md §6 (bw 11 measured-aac7/assumed-at-576, bw 47 and bw 100 assumed). Node memory
printed by `estimate.py` at this size: dev ≈ 348 GB, host ≈ 44 GB, pool ≈ 10 GB, node ≈ 386 GB (model; within ~0.2 %
of the C-measured 345.29 / 387.37 GB above — the usual model/C gap at the default plane cap). Every case is
**faster than the former 5.276 × 10¹³ headline at the same `--bw`** (archive:results/EST17.md §3: case i 750.1/744.6 s, case ii
352.6/360.6 s, case iii 288.0/302.7 s at 7.2.4) — fewer digits costs no time at all, it saves it.

#### 6. Open items

- **`TARGET_BELOW` (mem_model.py, 4.74 × 10¹³) was not touched.** It is now *above* the new target, not below it,
  so `estimate.py --target`'s "one step below" row is stale until a fresh lower-tier sweep picks a real value (a
  finer sweep below 4.08 × 10¹³ found tiers at 3.5/3.6/3.7/3.8/3.9 × 10¹³ with arena 201.86/206.16/206.16/210.45/
  214.75 GB — no single clean "one step below" was chosen here, out of this task's scope).
- **`ecalc/e16_headline.sh` was reviewed, not changed.** Its `E16_DIGITS` default (`G * 91600000000`) is a
  separately *measured* 10/12-node scaled test slice tied to specific plan-check figures (102/98 products, pool
  size, grid schedule) at the former 5.276 × 10¹³ target's per-node share — not the 576-node target digit count
  itself. A proportional scale-down (≈ 7.08 × 10¹⁰) is arithmetically trivial but would need a fresh `MN_PLAN_ONLY`
  sweep at g = 10/12 to re-measure the plan-check figures the script's comments assert; left for whoever next runs
  Phase 16 E at the new target.
- Bound (b) (node total + 30 ≤ 480) was never binding in this sweep — the device bound (a) decided the answer at
  every size tested, consistent with CAP17.

#### 7. Labels

measured: all `BS_LAYOUT_ONLY`/`MN_PLAN_ONLY` figures (§3, §4), the `hipMalloc` edge (A6), the VMM-shares-the-edge
finding (OFI17/RESULTS §107), the aac7 comm_ofi efficiency (EST17 §1).
modelled: every wall and node-GB figure from `estimate.py`/`mn_model.py`/`mem_model.py` arithmetic (§5), the
`mem_model.py --check-c` exactness (arithmetic identity, not a new measurement).
assumed: the 30 GB B7 allowance (bar b; STD17 estimated 15–30 GB at 576 from a 10-node ≈13 GB measurement), the
8/4-NIC injection bandwidths (bw 47/11) and line rate (bw 100), the write rate holding at 576 writers, the
ROCm 7.0.3 local-factor 1.22 (fitted at 10¹¹ on aac7).

## TGTBENCH2 — the user's second set of target tests, assessed against the ecalc design (2026-10-06, branch `tgtbench2`)

(former file: TGTBENCH2.md)

Input: `archive:results/TGTBENCH2_raw.md`, received about 18:40 EDT and kept verbatim. The tests are the user's, run **on the target** (576-node
MI300A, 8 Cassini NICs per node) with a generic benchmark harness, **not ecalc**. Times are Eastern.

Labels:
- **(m, target)**: measured on the target by the user.
- **(m, aac7)** / **(m, aac6)**: measured by us.
- **(m, layout)**: the C binary's own `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` print, on the aac7 login node, with no HIP calls.
- **(mod)**: modelled.
- **(a)**: assumed.

Base: `origin/b7-vslot` c058964 (B7ACCT's v-slot accounting, not merged). No compute-node work, no jobs, no network tests. Login-node
work was in `aac7:~/b7acct` (build aa86cac = b7-vslot's code); outputs are in `~/b7acct/tgtb2/` and the script is `~/b7acct/tgtb2.sh`.

#### RESUME (2026-10-06 19:00 EDT): complete

- Branch `tgtbench2`:
  - the model profile, the estimate driver and the raw file (cdc7547);
  - this report and the docs (the commits after it).
- Pushed; not merged. Nothing is armed and nothing is running.
- Everything here is a proposal. Nothing below changes runtime behavior, the launch line or `TARGET_DIGITS`.

#### 0. Summary

1. **The device edge is confirmed exactly** (A6: 93.36 GB/APU = **373.44 GB/node**, n = 5, 0 % spread, m target). CAP17/TGT17/B7ACCT already
   used this figure. With B7's v-slots, the current target 4.08 × 10¹³ has a C-layout device of **372.12 GB (m, layout)**. That is **1.32 GB
   under the edge**, before the ≈ 0.6 GB/APU that B7ACCT could not explain (inferred).
2. **M3's "421.5 GB, 26 % oversubscribed, hipMallocManaged host spill" is stale and wrong in three ways, and the spill is rejected:**
   - 421.5 GB is the old 5.276 × 10¹³ layout (06 A2: planes 120.9 + arena 300.7, from before `DM_MN_LEAN` and the 4.08 × 10¹³ target).
   - Its arithmetic does not close: 421.5 × 576 = 242.8 TB, not 265.4 TB; 373.44 × 576 = 215.1 TB, not 210.1 TB. The real
     oversubscription of that stale layout is 12.9 %, not 26 %.
   - Managed or host-backed memory on this APU was measured at **21 GB/s for `hipMemcpy` against 3.2–3.8 TB/s from HBM**, and 0.114 s/GB
     to map (ME10, m aac6).
3. **M1/V4's "251.5 s, communication 1.0 s at 25 GB/s" contradicts the volumes.**
   - Our model moves **18.6 TB per node through the NICs** at 4.08 × 10¹³ (mod). That is 2.3 TB per NIC; aac7 measured 2.47–2.63 TB per
     NIC at 10 nodes (m, cxi counters, STD17).
   - At 25 GB/s per APU (100 GB/s per node) the wire time alone is **186 s**, not 1.0 s.
   - The model's wall at 25 GB/s is **369.6 s** (mod), with 199.6 s of exposed communication.
   - Their "compute 250 s" comes from a 64 K-point NTT microbenchmark, not from ecalc's kernels or runs.
   - **A3 (fabric injection) and A4 (64-node scaling) are still unmeasured. They remain the top uncertainty.**
4. **MAP_RATE 0.010 s/GB (m, target)** is now a labelled `target-m` model profile; the aac7 constants are unchanged.
   - Init falls by 1.0 s, but the seed wait grows by 1.0 s, because the CPU seeds bind. **The wall is unchanged** (§3).
5. **Proposed target size: 3.76 × 10¹³ digits** (§4).
   - Device 367.82 GB (m, layout): ≤ 368 GB and 5.62 GB under the edge.
   - Node 383.08 GB, or 409.91 GB with the v-slots.
   - Pieces: 105 on node 0, 107 on the critical path.
   - Wall **222.7 s** at `--bw 47` (mod), against 282.4 s for 4.08 × 10¹³.
   - `TARGET_DIGITS` is not changed; the user decides.
6. **The launch-line findings (L1/L2/D1) are consistent with our launch line**, which is already Slurm-native `srun --ntasks`, one PE per
   node. They resolve WISHLIST §0.1 **for on-node multi-PE launches only**. Two proposals for the line (§2, L2):
   - `FI_LOG_LEVEL=warn`;
   - `FI_UNIVERSE_SIZE=4096`, with `SHMEM_SYMMETRIC_SIZE` set to our heap so a site default of 512M cannot shadow it.

#### 1. Item by item

Columns: the user's value | what ecalc assumes today (where) | consistent? | implication | action.

##### M2 — "updated constants"

| constant | user's value | ecalc today | consistent? | action |
|---|---|---|---|---|
| DEVICE_MEM(_PER_APU) | 93.36 GB/APU (m, target; n = 5, 0 % spread) "(−21 % vs 117.975 GB assumption)" | 93.36 GB/APU = 373 GB/node from newbench1 A6, used by CAP17/TGT17 (results/TARGET.md §CAP17 §1; TGT17 §1 bar (a)) and B7ACCT §5. **No ecalc record assumes 117.975 GB** (grep: no hit) | **yes**, same number, now n = 5 | **Adopt** as `mn_model.TARGET_DEVICE_EDGE_GB = 373.44` (m, target), used by `--fabric target-m`'s edge line. The "−21 %" refers to someone else's assumption |
| VMM_MAP_RATE | 0.010 s/GB (m, target) "7× faster than 0.070, init 6.9 → 1.0 s" | `MAP_RATE` 0.065 default, 0.070 aac7 (`mn_model.py:805, 814`), measured with `t_alloc` hipMalloc. ecalc's own VMM pools map at 0.043–0.047 s/GB (7.0.3) and 0.028 (7.2.4) (results/TIME_MODEL.md §C16 §2.7, m aac7) | plausible: a faster node or driver. **The unit is not stated** (per APU-GB mapped by one APU, or per node-GB) | **Adopt as a labelled target profile** (`target-m`, §3). The aac7 constants are not changed. The "6.9 → 1.0 s" is 0.070 or 0.010 × 98.6 GB of hypothetical host spill. It is not an ecalc quantity, and ecalc's init is not that product (§3) |
| HOST_MEM_PER_APU | 256 GB hipHostMalloc limit (m, target) | ecalc pins only the seed buffers (2 × 8 GiB/node, `as_seedbuf`) and the comm_ofi `host` pool form (off by default) | no conflict | Record as `TARGET_HOST_PIN_APU_GB = 256.0` (informational) |
| PROCESS_OVERHEAD_1PE | 28.0 GB (m, target) "was 86.0 GB modelled" | **86 GB** is ecalc's modelled fixed cost per *ecalc engine process* at the target: arena floor, tables and init host (P16 §3; 06 §3.2 L119; 05 PS14; 03 §7 L514). WISHLIST §2.2 already logged 28 GB as "measured, not by ecalc" | **not comparable**: different program and different quantity | **Question for the user** (Q4): is 28 GB host RSS, device use, or both, and of which process? It does not change the design (one process per node; 86 GB was the argument against 4 PEs/node, which is rejected anyway) |
| NTT_THROUGHPUT_PER_APU | 655.21 GB/s (n = 65 536, n = 20 runs) | ecalc's NTT at 2³¹: 117 ms per transform (01 §8, RESULTS §33). About 1.2 TB/s of HBM traffic (inferred: 4 passes × read+write × 17.2 GB). Modmul 1.31–1.34 Top/s per APU (m) | **not the regime**: a 512 KiB plane is L2-resident; ecalc's planes are 4–17 GB | **Reject as a model input** (C3 below) |
| TRANSPOSE_THROUGHPUT_PER_APU | 2601.54 GB/s (n = 4096) | ecalc's packs are measured inside the four-step (01, 03) | not the regime | **Reject** |
| INJECTION_BW_PER_APU | 25 GB/s "(conservative, A3 unmeasured)" | `--bw` cases: 11 (m aac7 efficiency, 4 NICs), 47 (a: 8 NICs at the aac7 efficiency), 100 (a: line rate) (EST17 §1) | it is an assumption, not a measurement | No adoption. The 25 GB/s row is in §3 (369.6 s) |
| XGMI_EFFICIENCY | 1.0 (all direct links) | K4 mesh; our push reaches 93–97 % of the links (m, aac6/aac7; 06 §3.2) | **yes** | none |

##### M3 / D3 / Issue B1 — "memory constraint, 26 % oversubscribed, mitigate with hipMallocManaged host spill"

- **The 421.5 GB layout is stale.** It is 06_EVALUATION A2's figure (planes 120.9 + arena 300.7) for the 5.276 × 10¹³ headline at
  `DM_MN_LEAN=0`, before OFIMEM. CAP17 measured 405.42 GB for that size on today's launch line, and TGT17 moved the target to 4.08 × 10¹³.
- **The arithmetic does not close:**
  - 421.5 × 576 = **242.8 TB**, not 265.4. 93.36 × 4 × 576 = **215.1 TB**, not 210.1.
  - The ratio is 1.129, so the stale layout is **12.9 %** over, not 26 %.
  - 265.4 / 576 = 460.8 GB/node matches no layout in our records.
  - "Init 0.5 s" (V4) and "1.0 s" (M3) contradict each other for the same 98.6 GB.
- **Today's layout (m, layout; this branch's build, the TGT17 line + `COMM_OFI_PLAN_CXI=1`) at 4.08 × 10¹³:**
  - planes 120.88 + arena 214.75 + comm_ofi pools 9.66 = 345.29 GB;
  - plus the v-slots, 26.83 GB → **372.12 GB**, which is ≤ 373.44 by 1.32 GB;
  - node 387.37 GB, or 414.20 with the v-slots (B7ACCT §5).
  - There is no oversubscription, but there is also no margin. §4 sizes below it.
- **Managed or host spill is rejected (proposal), with numbers:**
  - (1) On MI300A, "host" memory is the same HBM, but host-backed pages are mapped and accessed through the slow path:
    - `hipMemcpy` from host-backed memory **21 GB/s**;
    - managed mapping **0.114 s/GB**;
    - against HBM at 3.2–3.8 TB/s and `hipMalloc` mapping at 0.057–0.072 s/GB (ME10, m aac6; 05 ME10; 01 §8 "3.8 TB/s").
    - The spilled data would be planes or arena that every product pass touches, so ≈ 150× less bandwidth on that fraction.
    - At 26 % spilled (their figure), the local passes' memory time would grow by far more than the whole run. The 1.0 s claim prices only
      the mapping, not the use.
  - (2) It is a design change across `dbig.c` (VMM arena), the plane pools and `RNS_PLANES_FIRST`. The VMM arena cannot be registered with
    the NIC either (07 §4).
  - (3) It is unnecessary: 4.08 × 10¹³ fits, and §4's proposal leaves 5.6 GB.
  - Cheaper levers already exist, measured on the login node:
    - B7ACCT §5: free each level's v-slots after the level, `DIST_CHUNKS=8`, or both, which save 11.5–19.5 GB of device (mod);
    - CAP17: `ECALC_PLANE_CAP=2^30` (−60 GB, about +80 % wall, mod);
    - a lower size (§4).
- **Action:** reject, with the reason above. Record it in 05 as proposed. No "1–2 weeks of implementation".

##### Smoke tests, regression detection, V3 data integrity

- **Smoke 3/3 and regression 21/28:** these are the harness's own self-consistency checks against its own earlier runs. They contain no
  ecalc number.
  - The C1 line itself is inconsistent: its "n = 10 mean (0.801 s)" here vs "μ = 80.801" in C1.
  - Relevance: none to the design.
  - Action: none. Note only that "FAIL: B1 4PE anomaly" concerns multi-PE process memory, which ecalc does not use.
- **V3 (309 CSV files parse):** harness hygiene. No action.

##### V4 / M1 — "576-node runtime 251.5 s ± 3.6 %; compute 250.0 s, communication 1.0 s at 25 GB/s, init 0.5 s"

- **Our model** (`estimate.py`, EST17/TGT17 flag set, 4.08 × 10¹³, ROCm 7.2.4, all mod):

| `--bw` per APU | wall no-write / write | exposed communication | distributed levels + reciprocal + division |
|---|---|---|---|
| 11 (m aac7 efficiency, 4 NICs) | 606.7 / 602.4 s | 436.9 s | 358.4 + 45.5 + 133.6 |
| **25 (their "conservative" value)** | **369.6 / 365.3 s** | **199.6 s** | — |
| 47 (8 NICs at aac7 efficiency, a) | 282.4 / 282.9 s | 112.4 s | 138.8 + 20.9 + 53.4 |
| 100 (line rate, a) | 229.6 / 236.6 s | 59.4 s | 103.3 + 16.7 + 40.4 |

- **The bytes:**
  - The model sends **18.6 TB per node** through the NICs over the run (2.3 TB per NIC over 8). 9.1 TB of it crosses dragonfly global
    links (mod, `--verbose`; it is mostly the 8 tree levels, the reciprocal's 1.4 TB and the division's 4.6 TB).
  - **At 25 GB/s per APU = 100 GB/s per node, the wire time is 186 s.** The 1.0 s communication term implies 18.6 TB/s per node, 186× their
    stated rate.
  - A measured cross-check: at 8.1 × 10¹¹ on 10 aac7 nodes, each NIC carried **2.47–2.63 TB** (m, cxi counters, STD17 §108 B). The model's
    2.3 TB per NIC at the target is the same order.
- **Compute:**
  - Their 250 s is the 64 K-point NTT microbenchmark's rate scaled (C3), not ecalc's work.
  - Our local (non-exposed) part at 4.08 × 10¹³ is ≈ 170 s at `--bw 47` (282.4 − 112.4, mod). That includes init plus the seed wait
    (27.3 s), batch 19.1 s, top 16.1 s, and the local passes of the distributed products, which are calibrated on measured ecalc runs
    (04 §4 V-table).
- **"± 3.6 %", "86× uncertainty reduction", "r > 0.97":** these describe the harness's repeatability, not the model's error against an
  ecalc run at scale. The two inputs that move the answer by more than ±20 % are A3 (injection with comm_ofi on 8 NICs) and A4 (all-to-all
  scaling to 64+ nodes) (04 §5; EST17 §4: `--fall-off 0.3` costs ×1.9 at bw 47). **Neither is measured.**
- **Action:** reject the 251.5 s as an ecalc estimate. Keep the standing estimate's method. A3/A4 stay the top requests (WISHLIST
  §1.3/§1.4).

##### Issue log B1–B3

- **B1 (memory, CRIT):** see M3. It is stale and rejected; the real constraint is handled by §4.
- **B2 (multi-node access, gate):** **agree.** A3, A4, A5, A7, B5, B7, D1 and D4 need ≥ 2 nodes.
  - For ecalc the critical ones are:
    - A3: comm_ofi's per-node rate on 8 NICs, `t_comm --bw` at 2 nodes;
    - A4: `ecalc` 10¹⁰/node with `MN_COMM_MARK=1` at 2–64 nodes;
    - the 2-NICs-per-APU `COMM_OFI_NICS` run.
  - Action: carry into WISHLIST §0.1/§1.3/§1.4.
- **B3 (4-PE anomaly):** 1 PE 28.0, 2 PE 172.9, 4 PE 35.4 GB is not monotone, which suggests a measurement artefact. Relevance to ecalc is
  **none** (one process per node, PS14; the 4-PE form was rejected 2026-10-05). Action: none.

##### D1 / L1 / L2 / L3 / L4 — the `fi_enable(-28)` root cause and the launch environment

- **Root cause:**
  - The launcher. `oshrun`/`mpiexec` is unavailable in their sandbox, and Slurm-native `srun --ntasks` works: 12/12 configurations,
    7 tests unblocked.
  - That fits our records: WISHLIST §0.1 doubted the `cxi_core` explanation, and **our launch line is already Slurm-native**
    (`srun -N 576 --ntasks=576 --ntasks-per-node=1 …`, internal target notes §4). aac7 runs plain `srun` with Cray OpenSHMEMX.
  - Validated so far: 2 PEs on one node and 4 PEs on one node. **No multi-node launch is reported.**
  - So WISHLIST §0.1 is **resolved for on-node multi-PE launches. A 2-node launch is still to be shown**, now blocked by allocation (B2),
    not by `-28`.
- **FI_UNIVERSE_SIZE ≥ 4 × ntasks:** L2 also says "baseline config works perfectly (no special env vars required)", and lists
  `FI_UNIVERSE_SIZE=4` beside a tested range of 512–4096. As written it is a safe setting, not a demonstrated requirement.
  - **How comm_ofi uses libfabric** (`comm_ofi.c`):
    - one fabric/domain/EP/CQ/AV per NIC per device, so 4 × k endpoints per process (k = 2 on the target: 8);
    - `tx_attr->size = 4096` (`:148`), `FI_DELIVERY_COMPLETE` (`:149`);
    - **CQ size set explicitly to 8192** (`:155`);
    - **AV `FI_AV_TABLE` with count 0** (`:157`, the provider's default size);
    - `fi_av_insert` of every member of every communicator, never removed (`:262-282`). That is roughly Σ group sizes over the
      communicators per NIC, a few thousand entries at 576 nodes (inferred).
  - So `FI_UNIVERSE_SIZE` can matter to comm_ofi only through the provider's default AV and receive sizing. It also matters to Cray SHMEM's
    own endpoints.
  - **`FI_CXI_DEFAULT_CQ_SIZE` does not reach comm_ofi's CQs** (sized 8192 explicitly). It reaches only CQs opened with size 0, which means
    Cray SHMEM's. Their tested 65 536–262 144 all passed.
- **Proposal for internal target notes §4** (commented, **not adopted**; the user decides):
  - `FI_UNIVERSE_SIZE=4096`: ≥ 4 × 576 = 2304, and their largest tested value;
  - `FI_LOG_LEVEL=warn`;
  - **`SHMEM_SYMMETRIC_SIZE=2048M`** beside our `SHMEM_SYMMETRIC_HEAP_SIZE`/`XT_SYMMETRIC_HEAP_SIZE`. Their environment used
    `SHMEM_SYMMETRIC_SIZE=512M`, which is below our 1536 MiB pool plus margin. If the site's profile exports it, it could shadow ours.
    `comm_shmem` would then stop with rc 8 only if it reads that variable (TARGET.md §4 "the pool and the heap").
  - None of these changes a digit or a buffer. All three are environment variables for libfabric and SHMEM.
- **100 ms HIP-init stagger per PE:** this is for several PEs per node initialising HIP at once. ecalc has one PE per node and already
  initialises SHMEM before its HIP threads (`COMM_INIT_EARLY=1`, the default; 00 §1.3 phase 1). It does not apply. Action: none.
- **SHMEM_SYMMETRIC_SIZE=512M** (L2): see the proposal above.

##### A6 — memory edge extended

- Device 93.36 GB/APU (0 % spread): **adopted** (M2).
- Host 256 GB/APU: recorded.
- VMM 0.010 s/GB: profile (§3).
- Process overhead 28 GB: Q4.
- "265.4 vs 210.1 TB": rejected (M3).

##### A8 / D2 / D6 / B8 — xGMI: SDMA 50–58 GB/s per directed pair, uniform; SHMEM intra-node 1.8–38 GB/s; a fully connected mesh

- **Topology (D6/B8):** consistent with our K4 (00 glossary). No action.
- **SHMEM intra-node:** **ecalc never sends intra-node bytes through SHMEM.**
  - One process per node drives the four APUs. The on-node quarter of every layered exchange is an in-process xGMI push
    (`comm_layered.c`) and "never touches a NIC" (00 glossary).
  - SHMEM PEs are one per node, so every SHMEM or comm_ofi byte is inter-node.
  - The 21× asymmetry and the noted "HIP implementation bug (measuring local copy not xGMI)" concern their harness. Relevance: none.
- **SDMA vs our push kernels:**
  - Our push: **909–1014 GB/s per node, 244 GB/s per APU, 93–97 % of the links** (m, aac6/aac7; 06 §3.2).
  - The model's xGMI stage is 0.0571 s per 3 transforms of 2²⁹ points per APU (`mn_model.py:63`), ≈ 226 GB/s per APU (mod).
  - SDMA at 50–58 GB/s per directed pair gives at most 3 × 58 = 174 GB/s per APU, even if the three peers' copy engines all run
    concurrently. That is **below our push**.
  - The xGMI stage is also already 72–76 % hidden under the fabric stage (`HIDE_POW2`, m), and the run is fabric-bound (exposed fabric
    112 s at bw 47).
  - The "3× throughput gain" compares SDMA with their SHMEM path, which ecalc does not use.
  - One benefit that is not quantified: copy engines do not occupy CUs. But the xGMI stage is mostly hidden, so the ceiling is a few seconds
    (inferred).
  - **No aac7 test is proposed**: it could not change the design.

##### B2 (detailed) — device-heap GPU-direct vs host-staged: +62 % at 16 MB, −25 % at ≥ 64 MB

- **The SHMEM path:** `COMM_SHMEM_DEVHEAP=1` already falls back to the HIP-registered host pool on Cray, because libsma has no HIP heap
  (05 TR15). Under comm_ofi, which is now the default on cxi, the SHMEM pool carries only control and host operations (1536 MiB, OFIMEM).
  B2 does not apply to the SHMEM path.
- **comm_ofi:**
  - It writes 4 MiB chunks (`COMM_OFI_CHUNK_MB`) from a **fine-grained device pool** registered with `FI_HMEM_ROCR`.
  - A `host` form exists: `COMM_OFI_POOL=host`, NUMA-local host memory + `hipHostRegister` (07 §4). NIC16 measured −6 % for it off-NUMA
    on aac7.
  - B2's crossover is about message size. comm_ofi's messages are 4 MiB, in the range where B2 found device-direct ahead.
  - **Action:** no change. On the target, `t_comm --bw` with `COMM_OFI_POOL=fine` vs `host` at 2 nodes settles it with existing switches
    (WISHLIST §1.3 (e)).

##### B6 / B9 / D5 / B4 — barriers, collectives, atomics, synchronization

- Barriers: 2 PE 3.89 µs, 4 PE 5.67 µs (intra-node).
- Collectives: alltoall, allgather and allreduce scaling.
- CAS contention.
- **Relevance: low.**
  - ecalc's barriers and small collectives are inter-node: one PE per node, Cray SHMEM, `mn_model` `coll()` with `--lat`.
  - The bulk all-to-alls are ecalc's own puts, not `shmem_alltoall`.
  - Signals are put words, not CAS.
- B4 (1-PE synchronization): no data.
- Action: none. Inter-node latency at 64–576 PEs stays WISHLIST §1.5/§1.7.

##### C1 / D3 — single-node baseline: n = 10, μ = 80.801 at 10¹⁰ digits, CV 5.0 %; "validates local-factor 1.22 for ROCm 7.0.3 vs 7.2.4"

- **It is not ecalc.** ecalc at 10¹⁰ on one aac7 node:
  - **25.98–27.71 s at ROCm 7.0.3, 26.18–26.27 s at 7.2.4** (m, aac7, results/TIME_MODEL.md §C16 §2.7);
  - modelled 23.9 s (04 V7).
  - 80.801 s would be 3.0–3.4× slower than ecalc measured. The harness's own smoke line reports the same test as "0.801 s", 30× faster.
  - WISHLIST §3.1 already logged "0.801 s at 10¹⁰" as not ecalc.
- **The 1.22 claim contradicts our records.**
  - 1.22 is the 7.0.3 / 7.2.4 ratio **at 10¹¹** (205.9 vs 168.5 s, m aac7, C16; TC3).
  - **At 10¹⁰ the two toolchains measured the same** (26–28 s both, C16 §2.7).
  - One toolchain's 10¹⁰ run on the target cannot validate a ratio between two ROCm versions.
- **Per-APU spread** 14.3 → 5.8 % (D3): useful for the node-speed tail (WISHLIST §3.1, Q17), if it is per-APU compute. Record it as
  harness-measured.
- **Action:** question for the user (Q3): which program, configuration and ROCm? If it was ecalc, at what size and launch line? No change
  to `--local-factor`.

##### C2 — sustained thermal: 300 s, +1 °C, no throttling, clocks stable

- This **supports a ≈ 4–5 min run** (3.76 × 10¹³: 222.7 s at bw 47; 4.08 × 10¹³: 282.4 s; mod) on one node.
- Limits:
  - The load was the NTT/transpose microbenchmark, not ecalc's mixed HBM + xGMI + NIC load.
  - One node, not 576 (facility power).
- **Action:** update WISHLIST §3.3 to PARTIAL with data. Still wanted: the same during `ecalc 100000000000`.

##### C3 / C3a / D5 (bimodal) — NTT 655 GB/s at n = 65 536, transpose 2602 GB/s at 4096, bimodal 31.8 / 15.3 GB/s at 4096

- These sizes are 512 KiB and 32 KiB. They are cache- and launch-bound, three to five orders of magnitude below ecalc's transforms
  (2²⁹–2³¹ points per plane, 4–17 GB; 01 §2).
- ecalc's rates are measured on its own kernels:
  - 2³¹ in 117 ms (RESULTS §33);
  - modmul 1.31–1.34 Top/s per APU (RESULTS §19);
  - the pieces' local passes, calibrated on full runs.
- The "1.51 PB/s at 576" is 655 GB/s × 2304, and says nothing about ecalc.
- **Action: reject as model inputs.** The bimodality at 4096 (L1-capacity) does not concern ecalc's batch tier either: its products are
  ≥ 2¹⁰-point b1 passes inside larger planes.

##### M1 — see V4.

#### 2. What changes in the records

- **Adopted as model constants** (labelled, behind `--fabric target-m`; the default `target` profile is byte-identical):
  - `TARGET_DEVICE_EDGE_GB = 373.44` (m, target);
  - `TARGET_M_CONSTS = {MAP_RATE: 0.010}` (m, target);
  - `TARGET_HOST_PIN_APU_GB = 256` (m, target, informational).
- **Docs:** internal target wishlist (§0.1 status, §2.1, §2.2, §3.1, §3.3, and a TGTBENCH2 header line), internal target notes §4 (the proposal
  block), internal code notes (dated note), internal code notes (rows marked PROPOSED), ecalc/README.md and 04
  (the profile row).
- **Rejected, with reasons:**
  - M3's host spill;
  - V4/M1's 251.5 s and its 1.0 s communication term;
  - C3's kernel rates as model inputs;
  - the claim that C1 validates the 1.22 factor;
  - SDMA for xGMI.

#### 3. The 576-node estimate with the target profile (4.08 × 10¹³, `archive:drivers/ecalc/tgtb2_est.sh old-new`, all mod)

Old = TGT17's flag set: `--fabric target`, `MN_MODEL_MAP_RATE=0.070`. New = `--fabric target-m` (MAP_RATE 0.010). Both use
`DM_MN_LEAN=1 MN_OUT_DKM_HI=1 COMM_OFI=1 --g 576 --D 70833333333.33 --np-mn auto --lat 8.5e-6 --hide-pow2 0.72 --t-round 0.015`.

| ROCm | `--bw` | old: no-write / write; init + seed wait | new: no-write / write; init + seed wait |
|---|---|---|---|
| 7.2.4 | 11 | 606.7 / 602.4; 17.7 + 9.6 | 606.7 / 602.4; **16.7 + 10.6** |
| 7.2.4 | 47 | 282.4 / 282.9; 17.7 + 9.6 | 282.4 / 282.9; **16.7 + 10.6** |
| 7.2.4 | 100 | 229.6 / 236.6; 17.7 + 9.6 | 229.6 / 236.6; **16.7 + 10.6** |
| 7.0.3 (`--local-factor 1.22`) | 11 | 642.3 / 638.0; 21.6 + 11.7 | 642.3 / 638.0; **20.4 + 12.9** |
| 7.0.3 | 47 | 318.0 / 316.0; 21.6 + 11.7 | 318.0 / 316.0; **20.4 + 12.9** |
| 7.0.3 | 100 | 265.4 / 270.0; 21.6 + 11.7 | 265.4 / 270.0; **20.4 + 12.9** |

- The old rows reproduce TGT17 §5 exactly.
- **The init change is −1.0 s (7.2.4) and −1.2 s (7.0.3). The wall change is 0.0 s.**
- Why the wall does not move:
  - The model's init is a measured reference init plus MAP_RATE × the device's difference from that reference (`mn_model.py:1387`). It is
    not MAP_RATE × all bytes, so the harness's "6.9 → 1.0 s" is not the model's quantity.
  - bs waits for the CPU seeds, which end at 7.589 + 0.2681 × D_leaf/10⁹ ≈ 27.3 s (`SEED15B`, `mn_model.py:1607, 1685`; fitted on ten runs).
  - init + seed wait = max(init, seeds) = 27.3 s either way.
- A faster mapping helps only once the seeds stop binding (05 Q18). The device term is only ≈ 1.2 s at MAP_RATE 0.070 (0.070 s/GB × ≈ 17 GB
  of device above the reference). Even at MAP_RATE = 0 the model's init would fall only to ≈ 16.5 s; the rest is the measured reference
  init (HIP, SHMEM, pools). The seeds still end at 27.3 s.
- The modelled device under `target-m` is 370.5 GB (mem_model, with the v-slots), +2.9 GB to the edge. The C layout's 372.12 GB is the
  sizing figure.

#### 4. Target size (the user, 18:30 EDT: "use what is fast and relatively close to the limit")

##### The method

- `~/b7acct/tgtb2.sh` runs `MN_PLAN_ONLY=<T>:576` and `BS_LAYOUT_ONLY=<T/576>:576 ./ecalc <T> /dev/null` with TGT17's launch-line
  environment plus `COMM_OFI_PLAN_CXI=1` (trap 20).
- The build is b7-vslot (aa86cac).
- device = the `vslot:` line's `device_with` = planes + arena_with_room + ofi_pool + 4 × v-slots per APU.
- Swept from 3.0 to 4.1 × 10¹³ in 5 × 10¹¹ steps, then 10¹¹, then 10¹⁰ around every step. All are (m, layout).
- `plan check` was OK everywhere (1236 products below 4.0 × 10¹³, 1238 from 4.0).
- `mem_model.py --check-c` on the 3.76 × 10¹³ layout: **exact** (0 terms not exact; vslot, device + vslots and node + vslots to the byte).

##### Layout tiers

v-slots are 6.707 GB/APU = 26.83 GB/node everywhere in this range.

| T (digits) | arena | device (≤ 368?) | node / with v-slots | pieces node 0 / critical path (tree + recip + div) |
|---|---|---|---|---|
| 3.0–3.15 × 10¹³ | 193.27 | 350.64 | 365.90 / 392.73 | 82–86 / 98 |
| 3.2–3.3 × 10¹³ | 197.57 | 354.94 | 370.20 / 397.02 | 88–90 / 100 |
| 3.35–3.5 × 10¹³ | 201.86 | 359.23 | 374.49 / 401.32 | 92–94 / 100–102 |
| 3.55 – **3.7104** × 10¹³ | 206.16 | 363.53 (yes, 9.91 under the edge) | 378.79 / 405.61 | 101–103 / **107** |
| **3.7105 – 3.896 × 10¹³** | **210.45** | **367.82 (yes, the largest tier ≤ 368; 5.62 under the edge)** | 383.08 / 409.91 | 103–111 / 107 → 109 → 111–127 |
| 3.897 – 4.0815 × 10¹³ (today's target 4.08) | 214.75 | 372.12 (no; 1.32 under the edge) | 387.37 / 414.20 | 111–119 / 127–133 |

##### Critical-path piece steps inside the 367.82 GB tier

| range | critical path (tree + recip + div) | change |
|---|---|---|
| 3.7105 – 3.769 × 10¹³ | **107** (42 + 49 + 16) | — |
| 3.770 – 3.8209 × 10¹³ | 109 (42 + 49 + 18) | the division 16 → 18 |
| 3.8210 – 3.896 × 10¹³ | 111 → 127 | the tree level-8 group 42 → 60 |

##### Modelled walls (`archive:drivers/ecalc/tgtb2_est.sh sizes`, `--fabric target-m`, ROCm 7.2.4, no-write / write @ 1.0 GB/s, mod)

| T | device tier | critical path | bw 11 | **bw 47** | bw 100 | NIC TB/node |
|---|---|---|---|---|---|---|
| 3.71 × 10¹³ | 363.53 | 107 | 460.6 / 456.7 | 221.9 / 225.1 | 183.0 / 191.4 | — |
| **3.76 × 10¹³** | **367.82** | **107** | 462.0 / 458.1 | **222.7 / 226.1** | 183.7 / 192.4 | 13.8 (at bw 25) |
| 3.769 × 10¹³ | 367.82 | 107 | 462.4 / 458.4 | 222.9 / 226.4 | 183.9 / 192.7 | — |
| 3.77 × 10¹³ | 367.82 | 109 | 474.6 / 470.6 | 227.8 / 228.8 | 187.6 / 194.5 | — |
| 3.82 × 10¹³ | 367.82 | 109 | 476.9 / 472.9 | 229.2 / 230.6 | 188.9 / 196.1 | — |
| 3.89 × 10¹³ | 367.82 | 127 | 576.4 / 572.3 | 268.5 / 270.2 | 218.4 / 226.0 | — |
| 3.99 × 10¹³ | 372.12 | 131 | 591.5 / 587.3 | 275.6 / 275.5 | 224.2 / 230.6 | — |
| 4.08 × 10¹³ (today) | 372.12 | 133 | 606.7 / 602.4 | 282.4 / 282.9 | 229.6 / 236.6 | 18.6 |

##### The proposal (the user decides): `ecalc 37600000000000`

- **3.76 × 10¹³** is in the **largest layout tier ≤ 368 GB** (367.82 GB), at the **low piece tier** (critical path 107 = 42 + 49 + 16).
  It is 9 × 10¹⁰ digits (0.24 %) below the 107 → 109 step at 3.769 → 3.770 × 10¹³.
- **Device 367.82 GB** (m, layout): 0.18 GB under the 368 bar and **5.62 GB under the 373.44 edge**.
- If B7ACCT's unexplained ≈ 0.6 GB/APU (inferred) also sits on the device, ≈ 3.2 GB remains.
- **Node 383.08 GB; 409.91 with the v-slots** (480 budget: +70 GB).
- **Pieces 105 / 107. 1236 products, `plan check` OK.**
  - Largest piece: 1,462,222,222,224 limbs (DKM step 1 X_hi Q mod B^w, g 576), 105 of 143 pieces bound per product.
  - `plan pool`: `COMM_SHMEM_POOL_MB=1536`, heap ≥ 2048 MiB, 4 × 2304 MiB comm_ofi pools, the same as today's line.
- **Modelled wall 222.7 s / 226.1 s at bw 47** (7.2.4), against 282.4 / 282.9 s at 4.08 × 10¹³ (**−21 %** for −7.8 % digits).
  - 462.0 s at bw 11 (−24 %); 183.7 s at bw 100.
  - 13.8 TB per node through the NICs instead of 18.6 (−26 %) at the same `--bw`.
- **Next tier down: 3.71 × 10¹³.**
  - Device 363.53 GB, 9.91 GB under the edge.
  - Node 378.79 / 405.61 GB.
  - Pieces 103 / 107, the same critical path.
  - 221.9 / 225.1 s at bw 47.
  - It buys 4.29 GB more margin for 1.3 % fewer digits at −0.8 s. **That is the alternative if the user wants margin over digits** (Q1).
- **Next tier up:** 3.897–4.0815 × 10¹³ at 372.12 GB **fails the 368 bar**.
  - 3.99 × 10¹³ (131 critical) is 275.6 s; 4.08 × 10¹³ (133) is 282.4 s.
  - Within the 367.82 tier, the 109-piece sizes (3.77–3.82 × 10¹³) cost +5–6.5 s for +0.3–1.6 % digits. Above 3.821 × 10¹³ the
    critical path climbs to 127 pieces (268.5 s at 3.89 × 10¹³).
- **To adopt, the coordinator changes:**
  - `mem_model.TARGET_DIGITS` → 3.76e13 and `TARGET_BELOW` → 3.71e13 (or 3.76e13's own step below);
  - the launch line's `ecalc 37600000000000`;
  - `e16_headline.sh`'s per-node share (3.76 × 10¹³ / 576 = 6.5278 × 10¹⁰);
  - the TARGET.md §1 paragraph.
  - Not done here.

#### 5. Questions and decisions for the user

1. **Target size:** 3.76 × 10¹³ (device 367.82 GB, 5.62 under the edge, 222.7 s mod) or 3.71 × 10¹³ (363.53 GB, 9.91 under, 221.9 s)? Both
   replace 4.08 × 10¹³ (372.12 GB, 1.32 under the edge, 282.4 s).
2. **Launch-line environment** (proposal in internal target notes §4): add `FI_UNIVERSE_SIZE=4096`, `FI_LOG_LEVEL=warn` and
   `SHMEM_SYMMETRIC_SIZE=2048M`?
3. **C1:** which program, configuration and ROCm gave μ = 80.801 at 10¹⁰ (the smoke line says 0.801 s)? ecalc measured 26–28 s at 10¹⁰ on
   aac7 under both 7.0.3 and 7.2.4. Was a 7.2.4 run done on the target to support the "1.22" claim?
4. **28 GB process overhead (B1/D2):** host RSS, device use, or both, and of which process at which PE count (2 PE 172.9 > 4 PE 35.4 looks
   like an artefact)?
5. **VMM_MAP_RATE 0.010 s/GB:** per APU-GB mapped by one APU, or per node-GB? It does not change the wall today (the seeds bind), but it
   decides whether Q18 (shorter init) is worth anything.
6. **Next target session:** the two runs that decide the wall.
   - A3: `t_comm --bw` with comm_ofi on 2 nodes, `COMM_OFI_NICS="0,4;1,5;2,6;3,7"` vs 1 NIC per APU.
   - A4: `ecalc` at 10¹⁰/node with `MN_COMM_MARK=1` on 2, 8 and 64 nodes.
   - Both need the multi-node allocation (issue B2).

## CAP17 — the device-memory edge at the target (373 GB/node) against the layout (2026-10-06)

(former file: CAP17.md)

Login-node and local work only (aac7 `aac7-login`, no GPU, no compute jobs), on `main` at `8e03ba0` (rebased; `comm_ofi`
default on cxi; the launch line of internal target notes §4, `DM_MN_LEAN=1`). Build: fresh clone `~/ntt-cap17` on aac7,
`source ecalc/aac7env.sh && make -s -j16` (rocm/7.2.4, the aac7 default toolchain; the build is login-node-only
sizing, the toolchain used does not affect the layout). Every number below is **measured** (the C binary's own
`BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` print, on the login node, no HIP calls) unless marked **modelled** or **assumed**.

#### 1. Background

- A6 (**measured**, target): `hipMalloc` stops at **93.36 GB/APU = 373 GB/node**.
- OFI17 / RESULTS §107 (**measured**, aac7 SPX, job 12287): VMM (`hipMemCreate`) edge 444.0 GB vs `hipMalloc` dev
  edge ≈ 448.0 GB per node — within one 4 GB step, so **VMM shares the device cap**.
- So the device-located layout — `hipMalloc` planes + VMM arena + the `comm_ofi` device pools (Phase 17 OFIMEM,
  RESULTS §108) — must fit under **373 GB/node minus a margin**. This task uses two margins: **363 GB** (373 − 10)
  and **368 GB** (373 − 5).

#### 2. Method

`BS_LAYOUT_ONLY=<D per node>:576 ./ecalc 1 /dev/null` and `MN_PLAN_ONLY=<D>:576 ./ecalc 1 /dev/null` with the
launch line's environment (internal target notes §4): `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1
ECALC_NP=auto RNS_DIST_CACHE_FIT=1 RNS_DIST_CACHE_PARTIAL=1 MN_OUT_DKM_HI=1 MN_T_CHUNK_MB=1024 DM_MN_LEAN=1
COMM_SHMEM_ROUND_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 MN_TOPO_GROUP=0 ECALC_MEM_GUARD_GB=6`, plus
`COMM_OFI_PLAN_CXI=1` (the login node has no cxi; `mnrun.sh` sets this from the first compute node, per
`comm_ofi.c`'s comment) so the plan and the room line see the `comm_ofi` pools exactly as the compute nodes would.

**Trap found (new; see internal target notes update below): without `COMM_TRANSPORT=shmem` in the environment, `as_room_fits`
silently returns 0 for both the SHMEM pool and the `comm_ofi` pools** (`binsplit.c`'s `as_shmem_pool`: `if (size < 2
|| !(tr && !strcmp(tr, "shmem"))) return 0;`) — the `room:` line still prints `fits 1` and a plausible-looking device
figure, 9.66 GB *short*, with no warning. The first pass of this task's sweep used `BS_LAYOUT_ONLY` alone (no
`COMM_TRANSPORT`) and got exactly this: `ofi_pool 0` throughout. All numbers below are the corrected, full-launch-line
runs; every `device` figure in §3 includes the `comm_ofi` pools.

`device` (the quantity compared to 373/363/368) = `room planes` + `room arena_with_room` + `room ofi_pool` from the
C binary's own `room:` print line (bytes; decimal GB below). This matches the task's definition exactly (planes +
VMM arena + `comm_ofi` device pools) and **excludes** the SHMEM symmetric pool and the host-side terms (seed
buffers, `bs_grow`), which the code's own `room:` line buckets under `host` even though `COMM_SHMEM_DEVHEAP=1` backs
the SHMEM pool with device memory — this is a labelling choice in the C code (budget-checked against 480 GB total
node, not against 373 GB device-only), not a measurement gap; flagged as an open item in §5.

Every `device`/`room` figure below was cross-checked with `python3 mem_model.py --check-c <captured output>`:
**exact (0.0000 %) at every point tested**, including the `comm_ofi` pool term (Phase 17 OFIMEM, merged into `main`
as 9e42e7a, already covers this).

#### 3. The headline sizes' device layout (measured)

At the default `ECALC_PLANE_CAP=2^31`, `ECALC_NP=auto`:

| total digits | planes | arena (+room) | `comm_ofi` pools | **device** | vs 363 GB | vs 368 GB | vs 373 GB (raw edge) |
|---|---|---|---|---|---|---|---|
| 5.276 × 10¹³ (the launch line's headline) | 120.877 GB | 274.878 GB | 9.664 GB | **405.42 GB** | **+42.42** | **+37.42** | +32.42 |
| 5.167 × 10¹³ (the second test size) | 120.877 GB | 274.878 GB | 9.664 GB | **405.42 GB** | **+42.42** | **+37.42** | +32.42 |

Both headline sizes give the **same** device figure (the grid/plane structure is flat across this narrow range) —
and both are **over the raw 373 GB edge itself**, before any margin, by nearly 33 GB. Neither headline size fits
the device-memory edge at all under the launch line's current settings. (`plan check` is `OK` at both, 1240
products each — the digit count is not the blocker, the layout is.)

#### 4. The sweep: the largest digit count that fits

Sweeping `D` downward (same environment, `ECALC_PLANE_CAP=2^31`) finds the grid-structure step where `arena`
drops a tier (the arena is quantized to whole VMM chunks plus `BS_ARENA_ROOM`'s 0.16 × hole, so it steps rather
than scaling smoothly; `comm_ofi_pools` stayed flat at 9.664 GB across the whole range tested, 4.0–5.276 × 10¹³):

| total digits | arena tier | **device** | vs 363 GB | vs 368 GB |
|---|---|---|---|---|
| 4.454 × 10¹³ and up to 4.632 × 10¹³ | 240.518 GB | 371.06 GB | over (+8.06) | over (+3.06) |
| **4.452 × 10¹³ (the boundary, exact)** | **231.928 GB** | **362.47 GB** | **fits (+0.53 margin)** | **fits (+5.53 margin)** |
| 4.1–4.44 × 10¹³ | 231.928 GB | 362.47 GB | fits | fits |
| 4.0 × 10¹³ | 214.748 GB | 345.29 GB | fits (+17.71) | fits (+22.71) |

**The grid step is exact at 4.452 × 10¹³ → 4.454 × 10¹³** (bisected to 0.002 × 10¹³ resolution; `MN_PLAN_ONLY` at
4.452 × 10¹³: `plan check OK -- 1240 products`, largest piece bound "121 of 161 pieces"). `mem_model.py --check-c`
matched exactly at this point (room node 404,554,831,488 bytes C = model, 0.0000 %).

**Answer: the largest total-digit count on 576 nodes whose device layout fits both bars is 4.452 × 10¹³ digits**
(device 362.47 GB; margin 0.53 GB to the 363 GB bar, 5.53 GB to the 368 GB bar, 10.53 GB to the raw 373 GB edge).
This is a cut of **15.6 %** from the 5.276 × 10¹³ headline and **13.8 %** from the 5.167 × 10¹³ second test size.

**The 363 GB margin here is razor-thin (0.53 GB)** — under one VMM chunk (2 GiB) of real-world slop. For a size
actually run on the target, the next tier down, **4.1–4.44 × 10¹³ (device 362.47 GB is the same tier; the next
*lower* tier is ≤ 4.0 × 10¹³ at 345.29 GB, 17.7/22.7 GB of margin)**, is the safer recommendation unless the comm
pool / SHMEM-pool labelling question in §5 is resolved in the code's favor first.

Wall time (modelled, `estimate.py --target`-style single-point run, `--g 576 --D 77291666666.67 --np-mn auto --lat
8.5e-6 --hide-pow2 0.72 --t-round 0.015`): **260.7 s without the write, 279.0 s with it @ 1.0 GB/s** at 4.452 × 10¹³
— *faster* than the headline's 290.2/303.9 s (fewer digits), not slower; the digit cut costs capability, not time.

#### 5. Levers that could raise the fit without a size cut

All **modelled** unless stated; no compute jobs were run (login-node sizing only). Every lever below is an
existing, off-by-default environment switch — no code change, digits stay byte-identical.

##### (a) Host-pinned planes/arena (a redesign) — not recommended, no credible number without new code
Cited at internal code notes A2 and the decision register's **ME10** (internal code notes):
`hipMemcpy` to host-backed memory **measured** collapsing to **21 GB/s**, against MI300A's HBM **measured** at
**3.2 TB/s sustained per APU** (TARGET_HW_REVIEW row 7) — roughly **150× less bandwidth**. The arena (232–275 GB)
and planes (69–121 GB) are read and written on essentially every tree/reciprocal/division pass, not once; a single
extra full pass over the arena alone would cost ≈ 240 GB / 21 GB/s ≈ 11.4 s (**modelled, order-of-magnitude,
assumed one pass**), and the real code touches this memory many times per run, so the realistic cost is minutes,
not seconds. No defensible point estimate exists without actually building the redesign and re-benchmarking —
this is a research item, not a lever to pull before the target run.

##### (b) A smaller plane cap (`ECALC_PLANE_CAP`) — the strongest lever found; keeps the full headline size
Measured device (full launch-line env, including `comm_ofi` pools) at the **headline** 5.276 × 10¹³ / 5.167 × 10¹³:

| `ECALC_PLANE_CAP` | planes | arena | ofi pools | **device** | vs 363 | vs 368 | wall (modelled) |
|---|---|---|---|---|---|---|---|
| `2^31` (current default) | 120.877 | 274.878 | 9.664 | 405.42 | +42.42 | +37.42 | 290.2 s (baseline) |
| `3*2^29` | 90.813 | 266.288 | 9.664 | **366.76** | **−3.76 (fails)** | **+1.24 (barely fits)** | ≈ 496.6 s modelled (+71 %) |
| `2^30` | 69.329 | 266.288 | 9.664 | **345.28** | **+17.72 (fits)** | **+22.72 (fits)** | ≈ 527.3 s modelled (+82 %) |

`plan check` is `OK` at every cap tested, at both headline sizes (1216 products at `2^30`/`3*2^29` vs 1240 at the
default — fewer total products, but far more pieces per the time model's own categorization: 453 vs 159). **With
`ECALC_PLANE_CAP=2^30`, the full 5.276 × 10¹³ (and 5.167 × 10¹³) headline fits both the 363 and 368 GB bars with
17.7–22.7 GB to spare — no digit cut needed at all.** The cost is a modelled **+82 % wall time** (≈ 527 s vs 290 s).
`3*2^29` is a weaker version of the same lever: it clears 368 but not 363, at a smaller (but still large) +71 %
modelled slowdown.

**Caveat:** `estimate.py`'s own `mn_model.memory()` does not reproduce the C-measured device bytes at these two
off-default caps (it printed 380.3 / 358.8 GB against the measured 357.10/335.62 GB from an earlier, SHMEM-pool-less
sweep — a known calibration gap outside `ECALC_PLANE_CAP=2^31`). The **memory** numbers in the table above are the
C-measured ones (trustworthy, `--check-c`-style exact arithmetic, no HIP calls); the **wall-time** numbers are
`estimate.py`'s piece-count-driven model and should be read as a *trend* (roughly double the time), not a
calibrated absolute, until a real run confirms it.

##### (c) `ECALC_NP` / `RNS_STRATEGY` — already at the cheapest setting; not a lever
`ECALC_NP=auto` (the launch line's setting) is already the minimum-primes choice. Forcing `ECALC_NP=4` costs **+17.2
GB more** per node at the target (internal target notes' existing figure, modelled) — strictly worse. `RNS_STRATEGY=auto`
already selects the cheaper B/B4 form where it fits the pools, falling back to C only when it must; there is no
further headroom to find here without new code.

##### (d) Ask the admins for the amdgpu/TTM limits — zero engineering cost, depends on the answer
TARGET_WISHLIST §2.1 already names the parameters: `amdgpu` module params `gttsize`, `vm_size`,
`no_system_mem_limit`; `ttm` module params `pages_limit`, `page_pool_size`. OFI17/RESULTS §107 already answered half
of the open question (VMM shares the `hipMalloc` edge on aac7's SPX nodes) — the remaining ask is whether the
*target's* kernel allows raising the edge itself (currently 373 GB/node, A6, **measured**). If it can be raised even
modestly, both headline sizes (405.42 GB device, 32.42 GB over the raw 373 GB edge) could fit without any cap change
or digit cut. No modelled cost; this is the cheapest lever by far if the admins can grant it, and the one this report
recommends asking about first.

#### 6. Summary

| option | digits | device | fits 363/368 | wall (modelled, 5.276e13-equivalent effort) |
|---|---|---|---|---|
| headline, as launched today | 5.276 × 10¹³ | 405.42 GB | **no / no** (over the raw edge) | 290.2 / 303.9 s |
| size cut only | **4.452 × 10¹³** | 362.47 GB | yes (thin) / yes | 260.7 / 279.0 s (faster: fewer digits) |
| `ECALC_PLANE_CAP=2^30`, full headline | 5.276 × 10¹³ | 345.28 GB | yes / yes | ≈ 527.3 s modelled (+82 %) |
| `ECALC_PLANE_CAP=3*2^29`, full headline | 5.276 × 10¹³ | 366.76 GB | no / yes (thin) | ≈ 496.6 s modelled (+71 %) |
| ask the admins to raise the edge | 5.276 × 10¹³ | 405.42 GB | depends on the answer | 290.2 / 303.9 s (unchanged) |

**Open items carried forward:** the SHMEM pool's device-vs-host labelling under `COMM_SHMEM_DEVHEAP=1` (§4's
"razor-thin margin" note — if the SHMEM pool (1.61 GB) also turns out to be device-resident, 4.452 × 10¹³ fails the
363 GB bar by ≈ 1.1 GB and the safe choice becomes ≤ 4.0 × 10¹³); the host-pinned redesign (a) remains unquantified
without new code; the `ECALC_PLANE_CAP=2^30` wall-time estimate (b) is a trend, not a calibrated number, pending a
real run.

## SC15: scouting Batch 3 for the target 5.1 × 10¹³ digits on 576 nodes (Phase 15, agent SC; desk work)

(former file: SC15.md)

Branch `p15-SC` from `int15e` 6990778. Desk work only: no pipeline code and no node jobs. The aac6 login node ran `MN_PLAN_ONLY` and
`BS_LAYOUT_ONLY` in the clone `~/ntt-SC15`, which is at db6526f (int15e plus this branch's first commit).
Written 2026-09-28, 00:40–08:30 EDT. Every number is labelled **measured**, **modelled** or **assumed**.
- **measured** means a recorded run: an aac6 node log, or a login-node plan or layout, which is the C code's exact decision.
- **modelled** means `mn_model` / `mem_model` arithmetic on measured inputs.
- **assumed** means a target parameter that aac6 cannot give.

The scripts are `tests/sc15_model.py`, `sc15_km.py`, `sc15_nocache.py`, `sc15_extra.py`, `sc15_groups.py`, `sc15_round.py` and
`sc15_sweep2.py`. They are what-if variants that monkey-patch `mn_model`, run from `ecalc/`. Their outputs are in `archive:results-raw/SC15/`.

#### 0. The answer in one page

**Most of the modelled wall is now the distributed levels, the reciprocal and the division.** It is not init, and it is not the write.

The integrator's late facts change the baseline:
- **TC:** the mn transform cache's two slots, 137 GB per node, fit nowhere inside 480 GB at 5.1 × 10¹³.
- `mn_model` credits those two slots with −87.5 s.
- So the likely target baseline is **406.8 s without the write and 426.0 s with it** (auto, no cache, modelled). The standing figure of 324.3 / 355.4 s assumes the slots.
- aac6 data on the cache's value is mixed. TC's m1 at 10¹⁰ on 2 processes (measured): `total` 113.8 s with the cache off and 139.6 s with it on. So the true cost of losing the cache lies somewhere between 0 and 87 s. The ranking below holds at both ends; the gains are given at both ends.

**The ranked Batch 3 candidates** (gains at 5.1 × 10¹³ in seconds, modelled, as without / with the write at 0.6 GB/s; negative means faster):

| rank | item | gain, cache 0 (the likely baseline 406.8 / 426.0) | gain, cache 2 (324.3 / 355.4) | effort | risk | aac6 can measure | digits |
|---|---|---|---|---|---|---|---|
| 1 | **P24: four primes at 24 digits per transform point in the mn tier** (§3.1) | **−94.5 / −81.1** (×1.10 surcharge assumed: ≈ −80 / −70) | −64.0 / −54.8 (×1.10: −53.1 / −45.2) | high, 1.5–3 weeks | medium–high (the mn core's gather, CRT and carries) | correctness (t_mn_grid, mnaccept mn, 10⁹–10¹⁰ at sizes 2–4); the per-piece price, as NP measured 3 vs 4 primes | **moves every grid step by ≈ 4/3**: the 480 GB ceiling (5.57 × 10¹³) becomes reachable below today's 5.1 × 10¹³ wall (§4) |
| 2 | **DKM: the division in two quotient halves with a half-length reciprocal** (Karp–Markstein / GMP mu_div) (§3.2) | **−31.9 / −16.2** | −22.1 / −11.2 | medium, about 1 week | medium (the division and its correction loop; the writer's hook moves) | correctness at sizes 1–4; the size-1 analogue could be timed at 10¹¹ (not modelled here) | the critical path loses 20 pieces (242 → 224 at cache 2) |
| 3 | **`MN_GROUPS=4,16,64,576`** (launch line, no code) (§3.3) | **−30.6 / −30.6** | −13.0 / −12.9 | hours: a 4-way-level test on aac6 at size 4 | low–medium (4-way folds: the k-way code runs at 3-way today); **+8.6 GB** (node 464.0 GB modelled, C layout 435.50 GB measured) | the plan (measured: 212 pieces, plan check OK); the time only on the target (T5) | the step structure is flatter to 5.40 × 10¹³, but its own 480 GB ceiling is ≈ 5.40 × 10¹³ |
| 3b | `MN_GROUPS=2,4,8,16,32,64,576` (the 9-way top; the lower-memory form) | −14.6 / −14.6 | −5.1 / −5.1 | as 3 | low (+0.7 GB) | as 3 | 224 pieces |
| 4 | `MN_T_CHUNK_MB=2048` (launch line) (§3.4) | not computed at cache 0 | −5.5 / −14.9 / −48.2 at T_ROUND 0.01 / 0.03 / 0.1 s (assumed) | none | low (+4.3 GB) | no: T_ROUND is a target number | — |
| 5 | **the switch-over on min(pa, pb)** instead of pa + pb (NP's open issue) (§3.5) | −9.3 / −9.4 | −6.7 / −6.6 | ≤ 1 day | low (exact bound, argued in §3.5) | fully (NP's forced-bound knob) | — (superseded by P24 if P24 is built) |
| 6 | **one three-prime cache slot in plane pool 1** (a hypothesis: pool 1, 12 GiB per APU = 3 × 2²⁹ × 8 B, idle in the mn phases; to verify) | −9.9 / −10.0, 0 GB | 0 | about 2 days after the verification | medium (the pool's owners) | fully | — (superseded by P24) |
| 7 | a denser packed format: 16 limbs of 10¹⁸ in 15 words (60 bits each; −6.25 % bytes) | 0 / −4.1 | 0 / −4.1 | 1–2 days (packer, `unpack_digits`, `digcmp`, `t_out`) | low | fully | disk −1.4 TB |
| 8 | the writer's stripes (`MN_OUT_STRIPE`, built) | 0 / up to −19 (at 1.2 GB/s effective per node) | 0 / up to −33 | none | the aggregate rate is unknown | no (Lustre on the target) | — |
| 9 | **measure first: the top node's leaf** (`./ecalc 91694091804` with `ECALC_INIT_TL=1`) (§3.6) | decides items 9a / 9b | | 1 node, 5 min | — | yes | — |
| 9a | the seeds' CPU work from t = 0 (before the planes) | −3…−7 (assumed; only if the seeds bind) | same | 2–3 days | medium | yes | — |
| 9b | the VMM mapping rate: a microbenchmark of chunk size and flags, no GPU kernels running | 0 at the target if the seeds bind (modelled: the mapping floor ≈ 26 s, the seeds ≈ 32 s) | same | 1 node, 15 min | low | yes | — |
| 10 | the process exit (`_exit` after the output joins) | −1…−2 on both walls | same | hours | low | yes | — |
| — | already built, awaiting adoption: `BS_ARENA_ROOM=0.16` + `DIST_TWREC_G=1` (+ NP auto) | 10¹¹ (measured, fin15e): `dm` 106.5 → 91.4 s; `total` 200.9 → 185.2 s (4 + 4 runs); with the file 202.5 → 188.6 s. At the target: `DIST_TWREC_G` −4.8…−1.2 s (G5); ROOM unknown (+9 GB) | | | | | |
| — | **M6's mn edits**, −19.9 GB per node | 0 s | 0 s | about 1 day, after a 2-node `ECALC_LIVE` measurement | low–medium | yes (2 nodes) | the ceiling 5.57 → 6.0 × 10¹³; worth it only with P24 (§4) |

**Not worth doing** (§5): 3·2ᵏ planes in the mn tier (fewer pieces, the same points: 0 s); a new prime set (−3.3 s for more than 2 weeks);
a cost-aware grid that holds the reciprocal or division pieces under the three-prime bound (they are 18× over it: ≥ 14× slower, P15 (c));
equal digits per node (leaf −3.6 s; the tree is not comparable in the model); a hipMalloc arena at the target (`DM_TIGHT` needs the
VMM pool; the fin15e series measured init +8 s with `DB_POOL_VMM=0` at 10¹¹); the stream mapper (two node reboots, MAP15); N3x's fusion
(0.3 s); RL's switches (0).

**The largest uncertainties are target-only and assumed.** Ranged at cache 2 (modelled):
- the fabric's bandwidth, 50–200 GB/s per APU: +70 / −35 s;
- a global taper of 0.5: +31 s;
- `T_ROUND`, 0.01–0.1 s: −15 / +54 s;
- the write aggregate: up to −33 s;
- the cache itself: 0 to +87 s.

The first target session should measure these (TARGET_TASKS T1, T2, T5, T7) before Batch 3's gains are trusted.

**Suggested Batch 3** (for the user's decision; nothing is built):
- **(a)** P24, with DKM after it or in parallel (different files: `rns_dist.c` / CRT, and `newton_db.c`);
- **(b)** the min(pa, pb) switch-over now (cheap; kept even after P24 as the one-node guard);
- **(c)** the launch-line options for the target session: `MN_GROUPS` 3·3 / 9-way / 4·4·4·9 (T5) and `MN_T_CHUNK_MB` 1024 / 2048;
- **(d)** the rehearsal measurement (item 9), then 9a if the seeds bind;
- **(e)** the 60-bit format and `_exit`, both small.

Item 6 is worth doing only if P24 is not built.

#### 1. The breakdown at 5.1 × 10¹³ (the task's item 1)

`estimate.py --g 576 --D 8.854e10 --np-mn auto --verbose` gives 324.6 / 355.7 s. My harness gives 324.3 / 355.4 s at exactly 5.1 × 10¹³ / 576.
The integrator's earlier 322.6 / 354.1 s predates G5's model correction (`TWREC_F` no longer credited to the general map: +2.0 s).
With `ECALC_NP=4` the harness gives 346.6 / 377.8 s (= `estimate.py --target`). Seconds, modelled:

| phase | NP=4, cache 2 | auto, cache 2 | auto, **cache 0** | label |
|---|---|---|---|---|
| init | 19.3 | 19.3 | 19.3 | modelled: the one-node law (CAL15, fitted on measured 10¹¹ runs) at the top node's share (1.0356 × the average: exact arithmetic on the code's term split). The multi-node init (`shmem_init`, the meshes) is not modelled (MD) |
| seed wait | 12.9 | 12.9 | 12.9 | fitted (SEED15B, ten runs). The top node's spans ×1.2 in-run is assumed (bench ×1.5, S1) |
| batch | 30.8 | 24.2 | 24.2 | fitted on one-node runs. The seed fill's ×0.769 is measured at 10¹¹ and assumed at the leaf. Auto: the leaf at three primes |
| top levels (the leaf's mdev / dist) | 30.5 | 27.0 | 27.0 | as batch |
| distributed levels (tree 1–8) | 150.9 | 138.8 | 188.6 | modelled: S13's measured 2³¹ piece × the pipeline factor 1.22 (assumed to carry), 4 vs 3 primes 1.303 (NP measured 1.29–1.31), the fabric (assumed). Piece counts = the C plan (measured) |
| reciprocal | 44.0 | 43.8 | 55.7 | as the levels; 74 pieces, 35 of them at four primes (plan, measured) |
| division | 50.3 | 50.3 | 71.1 | as the levels; 40 pieces, all at four primes |
| other | 0.2 | 0.2 | 0.2 | modelled |
| digits' formatting and residues (no-write wall only) | 5.4 | 5.4 | 5.4 | measured rate at size 1 (0.061 s per 10⁹; 2 nodes measured 0.104) |
| exit | 2.4 | 2.4 | 2.4 | measured at size 1; assumed at 576 |
| **without the write** | **346.6** | **324.3** | **406.8** | |
| output exposed (with the write) | 38.9 (write 65.6 s at 0.6 GB/s − 29.0 hidden + exit) | 38.9 | 27.0 (the longer division hides more) | the 0.6 GB/s is measured single-stream and assumed for 576 at once; the overlap (0.577 × the division) is fitted at size 1 and assumed at 576 |
| **with the write** | **377.8** | **355.4** | **426.0** | |
| exposed fabric in the wall | 82.7 | 80.6 | — | assumed fabric (100 GB/s per APU, 2 µs) |

Memory: 455.4 GB per node (`mem_model`, modelled) against 480. The C layout is 426.61 GB (measured, login node). **Without the cache.**
With TC's two slots it would be 592.8 GB, and 524.1 GB with one (modelled). Piece counts: 242 on the critical path (128 + 74 + 40),
222 on node 0 (plan, measured, `archive:results-raw/SC15/plan_2,4,8,16,32,64,192,576.txt`).

#### 2. What is on the critical path, and why

- **The mn tier is 72 % of the wall without the write at cache 2** (levels + reciprocal + division = 232.9 of 324.3 s), and **78 % at
  cache 0** (315.4 of 406.8 s). The leaf (init, seeds, batch, top) is 83.4 s. The write adds 31 s (cache 2) or 19 s (cache 0) to the
  with-write wall.
- **The pieces' cost is their points** (modelled; one point costs the same at every cap). Fewer pieces only help when they mean fewer
  points. That is why 3·2ᵏ planes at the 192 level save nothing: the level goes from 36 to 24 pieces with the same points (§5). It is
  also why a larger cap (more digits per piece) helps quadratically: cost ∝ area / cap.
- **Three primes cannot hold the reciprocal's and division's pieces.** They are 1.04 × 10¹² terms against the bound of 5.84 × 10¹⁰ (18×).
  Splitting them under the bound multiplies the work (P15 (c): 14×). So under `auto` 35 of 74 reciprocal pieces and all 40 division
  pieces stay at four primes. **The answer to "can the division use three primes by a cost-aware grid" is no.**
- The one real lever on the four-prime pieces is the unused dynamic range. Four primes hold 2^206.6. A piece at the 2⁴⁰ cap needs
  min(pa, pb) · (10¹⁸)² < 2^158.6. So **48 bits are wasted per coefficient**; P24 uses them (§3.1).

#### 3. The items

##### 3.1 P24: four primes at 24 digits per transform point in the mn tier

- **Idea.** Today a transform point carries one 10¹⁸ limb. With four primes, a point can carry a 10²⁴ value: 4 limbs = 72 digits =
  3 points.
- **The coefficient bound.** A coefficient is below min(pa, pb) · 10⁴⁸. The cap gives min ≤ 2³⁹ points, so the bound is below
  2^198.5 < 2^206.6: a margin of ×280. It holds for every mn product, cyclic wrap included (each index pairs with at most one partner).
- **Byte-identical by construction:** it is the same exact product.
- **Cost per digit.** Measured per point: 4 primes 1.078 against 3 primes 0.827 (`T_PIECE_31_NP`). Per digit that is 1.078 × 0.75 =
  0.81 against 0.83, so P24 is slightly cheaper per digit than three primes, and it has no term bound.
- **The quadratic effect.** A piece at the cap carries 4/3 more digits, so the pieces fall 242 → 173 (cache 2: levels 12 → 8 per level,
  36 → 24, 20 → 12; the reciprocal 74 → 63, the division 40 → 26), and the grid steps move by ≈ 4/3.
- **The model** scales the mn operands (and the cuts) by 0.75 in points. The operand redistributions and the shifted adds keep moving
  limbs (`sc15_model.py`: pack).
- **What the model does not price:**
  - a load kernel reducing an 80-bit point mod p;
  - the CRT to base 10²⁴ and its split into limbs;
  - piece offsets in multiples of 4 limbs.

  Hence the ×1.05 / 1.10 / 1.20 surcharges on the local passes (`archive:results-raw/SC15/extra.txt`, cache 2): 265.7 / 271.2 / 281.9 s against
  260.3 s.
- **Code (not built)** in `rns_dist.c` `mn_core`:
  - the gather and pack into points (3 per 4 limbs);
  - `k_crt_batch`'s `dgarner4` output in base 10²⁴, then the carry and spill scan into 10¹⁸ limbs;
  - the grid's piece offsets;
  - `mn_plan.c` (points, the bound);
  - the models.

  The one-node tiers are unchanged (three primes at 18 digits are as cheap there).
- **Memory unchanged:** the planes are points, and the operands stay in limbs.
- **Tests:** `t_mn_grid` at 2–4 processes, `mnaccept mn`, and 10⁹ / 10¹⁰ identical at sizes 2–4. A per-product timing pair at size 2 on
  one node (NP's batch 4 method) prices the surcharge.

##### 3.2 DKM: the division in two halves with a half-length reciprocal

- **Today's cost.** The reciprocal is taken to k ≈ n = 2.83 × 10¹² limbs. Its last doubling (n/2 → n) is 20 pieces at 2⁴⁰. Then come
  A_h·μ (a high product, 20 pieces) and X·Q mod B^w (20 pieces). In area terms that is ≈ 2.0 n².
- **DKM's steps:**
  - r = 1/Q to h = n/2 + 2 limbs (the reciprocal stops one doubling earlier);
  - X_hi = high(A_top · r);
  - R1 = A − X_hi·Q·B^h, from X_hi·Q mod B^n;
  - X_lo = high(R1_top · r);
  - R = R1 − X_lo·Q, from mod B^(n+2);
  - today's correction loop.

  The area is ≈ 1.0 n².
- **Modelled** (`sc15_km.py` replaces `division_cost`; `archive:results-raw/SC15/dkm.txt`, `nocache.txt`):
  - the reciprocal 43.8 → 18.2 s and the division 50.3 → 53.7 s (cache 2);
  - pieces 74 + 40 → 54 + 42;
  - cache 2: −22.1 s without the write; cache 0: −31.9 s.
- **The with-write gain is about half.** The early writer can start only when X_lo exists, so it hides under the last low product
  (18.2 s) instead of 29 s. My overlap estimate: 344.2 s with the write at cache 2, 409.8 s at cache 0.
- **Combined with P24** (cache 2 / 0): 240.3 / 283.4 s without the write, and 290.3 / 330.1 s with it (DKM overlap).
- **Code:** `newton_mn_divmod` and `recip_mn` (stop at h). The mn primitives with cuts exist (`mn_prod_cut_x1`, shifts, addsub). The
  sign of R1 needs a guard limb. The writer's hook moves to after X_lo. The size-1 path (`newton_db_divmod_shifted`) has the same
  structure and could be measured at 10¹¹; it is not modelled here.
- **Risk:** the quotient halves can be off by a few. The existing loop (≤ 64 corrections, `ECALC_CORR_PATCH=2`) covers it. `ECALC_TEST_CORR`
  exercises it.

##### 3.3 The tree's group schedule (`MN_GROUPS`; TARGET_TASKS T5)

The C plan (measured on the login node, `archive:results-raw/SC15/plan_*.txt`, `layout_*.txt`; `ECALC_NP=auto COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024`)
and the model (the times), at 5.1 × 10¹³:

| MN_GROUPS | critical-path pieces (plan) | levels, node 0 / largest (plan) | C layout node (measured) | model node | wall cache 2 | wall cache 0 |
|---|---|---|---|---|---|---|
| 2,4,8,16,32,64,192,576 (today) | 242 | 8/12 ×4, 12/12 ×2, 32/36, 20/20 | 426.61 | 455.4 | 324.3 / 355.4 | 406.8 / 426.0 |
| 2,4,8,16,32,64,576 (9-way top) | 224 | …, 38/38 | 427.56 | 456.1 | 319.2 / 350.3 | 392.2 / 411.4 |
| 4,16,64,576 | 212 | 12/20, 16/20, 18/20, 38/38 | 435.50 | 464.0 | 311.3 / 342.5 | 376.2 / 395.4 |
| 4,16,64,192,576 | 230 | 12/20, 16/20, 18/20, 32/36, 20/20 | 435.50 | 464.3 | 316.5 / 347.6 | — |
| 2,4,16,64,192,576 | 234 | — | 435.50 | 464.3 | 318.1 / 349.2 | — |

The 4-way levels do the small products over larger groups, so they get larger caps, fewer points and fewer exchanges. The model's
earlier verdict (3·3 better than 9-way by 5 %) is reversed at 5.1 × 10¹³ under auto. The fabric terms are assumed, so the choice
should be made on the target (T5, three schedules). The C-plan sweeps (5.00–5.70 × 10¹³, `archive:results-raw/SC15/sweep_<groups>.txt`) show that
4,16,64,576 stays at 207–239 critical-path pieces up to 5.70 × 10¹³, against 239–294 today. **Its memory binds first:** it reaches
480 GB at ≈ 5.40 × 10¹³ (modelled, `sweep_g4.txt`).

##### 3.4 `MN_T_CHUNK_MB` (and `MDB_SHIFT_CHUNK_MB`) at 2048

`archive:results-raw/SC15/round.txt` (cache 2, modelled):

| chunk | T_ROUND 0.01 s | 0.03 s (the model's assumption) | 0.1 s | node |
|---|---|---|---|---|
| 1024 (today) | 308.9 s | 324.3 s | 378.3 s | 455.4 GB |
| 2048 | 303.4 s | 309.4 s | 330.1 s | 459.7 GB |
| 4096 | 300.6 s | 301.5 s | 304.6 s | 477.0 GB |

It is a launch-line choice that only the target's round cost can settle (TARGET_TASKS item 13: test 0 against 1024; add 2048). 4096 leaves
3 GB of margin: not recommended.

##### 3.5 The switch-over on min(pa, pb)

- **The bound.** A coefficient of a piece's product sums at most min(pa, pb) terms, each < (10¹⁸)². This is true for the cyclic
  convolution too, as long as each operand fits the length.
- **The check today** is pa + pb (NP's conservative form). Under min, tree levels 5 and 6 go to three primes: level 5's pa + pb is
  5.851 × 10¹⁰, only 0.14 % over the bound, with min 2.42 × 10¹⁰. A few reciprocal steps go to three primes too.
- **Modelled:** −6.7 s (cache 2) and −9.3 s (cache 0). A larger three-prime bound from a new prime set (L8: 8.25 × 10¹⁰) adds nothing
  beyond min (−3.3 s alone).
- **Code:** `ec_np_for` gets the two lengths (`mn_core`, `dist_core`), plus `mn_plan.c` and the models. The forced-bound knob tests it
  on aac6.

##### 3.6 The leaf's start: init, mapping and seeds

- **At 10¹¹, level 1 waits for the mapping** (MAP15, measured): 124 chunks at ≈ 0.2 s per 2 GiB, serialized by the runtime, and
  hipMalloc is ≈ 2× faster.
- **At the target's top node the seeds probably bind** (modelled):
  - the node maps fewer bs chunks (bs regions 201.8 GB against 261.2 at 10¹¹): ≈ 7 + 94 × 0.2 ≈ 26 s;
  - its seed spans are slower (×1.2–1.5, S1): init + seed wait = 32.2 s.
- **The one-node rehearsal settles which binds** (`./ecalc 91694091804` with `ECALC_INIT_TL=1`, 5 min on one node).
- **The integrator's fact (3):** `ECALC_NP=4` on one node at that size stops with rc 6. Plane pool 1 on APU 0 grows inside a phase,
  12.9 → 17.2 GB. So run the rehearsal with `ECALC_NP=auto` (at size 1 the one-node tiers are at three primes, as at the target's leaf).
  A 4-prime one-node rehearsal needs pool 1 sized at init for four planes.
- **Which item follows:**
  - If the seeds bind: start their CPU spans at t = 0 into the pinned buffers (the buffers first), and keep the pregrow mapping (no
    mapping during GPU compute: MAP's two reboots). About −3…−7 s (assumed).
  - If the mapping binds: the VMM microbenchmark first.
- The fin15e series (measured) found `DB_POOL_VMM=0` init +8 s at 10¹¹, so the hipMalloc arena is not the way at the target (and
  `DM_TIGHT` needs the VMM pool).

##### 3.7 The write

- **Packed file:** 39.35 GB per node at 0.6 GB/s is 65.6 s (modelled); 29.0 s of it hides under the division at cache 2 and 39 s at cache 0.
- **The 60-bit format:** 16 limbs in 15 words, exactly 18 digits per 60 bits (10¹⁸ < 2⁶⁰). It cuts 6.25 %, which is −4.1 s of exposed
  write, and −1.4 TB of disk.
- **Stripes** (built) or two writer streams per node, if Lustre's aggregate allows (576 × 1.2 GB/s ≈ 690 GB/s: unknown). The modelled
  gain is −33 s at cache 2 (322.6 s against 355.4 s with the write at 1.2 GB/s). This is target-only.

##### 3.8 Freeing memory for the cache (TC's question)

One slot at the target costs 68.7 GB per node. The room is 24.6 GB, plus the tree-phase arena slack of 19.9 GB (TC). None of these
closes the gap:
- M6's −19.9 GB;
- AS's +9 GB (the wrong way);
- DKM's smaller division buffers (the dm need falls toward the tree need: ≤ −20 GB, the same bound as M6's).

**Only item 6 (a three-prime slot in the idle pool 1) finds a slot's worth of memory without new bytes, and it is unverified:** whether
pool 1 is idle during the mn phases must be read from the code or measured. **P24 and DKM recover the cache's time instead of its memory.**
With cache 0, P24 + DKM + 4,16,64,576 is 259.8 / 306.5 s (modelled), against today's modelled (cache 2) 324.3 / 355.4 s.

#### 4. The next grid step and the digits (the task's item 3)

Today, under auto at cache 2 (the model sweep, which matches the C plan's steps; `archive:results-raw/SC15/sweep_min.txt` (min variant) and the
C-plan sweeps):
- **The critical path steps** 242 → 246 at 5.12 × 10¹³ (+5 s) and → 266–270 at 5.17–5.18 × 10¹³ (+22 s). From there it climbs to 289–294
  pieces by 5.57 × 10¹³, the 480 GB ceiling: 414 s without the write (`estimate.py --target`).
- **Growing toward 5.57 × 10¹³ costs +9 % digits for about +20 % time.** No launch-line or piece-count trick moves the steps cheaply:
  - 3·2ᵏ planes save no points;
  - a larger cap needs memory the planes do not have;
  - the 4-way schedule flattens the steps but hits 480 GB at ≈ 5.40 × 10¹³.
- **P24 is the one item that moves the steps**, by 4/3 in digits: the piece counts at 5.57 × 10¹³ fall to about today's at 4.2 × 10¹³.
  The ceiling then binds, not the steps: 5.57 × 10¹³ at 480 GB (modelled). With M6's mn edits (−19.9 GB) it is ≈ 6.0 × 10¹³.
- **The P24 sweeps** (5.0–6.0 × 10¹³, cache 2 and 0; `archive:results-raw/SC15/sweep2_*.txt`) give the walls. The table below is from them.

##### 4.1 The digits against the time (modelled; auto; the fabric assumed; `archive:results-raw/SC15/sweep2_*.txt`)

| total digits | today, cache 0 | P24, cache 0 | today, cache 2 | P24, cache 2 | node (all) |
|---|---|---|---|---|---|
| 5.10 × 10¹³ | 406.8 / 426.0 s, 242 pieces | 312.3 / 344.9 s, 173 | 324.3 / 355.4 s, 242 | 260.3 / 300.6 s, 173 | 455.4 GB |
| 5.20 × 10¹³ | 455.5 / 475.8, 274 | 314.6 / 349.3, 173 | — | 262.6 / 304.7, 173 | 460.5 |
| 5.40 × 10¹³ | 489.4 / 506.1, 291 | 336.6 / 370.7, 182 | — | 279.8 / 322.1, 182 | 470.6 |
| **5.55 × 10¹³ (just below the 480 GB ceiling)** | 493.2 / 511.6, 293 | **338.0 / 373.8, 182** | ≈ 414 / 447 (the `--target` ceiling line) | **281.0 / 325.1, 182** | 478.7 |
| 5.70 × 10¹³ (past 480; with M6's −19.9 GB ≈ 467) | 496.4 / 516.5, 292 | 342.8 / 380.3, 182 | — | 285.8 / 331.5, 182 | 486.9 (≈ 467 with M6) |
| 5.75 × 10¹³ (P24's next step) | 515.0 / 535.7, 304 | 385.0 / 423.0, 206 | — | 315.0 / 361.3, 206 | 489.6 |

- **With P24 the critical path is flat at 170–182 pieces from 5.0 to 5.70 × 10¹³.** The next step is at 5.75 × 10¹³.
- So **5.55 × 10¹³ (+8.8 % digits) runs in less time than today's 5.1 × 10¹³**, at either cache setting: 338.0 / 373.8 s against
  406.8 / 426.0 s at cache 0, and 281.0 / 325.1 s against 324.3 / 355.4 s at cache 2.
- The memory ceiling (480 GB at ≈ 5.57 × 10¹³) then binds. **M6's mn edits (−19.9 GB) would open 5.70 × 10¹³ (+11.8 %) at 342.8 / 380.3 s
  (cache 0).**
- Without P24, going from 5.1 to 5.55 × 10¹³ costs +86 s at cache 0. Staying at 5.1 × 10¹³ is right for today's code; with P24 the
  target should be re-chosen at ≈ 5.55 × 10¹³ (or 5.70 × 10¹³ with M6).

#### 5. Examined and not recommended

| item | finding (modelled unless marked) |
|---|---|
| 3·2ᵏ planes in the mn tier (the 192 level's plane at 3·2³⁷ fills the per-APU pool exactly) | level 7: 36 → 24 pieces, the same points: 324.3 → 324.1 s. At 576, 3·2³⁸ < 2⁴⁰; 9·2³⁷ would fill the pool but needs 9 \| p − 1 (new primes) |
| a larger three-prime bound (L8's 3 \| p − 1 set, 8.25 × 10¹⁰ terms) | −3.3 s, ≥ 2 weeks; nothing beyond min(pa, pb) |
| a cost-aware grid holding the reciprocal or division pieces under the three-prime bound | the pieces are 18× over it; P15 (c) modelled 14× slower |
| equal digits per node (instead of equal terms) | the leaf −3.6 s; the model's tree for equal shares is an older formula (+4.4 s), so it is inconclusive and needs a plan check |
| a transform cache for Q across the reciprocal's last step and X·Q | one 2⁴⁰ piece's planes are 61.2 GB per node |
| `DB_POOL_VMM=0` (the hipMalloc arena) | init +8 s at 10¹¹ (fin15e, measured); at the target `DM_TIGHT=0` is 502.8 GB |
| `DB_POOL_VMM_STREAM` | two node reboots (MAP15, measured) |
| `NTT_R3_FUSE`, `RNS_R3_MINK` | 0.3 s at the target (N3x15) |

#### 6. Tests and commands (all desk; the plan and layout on aac6's login node)

| what | command | result |
|---|---|---|
| the breakdown | `cd ecalc; python3 estimate.py --g 576 --D 8.854e10 --np-mn auto --verbose` (and `--np-mn 4`) | 324.6 / 355.7 s; 346.9 / 378.0 s (modelled) |
| the harness | `python3 ../tests/sc15_model.py base` | 346.6 / 377.8 (NP 4); 324.3 / 355.4 (auto); 288.2 (three primes, not runnable) |
| NP, P24, R3, groups, fabric | `sc15_model.py np / pack / r3 / groups / fabric` | §0, §3 |
| DKM | `sc15_km.py pack`, `sc15_km.py pack 4,16,64,576` | `archive:results-raw/SC15/dkm*.txt` |
| cache off | `sc15_nocache.py` | `archive:results-raw/SC15/nocache.txt` |
| chunk × T_ROUND | `sc15_round.py` | `archive:results-raw/SC15/round.txt` |
| the plans (login node, 2026-09-28 ≈ 00:30 EDT) | `ECALC_NP=auto COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024 MN_GROUPS=<g> MN_PLAN_ONLY=51000000000000:576 ./ecalc` for 5 schedules | rc 0, plan check OK for all; pieces §3.3 |
| the layouts | `ECALC_NP=auto COMM_SHMEM_ROUND_MB=1024 MN_GROUPS=<g> BS_LAYOUT_ONLY=88541666666.6667:576 ./ecalc 51000000000000 /dev/null` | node 426.61 / 427.56 / 435.50 GB (§3.3) |
| the plan sweeps | the same `MN_PLAN_ONLY` at 5.00–5.70 × 10¹³ step 0.01 for 3 schedules | `archive:results-raw/SC15/sweep_<groups>.txt` |

#### 7. Open issues

- **The gains are models on assumed fabric terms.** P24's surcharge and DKM's correction count are not measured. The size-1 DKM and
  P24's per-piece price can be measured on aac6 once built.
- **The cache's true value:** the model says 87.5 s, and TC's m1 data is mixed. Every gain here is given at both ends.
- **Item 6 (pool 1 idle in the mn phases) is a hypothesis.**
- **`mn_model`'s schedules that do not divide 576** (e.g. 2,…,512,576) are costed wrongly: the top group is not the largest. Only divisor
  schedules are used here.
- **The model sweeps of the base case at 0.01 × 10¹³** died with the session. The C-plan sweeps and `sweep_min.txt` cover the step
  structure.

#### RESUME
- Done: everything above; committed on `p15-SC`. aac6: clone `~/ntt-SC15` (db6526f, built), outputs in `~/SC15/` (copied to
  `archive:results-raw/SC15/`). No node job was ever submitted; none is pending.
- Nothing is owed; the proposal waits for the user.

## MS15: the node memory at 5.1–5.7 × 10¹³ on 576 nodes, and the largest target that fits 480 GB (Phase 15, agent MS)

(former file: MS15.md)

Branch `p15-MS` from `int15i` d2d05b3. The worktree HEAD was main 99c7de4, so I branched from d2d05b3. aac6 clone: `~/ntt-MS15`.
This was desk work on the aac6 login node (`BS_LAYOUT_ONLY`, `MN_PLAN_ONLY`, python), 2026-09-28, 22:10–22:50 EDT. I ran no GPU jobs.

Labels:
- **measured (layout)** / **plan**: the C code's own `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` output. It is exact for the code; nothing is allocated.
- **modelled**: `mem_model` / `mn_model` / `estimate.py`. The fabric is **assumed** at 100 GB/s per APU and 2 µs. The write is 0.6 GB/s per node. The mn cache has 0 slots. P24's surcharge of 1.025 was **measured** by P24.

"Room" means `BS_ARENA_ROOM`: 0.16 is the code's default since be2eec3, and 0 turns it off. "DKM layout" means the `dm_layout` follow-up that DKM proposed (not built, **modelled** only).

#### The candidate sizes (MN_P24=2 NEWTON_DKM=1, launch line ECALC_NP=auto RNS_DIST_CACHE_FIT=1 COMM_SHMEM_ROUND_MB=1024)

| total digits | why this size | node 0 pieces t + r + d / largest group (plan) | wall no write / with write, s (modelled) | node GB at room 0.16 (modelled; arena measured) | node GB at room 0 | node GB with the DKM layout, room 0.16 / 0 (not built) |
|---|---|---|---|---|---|---|
| 5.100 × 10¹³ | today's target | 145 = 72 + 51 + 22 / 157 | 288.2 / 334.4 | **471.93** fits | 455.57 fits | 463.34 / 450.40 |
| **5.167 × 10¹³** | **largest size that fits with the room**: the arena gains one 2 GiB chunk per APU at 5.168 × 10¹³ (layout) | 145 / 157 | 289.3 / 336.4 | **471.93** fits | 458.96 fits | 463.34 / 453.73 |
| 5.192 × 10¹³ | just below the 145 → 149 step (5.194) | 145 = 74 + 51 + 20 / 155 | 286.2 / 335.3 | 480.52 **over by 0.52** | **460.23** fits | 463.34 / 454.97 |
| **5.276 × 10¹³** | just below the largest step, 149 → 157 at 5.278, where DKM's division goes from 22 to 28 pieces (+14.7 s) | 149 = 76 + 51 + 22 / 159 | 295.5 / 343.9 | 480.52 over | **464.48** fits | 471.93 / 459.14 |
| 5.326 × 10¹³ | just below 157 → 161 (5.328) | 157 = 76 + 53 + 28 / 167 | 310.8 / 358.1 | 480.52 over | 467.02 fits | 471.93 / 461.62 |
| 5.382 × 10¹³ | just below 163 → 165 and the largest group's 167 → 169 (5.384) | 163 = 82 + 53 + 28 / 167 | 313.7 / 361.6 | 489.11 over | 469.86 fits | 471.93 / 464.41 |
| 5.550 × 10¹³ | INT3's / P24's size | 167 = 86 + 53 + 28 / 169 | 320.3 / 370.2 | 497.70 over | **478.84** fits (1.2 GB margin) | 480.52 over / 472.75 |
| **5.570 × 10¹³** | **largest size that fits with the room off** (the ceiling is 5.571 × 10¹³, 479.99 GB) | 167 / 169 | 322.9 / 373.0 | 497.70 over | **479.93** fits (0.07 GB margin) | 489.11 over / 473.74 |
| (5.696 × 10¹³) | the ceiling with the DKM layout, room 0 (not built) | 169 / 169 | 324.9 / 376.4 | 489.11 over | 486.77 over | 489.11 / 479.95 |
| (5.74 × 10¹³) | the tree's grid step (the largest group 169 → 189) | 169 / 189 | 359.8 / 411.8 | — | 489.23 over | — |

**The steps.** The plan sweeps are in `archive:results-raw/MS15/plan_sweep_p24dkm.txt`: 4.80–6.00 × 10¹³ in steps of 0.02, plus finer steps near each edge. Every point is `plan check … OK`.
- **Node 0's critical path** goes 145 → 149 at 5.194, → 157 at 5.278, → 161–163 at 5.328, → 165 at 5.384, → 167 at 5.50, → 169 at 5.67 × 10¹³.
- **The largest group** is 169 from 5.384 to 5.73 × 10¹³, and 189 from 5.74 × 10¹³.
- Between the steps, the modelled wall grows by only about 1–3 s per 10¹² digits.

**Reading (modelled).**
- **With the room (the code's default), the largest size that fits is 5.167 × 10¹³.** That is only 1.3 % more than today's target, at the same 145 / 157 pieces and +2 s.
- **With `BS_ARENA_ROOM=0` on the launch line, the ceiling is 5.571 × 10¹³.**
  - 5.57 × 10¹³ gives 322.9 / 373.0 s at 479.93 GB, with no margin.
  - 5.55 × 10¹³ gives 320.3 / 370.2 s at 478.84 GB.
  - 5.276 × 10¹³ gives 295.5 / 343.9 s at 464.5 GB, with 15.5 GB of margin. It sits just below the largest step (+13 s with the write).
  - In digits per second with the write: 5.192 × 10¹³ gives 1.548 × 10¹¹, 5.276 × 10¹³ gives 1.534 × 10¹¹, 5.1 × 10¹³ gives 1.525 × 10¹¹, and 5.57 × 10¹³ gives 1.493 × 10¹¹.
- **The room's time at size > 1 is not modelled**, so the walls are the same with the room on or off (**assumed**). At size 1 the room was **measured** to take the division from 68.2 to 53.5 s at 10¹¹ (no remaps; `ecalc/README.md` `BS_ARENA_ROOM`). Turning the room off at 576 may bring remaps back into the division. That cost is unknown, and it is the risk in every "room 0" row.
- **Turning the room off needs `BS_ARENA_ROOM=0` on the launch line.** The code's own room check (`binsplit.c as_room_fits`) keeps the room up to about 5.63 × 10¹³ (§1.3). On the default code, any target from 5.168 up to about 5.63 × 10¹³ therefore runs at 480.5–497.7 GB (modelled).

#### 1. The two figures (question 1)

##### 1.1 Both reproduced (modelled; `archive:results-raw/MS15/maxfit.txt`, and `tests/ms15_candidates.py`)

| call | 5.1 × 10¹³ | 5.55 × 10¹³ | its memory design |
|---|---|---|---|
| INT3: `estimate.py --g 576 --D <D> --cache-slots 0` (the design is `DEFAULT15C`) | 471.93 GB | **497.70 GB** | B2: `BS_ARENA_ROOM` 0.16, whole-chunk arenas |
| P24: `tests/p24_model.py` `design()` (a `p15b` design without `p15c`) | 455.43 GB | **478.70 GB** | B1: arena room 0 |
| `DEFAULT15C(arena_room=0)` | 455.43 | 478.70 | the same as P24's call |

The whole difference is the arena room: +16.5 GB at 5.1 × 10¹³ and +19.0 GB at 5.55 × 10¹³. P24's `design()` never set `p15c`, so `Design.arena_room` stayed 0.

##### 1.2 Against the C layout (measured, login node; `archive:results-raw/MS15/lay_*.txt`, `new_*.txt`)

The command was `ECALC_NP=auto RNS_DIST_CACHE_FIT=1 COMM_SHMEM_ROUND_MB=1024 [MN_P24=…] [NEWTON_DKM=1] [BS_ARENA_ROOM=0] BS_LAYOUT_ONLY=88541666666.6667:576,96354166666.6667:576 ./ecalc 55500000000000 /dev/null`.

| layout | arena at 5.1 × 10¹³ | arena at 5.55 × 10¹³ | dm need / device at 5.55 |
|---|---|---|---|
| defaults (room 0.16) | 300.65 GB | **326.42 GB** = estimate.py's arena | 80.332 GB |
| `BS_ARENA_ROOM=0` | 284.15 | **307.42** = P24's call's arena | 76.853 |
| `MN_P24=2 NEWTON_DKM=1`, room 0.16 (with the fix, §1.4) | 300.65 | 326.42 | 80.369 (+37 MB) |
| the same, room 0 | 284.28 | 307.56 (+0.14 GB) | 76.889 |

- The code's default (room 0.16, which the launch line does not override) lays out the arena that estimate.py prices. So **497.7 GB is the model's node at 5.55 × 10¹³ for the code as it is. P24's 478.7 GB is the node with `BS_ARENA_ROOM=0`.**
- **The call that was wrong is P24's `tests/p24_model.py design()`.** It is fixed: `p15c=True`, room 0.16 by default (a `room` argument gives 0), and `cache_fit=False`, so its rows still price the slots they force. Its sweep now prints 497.7 GB at 5.55 × 10¹³.
- estimate.py's call was right.
- A correction to INT3's §0 and §3.3: the arena at the target is set by the **dm need** (+ the room, in whole 2 GiB chunks per APU), not by the bs regions. The bs regions are 225.2 GB; the arena is 326.4 GB. P24's +37 MB per device disappears in the chunk rounding with the room, but shows as +0.14 GB without it.

##### 1.3 The node peak against the code's own room check (modelled; a finding)

- The node peak at the target is the **bs phase**: the device at init (planes 120.88 + arena) + 6.0 GB of VMM growth (**measured** at size 1) + the host at init (44.41 GB).
  - The host at init is the runtime 7.0 + staging 4.29 + the two 8 GiB seed buffers 17.18 + comm 6.0 + the SHMEM pool 9.93 GB.
  - The division's peak is 14 GB lower (483.6 against 497.7 at 5.55 × 10¹³, room 0.16).
- `binsplit.c as_room_fits` keeps the room while planes + arena + 31.6 GB ≤ 480. The 31.6 GB is 7 GB + 8 GiB + 6 GB + a 10 GB margin, which is 18.8 GB less than the model's 50.4 GB.
  - So the code keeps the room past the budget: 5.55 × 10¹³ gives 120.88 + 326.42 + 31.6 = 478.9 ≤ 480, so the room is kept, while the modelled node is 497.7 GB.
  - The model drops the room from about 5.63–5.65 × 10¹³ (mem_model's `as_room_fits` on the run's planes).
  - `BS_LAYOUT_ONLY` decides on `room_planes` of 103.70 GB (three-prime pools), so the layout keeps the room one chunk longer: 335.0 GB of arena at 5.64 × 10¹³ (`edges_room.txt`). This does not matter below 5.6 × 10¹³.
- The user can therefore not rely on the code to turn the room off for a larger target. `BS_ARENA_ROOM=0` has to be on the launch line. This is open issue 1.

##### 1.4 `BS_LAYOUT_ONLY` did not see `MN_P24=2` (measured; fixed)

- `rns_pool0_np()` reads `COMM_SIZE`, which `BS_LAYOUT_ONLY` did not set. So with `MN_P24=2`:
  - the layout's scratch was the default's (`lay_p24dkm.txt` = `lay_def.txt`, byte for byte);
  - **the layout's own `plan check` also ran without P24** (18-digit pieces, "151 of 264" instead of "145 of 187").
- With `COMM_SIZE=576` set by hand, both follow `MN_P24=1`'s pattern (+34 / +37 MB of dm need).
- **The fix** (`binsplit.c binsplit_layout_comm_size`, called in `ecalc.c`'s check loop and in `binsplit_layout_only`) sets `COMM_SIZE` to each point's g while that point is checked and laid out, then restores it. Nothing else in the layout reads it.
- Results after the fix:
  - `MN_P24=2` layout = the hand-set `COMM_SIZE=576` layout (identical, room 0.16 and 0);
  - the default, `MN_P24=1` and `BS_ARENA_ROOM=0` outputs are byte-identical to the build before the fix;
  - DOC2's mixed command (4e10:1, 1e11:1, two points at 576, 91694091804:1) is byte-identical to INT3's `archive:results-raw/INT315/layout_auto.txt`, apart from META.

##### 1.5 mem_model's P24 term (P24's open issue 2; done)

- `mem_model.mn_scratch` now has P24's branch of `rns_mul_dist_mn_scratch`: `mn_p24_of`, `p24_grid_shape` / `p24_split`, the points' plane, `rl` rows, and A's sequence freed before B's. It is off by default: `opts p24=0`.
- `mn_model.memory` passes `MN_P24` and `ECALC_NP_AUTO_MIN`, so `estimate.py`'s node follows `MN_P24`.
- `--check-c` reads `MN_P24` / `ECALC_NP_AUTO_MIN` from the environment, because the layout line does not print them.
- **`mem_model.py --check-c` results (all measured layouts, login node):**
  - 0 terms not exact and 0.0000 % arena difference on each of: the defaults, `MN_P24=1`, `MN_P24=2` (room 0.16 and 0, 5.1 and 5.55 × 10¹³);
  - the same on the 16 edge points 5.167–5.695 × 10¹³ with `MN_P24=2`, room 0.16 and 0 (64 plane figures);
  - the same on DOC2's command (20 plane figures, with and without `MN_P24=2`);
  - a `MN_P24=2` layout checked without `MN_P24` correctly shows 6 terms not exact.

#### 2. The node at 5.1 and 5.55 × 10¹³ (question 2)

| T | switches | wall no write / with, s (modelled) | pieces (plan) | node room 0.16 | node room 0 |
|---|---|---|---|---|---|
| 5.1 × 10¹³ | defaults | 400.0 / 420.5 | 222 (108 / 74 / 40), largest 242 | **471.93** (arena 300.65) | **455.43** (284.15) |
| 5.1 × 10¹³ | `MN_P24=2 NEWTON_DKM=1` | 288.2 / 334.4 | 145 (72 / 51 / 22), largest 157 | **471.93** (300.65) | **455.57** (284.28) |
| 5.55 × 10¹³ | defaults | 484.9 / 504.9 | 259 (132 / 81 / 46), largest 293 | **497.70** (326.42) | **478.70** (307.42) |
| 5.55 × 10¹³ | `MN_P24=2 NEWTON_DKM=1` | 320.3 / 370.2 | 167 (86 / 53 / 28), largest 169 | **497.70** (326.42) | **478.84** (307.56) |

- The nodes are **modelled**; every arena is **measured** by the layout and equals the model.
- `NEWTON_DKM=1` as built does not change the layout. The difference between the switch sets is P24's scratch only.
- The walls are at 5.1 / 5.55 × 10¹³ exactly. INT3's rows (400.2, 481.8; 288.1, 320.2) are at D 8.854 / 9.635 × 10¹⁰ per node.
- **The SHMEM pool** (`plan pool`, measured by the plan) is `COMM_SHMEM_POOL_MB=9472` (staging 8192 MiB = 4 × 2048 MiB, the rounds of `COMM_SHMEM_ROUND_MB=1024`; control 894.2 MiB). The heap needs ≥ 9984 MiB.
  - The pool is the same at both sizes, in both switch sets and with the room on or off.
  - The model's 9.93 GB equals it. P24's stage is bounded by the same rounds.
- **The transform cache** (`plan cache`, FIT, measured by the plan) is 0 slots in every case. One slot is 16 GiB per APU = 68.7 GB per node.
  - FIT's room at 5.1 × 10¹³ is 4.88 GB at room 0.16, and 21.39 / 21.25 GB at room 0 (defaults / P24).
  - At 5.55 × 10¹³ it is −20.88 GB at room 0.16, and −1.88 / −2.03 GB at room 0.
  - P24 products do not use the cache anyway.

#### 3. The largest total digits that fits 480 GB (question 3)

The model's scan and bisection (`tests/ms15_candidates.py --maxfit`, `archive:results-raw/MS15/maxfit.txt`) give these ceilings. The node is not monotonic in T: the room's arenas step by 8.59 GB per node, and the code drops the room past its own check.

| MN_P24 | room 0.16 | room 0 | room 0.16 + DKM layout (not built) | room 0 + DKM layout (not built) |
|---|---|---|---|---|
| 2 (with `NEWTON_DKM=1`) | **5.1673 × 10¹³** (471.93 GB; 480.52 GB from 5.168) | **5.5711 × 10¹³** (479.99) | 5.3973 × 10¹³ (471.93) | 5.6959 × 10¹³ (479.99) |
| 0 (defaults) | 5.1699 × 10¹³ | 5.5738 × 10¹³ | 5.4001 × 10¹³ | 5.6989 × 10¹³ |

**The layout confirms the edges** (measured, `archive:results-raw/MS15/edges_room*.txt`, `MN_PLAN_ONLY`'s 16 checks OK):
- with the room, the arena is 300.65 GB at 5.167 × 10¹³ and 309.24 GB at 5.168 × 10¹³;
- without it, the arena is 308.71 GB at 5.571 × 10¹³, the same as the model.

With the room, the node is 480.52 GB from 5.168 to 5.34 × 10¹³. That is only 0.52 GB over, but still over the 480 budget.

The candidate sizes and the steps are in the table at the top.

#### 4. DKM's memory follow-up (question 4)

`mem_model dm_layout(dkm=True)` models a `dm_layout` that follows DKM (not built): the reciprocal to h, the hole at k + 8, and DKM's division blocks.
- **With the room off**, it gives −5.1 GB at 5.1 × 10¹³ (DKM's own figure) and −6.1 GB at 5.55 × 10¹³. The ceiling moves from 5.571 to 5.696 × 10¹³. That does not reach the next piece step: the tree's largest group jumps at 5.74 × 10¹³.
- **With the room on it matters more.** The room is 0.16 × the hole, and DKM shrinks the hole, so the arena drops one or two whole chunks.
  - At 5.1 × 10¹³ the node goes 471.9 → 463.3 GB (−8.6).
  - At 5.55 × 10¹³ it goes 497.7 → 480.5 GB (−17.2), still 0.52 GB over.
  - The ceiling with the room moves from 5.167 to 5.397 × 10¹³.
- **Does it change the answer?**
  - **With the room off, no**: 5.55–5.57 × 10¹³ already fits without it. It would add about 5–6 GB of margin at 5.55 × 10¹³ (472.8 GB), which would make 5.55 × 10¹³ a comfortable size rather than a marginal one.
  - **With the room on, yes, partly**: it would allow up to 5.397 × 10¹³ (for example 5.382 × 10¹³ at 471.9 GB, 313.7 / 361.6 s), but not 5.55 × 10¹³.
  - It is not built. It needs `binsplit.c dm_layout` to follow `NEWTON_DKM`, and the size-1 gates.

#### 5. What changed (commits on `p15-MS`)

| file | change |
|---|---|
| `ecalc/binsplit.c`, `binsplit.h`, `ecalc.c` | `binsplit_layout_comm_size`: `BS_LAYOUT_ONLY` sets `COMM_SIZE` per point (§1.4). Only the layout-only path is touched; a run never calls it. The output is byte-identical without `MN_P24=2` |
| `ecalc/mem_model.py` | P24's scratch branch (off by default); `--check-c` reads `MN_P24` / `ECALC_NP_AUTO_MIN` (§1.5); `import os` |
| `ecalc/mn_model.py` | `memory()` and `run()`'s FIT computation pass `P24` / `NP_AUTO_MIN` to mem_model (2 lines) |
| `tests/p24_model.py` (P24's file) | `design()` on B2's memory: `p15c=True`, room 0.16, `cache_fit=False` (§1.2) |
| `tests/ms15_candidates.py` (new) | the candidate table and `--maxfit` |
| `ecalc/README.md` | one sentence in the `BS_LAYOUT_ONLY` row |
| `archive:results-raw/MS15/` | the layouts, plans, sweeps and model output |

**Gates.** The digits cannot change: only `BS_LAYOUT_ONLY`'s path and the python models are touched, and the layout exits before `rns_init`. The build is clean on aac6 (`make -s -j16`; only the old `newton.h` warning). `mem_model.py --check-c` is exact (§1.5). The default, `MN_P24=1` and room-0 layout outputs are byte-identical to d2d05b3's build. `MN_PLAN_ONLY` already set `COMM_SIZE`, and its path is untouched. The GPU gates (`t_newton`, `t_mul`, e9, 4 × 10¹⁰, mnaccept) were **not run**: no GPU jobs were allowed, and no run path changed.

#### 6. Open issues

1. **`as_room_fits` does not protect 480 GB at the target** (§1.3). Its host term is 31.6 GB against the model's 50.4 GB at init + bs. A size from 5.168 to about 5.63 × 10¹³ keeps the room and goes to 480.5–497.7 GB (modelled). Options for the user:
   - put `BS_ARENA_ROOM=0` on the launch line for a target above 5.167 × 10¹³;
   - or have `as_room_fits` use the model's host (7 + 4.29 + seed buffers + 6 + the SHMEM pool + 6 GB of bs growth).
   The second is not done: it is binsplit.c's run path and needs the gates.
2. **The room's time at size > 1 is unmodelled.** The "room 0" walls assume no division remaps at 576. At size 1 the room saved 14.7 s (measured at 10¹¹). A 2-node measurement with and without the room, at a size where the division would remap, would settle it.
3. The margin at 5.57 × 10¹³ is 0.07 GB (modelled). The model is measured to 0.05 % at size 1, and its size > 1 host terms are measured at 10⁹–10¹⁰ sizes. 5.55 × 10¹³ (1.2 GB margin) or 5.276 × 10¹³ (15.5 GB) are safer.
4. The DKM layout (§4) is not built.
5. `BS_LAYOUT_ONLY` decides the room on three-prime `room_planes` (103.70 GB), while the run uses 120.88 GB at 576 under auto. The layout keeps the room about one chunk longer than a run would. This only matters above 5.6 × 10¹³.

#### RESUME

Done: questions 1–4, the fixes (C layout, mem_model P24, p24_model), the candidate table, and this report. Nothing is running and no job is pending. To reproduce, from `ecalc/`:
- `MN_P24=2 MN_MODEL_DKM=1 MN_MODEL_P24_F=1.025 python3 ../tests/ms15_candidates.py <T…>`
- the same with `--maxfit` (about 3.5 min)
- on aac6 `~/ntt-MS15` (at 2f17d51 + this report): `MN_P24=2 python3 mem_model.py --check-c <layout>`

A successor would continue with open issues 1 and 2, for the user's decision.

## DOC15 — the documentation and the models on the user's decisions of 2026-09-27 (Phase 15, agent DOC)

(former file: DOC15.md)

Branch `p15-DOC` from `int15b` @ c2aedc2 (the worktree's HEAD was 7367e25; the branch was created at c2aedc2). Desk work: the
docs and the Python models; aac6's login node only (clone `~/ntt-DOC15` at c2aedc2, built; scratch `~/DOC15/`), no node job.
Times Eastern. Labels: **measured** (an aac6 run, named), **modelled** (the models' arithmetic), **assumed** (a target parameter
no aac6 run gives).

#### 1. The new 576-node figures (`ecalc/estimate.py --target`, 2026-09-27)

The design: the code's defaults of 2026-09-27 on the target's launch line (internal target notes §4) — **`ECALC_NP=4`**,
`COMM_SHMEM_ROUND_MB=1024`, `ECALC_MEM_GUARD_GB=6`, no top set; the part file **packed** (0.444 B/digit, 32.8 GB per node) and
started at the division's hook (`MN_OUT_EARLY=1`). All **modelled**; the fabric (100 GB/s per APU, 2 µs per message) and 576
nodes writing at once each at its single-stream rate are **assumed**; 0.6–0.8 GB/s is the Lustre single-stream rate
**measured** on a target node (the apumult catalog), 2.0 the old assumption.

| total digits | without the disk write | with it @ 2.0 GB/s | @ 0.8 GB/s | @ 0.6 GB/s | pieces | node memory |
|---|---|---|---|---|---|---|
| **4.25 × 10¹³ (the target)** | **256.0 s (4.27 min)** | 251.5 s (4.19 min) | 272.0 s (4.53 min) | **285.7 s (4.76 min)** | 88 + 66 + 28 = 182 | **416.0 GB** |
| 4.29 × 10¹³ (the last size below the step) | 256.4 s | 251.9 s | 272.7 s | 286.5 s | 182 | 418.1 GB |
| 4.30 × 10¹³ (**the grid step**: tree_max 88 → 100) | 271.0 s (+15.0) | 266.5 s | 287.5 s | 301.3 s | 194 | 418.6 GB |
| 4.40 × 10¹³ (past the second step) | 309.5 s | 304.8 s | 322.8 s | 336.9 s | 226 | 423.7 GB |
| **480-GB ceiling: 5.57 × 10¹³** (9.67 × 10¹⁰ per node) | 411.9 s (6.9 min) | 406.0 s | 426.9 s | 444.8 s (7.4 min) | 166 + 79 + 46 | 479.8 GB |
| 502-GB ceiling: 6.00 × 10¹³ (1.04 × 10¹¹ per node) | 467.0 s (7.8 min) | 460.6 s | 481.5 s | 500.7 s (8.3 min) | 194 + 84 + 52 | 501.5 GB |

- **The standing estimate: 4.25 × 10¹³ digits in 4.27 min without the disk write, 4.53–4.76 min with it at 0.8–0.6 GB/s
  (4.19 min at 2 GB/s), 416 GB per node, 1.2 % below the grid step at 4.29 → 4.30 × 10¹³.** The largest size under 480 GB is
  5.57 × 10¹³ in 6.9 / 7.4 min — past several steps, not recommended.
- By phase (4.25 × 10¹³): init 17.2 + seed wait 10.9 + batch 25.3 + top levels 19.6 + distributed levels 107.0 + reciprocal
  33.5 + division 35.5 + other 0.1 + the digits' residues 4.5 + the process's exit 2.4 = 256.0 s. The part file: 54.7 / 41.0 /
  16.4 s of writing at 0.6 / 0.8 / 2.0 GB/s, 20.5 s of it under the division.
- At 2 GB/s the with-write wall is *below* the no-write one: the whole write hides under the division, and without a digit file
  the code runs the residue pass after T1 (4.5 s) instead of inside the early writer. Not an error of the model: the code does
  that (`mn_early_hook` is set only with an outfile).
- Against the previous figures: P15's option (a) (the Phase 14 defaults on four primes: 277.9 s without the write, 396.3 s
  with the ASCII file at 0.6 GB/s, modelled) → −21.9 s of compute (fill −13.5 s of bs, fast `mul_1` −7.3 s of seed wait,
  middle product + `DIST_TWREC` −3.5 s of the distributed products, +2.4 s of exit now counted) and −88.7 s of exposed write.
  PLAN §36.10's 4.1 min (three primes) is not runnable (the plan check refuses three primes). With the ASCII file
  (`estimate.py --target --ascii`): 267.9 / 323.2 / 354.0 s at 2.0 / 0.8 / 0.6 GB/s.
- Node memory 416.0 GB = max(init 365.6 + host 44.4, bs 371.6 + 44.4, dm 377.9 + 28.8); the SHMEM pool **9472 MiB**
  (`plan pool` with `ECALC_NP=4`, measured on the login node). The fourth prime is +17.2 GB (pool 0); the fill and the early
  writer change nothing that binds at the target.
- The grid step: the code's plan with `ECALC_NP=4` (archive:results_raw_2026-10-19/P15/sweep576_np4.txt; the grids are the same at three and four
  primes) and the model agree (tree_max 88 → 100 at 4.30 × 10¹³).

#### 2. What changed

##### 2.1 The models (commit 5adb2f9)

`ecalc/mem_model.py`:
- **`BS_SEED_FILL` ported** (`seed_terms_for` = `binsplit.c bs_seed_terms_for`; `seed_span`: node 0's range as
  `binsplit_layout_only` uses it). The fill makes the bs regions hold one more batch level: at 4 × 10¹⁰ one node +35.4 GB of
  regions (95.7 → 131.1 GB), at 10¹¹ +18.4 GB (242.9 → 261.2); at the target's share −0.01 GB (the arena there is the dm
  need's, 244.7 GB, unchanged).
- `DEFAULTS15B` (the base of `mem_per_node` now) = `DEFAULTS15` + `seed_fill=128` + `out_early`; `DEFAULTS15` and `OLD13` carry
  `seed_fill=0` (B0 and before). `TARGET_NP = 4`.
- `VMM_DM_GROW_FILL = 16.3 GB` (**measured**, RESULTS §86's ten fill runs: device 377.3 GB at init, 393.6 at dm — the division
  grows the block pool by 12.9 GB with the fill, 2.1 without); applied at size 1 only (assumed 0 at size > 1).
- `HOST_EARLY = 0.9 GB` (modelled from results/IO15.md §6) on the host at dm at size > 1 with `MN_OUT_EARLY`.
- `--check-c FILE [POOL_LOG [MN_T_CHUNK_MB [BS_SEED_FILL]]]` (fill default 128); `--p15` prints B0's rows as `[B0]`, the new
  1e11 row, the target on four primes (and three, B0 for comparison), the ceilings on four primes.

`ecalc/mn_model.py`:
- `Design` gains `p15b` (the decisions of 2026-09-27), `np_mn` (ECALC_NP at size > 1; `at_g()` resolves it in `run`, `memory`,
  `max_digits`, `plan`), `packed`, `early`. `DEFAULT15B()` = np 3, np_mn 4, the p15 forms, p15b; **`DEFAULT` is now
  `DEFAULT15B()`**; `--model p15b` (default) / `p15` (B0) / `p13` / `legacy`; `--ascii`.
- The terms (each labelled in the code, above `CAL15_RUNS`):

| term | value | what | label |
|---|---|---|---|
| `FILL_BS` | 0.7693 | bs without the seed wait × this (`BS_SEED_FILL`) | measured at 10¹¹ (cand / B0, 56.39 / 73.30 s, 4 + 4 runs); assumed at the target's leaf |
| `SEED15B` | 7.589 + 0.2681 s per 10⁹ span digits, no ×1.29 above 2³³ | the seeds' end (`BI_MUL1_FAST`) | fitted on the ten cand runs (seeds end 34.40 s at 10¹¹) |
| middle product | Q_t·r with a high cut at take + 4 after the first step of each group | `NEWTON_RECIP_MID` in `recip_cost` | modelled; recip pieces 67 → 66 = the C plan |
| `TWREC_F` | 0.98 on the pieces' local passes | `DIST_TWREC` | modelled from the measured −1.8 s of dm at 10¹¹ (C215 §2); assumed to carry to the mn tier |
| `P15B_RECIP1`, `P15B_DIV1` | 0.9097, 1.0851 | size-1 reciprocal and division (MID, C2, TWREC, the fill's remaps) | measured ratios at 10¹¹; size 1 only |
| `PACKED_BPD` | 8/18 | bytes per digit of the part file | exact |
| early writer | exposed = max(0, max(write, residues) − OVL1 × division), OVL1 = 0.577 | `MN_OUT_EARLY` at size > 1 | OVL1 fitted at size 1, assumed at size > 1 |
| corrections | no T1 wait, no rewrite | `ECALC_CORR_PATCH=2` | from K15 |
| `EXIT_S` | 2.4 s on both walls | the process's exit (elapsed − `total` − dc) | measured, 2.15–2.64 s in every §86 run |
| C2 | 0 at the target | `RNS_AUTO_PIECE_COST` | modelled by C215 (the top node's leaf grids unchanged) |

- `--calib15b`: the model against RESULTS §86's paired 10¹¹ series (below). `Design.env()` prints `ECALC_NP=np_mn`.

`ecalc/estimate.py`: the default design is DEFAULT15B (`--np 3 --np-mn 4`), `--ascii`, `--b0` (the Phase 14 defaults); `--target`
prints the packed / early part file and the exit. `ecalc/design_table.py`: every row on the decisions (`P15B`; `--b0` for the
table of 2026-09-26), four primes in the 576 columns, the header and the write column's text.

##### 2.2 The calibration (`mn_model.py --calib15b`; measured = RESULTS §86, jobs 21550 / 21563, logs `~/fin15/` on aac6)

| arm | file | node | n | `total` measured | model | err | wall measured | model | err |
|---|---|---|---|---|---|---|---|---|---|
| B0 | yes | s24-16 | 3 | 238.0 | 238.5 | +0.2 % | 296.9 | 300.5 | +1.2 % |
| B0 | no | s24-26 | 4 | 210.2 | 214.3 | +2.0 % | 217.6 | 222.1 | +2.1 % |
| the defaults of 2026-09-26 (C2) | yes / no | | 3 / 4 | 214.6 / 203.8 | — | — | 239.4 / 205.9 | — | — (the model has no size-1 C2 term) |
| every candidate (= today's defaults), ASCII | yes | s24-16 | 3 | 200.8 | 195.1 | −2.8 % | 224.7 | 218.8 | −2.6 % |
| every candidate | no | s24-26 | 4 | 191.3 | 195.1 | +2.0 % | 193.6 | 197.5 | +2.0 % |
| every candidate, packed | yes | s24-16 | 3 | 201.1 | 195.1 | −3.0 % | **203.7** | 197.5 | −3.1 % |

Worst wall error 3.1 %. s24-16 runs ≈ 9 s (≈ 5 %) slower than s24-26 in this series (`total` 200.8 against 191.3 for the same
arm); the model sits between them. B0's modelled walls add `EXIT_S` for the comparison. The Phase 14 check (`--calib15`) is
unchanged (worst `total` error 4.2 %).

Memory at 10¹¹ on one node with the fill (measured, the same series): device 377.3 GB at init (model 377.3), 393.6 at dm
(model 393.6); host HWM 27.4 (model 27.1). The model's node peak 410.4 GB against the report's device max + host HWM 421.0:
the host HWM is at init and the device max at dm, so the report's sum is an upper bound; the model counts the host at dm.

##### 2.3 `mem_model.py --check-c` (exact; the protocol's gate)

`BS_LAYOUT_ONLY` on aac6's login node at c2aedc2 (files in `archive:results_raw_2026-10-19/DOC15/`), then `python3 mem_model.py --check-c <file>`:

| run | result |
|---|---|
| `ECALC_NP=4 BS_LAYOUT_ONLY=4e10:1,1e11:1,7.3784722222e10:576 ./ecalc 42500000000000 /dev/null` (layout_np4.txt; fill 128, the default) | **0 terms not exact**, largest arena difference 0.0000 % (4e10 131 080 388 608 / 139 993 284 608 B of regions / arena; 1e11 261 238 030 336 / 273 607 032 832; the target share 185 207 881 728 / 244 737 638 400); `plan check … OK` |
| `ECALC_NP=4 BS_LAYOUT_ONLY=73784722222.2222:576 …` (layout_target_np4.txt, the exact share: d = 42 500 000 000 016) | 0 not exact; the C layout's node total at 2³¹: 387.21 GB |
| `ECALC_NP=3 BS_LAYOUT_ONLY=4e10:1,1e11:1 …` (layout_np3.txt) | 0 not exact |
| `BS_SEED_FILL=0 ECALC_NP=4 …` (layout_fill0.txt), `--check-c <file> 31 1024 0` | 0 not exact |

Before the port the fill's layouts were off by −27.0 % (4e10) and −7.0 % (1e11) of the regions.

`ECALC_NP=4 MN_PLAN_ONLY=4.25e13:576 ./ecalc` (plan576_np4.txt) and with `COMM_SHMEM_ROUND_MB=1024` (plan576_np4_r1024.txt):
`plan summary … pieces tree 86 recip 66 div 28 total 180 | … largest group 88, total 182`; `plan check 4.25e+13 digits g 576,
ECALC_NP=4: OK -- 1240 products`; `plan pool … 43008` (9472 with the rounds).

##### 2.4 The documents (commits ee42962, 983e545, a00a58b, 3173e4c, 8a17bb2)

- `ecalc/README.md`: the header's defaults list (Phase 13c, Phase 14's nine, the decisions of 2026-09-27, the launch-line
  settings, RL not adopted, D3); every decided row shows its new default and the date (`ECALC_CORR_PATCH` 2, `NEWTON_RECIP_MID`
  1, `BS_SEED_FILL` 128 with the `BS_SEED_TERMS` rule, `BI_MUL1_FAST` 1, `ECALC_OUT_PACKED` 1, `ECALC_ODIRECT` auto in both rows,
  `RNS_AUTO_PIECE_COST` 1, `ECALC_CKPT_TOP` off + the `ECALC_CHECKPOINT` rule, `MN_OUT_STRIPE` / `MN_OUT_WAVES` "no default —
  measure on the target", `ECALC_MEM_GUARD_GB` 6 on the launch line, `COMM_SHMEM_ROUND_MB` on the launch line, `MN_T_CHUNK_MB`
  1024 kept — test 0 vs 1024); **new rows** `ECALC_CHECKPOINT`, `MN_OUT_EARLY`, `DIST_TWREC`, `ECALC_NP` (none existed);
  `digcmp.sh` and `tools/unpack_digits` in the quick start, the part-file paragraph and mnaccept's description; the recheck
  paragraph (the top set no longer on by default; the residue form).
- internal target notes: §1 a new dated block with the figures of §1 here (history kept); §3 rows (`ECALC_NP` 4, `MN_OUT_EARLY`,
  `ECALC_OUT_PACKED`, `ECALC_ODIRECT`, stripes / waves, `ECALC_MEM_GUARD_GB` 6, `ECALC_CKPT_TOP` off / `ECALC_CHECKPOINT=1`,
  `BS_CKPT_DIR` development only, `MN_T_CHUNK_MB`, the pool with four primes, RECHECK); §4 the launch line (`ECALC_NP=4`,
  `ECALC_MEM_GUARD_GB=6`, `unset ECALC_CHECKPOINT ECALC_CKPT_TOP BS_CKPT_DIR`, the output on Lustre), the packed part files,
  **"Off the clock: verify, convert, verify the converted output"** (RECHECK of the packed parts; `tools/unpack_digits` with its
  time; RECHECK of the ASCII parts; the leading 10¹¹ digits against `e_1e11.out`), the defaults paragraph; §5 `digcmp.sh`,
  `ECALC_CHECKPOINT=1` in steps 1–5 and 7 and not in 6, every step's estimate, step 6's `plan check … OK`, step 6b, step 7's
  new ceiling; §6 item 4 `MN_T_CHUNK_MB` 0 vs 1024, item 5 the estimate and the explicit striping / waves reminders, (d) with
  `MN_OUT_EARLY` the default, item 7 the top set; §7 the read-back and conversion times; §8 traps 12 (the converter) and 13
  (three primes refused).
- internal target task list: a dated standing-estimate bullet; T2 with `digcmp.sh` and `ECALC_CHECKPOINT=1`; **T3's pass criterion
  `plan check … OK` with `ECALC_NP=4`** (182 pieces, the layout command with the exact share); T4 without the top set, both
  walls; **T4b** the off-the-clock conversion and verification; **T10** measure striping and waves (decision 12); **T11**
  `MN_T_CHUNK_MB` 0 vs 1024 after the per-round cost (decision 13); the rules (two walls, `ECALC_CHECKPOINT=1` for development).
- `results/DESIGN_TABLE.md` regenerated (96 rows, 217 s; ee42962): the recommended row (auto, 2³¹, both chunks, depth 2) at the
  target 4.27 / 4.76 min, 5.57 × 10¹³ at 480 GB; the fastest row (chunking off) 3.98 / 4.52 min but only 4.07 × 10¹³ at 480 GB.
- internal apumult study: a dated note (the estimate, four primes); the text kept.

#### 3. Tests (all desk / login node)

| command | result |
|---|---|
| `python3 mem_model.py --check-c archive:results_raw_2026-10-19/DOC15/layout_{np4,np3,target_np4}.txt`; `… layout_fill0.txt 31 1024 0` | 0 terms not exact in every file (measured C layouts, modelled port) |
| `python3 mn_model.py --calib15b` | worst wall error 3.1 % (§2.2) |
| `python3 mn_model.py --calib15` | unchanged (B0 model) |
| `python3 mem_model.py --p15` | the target on four primes 416.0 GB; B0's rows unchanged |
| `python3 estimate.py --target` / `--ascii` / `--b0` | §1; `--b0` reproduces MD15's 231.1 / 349.6 s, 398.8 GB (three primes) |
| `python3 mn_model.py --D 7.3784722e10 --g 576 --groups 2,4,8,16,32,64,192,576` | 256.0 / 285.7 s, 182 pieces (tree_max 88, recip 66, div 28), 416 GB |
| `python3 design_table.py` | 96 rows in 217 s; its recommended row equals `estimate.py --target` |
| `python3 estimate.py --D 4e10 1e11 --g 1 4 576` | runs; one node at 10¹¹ 197.5 s without the file (measured 193.6) |
| TARGET §9's variable check | every variable named in TARGET.md is in the code or named as a model constant / not merged (`DB_POOL_VMM_PAR`, `_EXTEND`) |

#### 4. Open issues (for the integrator)

1. **`tools/unpack_digits` cannot convert one part** (found reading the tool at c2aedc2): it requires the complete set (part
   0 to limb 0) and stops with "is part k of 576; 1 files given". The user's decision 7 wants the conversion "on the nodes in
   parallel": that needs a one-part mode (a flag that skips the join checks for a single part and writes its ASCII slice —
   "2." only on part 0000, the newline only on the last part — so that `cat e.txt.part*` is the single file; the residue check
   per part already exists). Not mine to build (tools/ is not in my files). Until then the conversion is one stream: 42.5 TB at
   ≈ 0.6 GB/s ≈ 20 h (modelled). TARGET §4, §5 step 6b, §8 trap 12 and TARGET_TASKS T4b say so.
2. The fill's +4.9 s (net) of division remaps at 10¹¹ on one node is modelled as a size-1 effect only (at size > 1 the arena
   is the dm need's and the fill does not change it: assumed no remaps). A 2-node 10¹⁰ pair with and without the fill would
   check it.
3. `VMM_DM_GROW_FILL` (16.3 GB) is measured at 10¹¹ only and applied at every size-1 D; at 4 × 10¹⁰ the one-node peak is now
   modelled at 275.5 GB (unmeasured with the fill).
4. `EXIT_S` is new in the walls: the 576-node process exit (freeing ≈ 400 GB) is assumed to take what one node's did (2.4 s).
5. `PLAN.md` §36.10 and RESULTS §86's estimate line are the integrator's to update (the figures of §1).

#### RESUME

- Done: branch `p15-DOC` at c2aedc2 + 5adb2f9 (models), ee42962 (README), 983e545 (TARGET), a00a58b (TARGET_TASKS), 3173e4c
  (DESIGN_TABLE), 8a17bb2 (APUMULT_STUDY), this report. aac6 `~/ntt-DOC15` at c2aedc2 (built), `~/DOC15/` the login-node
  outputs (copied to `archive:results_raw_2026-10-19/DOC15/`). No job was run or is pending.
- Nothing left in the task list; the open issues of §4 are for the integrator / the user.

## DOC215 — the documentation and the models on the user's decisions of 2026-09-28 (Phase 15, agent DOC2)

(former file: DOC215.md)

Branch `p15-DOC2` from `int15g` @ be2eec3 (the worktree's HEAD was e107cd2, main; the branch was created at be2eec3). Desk work: the
docs and the Python models; aac6's login node only (the plan / layout runs against `~/ntt-int15g`, built at be2eec3, used read-only;
scratch `~/DOC215/`, copied to `archive:results-raw/DOC215/`); no node job. Times Eastern. Labels: **measured** (an aac6 run, named), **modelled**
(the models' arithmetic), **assumed** (a target parameter no aac6 run gives: the fabric's 100 GB/s per APU and 2 µs per message, 576
nodes writing at once each at its single-stream rate).

#### The standing 576-node figures (`ecalc/estimate.py --target`, 2026-09-28 18:01 EDT; archive:results-raw/DOC215/estimate_target.txt)

The design: main B2's defaults (the decisions of 2026-09-27 + `BS_ARENA_ROOM=0.16`, `DIST_TWREC_G=1`, `RNS_POOL1_4Q=1`) on the target's
launch line (internal target notes §4): **`ECALC_NP=auto`**, **`RNS_DIST_CACHE_FIT=1`** (the mn transform cache at the slots the code's budget
rule allows: **0** at 5.1 × 10¹³ and 4.74 × 10¹³), `COMM_SHMEM_ROUND_MB=1024`, `ECALC_MEM_GUARD_GB=6`, `MN_GROUPS=2,4,8,16,32,64,192,576`;
the part file packed from the division's hook. All **modelled**; the pieces are the C plan's (**measured**, login node).

| total digits | without the disk write | with it @ 2.0 GB/s | @ 0.8 GB/s | @ 0.6 GB/s | pieces node 0 / critical path | node memory |
|---|---|---|---|---|---|---|
| **5.10 × 10¹³ (the target)** | **400.0 s (6.67 min)** | 394.6 s (6.58 min) | 404.1 s (6.73 min) | **420.5 s (7.01 min)** | 222 / 242 (128 + 74 + 40) | **471.9 GB** |
| 5.11 × 10¹³ (node 0 steps) | 400.5 s | 395.1 s | 404.7 s | 421.1 s | 226 / 242 | 471.9 GB |
| 5.12 × 10¹³ (the step) | 407.1 s (+7.1) | 401.6 s | 411.3 s | 427.8 s | 230 / 246 | 471.9 GB |
| 5.17 × 10¹³ (the next step) | 436.1 s | 430.6 s | 440.8 s | 457.4 s | 230 / 266 | 480.5 GB (over 480) |
| **4.74 × 10¹³ (one step below)** | **368.7 s (6.14 min)** | 363.7 s | 375.4 s | **390.7 s (6.51 min)** | 202 / 226 (124 + 68 + 34) | 454.8 GB |
| 4.75 × 10¹³ (its step) | 380.5 s (+11.8) | 375.5 s | 385.5 s | 400.8 s | 209 / 233 | 454.8 GB |
| 480-GB ceiling: 5.167 × 10¹³ (8.97 × 10¹⁰ per node) | 435.7 s (7.3 min) | 430.2 s | 440.4 s | 457.0 s (7.6 min) | 152 + 74 + 40 | 471.9 GB |
| 502-GB ceiling: 5.892 × 10¹³ (1.02 × 10¹¹ per node) | 525.0 s (8.8 min) | 518.7 s | 528.9 s | 547.8 s (9.1 min) | 184 + 82 + 47 | 497.7 GB |

- **The standing estimate: 5.1 × 10¹³ digits in 6.67 min without the disk write, 6.73–7.01 min with it at 0.8–0.6 GB/s (6.58 min at
  2 GB/s), 471.9 GB per node — 8.1 GB below 480, 1.3 % below the memory ceiling.** 4.74 × 10¹³: 6.14 / 6.51 min, 454.8 GB.
- By phase at 5.1 × 10¹³: init 20.1 + seed wait 12.1 + batch 24.2 + top levels 27.0 + distributed levels 185.9 + reciprocal 54.0 +
  division 68.8 + other 0.2 + residues 5.4 + exit 2.4 = 400.0 s; the part file 39.35 GB per node, 39.7 s of it under the division.
- Node memory 471.9 GB = max(init 421.5 + host 44.4, **bs 427.5 + 44.4**, dm 429.8 + 28.8); arena 300.6, planes 120.9 (pool 0 at four
  planes, pool 1 at three); SHMEM pool 9472 MiB (**measured**, `plan pool`); the mn cache 0.
- The steps before the headline (TARGET §5, modelled; FIT allows both slots there and the model prices them): step 3 (64 × 10¹⁰) 36.0 /
  40.6 s; step 4 (576, 10¹²) 17.0 / 17.8 s; step 5 (576, 2.2 × 10¹³) 121.5 / 139.4 s, 334.5 GB (+137.4 GB of slots within FIT's room).
- Beside it (modelled, same launch line): the cache at 1 / 2 slots forced 350.3 / 325.7 s without the write (379.0 / 356.2 s with it),
  node 540.7 / 609.4 GB — neither fits; `ECALC_NP=4` instead of auto 423.3 / 443.8 s and **489.1 GB (over 480)**; ASCII instead of packed
  399.2 / 465.6 / 502.5 s at 2.0 / 0.8 / 0.6 GB/s; `MN_T_CHUNK_MB=0` 380.1 / 402.9 s at 497.7 GB (over 480).

**Against the figures the prompt carried**: the integrator's RESULTS §88 table (407.1 / 426.2 s at 0 slots, `auto`) is reproduced within
0.3 s without `DIST_TWREC_G` (406.8 / 426.0 s, CX's refit); `DIST_TWREC_G`'s modelled −6.8 s gives 400.0 / 420.5 s. **The node memory
is 471.9 GB, not ≈ 464 GB**: the arena room costs **+16.5 GB** per node at the target's share (284.15 → 300.65 GB of arena in the C
layout, measured), not the ≈ 8.6–9 GB quoted (that is closer to the one-node share's). See §2.2.

#### 1. What changed

##### 1.1 The models (commit 5f05796)

`ecalc/mem_model.py`:
- **`DEFAULTS15C`** = `DEFAULTS15B` + `arena_room=ARENA_ROOM` (**0.16**, the code's default since be2eec3) — the base of `mem_per_node`
  now; `DEFAULTS15B` (B1), `DEFAULTS15` (B0), `OLD13` carry `arena_room=0.0` explicitly (their measured rows are unchanged; `--p15` tags
  each measured row [B0] / [B1] and computes it on its own defaults).
- **`TARGET_NP = 'auto'`** (was 4; `report15` / `cache_report` print it as a string, the cache slot from pool 0's planes).
- **The FIT rule as the code computes it** (`rns_dist.c rns_dist_cache_plan`): `cache_fit_slots` = ⌊(480 GB − the layout node −
  `RNS_DIST_CACHE_FIT_RESERVE_GB` 32) / one slot⌋ capped at `RNS_DIST_CACHE_MN` (2), the layout node = planes + arena + `BS_HOST_INIT_BYTES`
  + 6 GB at size > 1 (`binsplit_node_bytes`). Before it was 480 − this model's peak. At 5.1 × 10¹³: room 4.88 GB, 0 slots — the same as
  the C plan's `plan cache` line (measured). New keys: `cache_slot`, `cache_fit_room`, `layout_node`, `arena_room`.
- `--check-c`: `BS_ARENA_ROOM` defaults to 0.16; `ECALC_NP=auto` is read from the file's `plan` lines (its `planes:` lines print np 3).
- `--p15`: the target block on the launch line with rows for `BS_ARENA_ROOM=0`, `ECALC_NP=4`, `ECALC_NP=4 BS_ARENA_ROOM=0` (B1's line).

`ecalc/mn_model.py`:
- **`DEFAULT15C()`** (the default `DEFAULT`, `--model p15c`): `Design(p15c=True)` = p15b + `arena_room` 0.16 (to `mem_opts`), pool 1 at
  the one-node primes (`RNS_POOL1_4Q=1`; `mem_opts` sets `pool1_np=None` — the time model's memory path had carried OLD13's pool 1 at
  3 q + 16, which hid `ECALC_NP=4`'s +17.2 GB), `np_mn='auto'`, **`cache_fit`**. `DEFAULT15B()` is B1 (`np_mn=4`, no room, the code's 2
  slots as priced before TC / CX).
- **The cache under FIT**: `run()` sets `CACHE_RUN_SLOTS = min(CACHE_MN_SLOTS, mem_model's cache_fit_slots)` for a `cache_fit` design at
  g > 1; `CACHE_FORCE` (`--cache-slots n`) prices n slots whatever FIT allows; the run's `mem` gets `cache`, `node_cache`, `cache_fits`.
- **`TWREC_G = True`** (the code's default since be2eec3; `--no-twrec-g`).
- **`P15C_DIV1 = 53.50 / 68.15 = 0.785`**: size 1 only, the division with the room (no remaps), **measured** in the fin15e series (job
  21670, s24-16, 2026-09-28 04:59–05:44 EDT; 6 + 6 runs: B2's switches 53.12–54.12 s, B1 67.14–69.19 s of `dm − recip`; the reciprocal
  37.80 vs 38.27 s, not applied); **assumed** at other D; no term at size > 1 (none existed to remove).
- **`--calib15c`** (`CAL15C_RUNS`, the same series): below.
- `cache_rooms` / `--cache-proposals` on B2's memory (the rooms with the arena room).

`ecalc/estimate.py`: default design `DEFAULT15C` (`--np-mn auto`), `--b1`, `--room F`, `--no-twrec-g`, `--cache-slots n`, `--no-cache-fit`;
`--target` prints the slots per row (and their bytes when forced) and a block with the cache at 0 / 1 / 2 slots and `ECALC_NP=4` at
5.1 and 4.74 × 10¹³. `estimate.py --target --b1` reproduces the int15g model's B1 figure (349.4 / 378.8 s, 455.4 GB).

`ecalc/design_table.py`: `P15C` (every row on B2 + the launch line: `np_mn` auto, the room, FIT, `TWREC_G`); `--b1`; the memo key carries
`p15c` and the room; the header text.

##### 1.2 The documents

- `ecalc/README.md` (7bfb5d5): the header's defaults list (Batch 2: `BS_ARENA_ROOM=0.16`, `DIST_TWREC_G=1`, `RNS_POOL1_4Q=1`; the launch
  line `ECALC_NP=auto`, `RNS_DIST_CACHE_FIT=1`; `NTT_R3_FUSE` / `RNS_R3_MINK` off; E11 / `DM_BAND` and `DB_POOL_VMM_STREAM` dropped,
  `ECALC_INIT_TL` kept; the target stays 5.1 × 10¹³). Rows: `ECALC_NP` (auto on the launch line; 4 now over 480), `ECALC_NP=auto`
  (the target's plan and figures), `RNS_POOL1_4Q` (stays), `RNS_DIST_CACHE_MN` (**the default's harm without FIT**, CX15 / TC15: +37.5 %
  at 10¹⁰, OOM at 4 procs, ≈ 609 GB at the target), `RNS_DIST_CACHE_FIT` (on the launch line; 0 slots, room 4.88 GB), `BS_ARENA_ROOM`
  (the integrator's default mark kept, + the measured 10¹¹ effect and the +16.5 GB at the target), `DIST_TWREC_G` (the mark kept, −6.8 s).
- internal target notes (5b803c3, 954e6f4): header; **§1 a new dated block** (the table above, why it is longer than
  yesterday's 344.7 s, the step margin, the plan check with `plan primes` / `plan cache`, by phase, the memory and the settings that
  decide the fit, the cache at 1 / 2 slots, `ECALC_NP=4`, the top node's rehearsal, the steps); the TGT block marked history; §3 rows
  `RNS_DIST_CACHE_MN` / new `RNS_DIST_CACHE_FIT`, `ECALC_NP`, `COMM_SHMEM_POOL_MB`, new `BS_ARENA_ROOM`, `DIST_TWREC_G`, `RNS_POOL1_4Q`;
  **§4 the launch line** (`export ECALC_NP=auto`, `export RNS_DIST_CACHE_FIT=1`) and the defaults paragraph; §5 steps 3–7 (estimates,
  step 6's `plan check … ECALC_NP=auto: OK` / `plan primes` / `plan cache … -> 0 slots` criteria, step 7's new ceiling 5.167 × 10¹³);
  §6 items 4 and 5; §8 trap 13 (do not go back to `ECALC_NP=4`: +17.2 GB, 489.1 GB) and **new trap 15** (never without FIT or
  `RNS_DIST_CACHE_MN=0`). The §9 variable check: only the known non-code names (model constants, library variables, and the dropped
  `DB_POOL_VMM_STREAM` / `DM_BAND`, named as not in the code).
- internal target task list (3a8a658): a dated "where things stand" bullet; T0 (pool unchanged with auto), **T3** (the launch line's plan
  command with `ECALC_NP=auto RNS_DIST_CACHE_FIT=1`, the pass criteria: `plan check … auto: OK`, `plan primes`, `plan cache … 0 slots`,
  222 / 242, layout 443.12 / model 471.9 GB; 4.74 × 10¹³ likewise), T4 (FIT on the line; 400.0 / 420.5 s, 471.9 GB), T4c (368.7 /
  390.7 s, 454.8 GB, −31.3 s per step), **T7 answered** by TC / CX (FIT on the launch line; what remains: measure on the target whether
  a slot fits — the `transform cache:` line at steps 3, 5, 6, a FIT vs `RNS_DIST_CACHE_MN=0` pair at step 5), T11 (497.7 GB).
- `results/DESIGN_TABLE.md` regenerated (5b803c3; 96 rows, 229 s): the recommended row (auto / 2³¹ / both / d2) equals `estimate.py --target`
  (6.67 / 7.01 min at 5.1 × 10¹³, 5.17 × 10¹³ at 480 GB); the fastest row (chunking `shift`) 6.34 min but only 4.55 × 10¹³ at 480 GB.

#### 2. Tests (desk and login node; commands from `ecalc/` unless noted)

##### 2.1 Login-node runs (measured; `~/DOC215/doc215_login.sh`, 2026-09-28 17:46 EDT, `~/ntt-int15g` at be2eec3)

Environment `LL = COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 ECALC_MEM_GUARD_GB=6 RNS_DIST_CACHE_FIT=1`.

| run | result |
|---|---|
| `ECALC_NP=auto $LL MN_PLAN_ONLY=51000000000000:576 ./ecalc` (plan_51e13_auto.txt) | rc 0; `plan summary` 222 / 242; **`plan primes` tree 76 of 108, recip 35 of 74, div 40 of 40 at four primes; leaf dist_db 0 of 12, recip chain 0 of 30**; `plan cache … FIT … room 4.88 GB … -> 0 slots`, node 443.12 GB (default: 2 slots → 580.55 GB); **`plan check 5.1e+13 digits g 576, ECALC_NP=auto: OK -- 1240 products`** (151 of 264 pieces over the bound); pool 9472 MiB |
| the same at 4.74 × 10¹³ (plan_474e13_auto.txt) | 202 / 226; `plan primes` 68 / 29 / 34; FIT room 22.06 GB → 0 slots; check OK (131 of 244); pool 9472 |
| `ECALC_NP=4 $LL …5.1e13` (plan_51e13_np4.txt) | 222 / 242, check OK, pool 9472; layout node 460.29 GB in the plan cache line |
| without `RNS_DIST_CACHE_FIT` (plan_51e13_auto_nofit.txt) | `RNS_DIST_CACHE_FIT=0`: the default's 2 slots, node 443.12 → 580.55 GB |
| the sweep 4.70–5.17 × 10¹³ with auto (sweep_auto.txt) | the same piece counts and steps as TGT's `ECALC_NP=4` sweep (5.10 last at 222 / 242; 5.11 → 226 / 242; 5.12 → 230 / 246; 4.74 → 4.75: 202 / 226 → 209 / 233) |
| `ECALC_NP=auto $LL BS_LAYOUT_ONLY=4e10:1,1e11:1,88541666666.6667:576,82291666666.6667:576,91694091804:1 ./ecalc 51000000000000 /dev/null` (layout_auto.txt; the room 0.16 = the default) | the target share: planes 120.88 + arena 300.65 = device 421.53, **node 443.12 GB**; 4.74 × 10¹³: 120.88 + 283.47 = 404.35, node 425.94; the top node 9.169 × 10¹⁰ at size 1: 103.70 + 268.44 = 372.13, node 387.72 |
| the same with `BS_ARENA_ROOM=0` (layout_auto_room0.txt) | target arena 284.15 (node 426.61); 4.74 × 10¹³ 267.95 (410.42) → **the room: +16.50 / +15.52 GB** |
| `ECALC_NP=4` (layout_np4.txt), `ECALC_NP=3` (layout_np3.txt) | np 4: planes 138.05, node 460.29 (target), 443.11 (4.74 × 10¹³) |

##### 2.2 `mem_model.py --check-c` (the protocol's gate; archive:results-raw/DOC215/check_c.txt)

| command | result |
|---|---|
| `python3 mem_model.py --check-c ../archive:results-raw/DOC215/layout_auto.txt` (defaults: room 0.16, auto read from the file) | **0 terms not exact**, largest arena difference 0.0000 %, 20 plane figures exact (4e10, 1e11, the 5.1 × 10¹³ and 4.74 × 10¹³ shares, 9.169 × 10¹⁰) |
| `… layout_np4.txt`, `… layout_np3.txt` | 0 not exact (20 / 12 plane figures) |
| `… layout_auto_room0.txt 31 1024 128 0` | 0 not exact |

Why ≈ 464 GB was low: the room adds 0.16 × the hole (19.98 GB per device at the target share: 3.20 GB) to each device's dm need, and the
arenas are laid out in whole 2 GiB VMM chunks; 4 devices × (3.20 + the rounding) = 16.50 GB of arena (measured by the C layout, exact in
the port). The node is then bs 427.5 + host 44.4 = **471.9 GB** (modelled). `binsplit.c as_room_fits` keeps the room (planes + arena +
31.6 GB = 453.1 ≤ 480). With `ECALC_NP=4` the node is 489.1 GB: **over 480**, and its as_room_fits (470.3 ≤ 480) keeps the room too.

##### 2.3 The models

| command | result |
|---|---|
| `python3 mn_model.py --calib15c` (archive:results-raw/DOC215/calib15c.txt) | B1 no file: `total` 200.9 measured / 195.1 model (−2.9 %), wall 207.7 / 197.5 (−4.9 %: B1's first run, 230.3 s elapsed, is in the mean); **B2 no file: 185.1 / 181.4 (−2.0 %), wall 187.8 / 183.8 (−2.1 %)**; B1 with the file wall 205.2 / 197.5 (−3.8 %); B2 with the file 191.3 / 183.8 (−3.9 %). dm modelled 102.2 → 88.6 s against measured 106.4 → 91.3 (s24-16 runs ≈ 4 % slow, as in RESULTS §86) |
| `python3 mn_model.py --calib15b`, `--calib15` | unchanged (worst wall 3.1 %; `total` 4.2 %) |
| `python3 mem_model.py --p15` (mem_p15.txt) | the measured [B0] / [B1] rows unchanged; the target on the launch line 471.9 GB; `BS_ARENA_ROOM=0` 455.4; `ECALC_NP=4` 489.1; B1's line 472.6 (matches RESULTS §88's 472.6); the ceilings 5.167 / 5.892 × 10¹³ at 480 / 502 GB |
| `python3 mem_model.py --cache` (mem_cache.txt) | 5.1 × 10¹³: FIT 0 slots (room 480 − 443.1 − 32 = 4.9 GB < 68.7); the default's 2 slots → 609.4 GB; 4.74 × 10¹³: room 22.1 GB, 0 slots |
| `python3 estimate.py --target` (estimate_target.txt, 52 s) | the table at the top |
| `python3 estimate.py --target --b1` (estimate_target_b1.txt) | 349.4 / 378.8 s, 455.4 GB (+ 137.4 GB of slots: 592.9, "does not fit") = the int15g model's B1 |
| `python3 mn_model.py --cache-proposals` (cache_proposals.txt) | auto on B2's memory: rooms 44.5 GB in the tree, **21.5 GB in the division** (34.2 before the arena room); CX's proposals that fit: 1 prime −12.8 s, tree 2 primes + division 1 prime −20.3 s, with the longer axis **−26.3 s** (the best that fits, as CX's −26.7) |
| `python3 design_table.py` | 96 rows, 229 s; the recommended row = `estimate.py --target` |
| `python3 mem_model.py`, `--pool`; `mn_model.py --D 8.854e10 --g 576 --groups …`; `estimate.py --max --g 576`; `estimate.py --D 4e10 1e11 --g 1 4 576` | run; consistent (400.2 / 420.7 s via `mn_model`, the same design) |
| TARGET §9's variable check | only the known non-code names |

#### 3. Open issues (for the integrator)

1. **The node figure is 471.9 GB, not ≈ 464** (RESULTS §88's "`BS_ARENA_ROOM` adds ≈ 8.6 GB" is the one-node share's order; at the target
   share it is +16.5 GB, measured by `BS_LAYOUT_ONLY` and exact in the port). The target is 8.1 GB below 480 and 1.3 % below the memory
   ceiling (5.167 × 10¹³). PLAN / RESULTS are the integrator's to correct.
2. **`ECALC_NP=4` no longer fits 480 GB at the target** with the room (489.1 GB modelled; its layout node 460.29 GB passes the code's own
   budget check, so nothing would stop it before the run). TARGET trap 13 says so.
3. **The partial cache (Batch 3)** must not use FIT's reserve rule as is: FIT's room at the target is 4.88 GB (480 − the layout node −
   32 GB), while CX's proposals draw on 44.5 GB (tree) / 21.5 GB (division) by the model's phase peaks. The division's room shrank from
   34.2 to 21.5 GB with the arena room; a one-prime slot (17.2 GB) still fits it (modelled).
4. The room's time at size > 1 is not modelled (0): the fill's division remaps were a size-1 effect in the model already; if the target's
   division remaps without the room, the room gains there too (unmeasured).
5. `P15C_DIV1` (0.785) is measured at 10¹¹ only; the model's B2 wall is −2.1 % against s24-16's series.
6. At the development steps (3–5) FIT allows both slots and the model prices them (e.g. step 5: 121.5 s with 2 slots); whether the default
   `RNS_DIST_CACHE_MARGIN_GB` / `hipMemGetInfo` path then agrees on the target is T7's measurement.

#### RESUME

- Done: branch `p15-DOC2` at be2eec3 + 5f05796 (models + login-node files), 7bfb5d5 (README), 5b803c3 (TARGET §1/§3/§4 +
  DESIGN_TABLE), 954e6f4 (TARGET §5/§6/§8), 3a8a658 (TARGET_TASKS), this report. aac6: `~/DOC215/` (the login-node outputs and
  the script; copied to `archive:results-raw/DOC215/`); `~/ntt-int15g` untouched. No job was run or is pending.
- Nothing left in the task list; the open issues of §3 are for the integrator / the user.

## L13d: the plan printer (PLAN §32, row L)

(former file: L13d.md)

Branch `p13d-L` (on main 566291d). How each number is labelled: **measured** = from a recorded aac6 run. **C-planned** = printed by
`MN_PLAN_ONLY`, which applies the code's own decisions to predicted operand sizes. **modelled** = from mn_model.py.

#### Verdict (C-planned, 576 nodes, defaults: auto, 2³¹ cap, MN_GROUPS 2,4,…,64,192,576)

- **At 4.4 × 10¹³ the critical path has 229 pieces, not 214.** That is tree 124 (each level's largest group) + reciprocal 71 +
  division 34. Node 0's groups have 197 (92 + 71 + 34). The model has 214 (90 + 71 + 53).
- **The model's 214 → 248 step (the binary levels going from 8 to 12 pieces per group) does not happen at one size in the
  C code.** It moves through the groups, highest term ranges first:
  - The first groups (nodes 572–575) cross at 4.29 → 4.30 × 10¹³.
  - The critical path rises 185 → 197 → 209 → 213 → 219 between 4.29 and 4.34 × 10¹³. The largest group of every binary level
    is at 12 from 4.34 × 10¹³.
  - Level-1 groups at 12 pieces: 140 of 288 at 4.4 × 10¹³, 169 at 4.435, 189 at 4.464, 245 at 4.6. Node 0's group stays at 8
    up to 5 × 10¹³.
  - Cause: a node's P and Q grow with its term index. Node 0's leaf Q has 3.28 × 10⁹ limbs, the average is 4.24 × 10⁹, and
    the top nodes have about 4.4 × 10⁹. The model gives every node the average share. A level's wall is its largest group.
- **A second step lies exactly at the target**: reciprocal + division go from 69 + 28 to 71 + 34 between 4.39 and
  4.40 × 10¹³ (critical path 221 → 229, node 0's 189 → 197).
- **The next step above 4.4 × 10¹³ is at 4.74 → 4.75 × 10¹³**: reciprocal 71 → 76 and division 34 → 36 (critical path
  229 → 236). Between 4.40 and 4.74 the count is flat at 228–229 (the division moves by ±1). The tree's largest groups next
  step at 4.81 → 4.82 × 10¹³ (124 → 128).
- **The last size below both steps is 4.29 × 10¹³, with 185 pieces on the critical path.** At 4.4 × 10¹³ the run is already
  past the model's step (partly, group by group) and exactly past the reciprocal/division step.
- **Two discrepancies in the model, for D2:**
  1. Uniform per-node shares: see above.
  2. The division's A_h operand. The code forms A_h = S >> (nq − 1 − dl), which has k ≈ dl + 1 limbs. **Measured**: A_h mu is
     555 555 557 × 555 555 558 limbs at 10¹⁰ over 4 node-processes. mn_model.division_cost uses `nah = 2 dl + 1`, which is
     twice that, so the model counts 53 division pieces at 4.4 × 10¹³ where the code forms 34.
- The steps the model places at 3.95–3.97 × 10¹³ (reciprocal/division) are at 3.95 → 3.96 in the C plan (reciprocal 61 → 69,
  division 25 → 28): the same position.

#### The 576 table (for D2)

**`archive:results_raw_2026-10-19/L13d_plan576.txt`** covers g = 576, 2.0–6.0 × 10¹³ total digits in steps of 0.01 × 10¹³: one line per size, then
every size where a count changes. Made by `ecalc/tests/plan_sweep.sh 576 2.0e13 6.0e13 0.01e13 archive:results_raw_2026-10-19/L13d_plan576.txt` on
the aac6 login node, in about 1 minute. It was regenerated after the pool_log fix (below) and is identical.

| column | what it counts |
|---|---|
| `tree` | node 0's groups (what node 0 prints) |
| `tree_max` | each level's largest group (the level's wall) |
| `recip`, `div` | one group each |
| `total` | tree + recip + div |
| `total_max` | tree_max + recip + div |
| `levels` | per level, node 0's group / the largest |

The categories are those of `mn_model.run(...)['pieces']`: formed pieces, with a one-plane product counted as 1. The division
count moves by ±1–5 from one size to the next. That is real: split_grid_cap's least-points grid changes shape, and the cuts
skip different pieces.

#### Design

- `MN_PLAN_ONLY=<total digits>:<g> ./ecalc` (argv[1] is optional; `MN_PLAN_QUIET=1` prints only the summary line). The hook
  in ecalc.c's main runs after the plane-cap switch and the pools' sizing, before `rns_init`. It uses no device and no
  communicator, then calls `_exit`. It takes 0.04 s at 576.
- The new files are `ecalc/mn_plan.c` and `ecalc/mn_plan.h`. The decisions come from the code's own functions:
  - rns_dist.c:
    - `mn_grid_shape` and `db_grid_shape` are extracted **unchanged** from mn_grid and mul_grid.
    - `grid_piece_skipped` is the two cuts; mul_grid and mn_grid now call it.
    - These are exported as `rns_dist_mn_plan` and `rns_dist_db_plan`.
    - `rns_dist_plan_pools` gives b_fits the pools rns_init would make (`rns_plane_pool_bytes`).
    - `rns_dist_plan_cap_test` is MN_TREE_LOGN_TEST without the cache release (which needs a device).
  - newton_db.c: `newton_chain_next` and `newton_mn_chain_start` are extracted unchanged from recip_db2 and recip_mn.
    `newton_mn_x1_level` is X1's rule as a function of g; `x1_level` calls it.
  - mn.c's schedule comes from `mn_groups_parse`. mn_plan.c mirrors the tree's walk (tree_level / tree_level_k in Horner
    order) for **every** group of every level.
- Only the operand sizes are predicted:
  - Q(a, b) = a…(b−1), from `lgammal`.
  - P = Q · f(a, b), where f = e − 1 at a = 1 and about 1/a above.
  - r has j + 1 limbs at the step to j.
  - d has `D_EST(j)` = j limbs.
  - S = Q e, and X has dl + 1 limbs.
  - Every Newton step is assumed to converge without a repeat.
- Tools: `ecalc/tests/plan_sweep.sh` (the table), `ecalc/tests/plan_compare.py` (a run's RNS_VERBOSE product lines against the
  plan's, in order), and `ecalc/tests/plan_validate.sh` (the validation job).
- Files touched outside the new ones:
  - `Makefile`: mn_plan.o and its dependencies.
  - `ecalc.c`: the hook.
  - `rns_dist.c` and `newton_db.c`: the extractions, all commented "Phase 13d L". They do not change behaviour: mnaccept
    passed, and the digits are identical.

#### Runs

Job 21103 on s24-30, 19:41–20:07 CDT, `tests/plan_validate.sh aac6-spx-c ~/L13d/val1` with PLAN_VALIDATE_ACCEPT=1. Every
real run used `RNS_VERBOSE=1 NEWTON_VERBOSE=1`. Every run was measured VERIFY OK, and every multi-process run printed "all N
nodes VERIFY OK". The comparison tables are in `archive:results_raw_2026-10-19/L13d_val/*.cmp`.

| run | env | wall (measured) | products compared | grids / with skips | grid, formed, skipped, g, cap | sizes |
|---|---|---|---|---|---|---|
| 10⁹ size 1 | – | 12.7 s | 52 | 0 / 0 | all equal | within 2 limbs (the r·d products) |
| 10⁹ size 2 | POOL_LOG=29 | 21.6 s | 24 | 0 / 0 | all equal | exact |
| 10⁹ size 3 | POOL_LOG=29 | 19.0 s | 26 | 0 / 0 | all equal | exact |
| 10⁹ size 4 | POOL_LOG=29 | 16.2 s | 26 | 0 / 0 | all equal | exact |
| 10¹⁰ size 1 | – | 23.3 s | 60 | 0 / 0 | all equal | within 2 limbs (max relative 0.17 = 6 vs 7 limbs at j = 3) |
| 10¹⁰ size 4 | POOL_LOG=29 | 72.8 s | 34 | 0 / 0 | all equal | exact |
| 10⁹ size 1, grids | DIST_LOGN_TEST=26 | 15.8 s | 52 | 2 / 0 | all equal | within 2 limbs |
| 10⁹ size 3, grids | POOL_LOG=29 DIST_LOGN_TEST=24 | 23.2 s | 26 | 9 / 2 | all equal | exact |
| 10¹⁰ size 4, grids | POOL_LOG=29 DIST_LOGN_TEST=27 | 88.1 s | 34 | 6 / 2 (the division's 2 × 3, 5 formed, 1 skipped) | all equal | exact |

- In the first pass, the four POOL_LOG=29 runs without DIST_LOGN_TEST disagreed on the cap alone: the plan printed 2^32 where
  the run printed 2^30. Plan mode had never set `rns_pool_log()`. Fixed in 690c38fa (`rns_preinit_pool_log`). After the fix,
  all 9 comparisons agree, and the 576 sweep is byte-identical (pool_log 31 is the default in both).
- The mn-tier sizes are exact, which confirms the lgamma sizes for P, Q, S, X and A_h. The only size differences are in
  the single-node reciprocal's d (data-dependent, 1–2 limbs).
- The gate `./mnaccept.sh 21103 --only unit,e9` gave **10 passed, 0 failed** in 1217 s, on d5feb1e9 (the C change). The later
  fix touches mn_plan.c only (plan mode).

#### Open issues

- d's size (the second operand of r·d) is estimated as j limbs. The measured value is 1–2 limbs more at small j and exact at
  the top. It can move a count only at a grid boundary.
- A Newton repeat (rare, data-dependent) adds its step's two products. The plan assumes none.
- The node-local leaf tree's device levels (`plan leaf`) are printed but were not validated: at 10⁹ and 10¹⁰ the leaf is all
  batch tier. The model does not count them either.

#### RESUME

- Done:
  - the printer (commits 42610f9 … 690c38fa)
  - the 576 table (committed)
  - validation (9 of 9 agree)
  - mnaccept unit,e9 10/10
- The job is cancelled. Nothing is running.

## G13d — the steps on real hardware (Phase 13d, PLAN §32 row G)

(former file: G13d.md)

Branch `p13d-G` (from main @ 566291d, the Phase 13c defaults; **no C changes**). aac6 clone `~/ntt-G13d` (built from
main). Every run on **aac6-spx-a**, one 45-minute job at a time. Full logs: aac6 `~/g13d/<tag>.log`
(`ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 RNS_VERBOSE=1`), one line per run in `~/g13d/summary.txt`. Scripts: `archive:drivers/ecalc/g13d_run.sh`
(the batch runner: one job, runs from a list, a watchdog, skips a run that would not finish in the job or before 23:38 EDT),
`archive:drivers/ecalc/g13d_hang.sh` (the same under gdb, which captures stacks on a hang), `archive:drivers/ecalc/g13d_chain.sh` (batches in sequence),
`archive:results_raw_2026-10-19/g13d/*.txt` (the run lists), `archive:results_raw_2026-10-19/g13d/g13d_table.py` (the tables below, from the logs).

Labels: **measured** (s24-16, this session), **modelled** (D2's `~/d2_steps1.txt`, mn_model.py at main), **assumed**.
Columns: *wall* = the runner's clock around `srun` (s); *total* = ecalc's own total (phases + init); *phases* = bs + 10dP + dm
+ T1 + dc + T2 (no outfile, so dc = 0); *pieces* = the `dist_db` lines of the products over 2²⁸ limbs, split at the end of
bs: `bs [N formed] count*ka×kb; dm [N formed] ...`; *dist calls* = the reciprocal's `dist` calls.

#### 0. A hang after init on the defaults (7.70 × 10¹⁰) — once, not reproduced in 5 more runs

`a_S4_lo_770e8` (job 21105, D = 7.70 × 10¹⁰, main's defaults, no outfile) stopped making progress right after init: the last
log line was `VmRSS 11.9 GB after init ... init 20.7 s`; the next line of a normal run is `bs: seeds were computed during init
(...)`, printed after `pthread_join` of the init-time seed thread (binsplit.c ~1117). After 10 minutes the process was at 0 %
CPU with 199 threads: **197 in `futex_wait_queue`, 2 in `kfd_wait_on_events`** (waiting on a GPU signal). gdb could not attach
(`ptrace_scope` = 1), so no stacks; the process was killed (rc 143) after 656 s.

Reproduction (the integrator's request): three more runs of 7.70 × 10¹⁰ on the defaults, same node, **under gdb** (which may
launch the process, and prints every thread's stack when the watchdog sends SIGINT): **all three completed, VERIFY OK**
(h_770e8_r1..r3: 134.1 / 129.4 / 131.4 s total). Two more without gdb, at 7.72 × 10¹⁰ and again at 7.70 × 10¹⁰ (f1,
131.7 s): VERIFY OK. So 1 hang in 5 runs at this size, 1 in 31 single-node runs this session. The isolating runs (RNS_STRATEGY=C; depth 1 + no shift chunking + K's kernels off) were not
run (they were conditional on a second hang). Open: the hang's stack. Both scripts now have a watchdog; `g13d_hang.sh`
catches the stacks if it recurs.

#### (b) The 2³⁰ cap at 1.42 vs 1.44 × 10¹¹ — no cliff between them

All on s24-16, main's defaults except the cap (so `RNS_STRATEGY=auto`), measured:

| tag | digits | wall s | total s | init | bs | recip | dm | phases | pieces > 2²⁸ limbs: bs [formed]; dm [formed] | dist calls (recip) | device GB | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| b_230_142e9_r1 | 1.42 × 10¹¹ | 1085 | 1080.90 | 60.0 | 347.1 | 311.6 | 673.6 | 1020.9 | bs [376] 10*1x3 6*3x3 2*5x3 2*5x7 2*12x8; dm [828] 1*2x1 1*2x2 1*7x2 1*5x5 1*10x5 1*9x11 1*18x11 2*18x22 | 829 | 501.4 | VERIFY OK |
| b_230_144e9_r1 | 1.44 × 10¹¹ | 1139 | 1132.17 | 29.8 | 394.9 | 330.5 | 707.3 | 1102.4 | bs [392] 10*1x3 6*3x3 2*6x3 2*5x7 2*11x9 (+2*2x1); dm [833] ... 1*10x10 1*20x10 2*19x21 | 846 | 507.6 | VERIFY OK |
| b_230C_142e9 (`RNS_STRATEGY=C`) | 1.42 × 10¹¹ | 1354 | 1349.66 | 43.5 | 470.8 | 393.0 | 835.1 | 1306.2 | bs [224] 10*1x2 6*2x3 2*5x2 2*4x5 2*9x6; dm [475] 1*2x1 1*2x2 1*4x2 1*3x5 1*7x4 1*7x8 1*14x8 2*13x17 | 510 | 501.4 | VERIFY OK |
| (P13b, s24-30, C, for reference) | 1.42 × 10¹¹ | 680 | 677.2 | 34.4 | 242.4 | 184.7 | 400.2 | 642.8 | the same grids (bs levels 23–26: 1×2, 2×3, 4×5 + 5×2, 9×6) | 510 | 501.4 | VERIFY OK |

**Answer (measured): on the same node and the same code, 1.44 × 10¹¹ is 1.047 × the wall of 1.42 × 10¹¹ (phases
1.080 ×), not 1.6 ×.** Δ phases +81.5 s for +1.41 % digits (smooth growth would give ≈ +14 s): a moderate step, in both phases
(bs +47.8 s: the top product re-grids 12×8 → 11×9 and a level's 5×3 → 6×3; dm +33.7 s, recip +18.9 s, 846 vs 829 dist calls).
P13b's 677 s → 1086 s pair differs from this one in two ways: the strategy (P13b ran `RNS_STRATEGY=C`, the default then; main's
auto cuts finer at the 2³⁰ cap: 829 dist calls in the reciprocal against P13b's 510) and the node (s24-30 vs s24-26). The
P13b logs settle the second: its 1.44 × 10¹¹ run had **the same grids** as its 1.42 × 10¹¹ run for every product up to the
top level (e.g. the 562277383 × 571080422 products, 1 × 2 pieces, 2.28 s vs 1.19 s), and its reciprocal made 516 vs 510 dist
calls, but every call was ~2–3 × slower (load 291 s vs 48 s, ntt 375 vs 123): a per-call slowdown of the whole run on s24-26
(memory state or the node), not a grid step.

**The C run on s24-16 settles it (measured): P13b's 677 s was the fast one.** At 1.42 × 10¹¹, `RNS_STRATEGY=C` on s24-16
forms exactly P13b's grids (510 dist calls in the reciprocal, the same bs levels) and takes **1306 s of phases against P13b's
643 s on s24-30 — 2.03 ×**; the difference is almost all the dist tier's operand *load* (reciprocal: load 452 s here, 48 s on
s24-30, for the same 510 calls: 0.89 vs 0.094 s per call; ntt 544 vs 123 s). P13b's own 1.44 × 10¹¹ run on s24-26 (load 291 s,
ntt 375 s over 516 calls) sits between. So near the node's memory edge at the 2³⁰ cap, the C strategy's per-call load and
transform times vary 2–10 × between nodes (or runs) with the work unchanged; no grid step between 1.42 and 1.44 × 10¹¹ explains
P13b's 1.6 ×. On s24-16, main's auto is the faster strategy at this size (1021 vs 1306 s of phases). Open: why s24-30 loaded
10 × faster in P13b's run (a node setting, or free-memory state at the edge; not measured here).

#### (a) The one-node sweep on the defaults (auto, 2³¹, shift chunking, depth 2)

Every run on the defaults (no env; `NOGDB=1` is a marker for the runner, the `h_` runs ran under gdb), no outfile (no
reference exists at these sizes; 5 × 10¹⁰ has one but no step is there), VERIFY OK on every completed run. Measured, s24-16:

| tag | digits | wall s | total s | init | bs | recip | dm | phases | pieces > 2²⁸ limbs: bs [formed] count*ka×kb; dm [formed] | dist calls (recip) | device GB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| a_S1_lo_512e8 | 5.12e10 | 85 | 79.20 | 18.9 | 31.3 | 12.18 | 28.9 | 60.3 | bs [12] 2*1x3 2*3x1; dm [19] 1*1x2 1*2x2 2*2x4 | 84 | 283.4 |
| a_S1_hi_519e8 | 5.19e10 | 90 | 83.42 | 17.0 | 32.9 | 14.67 | 33.4 | 66.4 | bs [12] 2*1x3 2*3x1; dm [36] 1*2x1 1*2x2 1*4x2 2*3x5 | 91 | 272.0 |
| a_S2_lo_576e8 | 5.76e10 | 100 | 94.45 | 18.0 | 36.5 | 15.56 | 39.9 | 76.5 | bs [10] 2*2x1 2*1x3; dm [40] 1*2x1 1*2x2 1*4x2 2*4x4 | 89 | 284.7 |
| a_S2_hi_583e8 | 5.83e10 | 105 | 98.81 | 19.7 | 35.9 | 18.65 | 43.1 | 79.1 | bs [10] 2*2x1 2*1x3; dm [47] 1*3x1 1*2x3 1*5x2 2*4x5 | 94 | 286.6 |
| a_S3_lo_659e8 | 6.59e10 | 112 | 109.83 | 22.0 | 45.3 | 18.80 | 42.5 | 87.8 | bs [18] 2*1x3 2*2x3; dm [33] 1*3x1 1*3x2 1*2x3 2*3x4 | 100 | 308.2 |
| a_S3_hi_666e8 | 6.66e10 | 124 | 120.97 | 22.8 | 46.0 | 21.75 | 52.0 | 98.1 | bs [18] 2*1x3 2*2x3; dm [54] 1*3x1 1*2x3 1*4x3 2*4x6 | 106 | 310.4 |
| a_mid_720e8 | 7.20e10 | 129 | 126.43 | 21.0 | 50.8 | 23.84 | 54.5 | 105.4 | bs [14] 2*1x3 2*2x2; dm [54] 1*2x1 1*2x2 1*7x2 2*5x5 | 103 | 327.1 |
| a_S4_lo_770e8 | 7.70e10 | 656 | **hung after init** (§0), killed | | | | | | | | |
| h_770e8_r1 (gdb) | 7.70e10 | 145 | 134.07 | 25.9 | 52.9 | 22.59 | 54.7 | 108.2 | bs [14] 2*1x3 2*2x2; dm [37] 1*2x1 1*2x2 1*4x2 2*4x4 | 97 | 342.6 |
| h_770e8_r2 (gdb) | 7.70e10 | 140 | 129.36 | 18.4 | 55.9 | 22.50 | 54.4 | 110.9 | (same) | 97 | 342.6 |
| h_770e8_r3 (gdb) | 7.70e10 | 140 | 131.38 | 21.0 | 55.5 | 22.45 | 54.3 | 110.4 | (same) | 97 | 342.6 |
| a_S4_lo_772e8 | 7.72e10 | 145 | 136.39 | 24.0 | 54.5 | 22.73 | 57.3 | 112.4 | bs [14] 2*1x3 2*2x2; dm [39] 1*2x1 1*2x2 1*4x2 2*4x4 | 97 | 343.3 |
| a_S4_hi_777e8 | 7.77e10 | 154 | 150.50 | 24.4 | 58.1 | 30.64 | 67.9 | 126.1 | bs [22] 2*1x3 2*4x2; dm [72] 1*1x3 1*5x1 1*3x3 1*5x3 2*5x6 | 124 | 344.8 |
| a_S5_lo_856e8 | 8.56e10 | 180 | 172.55 | 22.6 | 71.6 | 34.44 | 77.7 | 149.9 | bs [42] 4*1x3 2*6x1 2*3x3; dm [78] 1*3x1 1*1x3 1*3x3 1*6x3 2*6x6 | 143 | 369.3 |
| a_S5_hi_861e8 (×2) | 8.61e10 | 181, 185 | 178.92, 178.84 | 22.3, 22.0 | 72.7 | 34.67 | 83.8, 84.0 | 156.6, 156.8 | bs [42] 4*1x3 2*6x1 2*3x3; dm [85] 1*3x1 1*1x3 1*3x3 1*6x3 2*6x6 | 143 | 370.9 |
| a_S6_lo_924e8 | 9.24e10 | 192 | 189.83 | 24.0 | 77.0 | 36.90 | 88.7 | 165.8 | bs [40] 4*1x3 2*5x1 2*3x3; dm [86] 1*1x2 1*3x1 1*2x3 1*7x3 2*6x7 | 142 | 390.4 |
| a_S6_hi_931e8 | 9.31e10 | 195 | 191.70 | 20.7 | 78.4 | 40.83 | 92.5 | 171.0 | bs [40] 4*1x3 2*5x1 2*3x3; dm [95] 1*1x2 1*3x2 1*3x4 1*7x3 2*6x7 | 151 | 392.6 |
| a_S7_lo_1027e8 | 1.027e11 | 230 | 225.08 | 23.3 | 88.7 | 47.00 | 112.9 | 201.7 | bs [40] 4*1x3 2*5x1 2*3x3; dm [109] 1*1x2 1*2x2 1*2x4 1*9x3 2*6x9 | 151 | 422.4 |
| a_S7_hi_1034e8 | 1.034e11 | 245 | 237.25 | 26.3 | 91.5 | 53.34 | 119.3 | 210.9 | bs [40] 4*1x3 2*5x1 2*3x3; dm [125] 1*2x1 1*2x2 1*4x2 1*3x5 1*9x3 2*6x9 | 167 | 424.5 |
| a_S8_lo_1052e8 | 1.052e11 | 234 | 230.65 | 31.0 | 96.0 | 48.69 | 103.5 | 199.7 | bs [50] 4*1x3 2*3x3 2*5x2; dm [84] 1*2x1 1*2x2 1*4x2 1*5x3 1*3x5 2*5x6 | 165 | 430.1 |
| a_S8_hi_1058e8 | 1.058e11 | 249 | 245.65 | 22.4 | 98.3 | 55.35 | 124.8 | 223.2 | bs [50] 4*1x3 2*3x3 2*5x2; dm [127] 1*2x1 1*2x2 1*4x2 1*3x5 1*7x4 2*7x8 | 178 | 432.0 |
| a_S9_lo_1156e8 | 1.156e11 | 311 | 307.85 | 31.4 | 114.6 | 69.78 | 161.7 | 276.5 | bs [58] 4*1x3 2*3x3 2*7x2; dm [144] 1*2x1 1*2x2 1*4x2 1*4x4 1*8x4 2*8x8 | 193 | 462.4 |
| a_S9_hi_1163e8 | 1.163e11 | 317 | 313.86 | 28.3 | 114.4 | 77.82 | 170.9 | 285.5 | bs [46] 4*1x3 2*4x2 2*3x3; dm [160] 1*3x1 1*2x3 1*5x2 1*4x5 1*7x5 2*7x10 | 193 | 464.6 |
| a_S10_lo_1245e8 | 1.245e11 | 191 | **out of memory** at the start of the reciprocal (below), rc 1 | 42.8 | 142.0 | | | | bs [72] 6*1x3 2*3x3 2*6x3 | | 490.0 |
| a_S10_lo_1245e8_r2 | 1.245e11 | 182 | **out of memory again**, the same place (f2) | | | | | | bs [72] (the same) | | 490.0 |
| a_S10_hi_1252e8 | 1.252e11 | 420 | 413.15 | 31.2 | 159.9 | 99.23 | 221.8 | 382.0 | bs [82] 6*1x3 2*5x1 2*3x3 2*6x3; dm [177] 1*3x1 1*2x3 1*5x2 1*4x5 1*8x5 2*8x10 | 232 | 492.2 |

(*device* = the `mem [init]` line: planes + regions + tables. Two 8.61 × 10¹⁰ runs, one in each batch, agree to 0.2 s.)

**Each predicted step (D2's `d2_steps1.txt`), measured.** Δ phases across the pair, against the smooth growth over the same
Δ digits (assumed: phases ∝ digits, exponent 1.0, which the non-step intervals bear out: 7.20 → 7.70 gives 0.7, 8.61 → 9.24
gives 0.8), and D2's modelled jump:

| step (modelled position) | pair | Δ digits | Δ phases measured | smooth part | **excess = the step** | modelled jump | where (pieces formed) | verdict |
|---|---|---|---|---|---|---|---|---|
| S1 5.155e10 | 5.12 / 5.19 | +1.37 % | +6.1 s (+10.1 %) | +0.8 | **+5.3 s** | +4.6 | dm 19 → 36; bs unchanged | **seen** (dm); the tree part (top 2 → 3) not seen |
| S2 5.795e10 | 5.76 / 5.83 | +1.22 % | +2.6 s (+3.4 %) | +0.9 | **+1.7 s** | +5.0 | dm 40 → 47; bs unchanged (−0.6 s) | **weak**: dm part seen, the tree part (top 3 → 4) refuted |
| S3 6.625e10 | 6.59 / 6.66 | +1.06 % | +10.3 s (+11.7 %) | +0.9 | **+9.4 s** | +5.8 | dm 33 → 54 (recip +2.9, the rest of dm +6.6) | **seen**, 1.6 × the modelled size |
| S4 7.735e10 | 7.72 / 7.77 | +0.65 % | +13.7 s (+12.2 %) | +0.7 | **+13.0 s** | +15.3 | bs 14 → 22 (top 2×2 → 4×2), dm 39 → 72 | **seen** in both phases, as modelled; the 7.64e10 share sits below it |
| S5 8.585e10 | 8.56 / 8.61 | +0.58 % | +6.9 s (+4.6 %) | +0.9 | **+6.0 s** | +5.8 | dm 78 → 85; bs unchanged | **seen**, as modelled |
| S6 9.275e10 | 9.24 / 9.31 | +0.76 % | +5.2 s (+3.1 %) | +1.3 | **+3.9 s** | +10.0 | dm 86 → 95; bs unchanged (40 = 40) | **half**: dm part seen; the tree top 6 → 8 refuted |
| S7 1.0305e11 | 1.027 / 1.034 | +0.68 % | +9.2 s (+4.6 %) | +1.4 | **+7.8 s** | +8.4 | dm 109 → 125 (recip +6.3); bs unchanged (bs +2.8) | **seen** (dm); the tree part not seen |
| S9 1.1595e11 | 1.156 / 1.163 | +0.61 % | +9.0 s (+3.3 %) | +1.7 | **+7.3 s** | +18.2 | dm 144 → 160 (recip +8.0); bs re-grids 7×2 → 4×2 with *fewer* pieces (58 → 46), bs −0.2 s | **half**: dm seen; no tree jump |
| S10 1.2485e11 | 1.245 / 1.252 | +0.56 % | — (1.245 ran out of memory) | | | +7.5 | | not measured |
| S8 1.0545e11 | 1.052 / 1.058 | +0.57 % | +23.5 s (+11.8 %) | +1.1 | **+22.4 s** | +7.4 | dm 84 → 127 (recip +6.7, the rest of dm +14.6); bs unchanged | **seen, 3 × the modelled size** (the division's X Q / A_h μ grids 5×6 → 7×8) |
| minor steps | — | | | | | | | not run |

**Finding (measured): the division/reciprocal steps are real and where the model puts them (S1, S3, S4, S5, S7, S8 within
the 0.6–1.4 % brackets; S3 and S8 larger than modelled; note 1.052 × 10¹¹ (199.7 s) is faster than 1.034 × 10¹¹ (210.9 s):
between S7 and S8 the division's grid gets cheaper again), but the model's tree-top re-grids (S1, S2, S6, S7, S9) do not show in bs on the
hardware: bs's large-product pieces are the same on both sides of every step except S4.** Where the model predicts a
tree-top jump of 5–10 s, the bs phase moves by −0.6 … +2.8 s. For D2: the model's size-1 tree-top grid rule does not match what
`binsplit`'s top levels form on the hardware (the logs' `dist_db` lines before the `bs` total line have the C code's grids).

**The one-node edge on the defaults (measured).** 1.245 × 10¹¹ failed and 1.252 × 10¹¹ ran (VERIFY OK, 382 s of phases): at
1.245 the reciprocal's first `dbig` block-pool growth asked hipMalloc for 27.67 GB on APU 0 while 490.0 GB of device was mapped
(122.5 per APU) and was refused (`dbig block pool (inside a phase): allocation (hipmalloc) of 27.67 GB on APU 0 failed (out of
memory)`), although the pool itself held **41.24 GB free in 2 extents, the largest 27.50 GB** — 0.17 GB short of the request.
So the failure is the pool's fragmentation at the edge (a request 0.6 % larger than the largest free extent, then a fresh
hipMalloc past the ~515–518 GB device edge P13b measured), not the total: 1.252 × 10¹¹, with 2.2 GB more mapped, happened to
lay its blocks out so the request fitted. **Reproduced**: a second 1.245 × 10¹¹ run (f2) failed the same way; it is
deterministic for the size, not a one-off. This is above the design's 480 GB budget (the defaults map 490 GB here), so it
does not touch the target, but the one-node ceiling of the defaults is **not monotone near 1.25 × 10¹¹**: the largest clean
run below the failure is 1.163 × 10¹¹ (464.6 GB).

#### (c) The mn tier's steps at size 4 on one node

Four node-processes on s24-16 (`mnrun.sh 4`, TCP loopback), `DIST_LOGN_TEST=27 NEWTON_MN_GROUPS=0` (D2's env: the mn tier's
cap is 2^(27 + log₂ S) over an S-node group) **plus `POOL_LOG=29`** so four processes' plane pools fit one node
(`mn_logn_cap` takes min(DIST_LOGN_TEST, POOL_LOG), so the mn cap is unchanged). Measured:

| tag | total digits | wall s | total s | bs | recip | dm | phases | node 0's tree levels L1 / L2 s | level 1 (S = 2): nodes 0,1 / nodes 2,3 | level 2 (S = 4) | mn products after the tree (node 0): count, pieces formed | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| c2_T1_920e7 | 9.2e9 | 59 | 57.25 | 13.5 | 20.3 | 36.5 | 50.6 | 5.73 / 5.56 | 1×1 / 1×1 | 1×1 | 28, 34 | VERIFY OK all 4 nodes |
| c2_T1_950e7 | 9.5e9 | 64 | 62.20 | 21.3 | 20.7 | 34.6 | 56.5 | 5.69 / 13.06 | 1×1 / **1×2** | 1×1 | 28, 34 | VERIFY OK all 4 |
| c2_T1_980e7 | 9.8e9 | 87 | 84.37 | 25.9 | 29.4 | 51.7 | 78.3 | 6.51 / 16.72 | 1×1 / 1×2 | **1×2** | 30, 41 | VERIFY OK all 4 |
| c2_T1_1e10 | 1.0e10 | 109 | 107.12 | 32.7 | 35.8 | 65.5 | 100.7 | 6.06 / 24.07 | 1×1 / 1×2 | 1×2 | 30, 41 | VERIFY OK all 4, **identical** to results/e_1e10.out (sha1 of the joined parts) |
| c2_T1_102e8 | 1.02e10 | (killed at 58 s for the three-node slot, after the tree) | | | | | | | **1×2** / 1×2 | 1×2 | | tree only |
| f_T1_102e8 | 1.02e10 | 80 | 77.89 | 24.4 | 27.0 | 46.8 | 71.9 | – | **1×2** / 1×2 | 1×2 | 30, 41 | VERIFY OK all 4 (rerun, f1) |
| f_T2_126e8 | 1.26e10 | 113 | 110.91 | 35.7 | 33.1 | 68.2 | 104.6 | 11.12 / 21.15 | 1×2 / 1×3 | 1×2 | 30, 46 | VERIFY OK all 4 |
| f_T2_131e8 | 1.31e10 | 135 | 132.74 | 36.1 | 45.7 | 88.8 | 125.7 | 14.62 / 17.87 | 1×2 / 1×3 | **1×3** | 30, 50 | VERIFY OK all 4 |
| f_T3_143e8 | 1.43e10 | 136 | 133.27 | 41.0 | 49.4 | 84.9 | 126.8 | 15.02 / 21.96 | 1×3 / 1×4 | 1×3 | 30, 52 | VERIFY OK all 4 |
| f_T3_147e8 | 1.47e10 | 138 | 135.30 | 45.2 | 46.3 | 81.6 | 127.6 | 15.15 / 25.94 | 1×3 / 1×4 | 1×3 | 30, 55 | VERIFY OK all 4 |
| f2_T3_150e8 | 1.50e10 | 164 | 162.07 | 53.0 | 47.8 | 100.9 | 154.9 | 14.61 / 34.30 | 1×3 / 1×4 | **1×4** | 30, 55 | VERIFY OK all 4 |
| f2_T3_155e8 | 1.55e10 | 194 | 192.14 | 52.1 | 62.3 | 132.2 | 185.2 | 18.27 / 29.63 | **1×4 / 2×2** | 1×4 | 30, 55 | VERIFY OK all 4 |
| f2_T4_192e8 | 1.92e10 | 153 | 150.90 | 49.1 | 46.2 | 93.4 | 143.9 | 20.71 / 23.27 | 2×2 / 2×3 | 2×2 | 30, 64 | VERIFY OK all 4 |
| f2_T4_198e8 | 1.98e10 | 201 | 198.30 | 64.8 | 67.2 | 124.9 | 190.8 | 22.02 / 37.58 | 2×2 / 2×3 | **2×3** | 32, 75 | VERIFY OK all 4 |
| f2_pl27_grow_920e7 (`POOL_LOG=27 RNS_POOL_GROW=1`) | 9.2e9 | 67 | 62.45 | 18.8 | 24.0 | 38.1 | 57.6 | 5.95 / 10.10 | 1×1 / 1×1 | 1×1 | 28, 34 | VERIFY OK all 4 (the c1 configuration with growth allowed) |

**The S = 4 level's steps against D2's positions (the level where every node's share is the group's average):**

| step | modelled | measured bracket | verdict | Δ phases across the bracket |
|---|---|---|---|---|
| T1 (1×1 → 1×2) | 9.664 × 10⁹ | 9.5 – 9.8 × 10⁹ | **seen, in place** | +21.8 s (+39 %) for +3.2 % digits (with the reciprocal's 34 → 41 pieces) |
| T2 (1×2 → 1×3) | 1.2885 × 10¹⁰ | 1.26 – 1.31 × 10¹⁰ | **seen, in place** | +21.1 s (+20 %) for +4.0 %: bs +0.4, dm +20.6 (recip +12.6; 46 → 50 pieces) |
| T3 (1×3 → 1×4) | 1.445 × 10¹⁰ | **1.47 – 1.50 × 10¹⁰** | **moved: 1.7–3.8 % later than modelled** (1.43 and 1.47 have the same grids and the same wall, 126.8 / 127.6 s) | +27.3 s (+21 %) for +2.0 % (bs +7.8, dm +19.3) |
| T4 (2×2 → 2×3) | 1.95 × 10¹⁰ | 1.92 – 1.98 × 10¹⁰ | **seen, in place** | +46.9 s (+33 %) for +3.1 % (bs +15.7, dm +31.5; after-tree pieces 64 → 75) |

The S = 2 level's pairs split earlier for the top pair (nodes 2, 3) at every step, as the term shares predict: the top pair
reaches 1×3 before 1.26 × 10¹⁰, 1×4 before 1.43, 2×2 by 1.55 × 10¹⁰; the bottom pair 1×2 by 1.02, 1×3 by 1.43, 1×4 by 1.55.
Walls at size 4 on one node are noisy (four processes on four shared APUs, TCP loopback): 1.55 × 10¹⁰ (185 s of phases) is
slower than 1.92 × 10¹⁰ (144 s), and 1.43 → 1.47 moved bs +4.2 and dm −3.3 with no grid change; so the per-step Δ walls are
±10–20 s, but every step above is larger than that.

**D2's crisp test, measured: the nodes' shares are by terms, and the groups split in the modelled order.** Level 1's top pair
(nodes 2, 3) splits to 1×2 between 9.2 and 9.5 × 10⁹ (modelled 9.337 × 10⁹); the S = 4 level splits between 9.5 and 9.8 × 10⁹
(modelled 9.664 × 10⁹); the bottom pair (nodes 0, 1) stays 1×1 at 1.0 × 10¹⁰ and splits by 1.02 × 10¹⁰ (modelled 1.0013 ×
10¹⁰). So the three thresholds fall in their predicted brackets, in the predicted order — the uneven per-node digit share
(terms, not digits) is real, which is what moves the 576-node step earlier (D2). One difference from the model's piece
counts: the S = 4 level splits **1×1 → 1×2** (D2's table counts it as 2 → 4 pieces over the level's two products, which is
the same thing; the update's "2×2" is not what the code forms).

Walls: each split shows. At 9.5 × 10⁹ the top pair's 1×2 delays node 0's level 2 (which waits for nodes 2, 3) by 7.5 s and bs
by +7.8 s (+58 %) for +3.3 % digits; at 9.8 × 10⁹ the S = 4 split and the mn-tier products after the tree (34 → 41 pieces)
add +21.8 s of phases (+39 %) for +3.2 % digits. (Four processes share the node's four APUs over TCP loopback, so these walls
are the mechanism, not the target's rates; the 1.0 × 10¹⁰ run also wrote its 10 GB output, streamed during dm, so its dm is
not comparable.)

Before this: with `POOL_LOG=27` (c1, 9 runs) every run stopped 12 s in (rc 139). The tree's leaves needed plane pool 0 to grow
0.81 → 1.07 GB inside a phase, which main forbids (`rns_dpool`, rns_mul.c:214: prints the message and calls `exit(1)`); all four
APU threads hit it at once and the `exit(1)` from a worker thread, with the other threads still inside HIP, ended in `free():
corrupted unsorted chunks` and a segfault in every task. So: **the test configuration** — the same sizes with POOL_LOG=29 (c2, above) ran to VERIFY OK on all four nodes (POOL_LOG=27 is below what the leaves
need at 2.3 × 10⁹ digits per process; mnaccept's size-4 case uses POOL_LOG=27 only with RNS_POOL_GROW=1), not a defect in the
product code. A small defect on main remains: the guard's `exit(1)` from a worker thread is not clean (it should report and
`_exit`/abort, so the run ends with rc 1, not a segfault).

#### RESUME

*Updated 23:50 EDT; all batches finished, no job of mine running (last: 21117, cancelled 23:42 EDT).*

Done (all on s24-16, measured; 31 single-node runs, 24 size-4 launches (9 failed in the c1 configuration, 1 killed for the slot, 14 VERIFY OK on all four nodes), every completed run VERIFY OK; the one outfile compare,
1 × 10¹⁰ at size 4, identical):
- (b) 1.42 vs 1.44 × 10¹¹ at 2³⁰: no cliff (auto: 1021 → 1102 s of phases, a moderate step). The 1.6 × of P13b is a per-call
  slowdown (the dist tier's operand load) that varies between nodes/runs: C at 1.42 × 10¹¹ on s24-16 took 2.03 × P13b's s24-30
  time with the same grids.
- (a) 22 sizes over 10 step pairs (5.12 × 10¹⁰ … 1.252 × 10¹¹): S1, S3, S4, S5, S7, S8 seen (S3, S8 larger than
  modelled), S2 weak, S6 and S9 half — the model's tree-top re-grids never show in bs except at S4. The hang (§0) not
  reproduced in 5. 1.245 × 10¹¹ OOMs deterministically (block-pool fragmentation, 0.17 GB short) while 1.252 × 10¹¹ runs.
- (c) size 4 on one node, DIST_LOGN_TEST=27: the S = 4 level's T1, T2, T4 in their modelled brackets, T3 1.7–3.8 % later
  than modelled; the term-share order of the S = 2 splits confirmed (D2's crisp test). The c1 segfault is the configuration
  (POOL_LOG=27 needs RNS_POOL_GROW=1: that runs VERIFY OK), plus the guard's unclean `exit(1)` from worker threads.

Not done: S10's pair (1.245 cannot run), the minor steps; a second hang (no stacks); 1.44 × 10¹¹ at RNS_STRATEGY=C; why
s24-30's loads were 10 × faster in P13b.

To resume: `G_STOP="<time CDT>" setsid nohup bash ~/ntt-G13d/ecalc/g13d_chain.sh <batch...> > ~/g13d/chain.out 2>&1 <
/dev/null &` on aac6 with run lists in ~/g13d/<batch>.txt (format in g13d_run.sh; batches named h* run under gdb via
g13d_hang.sh); `python3 ~/g13d/g13d_table.py <tags>` and `python3 ~/g13d/g13d_mn.py <tags>` rebuild the tables from the logs.

## S13d — SHMEM across real nodes (TASKS 1.5), Phase 13d agent S (2026-09-23)

(former file: S13d.md)

(3-node slot: granted 22:20–23:00 EDT; used by job 21111, 22:18:55–22:34:24 EDT, cancelled. Nothing of S's is running.)


Branch `p13d-S` (from `main` @ 566291d). aac6 clones: `~/ntt-S13d` (built against Sandia OpenSHMEM, `make SHMEM_HOME=~/sos`,
on the login node) and `~/ntt-S13d-osh` (OpenMPI 4.1.6 OSHMEM, built on a compute node with `oshcc`); both at
e53eb83 (= main's code; 566291d adds only PLAN.md). Batch scripts and logs on aac6 in `~/S13d/` (`lib.sh`, `b<N>.sh`,
`b<N>/*.log`, `b<N>/summary.txt`). Nodes: aac6-spx-b + aac6-spx-c, one PE (= one node-process driving
four APUs) per node. The only inter-node link is `enp129s0`, **1 GbE** (measured: `/sys/class/net/enp129s0/speed` = 1000,
no other NIC) — every wall below is dominated by that link, not by the transport's form.

#### RESUME

- Batch 1 (job 21101): OSHMEM across the two nodes **hangs** (details §1). Moved to SOS.
- Batch 2 (job 21101, done): SOS `t_comm`, `DIST_LAYERED=1 t_dist 24` VERIFY OK; ecalc 10⁸ (host and device heap) and
  10⁹ identical over SOS and TCP. `t_mn_grid 0.5 27` did not finish in 600 s over the 1 GbE (killed; not a hang: both
  NICs at 119 MB/s line rate throughout).
- Batch 3 (job 21104, 20:07–20:43 CDT, done): SOS 10¹⁰ **identical** (2070.7 s); `t_comm` across nodes in all seven
  other transport modes VERIFY OK. (The 2400 s timeout wrapper was removed by hand ~1800 s into the run when the run was seen to
  need ~2070 s + the tail; `t_mn_grid 0.25 25` dropped for time.)
- Batch 4 (job 21108, 20:43–21:18 CDT, done): TCP 10¹⁰ identical, 2041.6 s (SOS 2070.7 s).
- Batch 5 (job 21111, the three-node slot, 21:18:55–21:34:24 CDT, cancelled): 10⁸ and 10⁹ at 3 real nodes identical at
  depth 1 and 2 with the stats, and plain SOS / TCP; `t_comm`, `DIST_LAYERED=1 t_dist 24` VERIFY OK at 3 PEs. §3.
- **Done. No job of S running; all jobs cancelled by 22:35 EDT.** Open: §4.

#### 1. Which SHMEM: OSHMEM hangs across nodes, Sandia SOS works

**OSHMEM (OpenMPI 4.1.6, UCX)** — `COMM_TRANSPORT=shmem ./mnrun.sh 2 ./tests/t_comm` (the `--mpi=pmix`, `setarch -L`
path; the transport's OSHMEM defaults: calls serialised, `COMM_SHMEM_ORDER=quiet`, helper-thread pushes):
`shmem_init` and the key exchange succeed across the nodes (`t_dist` printed "four meshes of 2 PEs over SHMEM: created in
12.9 s"), then the first exchange never completes: `t_comm` killed by its 300 s timeout with no output (on one node it
takes ~2 s), `DIST_LAYERED=1 t_dist 24` stuck after "meshes connected" for > 90 s. Stacks (gdb, both PEs, measured):
PE 1's push thread is inside `shmem_ctx_long_p → mca_spml_ucx_put → ucp_worker_progress → uct_tcp_iface_progress`
(a blocking put over UCX's tcp transport, holding the transport's serial lock), PE 0's main thread is in the polled
`wait_ge` (`shmem_long_test` + `sched_yield`). Across nodes UCX has only its tcp transport (no shared memory / CMA),
whose puts need the target's worker to be progressed; `shmem_long_test` evidently does not progress it, so each side
waits for the other. Not rescued by `OMPI_MCA_spml_ucx_async_progress=1` (tick 100 µs; 120 s timeout, no output) nor by
`OMPI_MCA_spml_ucx_nb_progress_thresh_global=1` (killed at 113 s, no output). Not pursued further: the transport's
concurrent form cannot run on OSHMEM 4.1 anyway (results/SHMEM_ITEMS.md §S: no working THREAD_MULTIPLE, no put-with-signal),
and SOS is the form the target's SHMEM resembles.

**Sandia OpenSHMEM 1.5.3 (`~/sos`, libfabric sockets provider, `srun --mpi=pmi2`)** — works first time across the
nodes in the target form (concurrent contexts per communicator, `putsig` inline pushes, pool-resident buffers): the
sockets provider runs a progress thread per process, so the target side needs no help from the caller.

#### 2. Two real nodes (job 21101, SOS unless marked TCP)

Commands from `~/ntt-S13d/ecalc` (the SOS build; the TCP runs use the same binary with `COMM_TRANSPORT=tcp`):
`COMM_TRANSPORT=<shmem|tcp> SLURM_JOB_ID=$J ./mnrun.sh 2 env <X> ./ecalc <d> ~/S13d/tmp/<tag>.txt`, the two parts
concatenated and `cmp`'d against `~/ntt/ecalc/ref/e_<d>.txt` (10¹⁰: `~/ntt/ecalc/results/e_1e10.out`), the reference
evicted from both nodes' page cache first. Every other switch at the Phase 13c defaults. Times measured, one run each.

##### 2.1 Transport tests (2 PEs, 2 nodes)

| test | command | result | wall |
|---|---|---|---|
| `t_comm` (all sizes: all-to-all 1 B–3 MiB, all-gathers, alltoallv 8 B / 1000 B / 3 MiB units, reductions, point-to-point; plain and symmetric buffers) | `./mnrun.sh 2 ./tests/t_comm` | **VERIFY OK on both PEs** (0 bad); pool peak 54 MiB | 2.2 s |
| layered all-to-all, 2 nodes × 4 APUs vs the one-rank engine | `./mnrun.sh 2 env DIST_LAYERED=1 ./tests/t_dist 24` | **VERIFY OK (12 checks) on both** | 11.7 s |
| the multi-node product tier | `./mnrun.sh 2 ./tests/t_mn_grid 0.5 27` | **not finished**: killed at the 600 s timeout after three of its shapes (one plane 4.15 s, 1×2 7.77 s, 2×3 13.94 s — product times on node 0); both NICs at 119 MB/s (line rate) while it ran, so slow, not hung. No VERIFY line either way | > 600 s |

##### 2.2 ecalc at 2 nodes

| run | switches | result | total (phases; init) | VmHWM | pool peak (PE 0) |
|---|---|---|---|---|---|
| 10⁸ SOS, host heap | `POOL_LOG=27` | **identical**, both VERIFY OK | 26.82 s (20.6; 6.2) | 26.0 GB | 130 MiB |
| 10⁸ SOS, device heap | `POOL_LOG=27 COMM_SHMEM_DEVHEAP=1` | **identical**, both VERIFY OK | 24.65 s (20.1; 4.5) | 17.4 GB | 130 MiB |
| 10⁸ TCP | `POOL_LOG=27` | **identical**, both VERIFY OK | 22.86 s (19.4; 3.5) | 17.5 GB | — |
| 10⁹ SOS, device heap | `POOL_LOG=29 COMM_SHMEM_DEVHEAP=1` | **identical**, both VERIFY OK | 171.17 s (bs 27.8, dm 136.8; 165.0; init 6.1) | 18.2 GB | 849 MiB (staging 847) |
| 10⁹ TCP | `POOL_LOG=29` | **identical**, both VERIFY OK | 170.62 s (bs 27.9, dm 136.9; 165.1; init 5.5) | 19.1 GB | — |

SOS vs TCP (measured): at 10⁹ the phases are equal to 0.1 s (165.0 vs 165.1 s) — both saturate the 1 GbE; at 10⁸ SOS
is 0.7 s slower in the phases and 1–3 s slower at init (SOS's startup: the 8 GiB pool, the external-heap registration,
PMI). The device heap saves 8.6 GB of host RSS (the 8 GiB pool is HBM through HIP instead of host `shmem_malloc`
memory; the same bytes on the APU). On this fabric the transport's form does not show in the wall: the comparison
that matters for the target (per-message cost, overlap) needs a faster link than aac6 has.

| **10¹⁰ SOS, device heap** | `COMM_SHMEM_DEVHEAP=1 COMM_SHMEM_POOL_MB=16384 ECALC_VERBOSE=1` (POOL_LOG default) | **identical to `results/e_1e10.out`**, both VERIFY OK | **2070.74 s** (bs 407.4, dm 1643.9 (recip 874.4), dc 4.2; 2055.5; init 15.2) | 18.4 GB | **8479 MiB** (staging 8477) |

**The transport's pool at 10¹⁰ over 2 nodes needs 8479 MiB (measured) — more than the 8192 MiB default** (and than
TARGET.md §4's `COMM_SHMEM_POOL_MB=8192`): with 8192 the run would have stopped with "the symmetric pool cannot hold …"
(`pool_alloc` exits). The peak is all staging (the tree's and the division's exchanges are staged: results/SHMEM_ITEMS.md §S12 §5,
`rns_dist.c`'s slabs are not pool-resident), 2.1 GiB per APU thread = send + receive of the largest exchange. It grows
with the per-node plane (10⁸: 84 MiB, 10⁹: 847 MiB, 10¹⁰: 8477 MiB at 2 nodes, roughly ×10 per decade); at the target the
largest per-node plane is set by the plane cap per node, not by the total, so the pool must be sized from the largest
exchange's per-APU send + receive slab × 4 APU threads + control (the rule; the number at the cap is not measured here) —
for D2's `estimate.py` pool column and TARGET.md §4. A 16384 MiB pool is what this run used.

##### 2.3 The transport's other modes across nodes (SOS, `t_comm`, 2 PEs on 2 nodes, batch 3)

`./mnrun.sh 2 env <X> ./tests/t_comm` with X = `COMM_SHMEM_ORDER=fence`, `COMM_SHMEM_ORDER=quiet`, `COMM_SHMEM_THREAD=1`,
`COMM_SHMEM_SERIAL=1`, `COMM_SHMEM_NOSYM=1`, `COMM_SHMEM_SPIN_US=0`, `COMM_SHMEM_DEVHEAP=1`: **VERIFY OK on both PEs in
all seven** (2.0–2.1 s each). Across real nodes SOS's `fence` orders an nbi put before its signal (OSHMEM 4.1's did not,
results/SHMEM_ITEMS.md §S12) — so the `fence` fallback for a library without put-with-signal holds on a real fabric too.

##### 2.4 10¹⁰ over TCP (batch 4, job 21108)

| run | switches | result | total (phases; init) | VmHWM |
|---|---|---|---|---|
| **10¹⁰ TCP** | `ECALC_VERBOSE=1` (POOL_LOG default) | **identical to `results/e_1e10.out`**, both VERIFY OK | **2041.59 s** (bs 401.5, dm 1622.2 (recip 862.7), dc 4.1; 2027.8; init 13.8) | 27.3 GB |

**SOS vs TCP at 2 real nodes (measured, one run each): 10⁸ 24.65 vs 22.86 s, 10⁹ 171.17 vs 170.62 s, 10¹⁰ 2070.74 vs
2041.59 s (SOS +1.4 %: bs +5.9 s, dm +21.7 s).** Both are pinned to the 1 GbE (119 MB/s each way per node, measured
during `t_mn_grid`); the ~1 % is SOS's sockets provider (a progress thread per process, per-message software
overheads) against the TCP transport's socket streams — noise-level on this link, and not a statement about the target's
fabric, where SHMEM runs on the NIC's RDMA.

#### 3. Three real nodes (batch 5, job 21111: s24-16 + s24-26 + s24-30, one PE per node, the general map)

Same commands with `./mnrun.sh 3`; SOS runs with `COMM_SHMEM_DEVHEAP=1`; 10⁸ `POOL_LOG=27`, 10⁹ `POOL_LOG=29`; the stats runs
add `COMM_LAYER_STATS=1 COMM_XGMI_STATS=1 COMM_ALLTOALLV_DEPTH=<1|2>`. Everything measured, one run each.

##### 3.1 Correctness

| run | result | total (phases; init) |
|---|---|---|
| `t_comm` (SOS, 3 PEs) | VERIFY OK × 3 | 3.8 s |
| `DIST_LAYERED=1 t_dist 24` (SOS, 3 nodes × 4 APUs) | VERIFY OK × 3 | 12.5 s |
| 10⁸ SOS, depth 1 + stats | **identical**, all 3 VERIFY OK | 35.21 s (31.4; 3.8) |
| 10⁸ SOS, depth 2 + stats | **identical**, all 3 VERIFY OK | 32.59 s (29.5; 3.1) |
| 10⁸ SOS (depth 2 default, no stats) | **identical**, all 3 VERIFY OK | 33.94 s (30.8; 3.2) |
| 10⁸ TCP (no stats) | **identical**, all 3 VERIFY OK | **23.15 s** (20.6; 2.5) |
| 10⁹ SOS, depth 2 + stats | **identical**, all 3 VERIFY OK | 282.50 s (bs 75.4, dm 201.8; 277.6; 4.9) |
| 10⁹ SOS, depth 1 + stats | **identical**, all 3 VERIFY OK | 276.25 s (bs 76.0, dm 194.4; 270.8; 5.5) |
| 10⁹ TCP (no stats) | **identical**, all 3 VERIFY OK | **201.66 s** (bs 54.3, dm 142.3; 197.0; 4.7) |
| `t_mn_grid 0.25 25` (SOS) | **not run**: my wrapper `stdbuf -oL` hid the binary from `mnrun.sh`'s libsma detection, so it launched as OSHMEM (heap 513 MiB, `shmem_malloc` of the 8 GiB pool failed at init). Not a transport result; `COMM_SHMEM_IMPL=sos` is the override | — |

**SOS is 36–47 % slower than TCP at 3 real nodes** (10⁸ 33.9 vs 23.2 s; 10⁹ 276–283 vs 201.7 s), where at 2 nodes the
two were equal (§2.4). The stats runs show the fabric at 0.017–0.019 GB/s per APU (≈ 70–75 MB/s per node, 3.78 GB per
APU at 10⁹) and 99.8–99.9 % busy, against the NIC's 119 MB/s at 2 nodes: with two peers per node the sockets
provider does not keep the link full. Likely cause (hypothesis, not measured): libfabric's sockets provider serves
all peers from one progress thread per process, and each node now receives from two peers at once; TCP's transport
has a stream per peer. Not a property of the transport code (the same calls at 2 nodes run at line rate), and not a
statement about the target (RDMA NICs, no software progress thread). The TCP walls are the ones to use for the
3-node aac6 reference.

##### 3.2 The xGMI time hidden under the fabric at 3 real nodes (the general map; X13b's missing case)

"Link time hidden" = the push kernels' stamped xGMI link time that overlaps a fabric stage (X13b's measure), per node
as `node / node view` (the per-APU-thread sum, and the union over the node's four APU threads). "Stage both/xGMI" = the
host-side xGMI stage overlapped. Nodes 0 and 1 run the tree's g = 2 level (pairwise) and the g = 3 level; node 2 only
the g = 3 level (the general map), so node 2 is the clean general-map number.

| run | depth | node 0 (s24-16) link hidden | node 1 (s24-26) | **node 2 (s24-30, g = 3 only)** | stage both/xGMI n0 / n1 / n2 | fabric busy/span | span per APU n0 / n1 / n2 |
|---|---|---|---|---|---|---|---|
| 10⁸ | 1 | 21.8 % / 26.0 % | 22.1 % / 25.2 % | **0.0 % / 0.0 %** | 64.1 / 61.1 / 0.0 % | 99.0–99.1 % | 23.61 / 23.35 / 21.78 s |
| 10⁸ | **2** | **73.5 % / 72.7 %** | **73.5 % / 73.7 %** | **73.1 % / 72.9 %** | 70.2 / 68.3 / 43.6 % | 99.4–99.5 % | 22.09 / 22.15 / 20.24 s |
| 10⁹ | 1 | 4.9 % / 7.5 % | 5.2 % / 8.7 % | **0.0 % / 0.0 %** | 32.1 / 42.1 / 0.0 % | 99.8 % | 195.8 / 213.0 / 212.4 s |
| 10⁹ | **2** | **72.9 % / 68.3 %** | **75.2 % / 70.6 %** | **78.3 % / 75.5 %** | 66.3 / 63.6 / 50.3 % | 99.9 % | 222.0 / 223.4 / 221.4 s |

**At 3 real nodes depth 2 hides 73 % (10⁸) and 73–78 % (10⁹) of the xGMI link time under the fabric on every node,
against 0 % on the general-map-only node at depth 1** (and 5–26 % on the nodes that also have the pairwise level) —
the same 3/4 structural ceiling (3 of K = 4 chunk pushes) X13b measured at 2 real nodes over TCP (74.3 %) and on
loopback at size 3 (72.6 % node 2, 73.1 % node 0). The general map at 3 real nodes is now measured, over SOS.

What it is worth here: nothing visible — the xGMI link time is 0.02–0.06 s per node against 20–220 s of fabric (the
1 GbE is ~10³× slower than xGMI; X13b's "ceiling min/(x+f)" is 0.1–1.9 % in these runs). The walls: 10⁸ depth 2 −2.6 s
(−7 %), 10⁹ depth 2 +6.2 s (+2.3 %; its fabric time per APU is 221 vs 195–213 s at depth 1 for the same 3.78 GB — the
link's rate varied between the runs, 0.017 vs 0.018–0.019 GB/s per APU; single runs, and the depth cannot move a link-
bound wall by more than its 0.2 % ceiling). The one-at-a-time inter stage's held time rises (10⁹ node 0: 0.07 → 106 s,
of it the fabric idle 0.04 s): the next exchange waiting behind the busy fabric, as designed. Depth 2's scratch per
APU thread 8.5 → 11.2 MB (10⁸, as X13b). SOS pool peak (PE 0): 85 MiB at 10⁸, 571 MiB at 10⁹ (3 nodes).

#### 4. Open issues

1. **OSHMEM across nodes hangs** (§1): UCX's tcp transport needs the target's worker progressed and the serialised
   `shmem_long_test` poll does not progress it. A possible fix (untested, no node time left): in the serialised wait loop
   call something that progresses the default worker (`shmem_ctx_quiet(SHMEM_CTX_DEFAULT)` every N polls). Low value:
   OSHMEM 4.1 lacks the target form anyway; SOS is the multi-node SHMEM on aac6.
2. **`t_mn_grid` at 2 and 3 real nodes not verified**: `0.5 27` does not finish in 600 s over the 1 GbE at 2 nodes (§2.1,
   line rate, not a hang); the quick `0.25 25` at 3 nodes did not launch (wrapper, §3.1). Next time: `COMM_SHMEM_IMPL=sos`
   and a 20-minute budget, or `0.25 25` at 2 nodes. The multi-node product tier is exercised end to end by every ecalc
   run above (all identical), but its dedicated shapes test is not.
3. **The transport's pool must be sized per run**: 10¹⁰ at 2 nodes needs 8479 MiB (> the 8192 default and TARGET.md §4's
   value). For D2 / the integrator: `COMM_SHMEM_POOL_MB` from the largest exchange per APU thread (§2.2).
4. **SOS at 3 nodes is 36–47 % slower than TCP** (§3.1), equal at 2 — the sockets provider with two peers per node. Not a
   target concern; for aac6 multi-node timing use TCP.
5. `mnrun.sh`'s libsma detection stops at the first non-`env` word: wrappers (`stdbuf`, `numactl`, `timeout`) defeat it.
   Not changed (no node time to test a change); `COMM_SHMEM_IMPL=sos` works around it.

No code changed; no file outside `results/TARGET.md §S13d` touched. No default changed.
