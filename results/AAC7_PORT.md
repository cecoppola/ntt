# AAC7_PORT — the aac7 port (Phase 16)

Scope: the aac7 port (Phase 16): bring-up at 4 to 12 nodes, the A/Bs and the 12-node scaled headline, the fastest software stack. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| A16 | Phase 16 A: ecalc on aac7 (Cray OpenSHMEMX 11.8.0, ROCm 7.0.3, Slingshot-11) |
| B16 | Phase 16 B: ecalc bring-up on aac7 at 4 → 8 → 12 nodes (10¹⁰ digits per node) |
| D16 | Phase 16 D: the A/Bs on aac7, and the Phase E scripts (the 12-node scaled headline) |
| V16 | Phase 16 V — fastest stack on aac7 (ROCm 7.2.4 default), survey, brief A/Bs |
| ROCM | ROCm 7.14.0 against 7.2.4 at 1 node (OPPORTUNITIES M7d) |

## A16 — Phase 16 A: ecalc on aac7 (Cray OpenSHMEMX 11.8.0, ROCm 7.0.3, Slingshot-11)

(former file: A16.md)

Agent A, branch `p16-A` from `main` a6667bd (2026-10-03). Times Eastern (aac7's login node prints PDT = EDT − 3 h).
Every number is **measured** unless marked modelled / assumed.

#### 1. What aac7's SHMEM is (measured on the login node, 2026-10-03 23:00–23:20 EDT)

- `module load cray-dsmml cray-openshmemx` (rc 0 on top of the default PrgEnv-cray; `rocm/7.0.3` is loaded by default):
  `cc --cray-print-opts=cflags` = `-I/opt/cray/pe/sma/11.8.0/ofi/sma/include -I/opt/cray/pe/dsmml/0.3.1/dsmml/include …`,
  `libs` = `-L…/sma/lib64 -L…/dsmml/lib … -lsma -lpmi -lpmi2 -Wl,--as-needed,-ldsmml,--no-as-needed …` (plus libsci / mpich,
  not needed). `libpmi`, `libpmi2`, `libdsmml.so.0`, `libsma.so.0` are also in `/opt/cray/pe/lib64` (ld.so.conf);
  `libfabric.so.1` only under `/opt/cray/libfabric/2.3.1/lib64` (the `libfabric/2.3.1` module's `LD_LIBRARY_PATH`).
- `shmem.h`: `SHMEM_MAJOR_VERSION 1`, `SHMEM_MINOR_VERSION 5`, vendor "CRAY OpenSHMEMX 11.8.0 rc OpenSHMEM : version 1.5
  compliance"; `shmem_init_thread`, `SHMEM_THREAD_MULTIPLE` (3), `shmem_ctx_create`, the typed put-with-signal family
  (`shmem_ctx_put<SIZE>_signal_nbi` by macro, so `shmem_ctx_putmem_signal_nbi` exists after preprocessing), `SHMEM_SIGNAL_SET`.
  So comm_shmem.c's generic `#else` branch (`-DCOMM_SHMEM` only) applies as is: `putsig` order, a context per communicator.
- **The symmetric heap is host memory.** `libsma.so` NEEDs only libfabric, libatomic, libdsmml, libm, libc — no HIP / HSA;
  its strings hold no `*DEVICE*` / `*GPU*` / `*HIP*` variable (the only memory variables: `SHMEM_SYMMETRIC_SIZE`,
  `SMA_SYMMETRIC_SIZE`, `SHMEM_USE_DSMML_SSHEAP*`); `man shmem_mem_manage`: "the SHMEM symmetric heap is always backed by huge
  pages and the full `XT_SYMMETRIC_HEAP_SIZE` amount is considered allocated during SHMEM initialization".
  `shmemx.h` has `shmemx_local_buffer_begin/end` (user buffer registration) and `shmem_malloc_with_hints` (1.5 hints only).
  libfabric's own `FI_HMEM_*` / `FI_MR_ROCR_CACHE_MONITOR_ENABLED` strings are libfabric's; whether the cxi provider would DMA
  from a HIP allocation is not reachable through this library's API (no device heap, no `shmemx_malloc_with_hints` device hint).
  → `COMM_SHMEM_DEVHEAP=1` cannot mean a device heap here; the transport now falls back (§2) to the registered host pool, which on
  the APU is the same HBM (the aac6 SOS-without-DEVHEAP form). The cost is measured in §5–6.
- NICs: `man intro_shmem`: "Each OpenSHMEMX PE will be assigned to exactly one NIC" (`SHMEM_OFI_NIC_POLICY` = BLOCK |
  ROUND-ROBIN | NUMA | USER, default BLOCK; `SHMEM_OFI_NUM_NICS` "the number of NICs the job can use on a per-node basis …
  Default: not set (OpenSHMEMX uses one NIC by default)"). With one process per node that is **one 200 Gb/s NIC per node**
  unless `SHMEM_OFI_NUM_NICS=4` spreads a PE's contexts (unmeasured; Phase C's first question). The full man page is at
  `~/p16/A/intro_shmem.txt` on aac7.
- Progress: "Cray OpenSHMEMX defaults to manual progress with a separate progress thread per PE … leave one or more hardware
  threads vacant"; `SHMEM_OFI_PROGRESS=AUTO` uses libfabric's. Slurm: "`--hint=nomultithread` and not using `--exclusive` have
  both caused poor progress or hangs" — the allocations here are `--exclusive -c 192`.
- GMP: `/usr/lib64/libgmp.so.10` only, no headers → GMP 6.3.0 built into `~/gmp` (§3).

#### 2. Design (the code; every new behavior behind a build option or a script variable, off by default)

| file | change |
|---|---|
| `ecalc/Makefile` | `SHMEM_CRAY=1`: `-DCOMM_SHMEM -I<sma>/include -I<dsmml>/include`; link `-L<sma>/lib64 -L<dsmml>/lib -L/opt/cray/pe/lib64 -lsma -lpmi -lpmi2 -ldsmml` + rpaths (the binary runs without the modules). Prefixes from the modules' `CRAY_OPENSHMEMX_PREFIX` / `CRAY_DSMML_PREFIX` or `CRAY_SMA=` / `CRAY_DSMML=`. `GMP_HOME=<prefix>`: `-I` on `CFLAGS`/`HIPFLAGS`, every `-lgmp` is now `$(GMP_LIBS)` (= `-lgmp`, or `-L -lgmp -rpath` with `GMP_HOME`). Default build unchanged (`make -n` identical). |
| `ecalc/comm_shmem.c` | generic branch only: with `COMM_SHMEM_DEVHEAP=1` the pool from `shmem_malloc` is checked with `hipPointerGetAttributes`; host memory → one stderr note and the registered-host-pool path (`S.devheap = 0`). SOS / OSHMEM branches untouched. |
| `ecalc/mnrun.sh` | impl `cray` (libsma whose rpath / resolved path is under `/opt/cray/pe`, or `COMM_SHMEM_IMPL=cray`): plain `srun` (Cray's PMI), `SHMEM_SYMMETRIC_SIZE` = `XT_SYMMETRIC_HEAP_SIZE` = pool + 512 MiB. `MNRUN_MODULES` (default `rocm`; `cray-dsmml cray-openshmemx rocm` where `/opt/cray/pe/sma` exists) in every wrapper and the plan-pool call. `MNRUN_CPUS_PER_TASK` (`auto` = CPUTot / tasks per node → `srun -c`; unset = as before). `ECALC_LOG_CLOCKS=1` → `aac7env.sh --log` per node before the command. |
| `ecalc/mnaccept.sh` | the same `MNRUN_MODULES` and `MNRUN_CPUS_PER_TASK` in `R()`; `MNACCEPT_TMP` (the digits' directory; aac7's `/tmp` is RAM). |
| `ecalc/aac7env.sh` | new: `source` = modules + `MNRUN_MODULES`, `MNRUN_CPUS_PER_TASK=auto`, `MNACCEPT_TMP=$HOME/p16/mnaccept_tmp`, `GMP_HOME`, `AAC7_PART`, helpers `aac7_alloc <n> <h:mm:00>` / `aac7_wait <J>`; `--log` = the N3 block (CPU set, `rocm-smi --showclocks` per APU, APU → NUMA → `cxi<j>`). |
| `ecalc/README.md` | the build line; rows `MNRUN_MODULES`, `MNRUN_CPUS_PER_TASK`, `MNACCEPT_TMP`, `ECALC_LOG_CLOCKS`; `COMM_SHMEM_IMPL` gains `cray`. |

#### 3. Tests (aac7, p16-A; EDT)

##### Step 2 — build (login node aac7-login, 2026-10-03 23:09–23:11 EDT)

`source ~/ntt/ecalc/aac7env.sh; cd ~/ntt/ecalc; make -s -j32 SHMEM_CRAY=1 GMP_HOME=$HOME/gmp` — rc 0 (the full compile + link
32.6 s wall with -j32 the first time, one warning; 8.6 s the relink). GMP 6.3.0: `./configure --prefix=$HOME/gmp`, `make -j32`,
`make install` — 2.5 min, no `make check`. Two things ROCm 7.0.3 / the Cray PE raised, both fixed minimally (183b34e):
1. `newton.h:58: warning: declaration of 'struct dbig_s' will not be visible outside of this function [-Wvisibility]` (clang 20,
   ROCm 7.0.3; 7.2.4's clang does not warn) → `struct dbig_s;` at file scope before the prototype.
2. `tools/` (`unpack_digits`, `wbench`) and `tests/t_comm` failed to link: make's built-in `CC = cc` is the Cray compiler wrapper
   on aac7, which appends the loaded modules' `-lpmi -lpmi2` without their `-L` (`ld.lld: unable to find library -lpmi`). `CC ?= gcc`
   never applied (a built-in default counts as defined) → both Makefiles set `CC = gcc` when `$(origin CC)` is `default`
   (aac6: `cc` was gcc anyway; a `CC=` on the make line still wins).
`ldd ecalc`: libsma.so.0 from `/opt/cray/pe/sma/11.8.0/ofi/sma/lib64`, libpmi / libpmi2 / libdsmml from `/opt/cray/pe/lib64`,
libfabric.so.1 from `/opt/cray/libfabric/2.3.1/lib64` (the default `libfabric` module's `LD_LIBRARY_PATH`; no rpath of ours),
libamdhip64.so.7 from `/opt/rocm-7.0.3/lib`. `mnrun.sh … MNRUN_SHOW_IMPL=only` → `SHMEM implementation cray`.
No `/usr/bin/time` on aac7 (the batch scripts time with `$SECONDS`).

##### Step 3 — references and the single-node runs (node aac7-n11, job 12167, `--exclusive -c 192`)

- `ref/gen_e 100000000 ref`: 33 s wall (128 threads, login node); `ref/gen_e 1000000000 ref`: (below).
- `srun --jobid=12167 -N1 -c 192 --gpus-per-node=4 … ./ecalc 1000000000 ~/p16/A/e9b.out` (defaults, packed): **rc 0, VERIFY OK,
  `total 13.98 s` (init 9.1)**; with `DB_POOL_VMM=0`: 10.49 s (init 5.7), VERIFY OK. (A first e9 attempt died with rc 6 — `hipMalloc
  of 2.15 GB on APU 3 failed (out of memory)` in `db_vmm_arena_alloc` — because my own earlier batch's e9 was still on the node:
  two VMM runs at once, the rule of the protocol; not a 7.0.3 fault. The node at idle: 501 GiB, 418 available, `/tmp` 2.8 GB used.)
- `./digcmp.sh ~/p16/A/e9b.out ref/e_1000000000.txt` → **identical** (and the `DB_POOL_VMM=0` run's file identical too).
- `ref/gen_e 1000000000 ref`: 418 s wall (login node, 128 threads; aac6: 442 s with 32).
- **10¹¹ on one node** (`ECALC_VERBOSE=2 ./ecalc 100000000000 ~/p16/A/e11.out`, defaults, packed output to NFS, 2026-10-03 23:20 EDT):
  `total 206.73 s (bs 96.8 + 10dP 0.2 + dm 85.3 + T1 0.0 + dc 0.0 + T2 0.0 = 182.2; init 24.2; other 0.3); VmHWM 27.8 GB`
  — `bs 96.76 s (seeds 18.2 school 0.0 batch 39.7 mdev 38.6; pool 99.1 GB)`, `dm 85.29 s (recip 15.1 s)`; init: **`region pools 18.30 s`**
  (`rns_init 24.16 s`) — the mapping rate at ROCm 7.0.3 (aac6 at 7.2.4: RESULTS §94's init). Against aac6's 166.7 ± 1.2 s (RESULTS §94)
  that is **+40 s (+24 %)** at the first try on 7.0.3 — one run, the output on NFS (`mn_out: O_DIRECT, packed form, 8 write threads,
  file system NFS`), no page-cache eviction needed (no reference read). Where the 40 s sit (bs / dm / init) is in the log
  `~/p16/A/e11.log`; a 7.2.4-module A/B is Phase B's one-off. The packed file (44.44 GB) was written to NFS in 382 s (0.116 GB/s,
  off the clock); `tools/unpack_digits -q -o results/e_1e11.out` took 1208 s (NFS read + write); **`sha1sum results/e_1e11.out` =
  578f5efb0ff2b9af6b681a375c9ff39197f55cb7 — equal to aac6's `~/V214/e_1e11.sha1`** (`results/e_1e11.sha1` on aac7).
  mnaccept's full step (the second 10¹¹ run, below): `total 196.32 s (bs 88.8 + dm 88.7; init 18.4)` — two runs: 206.7 / 196.3 s.
- Idle clocks (`aac7env.sh --log`): sclk 94–102 MHz, mclk 1300, fclk 2000 on every APU; APU i is on NUMA node i and so is `cxi<i>`
  (PCI domains 0000–0003, one Cassini per socket) — the map TARGET_HW_REVIEW §3 item 1 assumed, confirmed.

##### Step 4 — the transport on 2 real nodes (aac7-n20 + aac7-n21, job 12168; `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1`, through `mnrun.sh 2`, pool 8192 MiB, heap 8704M)

| test | result | wall |
|---|---|---|
| `./tests/t_comm` | `PE 0 of 2 over SHMEM: VERIFY OK (0 bad)`, PE 1 the same; `thread level 3 (concurrent, a context per communicator)`, `order putsig (inline)` | 11 s |
| `env DIST_LAYERED=1 ./tests/t_dist 24` | `VERIFY OK (12 checks)` on both nodes; the fallback note `COMM_SHMEM_DEVHEAP=1 but the library's shmem_malloc returned host memory (known to HIP, not device): the pool is HIP-registered host memory instead`, then `pool 8192 MiB, HIP-registered` | 15 s |
| `./tests/t_mn_grid 0.5 27` | `VERIFY OK (200 checks)` on both nodes; 31896 all-to-all exchanges, 47.15 s post-to-completion summed over the APU threads | 55 s |

A trap found on the way (fixed in a003cb8): with `COMM_TRANSPORT=shmem` only as an `env` word of the command (the launch line's form,
`mnrun.sh 2 env COMM_TRANSPORT=shmem … ./ecalc`), `mnrun.sh` took its non-SHMEM path — no heap variable, so Cray's default heap
(a 2.17 GB `/dev/zero` shared mapping at `0x30000000000`, measured in `/proc/<pid>/maps`) held a 2048 MiB pool and refused 4096+
(`shmem_malloc of 8192 MiB failed`). `mnrun.sh` now reads `COMM_TRANSPORT` from the command's leading `VAR=value` words too.
So Cray OpenSHMEMX 11.8.0 over cxi runs the transport's full call list (contexts, `putmem_signal_nbi`, `wait_until`, strided PE sets)
with the four APU threads concurrent; `hipHostRegister` of its hugepage heap succeeds (the symmetric slabs are device-visible: `t_dist`'s
`symmetric buffers 256 MiB`).

##### Step 5a — the one-node regression (`./mnaccept.sh 12167 --full --only unit,e9,mn,recheck,corr,full`, with `source aac7env.sh`: `MNRUN_CPUS_PER_TASK=auto`, `MNACCEPT_TMP=$HOME/p16/mnaccept_tmp`; 2026-10-04 00:08–01:08 EDT, 3562 s)

`results/mnaccept/12167/summary.txt`: **22 passed, 1 failed** — `unit t_ntt 24` (3215 checks), `t_mul 20`, `t_dbig 0`, `t_newton 20`,
`t_verify`, `t_out`, `t_patch`, `t_mn_grid` (2 procs), e9 both bases (identical; 13.69 / 23.52 s), mn e8 sizes 2, 3, 4 and e9 sizes 2, 4
(identical, all nodes VERIFY OK), recheck e9 / e8 (RECHECK OK, the corrupted copies RECHECK FAILED), the three corr steps (identical,
8 bytes patched), **full 1e11: identical, `total 196.32 s`, 558 s elapsed with the NFS write; its recheck RECHECK OK (492 s, the
file read 485.8 s from NFS)**. The one FAIL, `unit t_bs: VERIFY FAILED (2 of 10 checks)`, was `ref/e_1000000.sha256` and
`ref/e_10000000.sha256` missing (t_bs compares its 10⁶ and 10⁷ digits to them; my `gen_e` batch made only 10⁸ and 10⁹): after
`ref/gen_e 1000000 ref; ref/gen_e 10000000 ref`, `tests/t_bs` on 12167 → `VERIFY OK (10 checks)` (`~/p16/A/t_bs2.log`; the hashes
c6e9fe4f… and 0b68cf1d… equal the references). So the regression is clean at ROCm 7.0.3 / Cray SHMEM. On aac7 generate all four
references (10⁶, 10⁷, 10⁸, 10⁹) before mnaccept.

##### Step 5b — the 2-real-node ecalc (aac7-n20 + aac7-n21, job 12168; `SLURM_JOB_ID=$J ./mnrun.sh 2 env COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_CHECKPOINT=1 ECALC_VERBOSE=2 ./ecalc <d> ~/p16/A/mn2/<tag>.out`; the pool from `mnrun.sh`'s plan: 1280 MiB at 10⁹, 8704 MiB at 10¹⁰ → heap 9216M)

| run | digits | result | `total` | wall |
|---|---|---|---|---|
| e9 | 10⁹ | `mn: all 2 nodes: VERIFY OK`; `digcmp.sh` vs `ref/e_1000000000.txt`: **identical** | 17.20 s (bs 1.0 + dm 3.9 + dc 2.2; init 10.1) | 21 s |
| e10 (1st) | 10¹⁰ | **FAILED**: task 1 `Segmentation fault (core dumped)` 11 s in, inside `comm_shmem_init` (after its `comm_shmem pool: … holds the modelled need 8704 MiB` line, before the `comm_shmem: shmem, 2 PEs …` line: `shmem_init_thread` / `shmem_malloc` / the registration); PE 0 then hung in `_pmi_allgather: _pmi_network_allgather failed` flooding `inet_recv: unexpected socket EOF` — killed by `scancel` of the step after 800 s | — | 800 s (hung) |
| e10b | 10¹⁰ | VERIFY OK on both; vs the first 10¹⁰ + 2 bytes of `results/e_1e11.out` + newline: **identical** | 62.20 s (bs 5.8 + dm 19.3 + dc 18.6; init 18.3) | 69 s |
| e10c ×2 | 10¹⁰ | VERIFY OK, identical | 58.18 s, 56.81 s | 65 s, 64 s |
| e10d (`RNS_VERBOSE=1 DIST_STATS=1`) | 10¹⁰ | VERIFY OK | 61.41 s (bs 5.7 + dm 21.8 + dc 17.8; init 16.1) | 67 s |
| e10e (`ECALC_LOG_CLOCKS=10`, the sampler live) | 10¹⁰ | VERIFY OK | **101.58 s** (bs 25.7 + dm 27.9 + dc 29.5; init 18.4) — see step 6 | 109 s |

| e10f (`ECALC_LOG_CLOCKS=60` / 0) | 10¹⁰ | VERIFY OK | 58.87 s / 62.06 s | 66 / 67 s |
| e10g ×2 (2964256, the bytes counter) | 10¹⁰ | VERIFY OK, identical | 171.95 s (NFS contention, see step 6) / **56.04 s** | 178 / 61 s |
| e10h `SHMEM_OFI_NUM_NICS=4, 1, 4, 1` (no checkpoint) | 10¹⁰ | runs 1–3 VERIFY OK, 54–55 s total — **no difference between 1 and 4 NICs** at 2 nodes (one PE uses one NIC either way, or the NIC is not the limit here); run 4 (`NUM_NICS=1`): **task 1 segfault again** in `comm_shmem_init`, PE 0 hung, the `timeout 600` killed the step (the `DIFFERS` of that script is the aborted run's partial file, not a digit error) | 54–55 s | 59–60 s |

The segfault is **2 of 13** 10¹⁰ launches (both the second task, both at the 9216M heap, both before `comm_shmem: shmem, 2 PEs …`);
it is the **open issue** (§4; a reproduction loop with a backtrace preload is in §3 step 7): no core is readable (`core_pattern` →
systemd-coredump, the journal is root-only; no `gdb` on the nodes), so a backtrace needs a `-g` run under a user-readable
`core_pattern` or `ulimit -c` into `$HOME` — Phase B's first item if it recurs. The PE-0 hang after a peer's death is Cray's PMI, not
ours: launch with a `timeout`, as the batches here do.

**The first Slingshot numbers** (e10b/e10d, `ECALC_VERBOSE=2`, 2 PEs = 2 × 4 APU threads; measured):
- `mn: node 0 of 2, four meshes of 2 PEs over SHMEM: created in 9.66 s` (node 1: 10.40 s) — the communicator creation handshake is
  10 s of the 16–18 s init; the self-tests after it 0.25 + 0.16 s.
- `comm_shmem: pe 0: all-to-all 5508 exchanges, 55.08 s post to completion (summed over the APU threads)` (pe 1: 54.33 s; e10d with
  `RNS_VERBOSE`: 78.33 / 76.29 s); staging peaks 4240 MiB of the 8704 MiB pool (`alltoallv send 529.8 + recv 529.8 MiB` per
  communicator at the peak: the division's exchanges).
- `dist_mn node 0: 2^30 = 2^15 x 2^15 over 2 x 4 ranks … redistribute 0.209 ntt 1.402 crt 0.013 out 0.069 spills+carry 0.036 total 1.756 s`
  (the top-level 268088063 × 287467497-limb product; the same within 0.01 s on node 1 and on the second such product).
- The bytes were not in the Phase 14 counters; 2964256 adds them to the summary line. **e10g run 2** (56.04 s total, identical
  digits): `comm_shmem: pe 0: all-to-all 5508 exchanges, 50.13 s post to completion (summed over the APU threads), 186.62 GB
  received = 3.72 GB/s per APU thread` (pe 1: 49.12 s, 180.87 GB, 3.68 GB/s). That is the **first Slingshot number: 3.7 GB/s per
  APU thread post-to-completion** at 2 nodes, where post-to-completion holds the D2H/H2D staging copies, the peer's readiness wait
  and the put — the NIC's own rate is above it (one 200 Gb/s = 25 GB/s Cassini per node under Cray's one-NIC-per-PE default, §1;
  4 threads × 3.7 = 14.8 GB/s per node received); e10g run 1 under NFS contention: 0.55 / 1.17 GB/s. Phase C separates the terms. The checkpoint hand-over (`ECALC_CHECKPOINT=1`) was `ABANDONED
  (ECALC_CKPT_TOP=2): 2.15 of 4.44 GB handed over in 12.03 s (0.18 GB/s measured)` — NFS, as designed (the budgeted top set gives up).

##### Step 6 — the N3 log line (`ECALC_LOG_CLOCKS`, `aac7env.sh --log`)

Per node, before the command: `aac7env: node <h> task i of n <time> cpus 0-191`, `… clocks GPU[i] fclk=2000 mclk=1300 sclk=94`
(one line per APU), `… apus 0000:02:00.0:numa0 0001:02:00.0:numa1 0002:02:00.0:numa2 0003:02:00.0:numa3 nics cxi0:numa0 cxi1:numa1
cxi2:numa2 cxi3:numa3`. With `ECALC_LOG_CLOCKS=<s ≥ 2>` a sampler repeats the clocks line every s seconds while the command runs
(`… clocks t=30s GPU[0] fclk=2000 mclk=1300 sclk=95`). **Cost, measured**: the e10e run with a 10 s sampler took 101.6 s against 57–62 s
(bs 25.7 s vs 5.7) — but a later run *without* a sampler (e10g run 1, 172 s, `dm 123.8 s`, PE 0's all-to-all 337 s summed vs
PE 1's 155 s) was slower still while the login node's `sha1sum` was reading the 100 GB reference from the same NFS server, and the
60 s-sampler / no-sampler pair e10f gave 58.9 s / 62.1 s. So the slow runs are **NFS contention (the checkpoint hand-over and the
output writer share the server), not the sampler**; under load some samples come back without `sclk` (rocm-smi prints no level
then). The static block is free (`ECALC_LOG_CLOCKS=1`); a sparse sampler (60 s) costs nothing measurable; keep 10 s samplers for
diagnosis runs.

##### Step 7 — the segfault reproduction (job 12177, aac7-n20 + aac7-n21, 2026-10-04 03:05–03:25 EDT)

A preload `~/p16/A/segv.so` (`segv.c`: a SIGSEGV/SIGBUS/SIGABRT `sigaction` handler printing `backtrace_symbols_fd` to stderr; no
`gdb` on the nodes, `core_pattern` is systemd-coredump's root-only journal) with `FI_LOG_LEVEL=warn FI_LOG_PROV=cxi
SHMEM_ABORT_ON_ERROR=1`, through `mnrun.sh 2 … COMM_SHMEM_DEVHEAP=1` (`~/p16/A/segloop.sh`, `segloop2.sh`, logs `~/p16/A/seg/`):

| launches | pool / heap | segfaults |
|---|---|---|
| 12 × `tests/t_comm` | 8704 MiB / 9216M | 0 (8–11 s each) |
| 12 × `ecalc 10⁹` | 8704 MiB / 9216M | 0 (25–32 s each, 3 VERIFY OK each) |
| 10 × `ecalc 10¹⁰` | 8704 MiB / 9216M | **1** (run 10; runs 1–9: 53.7–58.9 s total) |

With the earlier runs: **3 segfaults in 23 launches of 10¹⁰ at 2 nodes (13 %), 0 in 24 of t_comm / 10⁹ at the same pool and heap**.
Every one is the same: the second task (`aac7-n21`) dies after its `comm_shmem pool: … holds the modelled need 8704 MiB` line
and `bs: arenas 45.1 GB allocated` (the VMM arenas are mapped before `comm_shmem_init`), i.e. inside `shmem_init_thread`, and PE 0
reports `[PE_0]:_pmi_allgather:_pmi_network_allgather failed` (PMI's startup all-gather lost its peer) and then hangs in
`inet_recv: unexpected socket EOF` until the step is killed. So the frame is **Cray's `shmem_init_thread` (its PMI key exchange /
NIC setup), before `shmem_malloc` and before `hipHostRegister`**; what differs from 10⁹ is the 45 GB of VMM-mapped device arenas
(8.6 GB at 10⁹) already in the address space — the lead is a libfabric/cxi or PMI startup that walks or registers the process's
mappings (`FI_CXI_ATS` / `FI_MR_*` ODP, `/proc/self/maps` scans) and trips over the VMM (`hipMemAddressReserve`) ranges.
The preload printed nothing in the failing task (it works on the login node: `kill -SEGV` gives 13 frames) — libsma or PMI installs
its own SIGSEGV handler at init over mine, so the next attempt needs the handler interposed on `sigaction` (or `DB_POOL_VMM=0` /
`SHMEM_OFI_*` / `FI_CXI_*` variations: untried, the 90-minute cap). Workaround meanwhile: a `timeout` around every launch and a
retry — the hang after a peer's death is Cray PMI's; a failed start costs the launch, nothing else (digits never came from a bad run).

#### 4. Open issues
- **10¹⁰ at 2 nodes: task 1 segfaults inside Cray's `shmem_init_thread` in 3 of 23 launches** (step 7); 0 of 24 at t_comm / 10⁹.
  Lead: the 45 GB VMM arena mapped before `comm_shmem_init`. Next: a `sigaction`-interposing preload, then `DB_POOL_VMM=0` ×10,
  `FI_CXI_ATS=0`, `FI_MR_CACHE_MONITOR=disabled`, and `comm_shmem_init` before the arenas (an ordering switch in ecalc.c).
- **Phase B / C must wrap every launch in `timeout`** and treat `_pmi_network_allgather failed` + `inet_recv` floods as a retry.
- `SHMEM_OFI_NUM_NICS=4` vs 1: no difference at 2 nodes × 10¹⁰ (54–55 s both); Cray's one-NIC-per-PE rule (§1) stands until C
  measures the injection with t_comm-sized puts; 4 PEs per node (one per APU, `SHMEM_OFI_NIC_POLICY=NUMA`) is the alternative to test.
- **NFS is the I/O**: the 10¹¹ packed write 44.4 GB in 382 s (116 MB/s), `unpack_digits` of the 100 GB ASCII 1208 s, `sha1sum` of
  it > 15 min; the top-set checkpoint 0.18 GB/s. Everything timed on aac7 must keep its output on NFS out of the wall (it is: `total`
  precedes the write) — or an admin mounts the NVMe (survey §2).
- 10¹¹ on one node at ROCm 7.0.3: 206.7 s vs aac6's 166.7 s (one run each side; bs 96.8 vs ?, dm 85.3 vs ?): Phase B's 7.2.4 A/B.

- `SHMEM_OFI_NUM_NICS` / one NIC per PE (§1): Phase C must measure one PE per node with 1 and 4 NICs against 4 PEs per node.

#### 5. Summary for the integrator

- Build line (aac7, from `ecalc/`): `source aac7env.sh; make -s -j32 SHMEM_CRAY=1 GMP_HOME=$HOME/gmp` (GMP 6.3.0 in `~/gmp`).
- 10⁹ one node: identical, 13.98 s (10.49 s with `DB_POOL_VMM=0`); 10¹¹ one node: **sha1 578f5efb0ff2b9af6b681a375c9ff39197f55cb7 =
  aac6's**, `total` 206.7 s and 196.3 s (aac6 166.7 ± 1.2 s at ROCm 7.2.4), `init: region pools 18.30 s` at ROCm 7.0.3 (the 1e11 arenas).
- t_comm / t_dist / t_mn_grid at 2 real nodes: VERIFY OK; mnaccept 12167: 22 PASS + t_bs PASS after the missing 10⁶/10⁷ references.
- 2 nodes: 10⁹ identical 17.2 s; 10¹⁰ identical, **56.0 s** best of 20 good runs (53.7–62.2 s); the first Slingshot number
  **3.7 GB/s per APU thread post-to-completion** (186.6 GB received per PE in 50.1 s summed over the 4 threads).
- Cray OpenSHMEMX 11.8.0: OpenSHMEM 1.5, THREAD_MULTIPLE, contexts, put-with-signal all used; **host-only symmetric heap** (no HIP
  in libsma) → `COMM_SHMEM_DEVHEAP=1` falls back to the HIP-registered host pool (the note in the log); one NIC per PE by default.

#### RESUME
- 2026-10-04 03:35 EDT: **all steps done**; branch `p16-A` at the head pushed; jobs 12167, 12168, 12177 cancelled (none of mine
  left). On aac7: `~/ntt` (p16-A), `~/ntt-b` (a detached clone at 2964256 used for the e10g–e10h and step-7 runs), `~/gmp`,
  `~/p16/A/` (all logs, `intro_shmem.txt`, `segv.c`, `jobs.txt`), `~/ntt/ecalc/results/e_1e11.out` (100 GB ASCII) + `.sha1`,
  `~/p16/A/e11.out` (44 GB packed), `~/p16/A/ref_1e10.txt`. Open: the step-7 segfault.
- 2026-10-04 00:10 EDT: steps 1–4 done, 5 done but for mnaccept (chained in `~/p16/A/step5a.sh` after step 3's sha1 of the 100 GB
  ASCII, `STEP5A_DONE`, summary `~/ntt/ecalc/results/mnaccept/12167/summary.txt`), 6 done; `~/p16/A/e10g.sh` (a second clone
  `~/ntt-b` at 2964256) runs two 10¹⁰ with the bytes counter (`E10G_DONE`). Jobs 12167 / 12168 to cancel at the end.
- 2026-10-03 23:40 EDT: steps 1, 2, 4 done; step 3's e9 identical-check and the 1e11 run in `~/p16/A/step3.sh` (log `step3.log`,
  `STEP3_DONE`); the 2-node e9 / e10 in `~/p16/A/step5b.sh` (log `step5b.log`, `STEP5B_DONE`); jobs 12167 (1 node, 5 h) and 12168
  (2 nodes, 4 h) in `~/p16/A/jobs.txt` — **cancel them at the end**. Then: mnaccept on 12167 (step 5), the report.

- 2026-10-03 23:25 EDT: step 1 (build options, scripts, README) written locally in `~/apucode/ntt-p16-A`; GMP
  6.3.0 building in the background on aac7 (`~/src/gmp_build.sh`, log `~/p16/A/gmp-build.log`, "GMP_DONE" when finished);
  `~/ntt` cloned on aac7 at main a6667bd. Next: commit + push, `git -C ~/ntt fetch && checkout p16-A`, build (step 2).

## B16 — Phase 16 B: ecalc bring-up on aac7 at 4 → 8 → 12 nodes (10¹⁰ digits per node)

(former file: B16.md)

Agent B, branch `p16-B` from `origin/p16-A` 26671c6 (2026-10-04). Times **Eastern** (aac7 prints PDT = EDT − 3 h).
Every number is **measured** unless marked modelled / assumed. Toolchain: A16's port (ROCm 7.0.3, Cray OpenSHMEMX 11.8.0,
`make -s -j32 SHMEM_CRAY=1 GMP_HOME=~/gmp` in `~/ntt-B` on aac7, build rc 0 in 39 s).

Launch line of every run (internal target notes §4; `mnrun.sh` plans the pool from `MN_PLAN_ONLY`, heap = pool + 512 MiB):
`SLURM_JOB_ID=$J ./mnrun.sh <n> env COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_CHECKPOINT=1 ECALC_VERBOSE=2 ECALC_LOG_CLOCKS=1 ./ecalc <total digits> ~/p16/B/n<n>/<tag>.out`
under `timeout`, from a `setsid nohup` batch (`~/p16/B/step<k>.sh`, log `step<k>.log`, marker `STEP<k>_DONE`); allocations
`sbatch -p 192C4G1H_MI300A_RHEL9_A1 -N<n> --exclusive -c 192 --gpus-per-node=4 -t … -J B16 --wrap "sleep …"` (`~/p16/B/jobs.txt`).
Two walls per run (D3): `total` = the digits computed and verified, before the part-file write; "exit" = the process's exit
(`$SECONDS` around `mnrun.sh`: includes srun start-up, the write to NFS, the checkpoint hand-over).

Digits: 10⁹ through `digcmp.sh` against `ref/e_1000000000.txt`; 10¹¹ through `digcmp.sh` against `results/e_1e11.out`
(sha1 578f5efb…); n × 10¹⁰ (4 × 10¹⁰, 8 × 10¹⁰: no reference file of that length) through `~/p16/B/pcmp.sh`: one
`unpack_digits` pass of the packed parts, `cmp -n D+2` against the 10¹¹ reference's prefix, and the length D + 3 — the same
bytes `digcmp.sh` would compare, without a 40–80 GB ASCII copy on NFS. 12 × 10¹⁰ = 1.2 × 10¹¹ exceeds every reference: the
first 10¹¹ + 2 bytes are compared, the rest rests on `VERIFY OK`.

Modelled walls (`estimate.py --g 4 8 12 --D 1e10 --bw 25 --lat 2e-6 --write-bw 0.1`, **modelled**): without the write 30.6 /
33.1 / 34.1 s at 4 / 8 / 12 nodes; with the write at 0.1 GB/s 73.0 / 75.3 / 76.8 s (the write on NFS is off the clock here).

#### 1. Step 1 — 4 nodes (job 12178: aac7-n40, aac7-n41, aac7-n50, aac7-n51; 2026-10-04 03:30 EDT)

Batch `~/p16/B/step1.sh` (log `~/p16/B/step1.log`, run logs `~/p16/B/n4/<tag>.log`). Launches 4, failures 0 (no segfault, no hang).

| run | digits (total) | `total` (s) | exit (s) | phases (s) | VERIFY | digits |
|---|---|---|---|---|---|---|
| e9 | 10⁹ | 18.02 | 23 | bs 1.1 + dm 4.1 + dc 1.3; init 11.5 | `mn: all 4 nodes: VERIFY OK` | identical (`digcmp.sh` vs `ref/e_1000000000.txt`) |
| e10a | 4 × 10¹⁰ | 135.99 | 142 | bs 23.9 + dm 44.5 + dc 41.8; init 25.7 | all 4 OK | **identical** (`pcmp.sh`: the first 4 × 10¹⁰ + 2 bytes = `e_1e11.out`'s, length 4 × 10¹⁰ + 3; 7 min of NFS reads) |
| e10b | 4 × 10¹⁰ | 127.47 | 134 | bs 23.8 + dm 43.7 + dc 34.1; init 25.8 | all 4 OK | **identical** (`pcmp.sh`) |
| e11 | 10¹¹ | **FAILED rc 6 at 135 s** (not a launch failure) | — | init 33.3 (rns_init 10.6; pool 21504 MiB from the plan) | — | — |

**e11 (10¹¹ on 4 nodes = 2.5 × 10¹⁰ per node) aborts in bs** — rank 0, after init, at bs level 22 (the last batch level):
`ecalc: FATAL [rank 0/4 host aac7-n40, APU 0, thread 0, after init, rc 6]: bs: region pool 0 of parity 1 would grow inside
bs, 10.87 -> 16.15 GB: the regions are sized at init and must not grow (RNS_POOL_GROW=1 allows it)`; `mem[0] [GROW] device 258.3
GB in use (per APU 64.6): planes 103.1, block pool 154.6`, host 32.4 GB RSS. The other three ranks were killed by srun. Memory
was not the limit (258 of 502 GB): the init-time region sizing (Phase 12 R, `binsplit.c:237`) under-sizes parity 1's pool 0 by
49 % at this per-node share on 4 nodes — a sizing bug of the single-node path at 2.5 × 10¹⁰ per node (A16's single-node 10¹¹
and the 2-node 10¹⁰ did not hit it; 10¹⁰ per node here did not either). **Open issue for the bs owner**; step 1c retries with
`RNS_POOL_GROW=1` (the README's documented escape: the pool grows with the accounting), log `~/p16/B/n4/e11g.log`.

**Where the 4 × 10¹⁰ wall sits** (e10b, node 0; measured): init 25.8 s of which `four meshes of 4 PEs over SHMEM: created in
16.4–17.8 s` (the communicator handshake: 9–10 s at 2 nodes in A16); `bs 23.80 s` = seeds during init + `tree levels 20.10 s`
(`mn: node 0 level 1 [0, 2): P 1074769838 limbs … in 7.06 s`; `level 2 [0, 4): P 2222222227 limbs … in 13.02 s`; the
checkpoint of the top set to NFS runs in the background from here and is ABANDONED: `2.42 of 8.89 GB handed over in 16.83 s (0.14
GB/s measured)`); `dm over 4 nodes 43.71 s (reciprocal 16.82)`; `dc 34.06 s` of which **`waiting for the writer 39.17`** (e10a:
37.37) — the early part-file writer (`MN_OUT_EARLY`) to NFS `ran 40.07 s, joined after 33.61 s here`. So on aac7 the `total` line
is **not** the "without the write" wall: the 4.4 GB packed part per node at NFS's ≈ 0.11–0.14 GB/s is inside `dc`, ≈ 32–34 s of
it. Without that wait the compute wall is ≈ 94–102 s against the modelled 30.6 s (3.1–3.3 ×; the model's fabric term assumes
25 GB/s per APU, the measured post-to-completion rate below is 2.8). Step 1b (e10d: parts on the nodes' RAM `/tmp`, no
checkpoint) measures the compute wall directly.

`comm_shmem` summary (e10b; e10a within 0.1 GB/s): pe 0 `all-to-all 6456 exchanges, 222.58 s post to completion (summed over
the APU threads), 635.30 GB received = 2.85 GB/s per APU thread`; pe 1 6456 / 223.51 s / 635.01 GB / 2.84; pe 2 5120 / 223.82 s /
645.51 GB / 2.88; pe 3 5120 / 223.94 s / 623.13 GB / 2.78. (10⁹: 3268–4268 exchanges, 13.3–13.8 GB, 1.9–2.2 GB/s.) Against A16's
3.7 GB/s at 2 nodes: 2.8 at 4 — and 635 GB received per node over 4 threads = 159 GB per APU thread, 222 s summed = 56 s per
thread of the ≈ 100 s compute.

`mem[rank]` (e10b, identical on the four ranks): init device 185.3 GB in use (per APU 49.5; planes 103.1, block pool 81.6); tree
185.3 (live 8.9, peak live 38.8–42.5); bs 185.3; end 103.7 (per APU 25.9). Host 18.6 GB RSS at init, 28.7 at bs, **VmHWM 29.3
GB**. The e9 run: VmHWM 20.2 GB.

Clocks and affinity at launch (`ECALC_LOG_CLOCKS=1`, the static block; e10b, 2026-10-04 03:32:53 EDT), CPU set `0-191` on
every task: sclk 94–95 MHz on every APU (idle); the memory / fabric DPM levels at the launch instant **differ per APU**:
aac7-n40 GPU[0] fclk=2000 mclk=1300, GPU[1–3] mclk=900 (no fclk level printed); aac7-n41 GPU[0–1] fclk=2000 mclk=1300,
GPU[2–3] fclk=1200 mclk=900; aac7-n50 GPU[0] mclk=900, GPU[1] fclk=1200 mclk=900, GPU[2] fclk=1600 mclk=1100, GPU[3]
fclk=2000 mclk=1300; aac7-n51 GPU[0] 2000/1300, GPU[1] 1600/1100, GPU[2] mclk=900, GPU[3] 1200/900 (A16 saw 2000/1300 on
every APU at idle on aac7-n11). These are idle DPM states, not a run-time clock; the per-rank `level 1` / `level 2` times
spread 6.76–7.06 s and 12.83–13.02 s (1.5 %), so no APU is binned low at this size. NUMA map on all four nodes: APU i (PCI
domain 000i) → NUMA node i → `cxi<i>` on NUMA node i.

##### Step 1b — the dist_mn lines and the no-write wall (job 12178, 2026-10-04 04:25–04:57 EDT)

| run | switches | `total` | exit | phases | VERIFY | digits |
|---|---|---|---|---|---|---|
| e10c | `RNS_VERBOSE=1`, checkpoint on, parts to NFS | 130.47 | 138 | bs 23.8 + dm 43.7 + dc 42.9 (writer wait 42.4); init 20.0 | all 4 OK | identical (`pcmp.sh`) |
| e10d | `ECALC_CHECKPOINT=0`, parts on the nodes' RAM `/tmp` | **87.30** | 93 | bs 23.7 + dm 43.1 + dc 0.0 (the writer `ran 2.5–3.3 s, joined after 0.00 s`); init 20.4 | all 4 OK | VERIFY only (4.44 GB per part, deleted on the nodes) |

So at 4 nodes the two walls are **87.3 s without the write (e10d, measured) and 127–136 s with the part files on NFS** (e10a–c's
`total`; the exit 134–142 s). The modelled no-write wall is 30.6 s: **2.85 ×** measured / modelled.

`dist_mn` on node 0 (e10c, `RNS_VERBOSE=1`; `redistribute / ntt / crt / out / spills+carry / total` in s; the top-level products):
- tree level 1 (2 × 4 ranks): `519214282 + 555555557 (+ x) limbs -> 1074769838`: 0.411 / 2.738 / 0.025 / 0.130 / 0.000 / **3.412**
  and 0.285 / 2.742 / 0.025 / 0.133 / 0.001 / 3.227 (`mn: node 0 level 1 … in 6.89 s`);
- tree level 2 (4 × 4 ranks): `1074769838 + 1147452389 (+ x) limbs -> 2222222227`: 0.700 / 5.397 / 0.025 / 0.350 / 0.003 / **6.541**
  and 0.452 / 5.374 / 0.025 / 0.352 / 0.003 / 6.247 (`level 2 … in 13.10 s`; `tree levels 20.00 s`);
- the division (`dm over 4 nodes 43.75 s (reciprocal 16.90)`): the reciprocal's chain 0.738 / 0.770 / 1.481 / 1.538 / 3.007 / 3.107 s
  (`138888891 + 138888890 -> 277777781` … `1111111116 + 555555558 -> 1111111120`), the division's `1111111112 + 1111111113 ->
  2222222225` 6.137 s and `1111111112 + 2222222227 -> 2222222229` 6.259 s (ntt 5.33–5.39 s each).
  The `ntt` term is 80–85 % of every piece; `redistribute` (the all-to-all) 7–11 %. The 2-node 2^30 piece of A16 (1.68 s) and
  these 2^31 pieces (6.1–6.5 s) scale 3.7 × for 2 × the limbs over 2 × the ranks.

##### Step 1c — 10¹¹ on 4 nodes with `RNS_POOL_GROW=1` (job 12178, 04:57–05:03 EDT)

e11g: **rc 0, `total 314.06 s` (bs 72.6 + dm 110.9 + dc 99.1 = 282.7; init 31.3), exit 323 s, `mn: all 4 nodes: VERIFY OK`,
`digcmp.sh` vs `results/e_1e11.out`: identical.** VmHWM 43.0 GB. The grown region pool: `mem[0] tree` device 276.5 GB (per APU
82.7; regions 18.2, block pool 154.6, live 22.2, peak live 83.8), host 32.6 GB; `end` 103.7. The checkpoint and the writer:
`waiting for the writer 106.76` s of the dc 99.1 (the 11.1 GB packed part per node to NFS), `created in 20.73 s` (the mesh
handshake). Per level: `level 1 … P 2690530161 limbs … in 20.27 s`, `level 2 … P 5555555560 limbs … in 32.12 s`, `tree levels
52.40 s`, `dm over 4 nodes 110.89 s (reciprocal 46.84)`. comm: 5596–6936 exchanges, 495–525 s post to completion, 1287–1343 GB
received, **2.48–2.66 GB/s per APU thread**. So `RNS_POOL_GROW=1` gives the identical digits; the abort is the init-time sizing of
parity 1's region pool at this share (a finding for the model: 10.87 GB sized, 16.15 GB needed at 2.5 × 10¹⁰ per node on 4 nodes;
agent R carries the sizing fix on p16-R).

#### 2. Step 2 — 8 nodes (job 12185: aac7-n11, aac7-n30, aac7-n31, aac7-n40, aac7-n41, aac7-n50, aac7-n51, aac7-n60 = ranks 0–7; 2026-10-04 07:10–07:57 EDT)

Batch `~/p16/B/step2.sh` (log `step2.log`, run logs `~/p16/B/n8/`). Launches 5, failures 0. All runs `RNS_VERBOSE=1`,
`ECALC_LOG_CLOCKS=1`; the clocks block at launch: every node's GPU[0] `fclk=2000 mclk=1300`, the other APUs mostly `mclk=900` or
without a level (idle DPM), sclk 94–95 everywhere; the NUMA map APU i → NUMA i → `cxi<i>` on all eight nodes.

| run | digits | `total` | exit | phases | VERIFY | digits |
|---|---|---|---|---|---|---|
| e9 | 10⁹ | 28.88 | 36 | bs 1.4 + dm 13.4 + dc 0.7; init 13.4 | all 8 OK | identical |
| e10a | 8 × 10¹⁰ (NFS parts, checkpoint) | 216.36 | 224 | bs 47.2 + dm 81.8 (recip 35.2) + dc 64.4 (writer wait 74.5); init 22.9 | all 8 OK | identical (`pcmp.sh`, 8 × 10¹⁰ + 2 bytes) |
| e10b | 8 × 10¹⁰ (the same) | 273.02 | 279 | bs 98.7 + dm 85.3 (recip 37.8) + dc 67.6 (writer wait 72.9); init 21.3 | all 8 OK | identical |
| e10d | 8 × 10¹⁰ (RAM `/tmp` parts, no checkpoint) | **327.09** | 334 | bs 118.6 + dm 187.6 (recip 118.5) + dc 0.0; init 20.8 | all 8 OK | VERIFY only |
| e11 | 10¹¹ (1.25 × 10¹⁰ per node; `RNS_POOL_GROW=1` — see the note) | 534.13 | 541 | bs 197.7 + dm 182.2 (recip 131.3) + dc 89.0 (writer wait 49.5); init 65.1 | all 8 OK | identical (`digcmp.sh`) |

(The e11 line ran with `RNS_POOL_GROW=1` from the start: the batch file was edited while bash was waiting in it and bash's read
offset took the fallback form; no `[GROW]` line appears in the log, so the switch was not exercised at 1.25 × 10¹⁰ per node.)

**The 8-node walls are not reproducible and the drift sits in ranks 0–3.** The per-rank level times (`mn: node r level l … in`):

| run | level 1 [0,2) / [2,4) / [4,6) / [6,8) | level 2 [0,4) / [4,8) | level 3 [0,8) | comm GB/s per APU thread (pe 0–7) |
|---|---|---|---|---|
| e10a | 5.8 / 7.2 / 6.9 / 7.2 | 15.5 / 13.3 | 22.2–22.9 | 1.95 1.94 1.97 1.98 2.02 2.02 2.01 1.95 |
| e10b | **24.0** / 7.0 / 6.9 / 7.3 | **47.8 / 65.7** (ranks 0–1 / 2–3) vs 13.3 | 21.9 (ranks 0–3) / **74.3** (ranks 4–7) | 1.28 1.28 1.50 1.51 2.02 2.02 2.01 1.96 |
| e10d | **25.9–31.4** / **40.0–62.7** / 6.9 / 7.2 | **44.2** / 13.3 | 38.9 / **94.4** | 0.99 1.04 0.88 0.94 1.03 1.03 1.03 1.00 |
| e11 | 7.8 / **37.5–130.7** / 7.8 / 8.2 | **137.0** / 14.6 | 48.3 / **170.2** | 0.81 0.81 0.73 1.62 0.82 0.82 0.82 0.79 |

Ranks 4–7 (aac7-n41, s5b0n0, s5b1n0, s6b0n0) repeat the 4-node figures every time (level 1 6.9–7.3 s, level 2 13.0–13.3 s =
step 1's 13.0); ranks 0–3 (aac7-n11, s3b0n0, s3b1n0, s4b0n0 — three nodes other users had held until ≈ 04:10 EDT) are 2–10 ×
slower from e10b on, and the level-3 time that the whole machine waits for grows 22 → 74 → 94 → 170 s run after run (the two
halves of level 3 print different times because the waiting half reports the join). The comm rate falls in step (2.0 → 0.8 GB/s per
APU thread). e10a, the first run, was clean (comm 1.95–2.02 everywhere, level 3 22 s): the degradation set in during the allocation.
What ran beside it: only C16 12180 (2 nodes, aac7-n20/s2b1n0, `sacct`) — the same chassis as rank 0's aac7-n11 is not
(s1 vs s2 are different blades). The static clocks block cannot see a run-time clock drop; ranks 0–3's `mem[rank]` were the same as
the others'. **Open: a per-node health check at job start (load, stray GPU processes) and a 10 s clocks sampler on a diagnosis run
at 8–10 nodes are Phase C's first items**; step 3 adds the health check.

**Both walls at 8 nodes**: with the write 216–273 s (`total`, parts to NFS; exit 224–279 s); without the write 327 s (e10d —
slower than with it, because of the drift, not the write: its dc is 0.0). Modelled 33.1 s: 6.5–9.9 ×. The e9 run (10⁹ on 8 nodes)
is 28.9 s against 18.0 at 4 and 17.2 at 2 (A16): `dm 13.4 s` and `init 13.4` — the fixed costs grow with the node count (the mesh
`created in 10.1–14.3 s`).

#### 3. Step 3 — 10 nodes (12 never free: skoranne's three interactive jobs run for days; 13 − 3 = 10)

**Toolchain: ROCm 7.2.4** (the user's directive of 2026-10-04 12:40 EDT: the fastest software on aac7; `~/ntt-B` rebuilt from clean under `module load rocm/7.2.4`, `MNRUN_MODULES="cray-dsmml cray-openshmemx rocm/7.2.4"`, `ldd` → `/shareddata/opt/rocm-7.2.4/lib/libamdhip64.so.7`, build rc 0 in 39 s; steps 1–2 were ROCm 7.0.3). Job 12193 (`-N10 -t 1:00`, ran
18:23:10–18:38:03 EDT on aac7-n11, s2b0n0, s2b1n0, s3b0n0, s3b1n0, s4b0n0, s4b1n0, s5b0n0, s5b1n0, s6b0n0; batch `~/p16/B/step3.sh`,
log `step3.log`, pre-check `step3pre.log`). `step3pre.log`: all 10 nodes load ≤ 4.7, MemAvail 434–494 GB, `gpu-procs: 0`
(nothing stray) — clean to launch. The launch clocks block is the same pattern as §1–2: GPU[0] `fclk=2000 mclk=1300`, the rest
`mclk=900–1300` or idle, `sclk` 94–98 everywhere.

| run | digits | `total` | exit | phases | VERIFY | digits |
|---|---|---|---|---|---|---|
| e9 | 10⁹ | 43.22 | 56 | bs 9.7 + dm 3.9 + dc 0.7; init 28.9 | all 10 OK | identical |
| e10a | 10 × 10¹⁰ (NFS parts, checkpoint) | 275.32 | 283 | bs 82.1 + dm 80.4 + dc 90.4; init 22.3 | all 10 OK | identical (`pcmp.sh`, 10¹¹ + 2 bytes) |
| e10b | 10 × 10¹⁰ (RAM `/tmp` parts, no checkpoint) | 202.29 | 209 | bs 96.6 + dm 86.4 + dc 0.0; init 19.1 | all 10 OK | VERIFY only |
| e11 | 10¹¹ (10¹⁰ per node) | 290.66 | 298 | bs 83.7 + dm 81.5 + dc 100.1; init 25.3 | all 10 OK | identical (`digcmp.sh`) |

e11 ran clean on the default (no `RNS_POOL_GROW=1`, unlike the 4- and 8-node e11 runs — 10¹⁰/node sits under whatever threshold
triggers the abort at 2.5 × 10¹⁰/node). e10a's and e11's level-2 checkpoints (`ECALC_CKPT_TOP=2`) both print six `ABANDONED`
nodes at ≈ 0.05–0.12 GB/s (same nodes, same rates both runs — 0, 1(or 9), 4, 5, 6(or 8), 7(or 8)) — a known, already-budgeted
background write that doesn't gate the run (§2 note carries over unchanged).

**No drift at 10 nodes.** Unlike the 8-node runs (§2), the per-rank level times stay flat across all three sized runs and across
the whole 10-node set — the per-rank table (`mn: node r level l … in`):

| run | level 1 (pairs, 5 groups of 2) | level 2 [0,10) (all 10, 4 combines) | comm GB/s per APU thread (pe 0–9) |
|---|---|---|---|
| e10a | 3.6–8.2 s (all 10 nodes) | 69.7–74.7 s (all 10 nodes) | 1.56 1.56 1.60 1.60 1.60 1.62 1.62 1.61 1.61 1.57 |
| e10b | 3.7–7.2 s | 85.5–89.3 s | 1.31 1.32 1.35 1.35 1.36 1.36 1.36 1.44 1.33 — |
| e11 | 3.7–7.2 s | 72.4–76.2 s | 1.51 1.51 1.55 1.55 1.55 1.56 1.57 1.57 1.60 — |

Level 1 and level 2 both sit within ~15% rank-to-rank in every run (no outlier rank, no 2–10× spread like §2's ranks 0–3), and
level 1 repeats the 8-node figures (3.6–8.2 s there too) — the 10-node tree adds one group (level 1 pairs → level 2 all-10 in one
step, no intermediate level 3/4 reduction: `meshes connected` at those levels is 0.00–0.09 s, not compute). The comm rate is
flat per run (1.56–1.62 e10a, 1.31–1.44 e10b, 1.51–1.60 e11) and tracks the write pattern, not node identity: e10a (NFS write)
and e11 (NFS write) both sit at ≈ 1.5–1.6 GB/s/thread; e10b (RAM `/tmp`, no checkpoint write) is lower at ≈ 1.3–1.4 — the opposite
of what the missing write would suggest, so the comm rate is answering level-2 all-to-all traffic, not the writer. §2's
per-node-health open item (load / stray-GPU-process check, clocks sampler) is answered here: the pre-check was clean and the
run stayed clean — the 8-node drift was allocation-specific (three recently-released nodes), not a function of node count.

#### 4. Open issue — launch failures

Launches 4 + 2 + 1 + 5 = 12 at 4–8 nodes (10⁹ … 10¹¹), **0 launch failures** (A16's `comm_shmem_init` segfault, 3 of 23 at 2
nodes, did not appear; every launch was under `timeout`). The one abort (e11 on 4 nodes) is the region-pool sizing, §1.

#### 5. The table (measured; modelled = `estimate.py --D <per node> --bw 25 --lat 2e-6 --write-bw 0.1`, the no-write wall; `@2.8` = the same at `--bw 2.8`, the measured post-to-completion rate at 4 nodes)

| nodes | ROCm | digits | per node | wall without the write (s) | wall with the write to NFS (s): `total` / exit | modelled @25 (s) | ratio | modelled @2.8 (s) | ratio | VERIFY / identical | launches / failures |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 (A16) | 7.0.3 | 10¹⁰ | 5 × 10⁹ | — | 56.0–62.2 / 64–69 | 27.5 | 2.0–2.3 | — | — | OK / identical | 7 / 1 (A16's segfault) |
| 4 | 7.0.3 | 10⁹ | 2.5 × 10⁸ | — | 18.0 / 23 | — | — | — | — | OK / identical | 1 / 0 |
| 4 | 7.0.3 | 4 × 10¹⁰ | 10¹⁰ | **87.3** (e10d) | 127.5–136.0 / 134–142 (e10a–c) | 30.6 | **2.85** | 76.9 | 1.14 | OK / identical ×3 | 4 / 0 |
| 4 | 7.0.3 | 10¹¹ | 2.5 × 10¹⁰ | — | 314.1 / 323 (`RNS_POOL_GROW=1`; the default aborts in bs) | 53.4 | 5.9 | — | — | OK / identical | 2 / 0 (1 abort, not a launch failure) |
| 8 | 7.0.3 | 10⁹ | 1.25 × 10⁸ | — | 28.9 / 36 | — | — | — | — | OK / identical | 1 / 0 |
| 8 | 7.0.3 | 8 × 10¹⁰ | 10¹⁰ | 327.1 (e10d, drifted) | 216.4–273.0 / 224–279 | 33.1 | 6.5–9.9 | 93.7 | 2.3–3.5 | OK / identical ×2 | 3 / 0 |
| 8 | 7.0.3 | 10¹¹ | 1.25 × 10¹⁰ | — | 534.1 / 541 (drifted) | 37.1 | 14.4 | — | — | OK / identical | 1 / 0 |
| 10 | 7.2.4 | 10⁹ | 10⁸ | — | 43.2 / 56 | — | — | — | — | OK / identical | 1 / 0 |
| 10 | 7.2.4 | 10 × 10¹⁰ | 10¹⁰ | 202.3 (e10b) | 275.3 / 283 (e10a) | 33.2 | 6.1–8.3 | 97.2 | 2.1–2.8 | OK / identical ×2 | 2 / 0 |
| 10 | 7.2.4 | 10¹¹ | 10¹⁰ | — | 290.7 / 298 | 33.2 | 8.8 | 97.2 | 3.0 | OK / identical | 1 / 0 |
| 12 | — | — | — | not available (10 is the machine's maximum for this user) | | 34.1 | | | | | |

Reading it: at 4 nodes the model is right once the fabric rate is the measured one (2.8 GB/s per APU thread post-to-completion,
not 25): 87.3 measured vs 76.9 modelled. At 8 nodes the walls drifted within one allocation (§2): the first run (e10a, 216 s, with
the ≈ 74 s NFS writer wait inside it, so ≈ 142 s of compute) is the one to model against (93.7 → 1.5 ×); the later ones are the
drift, not the machine. At 10 nodes there is no drift (§3): the no-write wall (e10b, 202.3 s) sits at 6.1 × the @25 model and
2.1 × the @2.8 one, close to the 8-node e10a ratios (6.5, 2.3) — the measured comm rate (1.3–1.6 GB/s/thread, §3) sits below
both 2.8 and 25, so neither bound is tight; e11 (10¹¹, same 10¹⁰/node) matches e10a's with-write wall almost exactly (290.7 vs
275.3) since both pay the same NFS write. The per-level times for Phase C/D are in §1 (step 1b: 2^31 pieces 6.1–6.5 s, `ntt`
80–85 %), §2 (the 8-node per-rank table) and §3 (the 10-node one).

#### Open issues

- **NFS compares are slow and shared**: `pcmp.sh` of a 4 × 10¹⁰ run (17.8 GB of packed parts + 40 GB of reference read on
  the login node) took 7 min for e10a and > 16 min for e10b while agent C's `cmp` of two 100 GB files ran on the same server
  (load 5.6 on aac7-login). The compares are off the clock, but they hold the node allocation if chained before the next run — step 2
  runs every timed run first and the compares last.

- A fresh clone has no `ref/e_1000000000.txt` / `results/e_1e11.out` (generated files, not in git): `digcmp.sh` printed `DIFFERS`
  for a correct e9 until the two were symlinked from `~/ntt`. Check the reference path before believing a DIFFERS.

#### RESUME
- 2026-10-04 12:05 EDT: steps 1, 1b, 1c, 2 done and written (§1–2, §5 rows); job **12193** (10 nodes, 1:00) pending behind
  D16 12190 (4 nodes, ends ≈ 15:37 EDT) and C16 12194; `~/p16/B/step3.sh` is armed on it (runs, scancels the job, then the
  compares; `STEP3_DONE`), `step3pre.sh` prints the per-node load / GPU-process check at the job's start. When `STEP3_DONE`: fill
  §3 and the 10-node row of §5 from `~/p16/B/step3.log` and `~/p16/B/n10/*.log` (both walls, per-rank `level l` lines, comm
  GB/s, clocks), delete nothing else (the batch deletes the parts and `.top`), commit, push, return the protocol line. Jobs:
  12178 and 12185 are cancelled; **12193 must not stay pending if the session stops** (`scancel 12193`).
- 2026-10-05: step 3 done. Job 12193 ran 18:23:10–18:38:03 EDT on 10 nodes; all three sized runs (e9, e10a, e10b, e11) OK,
  digits identical where checked; no drift (unlike §2's 8-node run) — §3 and the 10-node §5 rows filled from `step3.log` and
  `n10/*.log`. Phase B's three node counts (4, 8, 10) are now all measured and written; the open items are §2's 8-node drift
  (Phase C's first item per §2) and §4's launch-failure note (already closed, 0 failures throughout B). Nothing else pending
  on job 12193 — it self-cancelled at the end of `step3.sh`.

## D16 — Phase 16 D: the A/Bs on aac7, and the Phase E scripts (the 12-node scaled headline)

(former file: D16.md)

Agent D, branch `p16-D` from `main` e7dfcae (2026-10-04). Times **Eastern** (aac7 prints PDT = Eastern − 3 h). Every number
**measured** unless marked modelled / assumed. aac7 clone `~/ntt-D` (p16-D), outputs `~/p16/D/`, jobs in `~/p16/D/jobs.txt`.

#### 1. The E-phase script: `ecalc/e16_headline.sh` (no nodes used; the plan checks on the login node)

`SLURM_JOB_ID=<12-node allocation> ./e16_headline.sh [plan|record|dev|chain|below|all]` — the target's launch line (TARGET §4) on
aac7 through `mnrun.sh 12` (the cray branch plans the pool itself), `timeout` around every launch, a retry on the init segfault
(A16 §4: rc 124/139 or the PMI flood before any `total`), `_DONE` markers per step under `~/p16/E/`, both walls per launch in
`~/p16/E/log/walls.txt` (`total` = the run's clock without the write; the wrapper's elapsed = with the parts written to NFS).

| item | value (login node, `MN_PLAN_ONLY` / `BS_LAYOUT_ONLY`, 2026-10-04 00:55 Eastern; logs `~/p16/D/plan/`) |
|---|---|
| total digits | **1 099 200 000 000** = 12 × 9.16 × 10¹⁰ (the target's top node: 9.169 × 10¹⁰ of 5.276 × 10¹³ / 576) |
| `plan check` | `1.099e+12 digits g 12, ECALC_NP=auto: OK -- 102 products`; the largest piece 16 962 962 965 limbs (the division's DKM step 1); `plan primes`: 0 pieces at four primes |
| `plan summary` | pieces tree 64 recip 49 div 68 = **181** on node 0, **189** on the critical path; levels (node 0 / largest) 12/16 12/16 40/40 |
| the schedule at g = 12 | the default: **groups of 2, 4, then 12 (3-way)** — `MN_GROUPS` not set (PLAN: only at 576 or 9·2ᵏ) |
| `plan cache` | `RNS_DIST_CACHE_PARTIAL=1, RNS_DIST_CACHE_MN=2`: slots per grid product from the block pool (nothing added) |
| `plan pool` | **`COMM_SHMEM_POOL_MB=8704`** (staging 4 × 2048 MiB by tree level of 2), heap ≥ 9216 MiB — `mnrun.sh` sets both |
| layout (`BS_LAYOUT_ONLY=91600000000:12`) | planes 103.70 + arena 292.06 = device 395.76 GB, host 43.6 GB, **node 445.36 GB, fits 480** (TARGET_HW_REVIEW §4 row E: 445 GB modelled) |
| the grid steps at g = 12 | `~/p16/D/plan/sweep12.txt` (0.96–1.13 × 10¹² in 5 × 10⁹ steps, then 1 × 10⁹ around the step): critical path 157 (≤ 0.995) → 163 (1.000) → 167 (1.050) → 177 (1.065) → 179 → 182 (1.075) → 184 (1.080) → 185 (1.085–1.090) → **189 at 1.091–1.095** (the top level 36 → 40) → 188/189 to 1.110 → 192/193 from 1.115 |
| one step below | **1 090 000 000 000** (`E16_BELOW`): 177 / 185 pieces, levels 12/16 12/16 36/36, plan check OK |
| modelled walls (`estimate.py --g 12 --D 9.16e10 --write-bw 0.1`) | at `--bw 25` (one 200 Gb/s NIC per APU, ASSUMED): **323.3 s without the write, 691.9 s with it** at 0.1 GB/s per node (NFS: A measured 0.116 GB/s single-stream, ASSUMED to hold with 12 writers); at `--bw 3.7` (A's measured 2-node post-to-completion rate, which includes the staging copies — an upper bound of the fabric's cost): 1140.4 / 1385.4 s. Node 445 GB, pool 9 GB in the model |
| disk | 488 GB of packed parts per run, 1.1 TB of ASCII (NFS: 2.7 TB free on 2026-10-04): the dev run's parts are deleted after the byte compare with the record's, the ASCII parts after the ASCII RECHECK + the 10¹¹ prefix, the below run's after its RECHECK |

Steps: `plan` (stops unless `plan check … OK` and node ≤ 480 GB) → `record` (no checkpoint, no top set) → `dev` (`ECALC_CHECKPOINT=1`; its
packed parts compared byte-wise with the record's, headers' binary fields and limbs) → `chain` (RECHECK packed through `mnrun.sh 12 …
ECALC_RECHECK=1`; `tools/unpack_digits` per part, 2 tasks per node over the 24 parts; RECHECK on the ASCII parts; the leading 10¹¹ + 2
bytes of parts 0000–0002 against `results/e_1e11.out`) → `below` (+ its RECHECK). `E16_CANCEL=1` cancels the allocation at the end.
**Not run yet** (12 nodes are B's; the integrator launches it, or D if B's step 3 is done and 12 nodes are free).

#### 2. N2 — RCCL all-to-all vs the push kernel (1 node, job 12182, aac7-n11)

Job 12182 (1 node, exclusive), 2026-10-04 03:45–04:25 Eastern; logs `~/p16/D/n2/` (`n2.sh`, `n2b.sh`, `n2c.sh`, `n2.log`). All measured.

- **RCCL** `rccl-tests/7.0.3` (`module load rocm/7.0.3 openmpi/5.0.10-libfabric2.3.1-xpmem-2.7.4-rocm-7.0.3 rccl-tests/7.0.3`
  inside the srun — the module's `PATH` is not seen in a task, and the binary needs OpenMPI's `libmpi.so.40`; the file is
  `alltoall_perf`, not `all_to_all_perf`): `alltoall_perf -b 1M -e 1G -f 2 -g 4` (4 APUs, one process), three runs
  (`rccl_a2a_{1,2,3}.log`; run 3 with `-n 20 -w 5`): at size 1 GiB (256 MiB per rank, 192 MiB of it to peers) **time 3.44 ms
  out-of-place, 3.31 ms in-place → algbw 312 / 324 GB/s, busbw 234 / 243 GB/s** (busbw = the bytes crossing the links per second,
  summed over the node = **58.5–60.8 GB/s per APU**); 256 MiB: 300 / 309 algbw, 225 / 232 busbw; 64 MiB: 262 / 267, 197 / 201;
  16 MiB: 205 busbw 154; run-to-run spread < 1 %. "Avg bus bandwidth" 155.5 / 155.3 (1 MiB–1 GiB) and 221.3 (64 MiB–1 GiB).
- **The push kernel** (`DIST_H4=1 DIST_H4_MB=<mb> ./tests/t_dist 24`, the all-to-all mode 0, b%3, 76 blocks per peer;
  `h4_{64,256,1024}M.log`): per APU **236.0 GB/s sent at 64 MiB per (sender, peer), 250.8 GB/s at 256 MiB, 253.6 GB/s at 1 GiB**
  (every APU within 0.1 GB/s of the others; per link 84–90 GB/s; one link alone 83–88 GB/s), i.e. **≈ 1003–1014 GB/s per node of
  link traffic** at 256 MiB–1 GiB (aac6: 909 GB/s). Mode 1 (contiguous thirds) 252.5, fan-out 253.2 GB/s.
- **Verdict: RCCL's all-to-all moves 234–243 GB/s of link bytes per node against the push kernel's ≈ 1003 GB/s — 0.24×, not
  > 1.2×: no transport change** (TARGET_HW_REVIEW §3 item 4; RCCL's 1.6 TB/s "bus bandwidth" in the report is a ring all-reduce
  figure, not an all-to-all). The push's per-link rate (84–90 GB/s) is at the xGMI link's ≈ 91 GB/s.
- Also on this node: `COMM_XGMI_STATS=1 ECALC_VERBOSE=2 DIST_STATS=1 ./ecalc 10000000000` ×2: `total 30.22 s` (bs 8.4 + dm 6.4;
  init 12.7), identical to `~/p16/A/ref_1e10.txt` both times (72 / 84 s with the NFS write + compare). The `xgmi-stats` report does
  not print at size 1 (the H4 stamps are taken only on the layered communicator's path, `comm_xgmi.c:76`) — the H4 bench above is
  the single-node measurement.

#### 3. The 4-node pairs (`ecalc/tests/d16_pairs.sh`, one fresh job per pair since cc036af+; the first pair ran in one allocation)

**Set s1, 4 × 10¹⁰ digits on 4 nodes (10¹⁰ per node), the parts on NFS**, job 12190 (aac7-n31, s4b0n0, s4b1n0, s5b0n0; 2 h after
the integrator's "B's 8-node first" hold), 2026-10-04 11:38–12:04 Eastern, `ECALC_CHECKPOINT=1`, the launch line's switches; logs
`~/p16/D/s1/`:

| run | `total` (no write) | elapsed (with the NFS write) | VERIFY | digits vs `ref_4e10.txt` | phases |
|---|---|---|---|---|---|
| dkm_A1 `MN_OUT_DKM_HI=1` | **206.74 s** | 214 s | all 4 OK | identical | bs 23.3 + dm 67.9 + dc 73.9; init 41.7 (region pools 2.6–7.5 s per node) |
| dkm_B1 `MN_OUT_DKM_HI=0` | **144.32 s** | 151 s | all 4 OK | identical | bs 23.1 + dm 50.6 + dc 52.3; init 18.3 (2.2–2.6 s) |

One pair, A first in a fresh allocation, so the 62 s are not yet a verdict: the init alone differs by 23 s (the VMM mapping 7.5 vs 2.5 s on
two nodes), and dm / dc both 17–22 s slower under DKM_HI=1 with the writer on NFS during the division (the write is in the `total`
here: NFS at ≈ 0.1 GB/s, 4.4 GB per node, the writer competes with the division). The set is repeated one job per pair (the s1 of
the new script) after E. `digcmp.sh` of the 4 × 10¹⁰ file from NFS took 14 min per run on the login node — the script now hashes
the parts on the nodes and checks the baseline's top part against the reference.

**Set s1 repeated, one fresh job per pair** (`d16_pairs.sh s1` from `~/ntt-D` at 90fdeb1: **ROCm 7.0.3, no `COMM_INIT_EARLY`** —
the set started before the 7.2.4 build; kept on 7.0.3 for the whole set so its pairs compare like with like), jobs 12196–12205,
2026-10-04 12:14–16:49 Eastern; 4 × 10¹⁰ on 4 nodes, parts on NFS, `ECALC_CHECKPOINT=1`; logs `~/p16/D/s1/` (`walls.txt`, `<tag>.log`,
`<tag>.key`, `<tag>.sha1`). `total` = the run's clock (the NFS write runs in the writer thread *inside* it under `MN_OUT_DKM_HI=1`);
elapsed = the process's exit. All measured.

| pair (job) | A | `total` A (bs + dm + dc; init) | B | `total` B (bs + dm + dc; init) | B − A |
|---|---|---|---|---|---|
| dkm1 (12196) | DKM_HI=1 | 143.58 (23.4 + 42.8 + 57.0; 20.2) | DKM_HI=0 | 142.51 (27.1 + 51.2 + 42.6; 21.4) | −1.1 s |
| dkm2 (12200) | DKM_HI=1 | 149.34 (23.3 + 49.6 + 54.4; 21.9) | DKM_HI=0 | 141.01 (24.0 + 52.2 + 42.3; 22.4) | −8.3 s |
| tch1 (12201) | T_CHUNK 1024 | 180.80 (35.6 + 63.4 + 46.3; 35.4) | 2048 | 190.32 (33.0 + 92.1 + 37.7; 27.4) | +9.5 s |
| tch2 (12202) | 1024 | 124.56 (23.8 + 46.0 + 31.1; 23.6) | 2048 | **launch failure** (`_pmi_network_allgather failed`, rc 143 after 1163 s) | — |
| tch2r (12203) | 1024 | 142.89 (23.9 + 56.0 + 37.2; 25.6) | 2048 | 143.57 (29.5 + 55.7 + 35.1; 23.2) | +0.7 s |
| ser1 (12204) | SERIAL=0 | 193.15 (31.2 + 75.7 + 54.4; 31.8) | SERIAL=1 | 133.01 (24.4 + 50.9 + 38.7; 19.0) | −60.1 s |
| ser2 (12205) | SERIAL=0 | 128.44 (23.8 + 44.2 + 36.0; 24.4) | SERIAL=1 | **373.46** (109.1 + 214.5 + 25.5; 24.3) | +245.0 s |
| nic1 (12210) | `SHMEM_OFI_NUM_NICS=1` | 137.05 (23.2 + 42.8 + 43.5; 27.4) | =4 | 137.43 (23.8 + 50.7 + 41.4; 21.5) | +0.4 s |
| nic2 (12213) | =1 | 136.52 (24.4 + 51.0 + 39.2; 21.8) | =4 | 143.52 (28.0 + 53.5 + 32.1; 29.8) | +7.0 s |

The `stats` pairs (COMM_LAYER_STATS / COMM_XGMI_STATS per DKM form) were **not run**: the s1 driver was stopped after nic2
(18:20 Eastern, its next job 12215 cancelled) — aac7 has 10 usable nodes (13 − 3 held by skoranne), exactly B's and E's size, so
every 4-node job of ours delays them. ser_B2's 373 s ran on aac7-n31, which Slurm drained right after the job
(`needs_reboot : reboot issued : reboot timed out`, 17:26 Eastern; back idle ≈ 17:55).

**Set pe (2 nodes × 10¹⁰, 1 vs 4 node-processes per node; 7.2.4 + `COMM_INIT_EARLY=1`) — dropped.** pe_A1 (job 12212, 1 PE per
node): **88.59 s** (bs 10.6 + dm 23.7 + dc 34.3; init 19.9), top part = `ref_2e10.txt`. pe_B1 (`mnrun.sh 8`, 4 PEs per node): **all four
ranks on aac7-n41 stopped by the memory guard at 21.2 s** (`MemAvailable 3.17 GB < ECALC_MEM_GUARD_GB=6`; Cached 96 GB = the
HIP-registered host pools of 4 processes): each process sizes its plane pools for the default cap, not for D/4 — P16 §2.3 measured
the same (the 4-PE form needs `ECALC_PLANE_CAP=30`). Both nodes (s4b0n0, s4b1n0) were then drained for a reboot. The retry
pe_A1r on fresh nodes (12214): 293.48 s (init 71.0; beside the s1 job's NFS writes) — the same configuration 3.3× slower. The
1-vs-4-PE comparison is P16's (job 12208, with the cap); D does not repeat it.

**Node reboots after our jobs (measured, Slurm's states):** after 12205 (1 node), 12212 (2, the memory-guard abort) and 12213 (3 of 4,
a clean exit + `scancel`), Slurm put the nodes into `needs_reboot : reboot issued` — ≈ 30 min per cycle, each delaying B / E further.

Digits: every run `VERIFY OK` on all 4 nodes; the set's first run (dkm_A1) top part = `ref_4e10.txt`'s leading 4 999 999 988 bytes;
every DKM_HI=1 run's 8 part hashes identical to it. The two "DIFFERS" lines in `walls.txt` are not digit errors: (1) dkm_B1/B2
(DKM_HI=0) write 4 parts, not 8 — another layout, other hashes (the same form was byte-identical to the reference by `digcmp` in
job 12190 above); the script now keeps one baseline per part layout (12e7321); (2) ser_B2: the job's 45-min limit hit during the
hashing — 6 of 8 parts hashed, all 6 identical, parts 0006/0007 missing.

**Reading (measured):** the same configuration spans **124.6–193.2 s** across jobs (the A runs: 143.6, 149.3, 180.8, 124.6, 142.9,
193.2, 128.4), i.e. ±25 % job to job; the within-pair deltas of DKM_HI (−1, −8 s) and T_CHUNK (+10, +1 s) are inside that spread —
**no verdict on either at 4 nodes × 10¹⁰** beyond "not a large effect". `COMM_SHMEM_SERIAL=1` went −60 s then +245 s (dm 214.5 s:
the serialized contexts stalled in the division's products once in two) — **keep 0**. The two slow A runs (180.8, 193.2 s; init 31–35 s vs 20–24 s
elsewhere) shared aac7-n20 / s2b1n0 / s3b0n0, but dkm2 on the same three (+ s4b0n0) ran 149.3 s — no single node explains it.

**Set s2, 1.6 × 10¹¹ on 4 nodes (4 × 10¹⁰ per node), parts in the nodes' RAM `/tmp`; ROCm 7.2.4 + `COMM_INIT_EARLY=1`
(`~/ntt-D724`), one 1.5 h job per pair; logs `~/p16/D/s2/` (the first attempt, job 12219, in `s2_job12219/`). Measured:**

| run (job) | switch | `total` | digits |
|---|---|---|---|
| cache_A1 (12219) | `RNS_DIST_CACHE_PARTIAL=1` | 342.61 s (bs 100.6 + dm 202.6; init 39.1) | VERIFY OK (job cancelled during the compare) |
| cache_A1 (12221) | `RNS_DIST_CACHE_PARTIAL=1` | **350.54 s** | VERIFY OK; baseline 8 parts; **top part = `e_1e11.out`'s first 2 × 10¹⁰ bytes** |
| cache_B1 (12221) | `RNS_DIST_CACHE_PARTIAL=0` | **memory guard** on 2 of 4 nodes at 179–180 s ("after tree"; MemAvailable 4.6 / 5.4 GB, Cached 131 GB), the step hung to the 1500 s timeout | — |
| cache_A1r (12222) | `=1` | 669.97 s (the same config 1.9× slower: the fabric, as E's below run) | identical to the baseline |
| cache_B1r (12222) | `=0` | **memory guard** again (aac7-n30 at 103.1 s: 2.39 GB), timeout at 1500 s | — |

| cache_A2 (12223) | `=1` | 417.65 s | identical to the baseline (cache_B2 not run: the driver stopped, `D16_ONLY`) |
| tch_A1 (12224) | `MN_T_CHUNK_MB=1024` | **Bus error (core dumped) on aac7-n21**, step hung to 1500 s | — |
| tch_A1r (12225) | `=1024` | **`Node failure on aac7-n21`** at 420 s; the node is `down* Not responding` since | — |
| tch_A2 (12227) | `=1024` | **348.98 s** | identical to the baseline |
| tch_B2 (12227) | `=2048` | **329.47 s** (B − A = −19.5 s) | identical to the baseline |
| stats (12230) | `COMM_LAYER_STATS=1 COMM_XGMI_STATS=1` | 348.85 s | identical to the baseline (`~/p16/D/s2/stats.key`) |

**Reading:** at 4 × 10¹⁰ per node `RNS_DIST_CACHE_PARTIAL=0` (the full transform cache) **does not fit** on aac7's nodes as they are
now (MemAvailable ≈ 440 GB after the 77 GB Ollama RAM copy, §1b) — twice out of two; `=1` (the launch line's) runs. So the pair
gives a memory verdict, not a time one: **keep `RNS_DIST_CACHE_PARTIAL=1`** (§5's "memory Δ 0" is wrong at this size: the full
cache needs more than the ≈ 30 GB of headroom left). s8c (the same pair at 8 nodes) is **not run** for the same reason
(`~/p16/D/s8c/SKIPPED`); its tch pair is covered by s8.

#### 4. The 8-node pairs

**Set s8, 8 × 10¹⁰ on 8 nodes (10¹⁰ per node), parts on NFS; ROCm 7.2.4 + `COMM_INIT_EARLY=1`, one 45-min job per pair; logs
`~/p16/D/s8/`, 2026-10-05 02:39–09:18 Eastern. Measured:**

| run (job) | switch | `total` (bs + dm + dc; init) | digits |
|---|---|---|---|
| grp_A1 (12233) | `MN_GROUPS` default (2,4,8) | **218.11 s** (44.4 + 67.8 + 70.6; 35.2) | VERIFY OK; 16 parts hashed; **top part = `ref_8e10.txt`** |
| grp_B1 (12233) | 2,8 | lost: the job expired during A1's ≈ 50-min reference compare | — |
| grp_A1r (12239) | default | **225.17 s** | identical to the baseline (16 parts) |
| grp_B1r (12239) | 2,8 | **232.94 s** | VERIFY OK; top part = `ref_8e10.txt`; only 4 of 16 parts hashed (see below) |
| grp_C1r (12239) | 8 | lost: the job expired during B1r's compare | — |
| grp_A2 (12251) | default | **248.96 s** (53.9 + 91.0 + 53.4; 50.6) | identical to the baseline |
| grp_B2 (12251) | 2,8 | **234.50 s** (61.5 + 77.5 + 59.4; 36.0) | VERIFY OK; top part = `ref_8e10.txt`; 10 of 16 parts hashed |
| grp_C2 (12251) | 8 | lost: the job expired during B2's compare | — |
| grp_A2r (12258) | default | **203.16 s** (43.9 + 65.5 + 65.4; 28.2) | identical to the baseline |
| grp_B2r (12258) | 2,8 | launch failure rc 143 after 160 s (all tasks `Terminated`) | — |
| tch_A1 (12259) | `MN_T_CHUNK_MB=1024` | **216.60 s** (43.5 + 70.7 + 71.4; 31.0) | identical to the baseline |
| tch_B1 (12259) | 2048 | lost: the job expired | — |

The tch2 pair and the stats single were not run: the s8 driver was stopped at 09:18 Eastern for the approved step-below E run.

**Reading (measured):** default `MN_GROUPS` 203.2–249.0 s over four runs (±10 %); `2,8` 232.9 / 234.5 s, inside that range — **no
measurable difference at 8 nodes × 10¹⁰**; `MN_GROUPS=8` (the flat 8-way top) never ran: its launch came after a reference compare
three times and the 45-min job had expired each time. The B runs' digit evidence is VERIFY OK on all 8 nodes plus the top part
against the reference; their per-part hash files are short (4 and 10 lines for 16 parts written — the hashing `srun` returned
before every node had printed), so the "baseline (N parts)" in `walls.txt` is an artefact of the hash step, not another layout.
Two fixes for a rerun (not made): a ≥ 1.5 h job for any pair whose B or C writes a new layout, and a check that the hash file has
2G lines.

#### 5. The options table (the measured columns are filled as the pairs run; the modelled deltas at 576 are RESULTS §93–94's B3 model, 5.276 × 10¹³ on 576)

| switch (A vs B) | measured Δ at 4 nodes (`total` / with the write) | spread | memory Δ | modelled Δ at 576 | recommendation |
|---|---|---|---|---|---|
| `RNS_DIST_CACHE_PARTIAL` 1 vs 0 | s2 (4 × 10¹⁰ per node): **=0 died on the memory guard twice** (179 s, 103 s); =1 350.5 / 670.0 s, digits identical | — | =0 does not fit ≈ 440 GB nodes (measured) | −8 s (int15j, modelled) | **keep 1** (memory) |
| `MN_OUT_DKM_HI` 1 vs 0 | B − A **−1.1, −8.3 s** (`total`, 2 pairs, 7.0.3; the 12190 pair −62 s) | runs of one config 124.6–193.2 s across jobs (±25 %) | 0 | −7 s with the write (EW15, modelled at 0.6 GB/s) | on the launch line already |
| `MN_T_CHUNK_MB` 1024 vs 2048 | s1 (10¹⁰/node): B − A **+9.5, +0.7 s**; s2 (4 × 10¹⁰/node): **−19.5 s** (one pair; the other lost to a failing node); s8: 1024 216.6 s, 2048 lost (job expired) | ±25 % across jobs; the same config 350–670 s in s2 | +4.3 GB per node (window temporaries, modelled) | −10.9 s at `T_ROUND` 0.03 s (−5 at 0.01, −48 at 0.1; RESULTS §94) | T11: decide on the measured `T_ROUND` (C) |
| `COMM_SHMEM_SERIAL` 0 vs 1 | B − A **−60.1, +245.0 s** (B2: dm 214.5 s) | unstable under 1 | 0 | not modelled (the contexts' concurrency) | **keep 0** |
| `SHMEM_OFI_NUM_NICS` 1 vs 4 | B − A **+0.4, +7.0 s** (2 pairs; A16: no difference at 2 nodes × 10¹⁰) | inside the ±25 % across jobs | 0 | not modelled | no change: no measurable effect |
| `MN_GROUPS` 2,4,8 vs 2,8 vs 8 | s8 (10¹⁰/node, 8 nodes): default 203.2–249.0 s (4 runs), 2,8 232.9 / 234.5 s; 8 not run (the jobs expired) | default ±10 % | — | the 9-way top −4.7 s (T5, modelled) | no difference at 8 nodes; set only at 576 (T5) |
| RCCL all-to-all vs the push kernel (N2) | **RCCL 234–243 GB/s busbw vs the push ≈ 1003 GB/s per node: 0.24×** (measured, §2) | < 1 % | — | — | **drop: no transport switch** |

#### 1b. E at 10 nodes (the integrator, 2026-10-04 11:50 Eastern: 12 nodes are never free; the 12-node figures stay as the modelled column)

| item | value (login node, `~/p16/D/plan/plan_916e9_10.txt`, `sweep10.txt`) |
|---|---|
| total digits | **916 000 000 000** = 10 × 9.16 × 10¹⁰ (`E16_NODES=10`, the script's default now) |
| `plan check` | `9.16e+11 digits g 10, ECALC_NP=auto: OK -- 98 products`; largest piece 16 962 962 966 limbs |
| `plan summary` | tree 46 recip 46 div 43 = **135** on node 0, **139** on the critical path; levels 12/16 34/34; the default schedule **2, 10** (groups of 2, then the 5-way top) |
| `plan pool` | 8704 MiB, heap ≥ 9216 MiB |
| layout | node **453.95 GB** (fits 480; planes 103.70 + arena 300.65 GB) |
| the step below | 9.08 → 9.10 × 10¹¹: the top level 32 → 34 pieces (crit 137 → 144); **`E16_BELOW` = 908 000 000 000** |
| modelled walls (`--g 10 --D 9.16e10 --write-bw 0.1`) | **282.5 s without the write / 658.8 s with it at `--bw 25`**; 951.0 / 1233.3 s at `--bw 3.7`; node 454 GB |

Parts on NFS (the RAM `/tmp` would add 40.7 GB per node to a 454 GB node: over 480); the `total` wall is the no-write wall, the
process's exit the wall with the write.

**E attempt 1 — job 12195 (10 nodes, 18:46–18:56 Eastern), `~/ntt-D724` at dc4769e (ROCm 7.2.4, `COMM_INIT_EARLY=1` via
`E16_EXTRA`), logs `~/p16/E/` (`e9.txt`, `e16.out`, `log/`):**
- the build check: `ecalc 10⁹` on one node, `total 13.69 s`, **identical** to `ref/e_1000000000.txt`, `libamdhip64.so.7` from
  `/shareddata/opt/rocm-7.2.4` (measured).
- `plan`: OK (`COMM_SHMEM_POOL_MB=8704`, node 453.95 GB).
- **record: failed** — rc 134 after 458 s: rank 6 (aac7-n41) stopped by the memory guard at 283.0 s (`MemAvailable 4.60 GB <
  6`; MemFree 4.22, Cached 94.14 GB, RSS 19.29 GB; the other ranks' budget lines show 62–110 GB available at the same point), then
  the SHMEM job aborted (`VNI_NOT_FOUND` on the peers). The node **started** with MemAvailable **442 GB** (the other nine 463–519 GB:
  `log/nodes_12195.txt`; loads 2–4.6: the nodes had just left B's job and a reboot) against the 454 GB layout; 15 min later the same
  node showed 512 GB (job 12216's node log). Not a code fault: the run needs ≈ 454 GB + the 6 GB guard on every node at launch.
- Fix (a switch, off by default): `E16_MEMWAIT_GB=<GB>` in `e16_headline.sh` (d6e73d6) polls every node's MemAvailable before each
  launch (up to `E16_MEMWAIT_S`, then launches anyway and logs the values). The same commit fixes the walls' `total` regex (ecalc
  prints `total` + several spaces: every E walls line said `total none`).

**E attempt 2 — job 12217** (10 nodes, 5 h, started 19:30 Eastern). The memwait (`E16_MEMWAIT_GB=465`) logged every node's
MemAvailable **falling ≈ 1–1.5 GB/min on idle nodes** (aac7-n41 385 → 379 GB in 4 min). Cause, measured on the nodes:
**each node's boot runs `rsync -a --delete /shareddata/Ollama_Models/ /var/local/cache/ollama/` as root, and `/var/local/cache` is on
the RAM root overlay (`/rootfs.rw/upperdir`, tmpfs): 77 GB of model blobs per node** (66–73 GB copied after 55 min on the rebooted
nodes; 307 MB on aac7-n31). Every node Slurm rebooted today (after our jobs and others': `needs_reboot`) comes back ≈ 77 GB
short: MemTotal 526.2 GB → MemAvailable ≈ 435–445 GB once the copy ends, against the 454 GB layout of 9.16 × 10¹⁰ per node.
(Also cleaned: 50 GB `/tmp/p16D` on 3 nodes and a 20 GB `/tmp/p16D_top_*.txt` — s2's leftovers when its driver was stopped.)
**So the target's per-node share does not fit on aac7 now** (an admin-side change, not ours to undo). E runs one size down:

| size (10 nodes) | layout node (`BS_LAYOUT_ONLY`) | plan (crit path) |
|---|---|---|
| 9.16 × 10¹¹ (target share) | 453.9 GB | 139 |
| 8.8 × 10¹¹ | 445.4 GB | 135 |
| 8.6 × 10¹¹ | 436.8 GB | 133 |
| **8.4 × 10¹¹ (E now)** | **428.2 GB** | 128 |
| 8.0 × 10¹¹ | 419.6 GB | 123 |

Sweep 8.0–8.5 × 10¹¹ (`~/p16/D/plan/sweep10_84.txt`): critical path 123, 124, **122 (8.20)**, **129 (8.25)**, 125, 126, 128 (8.40),
131, 130 → **the step below = 8.20 × 10¹¹** (122 → 129). E at 8.4 × 10¹¹ = 8.4 × 10¹⁰ per node (91.7 % of the target's 9.16 × 10¹⁰),
`E16_OUT=~/p16/E84`, `E16_BELOW=820000000000`, `E16_MEMWAIT_GB=432`; watcher `~/p16/D/e10_84.sh 12217`; record launched 19:56 Eastern
with every node ≥ 439 GB.

**E record run, 8.4 × 10¹¹ digits on 10 nodes (job 12217, 19:56–20:37 Eastern; `~/p16/E84/log/rec.log`) — measured:**

| item | measured | modelled (`estimate.py --g 10 --D 8.4e10 --write-bw 0.1`) |
|---|---|---|
| `total` (the run's clock; the X_hi parts' NFS write runs inside the division) | **2429.52 s** (bs 755.6 + dm 1338.4 + dc 272.1 + T1 0.6; init 56.8, other 6.0) | 266.8 s at `--bw 25` (assumed); 905.9 s at `--bw 3.7`; **2274.6 s at `--bw 1.31`** (the measured rate, below) |
| elapsed (launch → the srun's exit, all parts on NFS) | **2456 s** | 610.3 / 1156.9 / **2327.2 s** (with the write) |
| digits | **`mn: all 10 nodes: VERIFY OK`** (in-run); **RECHECK of the packed parts: `mn: all 10 nodes: RECHECK OK`** (1552 s, 20:37–21:03 Eastern); unpack of the 20 parts, each checked mod the T1 primes: 20 of 20 rc 0 (2993 s); **the leading 10¹¹ + 2 bytes identical to `results/e_1e11.out`** (1391 s); the ASCII RECHECK **skipped** (time, `SKIPPED_ASCII_RECHECK`) | — |
| fabric | pe 0: 22 572 all-to-all exchanges, 9 791.5 GB received, 7 491 s post-to-completion summed over 4 APU threads = **1.31 GB/s per APU thread** (933 exchanges in 5 555 rounds of ≤ 1024 MiB) | `--bw` = GB/s per APU injection |
| tree | level 1 (pairs) 95.1 s; level 2 (the 5-way top, 4 combines) 618.0 s | — |
| division | 1338.4 s: reciprocal 298.5 (products 262.3), step 1 504.0, step 2 A·mu 201.8 + X_lo·Q 332.4; division products 1018.0 s | — |
| write | node 0: 2 × 18.67 GB, 592.8 + 214.9 s in the writer thread (≈ 0.03–0.09 GB/s per node, NFS, 10 writers) | 0.1 GB/s per node (assumed) |
| memory | budget rank 0: peak 400.2 GB (device 378.6, host 21.6), VmHWM 36.9 GB; every node ≥ 439 GB MemAvailable at launch | layout 428.2 GB per node |
| init | `COMM_INIT_EARLY=1`: transport opened in 8.5–15.0 s; no init fault in this launch | — |

**Reading:** the measured wall is **9.1× the `--bw 25` model and 2.7× the `--bw 3.7` one; fed the measured 1.31 GB/s per APU thread,
the model gives 2274.6 / 2327.2 s, within −6.4 % / −5.2 % of the measured 2429.5 / 2456 s** — the compute side of the model holds;
aac7's Slingshot path at 10 nodes (Cray OpenSHMEMX, the rounds' path) is the wall: ≈ 77 % of the run is the exchange (7 491 s summed
over 4 threads ≈ 1 873 s per thread of 2 430). (For scale: the 4 × 10¹⁰-on-4-nodes pairs of §3 ran 124–193 s.)

**The step-below run, 8.20 × 10¹¹ (job 12217, launched 22:16 Eastern; `log/below.log`) — not completed.** Level 1 took 64.2 s (the
record: 95.1 s), but the inter-node level 2 ran **5–7× slower than in the record run 80 min earlier on the same 10 nodes**: combines
1–3 ended at 554.1 / 1460.8 / 2428.8 s (record: 104.6 / 230.0 / 401.4 s). No foreign process on the nodes (load ≈ 4 = the four APU
threads, MemAvailable 33–114 GB in use as expected); the cause is outside the job (the shared Slingshot / NFS, not measured). At that
pace it could not finish inside the allocation (ends 00:30 Eastern), so it was stopped at 23:14 Eastern (driver killed, job 12217
cancelled; no walls line). **The step's cost (8.20 vs 8.40 × 10¹¹: critical path 122 vs 128 pieces) is not measured**; the record
run's pace itself varies by this much between runs on aac7's fabric, which the pairs of §3 (±25 % at 4 nodes) also show.

**The 576-node implication (modelled only; no 576-node fabric was measured):** `estimate.py --g 576 --D 9.16e10` (5.276 × 10¹³ digits):
**292.4 s without the write / 340.9 s with it at 0.6 GB/s per node** at the default `--bw 100` (assumed: the target's two 400 Gb/s
NICs per APU); **4 808.4 / 4 858.9 s at `--bw 1.31`** (if the target's fabric delivered per APU only what aac7's did here). Node 472 GB
modelled (fits 480; 502 with no guard). The 16× spread between the two is the open question: the target's per-APU all-to-all rate at
576 nodes is not measured anywhere in this phase — the E run shows the model is right *given* that rate.

#### RESUME
- 2026-10-05 10:05 Eastern — **armed, no agent attached.** Job 12262 (D16E, 10 nodes, 3.5 h; Slurm estimate 17:30 Eastern) ← driver
  `~/p16/D/e82.sh` (PID 1543186): memwait → run 8.20 × 10¹¹ (`~/ntt-D724m` ff66152, 7.2.4, `COMM_INIT_EARLY=1 MN_COMM_MARK=1`) → VERIFY →
  unpack parts 0000–0003 → the 10¹¹ prefix vs `e_1e11.out` → `~/p16/E82/log/walls.txt` + **`~/p16/E82/E82_DONE`** → `scancel`; a collapse
  (level 2's first combine > 330 s after level 1) → one retry in a fresh job (`below.retry.log`). Backstop `~/p16/D/e82_end.sh` (PID 1778618):
  when the driver exits, writes **`~/p16/E82/E82_END`** (`OK` / `FAILED` + the last log lines) and cancels any leftover D16E job.
  Collect (aac7, `sshpass -p <cluster-password> ssh aac7`):
  `cat ~/p16/E82/E82_END; cat ~/p16/E82/log/walls.txt; cut -c1-250 ~/p16/E82/log/e82.log`
  `grep -E '^comm-mark|^mn: node 0 level' ~/p16/E82/log/below*.log | cut -c1-260`   (per-level GB/s; none printed = MN_COMM_MARK did not work)
  `grep -E '^total +[0-9]|all 10 nodes: VERIFY|^recip\(mn\)|^divmod\(mn|comm_shmem: pe 0: all-to-all|FATAL' ~/p16/E82/log/below*.log | cut -c1-330`
  Compare with the 8.4 × 10¹¹ record (§1b): `total` 2429.52 s, level 2 combines 104.6 / 230.0 / 401.4 / 618.0 s, 1.31 GB/s per APU thread
  (`grep -E '^mn: node 0 level|comm_shmem: pe 0: all-to-all' ~/p16/E84/log/rec.log`). Write it into §1b, push p16-D.
- 2026-10-05 09:45 Eastern: s8 written up (§4/§5). **The step-below job 12262 (10 nodes, 3.5 h) is PENDING (Resources), Slurm's start
  estimate 17:30 Eastern**: the A1 partition has 13 nodes (the 14th c1 node, in `A1_CPX`, is not usable by this job); minus skoranne's 3
  and aac7-n21 (`down*`) = 9. It starts when skoranne's 12010 ends (17:30 Eastern) or s2b1n0 returns. `e82.sh` (PID 1543186) waits
  on it and runs everything (VERIFY, the 10¹¹ prefix, the collapse rule, one retry max); results in `~/p16/E82/log/`.
- 2026-10-05 08:50 Eastern: **the step-below E run approved** (8.20 × 10¹¹, 10 nodes; the coordinator). `~/p16/D/e82.sh` (PID 1543186):
  waits for s8's job 12259, stops the s8 driver (tch2 / stats of s8 not run), submits one 10-node 3.5 h job, runs `~/ntt-D724m`
  (ff66152: `MN_COMM_MARK=1` per-level fabric rate) with VERIFY + the 10¹¹ prefix (no RECHECK); a collapse (level 2's first combine
  > 330 s after level 1) → one retry in a fresh job. Output `~/p16/E82/log/` (`e82.log`, `walls.txt`, `below.log`), `E82_DONE`.
- 2026-10-05 05:20 Eastern (handover): **running unattended**: `~/p16/D/s2_switch.sh` (PID 1347757) → `d16_pairs.sh s8` (grp2, tch1/2,
  stats; job 12251 queued, est. 07:10 Eastern); `~/p16/D/chain2.done` when finished; results `~/p16/D/s8/walls.txt` → §4 / §5. s2 done
  (§3), s8c skipped, pe dropped, E done (§1b). `~/p16/E84/rec/` (350 GB packed parts) still on NFS — delete after review.
  Open: aac7-n21 `down*`; the 77 GB Ollama RAM copy on every rebooted node (admin-side) caps E at ≈ 8.4 × 10¹⁰ per node.
- 2026-10-05 02:25 Eastern: s2 tch pair 2 done (§3), pair 1 lost: **aac7-n21 failed** (Bus error, then `Node failure`; Slurm `down*
  Not responding`). s2's `stats` single running (job 12230); then s8 (8 nodes, 9 usable).
- 2026-10-05 01:25 Eastern: `~/p16/D/s2_switch.sh` (PID 1347757) waits for cache_A2's walls line, stops the s2 driver + chain before
  cache_B2 (PARTIAL=0 cannot fit), then runs `D16_ONLY="tch stats" d16_pairs.sh s2` (1.5 h jobs; same dir, same baseline) and `s8`
  (45 min jobs); `~/p16/D/chain2.done` at the end. s8c skipped (`~/p16/D/s8c/SKIPPED`).
- 2026-10-04 23:50 Eastern: s2 restarted (job 12221) with `D16_WALLTIME=1:30:00` for s2 / s8c (`pairs_chain724.sh` edited; 45 min cannot
  hold a 4 × 10¹⁰-per-node pair plus the first run's top-part compare). The first s2 attempt kept in `~/p16/D/s2_job12219/` (cache_A1
  342.61 s, VERIFY OK; job cancelled during the compare). Chain PID 1316986.
- 2026-10-04 23:20 Eastern: **E done as far as aac7 allows** — record 8.4 × 10¹¹ measured + verified (VERIFY, packed RECHECK, 10¹¹ prefix);
  the below run stopped (5–7× slower fabric, §1b). Packed parts kept: `~/p16/E84/rec/` (350 GB on NFS; delete when the integrator has
  read §1b). **Pairs chain running**: `pairs_chain724.sh 12217 s2 s8 s8c` (PID 1305510; 7.2.4 + `COMM_INIT_EARLY=1`), outputs
  `~/p16/D/<set>/walls.txt`.
- 2026-10-04 21:30 Eastern: E record 8.4 × 10¹¹ done (2429.52 s, VERIFY OK, §1b); RECHECK (packed) OK in 1552 s; the unpack running;
  **the ASCII RECHECK skipped by hand** (`~/p16/E84/SKIPPED_ASCII_RECHECK`: the 5 h job, ends 00:30 Eastern, cannot hold it and the below
  run); then the 10¹¹ prefix, the below run (8.20 × 10¹¹) + its RECHECK — driver `~/p16/D/e84_rest.sh` (cancels 12217 at the end).
- 2026-10-04 20:00 Eastern: **E running at 8.4 × 10¹¹ on job 12217** (`~/p16/D/e10_84.sh`, out `~/p16/E84/`): the 77 GB Ollama RAM cache on
  rebooted nodes (§1b) makes 9.16 × 10¹¹ impossible. After E: `rm -rf ~/p16/D/s2`, then `pairs_chain724.sh` with EJ = 12217 for s2 s8 s8c
  (s2 / s8c at 4 × 10¹⁰ per node = 404+ GB layouts: check they fit 435 GB first).
- 2026-10-04 19:30 Eastern: **E attempt 1 (12195) failed in the record run on the memory guard** (one node started at 442 GB, §1b);
  **E attempt 2 = job 12217** (watcher `e10_724f.sh`, PID 1222963; `E16_MEMWAIT_GB=465`). s2 started (job 12216, cache_A1 331.92 s,
  VERIFY OK, 8 parts hashed) and was **cancelled** for E during its top-part compare; the s2 driver and the chain are stopped. After E:
  restart `pairs_chain724.sh <EJ> s2 s8 s8c` (edit EJ to 12217; `rm -rf ~/p16/D/s2` first: its baseline is incomplete).
- 2026-10-04 18:25 Eastern: s1 stopped after nic2 (§3; stats not run); pe dropped (P16 covers it). `pairs_chain724.sh 12195 s2 s8 s8c`
  (PID 1211955) now waits until **both 12193 and 12195 have left the queue**, then runs s2, s8, s8c on 7.2.4 + `COMM_INIT_EARLY=1`. Nodes
  s2b0n0 / s5b0n0 / s6b0n0 rebooting (after 12213); B and E start when 10 are idle. E watcher e10_724e.sh (PID 1174354) armed.
- 2026-10-04 17:35 Eastern: **aac7-n31 DOWN+DRAIN** (`needs_reboot : reboot issued : reboot timed out`, Slurm 17:26 Eastern;
  it was in ser2's job 12205 — ser_B2's 373 s ran on it): 13 − 3 (skoranne) − 1 = **9 usable nodes, so B 12193 and E 12195 (10 nodes
  each) cannot start** (Slurm's estimate: 2026-10-05 17:30 Eastern). Until the node is back: the s1 driver runs nic/stats (job 12210),
  `pairs_chain724.sh` restarted without the s1 wait (PID 1197887) — pe on job 12212 now, then s2, s8, s8c beside s1 (concurrent NFS
  writers: noted per set). The E watcher e10_724e.sh still waits on 12195.
- 2026-10-04 16:55 Eastern (agent resumed): **s1 dkm / tch / ser pairs done** (§3, 7.0.3); the s1 driver (PID 1094761, `~/ntt-D`)
  continues with `nic`, `stats` — its next job 12210 (4 nodes) is queued behind B 12193 (starts 16:49) and E 12195. **E watcher
  replaced**: `~/p16/D/e10_724e.sh 12195` (PID 1174354) on `~/ntt-D724` rebuilt at dc4769e (main merged in: `COMM_INIT_EARLY`), ROCm
  7.2.4, `COMM_INIT_EARLY=1` + `E16_EXTRA=COMM_INIT_EARLY=1` (the old watcher 1158149 killed; it never ran). **Pairs chain replaced**:
  `~/p16/D/pairs_chain724.sh 12195 pe s2 s8 s8c` (PID 1176214; 7.2.4 + early init, `~/ntt-D724` at 12e7321) starts after the s1
  driver exits, holds while E runs (old chain 1094756 killed). Outputs: `~/p16/E/` (e9.txt, e16.out, log/walls.txt), `~/p16/D/<set>/`.
- 2026-10-04 12:10 Eastern: job 12190 released after the dkm pair (B's 10-node 12193 first); **the E job 12195 (10 nodes, 5 h,
  `D16E`) is queued behind it with the watcher `~/p16/D/e10.sh` → `~/p16/E/` (`e16.out`, `log/walls.txt`, `E16_DONE`; it cancels
  itself).** After E: `./tests/d16_pairs.sh s1`, `s2`, `pe`, then `s8`, `s8c` (each allocates its own jobs; `~/p16/D/<set>/`).
- 2026-10-04 04:30 Eastern: N2 done (§2: RCCL 0.24× of the push; no transport change); job 12182 cancelled. Waiting for B16 8-node (12185) to finish before the 4-node job.
- 2026-10-04 01:20 Eastern: `ecalc/tests/d16_pairs.sh` committed (cc036af; sets s1 / s2 / s8 / s8c, see its header; `SLURM_JOB_ID=$J
  ./tests/d16_pairs.sh s1` from `~/ntt-D/ecalc` after `source aac7env.sh`; outputs `~/p16/D/<set>/`, `walls.txt`, `<tag>.key`, `DONE`).
  N2 on job 12182: the push 10¹⁰ runs done (identical), rccl-tests chained in `~/p16/D/n2b.sh` (binary by absolute path:
  `/shared/apps/rhel9/rocm-7.0.3/rccl-tests-7.0.3/bin/all_to_all_perf`; the module's PATH is not seen inside srun), then `t_dist`'s
  H4 push bench in `n2c.sh` (`N2C_DONE`). `~/p16/D/ref_8e10.txt` being written (`ref_8e10.done`). The cache pair needs 4 × 10¹⁰
  per node (`~/p16/D/plan/grids_g4_g8.txt`: no mn grid product below 1.6 × 10¹¹ at g = 4, 3.2 × 10¹¹ at g = 8) → sets s2 / s8c.
- 2026-10-04 01:00 Eastern: step 1 done (the script committed; the plan / layout / sweep on the login node in `~/p16/D/plan/`); N2
  running on job 12182 (1 node, 2 h; `~/p16/D/n2.sh`, `N2_DONE` in `~/p16/D/n2/n2.log`); the 4-node job 12183 cancelled on the
  integrator's instruction (B's 8-node job 12185 first) — resubmit `sbatch -p 192C4G1H_MI300A_RHEL9_A1 -N 4 --exclusive -c 192
  --gpus-per-node=4 -t 4:00:00 -J D16 --parsable --wrap "sleep 14400"` when B16's 8-node job has finished. The 4 × 10¹⁰ reference
  `~/p16/D/ref_4e10.txt` (the 10¹¹ file's first 4 × 10¹⁰ + 2 bytes + newline) is ready. Next: N2's verdict, then the 4-node pair script.

## V16 — Phase 16 V — fastest stack on aac7 (ROCm 7.2.4 default), survey, brief A/Bs

(former file: V16.md)

Repo `ntt-V` on aac7 (branch `p16-V`). All numbers measured unless marked modelled/assumed.

#### 1. Default stack: rocm/7.2.4

`aac7env.sh`/`mnrun.sh` now load `AAC7_ROCM` (default `rocm/7.2.4`) instead of the bare `rocm` alias
(= system default `rocm/7.0.3`). `AAC7_ROCM=rocm/7.0.3` reverts to the old stack (e.g. to match the
target's toolchain, TARGET_HW_REVIEW).

**Trap found and fixed**: every `rocm/*` and `rocm-new/*` modulefile on aac7 carries `conflict rocm`.
The default shell profile already has `rocm/7.0.3` loaded, so a later `module load rocm/7.2.4` prints
`ERROR:150` to stderr (swallowed by the scripts' `2>&1`) and silently leaves 7.0.3 active — `hipcc`
stayed `/opt/rocm-7.0.3/bin/hipcc`. Fix: `module unload rocm/7.0.3` by its exact name before the load,
in `aac7env.sh` and in all four `module load $MNRUN_MODULES` sites of `mnrun.sh`'s wrappers. Verified
after the fix: `hipcc` resolves under `/shareddata/opt/rocm-7.2.4`, HIP 7.2.53211.

**D's branch (`p16-D`, commit `eb499a9`) built the same fix independently** (`AAC7_ROCM` + `MNRUN_UNLOAD=rocm`),
opt-in (unset `AAC7_ROCM` keeps 7.0.3). Kept both: V's differs by making `rocm/7.2.4` the *default* (unset
`AAC7_ROCM` now gets 7.2.4), per this task's directive; D's keeps the old default unless set. Not merged into
one implementation — flagging for the eventual RESULTS merge to pick one.

Build: `source ecalc/aac7env.sh; cd ecalc; make -s -j32 SHMEM_CRAY=1 GMP_HOME=$HOME/gmp` — rc 0 under the new
default. One-node e9 (1e9): `VERIFY OK`, digits `cmp`-identical to `~/p16/A/e9.out` and `digcmp.sh` identical
against `~/ntt/ecalc/ref/e_1000000000.txt`.

#### 2. Survey (login node `aac7-login`, read-only; "builds?" = `make SHMEM_CRAY=1 GMP_HOME=...` on `~/ntt-V`)

| Module | Version(s) seen | Builds ecalc? | Notes |
|---|---|---|---|
| `rocm` (system) | 7.0.3 | yes (old default) | `/opt/rocm-7.0.3`, no module needed on login |
| `rocm` (shareddata/base) | 7.2.3, **7.2.4** | yes, yes | 7.2.4 = aac6's default toolchain; **newest that builds** |
| `rocm` (shareddata/base) | 7.12.0, 7.13.0, 7.14.0 | **no** — all 3 | same failure: `ld.lld: relocation R_X86_64_32 cannot be used against local symbol; recompile with -fPIC` in the HIP device-code link step (`ntt.o`, `fatal.o`). A toolchain regression in these preview/next-gen builds, not a flag we can work around from ecalc's Makefile without more investigation |
| `rocm-new` | 7.1.1, 7.2.0(default), 7.2.1 | (7.2.1 tested: yes) | same 7.2.x family as the default |
| `rocm-new` | 10.0.0 | **no** | same -fPIC device-link failure as 7.12–7.14 |
| `rocm-afar`, `rocm-therock` | 7.0.0/7.1.1/22.x, 23.1.0/23.2.1 | not tested | afar/therock are internal/experimental trees; out of scope given 7.12–10.0.0 already fail uniformly |
| `cray-openshmemx` | 11.7.4, **11.8.0(default)** | — | **already the newest**; nothing to A/B |
| `libfabric` | **2.3.1 (only version)** | — | nothing newer on aac7 |
| `cray-mpich` | 8.1.32/8.1.33/9.0.0/9.0.1, **9.1.0(default)** | — | already the newest; GPU-aware (built against rocm, `craype-accel-amd-gfx942` loaded) |
| `openmpi` (5.0.10 variants, `/shared/apps/modules/rhel9`) | several `libfabric2.x`/`ucx`/`rocm-*` combos | — | has real OSHMEM: `oshcc/oshCC/oshc++/oshfort/oshrun/oshmem_info` + `liboshmem.so` present (checked the `rocm-7.0.3` + `ucx1.19.1` build). A genuine alternative SHMEM, not ported/tested against ecalc — out of scope for a brief survey |
| rocSHMEM | ships inside `rocm/7.13.0` (`include/rocshmem`, device bitcode) | — | different API (device-side SHMEM calls), not a drop-in for ecalc's host-side OpenSHMEM; noted, not pursued |
| Sandia OpenSHMEM (SOS) | not found | — | `~/sos` absent on aac7 (different home than aac6) |
| `FI_CXI_*` tunables | only `FI_CXI_RX_MATCH_MODE` found compiled into this libfabric 2.3.1 build (`strings libfabric.so.1`); `FI_CXI_RDZV_THRESHOLD`/`RDZV_EAGER_SIZE`/`DEFAULT_TX_SIZE` are **not present** in this build | — | `fi_info`/`man fi_cxi` unavailable on login (no cxi there); checked by `strings` on the lib instead |
| `SHMEM_OFI_*` tunables | ~150 names found in `libsma.so` (NIC_POLICY, NUM_NICS, MAX_NICS, GET/PUT_PIPELINE_*, etc.) | — | large surface; only `NIC_POLICY` and `RX_MATCH_MODE` tried (below) given the brief-A/B budget |

**ROCm conclusion**: 7.2.4 is simultaneously the new default *and* the newest version that builds ecalc —
nothing newer in the 7.12–10.0.0 generation links successfully (uniform `-fPIC` device-link error across
7.12.0, 7.13.0, 7.14.0, and rocm-new/10.0.0). No separate "7.2.4 vs newest" A/B was needed; C16 §2.7 already
measured 7.2.4 vs 7.0.3 (168.5 vs 205.9 s at 1e11, identical digits).

#### 3. Brief A/Bs, 2 nodes (aac7-n11, aac7-n20; job 12209), 1e10, `mnrun.sh 2`

Baseline (default stack, `COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_CHECKPOINT=1`):
**total 61.80 s**, `comm_shmem: ... 3.56 GB/s per APU thread` (within the 3.6–3.7 GB/s range cited).

| Knob | total (s) | Δ wall | GB/s per APU thread | Δ GB/s | 2nd run? |
|---|---|---|---|---|---|
| (baseline) | 61.80 | — | 3.56 | — | — |
| `FI_CXI_RX_MATCH_MODE=hybrid` | 63.86 | **+3.3% slower** | 3.68 | +3.4% | no — below both bars |
| `SHMEM_OFI_NIC_POLICY=ROUND-ROBIN` | 62.40 | +1.0% slower | 3.67 | +3.1% | no — below both bars |

(b) cray-openshmemx 11.8.0 vs newer: **skipped** — 11.8.0 is already the newest module on aac7 (§2).
(a) ROCm 7.2.4 vs newest-that-builds: **skipped** as a separate A/B — 7.2.4 is itself the newest that
builds (§2); already measured against 7.0.3 in C16 §2.7.

No knob cleared the >5% wall / >10% GB/s bar, so `AAC7_FI_TUNE=1` (added to `aac7env.sh`, off by default)
documents the negative result and stays empty rather than exporting tunables that don't help.

#### 4. Recommended default stack (measured)

`cray-dsmml cray-openshmemx/11.8.0 rocm/7.2.4` (now `aac7env.sh`'s default via `AAC7_ROCM`), `cray-mpich/9.1.0`,
`libfabric/2.3.1` — all already the newest usable versions on aac7 for ecalc's build. No newer ROCm,
OpenSHMEMX, libfabric, or mpich beats this; the only genuinely newer SHMEM-capable stack on the system
(OpenMPI 5.0.10 + OSHMEM) would need a port, out of scope here.

#### Files changed

- `ecalc/aac7env.sh`: `AAC7_ROCM` (default `rocm/7.2.4`), `module unload rocm/7.0.3` fix, `AAC7_FI_TUNE` hook (empty).
- `ecalc/mnrun.sh`: `MNRUN_MODULES` default picks up `AAC7_ROCM`; same unload fix in all 4 wrapper sites + the plan-pool call.
- `ecalc/README.md`: `AAC7_ROCM`, `AAC7_FI_TUNE` rows; `MNRUN_MODULES` row updated.

#### RESUME

Done: default flipped to rocm/7.2.4 (module-conflict bug found and fixed), e9 verified byte-identical,
full module/library survey (table above), 2-node brief A/B on 3 knobs (none promising). Pushed to
`origin/p16-V` (commits f53c05e, 62e11d2, and this report's commit). Left open: OpenMPI 5/OSHMEM and
rocSHMEM are real alternative SHMEM stacks on aac7 that were not ported/tested — a future phase's work,
not this one's. `AAC7_FI_TUNE=1` is a no-op today; revisit if a different knob set is identified.

## ROCM - ROCm 7.14.0 against 7.2.4 at 1 node (OPPORTUNITIES M7d)

(former file: ROCM.md)

**Question.** The target runs ROCm 7.14.0. Does ecalc build and run correctly with it, and is it faster or slower than the aac7 default 7.2.4?
**Change / branch.** None (docs only). Commit 39346d03 of `main`, built twice with `SHMEM_CRAY=1`, once under each ROCm module.
**Method.** aac7, 1 node, 1e10 digits, default settings (aac7 profile launch line), digits written and compared with the prefix of the
GMP reference; `VERIFY` read from each log; the log's HIP runtime path checked per run. ABBA-style, 4 rounds (B A, A B, B A, A B), A = rocm 7.2.4,
B = rocm 7.14.0. Unload every ROCm module before loading the wanted one (see Open issues).
**Results.** All 8 runs: VERIFY OK, digits identical, no segfault (measured).

| round | A 7.2.4 (s) | B 7.14.0 (s) | B - A (s) |
|---|---|---|---|
| 1 | 35.68 | 60.65 (init 33.8 s, cold first launch) | +24.97 |
| 2 | 38.49 | 34.40 | -4.09 |
| 3 | 38.28 | 35.24 | -3.04 |
| 4 | 36.14 | 37.25 | +1.11 |

- All pairs: B - A = +4.74 s, 95 % CI [-17.02, +26.49] (t, n = 4 pairs) (measured).
- Without round 1 (post hoc): B - A = -2.01 s, 95 % CI [-8.62, +4.59] (t, n = 3 pairs) (measured). Stage split (bs, recip, dm, init) equal in the two arms.
**Verdict.** 7.14.0 builds and gives identical digits; no speed difference is detectable at this size (CIs contain 0). Keep 7.2.4 as the aac7 default; no change.
**Open issues.**
- Cold first launch: the first 7.14.0 run took 60.65 s (`init` 33.8 s against about 9 s later). In an earlier attempt the first 7.14.0 run did not
  finish the reciprocal stage within a 240 s timeout (killed). Cause not established (assumed first-use code-object build); give the first target run a long timeout.
- Not measured: 1e11 digits, multi-node, 7.14.0 with the SHMEM-only path.
- Method trap: if another ROCm module is already loaded, `module load rocm/7.14.0` silently fails (conflict) and the run uses the old runtime;
  set `MNRUN_UNLOAD` to all ROCm modules and check the libamdhip64 line in the log.
- A 7.14.0 binary needs the 7.14.0 module at run time (its RPATH names only the llvm library directory).
**RESUME.** Done. Next, if wanted: one 7.14.0 gate at 2 nodes and a 1e11 ABBA; raw logs stay on aac7.
