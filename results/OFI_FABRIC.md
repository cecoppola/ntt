# OFI_FABRIC — comm_ofi (libfabric multi-NIC path), NIC use, the fabric limit and the exchange rate versus the NIC rate

Condensed 2026-10-19 (task 8b) from the merged per-item reports; originals in `archive:results_merged_2026-10-19/`, raw data and
drivers in `archive:results_raw_2026-10-19/`. Old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`. Labels: **m** measured, **d** derived
from measured numbers, **mod** modelled, **a** assumed. Times Eastern.

## Topic summary

- **Why comm_ofi exists.** Cray OpenSHMEMX binds one NIC per PE, and ecalc runs one process per node with four APU threads, so
  all of a node's exchange data went through one 200 Gb/s NIC (23.3 GB/s raw, m). One process can drive all four cxi NICs through
  libfabric (93.3 GB/s per node, m, §NIC16_experiments). `comm_ofi` does that for the data of every device exchange of
  `comm_shmem.c`; SHMEM keeps init, control words, signals and barriers.
- **Adopted and current.** `COMM_OFI` defaults to 1 wherever a cxi NIC is present (user decision 2026-10-06; `COMM_OFI=0` restores
  the SHMEM-only path exactly). Chunk 4 MiB and window 64 per NIC stay (tuning is flat, §TUNE17). Digits identical everywhere tested.
  Bench: all-to-all aggregate per node 44-48 GB/s at 4-10 nodes vs SHMEM 12-15 (m, §OFI17); full 10-node run 3532 s -> 1670 s
  (-52.7 %, m, §STD17), after which the part-file write to NFS (897.6 s) is the largest phase.
- **Memory.** Under comm_ofi staging lives in a per-device comm pool (`COMM_OFI_POOL_MB`); the planner, `mem_model.py` and `mnrun.sh`
  count it since OFIMEM (§STD17). B7 (the general map's v-slots, about 13 GB per node outside the layout) was confirmed there; since
  2026-10-12 the v-slots come from the comm pool by default (`COMM_LAYER_VSLOT_POOL`, §XEFF item 2), see ecalc/README.md.
- **Where the rate is still lost.** Layered (inter-stage) exchanges run at 78-82 % of the NIC at 4 nodes but 10-11 GB/s per APU thread
  at 10 nodes against 23.3 raw; direct exchanges (redistribution, results, spills) run near 1.9 GB/s and absorb skew (§XEFF, §C02).
  Of the items proposed in §XEFF: peer order (`rot`, now spelled `shift` in `COMM_SHMEM_PEER_ORDER`; wall-neutral, SHMEM_ITEMS),
  `COMM_XSTATS` (built), VSLOT_POOL + INTER2 (built; INTER2 default on until 2026-10-21, now 0, results/SHMEM.md); item 4 (OFI-native signalling) was shown NOT worth building (X3).
  §C02's `rr`/`winK` schedules were designed and a probe queued; the engine switch was not built.
- **Four PEs per node** (§P16) wins at 4 nodes / 10^10 (26 s vs 35-45 s, m) but loses 3.7x at 8 nodes, 2.4x at 4 x 10^10, does not
  fit memory at the target and needs per-PE plumbing; superseded by the proxy-PE work (`COMM_SHMEM_PE_PER_APU`, `archive:results_merged_2026-10-23/SHMEM_ITEMS_full.md`,
  results/SHMEM.md). The one-PE-per-APU rewrite was not built.
- **uffd.** `FI_MR_CACHE_MONITOR=userfaultfd` is the `mnrun.sh` default (S46, results/CRASHES_HANGS.md §S46); the 46-run soak (§T12)
  found 0 crashes, which is consistent with but does not prove removal.
- **Superseded/dated:** NIC16's own 2-node confirmation (its probes failed on a gres option; the `t_comm --bw` drivers of §OFI17
  replace it); §TUNE17 two-NICs-per-device has no gain on aac7 (one NIC per APU) and is correctness-only there; §P16's open items 1-9
  are history except the `MN_SELFTEST_GROW` finding (a launch of 32 or more node-processes stops at the layered self-test unless the
  switch is set; see ecalc/README.md for its current default).

## §OFI17 — `comm_ofi`: a multi-NIC bulk path for ecalc (Phase 17, branch `p17-ofi`)

**Question/change.** Use all four NICs of a node from one process. Design (m: built and tested): one process per node and the
in-process xGMI stage unchanged; under `COMM_OFI=1` the data of every device exchange of comm_shmem.c (alltoall, alltoallv, allgather,
rounds) goes as `fi_writemsg(FI_DELIVERY_COMPLETE)` chunks (4 MiB, window 64 per NIC) over the NICs of the calling APU thread's
device (one libfabric `cxi` fabric/domain/EP/CQ/AV per NIC; `COMM_OFI_NICS="0,4;1,5;..."` for two NICs per APU); when all chunks to a
peer completed, the existing SHMEM signal word is put and the receiver's protocol is untouched. Registered memory: a comm pool per
device (`COMM_OFI_POOL`: fine-grained device memory by default), sized `COMM_SHMEM_POOL_MB/4 + 256` MiB. About 60 changed lines in
comm_shmem.c plus comm_ofi.c (about 330 lines); endpoints 4 x k per process.

**Unit tests (1 node, m).** t_comm at 2 and 4 PEs, rounds (`COMM_SHMEM_ROUND_MB=1`, chunk 0.25), two NICs per device with a small
window, and the `COMM_OFI=0` baseline: VERIFY OK everywhere, data on the expected cxi devices. ecalc 1e8 at 2 node-processes (HIP build,
fine-grained pools registered with `FI_HMEM_ROCR`): VERIFY OK, digits **identical** to the reference. Bug found and fixed: the cxi
provider returns `mr_mode` without `FI_MR_VIRT_ADDR` (offset-based RMA addresses); the blob now carries base 0 then. Traps: use a
private `MNACCEPT_TMP` when two agents run `mnaccept`; a fresh clone needs `make -C tools` before `digcmp.sh` works.

**Multi-node (m).** `t_comm` at 2 nodes: VERIFY OK both modes. `mnaccept --only unit,e9,mn` with `COMM_OFI=1 MNRUN_NODES=2`: 16 passed,
0 failed, 1093 s, all sizes identical. All-to-all aggregate (`t_comm --bw`, 4 APU threads per node, 4 MiB slab):

| nodes | SHMEM GB/s | OFI GB/s | NIC spread |
|---|---|---|---|
| 2 | 19.83 | 25.01 | SHMEM cxi0 only, OFI all 4 |
| 4 | 14.92 | 48.51 | same |
| 8 | 11.71 | 44.44 | same |
| 10 | 12.29 | 44.11 | same |

SHMEM stays on one NIC per node and falls as nodes compete for it; OFI holds 44-48 GB/s from 4 nodes (2.3-3.8x). Full ecalc, 1 x 10^10
digits per node, `MN_COMM_MARK=1`, 2 tries each, all VERIFY OK and digit-identical:

| nodes | digits | SHMEM total | OFI total | OFI |
|---|---|---|---|---|
| 2 | 2 x 10^10 | 86.39, 86.02 s | 75.27, 76.23 s | -12 to -13 % |
| 4 | 4 x 10^10 | 116.12, 120.72 s | 87.26, 94.39 s | -22 to -25 % |

**Verdict: adopted** (RESULTS §106; user decision 2026-10-06 makes it the default on cxi). Closeout checks (m): `mnaccept --only mn`
only exercises comm_ofi when `COMM_TRANSPORT=shmem` is set (its default transport is TCP); with it and `COMM_OFI` unset, 5 passed, 85 s
(409 s over TCP), every device on its own cxi, so the new default turns OFI on by itself once SHMEM is the active transport. An early
attempt ran against a stale clone (no OFI engaged); not a code problem. `mnaccept --only unit,e9`: 10 passed, 1 failed (`t_mn_grid`
timeout at 1200 s, the known slow TCP-between-2-nodes path on that harness; not a regression).

**Superseded:** NIC16's 2-node confirmation (probes died at an `srun --gres=gpu:24` spec, a CPX-partition idiom; only the single-NIC
`shbw` fallback ran, 22.0 GB/s per PE) is replaced by the `t_comm --bw` runs above.

**Open items at the time:** the SHMEM pool kept full size under OFI (fixed by OFIMEM, §STD17); the budget check did not count the comm
pool (fixed, §STD17); two NICs per APU exercised only as NICs on different NUMA nodes on aac7 (§TUNE17).

## §P16 — Phase 16 P: the fabric limit as an algorithm-design problem on aac7 — four PEs per node

**Question.** Cray SHMEM binds one NIC per PE, so the one-PE-per-node form gives 13.5-14.8 GB/s per node (3.6 per APU thread), falling to
2.8 / 1.0 at 4 / 8 nodes (B16, C16; m). With 4 PEs per node each PE gets its own NIC (7.6-8.2 GB/s per PE at one context, 31-32 per node,
m). Does running 4 PEs per node (each with all four APUs, leaves 4x smaller; no engine change) win?

**Design findings.** Real "one PE = one APU" (NA = 1, prime d on device d mod 4) is a rewrite of the prime<->device binding in rns_mul,
dbig, binsplit and the xGMI layer (`rns_init` needs 4 devices; mn.c `NA 4`; the intra stage is an in-process xGMI push), estimated weeks,
not built. The prototype needs no code: `mnrun.sh 4n` with `ECALC_PLANE_CAP=30 ECALC_CHECKPOINT=0` (new switch `MNRUN_NODES=n` only selects
nodes). Modelled walls (`estimate.py`, **mod**): 4 PEs per node is 2.2-4x at the target (1933 s -> 470-870 s) and 68 -> 40-69 s at 4 nodes
(assumed per-PE rates 8-14 GB/s).

**Measured, job valid set (m, all 11 runs byte-identical to the reference):**
- `t_comm --bw`, on-node transport (Cray XPMEM / NIC loopback): 4 PEs on one node, one context each, **12.3 GB/s per PE at 4 MiB (10.3 at
  256 MiB)**, i.e. 10-12 GB/s per PE, about 60-90x slower than the in-process xGMI push (909 GB/s). 8 PEs on 2 nodes (4 per node): 4.4-9.0
  GB/s per PE, about 29 GB/s per node at 128 MiB slabs vs 13-19 for the one-PE node with four contexts.
- ecalc 10^10:

| nodes | form | PEs | total s | dm s | init s | total - init s | all-to-all GB/s per APU thread |
|---|---|---|---|---|---|---|---|
| 1 | 1 PE | 1 | 33.08 | 6.0 | 13.8 | 19.3 | - |
| 1 | 4 PEs | 4 | 60.58 | 36.6 | 15.0 | 45.6 | 2.35 (all on-node) |
| 2 | 1 PE r1/r2 | 2 | 38.13 / 39.05 | 13.7 / 13.6 | 18.4 / 19.5 | 19.7 / 19.5 | 3.41-3.44 |
| 2 | 4 PEs r1/r2 | 8 | 31.32 / 36.53 | 12.9 / 16.6 | 10.8 / 12.1 | 20.5 / 24.4 | 1.77-1.81 |
| 4 | 1 PE r1/r2 | 4 | 44.64 / 35.37 | 12.8 / 12.9 | 25.4 / 16.1 | 19.2 / 19.3 | 1.93-2.81 |
| 4 | **4 PEs r1/r2** | 16 | **25.87 / 25.96** | 8.1 / 9.4 | 12.3 / 11.2 | **13.5 / 14.8** | 1.42-1.53 |
| 4 | 2 PEs (control) | 8 | 28.86 | 11.1 | 10.9 | 18.0 | - |

  At 4 nodes the 4-PE form wins (25.9-26.0 vs 35.4-44.6 s; compute after init -25 to -30 %); at 2 nodes it is even (shorter wall only
  from a shorter init); on one node it loses 2x (SHMEM replaces xGMI on the node); APU time-sharing costs little in bs. One or two runs
  per cell (init spread in the 1-PE arm 16-25 s).
- Follow-up (m): at **8 nodes / 32 PEs every launch stopped at init** on main (both forms): `mn_selftest_layered(11, 11)` needs >= 32
  rows per rank and gets 16; the 576-node target would stop the same way (inferred from the code). Switch **`MN_SELFTEST_GROW=1`** raises
  the self-test size (self-test then runs at 2^23 points, 13-14 s). With it, 8 nodes / 10^10: **4 PEs 113.8-121.6 s vs 1 PE 30.8-42.2 s
  (3.7x slower)**; single-node recip chain 25-54 s (vs 2.4 s), fabric 0.12-0.14 GB/s per APU thread; digits identical. 4 nodes / 4 x
  10^10: 1 PE 223 s then 93.85 s (B16 measured 87), 4 PEs timeout 1500 s then 228.5 s (0.44 GB/s per thread). Cause not found
  (suspects: progress threads of 4 PEs x 4 contexts competing for the CPU, per-PE resources at 128 endpoints, replicated single-node
  chain; memory pressure at 432 GB per node modelled, not proven).
- Trap (m): `ECALC_CHECKPOINT=1` with several PEs per node fails T2 (the top checkpoint file `<outfile>.top` is one path per node, so the
  node's PEs overwrite each other; PEs 4-7 reported `VERIFY FAILED`). One unexplained 16-PE run with the checkpoint on took 238 s.
- Memory (**mod**, plan figures): the 4-PE form does not fit at the default plane cap (4 x 142.5 = 570 GB at 10^10 on 2 nodes); with
  `ECALC_PLANE_CAP=30` it fits (398 GB at 2 nodes, 364 at 4, 432 GB at 4 x 10^10). At the target (5.276e13, 576 nodes) 1 PE/node is
  417.4 GB per node, 4 PEs per node is 4 x 203-271 GB = 810-1085 GB: **does not fit with any existing switch** (about 86 GB fixed per
  process + 228 GB x per-PE digits fraction, mod), and the per-process budget check knows nothing of the other three.

**Verdict.** Not adopted: a 25-30 % compute gain at 4 nodes does not survive scale-up (loses at 32 PEs and at 4 x 10^10), and it does
not fit the target memory. Recommendation made then: drop the form, spend the effort on a per-node budget and one-PE-per-APU. The need
for one NIC per APU was later met by the proxy-PE design (`archive:results_merged_2026-10-23/SHMEM_ITEMS_full.md`). Standing facts: the on-node SHMEM lane is 10-12 GB/s
per PE, not xGMI; the model over-predicted both arms at 2-4 nodes (calibration points handed to the model owner).

## §C02 — (opportunity C8) the gap between the layered exchange rate and the NIC rate: where it goes at 4 / 6 nodes, three peer schedules, the plan

**Question.** Where does the gap between the layered inter-stage rate and the NIC rate (23.3 GB/s) go at 4 and 6 nodes; would a peer
schedule close it? Design + microbenchmark plan (2026-10-11); **no engine change**; new test code `ecalc/tests/t_a2a_sched.c` and a queued
probe item. Inputs: S48 runs at 6.441e10 digits per node with `COMM_XSTATS=1`, T09, A13.

**Answers (m unless marked).**
1. **4 nodes (power of two, `layeq`): the layered inter stage already runs at 18.2-19.1 GB/s per APU thread = 78-82 % of the NIC** in
   fast mode. XEFF's "10.1 vs 23.3" is a 10-node, 9-peer `layv` figure. A schedule can win at most about 4 GB/s (2-7 s of wall, mod); the
   **slow launch mode** (A13: NIC MCU group A, about 30 % of launches) costs more: the same stage at 12.6-14.2 GB/s (x0.69-0.74), every
   exchange kind slowing alike (`sig` in layeq 2.3 -> 17.1 s; A13 saw 2x `oxe_stall_fgfc` flow-control stalls). Per exchange (dm): 403 MB
   per thread in 21 ms vs 17.3 ms at NIC rate, so the whole protocol/tail overhead is about 4 ms per exchange (d).
2. **6 nodes (`layv`, general map, X2 on): the per-thread xstats rate of 7.0-8.6 GB/s (T06's "2.3x slower") is an accounting artefact of
   `COMM_LAYER_INTER2`**: two stages are in flight and `t_post` counts the overlap twice. From post-to-post spans (sum of `t_call`) the
   stage moves **13.4-15.6 GB/s** (d), 0.73-0.86 of the 4-node rate (not 0.41). Of 64-72 s per thread blocked in `layv` waits, **30-39 s is
   `roff` (waiting for a peer that has not posted: skew)**, the transport-shaped part 33 s (S48 r1/r2). Hand-offs: refit `layv_factor` on
   span rates (the 576-node layv penalty is then nearer its low end); `COMM_XSTATS` could print the span under INTER2.
3. Decomposition of the gap per GB per thread, fast mode (ms/GB; raw NIC 43): 4 nodes achieved 53 (gap 10, 19 %, about all per-exchange
   fixed cost); 6 nodes 64-75 (gap 21-32); 10 nodes 88 (gap 45, 51 %; 73 with `rot`). The schedule term grows with the peer count
   (0 -> 5-15 -> 15 ms/GB at 3 -> 5 -> 9 peers); the fixed per-exchange term is the floor at small g; the slow mode (+22-26 ms/GB at 4
   nodes) is the largest single term at 4 nodes.
4. **Candidate schedules (mod):** `rot` (in code, null on wall, S27); **`rr`** (chunk round-robin over ready peers) and **`winK`** (at
   most K peers open; K = 8-32 is the only knob that could matter at 576 peers) cost no memory or sync, about 50 lines in comm_shmem.c
   (`ready_first`, `rf_post_*`), honest gain -2...-8 s at 4-6 nodes, -5...-18 s at 10 nodes, more only if the slow mode responds (it
   would if it is receiver incast back-pressure, not NIC-side); lockstep `pair` rounds are worse (+15...+60 %); a hierarchical
   two-level all-to-all pays at 576 only if the per-peer cost is >= 8 us.
5. **Probe plan:** stand-alone `t_a2a_sched` (Cray SHMEM, 4 PEs per node, schedules seq/rot/rr/win2/pair paired inside each launch,
   NIC counters `oxe_mcu_meas_18..33` per launch), 12 launches on 4 nodes (about 8 min of hold). Pass rules decided beforehand: (1)
   `rr` or `win2` >= `rot` + 10 % at g = 4, 64 MiB, paired -> build `COMM_SHMEM_PEER_ORDER=rr|win<K>` (comm_shmem.c); (2) the slow-mode
   penalty vanishes only under rr/win2 -> sender-order incast, build and A/B; (3) all within 5 % -> schedule is not the lever, close
   C8 at this scale, keep `winK` as a 576-node question; (4) `pair` >= `rr` -> redesign. Staged A/B after a pass: t_comm VERIFY, gate 1
   and 2 nodes, 4-node ABBA >= 8 rounds with the MCU group recorded, then 10 nodes.

**Status.** The probe item was installed but not yet run when written (4 nodes busy); the engine switch was not built; results of the
probe are not in this file (look in `archive:results_merged_2026-10-23/SHMEM_ITEMS_full.md` and RESULTS.md for later schedule work: `COMM_SHMEM_PEER_ORDER` now takes
`rank` / `shift`). Side finding: the slow mode at 4 nodes outweighs anything a schedule can win (a14's launch-group work is the bigger
lever).

## §XEFF — why ecalc's exchanges underuse the transport, and what to change

**Question.** Design study 2026-10-08 (no runs; inputs S19A p10, S22 at 10 nodes, TIME_MODEL §S19B). Where does exchange time go and what
should be built? Labels: m, d, mod, a.

**Summary (10 nodes, per APU thread; S19A p10 node 0, m).** The SHMEM exchanges take 276.7 s post-to-completion for 1675 GB: **6.05 GB/s**
(equals the model's fitted rate B = 6). Two populations:

| population | bytes | time | rate |
|---|---|---|---|
| layered transform exchanges (inter stage) | 1414.5 GB (84 %) | 140.3 s | **10.1 GB/s** (3 np all-to-alls per product, back to back on the wire) |
| direct mesh-d exchanges (operand redistribution, result out, spills, `mdb_add_shifted`) | about 261 GB (16 %, d) | about 136 s (d) | **about 1.9 GB/s** (d); blocking alltoallv + immediate wait, about 1176 per thread (d) |

Two gaps: direct exchanges (about 110 s per thread above the layered rate, d; each is a sync point absorbing skew <= 68 s per thread at 10
nodes (S22 node spread, m); skew vs protocol split not measured) and layered exchanges (about 80 s, mod): per GB, raw NIC 43 ms -> +14
comm_ofi per-peer protocol (17.5 GB/s) -> +26 nine-peer contention (every sender walks peers in the same order; 12 GB/s) -> +14
per-exchange tail and lockstep rounds (10.1). Staging copies are not the problem (device copies about 1.6 TB/s; two staging copies about
1.3 ms per GB, about 1.4 % of wire time, d). Five HBM copies per v-exchange byte cost about 3 % (d); block transposes 2.8 s per APU (m).

**Findings by area (m unless marked).**
- *Sizes and rounds* (S22 `scale_n10_r1`): tree level 1 (g = 2) 10.75 GB/s per thread; level 2 (g = 10, general map) 6.45; reciprocal
  4.21; division 4.86; whole run 5.40 (total 456.3 s). Every exchange takes the rounds path (`COMM_SHMEM_ROUND_MB=1024`), 580-668 per node
  need more than one round; the round chunk is 114 MB per pair at g = 10 but **1.78 MB per pair at g = 576 (d)**. 88 % of layered time is
  in the >= 537 MB bucket at 10.26 GB/s; buckets below 17 MB sum to about 4 s of 149 s.
- *Transport:* nicbw 23.3 GB/s per NIC; `t_comm --bw` through comm_shmem + comm_ofi 7.7 / 12.3 / 11.0 GB/s per thread at 2 / 4 / 10 nodes;
  chunk/window flat; fit gives w about 17.5 GB/s single peer, about 12 with 9 peers (mod). Code causes: same peer order on every sender
  (`push_peers`, `rounder`: moving incast), head-of-line blocking (sender waits each receiver's offset word in order), delivery-complete on
  every 4 MiB write, signal as a SHMEM put through the process's single PE NIC (cxi0, shared by all four meshes), one exchange at a time
  per communicator. About 10.5 ms fixed cost per exchange (mod).
- *Depth:* layered depth 2 hides the xGMI stage (13.9 s per thread, 6.7 overlapped, 7.3 exposed); the inter stage is strictly serial per
  mesh (fabric busy 94.7 % of the span, which includes tail and drain); `COMM_ALLTOALLV_DEPTH` > 2 cannot help without a second inter
  communicator.
- *Skew:* S22 node spread of `comm_wait` per thread (tree / reciprocal / division) 14 / 26 / 28 s (sum 68 s) at 10 nodes, 0-20 s at <= 4;
  reciprocal's single-node chain 2.28 s minimum but 6.8-22.2 s typical; the same ranks wait most in both arms (S20).
- *APU thread coupling:* layered fabric time differs by max/min 1.10-1.18 across the four threads of a node (up to about 15 %, d); NIC
  binding of data is correct (one cxi per APU), only the control plane is pinned to one NIC (Cray one NIC per PE). One-thread windows under
  `MN_T_CHUNK_MB` (result + spill exchange, barrier, `mn_round_add` on one thread) are a candidate (a); the 2048 gain (-42 s, m) is local.
- *Direct exchanges (d):* 4704 per node minus layered = about 1176 per thread, about 222 MB each, about 116 ms vs 22 ms at layered rate:
  **about 94 ms of skew + fixed cost each**; a product does three blocking redistributions (A, B, X) and Kt x (result + spill
  alltoallv + barrier + one-thread add); none overlaps anything.

**Ranked candidate changes (S = seconds saved per APU thread at 10 nodes ~ wall; 576-node translation aac7-class 2.07 S, target 0.26-0.44 S
plus up to 12-40 s fixed-cost effect; mod / a):**

| # | change (off by default) | S at 10 nodes | memory | effort / risk | outcome |
|---|---|---|---|---|---|
| 0 | `COMM_XSTATS=1` per-exchange phase timers | 0 | 0 | S | built (see ecalc/README.md) |
| 1 | `COMM_SHMEM_PEER_ORDER=rot` rotated ready-first order | 10-40 (central 20, a) | 0 | S, low | built; wall-neutral at 10 nodes (S27; SHMEM_ITEMS) |
| 2 | `COMM_LAYER_INTER2=1` + `COMM_LAYER_VSLOT_POOL=1` (v-slots from the comm pool, second inter mesh per group) | 10-25 (central 18, mod) | neutral | M, medium | built; built-in default since 2026-10-12 (TASKS 27) |
| 3 | `MN_REDIST_FUSE=1` one alltoallv for A, B, X | 3-12 (a) | 0 | S-M | proposal |
| 4 | `COMM_OFI_SIG=1` OFI-native signalling off cxi0 | 10-30 (a) | 0 | M-L, medium-high | **not worth building (X3)** |
| 5 | `COMM_SHMEM_RPIPE=1` rounds pipelined two deep | 5-15 (a) | 0 | M | proposal |
| 6 | skew sources (reciprocal's single-node chain; `mn_round_add` on one thread) | 5-15 (a) | 0 | S-M | diagnose with XSTATS first |

Not recommended (m evidence): chunk/window tuning (flat, §TUNE17); two NICs per APU on aac7; `DIST_MN_SYM_SLABS=1` (about 10 GB per APU
to save copies costing about 1 %); `MN_TOPO_GROUP` (aborts under SHMEM, inert); more `DIST_CHUNKS` (time-neutral). The study also gave
measurement plan M1 (XSTATS: split `ready` into `roff_wait` skew and `sig_wait` transfer, per kind and APU), M2 (`t_comm` A/B), an exact
ABBA recipe for each switch (10 nodes, 6.441e10 digits per node, no write, stop at |t| > t_crit or 16 rounds; S22 paired sd 21 s gives
+/-11 s CI at 16 rounds, m) and implementation notes for items 0-2 (VSLOT_POOL allocates each slot once at the layout maximum and skips
rounds; INTER2 needs a second SHMEM communicator per (level, d) from a fresh id range, a two-entry pending ring, and requires
VSLOT_POOL). Decisions then proposed: X1 (items 0+1), X2 (item 2), X3 (item 4; gated on M2).

**X3 verdict (2026-10-08, M2 measured): item 4 is NOT worth building.** `t_comm --bw 4 5 256 sym`, 2 nodes, ABBA x3 = 6 runs per arm,
DC=1 (default) vs `COMM_OFI_DC=0` (transmit-complete writes, no delivery wait, no fence: an unsafe upper bound for item 4):

| slab | DC=1 GB/s per thread | DC=0 | lift |
|---|---|---|---|
| 256 MiB | 6.66 mean (6.14-7.63) | 6.67 (6.24-7.47) | +0.2 % |
| 64 MiB | 6.78 (6.42-7.14) | 6.79 (6.45-7.21) | +0.1 % |
| 16 MiB | 7.02 | 6.97 | -0.7 % |
| aggregate, 256 MiB | 24.9 | 24.2 | noise |

Spread about +/-8 %; VERIFY OK on both. Gate (>= 15 % lift) failed by two orders of magnitude: delivery-complete costs nothing
measurable on the bandwidth path and the 14 ms/GB "protocol term" is not the delivery wait. Only the F claim (control words leaving cxi0
at 576 PEs) remains untested; XSTATS can show whether it matters. `COMM_OFI_DC` (default 1) and `MNRUN_NODELIST` remain on branch s29 only.
Driver `archive:drivers/ecalc/s29_m2.sh`.


Sections moved to `archive:results_merged_2026-10-24/OFI_FABRIC.md` (2026-10-24 cleanup): §STD17; §TUNE17; §NIC16_experiments; §T12.