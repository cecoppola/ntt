# SHMEM on aac7: from "slow and erratic" to "stable" (2026-10-14 .. 2026-10-18)

Purpose: a self-contained account of what was learned while making the SHMEM transport of ecalc stable and fast on the
aac7 test cluster, so that an engineer or agent who returns to this problem does not have to re-derive anything.
Companion files in the repository: results/SHMEM.md (the measured record per item), results/SHMEM_ITEMS.md §N7G and the other
results/N7*.md files (the early proxy-mode steps), ecalc/README.md (every switch), TESTING.md (the gates).

Labels: (m) measured, (mod) modelled, (a) assumed. Times are Eastern. "Item NNx" is a queue item number; raw data stay on
the cluster. Where a number comes from a single run or a small set, the text says so.

## 1. Summary

- **Problem.** On aac7 the SHMEM transport (Cray OpenSHMEMX, "proxy mode") was 1.4 to 1.6 times slower than libfabric at 6
  nodes and wildly erratic: 24 default runs averaged 286 s against 184.5 s for libfabric, range 212 to 398 s (m, results/SHMEM.md).
  Part of the loss was a steady gap (about 36 s at 6 nodes, best runs, m); most of it was random "stalls" in which one node
  froze for seconds and every other node waited for it.
- **Cause of the stalls (found 2026-10-18, items 97k-97n, m).** The Linux kernel's background compaction thread
  (kcompactd, proactive mode) periodically defragments the first node of a job. On that node it picks up the
  node-process's own SHMEM symmetric heap, which is made of ordinary 4 KiB pages that the network card has pinned. It
  cannot move them, retries, and every retry unmaps and remaps the pages. Each unmap makes the GPU driver evict the
  process's GPU queues, so all four GPU-driving threads of that node stop for seconds. Libfabric has a smaller heap of the
  same kind (1048 MiB against 3096 MiB) and stalls less often.
- **Fix (merged, default on since 2026-10-18, main 23c2c1f).** `COMM_SHMEM_NP_HEAP_THP=1` pre-faults that heap and collapses it into 2 MiB pages
  before it is registered with the NIC. The kernel skips 2 MiB (compound) pages, so the retry loop never starts. Cost: about 2 s,
  overlapped with start-up.
- **Result (3 nodes, 4.8e10 digits, m).** With the fix no run stalled (SHMEM 47.4-50.9 s; libfabric 45.9-48.0 s); without
  it 9 of 24 runs stalled (up to 70.3 s). At 2 nodes SHMEM with the fix equals libfabric (43.3-45.1 against 43.0-45.2 s).
  Gates (digits identical, 1 and 2 nodes, SHMEM and libfabric, default against switch off) passed (items 97q, 97w).
- **What remains.** (1) The fix was validated at 2 and 3 nodes only; the 5- and 6-node stalls (first seen in traces
  without kernel counters) are very probably the same mechanism but this is not re-measured. (2) A steady gap of a few
  percent remains (3 nodes: +3.5 s, 7.5 %; 6 nodes before the fix: +6.7 % on the best runs, m); its cause is not found. (3) Successful compaction passes still
  evict GPU queues for 1.4-12 s summed per run; no visible time cost at 3 nodes yet, but it is the next thing to
  remove (root settings or hugetlb on the target). (4) Several hypotheses that took days were wrong; section 7 lists them
  so nobody repeats them.

## 2. System and transport background

### 2.1 Hardware and software (aac7, the test cluster)

- Each node has 4 APUs (AMD MI300A: CPU cores and GPU sharing one memory pool; one per NUMA node, so a node has 4 NUMA
  nodes), 4 Slingshot network cards (NICs, one "cxi" device per APU) and about 440 GB of free host memory at job start.
  Up to 6 nodes per user at a time. RHEL 9-family kernel (the kernel source read for the mechanism was v5.14; RHEL carries newer memory-management backports, e.g. MADV_COLLAPSE exists, so line-level details may differ).
  Transparent huge pages (THP) are "always" with defrag "madvise"; there is no hugetlb pool (HugePages_Total 0; overcommit 0).
  Memory fabric: the GPU allocations (hipMalloc) live in system memory on these APUs.
- ecalc runs one **node-process** per node with one thread per APU (4 "APU threads") that drive the GPUs. A multi-digit
  number is split over nodes; the main communication is a layered all-to-all (see below).
- Cray OpenSHMEMX 11.8 on aac7 maps its symmetric heap as one shared anonymous mapping of the character device /dev/zero
  at a fixed library base address (visible in the process maps as "/dev/zero (deleted)", flags rw-s). There is no XPMEM
  (cross-process memory attach), so two processes of one node cannot see each other's heap directly.

### 2.2 The transports compared

- **libfabric arm (L).** `COMM_OFI` unset: the data moves with libfabric `fi_write` straight from a device memory pool
  registered with the NIC. SHMEM is still initialised, but only for bootstrap and small host operations; its heap is the
  pool size plus 512 MiB (1048 MiB at the sizes used on the 3-node tests).
- **SHMEM arm (D, "default proxy mode").** `COMM_OFI=0`. Because the SHMEM heap is host memory and the GPU data lives
  elsewhere, there is a **proxy helper**: one extra SHMEM process (PE, "processing element") per APU, so a node runs 1 node-process + 4 helpers.
  Each helper's symmetric heap is a file in /dev/shm that the node-process also maps (and registers with HIP so the GPU can
  read and write it). Kernels copy data between the GPU pools and this "proxy pool"; the helper posts the network puts through its own NIC.
  Source: ecalc/comm_shmem.c (header comments "N7b: the proxies' heaps as named shared memory"), ecalc/README.md row
  `COMM_SHMEM_PE_PER_APU`.
- Helper heaps are mapped in 2 MiB pages since D36 (`COMM_SHMEM_PX_THP=1`, default): pre-faulted and collapsed with
  MADV_COLLAPSE before registration. The node-process's own SHMEM heap stayed in 4 KiB pages until the 2026-10-18 fix.
- **Layered exchange.** The all-to-all is done per node group in two stages: an intra-node stage over xGMI (the
  in-package GPU interconnect) and an inter-node stage over the NICs. `layeq` is the form for equal-sized blocks (power-of-two
  node counts, used at 2 and 4 nodes); `layv` ("variable") is the form for unequal blocks (3, 5, 6, 192, 576 nodes). `layv`
  uses "v-slots" (staging buffers for the variable exchange). `COMM_LAYER_INTER2` (default 1) keeps two inter-node exchanges in flight.
  Source: ecalc/comm_layered.c, v-slot code around lines 500-660.
- **Phases of a run** (names in the logs): `init` (number-system setup and transport open), `bs` (binary splitting, the bulk of
  the compute, includes `recip`, the reciprocal), `dm` (the final distributed multiplication), `dc` (digit conversion).
  "roffmax" is the largest wait for a peer's receive-offset word: the maximum over processes of the time spent waiting for others.

### 2.3 Names of the test arms used below

| Arm | Meaning |
|---|---|
| L | libfabric, `COMM_OFI` unset |
| LH | libfabric with its pool in host memory (`COMM_OFI_POOL=host`), a probe arm |
| D | SHMEM default proxy mode (`COMM_OFI=0` plus the built-in defaults) |
| T / T64 / T6 | D plus `COMM_SHMEM_PX_SEND=3` (PX3), `COMM_SHMEM_PEER_ORDER=shift`, `COMM_SHMEM_PX_BLOCK_MB=64` (plus memory preparation M in some items) |
| DT | the same T settings used as the SHMEM arm in the 2026-10-18 scaling and 3-node campaigns |
| DTI | DT plus `COMM_LAYER_INTER2=0` (one inter-node exchange in flight) |
| DTP | DT plus `COMM_SHMEM_PX_PIN=1` (helpers pinned to CPUs) |
| DTIR | DTI plus `COMM_LAYER_VSLOT_RELEASE=0` (v-slots are not freed and reallocated; switch removed 2026-10-19, task 4) |
| DTIM | DTI plus `FI_MR_CACHE_MONITOR=memhooks` |
| DTH / DTIH | DT / DTI plus `MEM_HSTAGE_THP=0` (host staging without huge pages; switch removed 2026-10-19, task 4) |
| suffix X | plus `COMM_SHMEM_NP_HEAP_THP=1` (the fix; default since 2026-10-18) |
| suffix Q | plus `MNRUN_MEMPREP_GB=100` (memory preparation step before the run; removed 2026-10-19, task 4) |
| suffix R | plus forced `MEM_FRAG_RESERVE_GB` large-block reserve on NUMA node 0 |
| F, FX, FL, ... | the same arm run right after a "fragmenter" (a tmpfs program that fragments host memory) |

PX3 = proxy send mode 3: plain non-blocking puts of 64 MiB chunks on one SHMEM context per peer node, then one fence and
one signal word per command (ecalc/README.md row `COMM_SHMEM_PX_SEND`). "memprep" = the memory preparation step (section 7).

### 2.4 Test-bench conventions (important for reading the evidence)

- Items are queued runs on aac7 with a fixed arm order; ABBA or mirrored orders and A/A controls (the same arm twice) are used
  because **the node set dominates**: the same arm can be clean on one set of nodes and stall on another.
- Sizes: 6 nodes 3.8646e11 digits (about 185-290 s) is the reference; 6 nodes 9.6e10 (about 60 s) reproduces the gap;
  4 nodes 6.4e10 and 2.5764e11; 3 nodes 4.8e10 (about 46-50 s, the smallest `layv` shape, used for the final diagnosis, run
  as packed 3+3 pairs); 2 nodes 1.2882e11, 3.2e10 or 1.6e10 digits ("fast" items under 15 minutes).
- Every run is digit-verified; a memory-fit check (the budget check against MemAvailable) can stop a run before compute.

## 3. Timeline of the investigation

Phases, what was believed, and what each test showed. Dates 2026-10.

| When | Phase | Hypothesis or action | Evidence and result (m unless noted) |
|---|---|---|---|
| 10-14 | N7D/N7E: proxy mode | One SHMEM PE per APU; copy kernel; pre-fault; put-with-signal chunks | 6 nodes: SHMEM B 274.8 s against libfabric 191.6 (+83.2 s, 3 pairs); N7E 28 pairs +129.3 s [+92.9, +165.7]. Defaults of COMM_OFI=0 become PE_PER_APU=1, PX_NUMA=1, PX_KCOPY=1 (D31). |
| 10-15 | N7F/N7G | Fixes A (offsets with data), B (chunk), C (parallel mesh); huge pages for helper pools | Fixes A/C no gain. `COMM_SHMEM_PX_THP=1`: item 37, -287.8 s [-507, -68], faster 4 of 4 (D36 adopted). PX_PIN no separation. |
| 10-16/17 | Variance study (items 43-54) | Slow node? slow step? | 24 runs mean 286 s, best 4 220.5 s, sd 50: no fixed straggler; slow runs lose time in node-local steps (init, self-test, first leaf) on a node that changes every run. |
| 10-17 | Scaling checks (items 57, 66, 67, 71) | Does the gap depend on node count? | 1 n and 2 n: no gap, no stalls (SHMEM 120.5 against 123.3 s at 2 n). 4 n: gap about +30 s; 6 n: +36..40 s. Stalls also hit libfabric once at 4 n. |
| 10-17 | Phase B: host memory fragmentation | A tmpfs fragmenter before the job (item 76) | Reproduced: 2 n run 115-125 s becomes 285-336 s; start-up steps 3-5 times slower (arena alloc 23-27 against 7 s). The node does not recover afterwards. Conclusion then: fragmentation causes node-local stalls. (True in a narrower sense, see section 4.) |
| 10-17/18 | Fixes for fragmentation | Precompaction in-process (72g); memory preparation as a separate step (72h/72i/72n); per-allocation compaction (73d); allocation bench (73c) | Prep removes about a third of the loss, then the cost moves to the next memory-touching step. MEM_DEV_COMPACT: 1220 s on an exhausted node, dropped. Only threading helps first touch (4 pinned threads, MADV_POPULATE_WRITE, 9.7 GB/s). |
| 10-17/18 | Phase C: the steady gap | t_comm probes (a bandwidth benchmark of the transport alone), PX_WORKERS, SS (PX_SEND=2), PX3 | 2 n proxy equals libfabric; at 4 n with all peers D/L 0.71-0.93. PX_WORKERS raised the proxy rate, not node throughput (not adopted). PX3 closed the probe gap (T64 17.46 ms against libfabric 18.38 and D 19.85 per 64 MiB exchange) but not the application gap with statistics. |
| 10-18 am | Plan D41 (8 h) | Combined candidate | 6 n confirmation never reached: stalls dominated every comparison; the D45 rule (8 pairs at 4 n, CI excluding 0) was unreachable (6 of 8 runs stalled even with memprep). |
| 10-18 pm | Fast items | Many small 2-4 n items for gap and stall questions | 2 n shows no gap; at 4 n D is +3 to +11 % over L; DT (PX3 set) at parity at 4 n 6.4e10 (items 95e, 95j). ktime/zero-copy/stage/gpusig/arena-first/ownalloc/bind probes: no gain (section 7). |
| 10-18 pm | 6 n confirmation (95o) | Best SHMEM settings against libfabric at 3.8646e11 | Libfabric 207.5 s (4 runs), SHMEM best settings 288.8 s mean (+39 %), best run 221.4 s (+6.7 %). 5 % not reached; the loss is in the run phases (exchange waits). |
| 10-18 eve | Per-message trace (COMM_TRACE) | Where do the extra seconds sit? | Large `layv` messages move at 1.6 GB/s per message against libfabric 3.9-4.4; plus node-wide stalls of 0.7-4.2 s on one or two nodes. Gap is a step at the layeq to layv switch (section 4.5). |
| 10-18 eve | INTER2 (item 95z1) | `COMM_LAYER_INTER2=0` | layv exchange rate 14.4 GB/s per APU against 3.95 (DT) and 9.7 (L): large improvement of the metric, totals still +1/+17/+12 % per round because stalls also hit. |
| 10-18 eve | 3-node campaign (97a-97w) | Small packed pairs with trace, heartbeat, kernel counters | Storm anatomy (kcompactd proactive passes), then the failing pages identified through smaps (97m), then the fix (97q-97w). Details in section 4. |

## 4. Root cause in detail

### 4.1 What a stall looks like

- Node-wide: the four APU threads of one node-process stop together (same instant, within 0.1 ms), for 0.5 to 4.2 s at 6 nodes
  (m, traces of items 95q/95t) and for about 5-8 s at 3 nodes (m, 97r/97m). The other nodes then wait at the next exchange
  ("skew"). A host thread that touches no GPU and no migrating pages (the heartbeat thread of the trace tool) keeps
  running, so the process is not descheduled and not swapped.
- During one 6-node event the NIC still received puts into the frozen node (four 30 MB puts landed with normal 20 ms wire time),
  so the NIC and the memory behind it kept working while the node-process was silent (m, one event).
- The stall length is 50-100 % of the length of the kernel activity described below; it lands in compute and host phases between exchanges
  (before the first `bs` exchange at 19-27 s into the run, in `dc`, at the `addsh` to `redist` hand-over) and not inside a transfer.
- Start-up skew is a separate, smaller thing: every run, libfabric included, has 0.5-2.4 s of skew in the first layeq exchange (10 s once).

### 4.2 Kernel mechanism (m, 3-node campaign; the kernel counters are in /proc/vmstat)

1. **Proactive compaction.** With `vm.compaction_proactiveness` = 20 (the default; AMD's MI300A guide keeps it) the kernel thread
   kcompactd wakes every 500 ms and scores each NUMA node: the share of free memory that sits in blocks smaller than 2 MiB. Above
   90 it starts a "pass" over the whole node; a pass stops at 80 or earlier if it cannot make progress. After a pass with no
   progress the kernel defers the next one by about 64 wake-ups (about 32 s).
   Signature: `compact_stall` = 0 and `compact_daemon_wake` = 0 (the direct path, which would raise them, is not used);
   `compact_migrate_scanned` jumps by about 19 million pages (about 74 GiB) in the last second of the pass.
2. **Which node.** After start-up, NUMA node 0 of the job's first node has a fragmentation score of 91-99 in nearly every run
   (other nodes 0-11). Start-up consumes all of that node's roughly 50 GiB of free 2 MiB blocks (the reserve code printed: small-block
   free 17.3 GiB, large-block free 0.6 GiB, score 96.7). Free memory also drifts down about 1.3 GiB per run in a series, so
   state accumulates across runs on the same nodes. Why start-up leaves node 0 exhausted is not fully traced (open, section 10).
3. **What a pass tries to move.** Compaction skips compound pages (THP, hugetlb) and skips pinned *private anonymous* pages
   early. It does isolate pinned *shared* or file-backed pages (shmem, page cache), even in SYNC_LIGHT mode. The node-process's
   Cray SHMEM symmetric heap is exactly that: a shared anonymous /dev/zero mapping at its fixed base, all 4 KiB pages
   (smaps capture, item 97m: 3096 MiB with SHMEM, 1048 MiB with libfabric). The NIC registration holds an extra reference to each page.
4. **Migration fails and is retried.** For each isolated page the migration code replaces the page-table entries with
   migration entries, finds the extra reference, and fails (EAGAIN or EBUSY), then restores the entries. Failed pages go back
   to the free list and the same ones are reused. Failing passes show `compact_isolated` about equal to `pgmigrate_fail`
   with `pgmigrate_success` near zero (healthy passes have isolated = 2 x success and zero failures).
   Failed-page counts per storm match the heap sizes: 740k pages = 2.9 GiB under SHMEM, 241k = 0.94 GiB under libfabric (m).
5. **Why every GPU thread stops.** Each unmap and remap fires the MMU notifier for the process; the AMD GPU driver (KFD)
   answers by evicting the process's GPU queues and restoring them afterwards. Measured: KFD `evicted_ms` rose by 42 s (summed
   over the process's GPUs) during one 8-second storm (97m, r1 DT), and by 67.6 s in the worst storm (97r, DT r1, 802k failed
   migrations). Four APU threads are all GPU-bound, so all four stop; the heartbeat thread is not.
   This chain (MMU notifier to queue eviction) is established by the matching timing and counters, not by reading the
   driver on the cluster (the kernel source consulted was a nearby version; RHEL differs, doubt low).
6. **Why the helper heaps do not fail.** They are shmem files already collapsed to 2 MiB pages (D36), so compaction skips them
   (m: 97m/97n smaps, every helper heap THP, no failures from them).

### 4.3 Why SHMEM stalls more than libfabric

- The failing set is the node-process's SHMEM heap: 3096 MiB with SHMEM (pool 2584 MiB + 512 MiB, set by the launcher from the memory plan) and 1048 MiB with
  libfabric (libfabric still initialises SHMEM for bootstrap; heap = pool + 512 MiB). More failing pages, more retries,
  more evictions; libfabric stalled 1 run of 4 in 97b and 0 of 7 in 97p, SHMEM 4 of 12 (m). This explains why "libfabric also
  stalls sometimes" (it did, at 4 n) and why SHMEM is worse: same mechanism, 3 times the exposed pages.
- Why the first node: only NUMA node 0 of the job's first node reaches the score threshold; other nodes sit at 0-11 (m). The cause of the asymmetry is not isolated (open, section 10).
- Why trace-free runs also stall: items 97i/97j (no trace files) stalled the same way, so dirty trace pages on the shared
  file system, a candidate at one point, are not needed for the stall.

### 4.4 The decisive evidence (how it was found)

- Heartbeat counters (per-second /proc/vmstat deltas, section 8) showed storms starting at 19-20 s (the first `bs` compute) lasting 5-8 s, every second with
  `pgmigrate_fail` of 17k-177k and `compact_isolated` 18k-190k, scan counters jumping at the end: that is one proactive pass.
- `COMM_TRACE_HB_MEM` (item 97k/97l, 97m): buddyinfo per zone, fragmentation score, meminfo, kcompactd CPU ticks (98 ticks per second for 8 s in the storm), KFD `evicted_ms`.
  97k (10 clean runs) showed free memory about 210 GB per node and scores 1-17 everywhere, so "fragmentation" in clean runs was low;
  the storm run 97m r1 DT had score 93 on node 0, kcompactd busy, 90-125k failed migrations per second, KFD evictions +42 s.
- The per-mapping smaps capture at 12, 25, 40 and 70 s (items 97m/97n) summed pages by category (anon 4 KiB, anon THP, shmem 4 KiB,
  shmem THP, ...) for the node-process and each helper. The one big 4 KiB shared mapping was the /dev/zero heap; it matched the failing-page counts.
- Fix test: with the heap collapsed, failing storms vanished (97r: failing storms only in the non-X arms), although the job's node 0 still compacted in every run, now
  with successful migrations (section 6).

### 4.5 The steady gap at 5-6 nodes, a second and separate effect

- The gap appears as a **step** at the layeq to layv switch (2 and 4 nodes use layeq; 5 and 6 use layv), not as a slope with node count (scaling items 95u-95x, 1.6e10 digits per node, traced, means of 2 runs, m):

| Nodes | L (s) | DTP (s) | DT (s) |
|---|---|---|---|
| 2 | 43.9 | 45.6 (+3.9 %) | 45.5 (+3.6 %) |
| 4 | 52.4 | 55.3 (+5.5 %) | 56.0 (+6.9 %) |
| 5 | 62.6 | 68.0 (+8.7 %) | 76.1 (+22 %) |
| 6 | 58.5 | 66.9 (+14 %) | 72.9 (+25 %) |

- Rate times peers (aggregate GB/s per NIC while sending): L 22 / 22 / 18 / 20 at 2 / 4 / 5 / 6 nodes, DT 21 / 21 / 9.3 / 8.8 (m). SHMEM's per-put rate is flat at 1.6-2.1 GB/s regardless of
  how many puts are in flight (so not thrash-like) and the same for every link and time window (no slow node or link).
- **INTER2 mechanism (item 95z1 and a code-and-trace reading).** With `PX_SEND=3` the helper does put, then a blocking `shmem_ctx_fence`, then
  the signal word, per command: effectively one flow per NIC at about 15 GB/s. The "done" stamp and the quiet are taken only when the
  command ring is empty, so the first exchange's completion waits for the second's puts too: both finish together, and the NIC idles
  about 5 ms between rounds. `COMM_LAYER_INTER2=0` removes the idle (modelled 10.3 against 14.2 ms per layv exchange; measured layv rate 14.4 against 3.95
  GB/s per APU). It is **not** the default (the default stays 1, comm_layered.c `comm_layered_inter2_on`); the application totals did not separate because
  stalls hit those runs too. Re-test now that stalls are gone.
- NIC address-translation (ATU) cache misses at 6 nodes, same bytes moved (cxi counters, m): libfabric 17-18 %, SHMEM default 24 %, T64 (64 MiB blocks) 62 %.
  The libfabric arm with its pool in host memory (LH) missed 15 %, so host memory itself is not the problem; the SHMEM access pattern is. The NIC driver uses large
  translations only for hugetlb VMAs (by reading the driver source), not for THP, and aac7 has no hugetlb pool, so this cannot be tested here. Setting FI_CXI_ATS or FI_CXI_ODP had no effect.
- Host-registered (not device) memory was ruled out as slow for GPU kernels: microbench 1.3-1.75 TB/s in all memory kinds, also under 4-node traffic (item 95g); the `ktime` copies add about 0.57 s per APU (about 10 % of a 5.8 s gap).

## 5. The fix and its validation

- **Change.** In ecalc/comm_shmem.c the existing mmap wrapper (already used for the helpers' heaps) now also recognises the node-process's own heap
  mapping (shared, /dev/zero or anonymous, at least the heap size): after the library's `mmap`, it pre-faults the range with several threads (`COMM_SHMEM_PX_INIT_THREADS`, default 4) and
  collapses it with MADV_COLLAPSE before the library registers it with the NIC. Address, flags and file descriptor are unchanged, so digits cannot change.
  One log line per process: `comm_shmem: pe N: COMM_SHMEM_NP_HEAP_THP=1: heap ... PMD-mapped after, collapse <s>`. It also covers `COMM_OFI=1` runs.
- **Cost.** 3096 MiB fully PMD-mapped in 1.9-2.0 s, overlapped with other start-up work; DTX init 21.8-23.4 s equals clean DT (m, 97r).
- **Validation (m).**
  - Gates, 1 and 2 nodes, SHMEM and libfabric, switch on and off: digits identical (97q, 97s; on the main-based build 97w).
  - 3 nodes, 4.8e10: with the fix, 0 stalls in 16 runs (SHMEM 47.4-50.9 s; libfabric with it 45.9-48.0 s). Without it 9 of 24 stalled (items 97q-97t: plain SHMEM, the reserve-only arm and libfabric), up to 70.3 s.
    (Counted per run from items 97q, 97r, 97s, 97t: with the fix 8 SHMEM, 4 SHMEM + reserve and 4 libfabric runs; at 2 nodes, item 97v, 6 more runs with the fix, none stalled.)
  - Same node sets: SHMEM with the fix about 6 % behind libfabric when libfabric runs well (49.6 against 46.6 s, 97q/97r); at 2 nodes (97v, 3.2e10) SHMEM+fix 43.3-45.1 against
    libfabric 43.0-45.2 s: parity.
  - The user adopted it as default on 2026-10-18 22:41 ET (D48) after gates 97w passed (1 n and 2 n, SHMEM and libfabric, default against `=0`).
- **Why it works and where it stops.** It removes only what the kernel cannot move. Compound pages are skipped by compaction. Anything else pinned in 4 KiB
  shared or file pages would fail the same way (section 8 checklist).

## 6. Remaining gap and residual effects

### 6.1 Residual GPU-queue evictions

- After the fix, node 0 of the job's first node still gets compaction passes in every run (score 91-99). They now succeed (the pages they move are movable), but each
  successful migration of a page mapped by the process still fires the MMU notifier: KFD `evicted_ms` summed 1.4-12 s per run in the X arms (m, 97r/97s), e.g.
  DTX 1.4-1.9 s against DTXR 3.0-3.8 s. No visible wall-clock cost at 3 nodes yet (clean runs equal within noise), but at many nodes a 1-second freeze of one node
  is a 1-second loss for everyone (section 6.4).
- Options: (a) `compaction_proactiveness=0` per job (root; AMD guidance keeps 20, so treat as an A/B, not a new default); (b) hugetlb heaps and staging on the target (compaction never touches
  them); (c) keep node 0 from dropping below the threshold (tried as a forced large-block reserve, section 7: not helpful on aac7).

### 6.2 Steady gap at 3 nodes after the stall class was removed (pre-fix clean runs, m)

| Phase | L (s) | DTI (s) | DTI - L (s) |
|---|---|---|---|
| total | 46.29 | 49.75 | +3.46 (7.5 %) |
| init | 21.54 | 23.07 | +1.53 (transport open 7.5 against 3.0 s; helper heap populate+collapse about 2.9 s per helper) |
| bs comm spans | 4.63 | 4.97 | +0.34 |
| recip comm spans | 5.57 | 6.0 | +0.43 |
| dm comm spans | 6.77 | 7.6 | +0.83 |
| rest | | | +0.37 (noise level) |

Per-exchange latency of DTI is lower than libfabric's (total 3.7-3.8 ms against 4.9-5.6 ms), so the excess is around the exchanges, not in them: cause open
(candidates: host-side hand-offs between node-process and helper at about +3 ms per exchange in the 4-node host-step timers, item 95i; proxy-pool copies about 10 % of the 4-node gap;
CPU affinity and HBM contention from the bounce copies, modelled about 0.5 s). At 4 nodes 6.4e10 (items 95e, 95j) DT/DTP were at parity with L, so at that size the gap is within noise.

### 6.3 At 6 nodes after the fix

Re-measured 2026-10-19 (items 98f, 99b, 99g, 3.8646e11, m): no stalls; SHMEM 211.7-213.1 s (sd 2.4-8.6) and PX3 205.7-208.1 s against libfabric 183.2-190.7 s (section 12). Before the fix, best SHMEM settings gave 221.4 s best against libfabric 207.5 s mean (95o, m); 5- and 6-node stall events had matching signatures (node-wide, all
APU threads, 0.5-4.2 s) but the traces predate the kernel counters. 

### 6.4 Scale-out model (mod)

Fitting a per-node stall probability to the 24 default runs, the stall excess grows like mu * ln(N p); at 576 nodes the model gives SHMEM 2.3-3.5 times libfabric, with stall variance (+256..427 s) dominating a steady gap of 11-81 s
(results/SHMEM.md). A separate lockstep model says the stall rate per node must fall by a factor of about 300 from the 6-node rate (3.7e-3 per node-second, modelled from four stalls) to keep the loss under 5 %
at 576 nodes; exchange-level tolerance can hide only 0.01-0.3 s of a multi-second freeze (dependencies: the forward NTT needs every peer). So the target needs the cause removed, not tolerated.

### 6.5 Corrected eviction figures at 4 nodes (m, 2026-10-19)

- 4 n, 6.4e10 digits, traced runs with the heartbeat memory counters: the per-GPU `evicted_ms` values of a process are almost equal, so one process-wide eviction is counted once per GPU (4 times) in the summed figure. Real queue-eviction time per node-process per run is 0.86 s with SHMEM and 1.21 s with libfabric (the earlier 3.2 and 4.9 s summed figures divided by 4).
- Timing: 93 % of the SHMEM eviction time falls in the first 30 s (start-up; init is 23-26 s), 59 % for libfabric. 98 % of it falls in seconds in which kcompactd migrated pages; the correlation is with kcompactd activity and migrated volume (0.4-7.7 GiB per node-run), not with direct compaction.
- No wall-clock cost is separable at 4 n. Likely source (not proven): 4 KiB pages in GPU-registered host staging buffers when NUMA node 0 has no free 2 MiB block; node state (three of four nodes held about 76 GiB of foreign shared memory) amplifies. The NP and helper heaps are fully PMD-mapped and are not a source. A default-off switch `MEM_HSTAGE_COLLAPSE=1` for that test exists on an unmerged branch; its 4-node A/B is queued.

## 7. What did not work, and why

Items are on aac7; "branch" names are local development branches (the dropped ones are archived in the local archive and deleted; msgtrace was merged into main). Every result is paired or interleaved unless marked.

| Idea | Items | Result | Verdict |
|---|---|---|---|
| Host-memory defragment before the run (memory preparation step, `MNRUN_MEMPREP_GB`, `MEM_PRECOMPACT_GB`) | 72g, 72h, 72i, 72n, 97g/97h | Removes about a third of the loss on a fragmented node; cost moves to the next memory-touching step; 4-158 s per node; 6 of 8 runs with prep still stalled (75a); 97g/97h (Q arms): no effect on the kcompactd stalls | Merged default-off (ead367d), removed 2026-10-19 (task 4); not the fix; start-up itself fragments node 0, so "prepare once before" cannot work |
| Per-allocation compaction `MEM_DEV_COMPACT` | 73d, 73e | After the fragmenter: 1220 s and one kill at 1800 s; populate calls of 25-869 s on exhausted nodes | Dropped, branch deleted |
| Own allocation (`MEM_OWN_GB`: one THP+MPOL_BIND reservation per NUMA node, pools carved from it) | 94a-94c, 95d | Gates ok at 1 n; at 2 n out-of-memory, then 849.7 s against 356.7 (reservation raced the proxy heaps, helpers lost huge pages, compaction storm); a fix (95d) still timed out | Parked after 3 failed rounds |
| Large-block reserve `MEM_FRAG_RESERVE_GB` / `_NODES` / `_FORCE` (branch fragres, "#1") | 97o, 97p, 97s, 97t | The guard skipped it on every node (start-up needs 113.6 GiB/node, 50 GiB large blocks free on node 0); forced 6 GiB on node 0 lowered the score (60 to 12) but init +4-8 s and, without the heap fix, storms got worse (466k/392k failed, KFD 23-30 s) | Dropped (user adopted only the heap fix, D48) |
| `MNRUN_MEMPREP_GB=100` as stall fix | 97g, 97h | No effect on the proactive storms | Dropped |
| `MEM_HSTAGE_THP=0` (4 KiB host staging) | 97b3, 97b4 | Storms still occurred (DTIH r2 55.1 s) | Not the stall; removed 2026-10-19 (task 4) |
| `COMM_SHMEM_PX_INIT_THREADS=16` | 97i, 97j | Init 23.0-25.0 against 22.4-25.5 s: no gain | Dropped |
| `COMM_SHMEM_PX_CONC=1` (concurrent puts, per-command done stamps) | 97c, 97d | Clean runs equal DTI/DTC; stalls hit either arm | Dropped |
| `COMM_SHMEM_PX_INIT`, `MESH_PAR`, `PX_PIN` | 97e, 97f | Clean SHMEM runs 49.7-52.5 s for all arms | No gain; `PX_INIT` and `MESH_PAR` removed 2026-10-19 (task 4), `PX_PIN` 2026-10-18 |
| `COMM_LAYER_VSLOT_RELEASE=0` (DTIR) | 97a/97b | Init +3 s (4096 against 2560 MiB heap), still stalls (r2 170.8 s) | Not the cause (pool is first-fit, no hipMalloc in the release path); switch removed 2026-10-19 (task 4) |
| `FI_MR_CACHE_MONITOR=memhooks` (DTIM) | 97b | Erratic (50-98.5 s) | Dropped |
| `PX_WORKERS`, `PX_CTX` multi-context issuing | 72c, 96-series | Higher proxy rate, same node throughput | Not adopted |
| `PX_SEND=2` + shift (SS) | 72b, 72d, 72j, 72l, 72p | 12 pairs, no separation | Not adopted; PX3 was built to replace it |
| PX3 (`PX_SEND=3`) | 72o, 72r, 75a-75d, 76a | Probe gap closed; application totals not separated (stalls dominate); T6 at 2 n +24.1 s [-34, +83], clean pairs +0.1 s | Merged default-off, not default |
| `COMM_SHMEM_PX_THP=0` (4 KiB helper heaps) | 95h | Init 39-40 s against 24-26 s | Rejected; THP stays default |
| `ECALC_HOST_THP=0` | 65 | +21.4 s | Not adopted |
| `COMM_SHMEM_POOL_MB=6144` | 78 | +18.6 s | Not adopted |
| `COMM_SHMEM_PE_PER_APU=2` | 80, 95b | +5.6 s [+2.5, +8.6] at 2 n; also worse at 2 n 3.2e10 | Not adopted (aac7 has one NIC per APU) |
| GPU signalling switches (`RD_OVERLAP`, `RD_NOQUIET`, `PX_SPIN`, branch gpusig) | 95m, 95r | No gain, one arm worse (76.5 s) | Dropped |
| Arena-first allocation (`ECALC_ARENA_FIRST`, branch arenafirst) | 95l, 95l2 | No benefit; conditional memprep (`MNRUN_MEMPREP_IF`) removed the fragmentation loss for a price | Not merged |
| NUMA bind of staging and heaps (`MEM_NUMA_BIND`, branch pxbind) | 94u2 | No effect on clean nodes at 2 n; placement was already correct | Not merged |
| Staging in device memory (env-only form `VSLOT_POOL=0 INTER2=0 VSLOT_RELEASE=0`) | 95e | 83.0 s against 62.9 | Dropped; a coded device staging was only modelled (5-8 % of the gap) |
| Libfabric tuning knobs (`FI_CXI_ATS`, `ODP`, hidden libsma pipeline knobs) | 94r, 95z0, 95z1 | Ignored by the library or no help | Dropped |

### Wrong turns worth remembering

1. **"Release and reallocation of v-slots causes the stalls."** False: the pool is first-fit with no `hipMalloc` in that path (comm_layered.c ~500-540); DTIR still stalled.
2. **"`COMM_LAYER_INTER2=0` causes stalls."** The 3-node results (DTI storms in 4 of 4 runs of one item, DT in 0 of 4) came from which node set each arm ran on. Compare arms within one item on mirrored orders.
3. **"Fragmentation is not the trigger"** (97k clean runs had score 1-17) and the opposite **"fragment-free nodes before the run fix it"**: neither holds. Only node 0 of the first node is fragmented, and it
   becomes so *during* start-up. Defragmenting before the job (memprep) cannot keep it defragmented.
4. **"Dirty trace pages on the shared file system cause the failing migrations."** Stalls happened without traces (97i/97j).
5. **"The helper `/dev/shm` heap (3096 MiB) is the failing set."** The helpers' heaps were already THP; the failing mapping is the *node-process's own* library heap. libfabric stalls too, which was the clue.
6. **"The steady gap is the GPU `call` stage."** Per-exchange stage timers showed the 4-node `call` doubling only for SHMEM and host-registered pools; at 2 and 3 nodes it is equal. The loss is around the exchanges (host hand-offs, imbalance), not inside the GPU stage (items 94q, 94k, 95g).

## 8. Tools and methods

### 8.1 Diagnostics available

- **COMM_TRACE v2** (merged into main 2026-10-18 from branch msgtrace after 1 and 2 node gates, item 97x; its switches are default-off and only diagnostic): `ecalc/comm_trace.h`, analysis tool `tools/trace_report.py`. The per-message trace records, for every message, the stages
  (ring post, proxy pickup, put, done, signal, arrival, copy done) with cross-node clock offsets (bound ±1.6-2.1 µs measured at 6 nodes), a report per exchange kind and a stalls section.
  Known limits: for layeq the proxy columns were a report artefact until fixed; copy-done stamps measure issue until fixed by hipEvent timing.
- **Heartbeat** (`COMM_TRACE_HB=1`): a thread per process samples every ~1 s: `/proc/vmstat` compaction, THP, allocation-stall, pgmigrate and numa counters, rusage, thread context-switch counts, and logs scheduling gaps above `COMM_TRACE_HB_GAP_MS`.
  Extra: `COMM_TRACE_THR=1` (thread snapshots), `COMM_TRACE_WD_MS=100` (progress watchdog), `COMM_TRACE_PH=1` (phase tags), `COMM_TRACE_HB_PX=1` (proxies too).
- **HB_MEM** (`COMM_TRACE_HB_MEM=1`, needs HB): per-zone buddyinfo and fragmentation score, meminfo (Dirty, Writeback, Mapped, Shmem, Unevictable, Mlocked, AnonPages), kcompactd and khugepaged CPU ticks,
  and the KFD per-GPU `evicted_ms` from the GPU driver's per-process statistics in sysfs. `tools/trace_report.py` section 8a prints the storm seconds with fragmentation and KFD columns; section 8b the threads/watchdog view.
- **smaps capture** (queue-item hook, a few shell lines): at 12, 25, 40 and 70 s after launch read the process memory map for the node-process and each helper; sum Rss, AnonHugePages, ShmemPmdMapped
  per mapping and by category (anon 4 KiB, anon THP, shmem 4 KiB, shmem THP, file). The 4 KiB shared category identifies the compaction victims.
- **Timers already in main:** `COMM_TRACE` v2 (heartbeat, phase tags, per-PE timestamps; `COMM_PHASE_TS` was removed 2026-10-18, D49), `COMM_XSTATS` (per-exchange stage timers; `COMM_LAYER_STATS` was removed 2026-10-19, D56), `ECALC_VERBOSE=2` (was `COMM_SHMEM_VERBOSE=1`, removed 2026-10-19) (pool and heap lines).
  The `t_comm` benchmark (ecalc/tests/t_comm.c) measures the transport alone and takes `T_COMM_TRACE`.
- **Queue-item generators** built the packed pairs with mirrored arm orders ("GROUP" items: several items start together on disjoint nodes), one item per node set, trace and
  heartbeat on every run, and a VERIFY step that digit-checks every run. Keep the discipline even if the generators are replaced.

### 8.2 Environment for the full diagnosis

```
COMM_TRACE=1 COMM_TRACE_HB=1 COMM_TRACE_HB_GAP_MS=20 COMM_TRACE_THR=1 COMM_TRACE_WD_MS=100 COMM_TRACE_PH=1 COMM_TRACE_HB_MEM=1
ECALC_VERBOSE=2   # plus the arm's own switches
```

Size and shape: 3 nodes at 4.8e10 digits (about 46-50 s), arms run as packed 3+3 pairs with mirrored orders and an A/A control. Smaller (2 nodes, 1.6e10) answers allocation and start-up questions in under 15 minutes
but cannot show layv, the 5-6 node step, or node-0 compaction of a job's first node reliably.

### 8.3 Healthy values (post-fix)

- 3 nodes 4.8e10: SHMEM 47-51 s, libfabric 46-48 s; init 21.8-23.4 s; node-process heap line shows PMD-mapped = heap size, collapse about 2 s.
- 2 nodes 3.2e10: both arms 43-45.5 s. 1 node 6.441e10: 65.9 s, CV 2.5 % (no stalls in 18 runs).

### 8.4 If stalls come back: a checklist

1. **Is it node-wide?** Heartbeat gaps or the late-node pattern: all four APU threads of one node stop at the same instant; others wait (skew), while the heartbeat thread keeps ticking. If instead only some threads
   or one APU stop, look at the intra rendezvous, GPU clocks, or one NIC.
2. **Counters per second on the late node** (/proc/vmstat deltas): `compact_stall` and `allocstall_*` up means direct reclaim or compaction in the allocation path (a new huge allocation under fragmentation: check `thp_fault_fallback`,
   `thp_collapse_*`, and which allocation). `compact_stall` = 0 and `compact_daemon_wake` = 0 with `compact_isolated` and `pgmigrate_fail` rising and ~19 M `compact_migrate_scanned` at the end is the proactive pass (this report's case).
3. **kcompactd CPU ticks and the fragmentation score** (HB_MEM). Score above 90 on a NUMA node starts a pass.
4. **KFD `evicted_ms` per GPU**: rising by seconds across the storm confirms GPU-queue eviction as the way a page-migration storm freezes the process.
5. **smaps capture:** find the large 4 KiB shared or file-backed mapping that is NIC-registered or pinned. Candidates after the heap fix: another library mapping, a new pool, a registered host buffer without
   MADV_HUGEPAGE, a changed `SHMEM_SYMMETRIC_SIZE` (a larger heap means more victims), a library upgrade that remaps the heap (the fix keys on a shared /dev/zero mapping of at least the heap size).
   Check the log line `COMM_SHMEM_NP_HEAP_THP=1: heap ..., PMD-mapped after` first: if it reports an error or fewer PMD-mapped MiB than the heap, the collapse failed (not enough 2 MiB blocks at start-up).
6. **Rule out the node set:** repeat on a second node set; keep an A/A arm; compare arms inside one item; the same arm is clean on one node set and storms on another.
7. **Do not guess from totals.** Single runs differ by 10 % or more at 6 nodes. Use the phase timestamps and counters.
8. If it is something else (network retry, GPU throttling, node daemons, CPU oversubscription): the ranked table of causes with counter signatures was written before the root cause was found and still serves as a map
   (memory management 30 %, network 15 %, library/driver events 15 %, GPU/power 10 %, node daemons/firmware 10 %, CPU oversubscription 10 %; priors, a).

## 9. Implications for the target system

(The target is a much larger system with the same node design: 4 APUs and 8 NICs per node. Its owners control kernel, memory and fabric settings; aac7 is user-only, and no administrator requests are to be made for aac7.)

- **hugetlb heaps.** The target will have hugetlb pages. A hugetlb-backed symmetric heap, staging and proxy pools are skipped by compaction (not on the LRU), need no populate+collapse at start-up (saves about 2.9 s per helper,
  and the 2 s per node-process), and give the NIC 2 MiB (or larger) translations (the cxi driver sizes NIC pages from hugetlb VMAs only, not from THP): this should cut the 24 % to 62 % ATU miss rates seen with 4 KiB mappings. The HPE docs say the legacy symmetric heap is backed by huge pages;
  whether OpenSHMEMX 11.8 on this node type does it must be checked on the target (the library's mapping on aac7 is plain /dev/zero shmem). LLNL reports 4 KiB pages exhausting the MR cache on MI300A and 2 MiB pages via libhugetlbfs fixing it (public slides).
  Reserve the pool at boot or in a job prolog, before fragmentation grows; the same HBM is shared with THP-always, which AMD requires for unified memory.
- **Compaction settings.** AMD's MI300A guidance keeps `compaction_proactiveness=20` and `compact_unevictable_allowed=1`; do not assume 0 is safe: test it per job (A/B: stalls, residual KFD evictions, GPU allocation time at start-up).
  Per-job prolog compaction plus dropping caches gives every job a clean start (cannot stop start-up from fragmenting node 0, but pairs with the heap fix).
- **NICs.** `SHMEM_OFI_NUM_NICS` defaults to 1 NIC per node; on a node with 8 NICs set it to 8 with a NUMA or user mapping (aac7: 4, capped by the launcher because the library aborts when asked for more NICs than exist).
- **Traffic class and routing.** Keep all-to-all traffic on the best-effort class with default adaptive routing; one class preserves put-then-signal ordering. Keep ODP and ATS off (pinned memory regions) and ask HPE about the
  IOMMU setting for the NIC and MI300A (AMD says IOMMU off, ATS needs it on). Fabric-manager group-load and bias settings are not public: ask HPE and A/B them.
- **Exchange design.** Plan A is direct all-to-all (one message per peer); the two-stage rack-then-column exchange is a fallback built only if direct fails. At 576 nodes a direct exchange sends 4600 messages per APU per exchange against 568 for the two-stage design (mod);
  the two-stage wins 8 times when message-rate-bound, ties when the global links are at most half loaded, and loses 1.9 times if bandwidth-bound with full taper (mod; design notes only).
- **Stall exposure.** Section 6.4: at 576 nodes a single-node freeze of seconds costs everyone. After the fix the first step on the target is to measure the stall rate per node-hour with the heartbeat and HB_MEM on a short run, before assuming it is zero.

### 9.1 hugetlb option for the target

`COMM_SHMEM_HUGETLB=1` (off by default, owner L5, review at the target session 2027-03-31) maps the node-process's symmetric heap from the 2 MiB hugetlb pool
instead of shared `/dev/zero` pages, so compaction cannot touch it at all (THP pages can still be split or migrated in principle). Scope: the node-process heap only.
The proxy helper heaps stay `/dev/shm` files with `COMM_SHMEM_PX_THP`: the helpers and the node-process share them by path, and a hugetlb version would need
hugetlbfs files or fd passing (not done). Without a pool (aac7) the mapping fails with ENOMEM, the run prints the needed and free page counts and falls back to the
`COMM_SHMEM_NP_HEAP_THP` path. Not measured: aac7 cannot run it. On the target: reserve about heap size / 2 MiB pages per node-process (plus the library's own needs),
check the printed line, and compare stall counts and the heap's `Shared_Hugetlb` in the `heap ... MiB PMD-mapped after` line (it reports hugetlb MiB then).

## 10. Open questions and next steps

1. **Re-run 6 nodes**: done 2026-10-19 (items 98f, 99b, 99g): no stalls (sd ≤ 5.8 s, m); see section 12.
2. **INTER2=0 as the default under PE_PER_APU=1 and PX_SEND>=2.** Two-line change, layv rate 14.4 against 3.95 GB/s per APU (m); needs gates (digits cannot change, scheduling only) and a clean statistical test now that stalls are gone. Measured 2026-10-19 (item 98n, 4 n, n 8 each): no gain (DT 53.8 = DTIX 53.8 s, m).
3. **PX3 + shift + 64 MiB**: resolved 2026-10-19: plain PX3 (4 MiB, rank order) is the default (D53); shift and larger chunks gave no gain and 64 MiB + shift was slower at 6 n (m); see section 12.
4. **Why start-up exhausts the 2 MiB blocks of node 0 of the first node** and whether a small change (allocation order, `madvise`, per-NUMA reservation after the heap) keeps node 0 below the compaction trigger without a net cost (the reserve experiment cost init and the guard skipped it).
5. **Residual KFD evictions** from successful passes (1.4-12 s per run): quantify the wall-time cost at 4-6 nodes; test `compaction_proactiveness=0` where available.
6. **Steady gap at 3 and 6 nodes** (+3.46 s, 7.5 % at 3 nodes before the fix): cause open. Candidates: host hand-offs between node-process and helper (about +3 ms per exchange on D, +0.7 ms on DT, 4 nodes), proxy-pool copies (about 0.57 s per APU), imbalance, SHMEM start-up (transport open +4.5 s of which 2.9 s helper heap populate+collapse).
7. **COMM_TRACE v2 / HB / HB_MEM** are merged into main (2026-10-18, gates item 97x); use them as in section 8.
8. **Hugetlb on aac7 is impossible** (no pool, overcommit 0), so the target-only path (hugetlb heap switch) can only be tested for fallback behaviour here. A default-off switch backing helper heaps with memfd MFD_HUGETLB or hugetlbfs is in the Proposed list.
9. **Branches:** the experiment branches of this work (fragres, fragres_mt, fixboth, npheap_mt, pxconc, pxctx, gpusig, ktime, inittl, arenafirst, ownalloc, pxbind) are archived as a git bundle in the local archive (`archive:branches_2026-10-18/`, with a README of outcomes) and deleted from the repository.

## 11. Reference

### 11.1 Switches (name, default now, status)

| Switch | Default | Status |
|---|---|---|
| `COMM_SHMEM_NP_HEAP_THP` | 1 | the stall fix, D48 |
| `COMM_SHMEM_PX_THP` | 1 | helper heaps in 2 MiB pages, D36 |
| `COMM_SHMEM_PE_PER_APU` | 1 for multi-node `COMM_OFI=0` | one proxy PE per APU (D29, D31); 2 tested, worse on aac7 |
| `COMM_SHMEM_PX_NUMA`, `COMM_SHMEM_PX_KCOPY` | 1 | N7C/N7D proxy-mode defaults (D31) |
| `COMM_SHMEM_PX_SEND` | 3 under `PE_PER_APU=1` (else 1) | PX3 is the default since D53 (user 2026-10-19), `=1` the fallback; 2 (SS) was removed |
| `COMM_SHMEM_PEER_ORDER` | rank | `shift` is in the DT/T6 set |
| `COMM_SHMEM_PX_BLOCK_MB` | 4 | 64 in the DT/T6 set |
| `COMM_LAYER_INTER2` | 1 | 0 is the candidate for SHMEM (section 4.5) |
| `COMM_LAYER_VSLOT_RELEASE` | removed 2026-10-19 (task 4); was 1 (follows VSLOT_POOL) | 0 tested, not useful |
| `COMM_SHMEM_PX_PIN` | 0 | not significant |
| `COMM_SHMEM_PX_INIT` (removed 2026-10-19, task 4), `_INIT_THREADS`, `MESH_PAR` (removed 2026-10-19, task 4), `OFFS_WITH_DATA` (removed 2026-10-18, D49) | 0 / 4 / 0 / 0 | no gain; INIT_THREADS also sets the collapse threads of the fix |
| `MNRUN_MEMPREP_GB`, `ECALC_MEMPREP_ONLY`, `MEM_PRECOMPACT_GB`, `MEM_HSTAGE_THP` | removed 2026-10-19 (task 4) | merged default-off (ead367d), not the fix |
| `COMM_PHASE_TS` | removed 2026-10-18 (D49) | diagnostic; COMM_TRACE v2 replaces it |
| `COMM_SHMEM_LAYV_ANY` | removed | no effect over 8 pairs; gates passed |
| `COMM_TRACE*`, `COMM_TRACE_HB*`, `MEM_FRAG_RESERVE_*`, `MEM_OWN_GB`, `MEM_NUMA_BIND`, `COMM_SHMEM_PX_CONC`, `PX_WORKERS`, `PX_CTX` | n/a | only on development branches |

### 11.2 Decisions D36-D48 (summarised)

- D36 (10-15): `COMM_SHMEM_PX_THP=1` is the default of the proxy mode (huge-page helper heaps). D35 earlier kept PX_THP/PX_PIN off as insignificant.
- D37 (10-17): queue rules dropped; the orchestrating agent decides node use (safety rules kept). Packing 2-6 concurrent items while the per-user limit is 6.
- D38 (10-17): two old branches deleted on GitHub. D39: the huge-page SHMEM heap branch (item 40) dropped. D40: no request for more than 6 nodes.
- D41 (10-18): an 8-hour SHMEM plan with the node split at the coordinator's judgement. D42: no node clean-up after fragmenter items. D43: no further requests to the cluster administrators; work with existing settings.
- D44: merge default-off switches after gates (1 and 2 nodes, digits identical) and review. D45: a default may be changed when at least 8 ABBA pairs at 4 nodes have a CI excluding 0, confirmed at 6 nodes, digits identical.
- D46: two abandoned branches deleted. D47: spare aac7 capacity goes to more SHMEM confirmation runs.
- D48 (10-18, 22:41 ET): of the two fixes proposed for the kernel storms (#1 large-block reserve on node 0, #2 remove the unmovable 4 KiB pages), #2 is adopted: `COMM_SHMEM_NP_HEAP_THP` default on, merged to main after the main-based gates (97w); #1 not adopted.
- Direction set on 2026-10-18 evening (user): a single protocol (pure SHMEM; libfabric only as a benchmark); stall diagnosis first, then design so one node's stall does not block everyone; target model: direct all-to-all first, two-stage fallback; target will have hugetlb pages and is controlled by the project's own team.

### 11.3 Item index (selected)

| Items | What |
|---|---|
| 37-41, 49-56 | THP default, LAYV_ANY, variance data (24 runs) |
| 43-54, 57, 66, 67, 71 | variance study, 1-6 node comparisons |
| 65, 76, 78 | host THP off, fragmenter reproduction, pool size |
| 72, 72b-72r, 75a-75d, 76a | t_comm probes, PX_WORKERS, SS, PX3, application pairs |
| 72g-72n, 73c-73e | precompaction, memory preparation, device compaction, allocation bench |
| 94a-94u, 95a-95n | ownalloc, bind, fast items, ktime, staging, strag, stage probes |
| 95o, 95p, 95r, 95y | 6 node confirmations and sweeps |
| 95q-95x, 96b, 95z0-95z3 | per-message trace, scaling 2-6 nodes, ATU, INTER2 |
| 97a-97b4 | 3-node campaign start: trace, arms L/DT/DTI/DTIR/DTIM, HSTAGE |
| 97c-97j | PX_CONC, init switches, memprep, INIT_THREADS |
| 97k-97n | HB_MEM and smaps capture: root cause |
| 97o-97p, 97s-97t | fragres gates and A/B, forced reserve with and without the fix |
| 97q-97r, 97v, 97w | NP_HEAP_THP gates, A/B (3 nodes, 2 nodes), merge gates |

## 12. Final conclusion (2026-10-19)

- **Stalls: solved.** The stalls came from compaction retrying the 4 KiB pinned node-process heap; `COMM_SHMEM_NP_HEAP_THP=1` (default) removed them. At 6 nodes after the fix there are no stalls: the run-to-run standard deviation is at most 5.8 s (m), against 50 s before. This closes the open question of section 6.3 and item 1 of section 10.
- **Residual GPU-queue evictions: low priority.** Real eviction time is about 1 s per node-process per run (0.86 s SHMEM, 1.21 s libfabric, mostly in the first 30 s) with no measurable wall-clock cost; `MEM_HSTAGE_COLLAPSE` did not change it. Look at them again only if a target trace shows seconds of eviction time.
- **What is left is not a stall.** The remaining distance to libfabric is the exchange gap that grows with the off-node peers (about +7 % at 4 nodes, +9 % at 6 nodes with the PX3 default; results/SHMEM.md); it is waiting on the per-peer chain, not memory behaviour.
- **Defaults after this work** (table 11.1): `NP_HEAP_THP=1`, `PX_THP=1`, `PE_PER_APU=1` for multi-node `COMM_OFI=0`, `PX_SEND` unset = 3 under `PE_PER_APU=1` (D53, `=1` fallback). PX_SEND=4, shift and other chunk sizes were tested and not adopted.
- **Target:** hugetlb and cray-shmem 12.1 are the open items (TASKS 29-32).
