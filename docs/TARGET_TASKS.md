# TARGET_TASKS.md — the work that needs the target machine (handoff note, 2026-09-23)

These tasks cannot be done on aac6. They are left for a later agent working **on the target
system**: 576 MI300A nodes, a K4 xGMI mesh inside each node, HPE Slingshot-2 in a dragonfly
(diameter 3), two 400 Gb/s NICs per APU (eight per node, 100 GB/s per APU per direction), and
SHMEM (Cray OpenSHMEMX expected; rocSHMEM or OpenSHMEM acceptable). On aac6 (Phase 13d S), Sandia OpenSHMEM works across real nodes in the target form; OpenMPI 4.1 OSHMEM hangs across nodes and must not be used there. See PLAN.md §25.
They come **after** every aac6 task in `TASKS.md`. The procedures live in `docs/TARGET.md`;
this note sets the order, the gates and what to hand back.

## Where things stand when you start

- **The production target is 4.4 × 10¹³ digits on 576 nodes** (RESULTS §80), but Phase 13d (RESULTS §81) found it sits
  past two grid steps, at 4.29 → 4.30 and 4.39 → 4.40 × 10¹³ (the C code's own plan, `MN_PLAN_ONLY`): 275 s (4.6 min)
  modelled, against 234 s (3.9 min), 452 GB per node, at 4.25 × 10¹³. Which size to run is the user's decision; check the
  one chosen with `MN_PLAN_ONLY=<total digits>:576` before the run (T3).
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
| T3 | **Confirm the target size sits below the grid step** before the headline run | `MN_PLAN_ONLY=<total digits>:576 ./ecalc` on a login node (Phase 13d L: the C code's own piece counts, no GPU, 0.04 s; `results/L13d_plan576.txt` has 2.0–6.0 × 10¹³) and the piece count from the model (`mn_model.run(fab, 7.64e10, 576, design=…)['pieces']`, `ecalc/design_table.py` for the design), checked against the per-level lines of the §5 step-4 and step-5 runs (`mn: node 0 level l …`, the `dist_mn` pieces). The memory the run will request, per node and with no GPU: `BS_LAYOUT_ONLY=7.64e10:576 ./ecalc 44000000000000 /dev/null` (D is the per-node share here) | 214 pieces on the tree levels and a layout inside 480 GB. If the measured piece counts disagree with the model's at steps 4–5, re-locate the step before choosing the size |
| T4 | **The headline run**: 4.4 × 10¹³ digits on 576 nodes | `docs/TARGET.md` §5 step 6 | wall, per-node `mem[rank]` peak against the model (within 2 %), `VERIFY OK` on all nodes, `ECALC_RECHECK=1`; then delete `<outfile>.top` (≈ 20 TB, TASKS 4.5) |
| T5 | **Top schedule by measurement**: 3·3 against 9-way (TASKS 3.3; the model says 3·3 by 5 %) | `MN_GROUPS` (`docs/TARGET.md` §3), both at §5 step 4 (10¹² digits) | keep the faster; record both walls |
| T6 | **rocSHMEM put-with-signal** from the host API (TASKS 3.7), only if rocSHMEM is the library | `t_comm` with the rocSHMEM transport | works, or the one switch that avoids it is set and documented |
| T7 | **The transform cache's allocation** (TASKS 3.6): 16 GiB per slot per APU by `hipMalloc`, not from the block pool and not sized to the group | the `transform cache:` init line at 64 and 576 nodes | fits inside the modelled peak, or a sizing fix with its measured saving |
| T8 | **Two endpoints per APU** (PLAN §29 H8, TASKS 6.5): one SHMEM context per NIC | T1 item 1 at one and two contexts | the injection GB/s per APU both ways; adopt only if the second NIC's bandwidth is not already reached with one context |
| T0 | **Size the SHMEM pool before any large run** (Phase 13d S). The model's `pool` column is flat at ≈ 9 GB, but in the target's form (pool-resident slabs) the pool holds the callers' exchange buffers, and aac6 measured **8479 MiB in use at 10¹⁰ digits on 2 nodes** (5 × 10⁹ per node), above the 8192 MiB default and TARGET §4's value; that run completed only with `COMM_SHMEM_POOL_MB=16384`. The target's per-node share is 15 × larger. Whether those buffers are also counted in the model's device total (so only the pool setting is wrong) or missing from the node total (so the 480 GB fit is at risk) is **not yet known** (TASKS status: the pool model). | read the transport's pool high-water line at §5 steps 2–4 and scale to the target's share; `estimate.py` once the model is fixed | `COMM_SHMEM_POOL_MB` (and the heap size, pool + 512 MiB) set from measurement for the headline run, and the node total re-checked against 480 GB |
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
