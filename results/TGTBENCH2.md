# TGTBENCH2 — the user's second set of target tests, assessed against the ecalc design (2026-10-06, branch `tgtbench2`)

Input: `results/TGTBENCH2_raw.md`, received about 18:40 EDT and kept verbatim. The tests are the user's, run **on the target** (576-node
MI300A, 8 Cassini NICs per node) with a generic benchmark harness, **not ecalc**. Times are Eastern.

Labels:
- **(m, target)**: measured on the target by the user.
- **(m, aac7)** / **(m, aac6)**: measured by us.
- **(m, layout)**: the C binary's own `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` print, on the aac7 login node, with no HIP calls.
- **(mod)**: modelled.
- **(a)**: assumed.

Base: `origin/b7-vslot` c058964 (B7ACCT's v-slot accounting, not merged). No compute-node work, no jobs, no network tests. Login-node
work was in `aac7:~/b7acct` (build aa86cac = b7-vslot's code); outputs are in `~/b7acct/tgtb2/` and the script is `~/b7acct/tgtb2.sh`.

## RESUME (2026-10-06 19:40 EDT): complete

- Branch `tgtbench2`:
  - the model profile, the estimate driver and the raw file (cdc7547);
  - this report and the docs (the commits after it).
- Pushed; not merged. Nothing is armed and nothing is running.
- Everything here is a proposal. Nothing below changes runtime behavior, the launch line or `TARGET_DIGITS`.

## 0. Summary

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

## 1. Item by item

Columns: the user's value | what ecalc assumes today (where) | consistent? | implication | action.

### M2 — "updated constants"

| constant | user's value | ecalc today | consistent? | action |
|---|---|---|---|---|
| DEVICE_MEM(_PER_APU) | 93.36 GB/APU (m, target; n = 5, 0 % spread) "(−21 % vs 117.975 GB assumption)" | 93.36 GB/APU = 373 GB/node from newbench1 A6, used by CAP17/TGT17 (results/CAP17.md §1; TGT17 §1 bar (a)) and B7ACCT §5. **No ecalc record assumes 117.975 GB** (grep: no hit) | **yes**, same number, now n = 5 | **Adopt** as `mn_model.TARGET_DEVICE_EDGE_GB = 373.44` (m, target), used by `--fabric target-m`'s edge line. The "−21 %" refers to someone else's assumption |
| VMM_MAP_RATE | 0.010 s/GB (m, target) "7× faster than 0.070, init 6.9 → 1.0 s" | `MAP_RATE` 0.065 default, 0.070 aac7 (`mn_model.py:805, 814`), measured with `t_alloc` hipMalloc. ecalc's own VMM pools map at 0.043–0.047 s/GB (7.0.3) and 0.028 (7.2.4) (results/C16.md §2.7, m aac7) | plausible: a faster node or driver. **The unit is not stated** (per APU-GB mapped by one APU, or per node-GB) | **Adopt as a labelled target profile** (`target-m`, §3). The aac7 constants are not changed. The "6.9 → 1.0 s" is 0.070 or 0.010 × 98.6 GB of hypothetical host spill. It is not an ecalc quantity, and ecalc's init is not that product (§3) |
| HOST_MEM_PER_APU | 256 GB hipHostMalloc limit (m, target) | ecalc pins only the seed buffers (2 × 8 GiB/node, `as_seedbuf`) and the comm_ofi `host` pool form (off by default) | no conflict | Record as `TARGET_HOST_PIN_APU_GB = 256.0` (informational) |
| PROCESS_OVERHEAD_1PE | 28.0 GB (m, target) "was 86.0 GB modelled" | **86 GB** is ecalc's modelled fixed cost per *ecalc engine process* at the target: arena floor, tables and init host (P16 §3; 06 §3.2 L119; 05 PS14; 03 §7 L514). WISHLIST §2.2 already logged 28 GB as "measured, not by ecalc" | **not comparable**: different program and different quantity | **Question for the user** (Q4): is 28 GB host RSS, device use, or both, and of which process? It does not change the design (one process per node; 86 GB was the argument against 4 PEs/node, which is rejected anyway) |
| NTT_THROUGHPUT_PER_APU | 655.21 GB/s (n = 65 536, n = 20 runs) | ecalc's NTT at 2³¹: 117 ms per transform (01 §8, RESULTS §33). About 1.2 TB/s of HBM traffic (inferred: 4 passes × read+write × 17.2 GB). Modmul 1.31–1.34 Top/s per APU (m) | **not the regime**: a 512 KiB plane is L2-resident; ecalc's planes are 4–17 GB | **Reject as a model input** (C3 below) |
| TRANSPOSE_THROUGHPUT_PER_APU | 2601.54 GB/s (n = 4096) | ecalc's packs are measured inside the four-step (01, 03) | not the regime | **Reject** |
| INJECTION_BW_PER_APU | 25 GB/s "(conservative, A3 unmeasured)" | `--bw` cases: 11 (m aac7 efficiency, 4 NICs), 47 (a: 8 NICs at the aac7 efficiency), 100 (a: line rate) (EST17 §1) | it is an assumption, not a measurement | No adoption. The 25 GB/s row is in §3 (369.6 s) |
| XGMI_EFFICIENCY | 1.0 (all direct links) | K4 mesh; our push reaches 93–97 % of the links (m, aac6/aac7; 06 §3.2) | **yes** | none |

### M3 / D3 / Issue B1 — "memory constraint, 26 % oversubscribed, mitigate with hipMallocManaged host spill"

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

### Smoke tests, regression detection, V3 data integrity

- **Smoke 3/3 and regression 21/28:** these are the harness's own self-consistency checks against its own earlier runs. They contain no
  ecalc number.
  - The C1 line itself is inconsistent: its "n = 10 mean (0.801 s)" here vs "μ = 80.801" in C1.
  - Relevance: none to the design.
  - Action: none. Note only that "FAIL: B1 4PE anomaly" concerns multi-PE process memory, which ecalc does not use.
- **V3 (309 CSV files parse):** harness hygiene. No action.

### V4 / M1 — "576-node runtime 251.5 s ± 3.6 %; compute 250.0 s, communication 1.0 s at 25 GB/s, init 0.5 s"

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

### Issue log B1–B3

- **B1 (memory, CRIT):** see M3. It is stale and rejected; the real constraint is handled by §4.
- **B2 (multi-node access, gate):** **agree.** A3, A4, A5, A7, B5, B7, D1 and D4 need ≥ 2 nodes.
  - For ecalc the critical ones are:
    - A3: comm_ofi's per-node rate on 8 NICs, `t_comm --bw` at 2 nodes;
    - A4: `ecalc` 10¹⁰/node with `MN_COMM_MARK=1` at 2–64 nodes;
    - the 2-NICs-per-APU `COMM_OFI_NICS` run.
  - Action: carry into WISHLIST §0.1/§1.3/§1.4.
- **B3 (4-PE anomaly):** 1 PE 28.0, 2 PE 172.9, 4 PE 35.4 GB is not monotone, which suggests a measurement artefact. Relevance to ecalc is
  **none** (one process per node, PS14; the 4-PE form was rejected 2026-10-05). Action: none.

### D1 / L1 / L2 / L3 / L4 — the `fi_enable(-28)` root cause and the launch environment

- **Root cause:**
  - The launcher. `oshrun`/`mpiexec` is unavailable in their sandbox, and Slurm-native `srun --ntasks` works: 12/12 configurations,
    7 tests unblocked.
  - That fits our records: WISHLIST §0.1 doubted the `cxi_core` explanation, and **our launch line is already Slurm-native**
    (`srun -N 576 --ntasks=576 --ntasks-per-node=1 …`, docs/TARGET.md §4). aac7 runs plain `srun` with Cray OpenSHMEMX.
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
- **Proposal for docs/TARGET.md §4** (commented, **not adopted**; the user decides):
  - `FI_UNIVERSE_SIZE=4096`: ≥ 4 × 576 = 2304, and their largest tested value;
  - `FI_LOG_LEVEL=warn`;
  - **`SHMEM_SYMMETRIC_SIZE=2048M`** beside our `SHMEM_SYMMETRIC_HEAP_SIZE`/`XT_SYMMETRIC_HEAP_SIZE`. Their environment used
    `SHMEM_SYMMETRIC_SIZE=512M`, which is below our 1536 MiB pool plus margin. If the site's profile exports it, it could shadow ours.
    `comm_shmem` would then stop with rc 8 only if it reads that variable (TARGET.md §4 "the pool and the heap").
  - None of these changes a digit or a buffer. All three are environment variables for libfabric and SHMEM.
- **100 ms HIP-init stagger per PE:** this is for several PEs per node initialising HIP at once. ecalc has one PE per node and already
  initialises SHMEM before its HIP threads (`COMM_INIT_EARLY=1`, the default; 00 §1.3 phase 1). It does not apply. Action: none.
- **SHMEM_SYMMETRIC_SIZE=512M** (L2): see the proposal above.

### A6 — memory edge extended

- Device 93.36 GB/APU (0 % spread): **adopted** (M2).
- Host 256 GB/APU: recorded.
- VMM 0.010 s/GB: profile (§3).
- Process overhead 28 GB: Q4.
- "265.4 vs 210.1 TB": rejected (M3).

### A8 / D2 / D6 / B8 — xGMI: SDMA 50–58 GB/s per directed pair, uniform; SHMEM intra-node 1.8–38 GB/s; a fully connected mesh

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

### B2 (detailed) — device-heap GPU-direct vs host-staged: +62 % at 16 MB, −25 % at ≥ 64 MB

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

### B6 / B9 / D5 / B4 — barriers, collectives, atomics, synchronization

- Barriers: 2 PE 3.89 µs, 4 PE 5.67 µs (intra-node).
- Collectives: alltoall, allgather and allreduce scaling.
- CAS contention.
- **Relevance: low.**
  - ecalc's barriers and small collectives are inter-node: one PE per node, Cray SHMEM, `mn_model` `coll()` with `--lat`.
  - The bulk all-to-alls are ecalc's own puts, not `shmem_alltoall`.
  - Signals are put words, not CAS.
- B4 (1-PE synchronization): no data.
- Action: none. Inter-node latency at 64–576 PEs stays WISHLIST §1.5/§1.7.

### C1 / D3 — single-node baseline: n = 10, μ = 80.801 at 10¹⁰ digits, CV 5.0 %; "validates local-factor 1.22 for ROCm 7.0.3 vs 7.2.4"

- **It is not ecalc.** ecalc at 10¹⁰ on one aac7 node:
  - **25.98–27.71 s at ROCm 7.0.3, 26.18–26.27 s at 7.2.4** (m, aac7, results/C16.md §2.7);
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

### C2 — sustained thermal: 300 s, +1 °C, no throttling, clocks stable

- This **supports a ≈ 4–5 min run** (3.76 × 10¹³: 222.7 s at bw 47; 4.08 × 10¹³: 282.4 s; mod) on one node.
- Limits:
  - The load was the NTT/transpose microbenchmark, not ecalc's mixed HBM + xGMI + NIC load.
  - One node, not 576 (facility power).
- **Action:** update WISHLIST §3.3 to PARTIAL with data. Still wanted: the same during `ecalc 100000000000`.

### C3 / C3a / D5 (bimodal) — NTT 655 GB/s at n = 65 536, transpose 2602 GB/s at 4096, bimodal 31.8 / 15.3 GB/s at 4096

- These sizes are 512 KiB and 32 KiB. They are cache- and launch-bound, three to five orders of magnitude below ecalc's transforms
  (2²⁹–2³¹ points per plane, 4–17 GB; 01 §2).
- ecalc's rates are measured on its own kernels:
  - 2³¹ in 117 ms (RESULTS §33);
  - modmul 1.31–1.34 Top/s per APU (RESULTS §19);
  - the pieces' local passes, calibrated on full runs.
- The "1.51 PB/s at 576" is 655 GB/s × 2304, and says nothing about ecalc.
- **Action: reject as model inputs.** The bimodality at 4096 (L1-capacity) does not concern ecalc's batch tier either: its products are
  ≥ 2¹⁰-point b1 passes inside larger planes.

### M1 — see V4.

## 2. What changes in the records

- **Adopted as model constants** (labelled, behind `--fabric target-m`; the default `target` profile is byte-identical):
  - `TARGET_DEVICE_EDGE_GB = 373.44` (m, target);
  - `TARGET_M_CONSTS = {MAP_RATE: 0.010}` (m, target);
  - `TARGET_HOST_PIN_APU_GB = 256` (m, target, informational).
- **Docs:** docs/TARGET_WISHLIST.md (§0.1 status, §2.1, §2.2, §3.1, §3.3, and a TGTBENCH2 header line), docs/TARGET.md §4 (the proposal
  block), docs/code/06_EVALUATION.md (dated note), docs/code/05_DECISION_REGISTER.md (rows marked PROPOSED), ecalc/README.md and 04
  (the profile row).
- **Rejected, with reasons:**
  - M3's host spill;
  - V4/M1's 251.5 s and its 1.0 s communication term;
  - C3's kernel rates as model inputs;
  - the claim that C1 validates the 1.22 factor;
  - SDMA for xGMI.

## 3. The 576-node estimate with the target profile (4.08 × 10¹³, `ecalc/tgtb2_est.sh old-new`, all mod)

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

## 4. Target size (the user, 18:30 EDT: "use what is fast and relatively close to the limit")

### The method

- `~/b7acct/tgtb2.sh` runs `MN_PLAN_ONLY=<T>:576` and `BS_LAYOUT_ONLY=<T/576>:576 ./ecalc <T> /dev/null` with TGT17's launch-line
  environment plus `COMM_OFI_PLAN_CXI=1` (trap 20).
- The build is b7-vslot (aa86cac).
- device = the `vslot:` line's `device_with` = planes + arena_with_room + ofi_pool + 4 × v-slots per APU.
- Swept from 3.0 to 4.1 × 10¹³ in 5 × 10¹¹ steps, then 10¹¹, then 10¹⁰ around every step. All are (m, layout).
- `plan check` was OK everywhere (1236 products below 4.0 × 10¹³, 1238 from 4.0).
- `mem_model.py --check-c` on the 3.76 × 10¹³ layout: **exact** (0 terms not exact; vslot, device + vslots and node + vslots to the byte).

### Layout tiers

v-slots are 6.707 GB/APU = 26.83 GB/node everywhere in this range.

| T (digits) | arena | device (≤ 368?) | node / with v-slots | pieces node 0 / critical path (tree + recip + div) |
|---|---|---|---|---|
| 3.0–3.15 × 10¹³ | 193.27 | 350.64 | 365.90 / 392.73 | 82–86 / 98 |
| 3.2–3.3 × 10¹³ | 197.57 | 354.94 | 370.20 / 397.02 | 88–90 / 100 |
| 3.35–3.5 × 10¹³ | 201.86 | 359.23 | 374.49 / 401.32 | 92–94 / 100–102 |
| 3.55 – **3.7104** × 10¹³ | 206.16 | 363.53 (yes, 9.91 under the edge) | 378.79 / 405.61 | 101–103 / **107** |
| **3.7105 – 3.896 × 10¹³** | **210.45** | **367.82 (yes, the largest tier ≤ 368; 5.62 under the edge)** | 383.08 / 409.91 | 103–111 / 107 → 109 → 111–127 |
| 3.897 – 4.0815 × 10¹³ (today's target 4.08) | 214.75 | 372.12 (no; 1.32 under the edge) | 387.37 / 414.20 | 111–119 / 127–133 |

### Critical-path piece steps inside the 367.82 GB tier

| range | critical path (tree + recip + div) | change |
|---|---|---|
| 3.7105 – 3.769 × 10¹³ | **107** (42 + 49 + 16) | — |
| 3.770 – 3.8209 × 10¹³ | 109 (42 + 49 + 18) | the division 16 → 18 |
| 3.8210 – 3.896 × 10¹³ | 111 → 127 | the tree level-8 group 42 → 60 |

### Modelled walls (`ecalc/tgtb2_est.sh sizes`, `--fabric target-m`, ROCm 7.2.4, no-write / write @ 1.0 GB/s, mod)

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

### The proposal (the user decides): `ecalc 37600000000000`

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

## 5. Questions and decisions for the user

1. **Target size:** 3.76 × 10¹³ (device 367.82 GB, 5.62 under the edge, 222.7 s mod) or 3.71 × 10¹³ (363.53 GB, 9.91 under, 221.9 s)? Both
   replace 4.08 × 10¹³ (372.12 GB, 1.32 under the edge, 282.4 s).
2. **Launch-line environment** (proposal in docs/TARGET.md §4): add `FI_UNIVERSE_SIZE=4096`, `FI_LOG_LEVEL=warn` and
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
