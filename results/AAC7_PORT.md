# AAC7_PORT — the aac7 port (Phase 16)

Scope: the aac7 port (Phase 16): bring-up at 2 to 10 nodes, the A/Bs, the scaled headline, the fastest software stack. Each section below condenses the former `results/<ID>.md` (old IDs resolve through `results/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; the unabridged text: `archive:results_merged_2026-10-19/`. Times Eastern; every number is **measured** unless marked modelled (mod) or assumed (a).

| item | title |
|---|---|
| §A16 | ecalc on aac7 (Cray OpenSHMEMX 11.8.0, ROCm 7.0.3, Slingshot-11): port, single node, 2 nodes |
| §B16 | bring-up at 4 / 8 / 10 nodes (10^10 digits per node) |
| §D16 | A/Bs on aac7, RCCL vs push kernel, the scaled headline (E) |
| §V16 | fastest stack on aac7 (ROCm 7.2.4 default), survey, brief A/Bs |
| §ROCM | ROCm 7.14.0 against 7.2.4 at 1 node |

## Topic summary

- **What it is.** ecalc runs on aac7 (MI300A nodes, 4 APUs per node, 4 Cassini NICs, Slingshot-11, Cray OpenSHMEMX 11.8.0 over cxi) through `make SHMEM_CRAY=1 GMP_HOME=<gmp>`; the launch wrappers are `ecalc/mnrun.sh` (impl `cray`) and `ecalc/aac7env.sh`; switch and variable rows are in `ecalc/README.md` (authoritative for defaults).
- **Port facts.** Cray SHMEM is OpenSHMEM 1.5 with THREAD_MULTIPLE, contexts and put-with-signal: the generic SHMEM branch of comm_shmem.c applies unchanged. Its symmetric heap is **host memory** (no HIP in libsma), so `COMM_SHMEM_DEVHEAP=1` falls back to the HIP-registered host pool (same HBM on the APU). One NIC per PE by default; `SHMEM_OFI_NUM_NICS=4` vs 1 made no measurable difference (2 and 4 nodes). Allocations are `--exclusive -c 192`.
- **Digits.** Identical to the references at every size tried: 10^9 and 10^11 on 1 node (sha1 of the 10^11 file equals aac6's), 10^9 .. 10^11 on 2 / 4 / 8 / 10 nodes, and 8.4 x 10^11 on 10 nodes (in-run VERIFY + packed RECHECK + leading 10^11 prefix).
- **Default stack now: ROCm 7.2.4** (`AAC7_ROCM`, §V16); newest version that builds (7.12+ fail the HIP device link with a -fPIC error); 7.14.0 builds only in a later test and shows no detectable speed difference (§ROCM). Cray OpenSHMEMX 11.8.0, libfabric 2.3.1, cray-mpich 9.1.0 are already the newest on aac7. Every `rocm/*` module carries `conflict rocm`: unload `rocm/7.0.3` by exact name before loading another (silent failure otherwise).
- **Fabric rate is the wall.** Post-to-completion all-to-all per APU thread: 3.7 GB/s at 2 nodes, 2.8 at 4, 1.3-2.0 at 8, 1.3-1.6 at 10 (10^10 per node), 1.31 at 10 nodes x 8.4 x 10^10; the model (`estimate.py`) with the measured rate matches the 10-node record within -6 % / -5 %; with an assumed 25 GB/s it is 6-9x too optimistic. The compute side of the model holds.
- **Walls and noise.** Same configuration spans +-25 % job to job at 4 nodes; 8 nodes showed within-allocation drift (ranks 0-3, 2-10x slower), 10 nodes did not; a later 10-node run saw level 2 5-7x slower than 80 min earlier (shared fabric / NFS, cause unmeasured). NFS (about 0.1 GB/s per writer) puts the part-file write inside `total` on aac7 unless parts go to RAM `/tmp` or `ECALC_CHECKPOINT=0`.
- **Open / dropped.** Open: intermittent `shmem_init_thread` segfault of task 1 at 2 nodes x 10^10 (3 in 23 launches, §A16; 0 in 12 / 12 launches at 4-10 nodes); wrap launches in `timeout` and retry. Dropped: RCCL all-to-all (0.24x the push kernel), `COMM_SHMEM_SERIAL=1` (unstable), `RNS_DIST_CACHE_PARTIAL=0` (does not fit memory at 4 x 10^10 per node), FI_CXI / NIC-policy tuning (no gain >5 %), 4 PEs per node at 2 nodes (memory guard without `ECALC_PLANE_CAP`).
- **Headline limit.** The target share 9.16 x 10^10 digits per node does not fit aac7 now: each rebooted node copies a 77 GB model cache into RAM (MemAvailable about 435-445 GB against a 454 GB layout); 12 nodes are never free (10 usable). E ran at 8.4 x 10^10 per node (§D16).
- **Superseded numbers.** The 12-node modelled column and §5 "memory delta 0" for the full cache are superseded by §D16 s2; A16's 1-node 10^11 (206.7 s at ROCm 7.0.3) is superseded by the 7.2.4 figure 168.5 s quoted from C16 in §V16.

## §A16 — ecalc on aac7 (Cray OpenSHMEMX 11.8.0, ROCm 7.0.3, Slingshot-11)

Branch `p16-A` from main a6667bd (2026-10-03).

**1. What the SHMEM is (login-node survey).** Cray OpenSHMEMX 11.8.0: OpenSHMEM 1.5, `shmem_init_thread`/`SHMEM_THREAD_MULTIPLE`, `shmem_ctx_create`, typed put-with-signal family, so comm_shmem.c's generic branch (`-DCOMM_SHMEM`) applies. **Host-only symmetric heap** (libsma needs no HIP/HSA; `man shmem_mem_manage`: always huge-page backed, full heap allocated at init). `COMM_SHMEM_DEVHEAP=1` therefore cannot mean a device heap; the transport detects host memory via `hipPointerGetAttributes` and uses the registered host pool (note in the log). NICs: one per PE by default (`SHMEM_OFI_NIC_POLICY` BLOCK default, `SHMEM_OFI_NUM_NICS` unset = one) = one 200 Gb/s NIC per node with one process per node. Progress is manual with a progress thread per PE: leave hardware threads vacant, use `--exclusive`. GMP: only a runtime lib, no headers: GMP 6.3.0 built into a user prefix (2.5 min).

**2. Design (all behind build options or script variables, default build unchanged).** `ecalc/Makefile`: `SHMEM_CRAY=1` (flags + rpaths so the binary runs without modules), `GMP_HOME=<prefix>` (`$(GMP_LIBS)`). `comm_shmem.c`: the DEVHEAP host-memory fallback. `mnrun.sh`: impl `cray` (plain `srun`, `SHMEM_SYMMETRIC_SIZE` = `XT_SYMMETRIC_HEAP_SIZE` = pool + 512 MiB), `MNRUN_MODULES`, `MNRUN_CPUS_PER_TASK=auto`, `ECALC_LOG_CLOCKS`. `mnaccept.sh`: same variables plus `MNACCEPT_TMP` (aac7 `/tmp` is RAM). `aac7env.sh` (new): modules, defaults, `aac7_alloc`/`aac7_wait`, `--log` block (CPU set, clocks per APU, APU -> NUMA -> `cxi<j>`). README rows added.

**3. Tests.**
- *Build:* rc 0 (33 s with -j32). Two minimal fixes (183b34e): a `struct dbig_s;` forward declaration (clang 20 warning at ROCm 7.0.3); `CC = gcc` when make's `CC` is the built-in default (the Cray `cc` wrapper adds `-lpmi` without `-L`, breaking tools/ and tests/t_comm links).
- *1 node (ROCm 7.0.3):* 10^9: VERIFY OK, digits identical, total 13.98 s (init 9.1); `DB_POOL_VMM=0` 10.49 s (init 5.7). An rc 6 hipMalloc OOM came from two VMM runs on one node (protocol rule), not a 7.0.3 fault. **10^11 on one node: total 206.73 s and 196.32 s** (2 runs; bs 96.8, dm 85.3, init 24.2 of which `region pools` 18.30 s; VmHWM 27.8 GB) against aac6's 166.7 +- 1.2 s at 7.2.4 (RESULTS §94): +40 s (+24 %); sha1 578f5efb... equals aac6's. Packed output 44.4 GB wrote to NFS in 382 s (0.116 GB/s, off the clock); unpack 1208 s. Idle clocks sclk 94-102, mclk 1300, fclk 2000 on every APU; APU i on NUMA i and `cxi<i>`: confirmed.
- *2 real nodes (t_comm, t_dist 24, t_mn_grid):* VERIFY OK (200 checks for the grid; 31 896 exchanges, 47.15 s post-to-completion summed). Trap fixed (a003cb8): with `COMM_TRANSPORT=shmem` only as an `env` word of the command, `mnrun.sh` took its non-SHMEM path (Cray default 2.17 GB heap held 2048 MiB, refused 8192); it now reads leading `VAR=value` words too.
- *mnaccept on 1 node (`--full`, 3562 s):* 22 passed, 1 failed (`t_bs`: only because `ref/e_1000000.sha256` and `e_10000000.sha256` were missing; passed after generating them; generate all four references 10^6..10^9 on aac7 before mnaccept). Includes full 10^11 identical (196.32 s total, RECHECK OK).
- *2-node ecalc:*

| run | digits | result | total |
|---|---|---|---|
| e9 | 10^9 | VERIFY OK, identical | 17.20 s (init 10.1) |
| e10b | 10^10 | VERIFY OK, identical to the 10^11 prefix | 62.20 s (bs 5.8 + dm 19.3 + dc 18.6; init 18.3) |
| e10c x2, e10d, e10f, e10g x2, e10h x3 | 10^10 | VERIFY OK, identical | 54-62 s; best **56.04 s** |
| e10 first launch, e10h run 4 | 10^10 | **task 1 segfault** in `comm_shmem_init`, PE 0 hangs in PMI | killed by timeout |

 The 10-s clocks sampler run (101.6 s) and a 172 s run were NFS contention (checkpoint hand-over and writer share the server; a 60-s sampler / no sampler pair gave 58.9 / 62.1 s): the static `ECALC_LOG_CLOCKS=1` block is free, keep 10-s sampling for diagnosis only. `SHMEM_OFI_NUM_NICS=4` vs 1: no difference (54-55 s).
- *First Slingshot numbers (2 nodes, 10^10):* mesh creation handshake 9.7-10.4 s of the 16-18 s init; 5508 all-to-all exchanges, 50.13 s post-to-completion summed over 4 threads, 186.6 GB received = **3.7 GB/s per APU thread** (includes D2H/H2D staging, peer wait and put; the NIC is faster); dist_mn top product 2^30 = 2^15 x 2^15 over 2 x 4 ranks: ntt 1.402 s of total 1.756 s. Checkpoint hand-over ABANDONED at 0.18 GB/s (NFS, as designed).
- *Segfault reproduction (job 12177):* 12 x t_comm 0, 12 x ecalc 10^9 0, 10 x ecalc 10^10 **1**; overall **3 in 23 launches of 10^10 at 2 nodes (13 %), 0 in 24 of t_comm / 10^9**. Always the second task, inside `shmem_init_thread` after `bs: arenas 45.1 GB allocated` (VMM arenas mapped first; 8.6 GB at 10^9); PE 0 then reports `_pmi_network_allgather failed`. Lead: libfabric/cxi or PMI startup walking the VMM-reserved ranges. A SIGSEGV preload printed nothing (libsma installs its own handler); no gdb, core goes to a root-only journal.

**4. Open issues.** The segfault (next: `sigaction`-interposing preload, `DB_POOL_VMM=0` x10, `FI_CXI_ATS=0`, `FI_MR_CACHE_MONITOR=disabled`, `comm_shmem_init` before the arenas); wrap every launch in `timeout` and retry on the PMI flood; NFS dominates I/O (checkpoint 0.18 GB/s, 10^11 packed write 116 MB/s); the 7.0.3 vs 7.2.4 single-node A/B (later: 168.5 vs 205.9 s at 10^11, C16 §2.7).

**5. Integrator summary.** Build: `source aac7env.sh; make -s -j32 SHMEM_CRAY=1 GMP_HOME=<gmp>` in `ecalc/`. 2 nodes: 10^9 identical 17.2 s; 10^10 identical **56.0 s** best of 20 good runs (53.7-62.2 s); 3.7 GB/s per APU thread.

## §B16 — ecalc bring-up on aac7 at 4 -> 8 -> 10 nodes (10^10 digits per node)

Branch `p16-B` from `p16-A` 26671c6 (2026-10-04); ROCm 7.0.3 for steps 1-2, 7.2.4 for step 3. Launch line: `mnrun.sh <n> env COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_CHECKPOINT=1 ECALC_VERBOSE=2 ECALC_LOG_CLOCKS=1 ./ecalc <digits> <out>` under `timeout`, from a batch with a done-marker. Two walls: `total` (digits computed and verified; on aac7 the NFS part-file write is inside `dc`) and exit (includes srun start, write, checkpoint). Digits: `digcmp.sh` against 10^9 / 10^11 references; n x 10^10 through a prefix compare of the unpacked parts with the 10^11 reference (`cmp -n D+2`, length D+3). Modelled no-write walls (`mn_model.py estimate --D 1e10 --bw 25 --lat 2e-6 --write-bw 0.1`): 30.6 / 33.1 / 34.1 s at 4 / 8 / 12 nodes (mod).

**Step 1 — 4 nodes (job 12178), 4 launches, 0 failures.**

| run | digits | total (s) | exit | phases | digits |
|---|---|---|---|---|---|
| e9 | 10^9 | 18.02 | 23 | bs 1.1 + dm 4.1 + dc 1.3; init 11.5 | identical |
| e10a / e10b | 4 x 10^10 | 135.99 / 127.47 | 142 / 134 | bs 23.9 + dm 44.5 + dc 41.8; init 25.7 (e10a) | identical |
| e11 | 10^11 | **FAILED rc 6 at 135 s** | | init 33.3 | |
| e10c (RNS_VERBOSE=1) | 4 x 10^10 | 130.47 | 138 | dc 42.9 (writer wait 42.4) | identical |
| e10d (`ECALC_CHECKPOINT=0`, parts in RAM `/tmp`) | 4 x 10^10 | **87.30** | 93 | bs 23.7 + dm 43.1 + dc 0.0; init 20.4 | VERIFY only |

- **e11 abort:** rank 0 at the last batch level of bs: `region pool 0 of parity 1 would grow inside bs, 10.87 -> 16.15 GB` (regions sized at init must not grow; `RNS_POOL_GROW=1` allows it). Memory was not the limit (258 of 502 GB). The init-time region sizing (Phase 12 R, `binsplit.c`) under-sizes parity 1's pool 0 by 49 % at 2.5 x 10^10 per node on 4 nodes (not hit at 10^10 per node, nor single-node 10^11). **Step 1c: with `RNS_POOL_GROW=1`: rc 0, total 314.06 s (bs 72.6 + dm 110.9 + dc 99.1; init 31.3), exit 323 s, VERIFY OK, identical** (VmHWM 43.0 GB; comm 2.48-2.66 GB/s per APU thread). The sizing fix is carried by agent R on `p16-R`.
- **The no-write wall.** At 4 nodes: **87.3 s without the write (measured) vs 127-136 s with parts on NFS**; modelled 30.6 s: 2.85x. Writer wait inside `dc` is 37-42 s (4.4 GB packed per node at 0.11-0.14 GB/s); the top-set checkpoint is ABANDONED (2.42 of 8.89 GB at 0.14 GB/s).
- **Fabric:** all-to-all 6456 exchanges, 635 GB received, 222.6 s summed = **2.85 GB/s per APU thread** (pe 0; pes 1-3: 2.78-2.88; 10^9: 1.9-2.2). Mesh handshake 16.4-17.8 s (9-10 s at 2 nodes). Memory (identical on ranks): device 185.3 GB in use, host VmHWM 29.3 GB.
- **dist_mn pieces (node 0, e10c):** level 1 product 1 074 769 838 limbs 3.41 s; level 2 2 222 222 227 limbs 6.54 s (redistribute 0.70, ntt 5.40, out 0.35); division pieces 6.1-6.3 s; `ntt` is 80-85 % of every piece, the all-to-all redistribute 7-11 %. 2^30 (2 nodes, 1.68 s) to 2^31 pieces (6.1-6.5 s) scale 3.7x for 2x limbs over 2x ranks.
- **Clocks at launch:** idle DPM levels differ per APU (fclk 1200-2000, mclk 900-1300), sclk 94-95; per-rank level times spread only 1.5 %, so no APU is binned low.

**Step 2 — 8 nodes (job 12185), 5 launches, 0 failures; `RNS_VERBOSE=1`.**

| run | digits | total (s) | exit | phases | digits |
|---|---|---|---|---|---|
| e9 | 10^9 | 28.88 | 36 | bs 1.4 + dm 13.4 + dc 0.7; init 13.4 | identical |
| e10a | 8 x 10^10 (NFS parts) | 216.36 | 224 | bs 47.2 + dm 81.8 + dc 64.4 (writer wait 74.5); init 22.9 | identical |
| e10b | 8 x 10^10 | 273.02 | 279 | bs 98.7 + dm 85.3 + dc 67.6; init 21.3 | identical |
| e10d (RAM parts) | 8 x 10^10 | 327.09 | 334 | bs 118.6 + dm 187.6 + dc 0.0; init 20.8 | VERIFY only |
| e11 (`RNS_POOL_GROW=1` by a batch-edit accident; no `[GROW]` line, so not exercised) | 10^11 | 534.13 | 541 | bs 197.7 + dm 182.2 + dc 89.0; init 65.1 | identical |

- **The 8-node walls are not reproducible; the drift sits in ranks 0-3.** Ranks 4-7 repeat the 4-node figures (level 1 6.9-7.3 s, level 2 13.0-13.3 s); ranks 0-3 (three nodes other users held until about 04:10) are 2-10x slower from e10b on; level 3 grows 22 -> 74 -> 94 -> 170 s run after run while comm falls 2.0 -> 0.8 GB/s per APU thread (e10a clean: 1.95-2.02 everywhere, level 3 22 s). Cause not identified (static clocks block cannot see run-time clocks). e9 at 8 nodes (28.9 s vs 18.0 at 4, 17.2 at 2): fixed costs grow with node count (mesh 10.1-14.3 s).
- Both walls at 8 nodes: with write 216-273 s, without 327 s (drift, not the write); modelled 33.1 s: 6.5-9.9x.

**Step 3 — 10 nodes (job 12193, ROCm 7.2.4, 18:23-18:38 EDT; 12 nodes are never free, 13 minus 3 held by another user).** Pre-check clean (load <= 4.7, MemAvail 434-494 GB, no stray GPU processes). Rebuilt from clean under `rocm/7.2.4` per the user's directive (2026-10-04 12:40).

| run | digits | total (s) | exit | phases | digits |
|---|---|---|---|---|---|
| e9 | 10^9 | 43.22 | 56 | bs 9.7 + dm 3.9 + dc 0.7; init 28.9 | identical |
| e10a (NFS parts) | 10 x 10^10 | 275.32 | 283 | bs 82.1 + dm 80.4 + dc 90.4; init 22.3 | identical |
| e10b (RAM parts) | 10 x 10^10 | **202.29** | 209 | bs 96.6 + dm 86.4 + dc 0.0; init 19.1 | VERIFY only |
| e11 (default, no GROW) | 10^11 | 290.66 | 298 | bs 83.7 + dm 81.5 + dc 100.1; init 25.3 | identical |

**No drift at 10 nodes:** level 1 3.6-8.2 s, level 2 (all 10, 4 combines) 69.7-74.7 / 85.5-89.3 / 72.4-76.2 s across runs, rank-to-rank within about 15 %; comm 1.56-1.62 (e10a), 1.31-1.44 (e10b), 1.51-1.60 GB/s per APU thread (e11). So the 8-node drift was allocation-specific. e11 ran clean without `RNS_POOL_GROW` (10^10 per node is under the abort threshold). Level-2 checkpoint prints six ABANDONED nodes at 0.05-0.12 GB/s (budgeted background write, does not gate the run).

**Launch failures:** 12 launches at 4-8 nodes (+ 10-node set), **0 failures**; the one abort is the pool sizing above.

**The table (measured vs modelled; `@2.8` = model with `--bw 2.8`).**

| nodes | ROCm | digits | per node | no-write wall (s) | with write: total / exit (s) | mod @25 (s) | ratio | mod @2.8 | ratio |
|---|---|---|---|---|---|---|---|---|---|
| 2 (A16) | 7.0.3 | 10^10 | 5 x 10^9 | - | 56.0-62.2 / 64-69 | 27.5 | 2.0-2.3 | - | - |
| 4 | 7.0.3 | 4 x 10^10 | 10^10 | **87.3** | 127.5-136.0 / 134-142 | 30.6 | **2.85** | 76.9 | 1.14 |
| 4 | 7.0.3 | 10^11 | 2.5 x 10^10 | - | 314.1 / 323 (`RNS_POOL_GROW=1`) | 53.4 | 5.9 | - | - |
| 8 | 7.0.3 | 8 x 10^10 | 10^10 | 327.1 (drifted) | 216.4-273.0 / 224-279 | 33.1 | 6.5-9.9 | 93.7 | 2.3-3.5 |
| 8 | 7.0.3 | 10^11 | 1.25 x 10^10 | - | 534.1 / 541 (drifted) | 37.1 | 14.4 | - | - |
| 10 | 7.2.4 | 10 x 10^10 | 10^10 | 202.3 | 275.3 / 283 | 33.2 | 6.1-8.3 | 97.2 | 2.1-2.8 |
| 10 | 7.2.4 | 10^11 | 10^10 | - | 290.7 / 298 | 33.2 | 8.8 | 97.2 | 3.0 |
| 12 | - | - | - | not available | | 34.1 | | | |

Reading: at 4 nodes the model is right with the measured fabric rate (87.3 vs 76.9 s). At 8 nodes use the clean e10a (about 142 s compute after removing the ~74 s writer wait; 1.5x the @2.8 model). At 10 nodes the measured comm rate (1.3-1.6) is below both 2.8 and 25, so neither bound is tight. Open items: NFS compares are slow and shared (run the timed runs first, compares last); a fresh clone has no `ref/e_1000000000.txt` or `results/e_1e11.out` (a spurious DIFFERS until they are linked: check reference paths first).

## §D16 — A/Bs on aac7, RCCL vs push kernel, and the scaled headline (E)

Branch `p16-D` from main e7dfcae (2026-10-04); script `ecalc/tests/d16_pairs.sh` (sets s1 / s2 / s8 / s8c; one fresh job per pair), `ecalc/e16_headline.sh` (steps plan / record / dev / chain / below; `timeout` around every launch, retry on the init segfault, `E16_CANCEL=1`, `E16_MEMWAIT_GB` polling MemAvailable before each launch, off by default).

**1. E plan at 12 nodes (login node, no nodes used).** Target top node 9.16 x 10^10 digits: **1 099 200 000 000** digits at g = 12, plan check OK (102 products, largest piece 16 962 962 965 limbs), 181 pieces on node 0 / 189 on the critical path, default schedule groups 2, 4, 12; `COMM_SHMEM_POOL_MB=8704`, heap >= 9216 MiB; layout 395.76 GB device + 43.6 GB host = **445.36 GB per node** (fits 480). Grid-step sweep: critical path 157 -> 189 at 1.091-1.095 x 10^12 (top level 36 -> 40); one step below = 1.09 x 10^12. Modelled walls (`mn_model.py estimate --g 12 --D 9.16e10 --write-bw 0.1`, mod): 323.3 s without the write / 691.9 s with it at `--bw 25` (a); 1140.4 / 1385.4 s at `--bw 3.7`. Disk per run: 488 GB packed parts, 1.1 TB ASCII. **Not run at 12 nodes** (never free).

**2. N2 — RCCL all-to-all vs the push kernel (1 node, job 12182).** RCCL `alltoall_perf -g 4` (rccl-tests 7.0.3): at 1 GiB, 3.44 ms, algbw 312-324 GB/s, **busbw 234-243 GB/s per node** (58.5-60.8 per APU); spread < 1 %. The push kernel (`DIST_H4=1 ./tests/t_dist 24`): per APU **236.0 GB/s at 64 MiB, 250.8 at 256 MiB, 253.6 at 1 GiB**, i.e. about **1003-1014 GB/s of link traffic per node** (aac6: 909), per link 84-90 GB/s at the xGMI limit of about 91. **Verdict: RCCL moves 0.24x of the push kernel (not > 1.2x): no transport change** (TARGET_HW_REVIEW §3 item 4; the 1.6 TB/s "bus bandwidth" in the report is a ring all-reduce figure). Also: `ecalc 10^10` x2 on 1 node, total 30.22 s, identical.

**3. 4-node pairs (4 x 10^10 on 4 nodes, parts on NFS).**
- *First s1 pair (job 12190, ROCm 7.0.3):* `MN_OUT_DKM_HI=1` 206.74 s (init 41.7) vs `=0` 144.32 s (init 18.3): one pair, A first in a fresh allocation, init alone differs 23 s: no verdict.
- *s1 repeated, one fresh job per pair (jobs 12196-12213, ROCm 7.0.3, no `COMM_INIT_EARLY`); total in s:*

| pair | A | B | B - A |
|---|---|---|---|
| dkm1 `MN_OUT_DKM_HI` 1 / 0 | 143.58 | 142.51 | -1.1 |
| dkm2 | 149.34 | 141.01 | -8.3 |
| tch1 `MN_T_CHUNK_MB` 1024 / 2048 | 180.80 | 190.32 | +9.5 |
| tch2 | 124.56 | launch failure (PMI, rc 143 after 1163 s) | - |
| tch2r | 142.89 | 143.57 | +0.7 |
| ser1 `COMM_SHMEM_SERIAL` 0 / 1 | 193.15 | 133.01 | -60.1 |
| ser2 | 128.44 | **373.46** (dm 214.5) | +245.0 |
| nic1 `SHMEM_OFI_NUM_NICS` 1 / 4 | 137.05 | 137.43 | +0.4 |
| nic2 | 136.52 | 143.52 | +7.0 |

 The `stats` pairs were not run (driver stopped: every 4-node job delays the 10-node runs). Reading: the same configuration spans **124.6-193.2 s** across jobs (+-25 %), so the DKM_HI and T_CHUNK deltas are inside the noise (**no verdict**); SERIAL=1 stalls the division once in two: **keep 0**; NICs: no effect. Digits: every run VERIFY OK on all 4 nodes, parts identical to the baseline (the "DIFFERS" lines were a different part layout for DKM_HI=0 and a job-limit-truncated hash step, not digit errors; the script keeps one baseline per layout, 12e7321).
- *Set pe (2 nodes x 10^10, 1 vs 4 PEs per node) — dropped:* 1 PE per node 88.59 s; 4 PEs per node: memory guard stop at 21.2 s (each process sizes plane pools for the default cap; needs `ECALC_PLANE_CAP=30`); the 1-vs-4 comparison is P16's.
- *Node reboots:* after several of our jobs Slurm puts nodes into `needs_reboot` (about 30 min per cycle), delaying B / E.
- *Set s2, 1.6 x 10^11 on 4 nodes (4 x 10^10 per node), parts in RAM `/tmp`, ROCm 7.2.4, `COMM_INIT_EARLY=1`:* cache_A (`RNS_DIST_CACHE_PARTIAL=1`) 342.61 / **350.54** / 669.97 (same config 1.9x slower) / 417.65 s, VERIFY OK, top part = the 10^11 file's first 2 x 10^10 bytes; **cache_B (`=0`) died on the memory guard twice** (MemAvailable 2.4-5.4 GB at 103-180 s); tch (`MN_T_CHUNK_MB`): 1024 Bus error / Node failure on aac7-n21 (down) twice, then 1024 **348.98 s** vs 2048 **329.47 s** (B - A = -19.5 s, one pair); stats single 348.85 s, identical. **Verdict: keep `RNS_DIST_CACHE_PARTIAL=1`** (a memory verdict: the full cache needs more than the ~30 GB headroom; s8c not run for the same reason).

**4. 8-node pairs (set s8, 8 x 10^10 on 8 nodes, parts on NFS, ROCm 7.2.4, 45-min jobs).** `MN_GROUPS` default (2,4,8): **218.11, 225.17, 248.96, 203.16 s**; `2,8`: 232.94 / 234.50 s (one launch failure rc 143); `MN_GROUPS=8` never ran (jobs expired during reference compares); `MN_T_CHUNK_MB=1024` 216.60 s (2048 lost). All VERIFY OK, top part = `ref_8e10.txt`. Reading: default +-10 %, `2,8` inside it: **no measurable difference at 8 nodes**. Hash files of the B runs were short (hash `srun` returned early): an artefact of the hash step. For reruns: >= 1.5 h jobs for pairs that write a new layout and check the hash file has all lines.

**5. The options table.**

| switch (A vs B) | measured delta | spread | recommendation |
|---|---|---|---|
| `RNS_DIST_CACHE_PARTIAL` 1 vs 0 | =0 died on the memory guard twice at 4 x 10^10 per node (does not fit ~440 GB nodes); =1 350.5 / 670.0 s | - | **keep 1** (mod at 576: -8 s) |
| `MN_OUT_DKM_HI` 1 vs 0 | -1.1, -8.3 s | +-25 % across jobs | already on the launch line (mod at 576: -7 s with write) |
| `MN_T_CHUNK_MB` 1024 vs 2048 | +9.5, +0.7 (s1); -19.5 (s2); s8 1024 216.6 s | +-25 % | T11: decide on measured `T_ROUND` (C) (mod -10.9 s at T_ROUND 0.03 s) |
| `COMM_SHMEM_SERIAL` 0 vs 1 | -60.1, +245.0 s | unstable | **keep 0** |
| `SHMEM_OFI_NUM_NICS` 1 vs 4 | +0.4, +7.0 s | inside noise | no change |
| `MN_GROUPS` 2,4,8 / 2,8 / 8 | default 203.2-249.0 s, 2,8 232.9 / 234.5 s | +-10 % | no difference at 8 nodes; set only at 576 (T5) |
| RCCL all-to-all vs push kernel | 0.24x | < 1 % | **drop** |

**1b. E at 10 nodes (integrator, 2026-10-04: 12 nodes never free; the 12-node figures stay as the modelled column).** Plan at 9.16 x 10^11 digits: plan check OK (98 products), critical path 139, schedule 2, 10, **node 453.95 GB** (fits 480); parts on NFS (RAM `/tmp` would add 40.7 GB per node). Modelled (mod): 282.5 s without / 658.8 s with the write at `--bw 25` (a); 951.0 / 1233.3 s at `--bw 3.7`.
- *Attempt 1 (job 12195):* build check 10^9 total 13.69 s, identical; record **failed** rc 134 after 458 s: rank 6 stopped by the memory guard at 283 s (MemAvailable 4.6 GB < 6); the node started with 442 GB (others 463-519 GB) against the 454 GB layout. Fix: `E16_MEMWAIT_GB` (d6e73d6), plus a `total` regex fix in the walls line.
- *Attempt 2 (job 12217):* idle nodes lose MemAvailable about 1-1.5 GB/min: **each node's boot copies a 77 GB model cache into the RAM root overlay**; every rebooted node comes back about 77 GB short (MemAvailable about 435-445 GB). **The target share 9.16 x 10^10 per node does not fit aac7 now** (admin-side, not ours to undo). Layout vs size: 9.16 x 10^11 453.9 GB; 8.8: 445.4; 8.6: 436.8; **8.4: 428.2 (E now)**; 8.0: 419.6. Step below = 8.20 x 10^11 (critical path 122 -> 129).
- **E record, 8.4 x 10^11 digits on 10 nodes (job 12217, 19:56-20:37):** total **2429.52 s** (bs 755.6 + dm 1338.4 + dc 272.1; init 56.8), elapsed **2456 s**; **VERIFY OK, packed RECHECK OK (1552 s), 20 of 20 parts unpacked rc 0 (2993 s), leading 10^11 + 2 bytes identical to the 10^11 reference**; ASCII RECHECK skipped (time). Fabric: pe 0 22 572 exchanges, 9791.5 GB received, 7491 s summed = **1.31 GB/s per APU thread**; tree level 1 95.1 s, level 2 618.0 s; division 1338.4 s (reciprocal 298.5, step 1 504.0, step 2 534.2; division products 1018.0); write 0.03-0.09 GB/s per node (NFS, 10 writers); budget peak 400.2 GB (VmHWM 36.9 GB). Model with `--bw 25` 266.8 s (a), 905.9 s at `--bw 3.7`, **2274.6 s at `--bw 1.31`** (elapsed 2327.2 s): measured is **9.1x the @25 model, 2.7x the @3.7, and within -6.4 % / -5.2 % of the model fed the measured 1.31 GB/s**. About 77 % of the run is the exchange.
- **Step-below 8.20 x 10^11 run: not completed.** Level 1 took 64.2 s (record 95.1) but level 2 ran **5-7x slower** than 80 min earlier on the same nodes (combines 1-3 at 554 / 1461 / 2429 s vs 104.6 / 230.0 / 401.4 s); no foreign process on the nodes; cause outside the job (shared fabric / NFS, not measured). Stopped at 23:14; the step's cost is not measured, and the record's own pace varies this much on aac7's fabric. A further attempt (job 12262, `MN_COMM_MARK=1` per-level rate) was armed and pending; results were to go into this section.
- **576-node implication (mod only):** `mn_model.py estimate --g 576 --D 9.16e10` (5.276 x 10^13 digits): 292.4 s without the write / 340.9 s with at 0.6 GB/s per node at the default `--bw 100` (a: two 400 Gb/s NICs per APU); 4808.4 / 4858.9 s at `--bw 1.31`. Node 472 GB modelled. The 16x spread is the open question: the target's per-APU all-to-all rate at 576 nodes is not measured; the E run shows the model is right given the rate.

## §V16 — fastest stack on aac7 (ROCm 7.2.4 default), survey, brief A/Bs

Branch `p16-V`.

**1. Default stack.** `aac7env.sh` / `mnrun.sh` load `AAC7_ROCM` (default `rocm/7.2.4`) instead of the bare `rocm` alias (= `rocm/7.0.3`); `AAC7_ROCM=rocm/7.0.3` reverts (target toolchain match). **Trap fixed:** every `rocm/*` and `rocm-new/*` module has `conflict rocm`; the profile loads `rocm/7.0.3`, so a later `module load rocm/7.2.4` printed an error (swallowed by `2>&1`) and left 7.0.3 active. Fix: `module unload rocm/7.0.3` by exact name before the load, in `aac7env.sh` and all four wrapper sites of `mnrun.sh`; verified `hipcc` resolves to 7.2.4 (HIP 7.2.53211). D's branch built the same fix independently as opt-in (`AAC7_ROCM` + `MNRUN_UNLOAD=rocm`); V makes 7.2.4 the default; one implementation to be picked at the RESULTS merge. e9 on 1 node: VERIFY OK, identical.

**2. Survey (login node, read-only).** `rocm` 7.0.3 (system), 7.2.3, **7.2.4** build ecalc (7.2.4 = aac6's default, newest that builds); **7.12.0, 7.13.0, 7.14.0 and rocm-new 10.0.0 do not build** (`ld.lld: relocation R_X86_64_32 ... recompile with -fPIC` in the HIP device-code link of `ntt.o`, `fatal.o`; a toolchain regression); rocm-new 7.1.1 / 7.2.0 / 7.2.1 same family as the default; afar / therock untested. `cray-openshmemx` 11.8.0, `libfabric` 2.3.1 (only one), `cray-mpich` 9.1.0 are already the newest. OpenMPI 5.0.10 has a real OSHMEM (`oshcc`, `liboshmem.so`): an alternative SHMEM, not ported. rocSHMEM (in rocm 7.13.0) is device-side API, not a drop-in. SOS not present. `FI_CXI_*`: only `FI_CXI_RX_MATCH_MODE` compiled in; `SHMEM_OFI_*`: about 150 names in libsma, only `NIC_POLICY` and `RX_MATCH_MODE` tried. 7.2.4 vs 7.0.3 already measured in C16 §2.7: 168.5 vs 205.9 s at 10^11, identical digits.

**3. Brief A/Bs, 2 nodes, 10^10 (job 12209).** Baseline total 61.80 s, 3.56 GB/s per APU thread. `FI_CXI_RX_MATCH_MODE=hybrid`: 63.86 s (+3.3 % slower), 3.68 GB/s (+3.4 %); `SHMEM_OFI_NIC_POLICY=ROUND-ROBIN`: 62.40 s (+1.0 % slower), 3.67 GB/s (+3.1 %). No knob cleared the >5 % wall / >10 % GB/s bar: `AAC7_FI_TUNE=1` (off by default, empty) documents the negative result.

**4. Recommended stack (measured).** `cray-dsmml cray-openshmemx/11.8.0 rocm/7.2.4`, `cray-mpich/9.1.0`, `libfabric/2.3.1`; nothing newer usable. Files: `ecalc/aac7env.sh`, `ecalc/mnrun.sh`, `ecalc/README.md` (rows `AAC7_ROCM`, `AAC7_FI_TUNE`). Open (future phase): OpenMPI 5 / OSHMEM and rocSHMEM as alternative SHMEM stacks.

## §ROCM — ROCm 7.14.0 against 7.2.4 at 1 node (OPPORTUNITIES M7d)

**Question.** The target runs ROCm 7.14.0: does ecalc build and run correctly with it, and is it faster or slower than 7.2.4? **Change.** None (docs only); commit 39346d03 of main built twice with `SHMEM_CRAY=1`. **Method.** aac7, 1 node, 10^10 digits, defaults; ABBA, 4 rounds, A = 7.2.4, B = 7.14.0; digits compared with the GMP reference prefix; the log's libamdhip64 path checked.
**Results (all 8 runs VERIFY OK, identical, no segfault):** total A / B per round: 35.68 / 60.65 (init 33.8 s, cold first launch), 38.49 / 34.40, 38.28 / 35.24, 36.14 / 37.25 s. B - A = +4.74 s, 95 % CI [-17.02, +26.49] (n = 4); without round 1 (post hoc) -2.01 s, CI [-8.62, +4.59] (n = 3); stage split equal.
**Verdict.** 7.14.0 builds and gives identical digits (§V16's survey reported that 7.14.0 fails the device link; this item's build succeeded: the discrepancy is not explained in the sources); no detectable speed difference (CIs contain 0). Keep 7.2.4 as the aac7 default.
**Open issues.** Cold first launch (60.65 s; an earlier first run did not finish the reciprocal within 240 s; cause not established, assumed first-use code-object build): give the first target run a long timeout. Not measured: 10^11, multi-node, the SHMEM-only path. Method trap: with another ROCm module loaded `module load rocm/7.14.0` silently fails (set `MNRUN_UNLOAD` to all ROCm modules; check the libamdhip64 line). A 7.14.0 binary needs the 7.14.0 module at run time (RPATH names only the llvm directory).
