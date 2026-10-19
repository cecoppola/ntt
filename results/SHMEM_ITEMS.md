# SHMEM_ITEMS — the SHMEM transport and proxy mode

Scope: the SHMEM transport and proxy mode: per-item records behind results/SHMEM.md and SHMEM_STALLS.md (N7 series, pool sizing, strided PE sets, target forms). | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| S | the SHMEM transport (M8-s), strided PE sets per tree level, the dragonfly third layer (2026-09-20) |
| S12 | the SHMEM transport's target forms, tested for real on Sandia OpenSHMEM (2026-09-21) |
| P214 | the SHMEM pool: measured, modelled, sized (PLAN §33 A2, TASKS 2.4, TARGET_TASKS T0) |
| V114 | the SHMEM pool by default, and the exchanges in rounds (Phase 14 agent V1) |
| N7 | SHMEM data path (`COMM_OFI=0`) against `comm_ofi`: sweep, diagnosis, peer-order fix, confirmation |
| MEAS-N7 | SHMEM data path (`COMM_OFI=0`) against libfabric (`COMM_OFI=1`) at 2 nodes |
| N7B | one SHMEM proxy PE per APU (`COMM_SHMEM_PE_PER_APU=1`): bandwidth, gates, 6-node ABBA |
| N7C | SHMEM proxy mode: why it is slow, and `COMM_SHMEM_PX_NUMA=1` + `HSA_ENABLE_SDMA=0` (config C) |
| N7D | SHMEM proxy mode toward comm_ofi speed at 6 nodes (copy kernel, parallel init, put-with-signal sends) |
| N7E | the N7D proxy-mode switches at 6 nodes: paired repeat (PX_SEND=2, PX_INIT=1) against comm_ofi |
| N7E_V | N7E-V — COMM_SHMEM_PE_PER_APU=2 (two helper PEs per APU): correctness record |
| N7G | COMM_SHMEM_PX_THP and COMM_SHMEM_PX_PIN on the proxy mode: pairs and verdict |
| SHM | the SHMEM pool under `COMM_OFI=0` is too small at 6 nodes (bug A of NIGHT) |

## S — the SHMEM transport (M8-s), strided PE sets per tree level, the dragonfly third layer (2026-09-20)

(former file: S.md)

Branch `s11` (from `main` @ 72aa2e9). PLAN.md §25–26 agent S.
Files: `ecalc/comm_shmem.c` (new), `comm.h` (additive), `mn.c` (communicator creation, `MN_TOPO_GROUP`), `mn.h` (one
accessor), `mnrun.sh`, `Makefile` (`SHMEM=1`), `tests/t_comm.c`, `tests/t_dist.c` (the `COMM_RANK` mode picks the SHMEM
communicator under `COMM_TRANSPORT=shmem`; two lines), `comm_layered.c` (the intra-minor form for the third layer —
outside my list; the major form's code paths are unchanged apart from `NA` → `p->na` and the device index).
`rns_dist.c`, `newton_db.c`, `ntt_dist.c` untouched. Size 1 untouched (nothing in the size-1 path calls the new code;
`shmem_init` is only called under `COMM_TRANSPORT=shmem`).

#### 1. The transport (`comm_shmem.c`)

Selected by `COMM_TRANSPORT=shmem` (default stays TCP). One PE per node-process; rank and size from
`shmem_my_pe() / shmem_n_pes()`; `COMM_RANK/SIZE/HOSTS/PORT` are not needed.

##### The OpenSHMEM subset used (1.4; nothing from 1.5)

| call | where |
|---|---|
| `shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided)`, `shmem_finalize`, `shmem_global_exit` | init / finalize (once per process, `mn_init` / `mn_finalize`) |
| `shmem_malloc`, `shmem_free`, `shmem_barrier_all` | the one symmetric pool at init (and the barrier after zeroing its mailbox); the only collectives of the library ever called |
| `shmem_ctx_create / destroy`, `SHMEM_CTX_DEFAULT` | one context per communicator (= per APU thread and level) when the calls run concurrently; the default context under the serial lock |
| `shmem_ctx_putmem_nbi` | every data transfer (slabs, blocks, ring chunks) |
| `shmem_ctx_long_p` | the signal, count, offset, value and ring-counter words |
| `shmem_ctx_fence` / `shmem_ctx_quiet` | ordering the data before its signal (see the OSHMEM trap below), local completion before the send staging is reused |
| `shmem_long_wait_until`, `shmem_long_test` | the waits (`wait_until` in concurrent mode; polled `test` under the lock in serial mode) |

Not used: `shmem_putmem_signal` / `shmem_put_signal` (1.5; the fence/quiet + `long_p` pair is the 1.4 equivalent — a
`#if SHMEM_MAJOR_VERSION >= 1 && SHMEM_MINOR_VERSION >= 5` switch to it is a two-line change in `push_all`), teams
(1.5; the strided PE set below is the shim), the library's reductions and all-gathers (process-level collectives cannot
be driven by four threads at once; every collective of `comm.h` is puts + signals on the communicator's own words).

##### Memory: one pool, receiver-chosen offsets

`shmem_malloc` is collective and the four APU threads create and use communicators independently, so nothing is
allocated collectively after init. One symmetric pool (`COMM_SHMEM_POOL_MB`, default 8192; `mnrun.sh` sets the OSHMEM
heap 512 MiB above it) holds everything remote PEs write:

* a mailbox table `[1024 ids][PEs]` of 8-byte words at its start (creation handshake);
* a per-PE first-fit allocator behind it for the communicators' control blocks and staging.

Symmetric addressing is by offset: a put to `pool + off` on PE r lands at PE r's `pool + off`, and `off` is always one
the **receiver** chose and published — its control block's offset at creation (through the mailbox), its receive
staging's offset per exchange (through the sender's `roff` word). So the offsets on different PEs need not agree, and
the allocators run locally (four threads: a mutex).

On aac6 (OSHMEM: host heap) the pool is `hipHostRegister`ed (portable) so the H2D/D2H copies between the APUs' slabs
and the staging are DMA (57 GB/s measured, the same as pinned memory). `COMM_SHMEM_DEVHEAP=1` (or
`-DCOMM_SHMEM_DEVICE_HEAP`) skips the registration and the copies become D2D (`hipMemcpyDefault` throughout) — the
switch for Cray / rocSHMEM device heaps; untested here (no such implementation on aac6), and it assumes the host can
read the control words in the device heap (rocSHMEM's host API: fine-grained memory).

##### The push model

A communicator's control block per member holds, per peer: `roff` (the receiver's published offset, tagged with the
exchange sequence: `seq << 40 | offset`), `sig` (the sender's signal = seq), `cnt` (the sender's byte count), two
parities of `val`/`vseq` (the tiny exchange), `prod`/`cons` (the point-to-point rings), and a ring per source
(`COMM_SHMEM_RING_KB`, 256).

* `alltoall(sb, rb, bytes, stream)`: my slabs D2H into the send staging (the self slab D2D into `rb`); I publish
  `roff[me] = (seq, my receive staging + r·bytes)` to every peer r; a helper thread then, per peer, waits for the peer's
  `roff`, `putmem_nbi` (staging → the peer's pool at that offset), then orders the data (quiet) and puts `cnt` and
  `sig`. The call returns after the staging copy — the slab pipeline's requirement (M7: the caller's GPU work and the
  layered communicator's xGMI stage of the next slab run under the transfer).
* `wait()`: join the helper, poll every `sig[r] ≥ seq`, check `cnt[r]`, `ctx_quiet` (my send staging is free), H2D the
  received slabs from the staging to `rb`. `inflight` = 1 (as TCP); the layered communicator's two-in-flight pipeline
  runs on top exactly as over TCP.
* `alltoallv`: the counts are known on both sides from the descriptors (B7); the receiver publishes its prefix offset
  per sender, the sender puts `scnt[r]` bytes and `cnt = scnt[r]`; the receiver aborts on `cnt ≠ rcnt[r]`.
* `allgather` / `allgather_host`: the same with one block to every peer (host blocks are put straight from the caller's
  buffer — a put's source may be private memory).
* `alltoallv_host`: as `alltoallv` from the caller's host buffers, complete on return.
* barrier, `allreduce_max`, `allreduce_modq`: a tiny exchange — every rank's 8-byte value to every rank (`val`, quiet,
  `vseq`), the reduction local (sum mod q as an all-gather of the values then the weighted sum, as TCP does). Parity
  double-buffering: a rank cannot get two tiny exchanges ahead of any other, so two slots suffice.
* `send`/`recv`: a byte stream per (source, dest) through the dest's ring; `prod[src]` at the dest counts bytes put,
  `cons[dest]` at the source bytes taken out. A message of at most a ring never blocks on the receiver (the carry
  flags in `rns_dist.c` send to all before receiving from any); larger ones (the host gather of `MN_COMBINE=host`,
  `mn_out`'s scatter stand-in) stream through it.

Sub-communicators = strided PE sets `{start + stride·r}` (`comm_shmem_create_at(start, stride, n, id)`), created by
their members together with a run-unique id (the mailbox row). `mn.c`: the four base meshes are ids 0–3 (PE set = all,
stride 1); a level's `all[d]` / `tr[d]` meshes are `NA + NA·slot + d` with the same slots the TCP ports used
(`2·level − 1`, `2·level`), the PE set `[g0, g0 + g)`; the third layer's in-group and cross-group meshes use lanes `d`
and `NA + d` of slot `2·level` (stride T for the cross-group set). `mn_group_at(level)` keeps its signature and its
group rule (`[k·2^l, (k+1)·2^l)`); L's `MN_GROUPS` schedule plugs in at the one place the group's `(g0, g)` is computed
(`mn_group_at`), since both transports build a group's meshes from `(g0, g)` alone.

##### Threads

`SHM_LOCK`: a no-op when the library provides `SHMEM_THREAD_MULTIPLE` and `COMM_SHMEM_SERIAL=0`; otherwise one
process-wide mutex around every library call, with the waits as polled `shmem_long_test` under short holds. **The
default is serial**: OSHMEM 4.1.6 returns `provided = MULTIPLE` but four threads in `shmem_long_wait_until` crash in
`spml_ucx_ctx_progress` (probe on aac6, results below). The target's Cray SHMEM has real MULTIPLE: run with
`COMM_SHMEM_SERIAL=0` there, which also gives each communicator its own context (under the lock the default context
serves; see the trap list). Cost of the lock on aac6: none measurable at these sizes (the puts are memcpys through
shared memory; the lock is held per put, not per wait).

##### What the target needs / what changes there

* `COMM_SHMEM_SERIAL=0` (contexts per APU thread, blocking waits).
* A device heap (`COMM_SHMEM_DEVHEAP=1`): the staging copies become D2D. The next step, not done here: the callers'
  slab buffers (`ntt_dist.c`'s `sbuf`/`rbuf`, `rns_dist.c`'s exchange buffers) allocated in the pool, so the pack
  kernels write the send slabs in place and the puts go slab → remote slab with no staging at all (an additive
  `comm_sym_alloc` in `comm.h`; every consumer already takes its buffers from a pool).
* `shmem_put_signal` where 1.5 exists (one call instead of quiet + put; frees the helper thread from the fence).
* Launch: `srun --mpi=pmix` is what `mnrun.sh` does already (the `setarch -L` wrapper and the OSHMEM MCA variables are
  harmless elsewhere).
* Chunked staging (a ring per peer instead of a whole-exchange staging) if pool memory matters: at 4 × 10¹⁰ over
  two nodes the largest exchange is ~1.1 GB per APU thread each way, four threads → the 8 GiB default; the pool is
  a run parameter.

#### 2. `mnrun.sh`

`COMM_TRANSPORT=shmem`: the same placement (the largest node count dividing `<procs>`, block distribution), launched
by `srun --mpi=pmix` (OSHMEM's PMIx client; direct launch works, `oshrun` is not needed — and not on the login node),
each process wrapped in `setarch x86_64 -L`, with `SHMEM_SYMMETRIC_HEAP_SIZE = COMM_SHMEM_POOL_MB + 512 MiB` and
`OMPI_MCA_memheap_base_max_segments=64`. TCP unchanged, except that the random port base is now drawn from
20000–26000: 20000–50000 reached into the ephemeral range (32768–60999) and a listener collided with an outgoing
connection's port once in ~4 runs at 8 processes (`bind: Address already in use`, one process exits, the rest hang).

Build: `make` detects `oshcc` (`SHMEM=1`; `-DCOMM_SHMEM` + `oshcc --showme:compile`, link `oshcc --showme:link
-lopen-rte -lopen-pal` — hipcc's lld does not follow liboshmem's own dependencies); `make SHMEM=0` builds without it
(the entry points abort with a message). `tests/t_comm` is host-only as before, now with `comm_shmem.c` in it.

#### 3. The dragonfly third layer (`MN_TOPO_GROUP=T`)

`comm_layered_create_minor(intra, inter, dev)`: the layered communicator of `comm_layered.c` generalised to any intra
size and to the intra-**minor** rank order `rho = na·r + d` (node `= T·a + b`: intra = the T nodes of dragonfly group
a, inter = the nodes with in-group index b across the groups). In that order the tree's node numbering is the
communicator's rank, so the minor comm serves as the inter transport of the outer (APU × node) comm unchanged: the
three-layer all-to-all is APU (xGMI) × node-in-group (one switch hop) × group (the global links), with one aggregated
message per peer group on the global links. Stages of the minor comm's equal all-to-all: a block gather of the slabs
bound for each in-group peer (strided in rank order) into the scratch, the intra exchange into `rb`, the transpose
into the scratch, the inter exchange into `rb` — which lands in rank order, so no final transpose. Its all-gather is
intra first (a group's slots are contiguous) then inter; `alltoallv` the same stages with the counts (the minor form
always packs the send slabs, since one intra peer's slabs are strided). `mn.c` builds `tr[d]` this way when
`MN_TOPO_GROUP > 1` divides the group's transform-node count `gt` and `gt > T` (else the plain mesh as before); the
two meshes are SHMEM strided sets or TCP meshes on the group's slot. The consumers (`rns_dist.c`, `mn_selftest_layered`)
see the same `comm` interface and rank.

#### 4. Tests (aac6, one node aac6-spx-a, jobs 20806 / 20812; the commands from `~/ntt-s/ecalc`)

`export COMM_TRANSPORT=shmem` throughout unless noted.

| what | command | result |
|---|---|---|
| `t_comm` 2, 4, 8 PEs (all-to-all 1 B – 3 MiB, all-gathers, alltoallv, max, sum mod q, point-to-point small + 3 MiB, then the strided sets of the even / odd PEs) | `SLURM_JOB_ID=$J ./mnrun.sh {2,4,8} ./tests/t_comm` | VERIFY OK on every PE at 2, 4, 8 |
| `t_dist 24`, one process per rank (mesh allgather, alltoallv, the distributed convolutions) | `./mnrun.sh 4 ./tests/t_dist 24` | VERIFY OK (54 checks) × 4 |
| `t_dist` layered at 2, 3, 4 (the layered self-tests, layered allgather + alltoallv, mesh allgather + alltoallv) | `./mnrun.sh {2,3,4} env DIST_LAYERED=1 ./tests/t_dist 24` | VERIFY OK (12 checks) on every node |
| `t_dist` layered at 8, plain vs three-layer | `./mnrun.sh 8 env DIST_LAYERED=1 MN_TOPO_GROUP={0,4} ./tests/t_dist 22` | VERIFY OK × 8 both ways; 5/5 repeats on shmem and on TCP (the layered self-test is against the one-rank engine: identical results) |
| `ecalc` 10⁸ at sizes 2, 3, 4 | `./mnrun.sh P env POOL_LOG=27 ./ecalc 100000000 /tmp/x.txt`; parts concatenated, `cmp` vs `ref/e_100000000.txt` | identical, all nodes VERIFY OK; totals 15.6 / 17.5 / 16.7 s |
| `ecalc` 10⁹ at sizes 2, 4 | `./mnrun.sh P env POOL_LOG=29 ./ecalc 1000000000 /tmp/x.txt`; `cmp` vs `ref/e_1000000000.txt` | identical, all nodes VERIFY OK; totals 43.4 / 42.9 s |
| the regression `mnaccept.sh` with `COMM_TRANSPORT=shmem` exported (it passes the environment through `--export=ALL`; no change needed) | `./mnaccept.sh $J` | see §6 (run in two halves for the batch limit) |
| 2–3 real nodes over shmem | | **not run**: the other two nodes were taken by agents L, M and V throughout (`sinfo` never showed two idle nodes while I had a batch); the launcher path is the same `srun --mpi=pmix -N nn` with the block placement |

Probes that shaped the design (`probe*.c`, not in the tree):

* OSHMEM `shmem_init` crashed in `mca_memheap_modex_recv_all` about half the time (any heap size, any UCX transport,
  `--mca memheap_base_key_exchange 0` worse). Cause: the "static" memheap registers every `rw-p` anonymous mapping
  below the executable's `_end` from `/proc/self/maps` (33 segments with the ROCm runtime loaded) and their count
  differs between PEs with the default top-down mmap layout; the modex assumes equal counts. `setarch x86_64 -L`
  (legacy bottom-up layout) gives 4 segments on every PE: 20/20 clean inits (plain and HIP binaries). OSHMEM 4.1.6 has
  no `memheap_base_static_include/exclude` parameter (that came later).
* `shmem_ctx_fence` in OSHMEM 4.1.6 does not order an nbi put before a later put to the same PE: at the signal the
  receiver saw 0.5–2.6 MB of a 3 MB slab (mode 0 of `probe3`); blocking `putmem` + fence and nbi + `quiet` both
  complete. The transport orders by `quiet` (`COMM_SHMEM_FENCE=1` selects fence for a conforming implementation).
* Four threads in `shmem_long_wait_until` → SIGSEGV in `spml_ucx_ctx_progress` despite `provided = MULTIPLE`; the same
  under a mutex with polled `shmem_long_test`: clean. Hence the serial default.
* A context created after another context was destroyed lost puts (the strided-set test after the full set): under
  the lock the default context is used; with `COMM_SHMEM_SERIAL=0` contexts are per communicator (untested on a
  library where MULTIPLE works).
* Direct launch `srun --mpi=pmix` works with OSHMEM (pmi2 does not: "not built with SLURM's PMI support").

#### 5. Traps found (for whoever touches this next)

1. The OSHMEM three above (`setarch -L`, quiet-not-fence, serial calls). All three are runtime switches or launcher
   wrappers; nothing in the code depends on OSHMEM.
2. `hipcc` + `oshcc --showme:link` needs `-lopen-rte -lopen-pal` added (lld).
3. The TCP port base must stay below 32768 (above: intermittent `bind` failures).
4. A communicator id may be used once per run (the mailbox row is never cleared; creation aborts on reuse) — `mn.c`'s
   ids are per (slot, lane) and every group is created once.

#### 6. The regression over SHMEM

`COMM_TRANSPORT=shmem ./mnaccept.sh 20817 --only e9,mn` and `--only unit,ckpt` (two halves for the 45-minute batch), at
8441d38: **16 passed, 0 failed** — unit (t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid
at 2 node-processes over SHMEM), e9 size 1 both bases identical (15.7 s / 25.7 s), mn 10⁸ at 2, 3, 4 and 10⁹ at 2, 4
identical with every node VERIFY OK (22.4 / 19.3 / 18.5 s; 43.9 / 43.8 s), ckpt 10⁸ at size 2 (abort at leaf level 6,
restart identical). `mnaccept.sh` passes the environment through (`srun --export=ALL`): no change to it. Logs in
`results/mnaccept/20817` of `~/ntt-s` on aac6. (An earlier pass at d8949bf had e9 size 1 failing: `mn_init` called
`shmem_init` for a single process with no launcher; fixed in 8441d38 — a single process stays size 1 as under TCP.)

#### 7. Open issues

* 2–3 real nodes over SHMEM not exercised (node availability); the only node-count-dependent code is the placement
  in `mnrun.sh`, shared with TCP, and OSHMEM's inter-node UCX/TCP path. First thing for the integrator when two nodes
  are idle: `./mnrun.sh 2 ./tests/t_comm` and the 5 × 10⁹ two-node run.
* Staging: the transport copies every slab through the pool (as TCP through pinned buffers). On the target the
  callers' slabs should live in the pool (see §1); pool memory is a run parameter until then.
* `COMM_SHMEM_SERIAL=0` and the device heap are code paths with no implementation to test them on here.
* L's `MN_GROUPS` schedule: `mn_group_at` still uses `2^level`; the group's `(g0, g)` is the only thing the SHMEM
  meshes need, so L's parser plugs in at that one line.

## S12 — the SHMEM transport's target forms, tested for real on Sandia OpenSHMEM (2026-09-21)

(former file: S12.md)

Branch `s12` (from `main` @ 7aded87). PLAN.md §27 row S: DECISIONS2 #4 (the target forms), #5 (the third layer), #9
(two real nodes). Files: `ecalc/comm_shmem.c`, `comm.h` (additive: two ops and two inlines, one query), `Makefile`
(`SHMEM_HOME=`), `mnrun.sh` (the SOS launch), `ntt_dist.c` (the slab allocation only), `tests/t_comm.c`; outside my
list, minimal and commented: `comm_layered.c` (its scratch from the inter transport's pool, the `sym_alloc` forwarding,
two `hipMemcpyDeviceToDevice` → `Default`). `mn.c` unchanged (nothing in the communicator creation needed to change).
Size 1 untouched.

#### 1. Sandia OpenSHMEM on aac6 (`~/sos`, `~/libfabric`, sources in `~/sos-src`, `~/libfabric-src`)

The login node has outbound internet (the compute nodes do not): cloned there. libfabric 1.20.1 (tag v1.20.1, the
tarball's version string says 1.23.1) built with the tcp, sockets, shm, rxm, udp, rxd providers into `~/libfabric`
(the system libfabric has no headers). SOS 1.5.3 (git main @ 4aa91d7) configured

    ./configure --prefix=$HOME/sos --with-ofi=$HOME/libfabric --enable-pmi-simple --enable-ofi-mr=basic CPPFLAGS=-DUSE_EXT_HEAP_HOST

after the external-heap patch below (`~/sos-src/sos_patch.py`, applied once; `git diff` in `~/sos-src` shows it).
What was learnt getting it to run (`~/sostest/hello.c`: thread-multiple init, a context, four threads in
`shmem_long_wait_until` at once):

* **Launch**: `srun --mpi=pmi2` with SOS's simple PMI-1 client (Slurm's pmi2 plugin speaks PMI-1 too); no `oshrun`, no
  `--with-pmi` (no Slurm PMI / PMIx headers on the nodes). `mnrun.sh` does this for a binary linked against `libsma`.
* **Provider**: `FI_PROVIDER=sockets`. The `tcp` provider (alone or `tcp;ofi_rxm`) is rejected by SOS's fabric query
  ("did not find any valid fabric services") — it lacks the atomics / counters SOS asks for. Sockets is a
  correctness-only provider (a progress thread per process); fine for the purpose.
* `--enable-remote-virtual-addressing` requires ASLR off (`setarch -R`) — dropped; the default (address translation
  through per-PE tables) needs nothing.
* SOS provides `SHMEM_THREAD_MULTIPLE` for real: four threads in `wait_until` concurrently, contexts created and
  destroyed freely, `shmem_putmem_signal_nbi` present — the three things OSHMEM 4.1.6 lacked (results/SHMEM_ITEMS.md §S).
* **The device heap**: SOS has an "external heap" API (`shmemx_heap_preinit_thread` / `shmemx_heap_create(base, size,
  device_type, index)` / `shmemx_heap_postinit`) meant for ZE and CUDA buffers under `USE_FI_HMEM`, which the sockets
  provider cannot serve (`FI_HMEM` capability). The patch adds `SHMEMX_EXTERNAL_HEAP_HOST` (= 2): the external heap is
  registered as ordinary system memory (`FI_HMEM_SYSTEM`, no `FI_HMEM` caps), compiled in with `-DUSE_EXT_HEAP_HOST`;
  it needs `--enable-ofi-mr=basic` (the default "scalable" MR mode has no external-heap path at all: `get_mr` aborts
  with "address outside of symmetric areas"). With it a HIP **fine-grained device buffer** (`hipExtMallocWithFlags`,
  `hipDeviceMallocFinegrained`; or `hipMallocManaged` with `COMM_SHMEM_DEVHEAP=2`) is the symmetric heap: the CPU
  side of the sockets provider reads and writes it directly (fine-grained = host-accessible on the APU), the kernels
  of all four APUs write the slabs into it (peer access enabled at init). That is the target's form (Cray / rocSHMEM:
  `shmem_malloc` returns device memory) exercised end to end.

#### 2. The transport's target forms (`comm_shmem.c`)

| switch | Phase 11 (OSHMEM) | now, on SOS | on the target |
|---|---|---|---|
| `COMM_SHMEM_SERIAL` | 1 (forced: OSHMEM crashes) | **0 by default** when the library provides MULTIPLE (OSHMEM stays 1: `-DCOMM_SHMEM_OSHMEM` from the Makefile): a context per communicator = per APU thread and level, `shmem_wait_until` from four threads, no lock | 0 |
| `COMM_SHMEM_ORDER` | quiet + `long_p` (helper thread) | **`putsig` by default on 1.5 headers**: `shmem_ctx_putmem_signal_nbi`, data and signal in one call, nothing blocks; `fence` (1.4 spec form: put, `ctx_fence`, `long_p`); `quiet` (the OSHMEM form) | putsig |
| the puts | a helper thread per exchange (the roff wait, the put, the blocking quiet) | **inline** under putsig / fence: the caller's thread polls each peer's `roff` for at most `COMM_SHMEM_SPIN_US` (2000) and posts the put; a helper thread only for the peers not ready in time (in a lockstep pipeline: none). `COMM_SHMEM_THREAD=1` forces the thread; quiet always uses it | inline |
| `COMM_SHMEM_DEVHEAP` | code path only | **1 = the pool is a HIP device buffer** registered as SOS's external heap (2 = managed); 0 = `shmem_malloc` host memory, `hipHostRegister`ed (the same HBM on the APU) | 1 (Cray/rocSHMEM: no registration, the library's heap is device memory: the pre-existing path) |
| `comm_sym_alloc` | — | **the callers' buffers in the pool**: an exchange whose send / receive buffer lies in the pool is put from / into directly — no staging, no copy. `ntt_dist`'s slab buffers (`dist_plan_create`) and the layered communicator's scratch come from it; `DIST_SYM_SLABS=0` / `COMM_SHMEM_NOSYM=1` restore the staged form | on |
| staging | per communicator, freed at destroy | **per exchange, released at completion** (Q's trap 11: the level meshes live to the end of the run); `COMM_SHMEM_KEEP_STAGING=1` restores Phase 11's form | released |

The signal word carries the byte count: `sig = seq << 40 | count` (the separate `cnt` word is gone; one put-with-signal
delivers both). Sequences are bounded at 2^23 per communicator (aborts beyond; a run uses thousands).

The one bug the target form exposed (found by `ecalc` 10^8 at size 4 on SOS, meshes 1 and 2 wrong in every point, the
staged form and every other switch fine): with a pool-resident receive buffer the receiver must not publish its
offsets before **its own stream** is done with that buffer — the previous transform's unpack kernel may still be
reading `rb` when the host reaches the next `alltoall` (the GPU lags the host by whole passes). Fixed: the stream
sync precedes the publication (the staged form was immune: its receive area was the transport's own).

##### The OpenSHMEM calls used (the portability contract for Cray OpenSHMEMX / rocSHMEM)

| call | spec | use |
|---|---|---|
| `shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided)`, `shmem_finalize`, `shmem_global_exit` | 1.4 | once per process |
| `shmem_my_pe`, `shmem_n_pes` | 1.0 | rank, size |
| `shmem_malloc`, `shmem_free` | 1.0 | the one pool at init (unless the external-heap form) |
| `shmem_barrier_all` | 1.0 | after the pool's mailbox is zeroed; before finalize. The only collective |
| `shmem_ctx_create(0, &ctx)`, `shmem_ctx_destroy`, `SHMEM_CTX_DEFAULT` | 1.4 | a shareable context per communicator (serial mode: the default context) |
| `shmem_ctx_putmem_nbi` | 1.4 | every data transfer (slabs, blocks, ring chunks) |
| `shmem_ctx_long_p` | 1.4 | the control words: roff, val, vseq, prod, cons, the mailbox, and the signal under fence / quiet |
| `shmem_ctx_putmem_signal_nbi(..., SHMEM_SIGNAL_SET, pe)` | **1.5** | the slab and its signal (`COMM_SHMEM_ORDER=putsig`, the default on 1.5 headers; `#if` on `SHMEM_MAJOR/MINOR_VERSION`) |
| `shmem_ctx_fence` | 1.4 | `COMM_SHMEM_ORDER=fence`: the data before its signal; the tiny exchange's value before its sequence; a ring chunk before its counter |
| `shmem_ctx_quiet` | 1.4 | `COMM_SHMEM_ORDER=quiet` (OSHMEM); the receiver's local completion at wait; before a context is destroyed |
| `shmem_long_wait_until(p, SHMEM_CMP_GE / NE, v)` | 1.4 | every wait in concurrent mode |
| `shmem_long_test` | 1.4 | the polled waits in serial mode; the bounded roff poll of the inline push |
| `shmemx_heap_preinit_thread`, `shmemx_heap_create`, `shmemx_heap_postinit` | SOS extension | only under `-DCOMM_SHMEM_SOS` with the patched header (`SHMEMX_EXTERNAL_HEAP_HOST`); the target's device heap is `shmem_malloc`'s |

Not used: teams, reductions, collectives other than the two barriers, `shmem_ptr`, atomics. rocSHMEM (host API
subset) and Cray OpenSHMEMX both have every 1.4 call above; put-with-signal is 1.5 (Cray: yes; rocSHMEM: `putmem_signal`
exists on the device API, the host API is to be checked — `COMM_SHMEM_ORDER=fence` is the fallback there, one switch).

#### 3. Build and launch

* `make SHMEM_HOME=$HOME/sos` (the Makefile: `-DCOMM_SHMEM -DCOMM_SHMEM_SOS -I$SHMEM_HOME/include`, links `-lsma`
  with an rpath; `make clean` between an OSHMEM and a SOS build — two clones are simpler: `~/ntt-s12` (SOS, built on
  the login node under `module load rocm`) and `~/ntt-s12-osh` (OSHMEM: `oshcc` exists only on the compute nodes, so
  `srun --jobid=$J bash -lc "module load rocm; make"`).
* `mnrun.sh`: with `COMM_TRANSPORT=shmem` it looks at the command's binary (`ldd | grep libsma`; `COMM_SHMEM_IMPL=sos|oshmem`
  overrides): SOS → `srun --mpi=pmi2`, `FI_PROVIDER=sockets`, `SHMEM_SYMMETRIC_SIZE` = pool + 512 MiB (64 MiB with
  `COMM_SHMEM_DEVHEAP`, when the pool is the external heap); OSHMEM → as before (`--mpi=pmix`, `setarch -L`).
* The mailbox table is sized before init from `SLURM_NTASKS` / `PMI_SIZE` / `COMM_SIZE` (4096 PEs without them: 32
  MiB) because the external heap must exist before `shmem_init`.

#### 4. Tests (aac6, one node aac6-spx-b, job 20872; `export COMM_TRANSPORT=shmem`, commands from the clone's `ecalc/`)

`t_comm` gained rounds on symmetric buffers (send only, receive only, both; twice in a row on the same buffers;
`alltoallv` and the all-gathers on them). Host-only, built against SOS on the login node
(`gcc -DCOMM_HOST_ONLY -DCOMM_SHMEM -DCOMM_SHMEM_SOS ...`, `./t_comm_sos`).

| what | command | result |
|---|---|---|
| `t_comm` SOS, the target form (concurrent contexts, putsig, inline, symmetric rounds) | `./mnrun.sh {2,4,8} ./t_comm_sos` | VERIFY OK on every PE at 2, 4, 8 |
| `t_comm` SOS, every other mode at 8 PEs | `./mnrun.sh 8 env X ./t_comm_sos` with X = `COMM_SHMEM_ORDER=fence`, `=quiet`, `COMM_SHMEM_THREAD=1`, `COMM_SHMEM_SERIAL=1`, `SERIAL=1 ORDER=quiet`, `SERIAL=1 ORDER=fence`, `COMM_SHMEM_SPIN_US=0`, `COMM_SHMEM_NOSYM=1` | 8/8 VERIFY OK in each |
| `t_comm` OSHMEM (`~/ntt-s12-osh`, serial + quiet default) | `./mnrun.sh {2,4,8} ./tests/t_comm` | VERIFY OK × 2, 4, 8 |
| `t_comm` OSHMEM with `COMM_SHMEM_ORDER=fence` | | **VERIFY FAILED on every PE** (millions of bad words) — the Phase 11 trap reconfirmed by the test: OSHMEM 4.1's fence does not order an nbi put; the quiet default stands there |
| `t_dist 24`, one process per rank, SOS, pool-resident slabs (default) and staged (`DIST_SYM_SLABS=0`) | `./mnrun.sh 4 [env DIST_SYM_SLABS=0] ./tests/t_dist 24` | VERIFY OK (54 checks) × 4, both |
| `t_dist 24`, SOS, **device heap** (`COMM_SHMEM_DEVHEAP=1`: the pool a fine-grained HIP buffer) | `./mnrun.sh 4 env COMM_SHMEM_DEVHEAP=1 ./tests/t_dist 24` | VERIFY OK (54) × 4 |
| `t_dist` layered at 2, 3, 4, SOS (host heap) | `./mnrun.sh {2,3,4} env DIST_LAYERED=1 ./tests/t_dist 24` | VERIFY OK (12) on every node |
| `t_dist` layered at 2, 4, SOS device heap | `... env COMM_SHMEM_DEVHEAP=1 DIST_LAYERED=1 ...` | VERIFY OK (12) on every node |
| `t_dist` layered at 8, plain and three-layer, SOS and OSHMEM | `./mnrun.sh 8 env DIST_LAYERED=1 MN_TOPO_GROUP={0,4} ./tests/t_dist 22` | VERIFY OK × 8 in all four (OSHMEM T=4 needs the default 8 GiB pool: 2 GiB ran out — the third layer's inner scratches) |
| `t_dist 24` and layered 2, 3, 4 on OSHMEM | as above in `~/ntt-s12-osh` | VERIFY OK (54 × 4; 12 × 2, 3, 4) |
| `ecalc` 10^8 at sizes 2, 3, 4, SOS host heap | `./mnrun.sh P env POOL_LOG=27 ./ecalc 100000000 ~/s12_e8.txt` | VERIFY OK all nodes; 10.1 / 12.9 / 11.5 s (size 4 after the fix: 3/3 runs) |
| `ecalc` 10^8 at 2, 3, 4, SOS **device heap** | `... env POOL_LOG=27 COMM_SHMEM_DEVHEAP=1 ...`, parts concatenated, `cmp` vs `ref/e_100000000.txt` | **identical** ×3; 9.0 / 10.3 / 11.3 s; VmHWM 17.4 GB (26 GB with the host pool: the 8 GiB pool is host memory there) |
| `ecalc` 10^9 at 2, 4, SOS host heap and device heap | `./mnrun.sh P env POOL_LOG=29 [COMM_SHMEM_DEVHEAP=1] ./ecalc 1000000000 ~/s12_e9.txt`, `cmp` vs `ref/e_1000000000.txt` | **identical** in all four; 39.9 / 41.3 s (host), 37.8 / 41.4 s (device) |
| `ecalc` 10^8 at 8 (SOS) | `./mnrun.sh 8 env POOL_LOG=27 ./ecalc 100000000 ...` | VERIFY OK × 8 |

#### 5. The symmetric memory per PE (for Q's model)

`comm_shmem` now prints at finalize the pool's peak by kind (control blocks, staging, the callers' symmetric buffers).
Measured on PE 0 (SOS, one node):

| run | form | control | staging peak | symmetric buffers peak | pool peak |
|---|---|---|---|---|---|
| 10^8, size 4 | default (staging released per exchange, sym slabs in the self-tests) | 6 MiB | 42 | 64 | 68 MiB |
| 10^8, size 8 | default | 14 | 21 | 32 | 40 |
| 10^9, size 4 | default | 6 | 423 | 64 | 429 |
| 10^9, size 4 | `COMM_SHMEM_NOSYM=1` (all staged, released) | 6 | 423 | 0 | 429 |
| 10^9, size 4 | `NOSYM=1 KEEP_STAGING=1` (Phase 11's form) | 6 | 627 | 0 | 633 |

Reading: (i) the control blocks are `8 words × n + n × 256 KiB ring` per communicator (the ring dominates: 256 KiB × n
per mesh; at 576 nodes and ~10 meshes per level that is a few hundred MiB — `COMM_SHMEM_RING_KB` is the knob); (ii)
released staging makes the staging term one exchange's worth per concurrently active mesh, not the sum over the
levels (627 → 423 MiB at 10^9/4; the difference grows with the level count); (iii) **the tree's exchanges in `ecalc`
are still staged**: `comm_sym_alloc` covers `ntt_dist`'s own slabs (`dist_plan_create`, the self-tests) and the layered
communicator's own scratch, but `rns_dist.c` (G's file) hands the layered communicator its slabs from pool 1
(`dist_plan_create_shared(..., sl, sl + q)`) and its scratch from the block pool (`comm_layered_scratch(cl, tmp, q * 8)`).
The one change that removes the staging term from the run: in `rns_mul_dist_mn`, `sl = comm_sym_alloc(cl, 2 q × 8)`
(falling back to pool 1 when it returns 0) and `tmp = comm_sym_alloc(cl, q × 8)` — then the staging column is 0 and
the symmetric-buffer column is 3 q × 8 per APU thread, i.e. exactly the bytes pool 1 holds today (they move from pool 1
into the symmetric pool: the per-node total is unchanged, and `COMM_SHMEM_POOL_MB` must hold 4 × 3 q × 8 = 12 q × 8:
48 GiB at q = 2^29 (the 2^31-point planes), which is why the pool should then be the device heap — on the APU the pool
is HBM either way). Until then the model's term for the transport is: staging = the largest concurrent exchange per
APU thread (≈ 2 q × 8 send + receive, released after each), plus the rings.

#### 6. The third layer (`MN_TOPO_GROUP`) — kept, off by default

`t_dist 26` layered at 8 PEs (the 2^26 layered self-test: 8 × 4 ranks, one node, the reference transform included in
the time; both transports in the serial-vs-target forms they run in):

| transport | `MN_TOPO_GROUP=0` (two layers) | `MN_TOPO_GROUP=4` (three layers) |
|---|---|---|
| SOS (target form) | 2.20–2.22 s | 2.55–2.58 s |
| OSHMEM (serial) | 2.13–2.18 s | 2.58–2.61 s |

On one node the third layer costs 16–20 % (its two extra block transposes and the second exchange stage, with no
global links to save): consistent with X's model that it does not pay at 576 on bandwidth grounds; it stays behind its
switch for the case the target's per-message cost turns out 10× the assumption. Both forms verify on both transports.

#### 7. The regression over SOS (job 20883, aac6-spx-c, `~/ntt-s12` @ 3e2cec2)

`COMM_TRANSPORT=shmem ./mnaccept.sh 20883` (the SOS build; `mnrun.sh` picks `--mpi=pmi2` + sockets from the binary):
**16 passed, 0 failed, 1607 s** — unit (t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid at
2 node-processes over SOS: 200 checks × 2), e9 size 1 both bases identical (14.5 / 25.4 s), mn 10^8 at 2, 3, 4 and
10^9 at 2, 4 identical with every node VERIFY OK (10.5 / 12.4 / 11.3 s; 43.6 / 39.2 s), ckpt 10^8 at size 2 (abort
at leaf level 6, restart identical). `--full` not run (the batch had 17 min left). Then the `mn` step again with
`COMM_SHMEM_DEVHEAP=1` (the pool a HIP device buffer, SOS's external heap): **5 passed, 0 failed** (9.8 / 11.8 / 10.7 s;
38.4 / 40.0 s, all identical). Logs in `~/ntt-s12/ecalc/results/mnaccept/20883` on aac6.

#### 8. What is not done / open

* **Two real nodes over SHMEM**: not run — never two idle nodes at once during the session (G, I, R, W and two other
  users' jobs); polled for ten minutes after my last batch. The inter-node path is SOS's sockets provider over the 1 GbE
  (the same code, `srun -N2` placement by `mnrun.sh`); first thing when two nodes are idle:
  `sbatch -N2 ...; ./mnrun.sh 2 ./t_comm_sos; ./mnrun.sh 2 env POOL_LOG=29 ./ecalc 1000000000 /tmp/x.txt`.
* `rns_dist.c`'s slabs in the pool (G's file): the one-line change above; the integrator's call.
* The pool size: `COMM_SHMEM_POOL_MB` stays a run parameter (8192 default); Q's `estimate.py` should size it as
  4 × (3 q × 8) once the slabs move, else 4 × 2 q × 8 + rings.
* rocSHMEM's host API for put-with-signal: unverified (fence fallback).

## P214 — the SHMEM pool: measured, modelled, sized (PLAN §33 A2, TASKS 2.4, TARGET_TASKS T0)

(former file: P214.md)

Agent P2, Phase 14. Branch `p14-P2` from `main` @ a839418. aac6 clones: `~/ntt-P214` (SOS build; 74af766 for b1, 27ff2d9
for b4), `~/ntt-P214b` (SOS; 12d8724 for b2, 19a0815 for b5), `~/ntt-P214c` (SOS; 0128067 for b3), `~/ntt-P214t` (the plain
build, 8b6955e = the final code, for the regression). Batches `~/P214/b<N>.sh`, logs `~/P214/b<N>/*.log` and `summary.txt`; the target's plan
`~/P214/plan576.txt`. Nodes: s24-26 + s24-30 for every two-node job (SOS over the 1 GbE, one PE per node, `COMM_SHMEM_DEVHEAP=1`
unless marked), s24-16 for the four-process runs. Every run below is one run; every digit check is `cat parts | cmp - ref`.

#### RESUME

- **Done.** The measurements (§1), the law (§2: `mem_model.py` / `estimate.py` / `mn_model.py`, and in C), TASKS 2.4 measured
  and kept off (§3), the pool rule (§4), the gates (§5: all green), docs (TARGET.md §4, the switch table, step 6, trap 11;
  TARGET_TASKS T0; README rows; `results/DESIGN_TABLE.md` regenerated). No job of P2 running; every job cancelled by 10:25 CDT.
- Final commit 7d377c4 (built on aac6 in `~/ntt-P214t`, `MN_PLAN_ONLY` lines checked against `mem_model.py --pool`). The
  regression ran on 8b6955e; the two commits after it change only the pool rule's shift term (C and Python, printed sizing).
- Open: the user's decision on `MN_T_CHUNK_MB=1024` for the target (§2, §7); a transport that stages in rounds or
  pool-resident exchange buffers would remove the second copy (§2).

#### 1. Measurements: the pool's high-water and what fills it

The transport already printed the pool's peak by kind at finalize (PE 0). Added (commits 74af766, 12d8724, 0128067, and the site key fix): what occupied the
pool **at** its peak (the kinds then, the live staging blocks, the largest staging block, the mailbox), every PE's line under
`COMM_SHMEM_VERBOSE=1` (or `ECALC_VERBOSE>=2`), a line per new staging peak naming the exchange, and under `COMM_SHMEM_VERBOSE=2`
every staged exchange by call site (count, largest send / receive; resolved with `llvm-symbolizer --obj=ecalc 0x200000+offset`).

| run | batch / job | digits | pool peak (MiB) | at the peak | largest staged exchange per APU thread (send + recv MiB) and its caller | wall |
|---|---|---|---|---|---|---|
| 2 real nodes, SOS | b1a / 21269 | 10⁸ `POOL_LOG=27` identical | 130 (PE 0 and 1) | self-tests' symmetric buffers 128 + control 2 (at init); staging peak 84.8 = 8 blocks | 10.6 + 10.6, result exchange | 25.15 s |
| 2 real nodes, SOS | b1a / 21269 | 10⁹ `POOL_LOG=29` identical | 849 (both) | staging 847.7 (8 blocks) + control 2 | 106.0 + 106.0, `mn_core`'s result exchange (b2: `rns_dist.c` `comm_alltoallv(G->all[d], xb, …, rbO, …)`) | 171.95 s |
| 2 real nodes, SOS | S13d / 21104 | 10¹⁰ (default POOL_LOG) identical | 8479 | staging 8477 | (2 × 1059.6, the same exchange by the law) | 2070.7 s |
| 4 processes, one node, SOS host heap | b1b / 21271 | 10⁹ `POOL_LOG=29` identical | 429 (all four) | staging 423.9 (8 blocks) + control 6 | 53.0 + 53.0 | 30.63 s |
| 4 processes, one node, SOS host heap | b1b / 21271 | 10¹⁰ `POOL_LOG=29` identical | 4244 (all four) | staging 4238.6 (8 blocks) + control 6 | 529.8 + 529.8 | 283.26 s |
| 2 real nodes, `DIST_LOGN_TEST=25` (the division in 2 × 2 pieces) | b4 / 21276 | 10⁹ identical | PE 0 531, **PE 1 637** | PE 1: staging 635.8 | PE 1: result exchange 53.0 + 106.0 (my rows of a piece + my share of C inside it); PE 0: 53.0 + 79.5 (the reciprocal's Q_t r) | 197.01 s |
| 2 real nodes, SOS, `COMM_SHMEM_POOL_AUTO=1` (pool 8960) | b5 / 21279 | 10¹⁰ identical | 8479 (both) | staging 8477.1 (8 blocks) | 1059.6 + 1059.7, the result exchange (96 exchanges); `mdb_shift` 1059.6 + 529.8 | 2035.97 s |
| 2 real nodes, `MN_T_CHUNK_MB=64` | b4 / 21276 | 10⁹ identical | PE 0 531, **PE 1 637** | PE 1: staging 635.8 | PE 1: `mdb_shift` (newton_db.c) 106.0 + 53.0 (the division's t, 2 n_Q limbs, → X); the result exchange now 64 + 64 | 170.77 s |

The staged sites at 10⁹ on 2 nodes (b3 `sos_e9_sites`, PE 0; send / receive MiB per APU thread, largest): result exchange
106 / 106 (392 exchanges); redistribution of A and of B 53 / 53 (96 each), of X 0 / 27.5; the transform's inter-node stage
(`comm_layered` inter → `ntt_dist` chunks) 32 / 32 (3456); the spills < 1; the self-tests 4 / 0. At 10⁸: 10.6, 5.3, 2.8, 4.0.

**What fills the pool**: only staging (the control blocks are 2 MiB at 2 PEs, 6 at 4; the mailbox 0.02 MiB; the self-tests'
128 MiB of symmetric buffers at init, released before the tree). The staging is per exchange and released after it
(Phase 12), so the peak is **4 APU threads × (send + receive) of the single largest exchange** — 8 blocks at every peak. Every
exchange of `ecalc` is staged: `rns_dist.c`'s buffers (xb in plane pool 1, rbO / the redistribution's sb, rb / the shifts'
sb, rb in the block pool) are not in the symmetric pool.

#### 2. The law (modelled from the code's buffers; measured to 0.3 MiB at all eight points)

Per node-process, bytes (`mem_model.shmem_pool`, and the same in C: `binsplit_shmem_pool_need` + `rns_mul_dist_mn_stage`):

    pool = 4 x 8 x max over the run's mn_core products and the division's shift of the staged send + receive per APU thread
           + control (4 x (g + sum of the lower levels' sizes) x (COMM_SHMEM_RING_KB + 64 B)) + mailbox (1024 x g x 8) + 256 MiB margin
    per product (pieces at the group's cap, mn_scratch's terms):
      result exchange  = RT(nc_piece)  [my rows of the piece]  +  min(share_c, nc_piece) / 4  [my share of C inside it]   (MN_T_CHUNK_MB=m: each <= m MiB)
      redistribution   = RT(pa) + min(share_a, pa) / 4 + 2 g rows   (the same for B)
      transform        = 2 q / 4
    the division's mdb_shift = (2 n_Q / g / 4 + n_Q / g / 4) / K, K = ceil(the shorter side / MDB_SHIFT_CHUNK_MB) (newton_db.c's rounds; 1024 by default)

Below the cap (one piece) the division's `A_h mu` (n_Q × n_Q, C of 2 n_Q limbs) sets it: **pool ≈ 32 n_Q / g bytes ≈ 1.78 bytes
per digit per node** — linear in the per-node share (×10 per decade: 84.8 → 847.7 → 8477 MiB at 2 nodes), halved at 4 processes
(same total). At the cap the rows stop growing but a quarter of **my share of C** does not: at 576 the result exchange of
`A_h mu` receives 2.05 × 10⁹ limbs per APU (16.4 GB) and sends 4.6 × 10⁸ (3.7 GB).

`python3 mem_model.py --pool` prints the check and the target table (all modelled from the law):

| 4.25 × 10¹³ on 576 nodes, `MN_GROUPS=2,4,8,16,32,64,192,576` | pool (GB) | set by | node total (GB) | 480 GB |
|---|---|---|---|---|
| the defaults (as TARGET.md §4) | **81.6** (COMM_SHMEM_POOL_MB=77824) | `A_h mu` result exchange, 4 × 20.1 GB | **524.7** | **exceeds** (also 502) |
| + `DM_TIGHT=1` | 81.6 | the same | 501.2 | exceeds |
| + `MN_T_CHUNK_MB=1024` | **45.0** (43008) | the top level's redistribution, 4 × 10.9 GB | **459.6** | fits |
| + `MN_T_CHUNK_MB=1024 DM_TIGHT=1` | 45.0 | the same | 434.3 | fits |
| + `DIST_MN_SYM_SLABS=1` (§3) | 133.1 | + 51.5 GB of resident slabs | 576.2 | exceeds |
| the old model (`--staging resident`: no staging, 8 GiB flat) | 8.6 | — | 451.7 | (not the code) |

`estimate.py` (default `--staging code` now): 576 × 7.378 × 10¹⁰ — pool 82, node 525 GB, "NO / no"; with `--chunk both`
(`MN_T_CHUNK_MB=1024`) pool 45, node 460, "yes / yes", wall 244.9 s (4.1 min) against 234.0 (3.9). The largest 576 run at
480 GB: **3.73 × 10¹³** digits with the defaults, **4.59 × 10¹³** with `MN_T_CHUNK_MB=1024` (`estimate.py --g 576 --max`).
`MN_PLAN_ONLY=4.25e13:576` now ends with the `plan pool` line: `COMM_SHMEM_POOL_MB=77824` (43008 with `MN_T_CHUNK_MB=1024`).

**Answer to T0**: the staged buffers are counted in the device total already (they are `mn_core`'s rbO, sb, rb and pool 1's
xb) and the pool holds a *second copy* of the largest exchange's pair; so the pool setting was wrong **and** the node total
was missing it. 452 GB (428 with `DM_TIGHT`) does **not** hold with the defaults (525 / 501 GB). It holds with
`MN_T_CHUNK_MB=1024` (460 / 434 GB) — an existing switch (Phase 13a M, bit-identical), not on by default (the README row's
"1024 since Phase 13c" is wrong: `mn_t_chunk_limbs` defaults to 0; only `MDB_SHIFT_CHUNK_MB` is 1024). Adoption is the user's.

What would remove the second copy (not done; a design for the next session): (a) staging in rounds inside the transport
(a cap such as 1 GiB per staged buffer, the exchange carried in sub-rounds) — the pool then ≈ 4 × 2 GiB + control whatever the
size; needs a per-round sequence in the offset / signal words; (b) allocate `mn_core`'s rbO, the redistribution's sb / rb and the
shifts' sb / rb from the symmetric pool (`comm_sym_alloc`) instead of the block pool, and take them out of the arena request —
the pool then holds what the arena held. Either brings the target back to ≈ 452 GB with the pool at ≈ 9 GB.

The device model's own term for the division's products is the top tree level's scratch (`L['need_dev'] += sc[0]`); the
`A_h mu` product's rbO at 576 is 16.4 GB per APU, about twice the tree top's 8.2 — worth checking in C1 (not changed here).

#### 3. TASKS 2.4: `rns_dist`'s slabs in the symmetric pool — measured, not the design

Implemented behind `DIST_MN_SYM_SLABS=1` (off; `rns_dist.c` `mn_core`): the transform's slabs (2 q) and the layered scratch (q)
from `comm_sym_alloc`, kept per APU at the largest q; the transpose scratch stays in pool 1; the cached plans are rebuilt when
the slab pointer changes (a `sl` field in the plan cache). 10⁹ on 2 real nodes, SOS (b3, job 21275):

| | default (`sos_e9_sites`) | `DIST_MN_SYM_SLABS=1` (`sos_e9_symsl`) |
|---|---|---|
| digits | identical | identical |
| pool peak (PE 0) | 849 MiB (staging 847.7) | **2385 MiB** (staging 847.7 + symmetric 1536) |
| the transform's staged exchanges | 3456 × 32 / 32 MiB | none |
| block pool peak live (node 0) | 2.5 GB | 2.0 GB (the layered scratch, q per APU, moved out) |
| device at init / host HWM | 34.4 / 18.2 GB | 34.4 / 18.2 GB |
| total | 172.48 s | 171.23 s |

The transform's staging never sets the peak (32 against 106 MiB here; q / 4 against the result exchange's q-sized rows at
every size), so moving the slabs removes nothing from the pool's high-water; it adds 3 q × 8 B per APU of resident slabs
(1536 MiB here; 51.5 GB per node at the 576 target at q ≈ 5.4 × 10⁸) while plane pool 1 stays (the size-1 tiers use it).
The saving S12 expected ("the per-node total is unchanged") assumed pool 1 would shrink by the same bytes. Kept off; the
one-line change is therefore closed as measured.

#### 4. The pool rule (`COMM_SHMEM_POOL_AUTO`, the warning, the errors)

After `rns_init`, before `mn_init` (`ecalc.c`, one line): `binsplit_shmem_pool_rule(N, COMM_SIZE)` computes the need (§2) when
`COMM_TRANSPORT=shmem` and the size > 1.
- `COMM_SHMEM_POOL_AUTO=1` (off by default): `COMM_SHMEM_POOL_MB` is raised to the need (whole 256 MiB) when the need exceeds
  it; b4 `sos_e8_auto` (`COMM_SHMEM_POOL_MB=256`): "COMM_SHMEM_POOL_MB 256 -> 512", pool peak 130 of 512, identical.
- Without it (the default behaviour, unchanged): a pool below the need gets one warning on stderr naming the size (b4
  `sos_e8_warn`: 256 < 512, the run completes identical, peak 130 of 256).
- A pool that runs out stops with `comm_shmem`'s error, which now names the pool's use by kind, the size that would have held
  that allocation, and the model's need for the run (b4 `sos_e8_small`, `COMM_SHMEM_POOL_MB=96`: rc 3 in 3 s, "cannot hold
  16 MiB more (in use 82 MiB: control 2, staging 0, symmetric buffers 80): COMM_SHMEM_POOL_MB >= 99 needed now; the model's
  need for this run: 512 MiB").
- A host heap (SOS without the device heap, OSHMEM) needs the SHMEM heap ≥ pool + 512 MiB; the `shmem_malloc` failure now says
  so with the number. `MN_PLAN_ONLY` prints the pool and the heap for the launch line (`plan pool`).

#### 5. Gates

| gate | command (on aac6) | result |
|---|---|---|
| SOS, 2 real nodes, every size | b1a/b2/b3/b4: `COMM_TRANSPORT=shmem ./mnrun.sh 2 env COMM_SHMEM_DEVHEAP=1 POOL_LOG=27/29 … ./ecalc 10^8/10^9 …` | identical (10⁸ × 4, 10⁹ × 7 incl. `DIST_MN_SYM_SLABS=1`, `MN_T_CHUNK_MB=64`, `DIST_LOGN_TEST=25`) |
| SOS, 4 processes | b1b: `./mnrun.sh 4 env POOL_LOG=29 ./ecalc 10^9/10^10` | identical |
| TCP, 2 real nodes | b4: `COMM_TRANSPORT=tcp ./mnrun.sh 2 env POOL_LOG=27/29 ./ecalc 10^8/10^9` | identical: 10⁸ 23.02 s, 10⁹ 170.03 s |
| **SOS 10¹⁰, 2 real nodes, the final SOS build, the pool from the rule** | b5 (job 21279): `COMM_TRANSPORT=shmem ./mnrun.sh 2 env COMM_SHMEM_DEVHEAP=1 COMM_SHMEM_POOL_AUTO=1 COMM_SHMEM_VERBOSE=2 ./ecalc 10000000000 …` | **identical** to `results/e_1e10.out`, both VERIFY OK, 2035.97 s (bs 397.2, dm 1620.4; init 14.1); the rule set the pool 8192 → 8960 MiB; peak 8479 of 8960 on both PEs (staging 8477.1: the result exchange 1059.6 + 1059.7, the law 8477.3); the shift 1059.6 + 529.8 |
| regression | b6 (job 21278, s24-16, plain build 8b6955e): `./mnaccept.sh 21278 --full --only unit,e9,mn,full` | **17 passed, 0 failed**, 1907 s: t_ntt, t_mul, t_bs, t_dbig, t_newton, t_verify, t_out, t_mn_grid (2 procs); e9 both bases identical; mn 10⁸ at 2, 3, 4 and 10⁹ at 2, 4 identical; 4 × 10¹⁰ identical (61.67 s) and RECHECK OK |
| TCP 10¹⁰, 4 processes | b6: `./mnrun.sh 4 env POOL_LOG=29 ./ecalc 10000000000 …` | identical, 81.31 s |
| `mem_model.py --check-c` | not needed: no arena sizing changed (the pool is outside the arena) | — |

#### 6. Files

`ecalc/comm_shmem.c` (instrumentation, the errors), `ecalc/rns_dist.c` (`DIST_MN_SYM_SLABS`, `rns_mul_dist_mn_stage`),
`ecalc/mdb.h` (its declaration), `ecalc/binsplit.c` / `binsplit.h` (`binsplit_shmem_pool_need`, `binsplit_shmem_pool_rule`),
`ecalc/ecalc.c` (two calls), `ecalc/mem_model.py` (`mn_stage`, `shmem_products`, `shmem_pool`, `--pool`, the default staging
`code`, `TARGET576`), `ecalc/estimate.py` / `ecalc/mn_model.py` (default `--staging code`), `ecalc/README.md` (three rows),
internal target notes, internal target task list. Outside the prompt's list: `binsplit.c`, `ecalc.c`, `mdb.h` (the rule needs the
layout's n_Q and the tree schedule; minimal, commented).

#### 7. Open issues

1. The target does not fit 480 GB with the defaults (525 GB modelled); `MN_T_CHUNK_MB=1024` makes it fit (460) at +0.2 min
   modelled — the user's decision. A structural fix (§2 (a) or (b)) would return ≈ 70 GB per node.
2. The law's multi-piece terms are validated at one small shape (2 × 2 pieces at 10⁹ on 2 nodes, exact); the 576 figure rests
   on them and on `mn_scratch`'s window term.
3. `make -j16` sometimes links a test before `rns_dist.o` is rebuilt (seen twice after a `git reset`; a second `make` fixes it) —
   the Makefile's test rules lack the object dependencies. Not changed.
4. The device model counts the division's products with the top tree level's scratch (§2, last paragraph).

## V114 — the SHMEM pool by default, and the exchanges in rounds (Phase 14 agent V1)

(former file: V114.md)

Branch `p14-V1` from `p14-defaults` @ 4855842. aac6 clones: `~/ntt-V114` (plain build, for `mnaccept.sh`), `~/ntt-V114s`
(`make SHMEM_HOME=~/sos`, Sandia OpenSHMEM). Batches: `~/V114/chain.sh` (b1–b4, one job at a time), helpers `~/V114/lib.sh`,
logs `~/V114/b<N>/*.log` and `summary.txt`. Background: results/SHMEM_ITEMS.md §P214.

#### RESUME

- **Done.** Final commit on `p14-V1` (this file). Built on aac6: `~/ntt-V114` (plain, 0b07ebc), `~/ntt-V114s` (SOS, 0b07ebc),
  `~/ntt-V114r` (SOS, 8f3b136 = the final code). No job of V1 running or pending. b5 was the last node batch (the
  integrator's instruction); b6 (the 2-node re-check on 8f3b136) was not submitted.
- Verdicts: §0. Open: §5.

#### 0. Verdicts

- **Task 1, `COMM_SHMEM_POOL_AUTO=1` by default: done, and it works correctly with no pool or heap set by hand**, provided
  the staging regions (8f3b136) are in place. Measured on SOS, 2 real nodes: 1e9 identical (host heap and device heap) and
  1e10 identical, with the pool from `mnrun.sh`'s plan. At 4 processes: 1e9 and 1e10 identical on 8f3b136. Without the
  regions, 1e10 at 4 processes failed because the pool fragmented (b4). A heap that is too small is refused cleanly with rc 8
  and every size named. The 2-node runs used 6e92c74 / 0b07ebc, which have no regions. The regions change only the default
  path's placement, not the sizes, and the 4-process runs have now checked it. A 2-node re-check on 8f3b136 is still open (§5).
- **C1, `COMM_SHMEM_ROUND_MB` (off): done, bit-identical at every size tested; it removes the second copy.** 1e10 on 2 nodes:
  pool 8194 → 2050 MiB per node-process, host HWM 27.8 → 21.3 GB, node MemAvailable min +6.6 GB, device total unchanged,
  total 2089.8 → 2072.9 s (not slower). 1e10 on 4 processes: 4244 → 2054 MiB, host HWM 23.3 → 21.2 GB, MemAvailable min
  +8.4 GB (node), 381.2 → 357.9 s. The law matches every measured point to 0.01 % (`mem_model.py --pool`). Target
  (modelled): pool 45.0 → 9.8 GB; node 459.6 → 424.4 GB (the Phase 14 defaults incl. DM_TIGHT: 434.3 → 399.1); without
  MN_T_CHUNK_MB the switch alone gives 452.9 GB, P214's expected figure. Recommended value for the target: 1024. Adoption is
  the user's decision.

#### 1. Task 1 — `COMM_SHMEM_POOL_AUTO=1` by default, correct with a host heap

The pool is carved from the SHMEM library's heap unless it is SOS's external heap (`COMM_SHMEM_DEVHEAP=1` with the patch, a
HIP buffer of exactly the pool). Raising the pool at init (the P2 rule) could therefore overflow a heap that the launcher
sized from the old pool. Now:

- **`mnrun.sh`** (when `COMM_SHMEM_POOL_MB` is unset): finds the command's SHMEM-linked executable followed by a digit count
  (`ecalc <digits> …`, through `env`/wrappers), runs it on the launch host with the command's `VAR=value` words and
  `MN_PLAN_ONLY=<digits>:<procs>` (no device touched, 0.04 s at 4.25e13:576), and takes `COMM_SHMEM_POOL_MB` from the
  `plan pool` line; the host heap (`SHMEM_SYMMETRIC_SIZE` for SOS, `SHMEM_SYMMETRIC_HEAP_SIZE` for OSHMEM) is then the pool
  + 512 MiB as before. Other commands (t_comm, …) keep 8192. `MNRUN_PLAN_POOL=0` skips the plan.
- **`ecalc`** (`binsplit_shmem_pool_rule`, before `shmem_init`): `COMM_SHMEM_POOL_AUTO` defaults to 1 (the pool raised to the
  need); then, when the pool lives in the library heap (`comm_shmem_pool_in_heap()`) and the launch line sets the heap
  variable (`comm_shmem_heap_env()`: K/M/G/T suffixes), a heap below the pool + 512 MiB stops **every rank** before
  `shmem_init` with rc 8 (`EC_RC_BUDGET`) and a message naming the heap variable and size, the pool, the need and what to
  set. A heap given by another variable is not seen (shmem_malloc's own error names the size then).
- **internal target notes §4**: the launch-line rule (plan → pool and heap; device heap sized by a variable counts as a heap).
- README: `COMM_SHMEM_POOL_AUTO` default 1, `COMM_SHMEM_POOL_MB` row.

#### 2. Task 2 (C1) — the second copy removed: the all-to-alls in rounds (`COMM_SHMEM_ROUND_MB`, off)

**Choice: P214 §2 (a), rounds inside the transport**, not (b) pool-resident caller buffers. Reasons from the code:
- (a) is local to `comm_shmem.c`: the callers (rns_dist.c's result exchange and redistributions, newton_db.c's mdb_shift,
  comm_layered's inter stage, ntt_dist's slab pipeline) and the arena (`rns_mul_dist_mn_scratch`, `mem_model --check-c`)
  are unchanged; TCP and SHMEM keep one memory layout.
- (b) would move rbO / sb / rb / xb (four call sites, two in pool 1 whose size-1 tiers still need it — P214 §3 showed that
  moving the transform slabs only *added* pool) into the symmetric pool; the pool would then be as large as those buffers
  (with a host heap, a heap of that size), the arena request would have to shrink by exactly them, and the device total is
  unchanged only if both are exact. It also needs a registered (device-accessible) pool, which OSHMEM may refuse.
- (a) bounds the pool at 4 × 2 × m + control whatever the size; its cost is latency per round, small when m is large.

**Design** (comm_shmem.c, `alltoallv_rounds` / `rounder`): with `COMM_SHMEM_ROUND_MB = m`, a pair (s → r) of b bytes goes in
K = ceil(b / c) rounds, c = m / (n − 1) (256-aligned, ≥ 4 KiB); K = 1 when b ≤ c. Both ends know b (the sender's scnt =
the receiver's rcnt, checked by the signal), so both derive K without a collective. The control words' sequence is now
**per pair** (`oseq[r]`, `iseq[r]`; an ordinary exchange advances every pair by one, so a pair of one round is exactly the
ordinary exchange's words and a member in rounds meets a member not in rounds pair by pair). Round j: publish, per active
sender, where its chunk j lands (a slot of min(c, b) in the receive staging, or the pool-resident receive buffer directly);
stage my chunks j; put each after the receiver's word of round j (put-with-signal / fence / quiet as configured); wait for
the signals of round j; quiet (the send staging is free); copy the received chunks out. A member enters round j after
completing j − 1 and publishes before it blocks, so the rounds cannot deadlock. The rounds run in a helper thread on the
communicator's own HIP stream (the call returns at once, as the ordinary exchange does — comm_layered overlaps its intra
stage with the inter exchange), `wait` joins it. The equal all-to-all goes through the same path (its counts as arrays).
Staging per round ≤ m each way, so the pool's staging ≤ 4 × 2 m.

The law follows in C (`rns_mul_dist_mn_stage`'s RMIN, `binsplit_shmem_pool_need`'s shift) and Python
(`mem_model.mn_stage` / `shmem_pool(round_mb=)`, `mem_per_node(opts round_mb)`): every side of every staged exchange at
most m. `COMM_SHMEM_VERBOSE` / PE 0 at finalize print the rounded exchanges, their rounds, the largest round's staging, and
every PE's all-to-all time (post to completion, summed over the APU threads).

#### 3. The model and the plan (modelled; login node, `MN_PLAN_ONLY`, SOS build)

`python3 mem_model.py --pool`, the 4.25e13 target on 576 nodes (GB per node):

| configuration | pool | node total | 480 GB |
|---|---|---|---|
| the code before MN_T_CHUNK_MB (P214) | 81.6 | 524.7 | exceeds |
| MN_T_CHUNK_MB=1024 | 45.0 | 459.6 | fits |
| MN_T_CHUNK_MB=1024 + DM_TIGHT (the Phase 14 defaults) | 45.0 | 434.3 | fits |
| **COMM_SHMEM_ROUND_MB=1024** (no MN_T_CHUNK_MB) | **9.8** | **452.9** | fits — P214's 452 GB and the 3.9 min estimate |
| COMM_SHMEM_ROUND_MB=1024 + DM_TIGHT | 9.8 | 429.4 | fits |
| COMM_SHMEM_ROUND_MB=1024 + MN_T_CHUNK_MB=1024 | 9.8 | 424.4 | fits |
| COMM_SHMEM_ROUND_MB=1024 + MN_T_CHUNK_MB=1024 + DM_TIGHT (defaults + the switch) | 9.8 | **399.1** | fits |

The pool with rounds = 4 × 2.15 GB (the tree level of 2's result exchange at the 1 GiB bound each way) + control 0.94 GB.

`MN_PLAN_ONLY=<d>:<g> ./ecalc | grep 'plan pool'` (COMM_SHMEM_POOL_MB, MiB; the defaults include MN_T_CHUNK_MB=1024):

| run | no rounds | ROUND_MB=1024 | ROUND_MB=256 |
|---|---|---|---|
| 1e9 on 2 | 1280 (848 staging) | 1280 | 1280 |
| 1e10 on 2 | 8704 (4 × 2048) | 8704 | **2560** (4 × 512) |
| 1e10 on 4, POOL_LOG=29 | 4608 (4 × 1059.7) | — | **2560** |
| 4.25e13 on 576 | 43008 | **9472** | — |

(At 2 nodes the 1 GiB bound is at the exchange size already — MN_T_CHUNK_MB=1024 — so the measurements use 256 MiB at
1e10 and 64 MiB at 1e9 to show the effect.)

#### 4. Tests (times Eastern; aac6 logs are Central + 1 h)

All on SOS (`~/ntt-V114s`), `COMM_TRANSPORT=shmem ./mnrun.sh <p> env … ./ecalc <d> <file>`, **no pool or heap set by hand**
(host heap unless marked), digits `cat parts | cmp - ref` (1e9: `~/ntt/ecalc/ref/e_1000000000.txt`, 1e10:
`results/e_1e10.out`). Every number measured unless labelled.

**b1, job 21435, s24-26 + s24-30, 18:22–18:32 EDT (build 6e92c74; the rounds' path then only for pairs of K ≥ 2):**

| run | result | pool (plan → peak) | node: device max / host HWM / MemAvailable min | all-to-all time (PE 0, summed over 4 threads) | total |
|---|---|---|---|---|---|
| t_comm, 2 PEs | VERIFY OK × 2 | 8192 (not ecalc) | — | — | 4.2 s |
| t_comm `COMM_SHMEM_ROUND_MB=1` | VERIFY OK × 2 (9 exchanges in 69 rounds) | | | | 2.3 s |
| t_comm `ROUND_MB=0.001` (4 KiB chunks) | VERIFY OK × 2 | | | | 24.5 s |
| t_comm `ROUND_MB=1 COMM_SHMEM_ORDER=quiet` | VERIFY OK × 2 | | | | 2.3 s |
| t_comm `ROUND_MB=1 COMM_SHMEM_SERIAL=1` | VERIFY OK × 2 | | | | 2.3 s |
| **1e9, task 1** | **identical**, VERIFY OK both | mnrun: 1280 from the plan → peak 849 (staging 847.7) | 115.8 / 19.8 / 396.3 GB | 638.9 s, 4144 exchanges | 177.35 s |
| 1e9 `ROUND_MB=64` (6e92c74) | identical | 1024 → peak 849: **no pair above 64 MiB** (per peer 53 MiB; the 106 MiB block includes the self slab) | 115.8 / 19.6 / 396.6 | 626.2 s | 176.40 s |
| heap refusal: `COMM_SHMEM_POOL_MB=256` by hand (mnrun: heap 768M) | **rc 8 in 8.8 s on both ranks**: "the SHMEM heap SHMEM_SYMMETRIC_SIZE=768 MiB cannot hold the symmetric pool COMM_SHMEM_POOL_MB=1280 MiB (+ 512 MiB; the modelled need of this run 1280 MiB …): launch with SHMEM_SYMMETRIC_SIZE >= 1792M …" | 256 → auto 1280 | — | — | — |
| 1e9 `COMM_SHMEM_DEVHEAP=1` | identical | 1280 → 849 | 115.8 / 18.5 / 396.3 | 636.9 s | 175.45 s |

**b2, job 21439, s24-26 + s24-30, 18:52–19:28 EDT (0b07ebc; the default path is the same code as 6e92c74) and b3, job 21442,
19:30–20:09 EDT (0b07ebc):** before / after at 2 real nodes

| run | result | pool (plan → peak) | device max / host HWM / MemAvailable min (per node) | all-to-all time (PE 0, 4 threads summed) | total (bs / dm) |
|---|---|---|---|---|---|
| t_comm `ROUND_MB=1` (0b07ebc: every exchange on the rounds' path) | VERIFY OK × 2 | | | | 4.4 s |
| **1e10, task 1** (b2) | **identical**, VERIFY OK both | mnrun 8704 from the plan → **8194** (staging 8192.0 = 4 × 2048, law 8192.5) | 186.7 / 27.8 / **351.2** GB | 7990.3 s, 5504 exchanges | 2089.83 s (403.1 / 1628.5) |
| 1e9 `ROUND_MB=64` (b3) | identical | 768 → **425** (staging 423.9; law 423.9) — before 849 | 115.8 / 19.3 / 396.8 — before 19.8 / 396.3 | 613.3 s — before 638.9 | 177.39 s — before 177.35 |
| **1e10 `ROUND_MB=256`** (b3) | **identical** | 2560 → **2050** (staging 2048.0 = 4 × 512; 84 exchanges in 176 rounds) — before 8194 | 186.7 / **21.3** / **357.8** GB — before 27.8 / 351.2 | 7677.8 s — before 7990.3 (−4 %) | **2072.91 s** (396.9 / 1620.3) — before 2089.83 |

So at 1e10 on 2 nodes the rounds take the pool from 8194 to 2050 MiB per node-process (−6.0 GiB), the host HWM from 27.8
to 21.3 GB and the node's lowest MemAvailable up by 6.6 GB (measured); the device total is unchanged (186.7 GB: the callers'
buffers are where they were); the run is not slower (−0.8 % total, −4 % summed exchange time: one run each, within noise —
the smaller staging copies outweigh the extra round trips on this 1 GbE link). The target's pool is modelled from the same
law (§3): 9.8 GB instead of 45.0, node 424.4 GB (with DM_TIGHT 399.1) instead of 459.6 (434.3).

**b4, job 21448, s24-16, 20:35–21:07 EDT (0b07ebc):**

| step | result |
|---|---|
| `./mnaccept.sh 21448 --only unit,e9,mn` (plain build `~/ntt-V114`) | **14 passed, 1 failed (unit t_bs: rc 139, segfault in "1. P, Q vs the GMP-free reference recursion")**, 1300 s. The same t_bs segfault on the integrator's `ntt-def14` @ 2085d1e0 (job 21445) and on V2's branch (job 21431): it predates this branch (the Phase 14 defaults), not caused by V1. e9 both bases, mn 10⁸ at 2/3/4 and 10⁹ at 2/4, t_mn_grid: PASS |
| t_comm, 4 PEs, `ROUND_MB=1` (the strided even / odd PE sets) | VERIFY OK × 4, 3.3 s |
| **1e10, 4 processes, task 1 (no pool set)** | **FAILED, rc 3 / 139 after the tree**: "the symmetric pool (4608 MiB) cannot hold 529 MiB more (in use 3167 MiB …)" on PEs 0 and 2 — the pool from the plan (4608 = the law's 4239 staging + control + margin) is enough in bytes but its one first-fit area **fragments** (four APU threads allocating and freeing staging of different sizes out of step). P214's 4-process runs set 12288 by hand and never met it. Fixed in 8f3b136 (below); re-run in b5 |
| 1e10, 4 processes, `ROUND_MB=256` | identical, VERIFY OK × 4, 346.85 s; pool 2560 from the plan → peak 2054 (staging 2048 = 4 × 512); 44 exchanges in 112 rounds (PE 0); device max 77.3 GB, host HWM 21.2, MemAvailable min 213.0 GB (the node, 4 processes) |

**b5, job 21452, s24-16, 21:12–21:26 EDT (8f3b136: the staging regions), 4 processes on one node, no pool set:**

| run | result | pool (plan → peak; regions) | device max / host HWM / node MemAvailable min | all-to-all (PE 0) | total |
|---|---|---|---|---|---|
| t_comm, 4 PEs (no rounds) | VERIFY OK × 4 | | | | 4.8 s |
| 1e9, POOL_LOG=29 | identical | 768 → 429 (4 × 107 MiB regions) | 42.9 / 18.8 / — | 68.9 s | 27.39 s |
| **1e10, POOL_LOG=29 (the b4 failure)** | **identical**, VERIFY OK × 4 | 4608 → **4244** (staging 4238.6; 4 × 1061 MiB regions + 364 general) | 77.3 / 23.3 / 205.1 GB | 1026.5 s | 381.16 s |
| 1e10 `ROUND_MB=256` | identical | 2560 → **2054** (4 × 513 regions) | 77.3 / 21.2 / 213.5 GB | 936.8 s (−9 %) | 357.90 s |

The fix (8f3b136, `COMM_SHMEM_STAGE_SLOTS`, default 1):** the staging lives in four regions at the pool's end, one per APU
(the calling thread's HIP device), each the model's per-APU need + 1 MiB (`COMM_SHMEM_STAGE_SLOT_MB`, exported by the pool rule
before `shmem_init`). A thread's staging is its current exchange's send + receive, freed before its next, so a region
never fragments; a request that does not fit its region falls back to the general first-fit area (control blocks, the
self-tests' symmetric buffers, `comm_sym_alloc`). Memory placement only; the pool size and the law are unchanged.

Finding from the `ROUND_MB=64` run: the ordinary path stages the **self slab** too (it never leaves the node): half the
staging at 2 PEs. Fixed in 0b07ebc **under the switch only**: every all-to-all takes the rounds' path (K = 1 included), whose
staging holds the peers' chunks alone; the law (C and Python) takes the peers' part, modelled as (g − 1)/g of each side,
bounded by m. Plan with 0b07ebc: 1e9 on 2 at 64 → 768 MiB (424 staging); 1e10 on 2 at 256 → 2560 (4 × 512); 1e10 on 4
at 256 → 2560.

#### 5. Open

1. **Two-node re-check on 8f3b136.** b1–b3 (2 nodes) ran on 6e92c74 / 0b07ebc, which have no staging regions. b5 checked
   the regions at 4 processes. A 1e9 run on 2 nodes on `~/ntt-V114r` (`~/V114/chain2.sh`'s b6, not submitted) would close
   this.
2. **t_bs segfaults** (rc 139, in "1. P, Q vs the GMP-free reference recursion"). It fails the same way on the
   integrator's `ntt-def14` and on V2's branch, so it predates V1 and comes from the Phase 14 defaults. It is not caused by
   V1, and it is the only failure in `mnaccept --only unit,e9,mn`.
3. **The 1e11 reference was not used.** `~/ntt/ecalc/results/e_1e11.out` did not exist. The runs used the 1e9 and 1e10
   references, as the task sizes require.
4. **The rounds' law is an upper bound.** It models a side as (g − 1)/g of the whole, bounded by m. It is exact at the
   measured points (2 and 4 PEs); at the target the bound m sets it anyway.
5. **Pool-resident exchanges also take the rounds' path** under the switch (only latency; there is no staging to save).
   The ordinary path still stages the self slab (half the staging at 2 PEs). That is left unchanged by default.
6. **Rounds do not cover `alltoallv_host` and the all-gathers.** Their sizes are small.
7. **mnrun.sh runs the plan on the launch host with `module load rocm`.** For a command it cannot recognise as
   `<shmem-linked exe> <digits>`, the pool stays at 8192.

#### 6. Files

`ecalc/comm_shmem.c` (rounds, per-pair sequences, the staging regions, the heap functions, the all-to-all time), `ecalc/comm.h` (two
declarations), `ecalc/binsplit.c` (the rule: default 1, the heap check; the shift's rounds), `ecalc/rns_dist.c`
(`rns_mul_dist_mn_stage`: RMIN), `ecalc/mnrun.sh` (the plan), `ecalc/mem_model.py` (round_mb, the target rows),
`ecalc/README.md` (four rows: POOL_MB, POOL_AUTO, ROUND_MB, STAGE_SLOTS), internal target notes (§4 rule, §3 row).

## N7 — SHMEM data path (`COMM_OFI=0`) against `comm_ofi`: sweep, diagnosis, peer-order fix, confirmation

(former file: N7.md)

Scope: the N7 record (2026-10-13 to 2026-10-14, aac7, 1 to 6 nodes): the SHMEM-only exchange gap to `comm_ofi`, the
sweep of SHMEM settings, the diagnosis, the switch `COMM_SHMEM_PEER_ORDER=shift` (off by default), and its confirmation.
Dated record, frozen when written; the code and the central docs win where they differ.

**Question.** Why is the SHMEM-only path (`COMM_OFI=0`) several times slower than `comm_ofi` at 2 to 6 nodes, which
part of the gap can settings close, and does a change to the order of the exchange puts (no change to digits) close
part of the rest?

**Change / branch.** Branch `n7`, commit 6310579 (`comm_shmem: COMM_SHMEM_PEER_ORDER=shift`), rebased on main 3a122c9.
`ecalc/comm_shmem.c` only (33 lines touched, net +18); one new switch `COMM_SHMEM_PEER_ORDER` = `rank` (default; unset
means rank) | `shift`. Under `COMM_OFI=1` the same push loop is shared, so the switch also reorders comm_ofi's writes
(order only; not measured as a change in comm_ofi speed). No allocation, library call, count or sequence change.

**Method.**
- Sweep (P1, measured): queue item `n7_sweep` on aac7, 2026-10-13 21:13 to 22:59 ET, 31 recorded runs (30 ok, 1 killed).
  Part A: 6 nodes, 3.8646e11 digits, `comm_ofi` vs `COMM_OFI=0` in ABAB order. Part B: 2 nodes, 1.2882e11 digits, three
  concurrent lanes (compare configs inside the sweep only), `SHMEM_OFI_NUM_NICS` 1/2/4 × `COMM_SHMEM_ROUND_MB` 256/1024/4096,
  2 runs per config; one-offs `COMM_SHMEM_SERIAL=1`, `COMM_SHMEM_SPIN_US=0 | 20000`, `COMM_SHMEM_ORDER=fence`. Best config
  checked at 6 nodes (2 runs). Digit probes at 1e10 digits against the reference.
- Node screen before each item: `tools/nodescreen.sh --want-nics 4` (rc 0, no node excluded).
- Diagnosis (P3, measured): per-stage times and exchange wait from the run logs (`COMM_XSTATS=1`, `MN_WAIT_STATS=1`);
  reading of `ecalc/comm_shmem.c` (exchange protocol) and of the vendor documentation for Cray OpenSHMEMX (one NIC per PE).
- Confirmation (P5, measured): cluster item `n7_confirm` (commit 6310579, 2026-10-13 23:46 ET to 2026-10-14 02:04 ET):
  node screen, switch registry and profile checks, `t_comm --bw` at 6 nodes (256 MiB slabs, 4 NICs), gates 1 and 2 nodes
  (`gate.sh` defaults), a 6-node 1e10 digit probe, and a 6-node ABBA of 3.8646e11 digits (`COMM_OFI=0`, 4 NICs, 256 MiB
  rounds, rank arm A and shift arm B). Driver stopped after two failed runs in a row (round 4, both arms, rc 124 at the
  1500 s cap). The `comm_ofi` anchor and the `COMM_SHMEM_SPIN_US=0` check were not run.

#### 1. Sweep (measured)

**Part A, 6 nodes, 3.8646e11 digits (total seconds):**

| run | transport | total s | bs | dm | init |
|---|---|---|---|---|---|
| A1 | `comm_ofi` | 245.99 | 98.4 | 93.8 | 47.2 |
| A1 | `COMM_OFI=0` (round 1024 MiB, NICs unset) | 820.63 | 293.7 | 481.0 | 40.3 |
| A2 | `comm_ofi` | 215.09 | 84.3 | 89.0 | 38.5 |
| A2 | `COMM_OFI=0` | killed at the 1200 s cap | NA | NA | NA |

- `comm_ofi` mean 230.5 s (sd 21.8, 2 runs). SHMEM default 820.6 s (1 run; the second run was killed, the log shows
  normal progress and no error, so it is recorded as slow, not as a hang). Paired first ABAB pair: 3.34 × (measured).

**Part B, 2 nodes, 1.2882e11 digits, mean of 2 runs (seconds, sd), `COMM_OFI=0`:**

| config (NUM_NICS × ROUND_MB) | mean s (sd) |
|---|---|
| 1 × 256 | 234.0 (24.9) |
| 1 × 1024 | 244.1 (27.1) |
| 1 × 4096 | 256.9 (23.8) |
| 2 × 256 | 232.5 (19.7) |
| 2 × 1024 | 269.4 (72.6) (one run 320.7 s) |
| 2 × 4096 | 251.1 (13.6) |
| **4 × 256** | **210.5 (1.8)** |
| 4 × 1024 | 222.5 (1.3) |
| 4 × 4096 | 254.7 (24.2) |

- Best config 4 × 256 (`SHMEM_OFI_NUM_NICS=4`, `COMM_SHMEM_ROUND_MB=256`): 210.5 s, the two runs within 2.5 s (measured).
- One-offs at 2 nodes, single runs (measured): serial 211.9 s (no gain); spin 0 243.1 s; spin 20000 258.1 s; order fence
  243.6 s. The spin and order variants are 15 to 48 s slower than the best config.
- Best config at 6 nodes (2 runs): 701.0 s and 696.0 s, mean 698.5 s (measured), 15 % below the SHMEM default run.
- Digit probes at 1e10 digits: `comm_ofi` 6 nodes 32.8 s, `COMM_OFI=0` 6 nodes 38.8 s, `COMM_OFI=0` 4 × 256 2 nodes
  45.4 s; all three digits identical (measured).

**Gap (measured):** `COMM_OFI=0` at 6 nodes is 3.0 × `comm_ofi` (698.5 s against 230.5 s); at 2 nodes the best SHMEM
config is 1.5 × the `comm_ofi` mean of the earlier 2-node measurement (210.5 s against 140.3 s, `results/SHMEM_ITEMS.md §MEAS-N7`).
The 2-node SHMEM default was 2.1 × (`results/SHMEM_ITEMS.md §MEAS-N7`). The 3.8646e11 and 1.2882e11 sweep runs were checked
`VERIFY=OK` only, not against a reference; the digit identity is shown by the probes and by the P5 gates.

#### 2. Diagnosis (measured, with one vendor fact)

- **Where the gap is.** At 2 nodes the whole gap is exchange wait: the distributed division multiply (dm) +109 s and
  binary splitting (bs) +32 s; init and digit output are unchanged (`results/SHMEM_ITEMS.md §MEAS-N7`, measured).
- **Why it grows with nodes.** At 6 nodes the tree has a level-2 group of 6, so the division runs an unequal
  all-to-all over 5 peers (`layv`). Under SHMEM `layv` moves 1.20 to 1.39 GB/s per APU thread, about 5.1 GB/s per node;
  under `comm_ofi` it moves 7.2 to 9.2 GB/s per APU, about 32 GB/s per node: 6.3 × slower. `layv` carries 3.4 of the
  4.6 TB a node receives at 6 nodes and about 82 % of the 6-node gap (measured, xstats). The equal-slab form at 2 nodes
  reaches 16.2 GB/s per node under SHMEM, so the fall-off is specific to the unequal exchange.
- **Why the settings cannot close it.** Cray OpenSHMEMX assigns each PE to exactly one NIC (vendor documentation, 11.x);
  ecalc runs one PE per node, so one of the node's four NICs carries the node's traffic. Measured: a probe with 4 PEs per
  node reaches about 31 to 32 GB/s per node over four NICs, the rate `comm_ofi` reaches (measured, earlier probe). No
  SHMEM setting in the sweep lets one PE use more NICs; `SHMEM_OFI_NUM_NICS=4` helps only by spreading the PE's
  resources (best 2-node config above). The one-PE-per-node structure is the constant part of the gap: this is the
  subject of N7b (decision D29 (a), one PE per APU).
- **Incast in the exchange order.** With the rank order every member sends first to peer 0, then peer 1, and so on, so a
  receiver is written by many senders at once in each step. A shifted ring makes each step a one-to-one pairing.
  This is the hypothesis Fix A tests; the runs below are its test.

#### 3. Fix A: `COMM_SHMEM_PEER_ORDER=shift` (implemented; off by default)

- Each member walks its peers as a shifted ring: step k sends to (me + k) mod n and publishes to / waits for the sender
  (me − k) mod n. The default `rank` keeps today's order exactly.
- Loops changed (8): the data push and its quiet-mode signals, `publish` and `arrive`, and in the all-to-all rounds the
  offset publish, the sender half, the quiet-mode signals and the round-signal wait. Staging copies, sequence checks,
  init and small allreduce loops are unchanged (order irrelevant).
- Registry: `ecalc/README.md` row; `switches_manual.tsv`, `switches_review.tsv` (experiment, owner N7, review 2026-11-12);
  regenerated `switches.tsv` and `switches_gen.h`. `tools/doccheck.py` 0 problems; `switch_registry.py --check` 0 problems.
- Build on aac7 (login node) with `make -B SHMEM_CRAY=1` (log has `-DCOMM_SHMEM`, 0 warnings).

#### 4. Confirmation (P5, measured)

**Transport probe, 6 nodes, 256 MiB slabs, 4 NICs, aggregate GB/s per node (two runs per arm):**

| arm | run 1 | run 2 | mean |
|---|---|---|---|
| rank (default) | 10.24 | 9.92 | 10.08 |
| shift | 12.48 | 11.95 | 12.21 |

Shift is +21 % (measured, n = 2 per arm; the two ranges do not overlap).

**Digits and gates:**
- Gate 1 node: PASS, digits 1e9 identical, VERIFY OK (measured).
- Gate 2 nodes: PASS, digits 1e9 identical, VERIFY OK (measured).
- 6-node probe, 1e10 digits, shift: VERIFY OK, compare identical, total 58.6 s (one run, measured).
- 6-node ABBA runs, 3.8646e11 digits: all 6 runs VERIFY OK; compare n/a (no reference at this size, as in the sweep).
  Digit identity at 6 nodes is therefore shown only by the 1e10 probe.

**Full-run ABBA, 6 nodes, 3.8646e11 digits, `COMM_OFI=0`, 4 NICs, 256 MiB rounds (seconds, measured):**

| pair | A rank | B shift | B − A |
|---|---|---|---|
| 1 | 728.80 | 1373.33 (outlier) | +644.5 |
| 2 | 763.84 | 648.52 | −115.3 |
| 3 | 776.84 | 638.51 | −138.3 |
| 4 | killed at 1500 s (rc 124) | killed at 1500 s (rc 124) | stopped |

- Paired B − A over 3 pairs: mean +130.3 s, 95 % CI [−976, +1237] (measured; the CI includes 0, so no claim).
- Pairs 2 and 3, without the outlier: shift −115 s and −138 s, about −15 % and −18 % (measured, 2 pairs).
- Stage split of the same pairs: bs and dm move the same way as the totals; the wait fields (`wdm`, `wbs`) are noisy
  between pairs (see the item's summary).
- The sweep stopped after two failed runs in a row, as its rule says. The spin-0 check and the `comm_ofi` anchor of this
  ABBA were not run.

#### 5. Verdict

- **Fix A: keep `COMM_SHMEM_PEER_ORDER=rank` as the default; do not adopt yet.** The transport probe shows +21 % per-node
  bandwidth at 6 nodes (measured, n = 2 per arm). The full-run A/B is inconclusive: the 3-pair CI includes 0, and the one
  outlier (+644 s) is larger than the two clean pairs (−115 s, −138 s) can offset. Adoption needs a decision number
  from more pairs (≤ 4 rounds per arm, the project's ABBA limit), which the next run should take with the 4 NICs and
  with a stop rule that does not end both arms at once.
- **The SHMEM path stays far behind `comm_ofi`.** Even the best SHMEM config is 3.0 × slower at 6 nodes (measured). The
  shift ordering recovers part of the bandwidth (measured); the rest is the one-NIC-per-PE limit, which only N7b (one PE
  per APU) addresses.

#### 6. Open issues

- **Node state after killed runs (observation, cause unknown).** Twice a SHMEM run was killed at its timeout, and the six
  nodes went to the scheduler's drain state `needs_reboot` straight afterwards: 2026-10-13 21:07 ET and 2026-10-14
  02:05 ET (measured, scheduler state). Not established whether the kill caused it; logged as a TASKS item to investigate.
- The 1500 s cap of the P5 ABBA was reached in both arms of round 4, while the same arms took 640 to 780 s in rounds 1 to 3
  (measured). Whether the slow runs are a system state or a launch effect is not established.
- The `comm_ofi` anchor of the P5 ABBA and the spin-0 check were not run (the driver was stopped).
- The `COMM_OFI=0` runs above 1e10 digits have no reference comparison.
- OpenSHMEMX 12.1.0 (the target's library) and two NICs per PE are untested; aac7 has 11.8.0 and one NIC per APU.

#### RESUME

- Branch `n7`, commit 6310579 (rebased on main 3a122c9): Fix A implemented, switch off by default. Results: this file.
- Done: P0 to P5 (sweep, diagnosis, fix, confirmation). Nothing running on the cluster after P5.
- Decision open: D29 (a) N7b one SHMEM PE per APU (target: 2 NICs per APU, PEs per APU parameter 1 or 2). Adoption of
  Fix A waits for more 6-node pairs.
- Next: N7b design (one PE per APU, communicator map and launch) before any run; then the Fix A confirmation with more pairs
  if the user asks for it. Node drain observation: TASKS Proposed.
- Raw logs stay on the cluster (item `n7_sweep`, item `n7_confirm`); the cluster scratch for the N7 build was removed.

## MEAS-N7 — SHMEM data path (`COMM_OFI=0`) against libfabric (`COMM_OFI=1`) at 2 nodes

(former file: MEAS-N7.md)

**Question.** OPPORTUNITIES N7: how much slower is the SHMEM-only data path than `comm_ofi` today, where does the gap sit, and what SHMEM pool/heap does the plan need with `COMM_OFI=0`?
**Change / branch.** None (measurement only). Build: `main` a6ba51d, `make -B SHMEM_CRAY=1` (log has `-DCOMM_SHMEM`), aac7 worktree `~/ntt-wt/meas`. Switch `COMM_OFI` (unset = 1 where a cxi NIC exists).
**Method.** Queue item `82_meas_n7` (user-approved 2026-10-12), one 2-node hold, driver `meas_abba.sh`: rounds of A B B A, A = `COMM_OFI=1`, B = `COMM_OFI=0`; 2 x 6.441e10 = 1.2882e11 digits, no digit writes; launch line = `tools/profile_load.py aac7 --line` (+ `COMM_XSTATS=1 MN_WAIT_STATS=1`); `mnrun.sh` recomputes the SHMEM pool/heap per arm from `MN_PLAN_ONLY`. VERIFY OK and rc 0 on all 8 runs. Round cap: the 40-min item budget allowed 2 rounds (SHMEM runs take 2 to 3 times longer), not the 4-round cap. Plan values: `tools/plan_values.py --profile aac7 128820000000 2` with `COMM_OFI=1` / `0` (`COMM_OFI_PLAN_CXI=1`). Digits were not compared (timing runs); VERIFY OK only.
**Results (measured).**

| run | A `COMM_OFI=1` total s | B `COMM_OFI=0` total s |
|---|---|---|
| round 1 | 157.39, 133.32 | 285.22, 280.26 |
| round 2 | 136.25, 134.11 | 406.20, 278.16 |

| statistic | value |
|---|---|
| paired round means B - A (2 pairs) | +137.4 s, +207.0 s |
| mean difference B - A | **+172.2 s, 95% CI [-270, +614] (n = 2 pairs, p 0.12)** |
| means of the 4 runs | A 140.3 s, B 312.5 s (B median-like value 281 s: 3 of 4 runs within 278 to 285 s; one run 406 s with init 64.7 s and `ready` waits doubled, a slow-mode run) |

The interval is wide only because of the 406 s run and n = 2; every B run is at least +122 s above every A run (smallest B 278.2 s, largest A 157.4 s), so the sign is not in doubt. Without the 406 s run the typical gap is about +145 s (2.1 x).

Per-stage split (seconds in the run's own phase line; mean of A over 4 runs, B over the 3 runs of ordinary speed; wait = the wait-stats sum over the 4 APU threads of the slower node):

| stage | A (OFI) | B (SHMEM) | B - A |
|---|---|---|---|
| init | 34 to 44 | 34 to 40 | about 0 |
| binary splitting (bs) | 45.5 | 77.1 | +31.6 |
| distributed division multiply (dm) | 54.4 | 163.0 | +108.6 |
| dc (digits) | 3.1 | 3.0 | about 0 |
| wait, bs / dm / recip (thread-summed) | 52 / 105 / 34 | 188 / 435 / 148 | x 3.6 / 4.1 / 4.4 |

So the whole gap is exchange wait: dm carries about 77 % of it, bs about 22 %, the rest none; compute and init are unchanged. (Phase sums do not add to total because of the unmeasured recip inside dm; see the run line.)

SHMEM pool and heap the plan needs (modelled by the planner, used by the runs), 1.2882e11 digits, 2 nodes:

| | `COMM_OFI=1` | `COMM_OFI=0` |
|---|---|---|
| `COMM_SHMEM_POOL_MB` | 512 (plus 4 comm_ofi pools of 2304 MiB = 9216 MiB device) | 8704 (8192 MiB staging = 4 x 2048 + control) |
| `SHMEM_SYMMETRIC_SIZE` = `XT_SYMMETRIC_HEAP_SIZE` | 1024M | 9216M |
| plan check | OK | OK |

At the target (OPPORTUNITIES, modelled earlier): pool 2304 to 10240 MiB, heap 10752 MiB.

**Verdict.** SHMEM-only is 2.1 x slower than `comm_ofi` at 2 nodes (+145 s typical, about +172 s with the slow run; wait-bound in dm and bs), so a SHMEM-primary target needs the data path fixed first (N7 follow-ups: both NICs, fewer round trips). Keep `COMM_OFI=1` as the default. The pool/heap with `COMM_OFI=0` is ready: 8704 / 9216M at this size and the plan stays OK.
**Open issues.** Only 2 pairs (time budget); the mean's CI is uninformative, the per-run ordering is the evidence. 2 nodes only; the 10-node ratio was 2.1 x (OFI17), consistent. OpenSHMEMX 12.1.0 and both NICs per PE untested. Digits against the reference not compared.
**RESUME.** Raw: aac7 `~/meas/N7/{totals.tsv,stats.txt,log/}`. Nothing running. Next: N7 fix experiments (aac7 2 to 6 nodes) if the user wants them.

## N7B — one SHMEM proxy PE per APU (`COMM_SHMEM_PE_PER_APU=1`): bandwidth, gates, 6-node ABBA

(former file: N7B.md)

Scope: the N7b record (2026-10-14, aac7, 1 to 6 nodes): the switch, its gates, `t_comm` bandwidth and the 6-node full-run
comparison against `comm_ofi` and SHMEM today. Dated record, frozen when written; the code and the central docs win where they differ.

**Question.** One SHMEM PE per node uses one of four NICs (vendor limit). Does one proxy PE per APU (each on its own NIC, the
node-process stays the compute rank) lift the SHMEM-only path (`COMM_OFI=0`) towards `comm_ofi`?

**Change / branch.** Branch `n7b`, merged to main 8acda73 (parameter `COMM_SHMEM_PE_PER_APU`, 0 = off = default, 1 = one proxy PE per
APU; needs `COMM_OFI=0` and more than one node). `ecalc/comm_shmem.c` (data plane in the `comm_ofi` mould: the proxies post the puts
from their own symmetric heaps; the node-process maps those heaps through named `/dev/shm` files, an `mmap` wrapper armed only inside a
proxy's `shmem_init`, because the on-node path of the aac7 library is absent), `ecalc/mnrun.sh` (launch of 1 + 4 PEs per node,
4 NICs, round-robin), `binsplit.c` (pool rule per PE). Design summary: logical PE = node (mn.c, meshes, callers unchanged); proxy d on
NIC d; control words stay on the node-process; value 2 (two NICs per APU, the target) designed, refused at init. Default unchanged.

**Method.**
- B2 (measured): 2 and 6 nodes, `t_comm --bw 4 5 256` (GB/s per node, 2 runs per mode in mirrored order), gates `tools/gate.sh` 1 and 2
  nodes at the defaults and with `--env "COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1"`, a 2-node 1e10 write + compare, two 2-node 1e10 no-write runs.
- B3 (measured): one queue item, 6 nodes, build of main b6ec90c: node screen (`--want-nics 4`, nothing excluded), 1e10 write + compare
  and `gate.sh --nodes 6` (1e9) in the new mode, ABBA of 3.8646e11 digits, no writes, `COMM_XSTATS=1 MN_WAIT_STATS=1`, 4 rounds
  (A B, B A, A B, B A), A = `comm_ofi` (default), B = `COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1`; one anchor run of SHMEM today
  (`COMM_OFI=0 SHMEM_OFI_NUM_NICS=4 COMM_SHMEM_ROUND_MB=256`). Paired B − A, 95 % CI with the t distribution, n = pairs.

#### Results

**`t_comm --bw`, GB/s per node, 256 MiB slabs (measured, 2 runs each):**

| nodes | comm_ofi | SHMEM today | PE per APU |
|---|---|---|---|
| 2 | 21.9 / 21.6 | 13.4 / 15.1 | 15.7 / 15.6 |
| 6 | 30.8 / 39.7 | 8.4 / 8.5 | 28.8 / 29.8 |

At 4 to 64 MiB, 6 nodes: PE per APU 30.0-32.5, comm_ofi 36.8-41.2, SHMEM today 10.2-12.4. The proxies' own put rate is 20.5-22.6 GB/s
per NIC while busy (measured). The B2 gate (≥ 25 GB/s per node at 6 nodes) passes.

**Digits and gates (measured):** gates 1 and 2 nodes, defaults and new mode: 4/4 PASS, identical; 2-node 1e10 written and
compared: identical; 6-node 1e10 written and compared in the new mode: identical (54.8 s); `gate.sh --nodes 6` in the new mode:
PASS, identical (ecalc 21.2 s). `/dev/shm` proxy files left per node after every item and run: 0.

**6-node ABBA, 3.8646e11 digits (measured, total s):**

| round | A comm_ofi | B PE per APU | B − A |
|---|---|---|---|
| 1 | 272.19 | 642.24 | +370.1 |
| 2 | 206.51 | 544.10 | +337.6 |
| 3 | 213.99 | 561.72 | +347.7 |
| 4 | 203.28 | 878.75 | +675.5 |
| mean | 224.0 (sd 32.4) | 656.7 (sd 154.1) | **+432.7, 95 % CI [+174.3, +691.1], n = 4 pairs** |

Paired B − A: bs +146.7 s [+72.5, +220.9]; dm +251.7 s [+66.7, +436.6]; pe 0 exchange wait +999.5 s [+57.4, +1941.6]. All-to-all rate
at pe 0 per APU thread: A 6.5-8.4 GB/s, B 1.07-2.46 GB/s. Anchor, SHMEM today, one run: 719.5 s, 1.34 GB/s (an earlier N7 value:
698.5 s mean of 2). All runs VERIFY OK on all 6 nodes.

#### Verdict
Correct (every digit check identical) and the bandwidth gate passes in `t_comm`, but **do not adopt**: the full run at 6 nodes is
2.9 × slower than `comm_ofi` (CI excludes 0) and about equal to SHMEM today (one anchor run, 9 % under it; no claim). The t_comm gain
(28.8-29.8 GB/s per node, 3.4 × SHMEM today) does not carry into ecalc, where the exchange rate is 1.1-2.5 GB/s per APU thread against
about 7.2 in `t_comm` at 29 GB/s per node. The parameter stays 0.

#### Open issues
- Why the ecalc exchanges run far below the `t_comm` rate in the new mode is not diagnosed (hypotheses only: serialization of the four
  APU threads behind the proxies' command ring and quiet, many smaller exchanges, `/dev/shm` pages in the copy path; init is also 32-68 s
  against 31-43 s).
- Run-to-run spread of the new mode is large (sd 154 s; round 4 878.8 s, no error).
- Value 2 (two NICs per APU, the target) is not built; aac7 has one NIC per APU, so it cannot be measured here.
- `mem_model.py` is not extended for the per-PE pools (the C rule sizes the pool).

#### RESUME
Branch `n7b-b3` (docs/results only); code merged earlier as 8acda73. Next, if wanted: per-phase diagnosis of the exchange rate in the new mode at 2
nodes from COMM_XSTATS lines, before any further 6-node time.

## N7C — SHMEM proxy mode: why it is slow, and `COMM_SHMEM_PX_NUMA=1` + `HSA_ENABLE_SDMA=0` (config C)

(former file: N7C.md)

Scope: the N7C record (2026-10-14, aac7, 2 and 6 nodes): diagnosis of the proxy mode (`COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1`, N7b), the switch
`COMM_SHMEM_PX_NUMA` (default 0), gates and the 6-node ABBA against `comm_ofi`. Dated record; the code and the central docs win where they differ.

**Diagnosis (measured at 6 n in the B3 logs and at 2 n in a 12-run item; modelled where marked).** `t_comm` is host-only and sees only the NICs.
ecalc's exchanges are mostly GPU work on the pool (xGMI pushes, transposes, scatter, `hipMemcpyAsync`). Under `comm_ofi` the pool is
fine-grained device memory on the APU's HBM; in the proxy mode it is the proxy's `/dev/shm` heap: 4 KiB pages (no huge-page route on aac7),
placed by first touch (local to its APU in 6 of 24 pool-runs, m), and copies that touch it go to the SDMA engine (21 GB/s, results/I.md).
Two fixes: F1 `COMM_SHMEM_PX_NUMA=1` (mbind of the proxy heap to the NIC's NUMA node: 32 of 32 pools local, m) and F2 `HSA_ENABLE_SDMA=0` (environment
only; copies by blit kernels). At 2 nodes, 5e10 digits (m, one run each, mirrored order): comm_ofi fabric stage 8.8 s, rounds kinds 7.5 s,
total 55.1-59.3 s; proxy mode 14.7-21.8 s, 12.5-12.7 s, 80.1-99.3 s; with both fixes 9.7 s, 7.6 s, 57.1 s (the first run 136.0 s: slow init, 80 s).
Either fix alone was not enough (NUMA only: fabric 13.5-17.2 s; SDMA off only: worse, 19.7-26.0 s). Remaining causes: init +28-80 s and its skew,
4 KiB pages (admin change), proxy batch-then-quiet coupling (docs: design only).

**Change.** `COMM_SHMEM_PX_NUMA` (0 = off = default; 1 = `mbind` the proxy heaps to the NIC's NUMA node before registration; pe 0 reports
pool page placement and page size). `ecalc/comm_shmem.c`, README and registry rows. `HSA_ENABLE_SDMA=0` is an environment setting and is added to no launch line.

**Test (measured, one queue item, 6 nodes, build of branch n7c; node screen: nothing excluded).** Config C = `COMM_OFI=0
COMM_SHMEM_PE_PER_APU=1 COMM_SHMEM_PX_NUMA=1 HSA_ENABLE_SDMA=0`.
- Gates `tools/gate.sh` (1e9): 1 and 2 nodes with config C: PASS, digits identical; 1 and 2 nodes at the defaults: PASS, identical.
- 1e10 write + compare with config C: 2 nodes identical (44.2 s), 6 nodes identical (39.4 s). All runs VERIFY OK; `/dev/shm` proxy files left per node: 0.
- 6-node ABBA, 3.8646e11 digits, no writes, `COMM_XSTATS=1 MN_WAIT_STATS=1`, 4 rounds (A B, B A, A B, B A); A = `comm_ofi` default, B = config C:

| round | A comm_ofi s | B config C s | B − A s |
|---|---|---|---|
| 1 | 235.05 | 385.08 | +150.0 |
| 2 | 210.39 | 460.17 | +249.8 |
| 3 | 219.02 | 461.66 | +242.6 |
| 4 | 200.91 | 527.96 | +327.1 |
| mean | 216.3 (sd 14.5) | 458.7 (sd 58.4) | **+242.4, 95 % CI [+127.1, +357.7], n = 4 pairs** |

Paired B − A: bs +90.5 s [+20.3, +160.8]; dm +129.3 s [+33.6, +225.0]; pe 0 exchange wait +425.2 s [+30.9, +819.4]. Pe 0 all-to-all rate per
APU thread: A 6.8-8.3 GB/s, B 2.0-4.0 GB/s (per-round values in the item summary).
- One B run with `HSA_ENABLE_SDMA` at its default (`PX_NUMA=1` only): 417.7 s (single run, m; inside B's spread, no claim on the copy share).
- Before (N7b B3, results/SHMEM_ITEMS.md §N7B, other build): proxy mode 656.7 s vs comm_ofi 224.0 s, +432.7 s [+174.3, +691.1].

#### Verdict
Correct (every digit check identical, gates PASS), switch merged off by default. **Not adopted:** at 6 nodes config C is still 2.1 x slower
than `comm_ofi` (+242.4 s, CI excludes 0), against 2.9 x before; the 2-node parity (57.1 vs 55.1 s) does not carry to 6 nodes with 5 peers and layv exchanges.
Placement and SDMA explain roughly 45 % of the gap (modelled from the means, 432.7 to 242.4 s; builds and days differ); the rest is open.

#### Open issues
- 6-node gap of +242 s: the 5-peer lock-step with proxy batch-then-quiet (F4: per-peer contexts or signal put with the data) and the init/skew
  (B init 35.8-74.5 s vs A 31.8-35.4 s) are untested hypotheses. The spread of B is large (sd 58 s).
- Targeted copy kernel instead of the global `HSA_ENABLE_SDMA=0` (F2b) is not built.
- 4 KiB pages need an admin change (`shmem_enabled`, `huge=` on `/dev/shm`) or reserved huge pages.

#### RESUME
Merged to main with the switch off. Next, if wanted: F4 design, per-phase init timestamps at 2 nodes, a 6-node run with `COMM_LAYER_STATS=1`
in config C to see which exchange kinds (layv) still lag.

## N7D — SHMEM proxy mode toward comm_ofi speed at 6 nodes (copy kernel, parallel init, put-with-signal sends)

(former file: N7D.md)

Scope: the N7D record (2026-10-14, aac7, 1, 2 and 6 nodes): where config C's 6-node gap went, three new switches of the proxy mode
(`COMM_SHMEM_PX_KCOPY`, `COMM_SHMEM_PX_INIT`, `COMM_SHMEM_PX_SEND`, all off by default), their gates and the 6-node ABBA against `comm_ofi`.
Dated record; the code and the central docs win where they differ. Labels: m = measured, mod = modelled, a = assumed.

**Question.** Can the proxy mode (`COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1`, results/SHMEM_ITEMS.md §N7B, results/SHMEM_ITEMS.md §N7C) reach `comm_ofi` speed at 6 nodes, and
which fixes carry it? Baseline (results/SHMEM_ITEMS.md §N7C, m): config C (`COMM_OFI=0 COMM_SHMEM_PE_PER_APU=1 COMM_SHMEM_PX_NUMA=1 HSA_ENABLE_SDMA=0`)
458.7 s vs comm_ofi 216.3 s at 6 n, 3.8646e11 digits: +242.4 s, 95 % CI [+127.1, +357.7], 4 pairs.

**Diagnosis (L1, from the N7C logs and one 6-node diagnosis item, m).** The gap was not on the wire: level 1 of the tree (1 peer) was at parity,
the proxies were busy 17 % of the run, and t_comm's 5-peer exchange ran at 0.75 of comm_ofi. Inside ecalc the GPU work on the proxy pool
(`/dev/shm`, 4 KiB pages) was slow and uneven between nodes (block transposes 13-138 s per APU vs 2-4 s under comm_ofi), and ecalc's lock-step
spread one slow node to all. Init cost +17 s per pair (mesh creation with 5 PEs per node, serial pool registration, skew). Run-to-run spread was
large (config C: 385-1588 s).

**Change / branch.** Branch `n7d` = `n7d-copy` + `n7d-init` + `n7d-f4` merged, plus two fixes. All switches default off (digits unchanged).
- `COMM_SHMEM_PX_KCOPY=1` (`ecalc/comm_copy.c`): copies that touch a proxy pool run as a GPU copy kernel instead of on the SDMA engine;
  replaces the process-wide `HSA_ENABLE_SDMA=0`. Fix: `comm.h` declares `comm_copy_async` only outside `COMM_HOST_ONLY` (the host-only
  `tests/t_comm` build failed on `hipError_t`).
- `COMM_SHMEM_PX_INIT=1` (+ `COMM_SHMEM_PX_INIT_THREADS`): every PE pre-faults its pool (`MADV_POPULATE_WRITE`) and the node-process registers
  the four proxy pools and its own in parallel. Fix: the `madvise` result is checked; on an error (printed under `COMM_SHMEM_VERBOSE=1`) the pages
  are touched by an atomic no-op instead. At 6 n the pre-fault of a 2.5 GiB proxy pool took 0.08-0.17 s with no error (m): it works.
- `COMM_SHMEM_PX_SEND=2`: a proxy sends 4 MiB `shmem_ctx_putmem_signal_nbi` chunks straight into the receiving proxy's pool, chunks of the
  queued commands interleaved, one context per peer node retired by its own quiet (no batch quiet, no second hop for the signal).

**Method.** One aac7 queue item (`22_n7d_t1`, 6 nodes, 12:59-14:12 ET, commit 61400d7; node screen: nothing excluded). B = `COMM_OFI=0
COMM_SHMEM_PE_PER_APU=1 COMM_SHMEM_PX_NUMA=1 COMM_SHMEM_PX_KCOPY=1 COMM_SHMEM_PX_INIT=1 COMM_SHMEM_PX_SEND=2 COMM_LAYER_VSLOT_POOL=0
COMM_LAYER_INTER2=0` (no `HSA_ENABLE_SDMA`). `tools/gate.sh --nodes 1, 2, 6` with B; 1e10 write + compare at 2 and 6 n with B; a 2-node KCOPY
pair (1e10, no writes, order C K K C); `t_comm --bw 4 5 256` at 6 n; 6-node ABBA at 3.8646e11, no writes, `COMM_XSTATS=1 MN_WAIT_STATS=1
COMM_LAYER_STATS=1` on every run, A = comm_ofi default; one-factor runs. Earlier 2-node items: `20_n7d_f4`, `21_n7d_init` (branch tests).

**Results.**

Correctness (m): gates 1, 2 and 6 nodes with B: PASS, digits identical (ecalc 10.2, 19.4, 20.1 s). 1e10 write + compare with B: 2 n identical
(53.7 s), 6 n identical (41.9 s). Every completed run VERIFY OK; `/dev/shm` proxy files left per node 0 after every run that ended normally. Branch
tests (m): `COMM_SHMEM_PX_SEND=2` gates 1/2 n PASS + 1e10 identical (item 20); `COMM_SHMEM_PX_INIT=1` gates 1/2 n PASS + 1e10 identical (item 21).

2-node KCOPY pair, 1e10, no writes (m; 2 pairs, mirrored order):

| arm | total s | fabric stage s per APU thread (node 0) | exchange wait xw0 s |
|---|---|---|---|
| C (`HSA_ENABLE_SDMA=0`) | 28.46, 28.63 | 3.82, 3.60 | 14.5, 19.5 |
| K (`COMM_SHMEM_PX_KCOPY=1`) | 29.05, 26.71 | 3.41, 3.29 | 10.6, 13.5 |
| K − C, mean [95 % CI] | −0.66 [−16.6, +15.3] | −0.36 [−0.89, +0.18] | −4.95 [−18.3, +8.4] |

The copy kernel matches `HSA_ENABLE_SDMA=0` (fabric stage 0.36 s lower, inside the spread).

t_comm `--bw 4 5 256`, 6 n, all peers, GB/s per node (m; one run each, order C X X C; X = `COMM_SHMEM_PX_SEND=2`):

| slab | C (runs 1 / 4) | C + X (runs 2 / 3) |
|---|---|---|
| 4 MiB | 34.43 / 34.61 | 33.40 / 31.47 |
| 64 MiB | 34.13 / 30.50 | 33.25 / 31.67 |
| 256 MiB | 28.31 / 26.34 | 31.07 / 32.05 |

6-node ABBA, 3.8646e11 digits (m; 3 complete pairs, orders A B, B A, A B):

| round | A comm_ofi s | B s | B − A s |
|---|---|---|---|
| 1 | 192.91 | 292.20 | +99.3 |
| 2 | 184.49 | 247.78 | +63.3 |
| 3 | 197.37 | 284.38 | +87.0 |
| mean | 191.6 | 274.8 (sd 23.7) | **+83.2, 95 % CI [+37.7, +128.7]** |

Paired B − A per phase (m, 3 pairs): bs +37.7 s [+7.9, +67.5], dm +39.6 s [+24.9, +54.4], init +2.1 s [−2.0, +6.1], block transposes (max over
nodes) +0.13 s per APU [−0.33, +0.59] (A 2.4 s, B 2.6 s; was 13-138 s under config C), fabric stage node 0 +19.1 s [−0.8, +38.9].
The gap to comm_ofi fell from +242.4 s (results/SHMEM_ITEMS.md §N7C) to +83.2 s: B is 1.43 × comm_ofi (config C was 2.1 ×); B's spread fell from 385-1588 s to
248-292 s. Init is at parity with comm_ofi.

One-factor runs, 6 n, 3.8646e11 (m, one run each, no claim):

| run | total s | bs | dm | init | block transposes max s per APU |
|---|---|---|---|---|---|
| B (mean of 3) | 274.8 | 115.6 | 116.2 | 35.7 | 2.6 |
| B without `COMM_LAYER_VSLOT_POOL=0 COMM_LAYER_INTER2=0` | 244.4 | 97.8 | 105.3 | 35.2 | 4.4 |
| B with `COMM_SHMEM_PX_SEND=1` | 318.1 | 142.3 | 124.2 | 45.3 | 7.0 |

With KCOPY + NUMA + PX_INIT the v-slots in the pool cost little (transposes 4.4 s): the LAYER switches showed no gain (one run inside B's range).
`PX_SEND=2` may help (318 vs 248-292 s), one run.

Round 4 did not complete: during R4's B run a node failed (Slurm: node failure, the node then down, "Not responding", 13:56 ET) and the following
comm_ofi run timed out on the broken allocation; the item stopped there (its rule: any timeout). No digit check failed. Cause unknown
(site-side; the pre-fault adds 4 x 2.5 GiB per node of host pages, which the plan already counts).

**D31 (b): config C as the `COMM_OFI=0` default (branch `n7d-def`).** Condition: the copy kernel's exchange stage equals config C with
`HSA_ENABLE_SDMA=0` (2-node pair above: fabric −0.36 s, total −0.66 s, inside the spread) and its gates pass (1/2/6 n above): met. Change: under an
explicit `COMM_OFI=0`, `mnrun.sh` launches the proxy mode (`COMM_SHMEM_PE_PER_APU=1`) for a multi-node SHMEM run of one node-process per node, and
`COMM_SHMEM_PX_NUMA` and `COMM_SHMEM_PX_KCOPY` default to 1 in the proxy mode; explicit settings win (`COMM_SHMEM_PE_PER_APU=0` = the plain SHMEM
path); `COMM_OFI` unset or 1 (comm_ofi) is unchanged; `ecalc/mem_model.py` mirrors the default. The LAYER switches stay at their defaults (no gain
in the one-factor run). Check (aac7 item `23_n7d_d31`, 2 nodes, commit f50d57f, m): `tools/gate.sh --nodes 1` and `--nodes 2` with plain
`COMM_OFI=0` PASS, digits identical; `--nodes 2` at the defaults PASS; 1e10 write + compare at 2 n with plain `COMM_OFI=0`: identical (44.2 s), the
log shows the proxy mode launched and 8 of 8 proxies with NUMA placement; `/dev/shm` files left 0.

**Verdict.** Merge the three switches, default off (gates and digits pass). The proxy mode is still slower than comm_ofi at 6 n (+83 s, CI excludes
0): comm_ofi stays the default. The copy kernel equals `HSA_ENABLE_SDMA=0`, so config C can be expressed without a process-wide environment setting
(D31 (b)). The LAYER switches are not needed.

**Open issues.** The remaining +83 s is in bs and dm exchange waits (xw0 +260 s summed over phases of pe 0, m), not in transposes or init; PX_SEND=2
vs 1 and LAYER on/off need paired runs; the node failure in R4 is unexplained; 4 KiB pool pages remain (admin change).

**RESUME.** `n7d` merged to main (switches off); D31 merged (`n7d-def`). Raw logs on the cluster (queue item 22 output). Next: more pairs of B vs comm_ofi if the user wants a decision on the proxy mode.

## N7E — the N7D proxy-mode switches at 6 nodes: paired repeat (PX_SEND=2, PX_INIT=1) against comm_ofi

(former file: N7E.md)

Scope: the N7E record (aac7 item n7e, 2026-10-14, 16:39 to 22:55 ET, commit 2f34f01, 6 nodes, 3.8646 × 10¹¹ digits): a paired ABBA
repeat of the proxy mode with `COMM_SHMEM_PX_SEND=2` and `COMM_SHMEM_PX_INIT=1` against `comm_ofi`, and the two switch-only
comparisons. Dated record; the code and the central docs win where they differ. Labels: m = measured, mod = modelled, a = assumed.

**Question.** Do the N7D switches `COMM_SHMEM_PX_SEND=2` and `COMM_SHMEM_PX_INIT=1` bring the proxy mode (`COMM_OFI=0`) to `comm_ofi`
speed at 6 nodes, and is the 1.43 × gap of results/SHMEM_ITEMS.md §N7D (config C plus N7D switches, 3 pairs) reproduced?

**Change / branch.** No code change. Branch `n7e` (worktree `../ntt-wt-n7e`), records only. Switches measured, all off by default
(unchanged): `COMM_SHMEM_PX_SEND` (default 1), `COMM_SHMEM_PX_INIT` (default 0).

**Method.** Launch line (all arms): `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto MN_T_CHUNK_MB=2048
COMM_SHMEM_ROUND_MB=1024 ECALC_MEM_GUARD_GB=6 ECALC_DEVICE_GB=444.0 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_LOG_CLOCKS=1`, plus the arm's
settings. Arms: A = `comm_ofi` (`COMM_OFI_VERBOSE=1`); B = `COMM_OFI=0 COMM_SHMEM_PX_SEND=2 COMM_SHMEM_PX_INIT=1`; I1S1 = `COMM_OFI=0
PX_SEND=1 PX_INIT=1`; I0S2 = `COMM_OFI=0 PX_SEND=2 PX_INIT=0`. Under an explicit `COMM_OFI=0` the N7D defaults of D31 apply to every
proxy arm (PX_NUMA and PX_KCOPY on) (assumed from TASKS/D31; the launch lines in the summary do not list them). Gate runs first at
1/2/4/6 nodes, 1e9 digits: all PASS, digits identical (summary). Totals per arm are the mean over runs with status ok; paired
differences are B − X per ABBA pair, mean with 95 % CI = mean ± t(n−1)·sd/√n. Overall: 83 of 83 recorded runs ok, 0 bad (summary);
1 planned run skipped for time. Per-arm wait columns are max over nodes (wbs / wdm / wrc: the bs, dm and reciprocal exchange waits).

**Results (6 n, 3.8646 × 10¹¹ digits; all times seconds; measured unless marked).**

| arm | n | total mean | sd | bs | dm | wbs | wdm | wrc | init |
|---|---|---|---|---|---|---|---|---|---|
| A comm_ofi | 29 | 191.0 | 15.1 | 76.6 | 78.0 | 145.0 | 164.6 | 56.9 | 32.3 |
| B PX_SEND=2, PX_INIT=1 | 35 | 348.3 | 111.1 | 130.3 | 175.3 | 322.2 | 401.2 | 189.7 | 35.6 |
| I1S1 PX_SEND=1, PX_INIT=1 | 4 | 421.7 | 58.5 | 161.2 | 212.1 | 364.1 | 441.2 | 246.8 | 39.4 |
| I0S2 PX_SEND=2, PX_INIT=0 | 3 | 313.3 | 43.5 | 115.3 | 158.5 | 279.7 | 342.1 | 192.8 | 35.2 |

Paired differences (negative = B faster):

| comparison | n pairs | total mean, 95 % CI | bs | dm | xw0 (exchange wait) | init |
|---|---|---|---|---|---|---|
| B − A (comm_ofi) | 28 | **+129.3 s, [+92.9, +165.7]**, excludes 0 | +43.6 [+30.6, +56.6] | +79.3 [+55.0, +103.5] | +338.8 [+251.2, +426.4] | +3.7 [+2.7, +4.6] |
| B − I1S1 (PX_SEND 2 vs 1) | 4 | +69.9 [−217.8, +357.6], n.s. | +13.3 [−100.7, +127.3] | +60.1 [−135.0, +255.1] | +154.0 [−526.9, +834.9] | −4.6 [−19.8, +10.5] |
| B − I0S2 (PX_INIT 1 vs 0) | 3 | +107.2 [−50.5, +264.9], n.s. | +50.1 [−52.6, +152.8] | +57.8 [−5.2, +120.8] | +250.8 [+82.1, +419.4], excludes 0 | −1.3 [−2.7, +0.1], n.s. |

Derived (measured): B / A = 1.68 × (paired, 190.8 s comm_ofi mean). Earlier config C with the N7D switches (results/SHMEM_ITEMS.md §N7D, 3 pairs):
274.8 s, 1.43 × comm_ofi, +83.2 s [+37.7, +128.7]. Spread of B is 111.1 s sd against 15.1 s for A.

**Verdict.** Not adopted; the defaults stay `COMM_SHMEM_PX_SEND=1`, `COMM_SHMEM_PX_INIT=0`. The gap to `comm_ofi` at 6 nodes is
+129.3 s (1.68 ×), wider than the 1.43 × (+83.2 s) of N7D: the N7D gap closure is **not reproduced**. The PX_SEND=2 vs 1 and
PX_INIT=1 vs 0 point estimates are not faster and their CIs include 0 (PX_SEND: n = 4; PX_INIT: n = 3). PX_INIT saves about
1.3 s of init (95 % CI [−2.7, +0.1], n.s.), so the pre-fault switch does not help either. The exchange waits remain the cost:
B's xw0 is +338.8 s over A (CI excludes 0). Needs more pairs to resolve PX_SEND and PX_INIT; a larger run is not justified by these data.

**Open issues.** (1) The source of the 111 s sd of the B arm (run-to-run spread on the proxy path, not located). (2) PX_SEND=2 and
PX_INIT=1 at more pairs (total CI half-widths 288 s at n = 4 for PX_SEND, 157 s at n = 3 for PX_INIT; measured). (3) Whether B carries the D31 defaults PX_NUMA and PX_KCOPY exactly as the N7D
config C did (assumed, see Method). (4) One planned run was skipped for time.

**RESUME.** Branch `n7e` off `main` 0a9d778; this file and the RESULTS, OPPORTUNITIES and TASKS rows are the record. No code changed.
Raw summary and tables stay on the cluster (`~/day/n7e/`), not in the repository. Next step, if the user schedules it: a paired
ABBA of B against A with more pairs, only on the user's word (aac7 budget, CLAUDE.md).

## N7E_V — N7E-V — COMM_SHMEM_PE_PER_APU=2 (two helper PEs per APU): correctness record

(former file: N7E_V.md)

Scope: the correctness gates of value 2 on aac7 (item n7v, 2026-10-14, commit 97275a2, 1 and 2 nodes, 1e9 and 1e10 digits). Dated
record; the code and the central docs win where they differ. Labels: m = measured.

**Question.** Does `COMM_SHMEM_PE_PER_APU=2` (1 + 8 PEs per node; an APU's staging and slabs interleaved in 4 MiB blocks over its two
helper PEs' heaps, `ecalc/comm_shmem.c` `px_map2`) give identical digits? (aac7 has one cxi NIC per APU, so both helpers share it: no
speed claim is made here.)

**Failure found, fixed.** The first 2-node gate (commit b989a6b) stopped with `the heap of helper PE N is not reachable ... too small
or unaligned`. Cause (m): `shmem_malloc` put the pool at offset 1920 mod 4096 in the helper's heap file, and `px_map2` maps the heap
files page by page (the offset is not a page multiple). Fix: with value 2 the pool is `shmem_align`ed to 2 MiB (every PE alike).
Value 1 (`shmem_ptr` or one whole-file mapping) is unchanged.

**Results (m).**

| run | result |
|---|---|
| tools/gate.sh --nodes 1, value 2 | PASS, VERIFY OK, digits identical, 1e9 |
| tools/gate.sh --nodes 2, value 2 | PASS, VERIFY OK, digits identical, 1e9; 9 PEs per node in the launch log |
| 1e10 at 2 nodes, written and compared, value 2 | identical, VERIFY OK, ecalc total 47.8 s (correctness context only) |
| `/dev/shm/ecalc_px_*` files left after each run | 0 on both nodes |

Default stays value 1; `COMM_SHMEM_PX_SEND=2` is refused with value 2.

## N7G — COMM_SHMEM_PX_THP and COMM_SHMEM_PX_PIN on the proxy mode: pairs and verdict

(former file: N7G.md)

**Question.** Do huge pages (`COMM_SHMEM_PX_THP=1`) and pinned proxy pools (`COMM_SHMEM_PX_PIN=1`) shorten the 6-node SHMEM
proxy run against the default `COMM_OFI=0` path? Both switches default to 0.

**Change / branch.** Branch `n7g`, commit 006a80f (switches `COMM_SHMEM_PX_THP`, `COMM_SHMEM_PX_PIN`, default 0). Gates on
`n7g` passed before the pairs (m).

**Method.** Three interleaved pairs at 6 nodes, 3.8646e11 digits. Arm A: `COMM_OFI=0` (defaults). Arm B: A plus
`COMM_SHMEM_PX_THP=1 COMM_SHMEM_PX_PIN=1`. Difference is B − A, paired, 95 % CI from the t distribution with n = 3.
Driver and tables on aac7 (`n7g_pairs/tables.txt`); not copied here.

**Re-gate of item 36 on `n7f` (commit 76c8742).** Gates pass 11 of 11 at 1 and 2 nodes, each switch alone and all together;
2-node 1e10 compare identical (m). Summary on aac7 (`n7g_regate/summary.txt`).

**Results (m).**

| arm | runs | mean (s) | sd (s) | pair roffmax (s) |
|---|---|---|---|---|
| A: defaults (`COMM_OFI=0`) | 3 | 432.1 | 83.6 | 652 / 979 / 670 |
| B: + THP + PIN | 3 | 307.2 | 93.7 | 252 / 540 / 347 |

B − A = −124.9 s, 95 % CI [−460.6, +210.8], n = 3 pairs (m). Not significant.

**Page size (m).** The helper pools are 4 KB pages. Shared-memory huge pages are unavailable on this system.
`MADV_COLLAPSE` works on the pool and costs 0.29 s per GiB.

**Verdict.** Not significant. The default stays 0 for both switches. THP and PIN need more pairs before any adoption
decision (the three pairs have a CI wider than the effect).

**Open issues.** The run-to-run spread is large (sd 84 to 94 s on 3 runs); the open N7F item covers the same kind of spread. Four more
pairs of THP-only against the default are not run (aac7 budget, needs the user's word). The effect of PIN alone is not
separated from THP.

**RESUME.** Branch `n7g`, commit 006a80f plus the N7G docs commit. Done: results, RESULTS row, OPPORTUNITIES note, TASKS
rows. Next (not run): 4 more pairs, THP only against the default, at 6 nodes (queue via the aac7 runner, paired ABBA,
≤ 4 paired rounds per the project rule). Decision D35: merge defaults? No; both switches stay off.

#### Item 37 (2026-10-15, gated): THP only, 4 pairs at 3.8646e11

Four interleaved pairs at 3.8646e11 digits. Arm A: default SHMEM. Arm B: `COMM_SHMEM_PX_THP=1` only (PIN left at 0).
Measured wall times: A mean 576.5 s (sd 155.5), B mean 288.8 s (sd 63.0). Paired B − A = −287.8 s, 95 % CI [−507, −68],
n = 4; B faster in 4 of 4 pairs (m).

Gates PASS: 1 and 2 nodes with THP, kill default on and off, comm_ofi 2-node gate, 6-node 1e10 compare identical (m).
Switch stays default 0 pending user decision D36.

#### Item 38 (2026-10-15, gated): default flip, D36 (a)

Build aedc13c, no PX_* variables set. Ran 20:52 to 21:18 ET (27 min, measured).

- 1 node, COMM_OFI=0: gate PASS, digits identical (m).
- 2 nodes, COMM_OFI=0: gate PASS, digits identical (m).
- 6 nodes, 1e10 digits: VERIFY OK, compare identical (m).
- 6 nodes, 3.8646e11 digits, 2 interleaved pairs: A = default (THP on), B = COMM_SHMEM_PX_THP=0.

| pair | A (default) | B (THP=0) | B - A |
|---|---|---|---|
| 1 | 237.4 s | 337.3 s | +99.9 s |
| 2 | 228.8 s | 544.3 s | +315.5 s |

Mean B - A +207.7 s (sd 152.5, n = 2, 95 % CI [-1162, +1578]; not significant alone). Direction matches item 37 (4 of 4 pairs). THP was active in A (huge helper pools 2562 MiB per node) and absent in B (m).

Verdict: PASS. D36 (a) applied: COMM_SHMEM_PX_THP defaults to 1 in the COMM_OFI=0 proxy mode.

#### Items 39 and 41 (2026-10-15/16): huge-page pairs, 6 nodes, 3.8646e11 (measured)

- Item 39: 4 pairs, default on vs COMM_SHMEM_PX_THP=0: B - A +28.4 s (95 % CI -27 to +84).
- Item 41: 4 pairs, same design: B - A +163.5 s (95 % CI -45 to +372). Default on: 290 s mean; off: 475 s.
- Both drivers hit their timeouts after the last run; tables are from the summary files, not DONE markers.
- Pooled over the 14 pairs of items 37, 38, 39 and 41 (the mean of the pair means, weighted by pairs): about 167 s saved by the default (modelled; the runs mix conditions).
- Item 40 (heap switch, COMM_SHMEM_HEAP_THP=1): gates PASS at 1 and 2 nodes and 6-node 1e10 identical; 2 pairs B - A +24.8 s (CI -179 to +229): no gain, stays 0.
- Item 40 task-3 arm (COMM_SHMEM_OFFS_WITH_DATA=64 COMM_SHMEM_MESH_PAR=1): 2 pairs, +49 s: no gain, stays 0.
- Verdict: huge-page default stays on; heap and task-3 switches stay off.

#### Items 45-48 (2026-10-16/17): libfabric baseline and default-run spread (measured)

- Libfabric (COMM_OFI unset), 6 nodes, 3.8646e11, 4 runs (item 45): 178, 181, 186, 193 s; mean about 184 s.
- Default SHMEM (huge pages on), item 46, 3 runs: 254, 256, 289 s; THP off, 1 run: 446 s.
- Default SHMEM, items 47 and 48, 8 runs: 221-397 s (sd about 60 s). The spread is larger than the gap between the default and libfabric means.
- Compaction-stall increase per node during each run (items 48, four default runs): the largest per run was 6770, 4170, 708 and 501. The slowest run (339 s) had the largest value, but the second-fastest (240 s) had the second largest, so no link is shown (n = 4).
- Straggler (item 43 analysis, n = 8): one node finishes late and the other five wait in layv; cause not identified.
- Drivers 45-48 hung at exit after their runs (drivers without a done-marker); fixed for 48 by writing a done-marker before the exit path.
- Verdict and the consolidated story (phase breakdown, switch table, variance, open items): results/SHMEM.md.

#### Items 49-51 (2026-10-17): layered-exchange switch and per-PE timestamps (measured)

- Layered-exchange switch COMM_SHMEM_LAYV_ANY (item 49-50, 4 paired ABBA runs at 3.8646e11): B - A per pair -19.6, -1.3, -101.6, -57.5 s; mean -45.0 s, 95 % CI about [-116, +26]; switch-on faster in 4 of 4 pairs; not significant. Gates PASS at 1 and 2 nodes; 1e10 at 6 nodes identical.
- Timestamp runs (item 51, 4 default runs): wall 223, 264, 186, 235 s.
- Timestamp analysis and the variance findings: results/SHMEM.md.

## SHM - the SHMEM pool under `COMM_OFI=0` is too small at 6 nodes (bug A of NIGHT)

(former file: SHM.md)

**Question.** Why does a 6-node run at 1e10 digits with `COMM_OFI=0` (SHMEM carries the device exchanges) stop after
init on every rank with rc 3, "the symmetric pool (2816 MiB) cannot hold 87 MiB more (in use 372 MiB: control 24,
staging 0, symmetric buffers 348)" (results/NIGHT_RUNS.md §NIGHT, known failure A), when 2 nodes pass; and what pool does the planner
have to give `COMM_OFI=0` at every node count?

**Change / branch.** Branch `shm` (commits c5aae92c, 30891e2b and the docs commit). No new switch: the pool size is a
model output, digits cannot change. `COMM_OFI=1` (the default transport) is unchanged.

- `ecalc/binsplit.c` `binsplit_shmem_pool_need`: under `COMM_OFI=0` the v-slots of the four APU threads are added to the
  pool (`sym += 4 x vsp`) also when `COMM_LAYER_VSLOT_RELEASE=1` (the default); before, M3 (results/C06.md) took
  `max(staging, 4 x vsp)`. The `plan pool` line now names the slots (`v-slots <MiB> = 4 x <MiB>`).
- `ecalc/mem_model.py` `shmem_pool`: the same law.
- `ecalc/README.md`, `COMM_LAYER_VSLOT_RELEASE` row: the "larger, not the sum" rule holds under `COMM_OFI` only.

**Cause.** comm_shmem cuts the pool into four staging regions at its end, one per APU, each the model's per-APU staging
(`COMM_SHMEM_STAGE_SLOT_MB`, set by the pool rule; added against fragmentation), and a general area in front. Staging
blocks go to their APU's region; the v-slots (`comm_sym_alloc`, kind "symmetric buffers") come from the general area
only. The pool rule still used M3's "staging and slots share the pool in time" law, `max(4 x staging, 4 x slots)`, which
no longer holds once staging is in its own regions: the general area was left at control + mailbox + the 256 MiB margin.
At 6 nodes, 1e10 (m, the failing log): pool 2816 MiB = 4 regions of 591 MiB + a general area of 452 MiB; 8 slots of
87 MiB (one pair per APU thread, `COMM_LAYER_VSLOT_MB_6=87`) + 24 MiB control need 720 MiB, so the 5th slot (ranks 0-3)
or the 6th (ranks 4-5, 16 MiB control) found no room. At 2 and 4 nodes the general map has no group that is not a power
of two, so there are no v-slots (0 MiB, mod) and the bug cannot show; it hits every node count with a cut group (3, 5, 6,
..., 576). Not a split per thread, a cap or alignment: a model law that the region layout had made wrong. mnrun.sh was
right: it hands the command's `COMM_OFI=0` to the planner, which sized the pool for `COMM_OFI=0`.

**Method.** Planner: `MN_PLAN_ONLY=<digits>:<nodes> ./ecalc <digits>` and `tools/plan_values.py` (the target template
with `__FILL__` replaced by placeholders) on the aac7 login node, builds of `origin/main` b1e4d567 (base) and of the
branch (fix), both `make -B SHMEM_CRAY=1` with `-DCOMM_SHMEM`. Runs: aac7 queue item `50_shm_pool.sh` (6-node hold),
launch line of `profiles/aac7.conf` + `COMM_OFI=0`, via `mnrun.sh` (pool from the planner): base at 6 nodes, 1e10 (the
reproduction); fix at 6, 4 and 2 nodes, 1e10, digits written and compared with the first 1e10 digits of the 1e11
reference (`tools/unpack_digits` | `cmp -n`); then `tools/gate.sh --build --nodes 2` and `tools/gate.sh --nodes 1`
(defaults, the aac7 line). Collect: `ssh <cluster> 'cat ~/shm/summary.txt'` (access: see the private protocol).

**Results.**

Pool per node-process from the planner (MiB, mod; the pool is host memory, the heap is the pool + 512):

| digits, nodes | `COMM_OFI=0` base | `COMM_OFI=0` fix | of which v-slots (fix) | `COMM_OFI=1` (both) |
|---|---|---|---|---|
| 1e10, 2 | 4608 | 4608 | 0 | 512 |
| 1e10, 4 | 3584 | 3584 | 0 | 512 |
| 1e10, 6 | 2816 | 3328 | 688 = 4 x 172 | 512 |
| 3.82e13, 576 (target template) | 10240 | 17664 | 7312 = 4 x 1828 | 2304 |

At the target the `COMM_OFI=0` pool grows by 7424 MiB (7.25 GiB) per node (mod); `plan check` stays OK (1240 products).
The base value 10240 MiB at the target (results/TGT.md) would have failed the same way: a general area of about 2 GiB
for 1.6 GiB of control and 7.1 GiB of slots (mod).

Runs (aac7, measured, 1e10 digits, `COMM_OFI=0`, aac7 launch line; queue item `50_shm_pool.sh`, summary `~/shm/summary.txt`; times ET):

| run | nodes | rc | VERIFY | digits | total | pool (MiB) |
|---|---|---|---|---|---|---|
| base `COMM_OFI=0` (reproduction, 09:50) | 6 | 3 | NO | not written (init failed) | - | 2816 |
| fix `COMM_OFI=0` (09:53) | 6 | 0 | OK | identical | 40.28 s | 3328 |
| fix `COMM_OFI=0` (09:55) | 4 | 0 | OK | identical | 39.20 s | 3584 |
| fix `COMM_OFI=0` (09:57) | 2 | 0 | OK | identical | 47.66 s | 4608 |
| `gate.sh --build --nodes 2` (defaults, 1e9, 09:58) | 2 | 0 | OK | identical | 91 s gate (ecalc total 19.10 s) | - |
| `gate.sh --nodes 1` (defaults, 1e9, 09:59) | 1 | 0 | OK | identical | 24 s gate (ecalc total 10.80 s) | - |

The base run reproduces the NIGHT failure (rc 3 on every rank after init, pool 2816 MiB). The fix passes at 6, 4 and 2
nodes with byte-identical digits. Timings are single runs per node count (measured; no ABBA pair, so no timing claim).

**Verdict.** Fixed. The 6-node `COMM_OFI=0` run that failed on main now completes with VERIFY OK and identical digits,
and the 4- and 2-node runs and both gates pass. The fix changes only the model's pool size (digits cannot change); no
switch was needed. Merged to `main` 2026-10-13.

**Open issues.**
- Memory cost under `COMM_OFI=0`: the slots and the staging no longer share space (+7.25 GiB host per node at the target,
  mod). Getting M3's sharing back means letting a thread's slots use its own staging region (they are not live at the
  same time under `COMM_LAYER_VSLOT_RELEASE=1`); that would be a behaviour change behind a switch, not done here.
- The model's control (14 MiB at 6 nodes) is below the measured in-use control (24 MiB); the 256 MiB margin covers it.
- 3 and 5 nodes (cut groups, slots present) not run.

**RESUME.** Done: runs in, merged to `main`, branch `shm` deleted (local and GitHub), worktrees and `~/shm/bin` removed.
Nothing open except the 3- and 5-node runs and the memory cost above (not blocking).
