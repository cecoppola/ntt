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


### 6. The NIC miss picture

Per node per run (m): SHMEM about 4-5e9 translation misses (3 n: miss0 5.3-5.5e9, of which the initiator side 4.2-4.4e9, about 80 %, and the target side about 1.1e9, about 20 %; 59 % of translated
requests miss); libfabric with device-memory pools about 1e4 (miss ratio 1.6 %), the same requests (req_ixe 2.0-2.1e9, req_oxe 6.7-7.0e9). Libfabric with a host pool (item 9lc) misses 3.5-3.8e9 and
runs about 17 s slower. Why (d, driver facts): host staging memory is mapped with 4 KiB ATU pages (no free hugetlb pages; THP does not reach the NIC mapping), while device memory registered by dmabuf is mapped
with large entries (hit_d1_0 8.5-9.1e9). At 2 n the misses cost nothing visible (SHMEM within noise of libfabric); at 3 n the remaining gap was not closed by removing the initiator misses (section 4), so
the cost of the misses on the gap is not shown. The target carries device buffers on both sides by construction.

Sections 5 (`PX_QTHREAD`), 7 (unmerged branches) and 8 (node health note) moved to `archive:results_merged_2026-10-24/SHMEM.md`.


The per-item sections (tables, drivers, run lists) were moved to `archive:repo_cl6_2026-10-25/results/SHMEM.md` (2026-10-25); section anchors that cited them now point to this summary.
