# TARGET_TASKS.md — the work that needs the target machine (handoff note, 2026-09-23; updated 2026-09-27 for the user's decisions of that day)

These tasks cannot be done on aac6. They are left for a later agent working **on the target
system**: 576 MI300A nodes, a K4 xGMI mesh inside each node, HPE Slingshot-2 in a dragonfly
(diameter 3), two 400 Gb/s NICs per APU (eight per node, 100 GB/s per APU per direction), and
SHMEM (Cray OpenSHMEMX expected; rocSHMEM or OpenSHMEM acceptable). On aac6 (Phase 13d S), Sandia OpenSHMEM works across real nodes in the target form; OpenMPI 4.1 OSHMEM hangs across nodes and must not be used there. See PLAN.md §25.
They come **after** every aac6 task in `TASKS.md`. The procedures live in `docs/TARGET.md`;
this note sets the order, the gates and what to hand back.

## Where things stand when you start

- **2026-09-27 (the user's decisions; supersedes the figures of the next bullets, kept as history)**: the target runs on
  **four primes** (`ECALC_NP=4` on the launch line: three primes cannot hold its pieces — the plan check refuses them,
  results/P15.md) with the defaults of 2026-09-27 (`ecalc/README.md`'s header: among them `BS_SEED_FILL=128`, `BI_MUL1_FAST`,
  `NEWTON_RECIP_MID`, `DIST_TWREC`, `ECALC_CORR_PATCH=2`, the **packed** part file `ECALC_OUT_PACKED=1`, `MN_OUT_EARLY=1`,
  `ECALC_ODIRECT=auto`; the top set off) and `ECALC_MEM_GUARD_GB=6`, `COMM_SHMEM_ROUND_MB=1024` on the launch line
  (`docs/TARGET.md` §4). **4.25 × 10¹³ digits: 256.0 s (4.27 min) without the disk write; 285.7 s (4.76 min) with it at
  0.6 GB/s, 272.0 s at 0.8, 251.5 s at 2.0; 416.0 GB per node; 182 pieces (88 + 66 + 28), 1.2 % below the step at 4.29 → 4.30 ×
  10¹³** (all modelled; the fabric and the write rate with 576 writers assumed; `estimate.py --target`). The model is within
  −3.1 … +2.0 % of the measured 10¹¹ series of RESULTS §86 (`mn_model.py --calib15b`). The digits are converted to ASCII **off
  the clock** (`tools/unpack_digits`, T4b).
- **The production target is 4.25 × 10¹³ digits on 576 nodes** (RESULTS §82; it was 4.4 × 10¹³ until Phase 13d found that
  size past two grid steps): ≈ 3.9 min modelled (234 s), 452 GB per node, 185 pieces on the critical path, 1.2 % below the
  step at 4.29 → 4.30 × 10¹³. Its top node's share, 7.64 × 10¹⁰ digits, was run on one aac6 node: 137.9 s, 354 GB.
- **The defaults are the chosen design** (Phase 13c): three primes, `NTT_MODMUL=1`,
  `RNS_STRATEGY=auto`, `ECALC_PLANE_CAP=2^31`, `MDB_SHIFT_CHUNK_MB=1024`,
  `COMM_ALLTOALLV_DEPTH=2`, `NTT_B1R=3`, `NTT_PLAN=1`. Every other option is one switch away
  (`ecalc/README.md`).
- **`ecalc` takes the TOTAL digit count** at every size, not the per-node share
  (`docs/TARGET.md` §4 launch line).
- **The 576-node figures are modelled** from inputs measured on aac6. Five inputs are
  **assumed**, and measuring them is task T1.

## The tasks, in order

| # | task | procedure | gate / what to hand back |
|---|---|---|---|
| T1 | **Measure the assumed inputs**: injection bandwidth per APU (assumed 100 GB/s), the overlap of the two fabrics at depth 1 and 2 (depth 2 measured 0.74 on two aac6 nodes over 1 GbE), per-message cost (assumed 2 µs), the cost of one chunk round `T_ROUND` (assumed 0.03 s, range 0.01–0.1: aac6's loopback noise hid it), the part-file write rate (assumed 2 GB/s per node; *Phase 15*: **the prior is now 0.6–0.8 GB/s single-stream** — `/ssd0` is Lustre over Slingshot, 122 TB shared, measured on a target node by the apumult catalog; measure one node, then 64 writing at once, and read the number of storage targets (`lfs df`) and the stripe count (`lfs getstripe -d`): TARGET §6 item 5), the device mapping rate, checkpoint bandwidth (the same Lustre: there is no node-local disk; **no digit or checkpoint file to `/tmp`**, likely tmpfs = HBM: check with `df -h /tmp`, TARGET §6 item 10), global-link taper, and the node's memory edge (aac6: ≈ 524 GB device + host) | `docs/TARGET.md` §6, items 1–9 | each value with its run and log line; then **regenerate** `results/DESIGN_TABLE.md` (`design_table.py --bws … --lat … --write-bw … [--mrun]`) and say whether the chosen row is still on the Pareto front and still the right choice for the target size |
| T2 | **Bring-up in order**: transport sanity (`t_comm`, `t_dist`, `t_mn_grid`), then 2 → 4 → 64 → 576 nodes | `docs/TARGET.md` §4 (launch line), §5 steps 1–5 | every step `VERIFY OK` on every node and identical to the reference where one exists (`ecalc/digcmp.sh <outfile> <reference>`: the packed parts through `unpack_digits --cmp`); `ECALC_RECHECK=1` after the large runs; *2026-09-27*: these steps run with **`ECALC_CHECKPOINT=1`** (the budgeted top set) and the launch line's `ECALC_NP=4`, `ECALC_MEM_GUARD_GB=6`; two walls per run |
| T3 | **Confirm the target size sits below the grid step** before the headline run | *2026-09-27*: with the launch line's environment, **`ECALC_NP=4`** included: `ECALC_NP=4 COMM_SHMEM_ROUND_MB=1024 MN_GROUPS=2,4,8,16,32,64,192,576 MN_PLAN_ONLY=42500000000000:576 ./ecalc` (measured on aac6's login node, results/DOC15/plan576_np4_r1024.txt: `plan summary` 180 / 182 pieces, `plan check … OK`, `plan pool` 9472 MiB). `MN_PLAN_ONLY=<total digits>:576 ./ecalc` on a login node (Phase 13d L: the C code's own piece counts, no GPU, 0.04 s; `results/L13d_plan576.txt` has 2.0–6.0 × 10¹³) and the piece count from the model (`mn_model.run(fab, 7.64e10, 576, design=…)['pieces']`, `ecalc/design_table.py` for the design), checked against the per-level lines of the §5 step-4 and step-5 runs (`mn: node 0 level l …`, the `dist_mn` pieces). The memory the run will request, per node and with no GPU: `BS_LAYOUT_ONLY=7.38e10:576 ./ecalc 42500000000000 /dev/null` (D is the average per-node share here) | **The last line of the plan-only run reads `plan check … OK`** (with `ECALC_NP=4`; three primes print `plan REFUSED …` and exit 3 — never launch on them), 182 pieces on the critical path at 4.25 × 10¹³ (88 + 66 + 28; 2026-09-27, `NEWTON_RECIP_MID`; it was 185 = 88 + 69 + 28 before `NEWTON_RECIP_CUT`) and a layout inside 480 GB (`ECALC_NP=4 BS_LAYOUT_ONLY=73784722222.2222:576 ./ecalc 42500000000000 /dev/null`: node 387.2 GB by the C layout, 416.0 GB by `mem_model.py` with the host; `mem_model.py --check-c` exact). If the measured piece counts disagree with the plan at steps 4–5, re-locate the step before the run |
| T4 | **The headline run**: 4.25 × 10¹³ digits on 576 nodes (`ecalc 42500000000000`), the launch line of `docs/TARGET.md` §4 (four primes, `ECALC_MEM_GUARD_GB=6`, **no `ECALC_CHECKPOINT`, no top set, no `BS_CKPT_DIR`**: a record timing run) | `docs/TARGET.md` §5 step 6 | both walls (D3: without and with the disk write; modelled 256.0 / 285.7 s at 0.6 GB/s), per-node `mem[rank]` peak against the model (416.0 GB, within 2 %), `VERIFY OK` on all nodes. (Before 2026-09-27 this row also deleted `<outfile>.top`, ≈ 20 TB: the headline no longer writes it) |
| T4b | **Off the clock, after T4**: RECHECK the packed parts, convert them to ASCII, verify the converted output | `docs/TARGET.md` §4 "Off the clock" and §5 step 6b: `ECALC_RECHECK=1` on the same launch line (residue form; ≈ 40 s of reading per node packed, modelled); `tools/unpack_digits` per part (≈ 1.5–2.1 min per node in parallel at 0.6–0.8 GB/s, modelled from results/IO15.md's 179 s per 100 GB — **needs a one-part mode the tool does not have yet**; as built it converts the whole set in one stream, ≈ 20 h); then RECHECK on the ASCII parts and the leading 10¹¹ digits against `results/e_1e11.out` | every node RECHECK OK (packed and ASCII), every conversion's residue check ok (exit 0), the 10¹¹ prefix identical; the times measured; the packed parts kept until all three pass |
| T5 | **Top schedule by measurement**: 3·3 against 9-way (TASKS 3.3; the model says 3·3 by 5 %) | `MN_GROUPS` (`docs/TARGET.md` §3), both at §5 step 4 (10¹² digits) | keep the faster; record both walls |
| T6 | **rocSHMEM put-with-signal** from the host API (TASKS 3.7), only if rocSHMEM is the library | `t_comm` with the rocSHMEM transport | works, or the one switch that avoids it is set and documented |
| T7 | **The transform cache's allocation** (TASKS 3.6): 16 GiB per slot per APU by `hipMalloc`, not from the block pool and not sized to the group | the `transform cache:` init line at 64 and 576 nodes | fits inside the modelled peak, or a sizing fix with its measured saving |
| T8 | **Two endpoints per APU** (PLAN §29 H8, TASKS 6.5): one SHMEM context per NIC | T1 item 1 at one and two contexts | the injection GB/s per APU both ways; adopt only if the second NIC's bandwidth is not already reached with one context |
| T0 | **Size the SHMEM pool before any large run** (Phase 13d S; **the law measured in Phase 14 P2**, results/P214.md). Every exchange of `ecalc` is staged through the pool, per exchange: the pool's peak = 4 APU threads × the largest exchange's send + receive + the control blocks — measured to 0.1 MiB at 10⁸ / 10⁹ / 10¹⁰ on 2 real nodes (84.8 / 847.7 / 8477 MiB of staging), 10⁹ / 10¹⁰ on 4 processes, and with the division in 2 × 2 pieces. Below the cap ≈ 32 n_Q / g bytes (1.78 B per digit per node, the division's A_h mu result exchange); at the cap the receive is a quarter of my share of C. **At 4.25 × 10¹³ on 576 nodes (modelled from the law): 81.6 GB of pool with the defaults — node 525 GB (501 with DM_TIGHT), over 480 — and 45.0 GB with `MN_T_CHUNK_MB=1024` — node 460 GB (434), inside.** The staged pair is a second copy of buffers the device total already counts, so the node total was missing it (the old model's flat 8 GiB). | `MN_PLAN_ONLY=4.25e13:576 ./ecalc` prints `plan pool` (the pool and the heap); on the target read `comm_shmem: pe r: pool peak` / `at the peak` (every PE with `COMM_SHMEM_VERBOSE=1`) at §5 steps 2–4 against `mem_model.py --pool` | `COMM_SHMEM_POOL_MB` from `plan pool` (or `COMM_SHMEM_POOL_AUTO=1` with a device heap), the heap = pool + 512 MiB, `MN_T_CHUNK_MB=1024` (the user's decision) or a transport that stages in rounds / pool-resident exchange buffers (results/P214.md §2), and the node total re-checked against 480 GB |
| T9 | **The general map's overlap at scale** (TASKS 3.5): every full-group product at 576 uses it | `COMM_LAYER_STATS=1` on the §5 step-4 run | the hidden fraction at depth 2 at 576, fed to `--gen-hide2` |
| T10 | **Reminder (the user's decision 12, 2026-09-27): measure striping and waves on the target** — `MN_OUT_STRIPE` and `MN_OUT_WAVES` have no default | `docs/TARGET.md` §6 item 5(b)/(c): at 64 nodes (step 3) the aggregate write with the default layout, with a stripe layout (`MN_OUT_STRIPE=<count>:<MB>:<OSTs>` or `lfs setstripe` on the directory) and with waves (`MN_OUT_WAVES=2`, 4; with `MN_OUT_EARLY=0`, which waves need); again at 576 if the aggregate saturates | the aggregate GB/s per form; a recommendation for the headline's launch line (the user decides) |
| T11 | **Reminder (the user's decision 13, 2026-09-27): test `MN_T_CHUNK_MB=0` against 1024** after the per-round cost is measured (1024 stays the default until then) | `docs/TARGET.md` §6 item 4: `T_ROUND` from the step-3 runs first; then the model (`estimate.py --chunk shift` = 0 against the default: 0 costs +28.5 GB per node at the target, 444.5 GB, modelled) and, if 0 pays and fits, step 5 with both values on the same nodes | both walls and the node peak at each value; keep the faster that fits 480 GB |

## Rules that carry over

- **Every adoption is the user's decision, made on measured data.** A new option goes in
  behind a switch, off by default, with the measurement beside it.
- **Label every number measured, modelled or assumed.**
- **Every session close reports** the estimated maximum digits for a 576-node job and its wall
  in minutes, from the regenerated table — **two walls** (the user's D3: without and with the disk write).
- **Development and bring-up runs use `ECALC_CHECKPOINT=1`** (the budgeted top set); the headline and every record timing
  run do not (the user's decision 11, 2026-09-27).
- **Plans go in `PLAN.md`, results in `RESULTS.md`.** American spelling.
- **Kill processes only by PID or an anchored match.** `pkill -f <script name>` also matches
  the ssh command that runs it, and has cost runs twice (RESULTS §79).
