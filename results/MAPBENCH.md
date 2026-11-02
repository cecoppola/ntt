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
- **Phase 2 (plane pools through the same path), not started:** `rns_mul.c rns_init` (the `da`/`db` dpool `hipMalloc`) to a VMM-backed dpool (contiguous VA over imported chunks, about 100 lines);
  `mapd.c` request protocol from one request per helper to a FIFO of (kind, count, size) requests so plane-pool chunks are created first, in today's order, then arena chunks (about 60 lines; the
  helper loop and `struct req` get a kind field); `dbig.c` consumer for the pool chunks and the `dev_used()` accounting in `rns_init` (measure tables before the helpers start); a call from `rns_init` that
  sends the pool request before staging. Estimated 250-300 lines, 1 day plus gates.
