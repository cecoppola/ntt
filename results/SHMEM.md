# SHMEM — the SHMEM fallback transport against libfabric: closing record (2026-10-23)

Scope: what is established about `COMM_TRANSPORT=shmem` (`comm_shmem.c`, with `comm_ofi.c` as its data plane) against libfabric on aac7, kept after the
proxy-mode records were archived. The default multi-node path is xshmem (`results/XSHMEM.md`); `README.md` describes the two supported paths. The proxy
mode (one helper process per APU, `COMM_SHMEM_PE_PER_APU` with its `COMM_SHMEM_PX_*` switches) was removed in P7 (2026-10-22, Q78); its per-item records
(N7B-N7G, items 37-99, the 2026-10 stall investigation and the phase breakdowns) are in `archive:results_merged_2026-10-23/` (`SHMEM_full.md`,
`SHMEM_ITEMS_full.md`, `SHMEM_STALLS.md`). Labels: (m) measured, (mod) modelled, (a) assumed.

## Summary of the proxy-mode phase (aac7, 2026-10-14 to 2026-10-21; m)

- **6 nodes, 3.8646e11 digits.** libfabric 184.5 s (n 4, sd 6.4); the SHMEM proxy mode 286.0 s mean over 24 runs (sd 50.1, range 212-398), best four 220.5 s: a steady gap
  of about 36 s plus about 65 s of run-to-run variance, before the heap fix. The best proxy settings (copy kernel, parallel init, put-with-signal sends, NUMA heaps) reached
  1.43 times comm_ofi in one run and 1.68 times in a 28-pair repeat; no switch closed the gap, and the proxy mode was removed.
- **Stalls.** The random multi-second stalls were kernel proactive compaction of the node-process's pinned 4 KiB symmetric heap, which evicts the GPU queues; with the heap
  in 2 MiB pages (`COMM_SHMEM_NP_HEAP_THP=1`, D48) there were 0 stalls in 16 runs at 3 nodes and 0 in 6 at 2 nodes, against 9 in 24 (section below).
- **Gap after the fix.** 2 nodes: parity (43.3-45.1 against 43.0-45.2 s); 3 nodes about 6 % behind (49.6 against 46.6 s); 4 nodes at 6.4e10 +2.2 % (54.34 against
  53.18 s, n 12 and 7); the gap grows with the number of off-node peers (6 nodes about +9 % with chunked puts) and is waiting in the layered variable-count exchange (closing
  record, section 1 below). SHMEM about 3.0 times comm_ofi at 6 nodes with the earlier best settings (698.5 against 230.5 s) and 2.9 times for one proxy PE per APU (656.7 against 224.0 s).
- **Not adopted** (all gone with the proxy mode): peer order shift, `PE_PER_APU` 1 and 2, `PX_THP`/`PX_PIN`, `PX_NUMA` with `HSA_ENABLE_SDMA=0`, `PX_KCOPY`, `PX_INIT`, `PX_SEND` 1-4 (PX3 chunked puts were the last default), `PX_SCHED`;
  a 6 GiB `MEM_FRAG_RESERVE_GB`, `MNRUN_MEMPREP_GB`, `ECALC_INIT_OVERLAP` and the helper-thread count had no effect.
- **Scale-out model** to 576 nodes and the phase tables are in the archive; they describe the proxy mode and are not current.
- **Start-up** (m): 2 nodes warm init 23.65 s with the node-process layout; heap collapse about 3 s at 4, 8 and 16 threads; the one-process-per-GPU layout cuts init about threefold (`results/XSHMEM.md`).

## Compaction stalls and `COMM_SHMEM_NP_HEAP_THP` (3 nodes, 4.8e10, items 97a-97t, 2026-10-18; m unless marked)

- Mechanism (items 97k-97n, heartbeat counters `COMM_TRACE_HB_MEM`, per-mapping smaps capture): the kernel's proactive
  compaction (`compaction_proactiveness` 20) starts on a NUMA node whose free memory is > 90 % in blocks < 2 MiB. After
  start-up this is NUMA node 0 of the job's first node in nearly every run (score 91-99; other nodes 0-11). A pass isolates
  the node-process's Cray SHMEM symmetric heap (`/dev/zero` shared mapping at the library base, all 4 KiB: 3096 MiB under
  SHMEM, 1048 MiB under libfabric), fails to migrate the NIC-registered pages and retries; every unmap evicts the process's
  GPU queues (KFD `evicted_ms` +42 s summed in an 8 s storm), so all 4 APU threads stall. Failed-page counts per storm match
  the heap sizes (740k pages = 2.9 GiB SHMEM, 241k pages = 0.94 GiB libfabric).
- Fix: `COMM_SHMEM_NP_HEAP_THP=1` pre-faults and collapses that heap to 2 MiB pages before registration (compaction skips
  compound pages); 3096 MiB fully PMD-mapped, 1.9-2.0 s per process, overlapped (init unchanged). Gates 1 n, 2 n SHMEM and
  libfabric, off and on: PASS, digits identical (97q, 97s; main-based build in 97w).
- Runs, 3 n (97q/97r/97s/97t): with the switch 0 of 16 runs stalled (SHMEM DT 47.4-50.9 s, libfabric 45.9-48.0 s;
  2 n, 97v: 0 of 6); without it 9 of 24 stalled (SHMEM up to 70.3 s). Same nodes: SHMEM with the switch about 6 % behind libfabric when
  libfabric runs well (49.6 vs 46.6 s, 97q/97r). Default 1 since D48 (user 2026-10-18).
- Not adopted: `MEM_FRAG_RESERVE_GB` (branch fragres): a 6 GiB reserve on NUMA node 0 lowers the score after start-up but
  costs 4-8 s of init and, without the heap fix, makes the storms worse (97s/97t). `MNRUN_MEMPREP_GB=100` (97g/97h),
  `MEM_HSTAGE_THP=0` (97b3/97b4), `COMM_SHMEM_PX_INIT_THREADS=16` (97i/97j), `COMM_SHMEM_PX_CONC=1` (97c/97d): no effect.
  `COMM_SHMEM_PX_INIT_THREADS` removed 2026-10-19 (a constant 4): 98d (2 n, measured) heap collapse about 3 s at 4, 8 and 16 threads (node-process heap mean 2.7-3.4 s, helper heaps 2.9-3.4 s).
- Remaining (m): successful compaction passes on that node still evict the GPU queues for 1.4-12 s summed per run; root
  access (`compaction_proactiveness=0` per job, hugetlb heaps) would remove them (private list of settings kept outside the
  repository).


## COMM_LAYER_INTER2 default 0 on both transports (2026-10-21, user: "Adopt SHMEM one in flight over two in flight"; "libfabric + 1 flight should be the reference speed")

- **3 n, 1.9323e11 digits** (item 9m5 on aac7, build eb0c5f0e, 14 runs, all VERIFY OK; m): SHMEM default (two in flight) 149.9 s (n 5, sd 2.1);
  SHMEM + `COMM_LAYER_INTER2=0` 143.2 s (n 3, sd 5.5); libfabric 140.2 s (n 3); libfabric + `COMM_LAYER_INTER2=0` 137.2 s (n 3). The layv summed
  wait fell from 264-266 s (SHMEM) to 183 s (SHMEM + INTER2=0). The earlier 4 n test (older build, items 98a-98o, archived) showed no gain; no 6 n measurement yet.
- **Change**: `COMM_LAYER_INTER2` unset is now 0 on both transports (user: libfabric with one in flight is the reference speed for SHMEM parity; no
  transport logic); `=1` restores two in flight. The `config:` eff line shows `layer_inter2:N`. Digits unchanged (exchange order only).

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

### 5. PX_QTHREAD (branch pxq, c8bb7f5d)

Poster and completer thread per proxy, two plain contexts per peer node alternated; needs PX3 under `PE_PER_APU=1` and THREAD_MULTIPLE; fatal under `COMM_OFI=1` by design. Gates 1, 2 and 3 n PASS,
digits identical (including the `PX_SCHED=0` chunked path, no hang). The paired timing was never run, so there is no speed claim. Not merged.

### 6. The NIC miss picture

Per node per run (m): SHMEM about 4-5e9 translation misses (3 n: miss0 5.3-5.5e9, of which the initiator side 4.2-4.4e9, about 80 %, and the target side about 1.1e9, about 20 %; 59 % of translated
requests miss); libfabric with device-memory pools about 1e4 (miss ratio 1.6 %), the same requests (req_ixe 2.0-2.1e9, req_oxe 6.7-7.0e9). Libfabric with a host pool (item 9lc) misses 3.5-3.8e9 and
runs about 17 s slower. Why (d, driver facts): host staging memory is mapped with 4 KiB ATU pages (no free hugetlb pages; THP does not reach the NIC mapping), while device memory registered by dmabuf is mapped
with large entries (hit_d1_0 8.5-9.1e9). At 2 n the misses cost nothing visible (SHMEM within noise of libfabric); at 3 n the remaining gap was not closed by removing the initiator misses (section 4), so
the cost of the misses on the gap is not shown. The target carries device buffers on both sides by construction.

### 7. Unmerged branches kept for reference

| branch | content |
|---|---|
| devsrc2 | first DEVSRC: device-source puts with a synchronous host-to-device copy |
| devsrc3 | DEVSRC rebased on main with the async copy and DEVSRC=3 zero-copy, DEVHEAP2 and the heap/VMM shim, probes (`t_vmmprobe`, `t_basevar`), `fi_hmem_shim.so` |
| gev | `COMM_XSTATS_GEV` event-pair diagnostic |
| pxq | `COMM_SHMEM_PX_QTHREAD` (section 5) |
| a2a | `t_comm --a2a`: benchmark-only probe of the library all-to-all calls against ours (e738c931; its 3 n item did not run to a result) |
| r3plane | model-only 3*2^k grouped-plane tier (`MN_R3_PLANE=1`, off; f52a424): saves memory against the non-power-of-two cap but keeps only 56-60 % of its time gain (mod) |

### 8. Node health note (REBOOT1)

On aac7 every job end of ours ends in a node reboot (needs_reboot); 830 of 880 reboots since 2026-10-12 followed our job ends (45 % of our node-ends reboot against 13 % for others' jobs). Every one of the NIC
losses found on 2026-10-20 appeared 3-35 min after one of those reboots (5 of 140 reboots that day, about 3.6 % per reboot, mod); further nodes were lost on 2026-10-21 and a rescreen found no lost node back. The data cannot
separate "the reboot causes the loss" from "the traffic does" (no pre-reboot NIC count existed). Mitigation: fewer job ends, i.e. one long hold running many items, with a NIC count per node
before each item (session hold, 2026-10-21). Three healthy nodes remained at the end of the phase.
