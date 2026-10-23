# SHMEM_ITEMS — the SHMEM fallback transport: pool sizing and strided PE sets

Scope: per-item records of the OpenSHMEM transport `COMM_TRANSPORT=shmem` (`ecalc/comm_shmem.c`): the design and portability contract (§S, §S12), the symmetric pool and its sizing law (§P214, §V114, §SHM). The proxy-mode records (§N7, §MEAS-N7, §N7B-§N7G) were archived 2026-10-23 (`archive:results_merged_2026-10-23/SHMEM_ITEMS_full.md`); the summary of that work is `results/SHMEM.md`. Condensed from the former per-item files (old IDs resolve through `results/INDEX.md`); raw data and drivers in `archive:results_raw_2026-10-19/`. Labels: m = measured, mod = modelled, a = assumed.

## Topic summary

- **Live conclusions.** (1) Pool = 4 APU threads x (send + receive) of the largest staged exchange; the law is exact (§P214) and rounds cut the second copy (§V114: 1e10 on 2 nodes pool 8194 -> 2050 MiB, total not slower m). (2) Under `COMM_OFI=0` the v-slots must count in the pool (§SHM). (3) The third layer (`MN_TOPO_GROUP`) costs 16-20 % on one node and stays off (§S12); `DIST_MN_SYM_SLABS=1` measured and kept off (§P214). Three Phase 11 OSHMEM traps (`setarch -L`, quiet not fence, serial calls) remain launcher or runtime switches only.
- **Current state.** The default multi-node transport is xshmem (`results/XSHMEM.md`); this transport is the fallback and the reference (`COMM_SHMEM_ROUND_MB=128` on the aac7 launch line, Q60). The proxy mode and its `PX_*` switches, `COMM_SHMEM_PEER_ORDER` and `COMM_SHMEM_LAYV_ANY` are removed.

| item | title |
|---|---|
| S | the SHMEM transport (M8-s), strided PE sets per tree level, the dragonfly third layer (2026-09-20) |
| S12 | the SHMEM transport's target forms, tested for real on Sandia OpenSHMEM (2026-09-21) |
| P214 | the SHMEM pool: measured, modelled, sized (PLAN §33 A2, TASKS 2.4, TARGET_TASKS T0) |
| V114 | the SHMEM pool by default, and the exchanges in rounds (Phase 14 agent V1) |
| SHM | the SHMEM pool under `COMM_OFI=0` is too small at 6 nodes (bug A of NIGHT) |

## §S — the SHMEM transport (M8-s), strided PE sets per tree level, the dragonfly third layer (2026-09-20)

Branch `s11` (from `main` @ 72aa2e9), PLAN agent S. New `ecalc/comm_shmem.c`; additive changes in `comm.h`, `mn.c`
(communicator creation, `MN_TOPO_GROUP`), `mnrun.sh`, `Makefile` (`SHMEM=1`), `tests/t_comm.c`, `tests/t_dist.c`;
`comm_layered.c` got the intra-minor form for the third layer. Size 1 untouched (`shmem_init` is only called under
`COMM_TRANSPORT=shmem`).

**Transport.** `COMM_TRANSPORT=shmem` (default stays TCP), one PE per node-process. OpenSHMEM 1.4 subset only:
`shmem_init_thread`, `shmem_malloc`/`free`, `barrier_all` (the only collective), `ctx_create/destroy`,
`ctx_putmem_nbi` (every data transfer), `ctx_long_p` (signal, count, offset words), `ctx_fence`/`ctx_quiet`,
`long_wait_until`/`long_test`. Not used: `put_signal` (1.5), teams, library reductions (process-level collectives cannot
be driven by four threads).
- *Memory*: one symmetric pool (`COMM_SHMEM_POOL_MB`, default 8192; heap 512 MiB above it): a mailbox table `[1024 ids][PEs]`
  plus a per-PE first-fit allocator. Offsets are always chosen and published by the **receiver**, so allocators run
  locally. On aac6 (OSHMEM host heap) the pool is `hipHostRegister`ed (H2D/D2H DMA 57 GB/s m); `COMM_SHMEM_DEVHEAP=1`
  skips registration (device heaps).
- *Push model*: per peer `roff` (receiver's published offset, `seq<<40|offset`), `sig`, `cnt`, tiny-exchange `val/vseq`
  (two parities), rings (`COMM_SHMEM_RING_KB`, 256) for point-to-point. `alltoall` stages D2H, publishes `roff`, a helper
  thread waits for each peer's `roff`, puts, quiets, signals; returns after the staging copy (slab pipeline needs that);
  `wait()` polls `sig >= seq`, checks `cnt`, H2D. `alltoallv`/`allgather` use the same words; barrier/`allreduce` are a
  tiny all-to-all of 8-byte values (parity double-buffer).
- *Sub-communicators* are strided PE sets `{start + stride*r}` (`comm_shmem_create_at`) with a run-unique id (mailbox row);
  `mn.c` ids: base meshes 0-3, level meshes `NA + NA*slot + d`.
- *Threads*: `SHM_LOCK` no-op with real `THREAD_MULTIPLE` and `COMM_SHMEM_SERIAL=0`; default serial (one mutex, polled
  `long_test`) because OSHMEM 4.1.6 reports MULTIPLE but four threads in `long_wait_until` crash in
  `spml_ucx_ctx_progress`. Lock cost not measurable on aac6.
- *`mnrun.sh`*: `srun --mpi=pmix`, `setarch x86_64 -L`, heap = pool + 512 MiB, `OMPI_MCA_memheap_base_max_segments=64`;
  TCP port base moved to 20000-26000 (ephemeral range 32768+ collided once in ~4 runs at 8 processes).

**Third layer (`MN_TOPO_GROUP=T`).** `comm_layered_create_minor`: layered communicator for any intra size in intra-minor
rank order (node = T*a + b): three-layer all-to-all = APU (xGMI) x node-in-group x group, one aggregated message per peer
group on the global links; `mn.c` builds `tr[d]` this way when T divides the group's transform-node count and `gt > T`.

**OSHMEM traps found (probes, not in tree).** (1) `shmem_init` crashed in `mca_memheap_modex_recv_all` about half the time:
the static memheap registers every `rw-p` mapping below `_end` and counts differ per PE; `setarch -L` gives 4 segments on
every PE (20/20 clean inits m). (2) `ctx_fence` does not order an nbi put before a later put to the same PE (receiver saw
0.5-2.6 MB of a 3 MB slab m); transport orders by `quiet` (`COMM_SHMEM_FENCE=1` for conforming libraries). (3) four threads
in `wait_until` SIGSEGV; mutex plus polled `test` is clean. (4) A context created after another was destroyed lost puts:
default context under the lock. (5) `srun --mpi=pmix` works with OSHMEM, pmi2 does not. Also: `hipcc` link needs
`-lopen-rte -lopen-pal`; TCP port base below 32768; a communicator id may be used once per run.

**Tests (aac6, m).** `t_comm` at 2/4/8 PEs VERIFY OK; `t_dist 24` x4 (54 checks); layered at 2/3/4 and 8 (plain and
three-layer) OK; `ecalc` 1e8 at sizes 2/3/4 identical (15.6/17.5/16.7 s), 1e9 at 2/4 identical (43.4/42.9 s). Regression
`COMM_TRANSPORT=shmem ./mnaccept.sh` at 8441d38: **16 passed, 0 failed** (e9 size 1 15.7/25.7 s; mn 1e8 at 2/3/4 22.4/19.3/18.5 s,
1e9 at 2/4 43.9/43.8 s; ckpt restart identical). An earlier pass at d8949bf had e9 size 1 failing (`mn_init` called
`shmem_init` for a single process); fixed in 8441d38.

**Open at the time.** 2-3 real nodes not exercised (nodes taken); staging copies every slab through the pool (target:
callers' slabs in the pool); `COMM_SHMEM_SERIAL=0` and device heap had no library here to test on; L's `MN_GROUPS`
schedule plugs in at `mn_group_at`. Follow-ups are in §S12.

## §S12 — the SHMEM transport's target forms, tested for real on Sandia OpenSHMEM (2026-09-21)

Branch `s12` (from `main` @ 7aded87). Files: `comm_shmem.c`, `comm.h` (two ops, two inlines, one query), `Makefile`
(`SHMEM_HOME=`), `mnrun.sh` (SOS launch), `ntt_dist.c` (slab allocation), `tests/t_comm.c`; minimal edits to `comm_layered.c`.

**Sandia OpenSHMEM (SOS) 1.5.3 on aac6**, built against libfabric 1.20.1 (tcp, sockets, shm, rxm, udp, rxd providers), simple
PMI (`srun --mpi=pmi2`), `FI_PROVIDER=sockets` (the `tcp` provider is rejected by SOS's fabric query; sockets is
correctness-only). SOS provides real `THREAD_MULTIPLE`, contexts, `putmem_signal_nbi`: the three things OSHMEM 4.1.6 lacked.
A local patch (`SHMEMX_EXTERNAL_HEAP_HOST`, `-DUSE_EXT_HEAP_HOST`, `--enable-ofi-mr=basic`) registers a HIP fine-grained device
buffer (`hipExtMallocWithFlags`; or managed with `COMM_SHMEM_DEVHEAP=2`) as the symmetric heap: the target's form
(`shmem_malloc` returns device memory) exercised end to end.

**Target forms (`comm_shmem.c`), what changed from §S:**

| switch | now |
|---|---|
| `COMM_SHMEM_SERIAL` | 0 by default when the library provides MULTIPLE (OSHMEM stays 1): a context per communicator, no lock |
| `COMM_SHMEM_ORDER` | `putsig` default on 1.5 headers (`ctx_putmem_signal_nbi`); `fence` (1.4); `quiet` (OSHMEM form) |
| puts | inline: caller polls each peer's `roff` for at most `COMM_SHMEM_SPIN_US` (2000), a helper thread only for peers not ready; `COMM_SHMEM_THREAD=1` forces the thread |
| `COMM_SHMEM_DEVHEAP` | 1 = the pool is a HIP device buffer (SOS external heap), 2 = managed, 0 = host `shmem_malloc` |
| `comm_sym_alloc` | callers' buffers in the pool: no staging, no copy (`DIST_SYM_SLABS=0` / `COMM_SHMEM_NOSYM=1` restore staging) |
| staging | per exchange, released at completion (`COMM_SHMEM_KEEP_STAGING=1` restores per-communicator staging) |

The signal word carries the count: `sig = seq<<40 | count` (sequences bounded 2^23 per communicator). One bug the target form
exposed (1e8 at size 4, meshes 1 and 2 wrong): with a pool-resident receive buffer the receiver must not publish offsets
before its **own stream** is done with the buffer (the GPU lags the host by whole passes); fixed by a stream sync before
publication. Portability contract (Cray OpenSHMEMX / rocSHMEM): all 1.4 calls above plus `shmem_ctx_putmem_signal_nbi`
(1.5; rocSHMEM host API unverified, `COMM_SHMEM_ORDER=fence` is the one-switch fallback). `mnrun.sh` picks SOS or OSHMEM from
`ldd` (`COMM_SHMEM_IMPL=` overrides).

**Tests (aac6, m).** `t_comm` SOS at 2/4/8 OK and every other mode at 8 PEs (fence, quiet, thread, serial, spin 0, nosym) 8/8 OK;
OSHMEM `t_comm` OK with the quiet default, **`COMM_SHMEM_ORDER=fence` on OSHMEM: VERIFY FAILED on every PE** (the §S trap
reconfirmed). `t_dist` and layered at 2/3/4/8 OK on SOS (host and device heap) and OSHMEM (T=4 on OSHMEM needs the 8 GiB pool).
`ecalc` 1e8 at 2/3/4 on SOS 10.1/12.9/11.5 s (host heap), 9.0/10.3/11.3 s (device heap, **identical**, VmHWM 17.4 GB against
26 GB with the host pool); 1e9 at 2/4 identical, 39.9/41.3 s (host), 37.8/41.4 s (device). Regression over SOS (job 20883):
**16 passed, 0 failed**, 1607 s; the `mn` step with `COMM_SHMEM_DEVHEAP=1` 5/5 identical.

**Pool memory per PE (m, PE 0, SOS one node).** 1e8 size 4: control 6 MiB, staging 42, symmetric 64, peak 68 MiB; 1e9 size 4:
staging 423, peak 429 MiB; with `NOSYM=1 KEEP_STAGING=1` (Phase 11 form) staging 627, peak 633 MiB. Control blocks are
`8 words*n + n*256 KiB ring` per communicator (`COMM_SHMEM_RING_KB` is the knob). The tree's exchanges in `ecalc` were still
staged: `rns_dist.c` hands the layered communicator pool-1 slabs; the one-line change (`sl = comm_sym_alloc(...)`) was left to
the integrator, and §P214 §3 measured it and kept it off.

**Third layer (m, `t_dist 26` layered at 8 PEs):** SOS 2.20-2.22 s (two layers) vs 2.55-2.58 s (T=4); OSHMEM 2.13-2.18 vs
2.58-2.61 s: on one node the third layer costs 16-20 %. It stays behind its switch (off), in case the target's per-message cost
is 10x the assumption.

**Open then.** Two real nodes over SHMEM not run (never two idle nodes; done in §P214); rocSHMEM host put-with-signal unverified.

## §P214 — the SHMEM pool: measured, modelled, sized (PLAN §33 A2, TASKS 2.4, TARGET_TASKS T0)

Phase 14 agent P2, branch `p14-P2` from `main` @ a839418. SOS build on aac6, two real nodes over 1 GbE (one PE per node,
`COMM_SHMEM_DEVHEAP=1`) and four-process one-node runs; every digit check `cat parts | cmp - ref`. Final commit 7d377c4; regression
ran on 8b6955e. Open at return: the user's decision on `MN_T_CHUNK_MB=1024` for the target; staging in rounds would remove the
second copy (done in §V114).

**Measurements (m).** Pool peak = 4 APU threads x (send + receive) of the single largest staged exchange (8 blocks at every
peak); only staging fills it (control 2 MiB at 2 PEs, 6 at 4; mailbox 0.02 MiB). Every exchange of `ecalc` was staged.

| run | digits | pool peak MiB | largest staged exchange per APU thread (send + recv MiB) | wall s |
|---|---|---|---|---|
| 2 nodes SOS | 1e8 identical | 130 | 10.6 + 10.6 (result exchange) | 25.15 |
| 2 nodes SOS | 1e9 identical | 849 | 106.0 + 106.0 (`mn_core` result exchange) | 171.95 |
| 2 nodes SOS | 1e10 identical | 8479 | 2 x 1059.6 | 2070.7 |
| 4 proc one node | 1e9 identical | 429 | 53.0 + 53.0 | 30.63 |
| 4 proc one node | 1e10 identical | 4244 | 529.8 + 529.8 | 283.26 |
| 2 nodes, `DIST_LOGN_TEST=25` (2 x 2 pieces) | 1e9 identical | PE 0 531, PE 1 637 | PE 1: 53.0 + 106.0 | 197.01 |
| 2 nodes, `COMM_SHMEM_POOL_AUTO=1` | 1e10 identical | 8479 | 1059.6 + 1059.7 (96 exchanges) | 2035.97 |
| 2 nodes, `MN_T_CHUNK_MB=64` | 1e9 identical | PE 1 637 | `mdb_shift` 106.0 + 53.0; result exchange 64 + 64 | 170.77 |

**The law (mod; measured to 0.3 MiB at all eight points).** `pool = 4 x 8 x max over mn_core products and the division shift of
staged send + receive per APU thread + control (4 x (g + lower sizes) x (RING_KB + 64 B)) + mailbox (1024 x g x 8) + 256 MiB
margin` (`mem_model.shmem_pool`; in C `binsplit_shmem_pool_need` + `rns_mul_dist_mn_stage`). Per product: result exchange =
RT(nc_piece) + min(share_c, nc_piece)/4 (each <= m with `MN_T_CHUNK_MB=m`); redistribution = RT(pa) + min(share_a, pa)/4 + 2g
rows; transform = 2q/4; division `mdb_shift` = (2 n_Q/g/4 + n_Q/g/4)/K. Below the cap, `A_h mu` sets it: **pool ~ 32 n_Q/g
bytes ~ 1.78 bytes per digit per node** (x10 per decade; halved at 4 processes). Target 4.25e13 on 576 nodes
(`MN_GROUPS=2,4,8,16,32,64,192,576`, GB per node, mod):

| configuration | pool | node total vs 480 GB |
|---|---|---|
| defaults (TARGET.md §4) | 81.6 (`COMM_SHMEM_POOL_MB=77824`) | 524.7, exceeds |
| + `DM_TIGHT=1` | 81.6 | 501.2, exceeds |
| + `MN_T_CHUNK_MB=1024` | 45.0 | 459.6, fits |
| + `MN_T_CHUNK_MB=1024 DM_TIGHT=1` | 45.0 | 434.3, fits |
| + `DIST_MN_SYM_SLABS=1` | 133.1 | 576.2, exceeds |

`estimate.py` (default `--staging code`): defaults pool 82 / node 525 GB "no"; `--chunk both` pool 45, node 460, wall 244.9 s
(4.1 min) vs 234.0. Largest 576 run at 480 GB: 3.73e13 digits (defaults), 4.59e13 (`MN_T_CHUNK_MB=1024`) (mod).
**Answer to T0:** the staged buffers were counted in the device total already, and the pool holds a *second copy* of the largest
exchange's pair, so both the pool setting and the node total were wrong; 452 GB does not hold with defaults. The README row's
"`MN_T_CHUNK_MB` 1024 since Phase 13c" was wrong: `mn_t_chunk_limbs` defaults to 0 (only `MDB_SHIFT_CHUNK_MB` is 1024). Designs to
remove the second copy: (a) staging in rounds (done in §V114), (b) rbO/sb/rb from the symmetric pool (not done).

**TASKS 2.4 (`DIST_MN_SYM_SLABS=1`, off; m, 1e9, 2 nodes).** Digits identical; pool peak 849 -> **2385 MiB** (staging 847.7 +
symmetric 1536), the transform's 3456 staged exchanges vanish, block pool peak 2.5 -> 2.0 GB, total 172.48 vs 171.23 s. The
transform's staging never sets the peak (32 vs 106 MiB), so the slabs only add 3q*8 B per APU of resident memory (51.5 GB per node
at the 576 target). Kept off; closed as measured.

**Pool rule.** `binsplit_shmem_pool_rule(N, COMM_SIZE)` after `rns_init`: `COMM_SHMEM_POOL_AUTO=1` raises the pool to the need
(256 MiB steps; 256 -> 512 m, identical); without it a too-small pool gets one warning; a pool that runs out stops with an error
naming the use by kind, the size needed and the model's need (`COMM_SHMEM_POOL_MB=96`: rc 3 in 3 s). `MN_PLAN_ONLY` prints
`plan pool`. A host heap needs heap >= pool + 512 MiB.

**Gates (m).** SOS 2 real nodes identical at every size (1e8 x4, 1e9 x7, incl. sym slabs, chunk 64, `DIST_LOGN_TEST=25`); 4
processes 1e9/1e10 identical; TCP 2 nodes identical (1e8 23.02 s, 1e9 170.03 s); **1e10 on 2 real nodes with the final SOS
build and the rule: identical, 2035.97 s (bs 397.2, dm 1620.4, init 14.1)**, pool 8192 -> 8960, peak 8479; regression (8b6955e):
**17 passed, 0 failed**, 1907 s (4e10 identical 61.67 s, RECHECK OK); TCP 1e10 4 processes identical 81.31 s.

**Open then.** Target does not fit 480 GB with defaults (525 GB mod); `MN_T_CHUNK_MB=1024` fits (460) at +0.2 min mod (user's
decision). The law's multi-piece terms are validated at one small shape; the 576 figure rests on them. The Makefile's test rules
lack object dependencies (`make -j16` can link a test before `rns_dist.o` is rebuilt). The device model counts the division's
products with the top tree level's scratch (`A_h mu` rbO at 576 is 16.4 GB per APU, twice the tree top's 8.2).

## §V114 — the SHMEM pool by default, and the exchanges in rounds (Phase 14 agent V1)

Branch `p14-V1` from `p14-defaults` @ 4855842; background §P214. Final code 8f3b136; b6 (2-node re-check on 8f3b136) was never
submitted.

**Task 1: `COMM_SHMEM_POOL_AUTO=1` by default (done).** `mnrun.sh` (when `COMM_SHMEM_POOL_MB` is unset) finds the SHMEM-linked
executable followed by a digit count, runs it with `MN_PLAN_ONLY=<digits>:<procs>` (no device touched, 0.04 s at 4.25e13:576) and
takes the pool from the `plan pool` line; heap = pool + 512 MiB (`MNRUN_PLAN_POOL=0` skips; other commands keep 8192).
`binsplit_shmem_pool_rule` defaults to auto; with the pool in the library heap and a launch heap variable set, a heap below
pool + 512 MiB stops every rank before `shmem_init` with rc 8 (`EC_RC_BUDGET`) naming the variable and sizes (verified: heap 768M
vs need 1280M, rc 8 in 8.8 s). Measured on SOS 2 real nodes: 1e9 identical (host and device heap) and 1e10 identical with the
pool from the plan. 1e10 on 4 processes **failed** on 0b07ebc (pool 4608 enough in bytes but its one first-fit area fragmented
with four APU threads allocating out of step) and passed after the fix (8f3b136, `COMM_SHMEM_STAGE_SLOTS`, default 1): staging in
four regions at the pool's end, one per APU, each the per-APU need + 1 MiB (`COMM_SHMEM_STAGE_SLOT_MB`), fallback to the general
area. Memory placement only; sizes and law unchanged.

**Task 2 (C1): `COMM_SHMEM_ROUND_MB=m` (off): all-to-alls in rounds; bit-identical at every size tested; removes the second copy.**
Chosen over pool-resident caller buffers (b) because it is local to `comm_shmem.c` (TCP and SHMEM keep one layout; arena and
callers unchanged). Design: a pair (s->r) of b bytes goes in K = ceil(b/c) rounds, c = m/(n-1) (256-aligned, >= 4 KiB); both
ends derive K from b; control-word sequences are **per pair** (`oseq[r]`, `iseq[r]`), so rounded and unrounded members
interoperate; rounds run in a helper thread on the communicator's HIP stream (call returns at once, `wait` joins). Staging per
round <= m each way: pool staging <= 4 x 2m. Under the switch every all-to-all takes the rounds' path (0b07ebc), which also
stops staging the self slab (half the staging at 2 PEs). Not covered: `alltoallv_host`, all-gathers (small).

**Measured (m, SOS, no pool/heap set by hand).**

| run | pool MiB plan -> peak | host HWM GB | node MemAvailable min GB | total s |
|---|---|---|---|---|
| 1e9, 2 nodes, default | 1280 -> 849 | 19.8 | 396.3 | 177.35 |
| 1e9, 2 nodes, `ROUND_MB=64` | 768 -> 425 | 19.3 | 396.8 | 177.39 |
| 1e10, 2 nodes, default | 8704 -> 8194 | 27.8 | 351.2 | 2089.83 (bs 403.1, dm 1628.5) |
| 1e10, 2 nodes, `ROUND_MB=256` | 2560 -> **2050** | 21.3 | 357.8 | **2072.91** (396.9 / 1620.3) |
| 1e10, 4 proc, default (8f3b136) | 4608 -> 4244 | 23.3 | 205.1 | 381.16 |
| 1e10, 4 proc, `ROUND_MB=256` | 2560 -> **2054** | 21.2 | 213.5 | 357.90 |

All identical, VERIFY OK; device total unchanged (186.7 GB at 2 nodes). Summed all-to-all time 7990.3 -> 7677.8 s (-4 %) at 2
nodes, 1026.5 -> 936.8 s (-9 %) at 4 processes: rounds are not slower (one run each, within noise). `t_comm` with `ROUND_MB`
1 / 0.001 (4 KiB chunks) / quiet / serial VERIFY OK. Law matches every point to 0.01 %.
Target (mod, `mem_model.py --pool`, GB per node): `COMM_SHMEM_ROUND_MB=1024` alone pool 9.8, node 452.9 (P214's figure); with
`DM_TIGHT` 429.4; with `MN_T_CHUNK_MB=1024` 424.4; with both 399.1 (the Phase 14 defaults plus the switch; defaults without the
switch 434.3). Plan `MN_PLAN_ONLY` 4.25e13:576: 43008 -> 9472 MiB. **Recommended for the target: 1024; adoption is the user's.**

**Open.** (1) 2-node re-check on 8f3b136 (b1-b3 used 6e92c74 / 0b07ebc without regions; b5 checked regions at 4 processes).
(2) `t_bs` segfault (rc 139, "P, Q vs the GMP-free reference recursion") in `mnaccept --only unit,e9,mn` on b4: same on the
integrator's `ntt-def14` and V2's branch, so it predates V1 (Phase 14 defaults). (3) The rounds' law is an upper bound (side =
(g-1)/g of the whole, bounded by m), exact at measured points. (4) The 1e11 reference did not exist; 1e9/1e10 references used.

## §SHM — the SHMEM pool under `COMM_OFI=0` is too small at 6 nodes (bug A of NIGHT)

Question: why does a 6-node 1e10 run with `COMM_OFI=0` stop after init on every rank with rc 3 ("the symmetric pool (2816 MiB) cannot hold 87 MiB more (in use 372 MiB:
control 24, staging 0, symmetric buffers 348)", `archive:results_merged_2026-10-23/NIGHT_RUNS.md` §NIGHT failure A) while 2 nodes pass? Branch `shm` (c5aae92c, 30891e2b), no new switch (model output only);
merged to main 2026-10-13.
**Cause.** `comm_shmem` cuts the pool into four per-APU staging regions at its end (§V114) plus a general area in front; v-slots (`comm_sym_alloc`) come from the general area
only; the pool rule still used M3's `max(4 x staging, 4 x slots)` (results/GRID_GROUPS.md §C06), which no longer holds with separate regions, leaving the general area at control +
mailbox + 256 MiB margin. At 6 nodes, 1e10 (m): pool 2816 = 4 x 591 + 452 MiB general; 8 slots of 87 MiB + 24 MiB control need 720 MiB. It hits every node count with a cut group
(3, 5, 6, ..., 576); at 2 and 4 nodes there are no v-slots. `mnrun.sh` was right (it passes `COMM_OFI=0` to the planner).
**Fix.** `binsplit_shmem_pool_need` adds the v-slots (`sym += 4 x vsp`) under `COMM_OFI=0` also with `COMM_LAYER_VSLOT_RELEASE=1`; `plan pool` names the slots; `mem_model.shmem_pool` the same; README `COMM_LAYER_VSLOT_RELEASE` row: the
"larger, not the sum" rule holds under `COMM_OFI` only.
**Results.** Pool per node-process (mod, MiB, base -> fix): 1e10 2 nodes 4608 -> 4608; 4 nodes 3584 -> 3584; 6 nodes 2816 -> **3328** (v-slots 688 = 4 x 172); 3.82e13 on 576 (target template)
10240 -> **17664** (v-slots 7312 = 4 x 1828; +7.25 GiB per node; plan check OK, 1240 products); `COMM_OFI=1` 512 (2304 at the target). Runs (m, aac7, 1e10, `COMM_OFI=0`): base at 6 nodes rc 3
(reproduces NIGHT); fix at 6 / 4 / 2 nodes rc 0, VERIFY OK, digits identical (40.28 / 39.20 / 47.66 s, single runs, no timing claim); `gate.sh --build --nodes 2` and `--nodes 1` PASS identical.
**Verdict.** Fixed. **Open.** The slots and staging no longer share space (+7.25 GiB host per node at the target, mod); restoring M3's sharing needs a thread's slots to use its own staging region (behaviour
change behind a switch, not done); model control 14 MiB vs measured 24 MiB at 6 nodes (the margin covers it); 3 and 5 nodes (cut groups) not run.
