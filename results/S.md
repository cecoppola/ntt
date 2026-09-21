# S — the SHMEM transport (M8-s), strided PE sets per tree level, the dragonfly third layer (2026-09-20)

Branch `s11` (from `main` @ 72aa2e9). PLAN.md §25–26 agent S.
Files: `ecalc/comm_shmem.c` (new), `comm.h` (additive), `mn.c` (communicator creation, `MN_TOPO_GROUP`), `mn.h` (one
accessor), `mnrun.sh`, `Makefile` (`SHMEM=1`), `tests/t_comm.c`, `tests/t_dist.c` (the `COMM_RANK` mode picks the SHMEM
communicator under `COMM_TRANSPORT=shmem`; two lines), `comm_layered.c` (the intra-minor form for the third layer —
outside my list; the major form's code paths are unchanged apart from `NA` → `p->na` and the device index).
`rns_dist.c`, `newton_db.c`, `ntt_dist.c` untouched. Size 1 untouched (nothing in the size-1 path calls the new code;
`shmem_init` is only called under `COMM_TRANSPORT=shmem`).

## 1. The transport (`comm_shmem.c`)

Selected by `COMM_TRANSPORT=shmem` (default stays TCP). One PE per node-process; rank and size from
`shmem_my_pe() / shmem_n_pes()`; `COMM_RANK/SIZE/HOSTS/PORT` are not needed.

### The OpenSHMEM subset used (1.4; nothing from 1.5)

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

### Memory: one pool, receiver-chosen offsets

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

### The push model

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

### Threads

`SHM_LOCK`: a no-op when the library provides `SHMEM_THREAD_MULTIPLE` and `COMM_SHMEM_SERIAL=0`; otherwise one
process-wide mutex around every library call, with the waits as polled `shmem_long_test` under short holds. **The
default is serial**: OSHMEM 4.1.6 returns `provided = MULTIPLE` but four threads in `shmem_long_wait_until` crash in
`spml_ucx_ctx_progress` (probe on aac6, results below). The target's Cray SHMEM has real MULTIPLE: run with
`COMM_SHMEM_SERIAL=0` there, which also gives each communicator its own context (under the lock the default context
serves; see the trap list). Cost of the lock on aac6: none measurable at these sizes (the puts are memcpys through
shared memory; the lock is held per put, not per wait).

### What the target needs / what changes there

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

## 2. `mnrun.sh`

`COMM_TRANSPORT=shmem`: the same placement (the largest node count dividing `<procs>`, block distribution), launched
by `srun --mpi=pmix` (OSHMEM's PMIx client; direct launch works, `oshrun` is not needed — and not on the login node),
each process wrapped in `setarch x86_64 -L`, with `SHMEM_SYMMETRIC_HEAP_SIZE = COMM_SHMEM_POOL_MB + 512 MiB` and
`OMPI_MCA_memheap_base_max_segments=64`. TCP unchanged, except that the random port base is now drawn from
20000–26000: 20000–50000 reached into the ephemeral range (32768–60999) and a listener collided with an outgoing
connection's port once in ~4 runs at 8 processes (`bind: Address already in use`, one process exits, the rest hang).

Build: `make` detects `oshcc` (`SHMEM=1`; `-DCOMM_SHMEM` + `oshcc --showme:compile`, link `oshcc --showme:link
-lopen-rte -lopen-pal` — hipcc's lld does not follow liboshmem's own dependencies); `make SHMEM=0` builds without it
(the entry points abort with a message). `tests/t_comm` is host-only as before, now with `comm_shmem.c` in it.

## 3. The dragonfly third layer (`MN_TOPO_GROUP=T`)

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

## 4. Tests (aac6, one node ppac-pl1-s24-16, jobs 20806 / 20812; the commands from `~/ntt-s/ecalc`)

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

## 5. Traps found (for whoever touches this next)

1. The OSHMEM three above (`setarch -L`, quiet-not-fence, serial calls). All three are runtime switches or launcher
   wrappers; nothing in the code depends on OSHMEM.
2. `hipcc` + `oshcc --showme:link` needs `-lopen-rte -lopen-pal` added (lld).
3. The TCP port base must stay below 32768 (above: intermittent `bind` failures).
4. A communicator id may be used once per run (the mailbox row is never cleared; creation aborts on reuse) — `mn.c`'s
   ids are per (slot, lane) and every group is created once.

## 6. The regression over SHMEM

`COMM_TRANSPORT=shmem ./mnaccept.sh 20817 --only e9,mn` and `--only unit,ckpt` (two halves for the 45-minute batch), at
8441d38: **16 passed, 0 failed** — unit (t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_mn_grid
at 2 node-processes over SHMEM), e9 size 1 both bases identical (15.7 s / 25.7 s), mn 10⁸ at 2, 3, 4 and 10⁹ at 2, 4
identical with every node VERIFY OK (22.4 / 19.3 / 18.5 s; 43.9 / 43.8 s), ckpt 10⁸ at size 2 (abort at leaf level 6,
restart identical). `mnaccept.sh` passes the environment through (`srun --export=ALL`): no change to it. Logs in
`results/mnaccept/20817` of `~/ntt-s` on aac6. (An earlier pass at d8949bf had e9 size 1 failing: `mn_init` called
`shmem_init` for a single process with no launcher; fixed in 8441d38 — a single process stays size 1 as under TCP.)

## 7. Open issues

* 2–3 real nodes over SHMEM not exercised (node availability); the only node-count-dependent code is the placement
  in `mnrun.sh`, shared with TCP, and OSHMEM's inter-node UCX/TCP path. First thing for the integrator when two nodes
  are idle: `./mnrun.sh 2 ./tests/t_comm` and the 5 × 10⁹ two-node run.
* Staging: the transport copies every slab through the pool (as TCP through pinned buffers). On the target the
  callers' slabs should live in the pool (see §1); pool memory is a run parameter until then.
* `COMM_SHMEM_SERIAL=0` and the device heap are code paths with no implementation to test them on here.
* L's `MN_GROUPS` schedule: `mn_group_at` still uses `2^level`; the group's `(g0, g)` is the only thing the SHMEM
  meshes need, so L's parser plugs in at that one line.
