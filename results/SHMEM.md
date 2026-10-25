# SHMEM — the plain OpenSHMEM transport: pool sizing, the closing record, the stall checklist

Scope: what is established about the plain OpenSHMEM transport (`COMM_TRANSPORT=shmem`, `comm_shmem.c`, with `comm_ofi.c` as its data plane), which was removed 2026-10-25 (Q103;
the transport is xshmem on both paths, `results/XSHMEM.md`; `README.md` describes the two paths): the symmetric pool and its sizing law (§P214, §V114), the Cray SHMEM phase on aac7
(closing record), and the checklist for node-wide stalls. The proxy-mode records and the stall investigation (items 37-99, 97a-97t) are in `archive:results_merged_2026-10-23/`
(`SHMEM_full.md`, `SHMEM_ITEMS_full.md`, `SHMEM_STALLS.md`); §S, §S12 and §SHM of the former SHMEM_ITEMS are in `archive:results_merged_2026-10-24/SHMEM_ITEMS.md`. Old IDs resolve
through `archive:results_merged_2026-10-24/INDEX.md`. Labels: (m) measured, (mod) modelled, (a) assumed.

## Summary (what the records establish)

- **Pool law.** Pool = 4 APU threads x (send + receive) of the largest staged exchange; exact (§P214); rounds cut the second copy (§V114: 1e10 on 2 nodes pool 8194 -> 2050 MiB, total not slower, m).
- **Stalls.** The random multi-second stalls of the node-process layout were kernel proactive compaction of its pinned 4 KiB symmetric heap, which evicts the GPU queues; a 2 MiB-page heap (`COMM_SHMEM_NP_HEAP_THP=1`, D48) gave 0 stalls in 16 runs at 3 nodes against 9 in 24 (m). The checklist for a recurrence follows.
- **Gap to libfabric.** 3 n 1.9323e11: SHMEM 157.0 against libfabric 143.0 s, all of it waiting in the layered variable-count exchange (closing record section 1); proxy mode (one helper PE per APU) never closed it and was removed (Q78); SHMEM 3.0 times comm_ofi at 6 nodes with the earlier best settings (m).
- **One exchange in flight** (`COMM_LAYER_INTER2` removed, 2026-10-21): 3 n 1.9323e11 SHMEM 149.9 -> 143.2 s, libfabric 140.2 -> 137.2 s (m, 14 runs, digits unchanged).

## §P214 — the SHMEM pool: measured, modelled, sized (PLAN §33 A2, TASKS 2.4, TARGET_TASKS T0)

Phase 14 agent P2, branch `p14-P2` from `main` @ a839418. SOS build on aac6, two real nodes over 1 GbE (one PE per node,
`COMM_SHMEM_DEVHEAP=1`) and four-process one-node runs; every digit check `cat parts | cmp - ref`. Final commit 7d377c4; regression
ran on 8b6955e. Open at return: the user's decision on `MN_T_CHUNK_MB=1024` for the target; staging in rounds would remove the
second copy (done in §V114).

**Measurements (m).** Pool peak = 4 APU threads x (send + receive) of the single largest staged exchange (8 blocks at every
peak); only staging fills it (control 2 MiB at 2 PEs, 6 at 4; mailbox 0.02 MiB). Every exchange of `ecalc` was staged.

| run | digits | pool peak MiB | largest staged exchange per APU thread (send + recv MiB) | wall s |
|---|---|---|---|---|
| 2 nodes SOS | 1e8 identical | 130 | 10.6 + 10.6 (result exchange) | 25.15 |
| 2 nodes SOS | 1e9 identical | 849 | 106.0 + 106.0 (`mn_core` result exchange) | 171.95 |
| 2 nodes SOS | 1e10 identical | 8479 | 2 x 1059.6 | 2070.7 |
| 4 proc one node | 1e9 identical | 429 | 53.0 + 53.0 | 30.63 |
| 4 proc one node | 1e10 identical | 4244 | 529.8 + 529.8 | 283.26 |
| 2 nodes, `DIST_LOGN_TEST=25` (2 x 2 pieces) | 1e9 identical | PE 0 531, PE 1 637 | PE 1: 53.0 + 106.0 | 197.01 |
| 2 nodes, `COMM_SHMEM_POOL_AUTO=1` | 1e10 identical | 8479 | 1059.6 + 1059.7 (96 exchanges) | 2035.97 |
| 2 nodes, `MN_T_CHUNK_MB=64` | 1e9 identical | PE 1 637 | `mdb_shift` 106.0 + 53.0; result exchange 64 + 64 | 170.77 |

**The law (mod; measured to 0.3 MiB at all eight points).** `pool = 4 x 8 x max over mn_core products and the division shift of
staged send + receive per APU thread + control (4 x (g + lower sizes) x (RING_KB + 64 B)) + mailbox (1024 x g x 8) + 256 MiB
margin` (`mem_model.shmem_pool`; in C `binsplit_shmem_pool_need` + `rns_mul_dist_mn_stage`). Per product: result exchange =
RT(nc_piece) + min(share_c, nc_piece)/4 (each <= m with `MN_T_CHUNK_MB=m`); redistribution = RT(pa) + min(share_a, pa)/4 + 2g
rows; transform = 2q/4; division `mdb_shift` = (2 n_Q/g/4 + n_Q/g/4)/K. Below the cap, `A_h mu` sets it: **pool ~ 32 n_Q/g
bytes ~ 1.78 bytes per digit per node** (x10 per decade; halved at 4 processes). Target 4.25e13 on 576 nodes
(`MN_GROUPS=2,4,8,16,32,64,192,576`, GB per node, mod):

| configuration | pool | node total vs 480 GB |
|---|---|---|
| defaults (TARGET.md §4) | 81.6 (`COMM_SHMEM_POOL_MB=77824`) | 524.7, exceeds |
| + `DM_TIGHT=1` | 81.6 | 501.2, exceeds |
| + `MN_T_CHUNK_MB=1024` | 45.0 | 459.6, fits |
| + `MN_T_CHUNK_MB=1024 DM_TIGHT=1` | 45.0 | 434.3, fits |
| + `DIST_MN_SYM_SLABS=1` | 133.1 | 576.2, exceeds |

`estimate.py` (default `--staging code`): defaults pool 82 / node 525 GB "no"; `--chunk both` pool 45, node 460, wall 244.9 s
(4.1 min) vs 234.0. Largest 576 run at 480 GB: 3.73e13 digits (defaults), 4.59e13 (`MN_T_CHUNK_MB=1024`) (mod).
**Answer to T0:** the staged buffers were counted in the device total already, and the pool holds a *second copy* of the largest
exchange's pair, so both the pool setting and the node total were wrong; 452 GB does not hold with defaults. The README row's
"`MN_T_CHUNK_MB` 1024 since Phase 13c" was wrong: `mn_t_chunk_limbs` defaults to 0 (only `MDB_SHIFT_CHUNK_MB` is 1024). Designs to
remove the second copy: (a) staging in rounds (done in §V114), (b) rbO/sb/rb from the symmetric pool (not done).

**TASKS 2.4 (`DIST_MN_SYM_SLABS=1`, off; m, 1e9, 2 nodes).** Digits identical; pool peak 849 -> **2385 MiB** (staging 847.7 +
symmetric 1536), the transform's 3456 staged exchanges vanish, block pool peak 2.5 -> 2.0 GB, total 172.48 vs 171.23 s. The
transform's staging never sets the peak (32 vs 106 MiB), so the slabs only add 3q*8 B per APU of resident memory (51.5 GB per node
at the 576 target). Kept off; closed as measured.

**Pool rule.** `binsplit_shmem_pool_rule(N, COMM_SIZE)` after `rns_init`: `COMM_SHMEM_POOL_AUTO=1` raises the pool to the need
(256 MiB steps; 256 -> 512 m, identical); without it a too-small pool gets one warning; a pool that runs out stops with an error
naming the use by kind, the size needed and the model's need (`COMM_SHMEM_POOL_MB=96`: rc 3 in 3 s). `MN_PLAN_ONLY` prints
`plan pool`. A host heap needs heap >= pool + 512 MiB.

**Gates (m).** SOS 2 real nodes identical at every size (1e8 x4, 1e9 x7, incl. sym slabs, chunk 64, `DIST_LOGN_TEST=25`); 4
processes 1e9/1e10 identical; TCP 2 nodes identical (1e8 23.02 s, 1e9 170.03 s); **1e10 on 2 real nodes with the final SOS
build and the rule: identical, 2035.97 s (bs 397.2, dm 1620.4, init 14.1)**, pool 8192 -> 8960, peak 8479; regression (8b6955e):
**17 passed, 0 failed**, 1907 s (4e10 identical 61.67 s, RECHECK OK); TCP 1e10 4 processes identical 81.31 s.

**Open then.** Target does not fit 480 GB with defaults (525 GB mod); `MN_T_CHUNK_MB=1024` fits (460) at +0.2 min mod (user's
decision). The law's multi-piece terms are validated at one small shape; the 576 figure rests on them. The Makefile's test rules
lack object dependencies (`make -j16` can link a test before `rns_dist.o` is rebuilt). The device model counts the division's
products with the top tree level's scratch (`A_h mu` rbO at 576 is 16.4 GB per APU, twice the tree top's 8.2).

## §V114 — the SHMEM pool by default, and the exchanges in rounds (Phase 14 agent V1)

Branch `p14-V1` from `p14-defaults` @ 4855842; background §P214. Final code 8f3b136; b6 (2-node re-check on 8f3b136) was never
submitted.

**Task 1: `COMM_SHMEM_POOL_AUTO=1` by default (done).** `mnrun.sh` (when `COMM_SHMEM_POOL_MB` is unset) finds the SHMEM-linked
executable followed by a digit count, runs it with `MN_PLAN_ONLY=<digits>:<procs>` (no device touched, 0.04 s at 4.25e13:576) and
takes the pool from the `plan pool` line; heap = pool + 512 MiB (`MNRUN_PLAN_POOL=0` skips; other commands keep 8192).
`binsplit_shmem_pool_rule` defaults to auto; with the pool in the library heap and a launch heap variable set, a heap below
pool + 512 MiB stops every rank before `shmem_init` with rc 8 (`EC_RC_BUDGET`) naming the variable and sizes (verified: heap 768M
vs need 1280M, rc 8 in 8.8 s). Measured on SOS 2 real nodes: 1e9 identical (host and device heap) and 1e10 identical with the
pool from the plan. 1e10 on 4 processes **failed** on 0b07ebc (pool 4608 enough in bytes but its one first-fit area fragmented
with four APU threads allocating out of step) and passed after the fix (8f3b136, `COMM_SHMEM_STAGE_SLOTS`, default 1): staging in
four regions at the pool's end, one per APU, each the per-APU need + 1 MiB (`COMM_SHMEM_STAGE_SLOT_MB`), fallback to the general
area. Memory placement only; sizes and law unchanged.

**Task 2 (C1): `COMM_SHMEM_ROUND_MB=m` (off): all-to-alls in rounds; bit-identical at every size tested; removes the second copy.**
Chosen over pool-resident caller buffers (b) because it is local to `comm_shmem.c` (TCP and SHMEM keep one layout; arena and
callers unchanged). Design: a pair (s->r) of b bytes goes in K = ceil(b/c) rounds, c = m/(n-1) (256-aligned, >= 4 KiB); both
ends derive K from b; control-word sequences are **per pair** (`oseq[r]`, `iseq[r]`), so rounded and unrounded members
interoperate; rounds run in a helper thread on the communicator's HIP stream (call returns at once, `wait` joins). Staging per
round <= m each way: pool staging <= 4 x 2m. Under the switch every all-to-all takes the rounds' path (0b07ebc), which also
stops staging the self slab (half the staging at 2 PEs). Not covered: `alltoallv_host`, all-gathers (small).

**Measured (m, SOS, no pool/heap set by hand).**

| run | pool MiB plan -> peak | host HWM GB | node MemAvailable min GB | total s |
|---|---|---|---|---|
| 1e9, 2 nodes, default | 1280 -> 849 | 19.8 | 396.3 | 177.35 |
| 1e9, 2 nodes, `ROUND_MB=64` | 768 -> 425 | 19.3 | 396.8 | 177.39 |
| 1e10, 2 nodes, default | 8704 -> 8194 | 27.8 | 351.2 | 2089.83 (bs 403.1, dm 1628.5) |
| 1e10, 2 nodes, `ROUND_MB=256` | 2560 -> **2050** | 21.3 | 357.8 | **2072.91** (396.9 / 1620.3) |
| 1e10, 4 proc, default (8f3b136) | 4608 -> 4244 | 23.3 | 205.1 | 381.16 |
| 1e10, 4 proc, `ROUND_MB=256` | 2560 -> **2054** | 21.2 | 213.5 | 357.90 |

All identical, VERIFY OK; device total unchanged (186.7 GB at 2 nodes). Summed all-to-all time 7990.3 -> 7677.8 s (-4 %) at 2
nodes, 1026.5 -> 936.8 s (-9 %) at 4 processes: rounds are not slower (one run each, within noise). `t_comm` with `ROUND_MB`
1 / 0.001 (4 KiB chunks) / quiet / serial VERIFY OK. Law matches every point to 0.01 %.
Target (mod, `mem_model.py --pool`, GB per node): `COMM_SHMEM_ROUND_MB=1024` alone pool 9.8, node 452.9 (P214's figure); with
`DM_TIGHT` 429.4; with `MN_T_CHUNK_MB=1024` 424.4; with both 399.1 (the Phase 14 defaults plus the switch; defaults without the
switch 434.3). Plan `MN_PLAN_ONLY` 4.25e13:576: 43008 -> 9472 MiB. **Recommended for the target: 1024; adoption is the user's.**

**Open.** (1) 2-node re-check on 8f3b136 (b1-b3 used 6e92c74 / 0b07ebc without regions; b5 checked regions at 4 processes).
(2) `t_bs` segfault (rc 139, "P, Q vs the GMP-free reference recursion") in `mnaccept --only unit,e9,mn` on b4: same on the
integrator's `ntt-def14` and V2's branch, so it predates V1 (Phase 14 defaults). (3) The rounds' law is an upper bound (side =
(g-1)/g of the whole, bounded by m), exact at measured points. (4) The 1e11 reference did not exist; 1e9/1e10 references used.

## If compaction stalls come back, and what the target needs (condensed from the archived stall report)

- **Node-wide?** All four APU threads of one node stop at the same instant while the heartbeat keeps ticking; if only some threads stop, look at the intra rendezvous, GPU clocks or one NIC.
- **Counters per second on the late node** (/proc/vmstat): `compact_stall` and `allocstall_*` up = direct reclaim or compaction in an allocation; `compact_stall` 0 with `compact_isolated`
  and `pgmigrate_fail` rising and millions of `compact_migrate_scanned` = the proactive pass of this report; kcompactd ticks and the fragmentation score (above 90 on a NUMA node starts a pass);
  KFD `evicted_ms` per GPU rising by seconds across the storm. `COMM_TRACE` (v2) with `COMM_TRACE_HB_MEM` records these (`tools/trace_report.py`).
- **Candidates:** a large 4 KiB shared or NIC-registered mapping (another pool, a registered host buffer without `MADV_HUGEPAGE`, a larger `SHMEM_SYMMETRIC_SIZE`, a library that remaps its heap); check the
  `COMM_SHMEM_NP_HEAP_THP=1: heap ... PMD-mapped after` log line first. Repeat on a second node set with an A/A arm; do not infer from single runs (6-node runs differ by 10 % or more).
- **Target (a, until measured there):** a hugetlb-backed symmetric heap is skipped by compaction and gives the NIC 2 MiB translations (the cxi driver sizes NIC pages from hugetlb VMAs only); on the target measure the stall rate per
  node-hour first, keep AMD's `compaction_proactiveness=20` and `compact_unevictable_allowed=1` unless an A/B shows otherwise, set the NIC count to the node's NICs, keep all-to-all on the best-effort class with adaptive
  routing, and keep ODP and ATS off. `COMM_SHMEM_HUGETLB=1` (legacy `comm_shmem.c` heap from the hugetlb pool, off by default) is not the target path: the target runs `xshmem_cray.h`, where the libsma heap on hugetlb is
  the test (`TASKS.md`, target session).

## Closing record of the aac7 Cray SHMEM phase (2026-10-21)

User order 2026-10-21 (about 01:30 EDT): the tuning of Cray SHMEM on aac7 ends here. The project turns to target-style communication (Cray SHMEM 12.1, eight
Slingshot-400 NICs per node, GPU-memory buffers) emulated with libfabric. This section is the record of what the last night established and closed. Labels:
m measured, mod modelled, d derived. Nothing here changes code or defaults.

### 1. The 3 n gap, located (items 9lr, analysis GAP3N2)

9lr: 3 n, 1.9323e11 digits, 6 ABBA pairs, all VERIFY OK, `COMM_XSTATS=1 MN_WAIT_STATS=1` (m). Libfabric 143.0 s (sd 5.3), SHMEM 157.0 s (sd 11.2; 152.5 s without the cold
first run); paired difference -14.0 s, 95 % CI [-26.0, -2.0]. Per phase, SHMEM against libfabric (m): bs 54.1 vs 48.6, dm 63.0 vs 55.5, recip 18.5 vs 16.4, init 35.6 vs 34.8 s.

| quantity per thread (d) | SHMEM (5 runs) | libfabric (6 runs) | difference |
|---|---|---|---|
| wall, bs+dm+recip | 113.66 s | 104.07 s | +9.6 s |
| blocked in `comm_wait` | 52.75 s | 40.01 s | +12.7 s |
| non-wait (wall minus wait) | 60.91 s | 64.06 s | -3.1 s |

- The gap is waiting, not compute. Against the fast libfabric launches the wall gap is +12.1 s = wait +11.6 s, non-wait +0.5 s. The "compute stretch" explanation is not supported.
- One exchange kind carries it: `layv` (layered inter stage, variable counts; 2 remote peers at 3 n) is 1.42 x libfabric per exchange in bs, dm and recip alike
  (bs 55.0 vs 38.7 ms; 8.7 vs 12.4 GB/s per thread), +34-36 microseconds per MB in all three phases; it carries 61-66 % of the wait gap. The direct kinds (redist,
  result, spill, addsh) have equal bytes and equal time (+0.5 s per thread in total). At 2 n the same path is within 5-7 % of libfabric.
- ATU counters are constant across the SHMEM runs (CV < 1 %), so they cannot explain run-to-run spread; they differ categorically between the arms (section 6).
- Libfabric has a launch lottery of about 11 s, in non-wait time, not in wait time.

### 2. One exchange in flight (item 9m5) and its adoption

Two layered exchanges in flight per thread were worse than serial on SHMEM: with `COMM_LAYER_INTER2=0` the 3 n total fell 149.9 -> 143.2 s and the layv summed wait 264-266 -> 183 s;
libfabric 140.2 -> 137.2 s (m, the INTER2 section above). Adopted as the default on both transports; see the preceding section for numbers, gates and the change. Reading: the proxy serialises
(one proxy thread per APU, per-context quiet, puts to the two peers strictly ordered), so two exchanges interfere (LAYV1, code reading, no run proves it). What is left after the adoption: SHMEM
one in flight 143.2 s against libfabric one in flight 137.2 s (3 n, n 3 each, +6 s, m); no 6 n measurement.

### 3. Environment screens at 2 n (items 9l3, 9lx, 9ly, 9m4; 1.2882e11 digits, build eb0c5f0e; m)

21 settings against interleaved SHMEM controls (controls 109.5-113.0 s, dm 36.7-37.1 s; one cold control 129.6 s). None was faster; the ATU miss counters did not move
(+-1 %) in any arm except K09. Names as in the screen registry:

| result | settings |
|---|---|
| within noise (108.3-113.5 s, dm unchanged) | CQ size 512k, CQ fill 90 %, TX size 4096, no inject-DMA (IDC), LLRING always, EQ ack batch 256, CQ low-empty policy, MR cache count 0, HMEM P2P off, NBI block 16M, NBI pipeline 16M, target ordering relaxed, target ordering strict, WAW off, HRP, counter delay, counter spin, delivery-complete, max contexts 16, hybrid MR descriptors |
| slower | K09 `FI_CXI_IOTLB=0`: 137.0 s (+25 s; dm 57.3 vs 37.0, recip 20.0 vs 13.2, bs 42.6 vs 36); miss0 6.0-6.2e9 vs 4.1-4.3e9 (+47 %), miss_ixe 1.37-1.45e9 vs 0.81-0.84e9 |
| abort | R05 and R06 (TX size / NBI block combinations): `fi_cntr_wait` returns -22; R11 (put-signal with triggered command queue): abort at init, rc 134 |

Reading: the screen cannot make the 4.1e9 default misses cheaper, but K09 shows S time is sensitive to the translation path when a miss gets dearer. The library's own reading of the hidden `_SHMEM_*`
variables was not confirmed (no env display or ordering lines printed). Not run: K13-K16 (hidden libsma guesses), skipped for hang risk or absence: no-AP mode, kdreg2 (not present), OMR0.

### 4. Closed approaches

| approach | outcome (m) |
|---|---|
| DEVHEAP2 (`COMM_SHMEM_DEVHEAP2`: proxy symmetric heaps in fine-grained device memory, node process attaches by HIP IPC) | gate: node-process CPU read of the IPC-opened device heap faults (SEGV in both node processes, 5/5 timing runs died at 9 s). Without a CPU path the node process cannot reach the mailbox in that heap. Parked and closed (Q64). Enabling probes (9lk, 9lq): libsma accepts a different heap base per PE, and a device-backed heap with the FI_HMEM shim passed at 2 and 4 PEs per node; hip VMM (pinned) has no CPU mapping, CMA fails on all driver mappings |
| DEVSRC (`COMM_SHMEM_PX_DEVSRC`: proxy puts from a device ring, async stream copy) | 7/7 gates PASS, digits identical. Initiator misses fall from 3.2-3.5e9 to 1.4e6 per node and run, but time does not: 2 n pooled S 113.0 s (n 4) vs DS async 117.4 s (n 5), +4.4 s (extra device-to-device copy contends with compute, bs +4-6 s); 3 n (9lg, synchronous copy) S 165.2, libfabric 162.3, device source 175.0 s. Removing initiator misses does not speed dm |
| DEVSRC=3 zero-copy (proxy puts from compute buffers opened by HIP IPC) | first remote put from an IPC-opened device pointer crashes in the ROCr runtime (NULL deref at registration); with containment the proxies hang (9lo). Closed on this stack |
| NUMA flush (pinned allocate-and-free before a run) | each flush added 3.4k-37k node-0 misses, a run adds at most about 70; no way to reach zero. Off for good; rule: a node is fine if node 0 has at least about 96 GB free at start (REBOOT1, 9ks) |
| GEV (`COMM_XSTATS_GEV`, event pairs post to wait-start; diagnostic) | gates PASS with xstats_gev lines; the stream keeps up (gpu about host at gate scale). The 3 n GEV run was not needed: section 1 already rules out compute stretch |

### 6. The NIC miss picture

Per node per run (m): SHMEM about 4-5e9 translation misses (3 n: miss0 5.3-5.5e9, of which the initiator side 4.2-4.4e9, about 80 %, and the target side about 1.1e9, about 20 %; 59 % of translated
requests miss); libfabric with device-memory pools about 1e4 (miss ratio 1.6 %), the same requests (req_ixe 2.0-2.1e9, req_oxe 6.7-7.0e9). Libfabric with a host pool (item 9lc) misses 3.5-3.8e9 and
runs about 17 s slower. Why (d, driver facts): host staging memory is mapped with 4 KiB ATU pages (no free hugetlb pages; THP does not reach the NIC mapping), while device memory registered by dmabuf is mapped
with large entries (hit_d1_0 8.5-9.1e9). At 2 n the misses cost nothing visible (SHMEM within noise of libfabric); at 3 n the remaining gap was not closed by removing the initiator misses (section 4), so
the cost of the misses on the gap is not shown. The target carries device buffers on both sides by construction.

Sections 5 (`PX_QTHREAD`), 7 (unmerged branches) and 8 (node health note) moved to `archive:results_merged_2026-10-24/SHMEM.md`.
