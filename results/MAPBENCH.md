# MAPBENCH — S1/E1: helper processes that create the GPU allocations, against one process with four threads (2026-11-01)

Program: `ecalc/tests/t_mapbench.c` (`make tests/t_mapbench`; standalone, not linked into ecalc). One aac7 node, 4 APUs, queue item
run 2026-11-01 about 11:07-11:25 ET (ROCm 7.2.4). Per chunk: `hipMemCreate`, `hipMemMap`, `hipMemSetAccess` (4 APUs), then a kernel
writing one word per 4 KiB page (first touch). Size as ecalc: 20 chunks of 2 GiB per APU (40 GiB per APU, 160 GiB per node), plus a
quick size (4 x 0.5 GiB). Wall = from the go signal to the last first-touch kernel, HIP initialized and address ranges reserved
before (helper start-up is separate, `helper_init`, 0.4 s, and overlaps the main process's own start-up). Three repetitions per
arm and load, run interleaved, one process per repetition.

Arms: **A** one process, 4 threads, global lock around a chunk's create+map+access+touch (today's background mapper); **B** as A
without the lock; **C** as B, threads pinned to their APU's NUMA node CPUs; **D** 4 helper processes (one per APU, pinned to its
NUMA node) create with `requestedHandleType = PosixFileDescriptor`, `hipMemExportToShareableHandle`, send the fd over a Unix socket
(SCM_RIGHTS); the main process's 4 threads (unpinned) `hipMemImportFromShareableHandle`, map, set access, touch; **E** as D with
the main threads pinned. "Load" = 48 threads copying 128 MiB buffers in a loop (memory bandwidth, started 0.7 s before the timing).

## Results (measured; full size, median of 3, min-max in brackets, seconds)

| arm | wall, no load | wall, load | speedup vs A (no load / load) |
|---|---|---|---|
| A lock | 15.87 (15.16-15.88) | 15.77 (14.42-16.83) | 1 |
| B no lock | 15.65 (15.19-15.89) | 15.46 (15.23-15.61) | 1.01 / 1.02 |
| C no lock, pinned | 12.29 (12.25-12.37) | 12.41 (12.38-12.42) | 1.29 / 1.27 |
| D helpers | 3.82 (3.81-3.83) | 3.87 (3.82-3.89) | **4.2 / 4.1** |
| E helpers, main pinned | 3.82 (3.76-3.84) | 3.87 (3.82-3.88) | 4.2 / 4.1 |

Quick size (1 repetition each, no load): A 1.20, B 1.11, C 0.98, D 0.49, E 0.54 (A/D = 2.4); the helper's 0.4 s start-up is larger
relative to this size.

Phases, mean over the 4 APUs of each thread's summed time (full size, no load, median; A's phases exclude the wait for the lock):

| arm | create | export / import | map | access | first touch | wait for helper |
|---|---|---|---|---|---|---|
| A | 3.01 | - | 0.11 | 0.85 | 0.00 | - |
| B | 6.46 | - | 3.65 | 2.20 | 1.64 | - |
| C | 5.33 | - | 3.21 | 1.84 | 0.51 | - |
| D main | - | 0.02 (import; export in the helper 0.001) | 1.96 | 1.26 | 0.19 | 0.11 |
| D helper | 2.17 (20 chunks, ends at 2.19) | | | | | |

Load changes nothing beyond the spread (D 3.82 -> 3.87; A 15.87 -> 15.77).

## Answers

- **Pass criterion (D at least 2.5x faster than A, create through first touch): met, 4.2x measured** (4.1x under load). A to B shows the
  in-process serialization (four unlocked threads are no faster than one locked sequence: each phase of B takes about twice A's);
  pinning (C) recovers only 22 %; separate processes recover 74 %.
- **Where the page clearing happens (U1): at `hipMemCreate`, in the creating process.** The helper's create is 0.108 s per 2 GiB chunk
  (about 19 GiB/s); the importer's import is 1 ms per chunk and the first-touch kernel 10 ms per chunk (A: create 0.15 s, touch 0.00 s).
  So the gain exists: creation, the dominant cost, moves to the helpers and runs in parallel across processes.
- **What remains in the main process** is map + access: 3.2 s per APU for 20 chunks = 0.16 s per chunk, which is above the design's
  secondary target (import+map+access of 0.05 s per chunk) and is now the critical path (helper 2.2 s, main 3.8 s). Map and access were
  not compared with a local handle here (A's map 0.11 s is not comparable: its lock keeps the other threads out; B's 3.65 s is
  four threads contending). Not measured: whether the main-side map+access can be cut (the runtime's per-process cost).
- **Checks (all passed, measured):** data written by APU d is read correctly by APU d+1 through the main process's mapping (peer access
  after import works); unmap + remap of an imported chunk at another slot and back works (APU d and d+1 read the same data); after
  all helpers exited (waitpid) the imported memory reads back correctly and `hipMemGetInfo` used memory is unchanged (40.1 GiB per
  APU): **helpers need not stay alive** once the main process has imported the handle (the fd may be closed after import); releasing
  all handles returns the memory (free 87.8 -> 125.8 GiB on APU 0). Same results for D and E at the quick size.
- Shareable handles: ROCm 7.2.4 on aac7 accepts `hipMemHandleTypePosixFileDescriptor` for pinned device allocations on MI300A. No error.

## Caveats

- Standalone, not ecalc: a process that has done only HIP init; in ecalc the same helpers would start at program start and create
  while the plan fit and `mn_init` run, so their 0.4 s start-up is hidden, but the main process's other work competes for the
  runtime lock. The pass is for the creation phase; the gain in ecalc's init also depends on what binds (seed wait, rns_init tail).
- Memory: helper RSS (4 HIP runtimes) not measured here. The main process maps what the helpers created, one extra copy of nothing.
- 3 repetitions; the spread is under 4 % for D, up to 14 % for A under load.

Raw lines: `ssh <cluster> 'cat ~/mapbench/all.txt'` (RESULT lines, one per run).

**RESUME.** Branch mapd: program and result committed, not merged. Next (decision for the coordinator): integrate behind
`ECALC_MAPD=1` (design §3 step 2), or take only the NUMA pinning (arm C, 1.29x) which needs no helper.

## Integration, phase 1 (arenas only): `ECALC_MAPD=1` (2026-11-01)

Code: `ecalc/mapd.c`, `mapd.h`; hooks in `dbig.c` (`vmm_map_run` takes the next chunk from the APU's helper, `db_vmm_arena_alloc` sends the
request, the mapper lock and the background mapper are unchanged) and `ecalc.c` (`mapd_early()` at main start, helper entry on
`--mapd-helper`). The helper is this executable; one per APU, NUMA-pinned, socket pair with fd passing; a helper exits after its last chunk.
Failure (spawn, helper init, create or export, 60 s without a chunk, import) kills that helper, prints one `ECALC_MAPD:` line, and the rest of
that APU's chunks are created in-process. Plane pools stay on `hipMalloc`; not used with `ECALC_PE_PER_GPU`. Test knob `ECALC_MAPD_TEST_DIE=<dev>:<k>`.

- **Gates (measured, aac7, 1e9 digits, tools/gate.sh, ref e_1e9):** 1 node unset PASS, `ECALC_MAPD=1` PASS; 2 nodes unset PASS, `=1` PASS; all "digits identical",
  VERIFY OK. (Unset 1-node gate: item 41_s1_a g1_off.)
- **Init timeline, 2 nodes, 1.6e10 digits, `ECALC_INIT_TL=1`, nodes s1b0 + s5b0, interleaved off/on/off/on; single runs (measured), rank-0 node:**

| run | init total (s) | rns_init (s) | slowest foreground arena (s) | last background mapping done (tl s) |
|---|---|---|---|---|
| off 1 | 12.75 | 10.46 | 2.36 | 10.32 |
| on 1 | 9.22 | 6.86 | 1.17 | 6.73 |
| off 2 | 13.46 | 10.06 | 2.42 | 10.05 |
| on 2 | 9.83 | 7.46 | 1.03 | 7.42 |

  Init: off 13.1 s mean, on 9.5 s mean (-3.6 s, -28 %); arena mapping (foreground half, slowest APU): 2.4 s -> 1.1 s; mapping complete 10.2 -> 7.1 s on the rns_init clock.
  The main process waited 0.11-0.32 s per APU for helpers and spent 0.01-0.24 s importing. Arenas here are small (3-9 chunks per APU); the gain scales with
  the chunk count, so the 2e13 arenas (7-12 chunks in the background alone) should gain more (modeled, not measured).
- **Fallback (measured):** `ECALC_MAPD_TEST_DIE=2:0` at 1e9, 1 node: `ecalc: ECALC_MAPD: helper of APU 2 failed (helper exited) after 0 chunks: the remaining 1 chunks are created in-process`, rc 0, VERIFY OK,
  digits identical (gate PASS). `=1:3` at 1e10, 1 node: line "after 3 chunks: the remaining 2 chunks ..." and the run continued to the same stage as the unset arm.
  The 1e10 1-node runs (this and the unset arm) both exit with rc 1 in the memory phase (node MemFree 5.4 GB), a limit of the launch line at that size, not of the switch; so the mid-stream fallback has no
  digits check; the 1e9 case has one.
- **Not checked:** the gate's comparison at 1.6e10 against the 1e11 reference prints DIFFERS for the unset arm as well (also at 1e9 against that reference): the comparison does not accept a longer reference, so
  digits at 1.6e10 are not compared (VERIFY OK passed in all four runs). Memory headroom with the helpers' extra runtimes, 6-node and PPG layouts: not measured.
- **Phase 2 (plane pools through the same path), integrated below.**

## Integration, phase 2 (plane pools) and review fixes (2026-11-01)

Code: `mapd.c` serves a FIFO of (kind, count, size, final) requests: the plane pools' chunks first (`rns_init` sends the requests after its device-memory
measurement, in today's creation order: pool 0, pool 1, then the arena), then the arena. `mapd_pool_alloc` builds one VA range over the imported chunks
(`hipMemAddressReserve`, map, access for all APUs); `dpool.vmm` (set by `rns_init`) makes `dpool_malloc` in `mem.c` call it, through two function pointers
(`mem_vmm_pool_alloc`, `mem_vmm_pool_free`) that `rns_init` sets. Any failure kills that helper, logs one line, and the rest is created in-process; a pool
that cannot be built falls back to `hipMalloc`. Test knob `ECALC_MAPD_TEST_DIE=<dev>:<k>[:p]` (`p`: die while serving pool chunks). A log line
`ECALC_MAPD: plane pool of X GB on APU d built from N chunks of the helper` proves the path (it also appears when the chunks were made in-process after a helper failure).

- **Bug found in the first phase-2 build (fixed):** all pools silently fell back to `hipMalloc` (log: "pool chunks requested earlier were not taken") because `mem.c`
  tested weak references (`mapd_pool_alloc && db_pool_vmm_chunk`) that the toolchain resolved to null although `mapd.o` and `dbig.o` are linked. The weak symbols are gone;
  hooks are set at run time. Digits were correct throughout.
- **Review fixes:** requests are sent with `MSG_NOSIGNAL` and the helper ignores SIGPIPE; `reap()` is a bounded loop (kill after 0.5 s, abandon after 2 s with a log line);
  `mapd_ready()` waits (60 s) outside the mapper lock; a helper without a coming request is closed at the end of `rns_init` (`mapd_end_init`); the helper pins to the intersection of its NUMA node
  and `sched_getaffinity` and exits when its parent changes.
- **Helper footprint (measured, 1 node, 1e9, item 50_s1_a):** `APU d device memory in use before the contexts` is 31.3 MiB per APU, unset and `=1` alike: the helper's own context is not visible in
  `hipMemGetInfo` at that point (or is below its resolution).
- **Gate, 1 node, 1e9, unset vs `=1` (measured, item 54_s1_d):** PASS in both, result sha256 identical (`d1eb69d4...`), also with a helper dying during the pool chunks (`=1:0:p`: "helper of APU 1 failed ... the remaining 7 chunks are created in-process", PASS) and during the arena (`=2:0`, PASS).
  Plane pools done: 3.66 s unset, 1.34 s with `=1` (single runs, measured; tl clock).
- **Full-size check, aac7 (2026-11-02, measured, label "(m)"):**
  - 1 node, 1e10 digits, single runs: `ECALC_MAPD` unset vs `=1`, both rc 0, VERIFY OK, identical digit hash. ecalc total 49.8 s unset (m) vs 31.5 s with `=1` (m).
  - 2 nodes, 1.6e10 digits, two interleaved runs per arm (m): all four digit hashes identical (`296bdc45fb3ab6ee`, sha256 prefix of the digit file).
    - ecalc total: 53.13 and 50.94 s unset (m) vs 44.25 and 43.73 s with `=1` (m).
    - init total: 12.75-13.10 s unset (m) vs 6.56-6.67 s with `=1` (m).
    - plane pools done: about 4.9-5.5 s unset (m) vs about 2.4 s with `=1` (m).
    - region arenas (hook) done: 10.3-10.7 s unset (m) vs 4.1-4.3 s with `=1` (m).
  - Note: `tools/gate.sh` prints "digits DIFFERS" for sizes below the 1e11 reference file even when VERIFY is OK. The digit comparison here is by hash between the two arms (`GATE_SHA=1`), not against the reference.

## Known limits

- With the switch on, the helper creates the whole arena (both halves) at the `rns_init` request, whereas switch 0 maps the second half only after `db_vmm_bg_release`, so the seeds see less free memory early. Not tested at the edge size; `cache_avail` slot counts may change (digits unaffected).
- The helper keeps its own handle until it exits, so on a pool regrow while the helper lives, the memory returns only at helper exit.
- An arena requested after `rns_init` returns gets no helper (it is created in-process).
- Growth/restore under `g_pool_mx` can wait up to 60 s on a slow helper.
