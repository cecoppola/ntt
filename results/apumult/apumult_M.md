# apumult spill study: disk and APU memory measurements (agent M)

Measured 2026-09-23, 22:58 to 23:22 CDT, on aac6 node **ppac-pl1-s24-16** (job 21121, `-p PPAC_MI300A_SPX -N1 --gpus=4`).
Nothing in the repo was touched. Sources are in `scratchpad/diskbench/` (`diskio.c`, `hipmem.hip`). They were built on the node:
`gcc -O2 -pthread diskio.c -o diskio`; `module load rocm; hipcc -O2 hipmem.hip -o hipmem -lpthread`.
Every file written to the node disk was removed (`rm`), and `~/diskbench` on aac6 was deleted afterwards. Disk free stayed at 1.3 TB throughout.

**Node facts (measured):** the NVMe is a **Samsung SSD 980 PRO 2TB**, not a 970 EVO Plus. It runs at PCIe 16 GT/s x4 with the mq-deadline scheduler, and ext4 is mounted on `/` (so `/tmp` is on it). 505 GB were used and 1.3 TB free.
MemTotal is 502 GiB and MemAvailable about 493 GiB at idle. `vm.dirty_ratio` = 20, `dirty_background_ratio` = 10. liburing and fio are not installed, so queue depth comes from threads.
Note: aac6 is only a lower bound for disk bandwidth. The target system has its own `/ssd0`. The APU memory findings (sections 2 to 4) carry over, because the node architecture is the same.

## 1. Sequential file I/O on /tmp (1 GiB blocks)

| test (measured) | size | threads | GB/s | memory effect |
|---|---|---|---|---|
| O_DIRECT write + fsync | 32 GiB | 1 | **2.07** | Cached flat at 0.8 GiB |
| O_DIRECT write + fsync | 32 GiB | 4 | 1.95 | none |
| **O_DIRECT write + fsync, sustained** | **200 GiB** | 1 | **2.00** (flat 1.6 to 2.15 in every 2 s window, 0 to 200 GiB; no SLC cliff seen within 200 GiB) | none |
| O_DIRECT read | 32 GiB | 1 | **5.92** | none |
| O_DIRECT read | 32 GiB | 4 | 5.37 | none |
| buffered write + fsync | 64 GiB | 1 | **0.65** (0.70 before fsync). Dirty reaches about 15 GiB, then the writer is throttled to about 0.55 GB/s writeback | Cached +64 GiB (0.8 to 64.8) |
| buffered write + fsync | 24 GiB | 4 | 1.79 into the cache, **0.92 including fsync** | Cached +24 GiB, Dirty 24 GiB |
| buffered write, flush + DONTNEED every 2 GiB (`-k 2`) | 24 GiB | 1 | 0.50 | Cached peak 3.3 GiB |
| buffered read (cold) | 32 GiB | 1 | 2.82 | Cached +32 GiB |
| buffered read (cold) | 200 GiB | 1 | 2.76 | Cached +200 GiB |

Summary: the sustained spill rate on aac6 is **2.0 GB/s write and 5.9 GB/s read, with O_DIRECT**. Buffered writes are 2 to 4 times slower (0.5 to 0.9 GB/s), because ext4 writeback is the bottleneck.
More threads do not help O_DIRECT here: with 1 GiB requests, one thread already saturates the drive.
Commands: `./diskio -w -d -f /tmp/dbm.bin -s 200 -y`, `./diskio -r -d -f /tmp/dbm.bin -s 32 [-t 4]`, `./diskio -w -f /tmp/dbm.bin -s 64 -y`, `./diskio -w -k 2 -y -a -f ... -s 24`, `./diskio -r -f ... -s 200`.

## 2. Does buffered I/O eat HBM on the APU?

| moment (measured) | MemFree | MemAvailable | Cached | hipMemGetInfo free (each APU) |
|---|---|---|---|---|
| idle | 494.4 | 493.2 | 0.8 | 128.0 / 128.0 |
| after 64 GiB buffered write + fsync | **428.6** | 492.4 | **64.8** | **128.0** (blind to it) |
| holding 4 x 100 GiB hipMalloc (+memset) on top of that | 36.1 | 89.6 | 54.8 (10 GiB evicted) | 27.8 |
| after hipFree | 437.7 | 491.2 | 54.8 | 127.8 |
| after `posix_fadvise(DONTNEED)` on the file | **494.7** | 493.6 | **0.8** | 128.0 |
| after 200 GiB buffered read (cache full) | ~293 | 492.7 | **200.1** | 128.0 |
| holding **4 x 112 = 448 GiB** hipMalloc (+memset) | 10.1 | 41.7 | **33.8** (166 GiB evicted) | 15.8 |

Findings (measured):
- **Yes, the page cache is HBM.** A buffered write or read of N GiB takes N GiB out of MemFree and does not give it back on its own (Cached stays after fsync). MemAvailable counts that cache as reclaimable.
- **`fsync` + `posix_fadvise(POSIX_FADV_DONTNEED)` gives all of it back at once** (Cached 64.8 to 0.8, and 200 to 0.3). DONTNEED only drops clean pages, so fsync (or `sync_file_range` WAIT) must come first. The `-k` mode, which flushes and drops every 2 GiB, keeps the cache under 3.3 GiB but writes at only 0.50 GB/s.
- **O_DIRECT avoids the cache entirely**: Cached and Dirty stayed flat at 0.3 to 0.8 GiB for 200 GiB written and read.
- **A clean page cache does not block a large hipMalloc.** 448 GiB across 4 APUs succeeded with 200 GiB of clean cache present, and the kernel evicted 166 GiB of cache to make room. The cost is time: 448 GiB of hipMalloc + memset took **76.9 s with the cache full, against 36.5 s with it empty**.
  Dirty pages would first have to be written back at about 0.5 GB/s. That is the real danger of buffered spills: up to 20 % of RAM (dirty_ratio) can be dirty.
- **Trap: `hipMemGetInfo` is useless as a free-memory gauge on the APU.** It reports 128 GiB minus this process's own hipMalloc on that APU. It ignores page cache, host malloc, and even memory the process holds on other APUs (while holding 224 GiB it said 71.8 GiB free per APU = 287 GiB, but MemFree was 68.6 GiB). Use `/proc/meminfo` MemFree / MemAvailable instead.
- **Trap: overcommitting hipMalloc does not return an error; the process gets OOM-killed.** Holding 200 GiB of touched host memory and then asking for 4 x 112 GiB of hipMalloc + memset produced `srun: task 0: Killed`, not hipErrorOutOfMemory.

## 3. Spilling device (hipMalloc) memory to NVMe

`./hipmem spill G mode ndev`: hipMalloc G GiB on each APU, filled by a kernel, written in 1 GiB chunks to `/tmp/spill_d.bin` (O_DIRECT) + fsync, then the buffer is zeroed, read back, and checked by a kernel. The file is unlinked afterwards.

| mode (measured) | APUs x size | write+fsync GB/s | read GB/s | verified |
|---|---|---|---|---|
| **direct**: O_DIRECT `pwrite()` straight from the hipMalloc pointer | 1 x 16 GiB | **fails: `pwrite: Bad address` (EFAULT)** | not run | n/a |
| **bounce**: hipHostMalloc 2 x 1 GiB, hipMemcpyAsync D2H double-buffered, then O_DIRECT write (reverse: O_DIRECT read, then H2D) | 1 x 16 GiB | **2.24** | **6.22** | 0 mismatches |
| bounce, 4 threads (one per APU, one file each) | 4 x 16 GiB | **2.05 aggregate** (0.52 each) | **5.03 aggregate** (1.3 each) | 0 mismatches |
| cpucopy: CPU `memcpy` from the hipMalloc pointer into a posix_memalign buffer, then O_DIRECT | 1 x 16 GiB | 1.26 | 3.25 | 0 mismatches |
| managed: `hipMallocManaged` buffer, O_DIRECT `pwrite()/pread()` directly on it | 1 x 8 GiB | **2.57** | **6.24** | 0 mismatches |

Findings (measured):
- **O_DIRECT from a hipMalloc'd pointer does not work.** The CPU can load and store it (cpucopy works), but the kernel cannot pin those pages for DMA (get_user_pages), so the call fails with EFAULT. A bounce buffer is needed.
- **The bounce path runs at full disk speed** (2.2 GB/s write, 6.2 GB/s read, one APU). The D2H/H2D copy is hidden behind the disk by double-buffering. Four APUs in parallel share the one NVMe and get the same aggregate (2.05 / 5.0 GB/s).
- The CPU memcpy path costs about 2x, because CPU reads of device-coarse-grained memory are slow. Do not use it.
- `hipMallocManaged` memory *can* be the direct target or source of O_DIRECT (no bounce, 2.6 / 6.2 GB/s). It is an option only if the buffer can be allocated managed; the pipeline's dbig pools use hipMalloc.
- Spill cost per GB at aac6 rates: about 0.49 s/GB to write and 0.17 s/GB to read back, so a 100 GB round trip takes about 66 s on this disk.

## 4. madvise(MADV_DONTNEED) on host memory, and hipMalloc

| step (measured, `./hipmem madv 200`, `./hipmem madvalloc 200 110 1`, `./hipmem madvalloc 200 56`) | MemFree GiB | hipMemGetInfo free | hipMalloc result |
|---|---|---|---|
| idle | 494.0 | 128.0 each | not run |
| 200 GiB mmap'd (or malloc'd) and touched | 293.9 | **128.0 (unchanged)** | not run |
| after `madvise(MADV_DONTNEED)` (5.5 s for 200 GiB) | **493.9** | 128.0 | not run |
| then hipMalloc + memset 4 x 112 = 448 GiB in the same process | 43.1 | 15.8 | **OK, 36.5 s** |
| (other run) 200 GiB held, then 4 x 56 = 224 GiB hipMalloc | 68.6 | 71.8 | OK but **51.6 s** |
| same after madvise | 268.7 | 71.8 | OK, **17.5 s** |
| 200 GiB held, then 4 x 112 GiB hipMalloc (does not fit) | not shown | not shown | **process OOM-killed** |

Findings: **madvise(DONTNEED) returns host memory to the pool that hipMalloc draws from, immediately and in full.** MemFree goes back by 200 GiB, and 448 GiB of hipMalloc then succeeds. hipMemGetInfo does not show this, because it never saw the host allocation in the first place (see the trap in section 2).
hipMalloc into a node under memory pressure is about 3x slower (51.6 s against 17.5 s for 224 GiB), because the kernel has to reclaim and compact first.

## Bottom line for the proposal
1. The spill rate on aac6 is **2.0 GB/s write and 5.9 GB/s read, sustained, with O_DIRECT.** Buffered writes run at only 0.5 to 0.9 GB/s. Use O_DIRECT, or at least fsync + fadvise DONTNEED.
2. **Buffered I/O does eat HBM** (the page cache lives in the same 512 GB). A clean cache is reclaimable, but reclaiming it doubles hipMalloc time. A dirty cache also costs writeback time. O_DIRECT or fsync+DONTNEED removes the problem.
3. **O_DIRECT straight from hipMalloc memory fails (EFAULT).** Use a double-buffered hipHostMalloc bounce, which reaches full disk speed, or hipMallocManaged buffers, which work directly.
4. **madvise(DONTNEED) on host buffers really returns HBM to hipMalloc.** Measure with /proc/meminfo, never hipMemGetInfo. Overcommitting hipMalloc means an OOM kill, not an error code.
