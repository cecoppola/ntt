# apumult_R: spilling to SSD and releasing memory on MI300A (research findings)

Agent R, 2026-09-24. Scope: what the apumult techniques (SSD spill, madvise(DONTNEED)) can
do for a **device-resident** e pipeline on MI300A nodes, and what they cost. The research used
web sources plus the Linux v6.8 kernel source (amdgpu/TTM/mm), since several answers are
settled by kernel code rather than documentation. No GPU was available to this agent, so every
claim marked "verify" comes with a 1-minute test to run on aac6 (see §8).

Confidence scale: **H** = documented or read in kernel source; **M** = strong inference from
source or docs, not tested on MI300A; **L** = plausible, not confirmed.

---

## 0. Answers in brief

| Question | Answer | Conf. |
|---|---|---|
| (a) Can an APU pipeline spill to disk without the page cache taking the freed HBM? | **Yes, but only with O_DIRECT (or RWF_DONTCACHE on kernel ≥ 6.14).** Buffered I/O puts the page cache in the same HBM, and 2 MiB `hipMalloc` allocations will **not** reclaim it (they fall back to smaller pages). `posix_fadvise(DONTNEED)` only drops pages that are already clean. | H |
| (b) Realistic spill bandwidth | **aac6 (970 EVO Plus 2 TB, PCIe 3.0 x4):** about 3.3 GB/s write for the first ~78 GB, then **~1.5–1.75 GB/s** (drops to ~0.8 GB/s if the drive is the later "Elpis" revision). Read is **~3.3–3.5 GB/s**. **Target (EX255a-class):** one optional M.2 per node, PCIe Gen4. Expect **~2–4 GB/s write, ~5–7 GB/s read**. The model is not public, so **measure /ssd0 with fio**. HBM moves 3.5 TB/s per APU. | H for aac6, L–M for target |
| (c) Does HIP VMM give a cheap "release dead pages" for device buffers? | **Yes, it works functionally.** `hipMemUnmap` + `hipMemRelease` of a chunk returns its pages. `hipMemCreate` + `hipMemMap` puts pages back later. The cost is kernel page zeroing, **~35–40 ms/GiB** (the same as hipMalloc). It is **still Beta** and has open issues on gfx94x. **Chunked hipMalloc/hipFree gives the same saving** with no Beta API. `madvise` on a hipMalloc pointer **cannot** release anything: the kernel rejects it with EINVAL. | H (mechanism), M (cost) |

---

## 1. MI300A memory model

### 1.1 What backs each allocator

- **`hipMalloc` → TTM "GTT" buffer object made of ordinary system pages. On an APU there is no VRAM.** In the kernel, KFD sees `is_app_apu` and changes a VRAM request into `AMDGPU_GEM_DOMAIN_GTT`. The allocation is counted against both the system-memory limit and the **TTM limit** (`ttm_mem_needed = size`). **(H)** Source: `drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c` (v6.8), lines ~199 and ~1656:
  https://github.com/torvalds/linux/blob/v6.8/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c
  - Pages come from a per-NUMA-partition TTM pool (`amdgpu_ttm_pools_init`, one pool per memory partition with its NUMA node). **(H)** https://github.com/torvalds/linux/blob/v6.8/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c
  - This TTM limit is the **96 GiB-of-128 default** for device allocators. It is raised with `modprobe amdttm pages_limit=134217728 page_pool_size=134217728`, counted in 4 KiB pages. **(H)** https://rocm.docs.amd.com/en/docs-6.2.2/how-to/system-optimization/mi300a.html , https://instinct.docs.amd.com/projects/amdgpu-docs/en/latest/system-optimization/mi300a.html
  - hipMalloc allocates up front, gives the most contiguous pages (158 K TLB misses against about 1 M for the other allocators), reaches the best bandwidth (**3.5–3.6 TB/s**), and costs **~37 ms per GiB** to allocate. **(H)** "Dissecting CPU-GPU Unified Physical Memory on AMD MI300A APUs", https://arxiv.org/html/2508.12743v1
- **`hipHostMalloc`** is up front and costs 200–400 ms/GiB, with 2.1–2.2 TB/s GPU bandwidth in the paper above. AMD says "host allocations are not affected" by the 96 GiB limit. **(H)** Our own measurement agrees: 460 GiB through hipHostMalloc gets past the amdttm cap, at 14.0 TB/s per node (memory note `apu-ntt-measured-facts`). Getting past the TTM cap means it is **not** a TTM GTT BO. The kernel code points to a USERPTR/SVM mapping of ordinary anonymous pages. **(M)**
- **`hipMallocManaged`**: with XNACK off it is up front (2.1–2.2 TB/s). With `HSA_XNACK=1` it allocates on first touch (1.8–1.9 TB/s). **(H)** arXiv 2508.12743; https://rocm.docs.amd.com/projects/HIP/en/docs-7.0.1/how-to/hip_runtime_api/memory_management/unified_memory.html
- **`malloc`**: ordinary anonymous memory. The GPU can reach it only with `HSA_XNACK=1` plus an HMM-capable driver, through GPU page faults of ~18 µs each and up to ~1.1 M pages/s. On MI300A, "physical page migration is neither needed nor useful, however, handling page-faults via XNACK is still necessary". LLNL measures a ~15 % GPU penalty from 4 KiB pages unless THP is on. **(H)** https://rocm.blogs.amd.com/software-tools-optimization/mi300a-programming/README.html , https://hpc.llnl.gov/documentation/user-guides/using-el-capitan-systems/using-el-capitan-systems-memory-management
- AMD **requires** `transparent_hugepage=always` and proactive compaction (`compaction_proactiveness=20`) on MI300A, because "the APU dynamically shares memory between the CPU and GPU". **(H)** instinct.docs.amd.com system-optimization page above.

### 1.2 Does madvise(MADV_DONTNEED) give back HBM that a later hipMalloc can use?

- **On `malloc`/`mmap` anonymous memory: yes.** The pages go straight back to the buddy allocator. hipMalloc's TTM pool allocates from that same buddy allocator on the same NUMA node, so the pages are usable, with two caveats:
  1. Release whole 2 MiB-aligned ranges. With THP=always, a partial range splits a huge page and leaves 4 KiB fragments, and hipMalloc then gets poorer TLB fragments.
  2. Do not use MADV_FREE. It is lazy, and RSS stays up until the kernel is under memory pressure.
  **(H** for the kernel mechanism, **M** for "no fragmentation penalty on the next hipMalloc")
- **With `HSA_XNACK=1` and the GPU touching that malloc memory**, the MMU notifier invalidates the SVM range and the pages are freed. The next GPU access faults in a zero page. This is the clean path for host-resident code such as apumult's. **(M)**
- **On a `hipMalloc` pointer: no.** The CPU mapping of a TTM BO is `VM_PFNMAP|VM_IO` (`ttm_bo_vm.c` line 481), and `madvise_dontneed_free_valid_vma()` forbids `VM_PFNMAP`, so the call returns **EINVAL**. Even without that check, it would only zap CPU PTEs. The BO would stay allocated. **(H)** https://github.com/torvalds/linux/blob/v6.8/mm/madvise.c (lines 820–832), https://github.com/torvalds/linux/blob/v6.8/drivers/gpu/drm/ttm/ttm_bo_vm.c
- **On `hipHostMalloc` memory: not recommended.** If it is a pinned USERPTR BO, the invalidation evicts the process's GPU queues, and the restore worker faults the pages back in, so nothing is saved and there is a stall. **(L, verify)**
- **Trap: freed hipMalloc pages do not return to "free".** `hipFree` hands pages back to the TTM pool, which **clears them** (`clear_page` in `ttm_pool_type_give`). The pool keeps up to `page_pool_size` pages, and AMD's recommended setting is all 128 GiB, until the TTM shrinker releases them under pressure. So after a hipFree, `MemFree` in /proc/meminfo stays low. An RSS or MemFree watchdog such as apumult's pollv3 will misread the situation. Pooled pages are, however, the first ones a later hipMalloc reuses. **(H)** https://github.com/torvalds/linux/blob/v6.8/drivers/gpu/drm/ttm/ttm_pool.c (lines 225–240, 541)

### 1.3 The page cache lives in the same HBM

- MI300A has no DDR: "GPUs, CPUs, OS, ram disks (i.e., /tmp) all share same HBM3". On El Capitan-class systems, **/tmp is a RAM disk in HBM**, so never spill to /tmp. **(H)** LLNL El Capitan Getting Started, March 2026: https://hpc.llnl.gov/sites/default/files/2026-03/Getting-started-March-2026.pdf
- Buffered `write()` fills the page cache: up to `vm.dirty_ratio` (default 20 %, about 100 GB on a 512 GB node) of dirty pages, and after writeback **the clean pages stay** until something reclaims them. They sit on the NUMA node (APU) of the thread doing the writing. **(H**, standard Linux)
- **Why this matters for hipMalloc specifically:** TTM tries high-order (2 MiB) pages with `__GFP_NORETRY | __GFP_KSWAPD_RECLAIM`. That means **no direct reclaim or compaction** for large pages. It only drops to order-0, which does reclaim, when the large pages run out. So a node full of page cache does not make hipMalloc fail. It makes hipMalloc return **fragmented 4 KiB–64 KiB pieces** and the kernels run slower. **(H** code, `ttm_pool.c` lines 88–94 and 430–505; **M** for the performance effect.) Our own note agrees: a run right after a 40 GB digit comparison was 5–15 % slower until the file was evicted (memory note `apu-ecalc-phase8-state`).
- **How to avoid it:**
  1. **O_DIRECT** (buffer, offset and length aligned to 4 KiB; ideally 1–8 MiB I/Os). y-cruncher makes raw I/O mandatory for the same reason, the "thrash of death" in which "the OS is so aggressive with disk caching that it pages itself out". **(H)** https://www.numberworld.org/y-cruncher/guides/swapmode.html
  2. `posix_fadvise(POSIX_FADV_DONTNEED)` **only drops clean pages**. Dirty pages are skipped, so the order must be `sync_file_range(…WAIT_BEFORE|WRITE|WAIT_AFTER)` or `fdatasync`, then fadvise. **(H)** https://man7.org/linux/man-pages/man2/posix_fadvise.2.html , https://man7.org/linux/man-pages/man2/sync_file_range.2.html
  3. `RWF_DONTCACHE` (uncached buffered I/O, through `preadv2`/`pwritev2` or io_uring `rw_flags`) was merged for **Linux 6.14**. It needs no alignment and prunes pages after the I/O. aac6 runs 6.8, so it is not available there, and the target kernel is unknown. **(H)** https://lwn.net/Articles/998783/ , https://www.phoronix.com/news/Uncached-Buffered-IO-Linux-6.14

---

## 2. HIP virtual memory management (VMM) on MI300A

- **API:** `hipMemCreate` (physical), `hipMemAddressReserve` (VA), `hipMemMap`, `hipMemSetAccess`, `hipMemUnmap`, `hipMemRelease`, `hipMemAddressFree`. Support is checked with `hipDeviceAttributeVirtualMemoryManagementSupported`. The docs list growing or shrinking a buffer without copying as the intended use. `hipMemAllocationProp` supports only `hipMemLocationTypeDevice` in 6.4. Newer releases add host-NUMA locations. **(H)** https://rocm.docs.amd.com/projects/HIP/en/docs-6.4.1/how-to/hip_runtime_api/memory_management/virtual_memory.html , https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/memory_management/virtual_memory.html
- **Status:** the API is documented as **Beta**: "feature is complete, it can change and might have outstanding issues". It has existed since ROCm 5.x/6.0 on Linux. **(H)** https://rocm.docs.amd.com/projects/HIP/en/docs-6.0.0/doxygen/html/group___virtual.html
- **Known issues:**
  - On gfx94X CI (MI300 family), hip-tests show intermittent "hipMemCreate reports out of memory" failures for 64–128 MiB requests. The root cause is unknown. **(H)** https://github.com/ROCm/TheRock/issues/8269 , https://github.com/ROCm/TheRock/issues/8270
  - ROCm 7.14 opens **one file descriptor per hipMemCreate**. With `ulimit -n 1024` this shows up as a false OOM at about 1017 handles, and a fix has been submitted. Use large chunks or raise `ulimit -n`. **(H)** https://github.com/ROCm/legacy-rocm-build/issues/6661
  - Unlike `hipFree`, release does not synchronise the device, so the caller must synchronise the stream itself before unmapping. **(H)** docs above.
- **Granularity:** query `hipMemGetAllocationGranularity`. Physical backing is **2 MiB**. Reported values vary by version (7.1.1 reports 4 KiB as recommended, 7.14 reports 2 MiB). Sizes that are not multiples of the granularity return hipErrorOutOfMemory. **(H** for dGPU, **M** for MI300A, verify)
- **Can it release part of a buffer and put pages back later? Yes.**
  1. Reserve one large VA range.
  2. Back it with N chunk handles (e.g. 256 MiB–1 GiB each).
  3. To release: `hipMemUnmap(chunk)` + `hipMemRelease(handle)`. The pages return to the TTM pool (cleared) or the buddy allocator.
  4. To put pages back: `hipMemCreate` + `hipMemMap` + `hipMemSetAccess` at the same VA.
  Kernels keep using one flat pointer. This is the device-memory equivalent of madvise DONTNEED. **(H** mechanism, CUDA-compatible semantics)
- **Cost:** on an APU, hipMemCreate(device) takes the same KFD path as hipMalloc: a TTM GTT BO with zeroed pages (`__GFP_ZERO` on fresh pages, or `clear_page` on pool return). Expect **~35–40 ms/GiB**, i.e. ~25–30 GB/s, single-threaded in the kernel, the same as hipMalloc's 37 ms/GiB, plus the GPU page-table update. Putting back 100 GiB therefore costs about 4 s, which is small next to 100 GiB of NVMe reads (30–60 s). **(M, verify)**
- **VMM memory still counts against the amdttm `pages_limit`** (KFD VRAM path, `ttm_mem_needed`). It is not a way past the 96 GiB default. **(H** code)
- **Cheaper alternative with the same saving:** allocate large buffers as K separate `hipMalloc` chunks with chunk-aware indexing (or a pointer table), and `hipFree` the dead chunks. VMM adds only the single contiguous VA. The zeroing cost is identical. **(M)**
- **VMM/hipMalloc memory cannot be an O_DIRECT target** (see §3). Spilling a VMM chunk still needs a bounce buffer.

---

## 3. Moving device data to and from NVMe quickly

- **O_DIRECT into a hipMalloc pointer fails (EFAULT).** Direct I/O pins the user buffer with GUP, and `check_vma_flags()` returns `-EFAULT` for `VM_IO|VM_PFNMAP`, which is how TTM maps BOs. The same applies to hipMemCreate memory. **(H** code: https://github.com/torvalds/linux/blob/v6.8/mm/gup.c line 1039; **verify on aac6.)** A *buffered* `write()` from a hipMalloc pointer does work, because `copy_from_user` handles PFNMAP, but it fills the page cache.
- **O_DIRECT from malloc or hipHostMalloc memory works**: ordinary pages, and GUP succeeds. On the APU, **those buffers are GPU-accessible** (hipHostMalloc always; malloc with XNACK=1 or `hipHostRegister`). So a spill buffer that lives in hipHostMalloc memory is written to NVMe **with zero copies**, which is the APU equivalent of GPU-direct storage. **(M, verify)** Our measurement says hipHostMalloc memory runs at full kernel bandwidth (14.0 TB/s per node), while the arXiv paper reports 2.1–2.2 TB/s per APU for hipHostMalloc. Re-check with the NTT kernel before moving hot buffers.
- **For hipMalloc (or VMM) buffers use a bounce ring:** 2–4 pinned hipHostMalloc buffers of 64–256 MiB each.
  - Copy each chunk device→ring with an async copy or a copy kernel, then issue O_DIRECT pwrite from the ring, overlapped.
  - On an APU the copy is HBM→HBM: hipMemcpy/SDMA reaches ~58–90 GB/s, and a copy kernel much more. That is at least 20× faster than the SSD, so the bounce is free. https://github.com/ROCm/ROCm/issues/3652
  - The ntt/ `dbig.c` bounce path running at 1.6–2 GB/s (apumult.md §8) is disk-limited, not copy-limited.
- **Submission:** 2–4 threads doing `pwrite`/`pread` with O_DIRECT and 1–8 MiB requests, or io_uring with queue depth 8–32, saturate one NVMe device. For large sequential transfers io_uring saves CPU but no bandwidth. y-cruncher's advice is to keep I/O sequential and large. **(H** general; y-cruncher guide above)
- **AMD "GPU-direct storage":** **hipFile** ("AMD Infinity Storage", AIS) provides direct-to-GPU I/O and automatically falls back to POSIX.
  - Early-access technology preview, "not recommended" for production.
  - Requires ROCm ≥ 7.2, amdgpu-dkms ≥ 30.20.1, `CONFIG_PCI_P2PDMA`, local NVMe only (no multipath), ext4 (data=ordered) or xfs.
  - https://rocm.docs.amd.com/projects/hipFile/en/latest/ , https://github.com/ROCm/hipFile/blob/develop/INSTALL.md , https://rocm.docs.amd.com/en/latest/components/storage-libs.html
  - **On MI300A it buys nothing** beyond O_DIRECT from hipHostMalloc memory, since device and host memory are the same HBM. It would matter only if the target forces hipMalloc buffers and you want to avoid the bounce, and the bounce is free anyway. aac6's default ROCm 7.2.4 meets its version floor, so it could be tried there. **(M)**
- There is also **ROCm XIO** (beta 0.1.0): GPU-initiated NVMe doorbells. It is research-grade and not needed here. https://rocm.docs.amd.com/projects/rocm-xio/en/beta-0.1.0/conceptual/memory-modes.html

---

## 4. y-cruncher out-of-core ("Swap Mode"): lessons

**How it works**
- Swap Mode is true out-of-core arithmetic. Disk is "far memory", and all large operands, including the multiplication working sets, live there. RAM (by default ~94 % of physical memory) is the cache.
- It uses raw I/O (O_DIRECT / `FILE_FLAG_NO_BUFFERING`) to avoid the OS cache.
- Buffers are 64 MB per worker for HDDs and 1 MB for SSDs. Strided access should reach 1/3–1/2 of sequential bandwidth.
- Data is checksummed (0.1 % overhead) because silent disk errors happen.
- Source: https://www.numberworld.org/y-cruncher/guides/swapmode.html
- It pre-faults, locks, and fills its RAM with random data so the OS will neither swap nor compress it. Paging "even a small part" of it to disk degrades performance "by orders of magnitude". https://www.numberworld.org/y-cruncher/guides/memory.html

**Bandwidth requirement**
- Storage needs about **1/4 of memory bandwidth** before it stops being the bottleneck, with diminishing returns above ~1/3.
- In Yee's words, that is ~20 GB/s for a 7950X desktop and ~200 GB/s for a 256-core Bergamo.
- The 100 T pi run was "8-to-1 bottlenecked by storage", and no pi record has yet had enough bandwidth.
- The storage cannot be tiered: all of it must be fast.
- Source: https://www.numberworld.org/y-cruncher/technology_wishlist.html
- With too little bandwidth, CPU utilisation falls below 25 % in the swap phases, and core count stops mattering. https://ehfd.github.io/world-record/optimizing-y-cruncher-to-actually-set-world-records/

**I/O volume**
- StorageReview's 100 T pi run (19 QLC SSDs in RAID 0, >38 GB/s write, 1.5 TB RAM) **read 35.7 PiB and wrote 31.5 PiB**. That is about **350 bytes written per digit**, roughly 29 TB per drive per day for 54 days. https://www.storagereview.com/review/storagereview-calculated-100-trillion-digits-of-pi-in-54-days-besting-google-cloud
- The 314 T pi run used 34 Micron 6550 Gen5 SSDs (about 2.1 PB) for a 110-day run. https://www.tomshardware.com/pc-components/storage/pi-calculating-record-shattered-at-314-trillion-digits-with-a-four-month-run-on-a-single-server-storagereview-retakes-the-crown-thanks-to-storage-bandwidth

**e records**
- **35 T digits**, Jordan Ranous, 2023-12-24: 2× Xeon Platinum 8460H, **512 GB RAM**, **94.5 h** compute and 92.5 h verify. The storage configuration is not published.
- 12 T, D. Christle, 2020: 252 GB RAM, **25 hard drives**, 19.4 days.
- 1 T, S. Kondo, 2010: 12 GB RAM, 8 × 1 TB, 224 h.
- Source: https://www.numberworld.org/y-cruncher/records.html
- In 2010, 500 G digits of e on a 12 GB machine with 4 HDDs (~480 MB/s) took 12.8 days. Yee noted that with enough drives, swap runs are "no longer dominated by disk access". https://www.numberworld.org/misc_runs/e-500b.html

**Lessons for us (inference, M)**
1. Our per-node ratio is SSD 2–7 GB/s against HBM 4 × 3.5 TB/s ≈ 14 TB/s, i.e. **~1/2000–1/7000**, while y-cruncher needs ≥ 1/4. **True out-of-core multiplication (streaming NTT operands from disk on every product) is therefore hopeless** on these nodes. It would be thousands of times slower than in-core.
2. Only **"park dead-until-later buffers"** spilling is viable. Each spilled byte crosses the disk twice per phase boundary, not hundreds of times per digit. That is exactly the apumult pattern (Q, P and µ spilled between phases), and its cost is simply `bytes/BW_write + bytes/BW_read`, which can partly overlap with compute.
3. Choose spill candidates by (bytes freed) ÷ (seconds of I/O not hidden by compute), and put them only at phase boundaries.
4. Use raw I/O, checksums on spill files (y-cruncher sees silent errors), and keep one file per buffer on a local XFS/ext4.

---

## 5. Node-local storage on MI300A HPE Cray EX nodes

**EX255a blade**
- Two 4-APU node cards per blade, 4–8 Slingshot-11 200G ports per node.
- **"1 local NVMe M.2 SSD per node (up to 2 per blade)"**, optional, through the "HPE Cray EX255a SSD M.2 Support Kit" (S0N59A). **(H)** SDSC Cosmos PEARC25 slides: https://www.sdsc.edu/_files/docs/PEARC25_Workshop_Cosmos_M.Tatineni-_IS2_final.pdf ; https://www.glennklockwood.com/garden/nodes/Cray-EX255a ; https://buy.hpe.com/us/en/options/compute-options/hpc-cray-options/hpe-cray-ex255a-ssd-m-2-support-kit/p/s0n59a
- HPE describes the in-system SSDs as attached "by PCIe 4". **(M)**
- **HPE's pages that would give the drive model and capacity (psnow datasheets, QuickSpecs) returned 403 or timed out.** The drive model and bandwidth are **unknown (L)**. A Gen4 x4 M.2 enterprise drive (e.g. 1.92–3.84 TB) is typically 5–7 GB/s read and 2–4 GB/s sustained write.
- A search snippet about HLRS Hunter claimed two M.2 drives, ~3.5 TB, 8/4 GB/s read/write. The Hunter KB page itself confirms only "some EX255a nodes feature a node-local NVMe drive", with the 3.5 TB drives on 16 *CPU* nodes and no bandwidth figure, so treat 8/4 GB/s as unverified. https://kb.hlrs.de/platforms/index.php/HPE_Hunter_Hardware_and_Architecture
- **The target's `/ssd0` is presumably this M.2 drive (apumult ran there). Measure it; see §8.**

**El Capitan / Tuolumne / RZAdams (LLNL)**
- Compute blades have **no local SSD in use**. Local storage is **Rabbit** near-node flash: one Rabbit 4U per chassis holding 16 (+2 spare) SSDs, PCIe Gen4, direct-attached to 8 compute blades (16 nodes) through bulkhead cables, plus an EPYC Rabbit-P storage node. That is about **one SSD's worth per node**, shared, with an x8 PCIe path per node according to Next Platform.
- The default is **`/l/ssd` ≈ 200 GB per node, XFS, wiped at job end**. Larger allocations are available through Flux `#DW jobdw type=xfs capacity=1TiB`. There are 720 Rabbits for 11,520 nodes.
- The Lustre file system is 401 PB at 2.6+ TB/s for the whole machine, i.e. about 0.23 GB/s per node when all nodes write at once.
- `/tmp` is a RAM disk in HBM.
- **(H)** Sources: https://hpc.llnl.gov/documentation/user-guides/using-el-capitan-systems/file-systems-rabbits , https://hpc.llnl.gov/sites/default/files/2026-03/Getting-started-March-2026.pdf , https://hpc.llnl.gov/documentation/user-guides/using-el-capitan-systems/hardware-overview , https://www.nextplatform.com/2021/03/09/livermore-converges-a-slew-of-new-ideas-for-exascale-storage/ , https://blocksandfiles.com/2021/02/23/el-capitan-hpe-rabbit-near-node-storage/ , https://www.opensfs.org/wp-content/uploads/Fast-IO-El-Capitan-Rabbits.revised.pdf
- Rabbit per-node bandwidth is not published. **(L)**

**Other MI300A systems**
- SDSC Cosmos (42 nodes) uses VAST (~300–600 TB) and optional M.2.
- HLRS Hunter (188 APU nodes) has Lustre at ~155 GiB/s across the whole system.

**Implication for a 576-node target**
- Node-local M.2 is **optional hardware**. If present, expect **one Gen4 drive per node (about 2–4 GB/s write, 5–7 GB/s read, 1–4 TB)**.
- Lustre is not a spill target at scale: a system-wide 150–2600 GB/s shared by 576 nodes gives 0.3–4.5 GB/s per node, contended, and runs over the same Slingshot links as the SHMEM traffic.
- **Confirm the device, capacity and fio numbers on the target before planning d_max.** At 1.35×10¹⁴ digits the spill volume per node is several hundred GB, which must fit on /ssd0.

---

## 6. Samsung 970 EVO Plus 2 TB (aac6 compute nodes)

**Datasheet and review figures**
- PCIe 3.0 x4, Phoenix controller, 2 GB LPDDR4.
- **3,500 MB/s read / 3,300 MB/s write** (spec). **1,200 TBW**, 5-year warranty.
- **Intelligent TurboWrite (SLC cache) ~78 GB** on the 2 TB model.
- Once the cache is exhausted, direct-to-TLC writes run at **~1.5–1.75 GB/s**.
- **(H)** https://ssdrive.net/samsung-970-evo-plus-2-tb , https://hothardware.com/reviews/samsung-ssd-970-evo-plus-2tb-review , Samsung datasheet (smaller capacities: 42 GB cache/1 TB, 600 TBW/1 TB) https://image-us.samsung.com/SamsungUS/SSD-970EVOPLUSDSHT-MAR19T-Final-3-18-19.pdf

**Revision risk**
- Units made after about 2021 use the **Elpis** controller: ~3× larger SLC cache, but **sustained writes fall to ~0.8 GB/s** after it, against ~1.5 GB/s for the original, and the drive runs ~10–20 °C hotter (up to ~100 °C).
- Check the PSID/model string with `nvme id-ctrl`. The old part is `MZVLB2T0HALB`, the new one `MZVL22T0HBLB`. These names follow the published 1 TB pattern MZVLB1T0HBLR / MZVL21T0HBLU and are inferred for 2 TB, so check them.
- **(H)** https://www.techspot.com/news/90998-samsung-swapping-parts-their-970-evo-plus-ssds.html , https://www.sammobile.com/news/samsung-quietly-revises-970-evo-plus-ssd-with-new-gen-4-0-controller/

**Throughput per spill**

| Spill size | Write time | Read back | Round trip |
|---|---|---|---|
| 200 GB (Phoenix) | ~24 s at 3.3 GB/s + ~80 s at 1.5 GB/s ≈ **105 s** | ~60 s at ~3.3 GB/s | **~165 s** |
| 200 GB (Elpis, estimate) | ~150–250 s | ~60 s | ~210–310 s |

- The SLC cache only refills while the drive is idle, so back-to-back spills get the slow rate.

**Endurance**
- 1,200 TBW ÷ 0.2–0.5 TB written per run gives **2,400–6,000 runs**, so endurance is **not a concern** for development use.
- Keep the drive below ~80 % full: a full TLC drive shrinks the dynamic SLC cache and slows sustained writes.
- Thermal throttling is possible under minutes of sustained writes if the chassis has no airflow over the M.2.

**aac6 caveat**
- On the node recorded in `aac6-environment.md`, `/` is `/dev/md0p1`: **XFS, 800 G partition, ~293 G free**. So the *capacity* available for spill may be well under the drive size, and the md layout (RAID0 vs RAID1) changes bandwidth.
- Run `lsblk` and `mdadm --detail` on a compute node before relying on either figure.

---

## 7. Implications

### (a) Can an APU pipeline spill to disk without the page cache taking the freed memory?

Yes, provided the spill code follows these rules:
1. Use **O_DIRECT** for every spill write and read-back, or RWF_DONTCACHE on kernel ≥ 6.14. Never use buffered I/O, and never use `/tmp`, which is HBM.
2. Do the O_DIRECT from a **pinned hipHostMalloc bounce ring**, or from buffers that are themselves hipHostMalloc memory. **A hipMalloc pointer cannot be the O_DIRECT buffer (EFAULT).**
3. If anything is written buffered (e.g. final digit output), follow it with `fdatasync` and then `posix_fadvise(DONTNEED)`, as the ecalc scripts already do for the reference file.
4. Release device memory with `hipFree` of chunks or VMM unmap/release, not madvise. madvise is right only for malloc'd (host) memory.
5. Watchdogs must count TTM-pool pages as reclaimable, or read `/sys/kernel/debug/ttm/page_pool` or the amdgpu memory counters, not just MemFree.

### (b) Realistic spill bandwidth

- **aac6:** ~1.5–1.75 GB/s sustained write after 78 GB (possibly ~0.8 GB/s on an Elpis unit), ~3.3 GB/s read. That is about **165 s per 200 GB round trip**, not hideable behind a 112 s run at 4×10¹⁰, and it gets worse as d grows. Treat aac6 as a place to test correctness, not to judge performance.
- **Target:** unknown model. Likely one Gen4 M.2 at **2–4 GB/s write, 5–7 GB/s read**, about 50–100 s per 200 GB round trip.
- **Budget rule:** spill time ≈ Σ(bytes)/BW_w + Σ(bytes)/BW_r. Only spills whose I/O overlaps a compute phase of similar length are cheap.
- apumult's own figures fit this picture: 3.4× digits but 3.2× slower at 4×10¹⁰ (234.8 s against 73.1 s).

### (c) Does HIP VMM give a cheap "release dead pages" for device buffers?

- **Functionally, yes.** Unmap and release chunks of a reserved VA range, then create and map them back later. The pages come back to the system and are clean.
- **Cost:** kernel zeroing at ~35–40 ms/GiB when pages come back, the same as hipMalloc. That is small next to any disk traffic (seconds per 100 GiB, against tens of seconds of I/O).
- **Cost in maturity:** Beta API, open gfx94x issues, an fd-per-handle bug in 7.14, and it is still counted against the amdttm pages_limit.
- **Recommendation:** implement "release" as **chunked device allocations** (hipMalloc/hipFree of ≥ 256 MiB chunks behind the existing dbig block-pool). Adopt VMM only if a flat contiguous VA is truly needed by the NTT kernels.
- Either way, **release is cheap. The expensive part of the apumult techniques is the SSD round trip, not the page release.**

---

## 8. Tests to run on aac6 (each ~1 minute)

1. **VMM support.** Print `hipDeviceAttributeVirtualMemoryManagementSupported` and `hipMemGetAllocationGranularity` (MINIMUM and RECOMMENDED). Then time a loop of 64 × 1 GiB `hipMemCreate`+`hipMemMap`+`hipMemSetAccess`, followed by unmap and release, and check `free -g` in between.
2. **O_DIRECT targets.** Open a file with `O_DIRECT` and `pread` 64 MiB into (i) a hipMalloc pointer, where EFAULT is expected, (ii) a hipHostMalloc pointer, where OK is expected, and (iii) posix_memalign memory, where OK is expected.
3. **madvise.** `madvise(MADV_DONTNEED)` on a hipMalloc pointer should return EINVAL. On hipHostMalloc memory, check whether RSS or MemFree actually drops (it probably does not).
4. **Disk.**
   `fio --name=w --filename=/path/on/ssd/f --rw=write --bs=4M --iodepth=16 --ioengine=io_uring --direct=1 --size=200G`, then the same with `--rw=read`. Record the bandwidth curve (`--write_bw_log`) to see where the SLC cache runs out. Run `nvme id-ctrl /dev/nvme0` to identify the controller revision. Repeat on the target's `/ssd0`.
5. **Page-cache effect.** Write 100 GB buffered, then hipMalloc 100 GiB and time an NTT pass. Repeat after `fadvise(DONTNEED)`. This checks the fragment-fallback prediction in §1.3.
