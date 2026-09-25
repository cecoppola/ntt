# TARGET_TASKS.md — the work that needs the target machine (handoff note, 2026-09-23)

These tasks cannot be done on aac6. They are left for a later agent working **on the target
system**: 576 MI300A nodes, a K4 xGMI mesh inside each node, HPE Slingshot-2 in a dragonfly
(diameter 3), two 400 Gb/s NICs per APU (eight per node, 100 GB/s per APU per direction), and
SHMEM (Cray OpenSHMEMX expected; rocSHMEM or OpenSHMEM acceptable). On aac6 (Phase 13d S), Sandia OpenSHMEM works across real nodes in the target form; OpenMPI 4.1 OSHMEM hangs across nodes and must not be used there. See PLAN.md §25.
They come **after** every aac6 task in `TASKS.md`. The procedures live in `docs/TARGET.md`;
this note sets the order, the gates and what to hand back.

## Where things stand when you start

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
| T1 | **Measure the assumed inputs**: injection bandwidth per APU (assumed 100 GB/s), the overlap of the two fabrics at depth 1 and 2 (depth 2 measured 0.74 on two aac6 nodes over 1 GbE), per-message cost (assumed 2 µs), the cost of one chunk round `T_ROUND` (assumed 0.03 s, range 0.01–0.1: aac6's loopback noise hid it), the part-file write rate (assumed 2 GB/s per node), the device mapping rate, checkpoint bandwidth, global-link taper, and the node's memory edge (aac6: ≈ 524 GB device + host) | `docs/TARGET.md` §6, items 1–9 | each value with its run and log line; then **regenerate** `results/DESIGN_TABLE.md` (`design_table.py --bws … --lat … --write-bw … [--mrun]`) and say whether the chosen row is still on the Pareto front and still the right choice for the target size |
| T2 | **Bring-up in order**: transport sanity (`t_comm`, `t_dist`, `t_mn_grid`), then 2 → 4 → 64 → 576 nodes | `docs/TARGET.md` §4 (launch line), §5 steps 1–5 | every step `VERIFY OK` on every node and identical to the reference where one exists; `ECALC_RECHECK=1` after the large runs |
| T3 | **Confirm the target size sits below the grid step** before the headline run | `MN_PLAN_ONLY=<total digits>:576 ./ecalc` on a login node (Phase 13d L: the C code's own piece counts, no GPU, 0.04 s; `results/L13d_plan576.txt` has 2.0–6.0 × 10¹³) and the piece count from the model (`mn_model.run(fab, 7.64e10, 576, design=…)['pieces']`, `ecalc/design_table.py` for the design), checked against the per-level lines of the §5 step-4 and step-5 runs (`mn: node 0 level l …`, the `dist_mn` pieces). The memory the run will request, per node and with no GPU: `BS_LAYOUT_ONLY=7.38e10:576 ./ecalc 42500000000000 /dev/null` (D is the average per-node share here) | 185 pieces on the critical path at 4.25 × 10¹³ (88 + 69 + 28) and a layout inside 480 GB. If the measured piece counts disagree with the plan at steps 4–5, re-locate the step before the run |
| T4 | **The headline run**: 4.25 × 10¹³ digits on 576 nodes (`ecalc 42500000000000`) | `docs/TARGET.md` §5 step 6 | wall, per-node `mem[rank]` peak against the model (within 2 %), `VERIFY OK` on all nodes, `ECALC_RECHECK=1`; then delete `<outfile>.top` (≈ 20 TB, TASKS 4.5) |
| T5 | **Top schedule by measurement**: 3·3 against 9-way (TASKS 3.3; the model says 3·3 by 5 %) | `MN_GROUPS` (`docs/TARGET.md` §3), both at §5 step 4 (10¹² digits) | keep the faster; record both walls |
| T6 | **rocSHMEM put-with-signal** from the host API (TASKS 3.7), only if rocSHMEM is the library | `t_comm` with the rocSHMEM transport | works, or the one switch that avoids it is set and documented |
| T7 | **The transform cache's allocation** (TASKS 3.6): 16 GiB per slot per APU by `hipMalloc`, not from the block pool and not sized to the group | the `transform cache:` init line at 64 and 576 nodes | fits inside the modelled peak, or a sizing fix with its measured saving |
| T8 | **Two endpoints per APU** (PLAN §29 H8, TASKS 6.5): one SHMEM context per NIC | T1 item 1 at one and two contexts | the injection GB/s per APU both ways; adopt only if the second NIC's bandwidth is not already reached with one context |
| T0 | **Size the SHMEM pool before any large run** (Phase 13d S; **the law measured in Phase 14 P2**, results/P214.md). Every exchange of `ecalc` is staged through the pool, per exchange: the pool's peak = 4 APU threads × the largest exchange's send + receive + the control blocks — measured to 0.1 MiB at 10⁸ / 10⁹ / 10¹⁰ on 2 real nodes (84.8 / 847.7 / 8477 MiB of staging), 10⁹ / 10¹⁰ on 4 processes, and with the division in 2 × 2 pieces. Below the cap ≈ 32 n_Q / g bytes (1.78 B per digit per node, the division's A_h mu result exchange); at the cap the receive is a quarter of my share of C. **At 4.25 × 10¹³ on 576 nodes (modelled from the law): 81.6 GB of pool with the defaults — node 525 GB (501 with DM_TIGHT), over 480 — and 45.0 GB with `MN_T_CHUNK_MB=1024` — node 460 GB (434), inside.** The staged pair is a second copy of buffers the device total already counts, so the node total was missing it (the old model's flat 8 GiB). | `MN_PLAN_ONLY=4.25e13:576 ./ecalc` prints `plan pool` (the pool and the heap); on the target read `comm_shmem: pe r: pool peak` / `at the peak` (every PE with `COMM_SHMEM_VERBOSE=1`) at §5 steps 2–4 against `mem_model.py --pool` | `COMM_SHMEM_POOL_MB` from `plan pool` (or `COMM_SHMEM_POOL_AUTO=1` with a device heap), the heap = pool + 512 MiB, `MN_T_CHUNK_MB=1024` (the user's decision) or a transport that stages in rounds / pool-resident exchange buffers (results/P214.md §2), and the node total re-checked against 480 GB |
| T9 | **The general map's overlap at scale** (TASKS 3.5): every full-group product at 576 uses it | `COMM_LAYER_STATS=1` on the §5 step-4 run | the hidden fraction at depth 2 at 576, fed to `--gen-hide2` |

## Rules that carry over

- **Every adoption is the user's decision, made on measured data.** A new option goes in
  behind a switch, off by default, with the measurement beside it.
- **Label every number measured, modelled or assumed.**
- **Every session close reports** the estimated maximum digits for a 576-node job and its wall
  in minutes, from the regenerated table.
- **Plans go in `PLAN.md`, results in `RESULTS.md`.** American spelling.
- **Kill processes only by PID or an anchored match.** `pkill -f <script name>` also matches
  the ssh command that runs it, and has cost runs twice (RESULTS §79).
