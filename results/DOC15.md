# DOC15 — the documentation and the models on the user's decisions of 2026-09-27 (Phase 15, agent DOC)

Branch `p15-DOC` from `int15b` @ c2aedc2 (the worktree's HEAD was 7367e25; the branch was created at c2aedc2). Desk work: the
docs and the Python models; aac6's login node only (clone `~/ntt-DOC15` at c2aedc2, built; scratch `~/DOC15/`), no node job.
Times Eastern. Labels: **measured** (an aac6 run, named), **modelled** (the models' arithmetic), **assumed** (a target parameter
no aac6 run gives).

## 1. The new 576-node figures (`ecalc/estimate.py --target`, 2026-09-27)

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
- The grid step: the code's plan with `ECALC_NP=4` (results/P15/sweep576_np4.txt; the grids are the same at three and four
  primes) and the model agree (tree_max 88 → 100 at 4.30 × 10¹³).

## 2. What changed

### 2.1 The models (commit 5adb2f9)

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

### 2.2 The calibration (`mn_model.py --calib15b`; measured = RESULTS §86, jobs 21550 / 21563, logs `~/fin15/` on aac6)

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

### 2.3 `mem_model.py --check-c` (exact; the protocol's gate)

`BS_LAYOUT_ONLY` on aac6's login node at c2aedc2 (files in `results/DOC15/`), then `python3 mem_model.py --check-c <file>`:

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

### 2.4 The documents (commits ee42962, 983e545, a00a58b, 3173e4c, 8a17bb2)

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

## 3. Tests (all desk / login node)

| command | result |
|---|---|
| `python3 mem_model.py --check-c results/DOC15/layout_{np4,np3,target_np4}.txt`; `… layout_fill0.txt 31 1024 0` | 0 terms not exact in every file (measured C layouts, modelled port) |
| `python3 mn_model.py --calib15b` | worst wall error 3.1 % (§2.2) |
| `python3 mn_model.py --calib15` | unchanged (B0 model) |
| `python3 mem_model.py --p15` | the target on four primes 416.0 GB; B0's rows unchanged |
| `python3 estimate.py --target` / `--ascii` / `--b0` | §1; `--b0` reproduces MD15's 231.1 / 349.6 s, 398.8 GB (three primes) |
| `python3 mn_model.py --D 7.3784722e10 --g 576 --groups 2,4,8,16,32,64,192,576` | 256.0 / 285.7 s, 182 pieces (tree_max 88, recip 66, div 28), 416 GB |
| `python3 design_table.py` | 96 rows in 217 s; its recommended row equals `estimate.py --target` |
| `python3 estimate.py --D 4e10 1e11 --g 1 4 576` | runs; one node at 10¹¹ 197.5 s without the file (measured 193.6) |
| TARGET §9's variable check | every variable named in TARGET.md is in the code or named as a model constant / not merged (`DB_POOL_VMM_PAR`, `_EXTEND`) |

## 4. Open issues (for the integrator)

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

## RESUME

- Done: branch `p15-DOC` at c2aedc2 + 5adb2f9 (models), ee42962 (README), 983e545 (TARGET), a00a58b (TARGET_TASKS), 3173e4c
  (DESIGN_TABLE), 8a17bb2 (APUMULT_STUDY), this report. aac6 `~/ntt-DOC15` at c2aedc2 (built), `~/DOC15/` the login-node
  outputs (copied to `results/DOC15/`). No job was run or is pending.
- Nothing left in the task list; the open issues of §4 are for the integrator / the user.
