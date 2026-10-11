# STD17 — OFI memory accounting (OFIMEM) and the standard 10-node run with all NICs (Phase 17, branch `p17-ofimem`)

Times Eastern (aac7 logs are Pacific: +3 h). Numbers labelled **measured** / **modelled** / **assumed**.

## RESUME (2026-10-06 13:55 EDT) — complete

All three parts done; nothing armed, nothing running (driver exited 13:47 EDT; jobs 12287 / 12294 untouched beyond `--overlap`
steps). Logs on aac7: `~/ofimem17/R/log/` (std17.log, n2_sanity.log, n10_record.log, n10_recheck.log, plan/layout, nodes_start.txt,
ctr_before/after/diff.txt), verdict `~/ofimem17/R/STD17_DONE`. Clone `~/ofimem17` (branch `p17-ofimem`, merged into `main`).
Open: B7's v-slots (§4) are not yet counted in the layout / model.

## 1. Part A — memory accounting under `COMM_OFI` (design)

Before: under `COMM_OFI` (the default on cxi since `e41b47d`) every device exchange stages in the per-device comm pool
(`comm_ofi_alloc`), but the SHMEM pool kept its full staging size and the comm pools (`COMM_SHMEM_POOL_MB`/4 + 256 MiB each) sat
beside it, uncounted by `binsplit_node_bytes`, `as_room_fits`, the `plan pool` line and `mem_model.py`. What SHMEM still carries under
OFI (comm_shmem.c): the control blocks / rings / signal words, the mailbox, and the host-buffer ops (`alltoallv_host`,
`allgather_host`: `set_xo(p, 0)`; only `mn_allgather`'s few words and the OFI blob exchange in ecalc). The smallest correct change:

- `comm_ofi_planned()` (comm_ofi.c): the decision `comm_ofi_enabled` will make, opening nothing (COMM_OFI, else a cxi NIC here, else
  `COMM_OFI_PLAN_CXI=1`); 0 in a build without libfabric.
- `binsplit_shmem_pool_need` under OFI: SHMEM need = 128 MiB (host-op staging; the old floor) + control + mailbox + 256 MiB; the comm pool
  per device = max(one APU's staging, 128 MiB) + its `DIST_MN_SYM_SLABS` slabs + 256 MiB, in whole 256 MiB.
- `binsplit_shmem_pool_rule` under OFI: an unset `COMM_SHMEM_POOL_MB` becomes the SHMEM need (not the 8192 default), no staging
  regions (`COMM_SHMEM_STAGE_SLOT_MB` not exported), `COMM_OFI_POOL_MB` exported as the need when unset (a lower hand value warns); the
  `plan pool` line adds `the comm_ofi pools 4 x COMM_OFI_POOL_MB=…`.
- `as_shmem_pool(…, &ofi)` → `as_room_fits` and `binsplit_node_bytes` add the four comm pools to the host term; the `room:` line prints
  `ofi_pool`.
- `mem_model.py`: `ofi_planned()`, `shmem_pool(ofi=)`, `mem_per_node(ofi=None → as the code decides)`, `--check-c` compares
  `room ofi pools`.
- `mnrun.sh`: the plan runs on the login node, which on aac7 (uan1) has **no cxi** — so with `COMM_OFI` unset it probes the first compute
  node once (`srun --overlap test -e /sys/class/cxi/cxi0`) and exports `COMM_OFI_PLAN_CXI`.
- Docs: README rows (`COMM_OFI_POOL_MB`, new `COMM_OFI_PLAN_CXI`), internal code notes §4/§5, 02_PIPELINE_MEMORY.md §3.7,
  internal target notes launch line (`COMM_SHMEM_POOL_MB=1536`, heap 2048M).

## 2. Part A — tests (all on aac7)

| test | result |
|---|---|
| `MN_PLAN_ONLY=52760000000000:576` (target launch env + `DM_MN_LEAN=1`), login node | COMM_OFI=0: `COMM_SHMEM_POOL_MB=9472` (staging 8192 = 4 × 2048 by tree level of 2, control 894.2 MiB), heap ≥ 9984; **COMM_OFI=1: `COMM_SHMEM_POOL_MB=1536`, heap ≥ 2048 MiB, 4 × `COMM_OFI_POOL_MB=2304`** (measured C output) |
| `BS_LAYOUT_ONLY=9.1597e10:576,9.1597e10:2,5e10:2` + `mem_model.py --check-c … 31 1024 128 0.16 auto`, COMM_OFI 0 and 1 | **exact** both (0 terms not exact; 12 plane figures exact; `room ofi pools` C = model) |
| target node (5.276e13 / 576, `DM_MN_LEAN=1`, the `room:` line = `tests/dl15_ceilings.py`) | no OFI 446.16 GB; **OFI before OFIMEM 457.2 GB** (446.16 + 4 × 2624 MiB uncounted; with the launch line's hand-set 9472: the same); **OFI after 447.50 GB** (SHMEM 1536 MiB + 4 × 2304 MiB); keeping `COMM_SHMEM_POOL_MB=9472` by hand would give 455.8 GB — hence the TARGET.md edit (all modelled; ≤ 480 GB) |
| 10-node layout (8.1e11, LINE10) | SHMEM-only 419.59 GB; OFI before OFIMEM ≈ 430.0 GB (uncounted, over RUN16's 427 GB budget); **OFI after 420.66 GB** (SHMEM 512 MiB + 4 × 2304 MiB) (modelled) |
| 1 node, 2 node-processes, job 12294 (`mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_OFI_VERBOSE=1 POOL_LOG=27 ECALC_VERBOSE=2 ./ecalc {1e8,1e9}`, COMM_OFI unset) | rc 0, `all 2 nodes: VERIFY OK`, `digcmp.sh` **identical** to ref/e_{1e8,1e9}.txt (7.95 s / 13.25 s); mnrun's probe: `COMM_OFI_PLAN_CXI=1`; SHMEM pool 512 MiB, **peak 2 MiB (control 2, staging 0)**; comm pools 512 MiB each, **peak 106.0 MiB** at 1e9 (model's per-APU staging 211.9 MiB); every cxi0–3 carried 4.0–4.2 GB (measured; note 12294's node is CPX — four partitions of one MI300A, RESULTS §107 — so this checks the pools and digits, not memory scale) |

## 3. Part B — the standard 10-node run with all NICs (job 12287, measured)

Driver `tests/std17_drive.sh` (armed 11:20 EDT; the marker appeared 11:43; no other steps in 12287 then). MemAvailable at start:
min **441.0 GB** (x9000c1s3b0n0, s4b0n0, s4b1n0, s6b0n0; the other six 518.5–520.5) → the full size **8.1 × 10¹¹** (81e9 per node),
layout node **420.66 GB** (modelled; 20.3 GB under the minimum; `room:` shmem_pool 512 MiB, ofi_pool 4 × 2304 MiB). Launch line =
RUN16 try 2's LINE10 + `COMM_OFI_VERBOSE=1`; `COMM_OFI` unset (on: cxi). `plan check … OK — 96 products`.

- **2-node sanity** (2e10): rc 0, `total` 78.41 s, VERIFY OK; SHMEM pool peak 2 MiB (control only); comm pools 1536 MiB, peak 1059.6.
- **Record:** rc 0, **`total` 1669.98 s** (elapsed 1679 s), `mn: all 10 nodes: VERIFY OK`. **SHMEM try 2: 3532.05 s → −52.7 %.**

| phase | SHMEM try 2 (RUN16) | OFI (this run) |
|---|---|---|
| init | 128.8 s | 30.5 s |
| bs | 1225.3 s | 262.6 s |
| dm (recip) | 2174.2 s (267.6) | 478.3 s (205.9) |
| dc (the part-file write, NFS, O_DIRECT) | 0.1 s (hidden under the slow dm) | **897.6 s** (now exposed: 54 % of `total`) |
| total | 3532.05 s | 1669.98 s |

comm-mark (per APU thread, summed over 4 × 10 threads; same exchanges and bytes in both):

| level | exchanges, GB | SHMEM GB/s (wall) | OFI GB/s (wall) | ratio |
|---|---|---|---|---|
| tree level 1 | 1380, 599.31 | 1.05 (324.06 s) | **8.05** (114.12 s) | 7.7× |
| tree level 2 | 4904, 2779.33 | 0.75 (1023.72 s) | **5.63** (174.03 s) | 7.5× |
| reciprocal | 6788, 1209.80 | 1.38 (267.65 s) | **7.86** (205.87 s) | 5.7× |
| division | 8404, 4421.32 | 0.61 (1906.51 s) | **4.84** (272.41 s) | 7.9× |

- **All NICs:** the cxi hardware counters (`hni_sts_tx_ok_octets`, before/after the record) moved on **all 40 NICs (4 × 10 nodes),
  2471.7–2627.2 GB tx each**, 102.7 TB in all (includes the NFS write); `COMM_OFI_VERBOSE` per device and rank: 2.36–2.51 TB on its own
  cxi. SHMEM try 2 used cxi0 only.
- **Pools:** comm pool peak **2048.0 MiB of 2304** on every device of every rank (the model's per-APU staging exactly: the rounds'
  1024 + 1024 MiB; 256 MiB margin unused); SHMEM pool 512 MiB, peak 2 MiB.
- **RECHECK (packed):** rc 0, elapsed 2284 s, `mn: all 10 nodes: RECHECK OK`.
- **1e11 prefix** vs `~/ref/e_1e11.out` (the other agent moved it): **identical** (3387 s); parts deleted afterwards (`~/ofimem17/R` holds logs only, 676 KB). Driver verdict: `SUCCESS`.
- Note: with the fabric 5.7–7.9× faster, the run is now bound by the part-file write to NFS home (897.6 s, ≈ 0.4 GB/s aggregate for
  ≈ 340 GB packed); compute + exchange (bs + dm) fell 3399.5 → 740.9 s (−78 %).

## 4. Part C — B7, the general-map v-exchange slots (measured from `MEM_REPORT_DEVS`)

`MEM_REPORT_DEVS=1` prints per APU the layout's `in use` and the driver's `used`; the difference is device memory **outside the layout**
(hipMalloc'd: the comm pools, the v-slots, the runtime). Per APU, every rank and APU (measured):

| run | g | driver − in use per APU at [tree] / [released] / [bs] |
|---|---|---|
| this run (OFI) | 10 (general map) | **6.3–6.4 GB** |
| RUN16 try 2 (SHMEM, same size) | 10 | **3.7–3.8 GB** |
| 2-node sanity (OFI) | 2 (power of two: no general map) | 2.0–2.1 GB |

OFI − SHMEM at g = 10 = 2.6 GB ≈ the comm pool (2304 MiB = 2.42 GB) — counted now by OFIMEM. The g = 2 run (comm pool 1.61 GB) leaves
≈ 0.4 GB of runtime per APU; so at g = 10 **≈ 3.3 GB per APU, ≈ 13 GB per node, sits outside the layout and outside the model** —
B7 is **confirmed** (the attribution to `need_vslot`'s grow-only hipMalloc, comm_layered.c:467-474, is inferred: it is the general map's
only such site; the size matches B7's ≈ q·8 per APU estimate). It was already present in the SHMEM run (RUN16's "407 GB measured"
counted only `in use` + host).

Per-node peak vs the layout (rank 0 at [bs], the device peak): driver 4 × 100.9 = 403.6 GB + host RSS 19.9 = **423.5 GB measured
vs 420.66 GB layout (+2.9 GB)**; RUN16 try 2 likewise ≈ 4 × (94.65 + 3.8) + 28.8 ≈ 422.6 vs 419.59. The layout still lands within ≈ 3 GB
only because its host term (44.7 GB: seed buffers 17.2 GB, freed after init) absorbs the ≈ 13 GB; the guard and the 12 GB reserve held
(MemAvailable min 441 GB → ≈ 17.5 GB left at the peak). At the target B7's estimate is ≈ 15 GB per node per general-map level kept
alive (192 and 576 → up to 30): 447.5 → 462.5–477.5 GB (modelled + B7's estimate) — still ≤ 480 but thin. **Recommendation:** count
the v-slots in `rns_mul_dist_mn_scratch` / `mem_model.py` (06_EVALUATION §4.1 item 5), or free them at the end of each level.

## 5. Open items

- The comm pool's model is ≈ 2 × the measured peak (211.9 vs 106 MiB at 1e9 / 2 procs), as the SHMEM law was: conservative.
- `COMM_OFI_PLAN_CXI` depends on mnrun.sh's probe; a launch without mnrun.sh on a cxi-less login node plans the SHMEM-only pool
  (`COMM_SHMEM_POOL_MB` set from that plan counts as set by hand: the run keeps that SHMEM pool beside the comm pools, the pre-OFIMEM
  footprint — safe but not lean). The target launch line sets `COMM_OFI=1`, so its plan is right on any host.
