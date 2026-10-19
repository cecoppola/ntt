# SHMEM_ITEMS — the SHMEM transport and proxy mode

Scope: per-item records behind results/SHMEM.md and SHMEM_STALLS.md (N7 series, pool sizing, strided PE sets, target forms). Condensed from the former per-item files (old IDs resolve through `results/INDEX.md`); original text in `archive:results_merged_2026-10-19/SHMEM_ITEMS.md`, raw data and drivers in `archive:results_raw_2026-10-19/`. Labels: m = measured, mod = modelled, a = assumed.

## Topic summary

- **What this is.** The OpenSHMEM transport of ecalc (`COMM_TRANSPORT=shmem`, `ecalc/comm_shmem.c`): its design and portability contract (§S, §S12), the symmetric pool and its sizing law (§P214, §V114, §SHM), and the long attempt to make the SHMEM data path (`COMM_OFI=0`, and its proxy mode, one helper PE per APU) match libfabric speed at 6 nodes on aac7 (§N7 to §N7G).
- **Current state (checked against `ecalc/README.md`).** `COMM_OFI` unset follows `cxi_present()`: libfabric (`comm_ofi`) is the data path on aac7, so SHMEM is control plane plus the `COMM_OFI=0` fallback. An explicit `COMM_OFI=0` launches the proxy mode (`COMM_SHMEM_PE_PER_APU=1`) with `PX_NUMA` and `PX_KCOPY` on (D31 (b)) and `PX_THP` on (D36 (a)). `COMM_SHMEM_PX_SEND` default 1 (2 and 3 exist), `PX_INIT` 0, `PEER_ORDER` `rank`, `PE_PER_APU=2` refused with `PX_SEND=2`. `COMM_SHMEM_POOL_AUTO` default 1 (§V114); `COMM_SHMEM_ROUND_MB` off in code, 1024 on the target launch line (D2).
- **Live conclusions.** (1) Pool = 4 APU threads x (send + receive) of the largest staged exchange; the law is exact (§P214) and rounds cut the second copy (§V114: 1e10 on 2 nodes pool 8194 -> 2050 MiB, total not slower m). (2) SHMEM-only is 2.1x slower than libfabric at 2 nodes and 3.0x at 6 (§MEAS-N7, §N7), because one PE per node uses one of four NICs; the unequal `layv` exchange is the sensitive part. (3) The proxy mode lifted `t_comm` to 29-30 GB/s per node but not ecalc (§N7B 2.9x slower at 6 nodes); NUMA placement + SDMA-off/copy kernel brought it to 2.1x (§N7C), then 1.43x (§N7D, 3 pairs) and 1.68x over 28 pairs (§N7E: the closure was not reproduced); the cost is exchange wait with a very large run-to-run spread. (4) Huge helper pools were adopted as default (§N7G, D36 (a)) on 14 pairs; the later spread analysis and the libfabric-parity work are in results/SHMEM.md.
- **Superseded or dropped.** `COMM_SHMEM_LAYV_ANY` (§N7G items 49-50, n.s.) was removed from main (b47a2f3). `DIST_MN_SYM_SLABS=1` measured and kept off (§P214). The third layer (`MN_TOPO_GROUP`) costs 16-20 % on one node and stays off (§S12). `COMM_SHMEM_PX_PIN` has no README row on main now (checked by grep); `HEAP_THP` and `OFFS_WITH_DATA` stayed off (§N7G item 40). Three Phase 11 OSHMEM traps (`setarch -L`, quiet not fence, serial calls) remain launcher or runtime switches only.

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
| N7E_V | `COMM_SHMEM_PE_PER_APU=2` (two helper PEs per APU): correctness record |
| N7G | `COMM_SHMEM_PX_THP` and `COMM_SHMEM_PX_PIN` on the proxy mode: pairs and verdict |
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

## §N7 — SHMEM data path (`COMM_OFI=0`) against `comm_ofi`: sweep, diagnosis, peer-order fix, confirmation

aac7, 2026-10-13/14, 1-6 nodes. Question: why is SHMEM-only several times slower than `comm_ofi`, what can settings close, and
does the order of the exchange puts (digits unchanged) close part of the rest? Branch `n7`, commit 6310579 (rebased on main
3a122c9): `comm_shmem.c` only, switch `COMM_SHMEM_PEER_ORDER` = `rank` (default) | `shift` (each member walks peers as a shifted
ring: step k sends to (me+k) mod n, so each step is one-to-one; the push loop is shared with `COMM_OFI=1`, order only).

**Sweep (m, 31 runs).** Part A, 6 nodes, 3.8646e11 digits: `comm_ofi` 245.99 and 215.09 s (mean 230.5, sd 21.8); `COMM_OFI=0`
820.63 s (second run killed at the 1200 s cap, normal progress, recorded as slow); first pair 3.34x. Part B, 2 nodes, 1.2882e11,
mean of 2, `COMM_OFI=0`: `SHMEM_OFI_NUM_NICS` x `COMM_SHMEM_ROUND_MB`: 1x256 234.0, 1x1024 244.1, 1x4096 256.9, 2x256 232.5, 2x1024
269.4 (sd 72.6), 2x4096 251.1, **4x256 210.5 (sd 1.8)**, 4x1024 222.5, 4x4096 254.7 s. One-offs: serial 211.9 (no gain), spin 0 243.1,
spin 20000 258.1, order fence 243.6 s. Best config at 6 nodes (2 runs) 701.0 / 696.0 s, mean 698.5, 15 % below the SHMEM default.
Digit probes at 1e10 identical (`comm_ofi` 6 n 32.8 s, `COMM_OFI=0` 6 n 38.8 s, 4x256 at 2 n 45.4 s). **Gap (m):** 3.0x at
6 nodes (698.5 vs 230.5 s); 1.5x at 2 nodes for the best config (210.5 vs 140.3 s, §MEAS-N7), 2.1x for the default.

**Diagnosis (m, one vendor fact).** At 2 nodes the whole gap is exchange wait: dm +109 s, bs +32 s; init and output unchanged. At 6
nodes the level-2 group of 6 makes the division run an unequal all-to-all over 5 peers (`layv`): SHMEM 1.20-1.39 GB/s per APU
thread (~5.1 GB/s per node) vs `comm_ofi` 7.2-9.2 per APU (~32 per node), 6.3x slower; `layv` carries 3.4 of the 4.6 TB a node
receives and ~82 % of the gap; the equal-slab form at 2 nodes reaches 16.2 GB/s per node. Settings cannot close it: Cray
OpenSHMEMX assigns each PE to one NIC, ecalc runs one PE per node (a 4-PE-per-node probe reaches 31-32 GB/s per node, the rate of
`comm_ofi`); `NUM_NICS=4` only spreads the PE's resources. That structural limit is the subject of §N7B (D29 (a)).

**Confirmation of `shift` (P5, m).** `t_comm --bw` at 6 nodes (256 MiB slabs, 4 NICs): rank 10.24 / 9.92, shift 12.48 / 11.95 GB/s per
node (+21 %, n = 2, ranges disjoint). Gates 1 and 2 nodes PASS identical; 6-node 1e10 identical (58.6 s). Full-run ABBA at 6 nodes,
3.8646e11, 4 NICs, 256 MiB rounds: pairs B - A +644.5 (outlier, shift 1373 s), -115.3, -138.3 s; round 4 killed at 1500 s in both arms (rc
124). Paired mean +130.3 s, CI [-976, +1237] (n = 3, includes 0); without the outlier shift -15 % and -18 % (2 pairs). All VERIFY OK;
digit identity at 6 nodes shown only by the 1e10 probe.

**Verdict.** Keep `COMM_SHMEM_PEER_ORDER=rank`; do not adopt `shift` yet (transport +21 %, full run inconclusive; README review date
2026-11-12). SHMEM stays far behind `comm_ofi` (3.0x at 6 nodes): the remainder is the one-NIC-per-PE limit.
**Open.** Twice a killed SHMEM run was followed by all six nodes going to `needs_reboot` (2026-10-13 21:07, 2026-10-14 02:05 ET;
cause not established); the 1500 s cap was hit in both arms of round 4 though the same arms took 640-780 s earlier; the `comm_ofi`
anchor and spin-0 check were not run; runs above 1e10 have no reference compare; OpenSHMEMX 12.1.0 (target) and two NICs per PE
untested (aac7 has 11.8.0, one NIC per APU).

## §MEAS-N7 — SHMEM data path (`COMM_OFI=0`) against libfabric (`COMM_OFI=1`) at 2 nodes

Measurement only (main a6ba51d, `make -B SHMEM_CRAY=1`), queue item `82_meas_n7`, 2 x 6.441e10 = 1.2882e11 digits, two ABBA rounds
(time budget), `COMM_XSTATS=1 MN_WAIT_STATS=1`; VERIFY OK, rc 0 on all 8 runs, digits not compared.

| (m, total s) | A `COMM_OFI=1` | B `COMM_OFI=0` |
|---|---|---|
| round 1 | 157.39, 133.32 | 285.22, 280.26 |
| round 2 | 136.25, 134.11 | 406.20, 278.16 |
| mean of 4 | 140.3 | 312.5 (3 of 4 within 278-285; the 406 s run is slow mode, init 64.7 s) |

Paired B - A +172.2 s, 95 % CI [-270, +614] (n = 2 pairs, p 0.12); every B run is >= +122 s above every A run, so the sign is not in
doubt; typical gap +145 s (2.1x). Stage split (A mean of 4, B of the 3 ordinary runs): init about equal (34-44 / 34-40 s); bs 45.5 vs
77.1 (+31.6); dm 54.4 vs 163.0 (+108.6); dc 3.1 vs 3.0; thread-summed waits bs/dm/recip 52/105/34 vs 188/435/148 s (x3.6/4.1/4.4):
the whole gap is exchange wait, dm about 77 %, bs about 22 %. Plan (mod): `COMM_OFI=1` pool 512 MiB plus 4 comm_ofi pools of 2304 MiB device,
heap 1024M; `COMM_OFI=0` pool 8704 MiB (4 x 2048 staging + control), heap 9216M; plan check OK. At the target (mod) pool 2304-10240 MiB,
heap 10752 MiB.
**Verdict.** SHMEM-only 2.1x slower at 2 nodes; keep `COMM_OFI=1` default; a SHMEM-primary target needs the data path fixed first.
**Open.** Only 2 pairs; 2 nodes only (the 10-node ratio was 2.1x, OFI17); OpenSHMEMX 12.1.0 and both NICs per PE untested.

## §N7B — one SHMEM proxy PE per APU (`COMM_SHMEM_PE_PER_APU=1`): bandwidth, gates, 6-node ABBA

aac7, 2026-10-14. Question: does one proxy PE per APU (each on its own NIC; the node-process stays the compute rank) lift SHMEM-only
toward `comm_ofi`? Branch `n7b`, merged to main 8acda73 (default 0; needs `COMM_OFI=0` and >1 node). Design: logical PE = node (meshes and
callers unchanged); proxy d on NIC d posts the puts from its own symmetric heap; the node-process maps those heaps through named
`/dev/shm` files (an `mmap` wrapper armed only inside a proxy's `shmem_init`, since the on-node path of the aac7 library is absent);
control words stay on the node-process; `mnrun.sh` launches 1 + 4 PEs per node, 4 NICs round robin; value 2 designed, refused at init.

**`t_comm --bw 4 5 256`, GB/s per node, 256 MiB slabs (m, 2 runs each):** 2 nodes comm_ofi 21.9 / 21.6, SHMEM today 13.4 / 15.1, PE per
APU 15.7 / 15.6; 6 nodes 30.8 / 39.7, 8.4 / 8.5, **28.8 / 29.8**. At 4-64 MiB, 6 nodes: PE per APU 30.0-32.5, comm_ofi 36.8-41.2, SHMEM
today 10.2-12.4; proxies put at 20.5-22.6 GB/s per NIC while busy. The B2 gate (>= 25 GB/s per node at 6 nodes) passes.
**Correctness (m):** gates 1 and 2 nodes at defaults and in the new mode 4/4 PASS identical; 2-node 1e10 identical; 6-node 1e10 identical
(54.8 s); `gate.sh --nodes 6` PASS (ecalc 21.2 s); `/dev/shm` proxy files left 0.

**6-node ABBA, 3.8646e11 (m, total s):** comm_ofi 272.19 / 206.51 / 213.99 / 203.28 (mean 224.0, sd 32.4) vs proxy 642.24 / 544.10 / 561.72
/ 878.75 (mean 656.7, sd 154.1); **B - A +432.7 s, 95 % CI [+174.3, +691.1], n = 4**; bs +146.7 [+72.5, +220.9], dm +251.7 [+66.7,
+436.6]; per-APU-thread all-to-all rate A 6.5-8.4 vs B 1.07-2.46 GB/s. Anchor (SHMEM today, 1 run) 719.5 s, 1.34 GB/s.
**Verdict.** Correct and the bandwidth gate passes, but **not adopted**: 2.9x slower than `comm_ofi` at 6 nodes, about equal to SHMEM today;
the `t_comm` gain does not carry into ecalc (1.1-2.5 vs ~7.2 GB/s per APU thread). Parameter stays 0.
**Open then.** Why ecalc exchanges run below the `t_comm` rate was undiagnosed (answered in §N7C); spread large (sd 154 s); value 2 not
built (aac7 has one NIC per APU); `mem_model.py` not extended for per-PE pools.

## §N7C — SHMEM proxy mode: why it is slow, and `COMM_SHMEM_PX_NUMA=1` + `HSA_ENABLE_SDMA=0` (config C)

aac7, 2026-10-14, 2 and 6 nodes. **Diagnosis (m).** `t_comm` is host-only and sees only the NICs; ecalc's exchanges are mostly GPU work on the
pool. Under `comm_ofi` the pool is fine-grained device memory on the APU's HBM; in the proxy mode it is the proxy's `/dev/shm` heap: 4 KiB
pages (no huge-page route on aac7), placed by first touch (local to its APU in 6 of 24 pool-runs), and copies touching it go to the SDMA
engine (21 GB/s, results/MEMORY.md §I). Fixes: F1 `COMM_SHMEM_PX_NUMA=1` (mbind of the proxy heap to the NIC's NUMA node: 32 of 32 pools
local m) and F2 `HSA_ENABLE_SDMA=0` (environment only). 2 nodes, 5e10, one run each: comm_ofi fabric stage 8.8 s, total 55.1-59.3; proxy
14.7-21.8 / 80.1-99.3; with both fixes 9.7 / 57.1 s. Either fix alone was not enough (NUMA only fabric 13.5-17.2 s; SDMA off only worse,
19.7-26.0 s).
**Change.** `COMM_SHMEM_PX_NUMA` (default 0 then; now 1 inside the proxy mode, §N7D), `ecalc/comm_shmem.c`; `HSA_ENABLE_SDMA=0` added to no launch line.
**Test (m, 6 nodes, config C = `COMM_OFI=0 PE_PER_APU=1 PX_NUMA=1 HSA_ENABLE_SDMA=0`).** Gates 1 and 2 nodes PASS identical; 1e10 identical at 2
(44.2 s) and 6 nodes (39.4 s). ABBA 3.8646e11, 4 rounds: comm_ofi 235.05 / 210.39 / 219.02 / 200.91 (mean 216.3, sd 14.5) vs config C 385.08 /
460.17 / 461.66 / 527.96 (mean 458.7, sd 58.4); **B - A +242.4 s, CI [+127.1, +357.7], n = 4**; bs +90.5, dm +129.3, exchange wait +425.2 s;
rate per APU thread A 6.8-8.3 vs B 2.0-4.0 GB/s. One run with SDMA at default (`PX_NUMA=1` only) 417.7 s (no claim). Before (§N7B): 656.7 vs 224.0 s.
**Verdict.** Correct, merged off. Not adopted: 2.1x slower at 6 nodes (2.9x before); 2-node parity (57.1 vs 55.1 s) does not carry to 6 nodes with
5 peers and `layv`. Placement and SDMA explain ~45 % of the gap (mod, 432.7 -> 242.4 s; builds and days differ).
**Open then.** 5-peer lock-step with proxy batch-then-quiet, init and skew (B init 35.8-74.5 s vs A 31.8-35.4), 4 KiB pages (admin change).

## §N7D — SHMEM proxy mode toward comm_ofi speed at 6 nodes (copy kernel, parallel init, put-with-signal sends)

aac7, 2026-10-14, 1, 2 and 6 nodes. Baseline: config C 458.7 vs comm_ofi 216.3 s (§N7C). **Diagnosis (m).** The gap was not on the wire: tree level 1 at
parity, proxies busy 17 % of the run, `t_comm` 5-peer exchange at 0.75 of comm_ofi; inside ecalc the GPU work on the proxy pool was slow and
uneven (block transposes 13-138 s per APU vs 2-4 s under comm_ofi) and lock-step spread one slow node to all; init +17 s per pair; config C spread
385-1588 s.
**Change (branch `n7d`, all off by default).** `COMM_SHMEM_PX_KCOPY=1` (`comm_copy.c`: GPU copy kernel for copies touching a proxy pool, replaces
`HSA_ENABLE_SDMA=0`); `COMM_SHMEM_PX_INIT=1` (+ `_THREADS`: pre-fault pools with `MADV_POPULATE_WRITE`, parallel registration; pre-fault of 2.5 GiB took
0.08-0.17 s m); `COMM_SHMEM_PX_SEND=2` (4 MiB `putmem_signal_nbi` chunks straight into the receiving proxy's pool, one context per peer node, no batch
quiet).
**Results (m).** Gates 1/2/6 nodes PASS identical; 1e10 identical at 2 n (53.7 s) and 6 n (41.9 s). 2-node KCOPY pair (2 pairs): total C 28.46 / 28.63
vs K 29.05 / 26.71 s, K - C -0.66 s [-16.6, +15.3], fabric stage -0.36 [-0.89, +0.18]: the kernel matches `HSA_ENABLE_SDMA=0`. `t_comm --bw` at 6 n:
PX_SEND=2 31.07-32.05 vs 26.3-28.3 GB/s per node at 256 MiB, equal at 4-64 MiB. **6-node ABBA (3 complete pairs, 3.8646e11):** comm_ofi 192.91 /
184.49 / 197.37 (mean 191.6) vs B 292.20 / 247.78 / 284.38 (mean 274.8, sd 23.7); **B - A +83.2 s, CI [+37.7, +128.7]**; bs +37.7, dm +39.6, init
+2.1 [-2.0, +6.1], block transposes +0.13 s per APU (A 2.4, B 2.6; was 13-138 s), fabric stage +19.1 [-0.8, +38.9]. Gap to comm_ofi +242.4 -> +83.2 s
(1.43x, was 2.1x); B spread 385-1588 -> 248-292 s; init at parity. One-factor runs (1 each, no claim): without `VSLOT_POOL=0 INTER2=0` 244.4 s; with
`PX_SEND=1` 318.1 s. Round 4 did not complete (a node failed, site-side, cause unknown).
**D31 (b) applied (branch `n7d-def`).** Under an explicit `COMM_OFI=0`, `mnrun.sh` launches the proxy mode (`PE_PER_APU=1`) for multi-node runs of one
node-process per node, and `PX_NUMA` and `PX_KCOPY` default to 1 in it (explicit settings win; `PE_PER_APU=0` = plain SHMEM; `mem_model.py` mirrors it).
Check (item `23_n7d_d31`, 2 nodes, f50d57f, m): gates 1 and 2 nodes with plain `COMM_OFI=0` PASS; 1e10 identical (44.2 s); 8 of 8 proxies NUMA-placed.
**Verdict.** Merge the switches, off; proxy mode still slower than comm_ofi at 6 n (+83 s, CI excludes 0), so comm_ofi stays default; LAYER switches not
needed. **Open then.** Remaining +83 s is exchange wait (xw0 +260 s), not transposes or init; node failure unexplained; 4 KiB pages.

## §N7E — the N7D proxy-mode switches at 6 nodes: paired repeat (PX_SEND=2, PX_INIT=1) against comm_ofi

aac7 item n7e, 2026-10-14 16:39-22:55 ET, commit 2f34f01, 6 nodes, 3.8646e11; records only, no code change. Question: do `PX_SEND=2` and `PX_INIT=1` bring
the proxy mode to comm_ofi speed, and is §N7D's 1.43x reproduced? Arms: A comm_ofi; B `COMM_OFI=0 PX_SEND=2 PX_INIT=1`; I1S1 `PX_SEND=1 PX_INIT=1`;
I0S2 `PX_SEND=2 PX_INIT=0` (D31 defaults PX_NUMA and PX_KCOPY assumed on). Gates 1/2/4/6 nodes PASS identical; 83 of 83 runs ok.

| arm (m, mean s) | n | total (sd) | bs | dm | init |
|---|---|---|---|---|---|
| A comm_ofi | 29 | 191.0 (15.1) | 76.6 | 78.0 | 32.3 |
| B PX_SEND=2, PX_INIT=1 | 35 | 348.3 (111.1) | 130.3 | 175.3 | 35.6 |
| I1S1 | 4 | 421.7 (58.5) | 161.2 | 212.1 | 39.4 |
| I0S2 | 3 | 313.3 (43.5) | 115.3 | 158.5 | 35.2 |

Paired: **B - A +129.3 s, CI [+92.9, +165.7] (28 pairs)**, bs +43.6, dm +79.3, exchange wait +338.8 [+251.2, +426.4]; B - I1S1 (SEND 2 vs 1) +69.9 [-217.8, +357.6]
(n = 4, n.s.); B - I0S2 (INIT 1 vs 0) +107.2 [-50.5, +264.9] (n = 3, n.s.), init -1.3 [-2.7, +0.1]. B / A = 1.68x (vs 1.43x, +83.2 s in §N7D: **not reproduced**).
**Verdict.** Not adopted: defaults stay `PX_SEND=1`, `PX_INIT=0`; neither switch shows a gain; exchange wait is the cost. **Open.** Source of B's 111 s sd
unlocated; more pairs needed (CI half-widths 288 s at n = 4 for SEND, 157 s at n = 3 for INIT); D31 defaults assumed in B; one planned run skipped.

## §N7E_V — `COMM_SHMEM_PE_PER_APU=2` (two helper PEs per APU): correctness record

aac7 item n7v, 2026-10-14, commit 97275a2, 1 and 2 nodes. Value 2 = 1 + 8 PEs per node; an APU's staging and slabs interleave in 4 MiB blocks over its
two helper heaps (`px_map2`); aac7 has one NIC per APU, so no speed claim. **Failure found and fixed (m):** the first 2-node gate (b989a6b) stopped
with "heap of helper PE N is not reachable"; `shmem_malloc` put the pool at offset 1920 mod 4096 and `px_map2` maps heap files page by page. Fix: with
value 2 the pool is `shmem_align`ed to 2 MiB. **Results (m):** gate 1 node and 2 nodes (9 PEs per node) PASS identical (1e9); 1e10 at 2 nodes
identical, 47.8 s; `/dev/shm/ecalc_px_*` left 0. Default stays value 1; `COMM_SHMEM_PX_SEND=2` is refused with value 2.

## §N7G — COMM_SHMEM_PX_THP and COMM_SHMEM_PX_PIN on the proxy mode: pairs and verdict

Question: do huge pages (`PX_THP=1`) and pinned proxy pools (`PX_PIN=1`) shorten the 6-node proxy run (`COMM_OFI=0`)? Branch `n7g`, commit 006a80f, defaults 0.
Page size (m): helper pools are 4 KB pages, shared-memory huge pages unavailable; `MADV_COLLAPSE` works and costs 0.29 s per GiB.
- **First pairs (3 pairs, 3.8646e11, m).** A defaults mean 432.1 s (sd 83.6), B THP+PIN 307.2 s (sd 93.7): B - A -124.9 s, CI [-460.6, +210.8], n.s. Re-gate on
  `n7f` (76c8742): 11 of 11 gates pass at 1 and 2 nodes, 2-node 1e10 identical. Default stays 0 (D35).
- **Item 37 (THP only, 4 pairs, m).** A 576.5 s (sd 155.5), B 288.8 s (sd 63.0): **-287.8 s, CI [-507, -68]**, B faster in 4 of 4 pairs; gates PASS (1 and 2 nodes,
  comm_ofi 2-node, 6-node 1e10 identical).
- **Item 38 (default flip, D36 (a), build aedc13c, m).** Gates 1 and 2 nodes PASS identical, 6-node 1e10 identical; 2 pairs THP default vs `PX_THP=0`: 237.4 vs 337.3 and
  228.8 vs 544.3 s (mean +207.7, n = 2, n.s. alone); THP active in A (huge helper pools 2562 MiB per node). **D36 (a) applied: `COMM_SHMEM_PX_THP` defaults to 1 in the proxy mode.**
- **Items 39 and 41 (4 pairs each, m).** Default on vs off: +28.4 s [-27, +84] and +163.5 s [-45, +372]; default 290 s mean, off 475 s. Pooled over the 14 pairs of items
  37-41: about 167 s saved by the default (mod; conditions mix). Item 40: `COMM_SHMEM_HEAP_THP=1` no gain (2 pairs +24.8 s [-179, +229]), `OFFS_WITH_DATA=64 MESH_PAR=1`
  no gain (2 pairs, +49 s); both stay 0.
- **Items 45-48 (m).** Libfabric 6 nodes 3.8646e11, 4 runs 178-193 s (mean ~184). Default SHMEM (THP on) 254 / 256 / 289 s (3 runs); THP off 446 s (1 run); default
  items 47-48, 8 runs 221-397 s (sd ~60): the spread exceeds the gap between the default and libfabric means. Compaction-stall increases per node (4 runs) show no link
  to run time (n = 4). One node finishes late and five wait in `layv` (n = 8, cause unidentified). Drivers 45-48 hung at exit (no done-marker; fixed for 48).
- **Items 49-51 (m).** `COMM_SHMEM_LAYV_ANY`, 4 ABBA pairs: B - A -19.6, -1.3, -101.6, -57.5 s, mean -45.0 s, CI about [-116, +26], faster in 4 of 4, n.s.; gates PASS. Timestamp
  runs (item 51) wall 223 / 264 / 186 / 235 s. (The switch was later removed, see the topic summary.)
**Verdict.** Huge-page default stays on; heap and task-3 switches stay off; PIN not adopted (never separated from THP). Consolidated story, phase breakdown and variance:
results/SHMEM.md.

## §SHM — the SHMEM pool under `COMM_OFI=0` is too small at 6 nodes (bug A of NIGHT)

Question: why does a 6-node 1e10 run with `COMM_OFI=0` stop after init on every rank with rc 3 ("the symmetric pool (2816 MiB) cannot hold 87 MiB more (in use 372 MiB:
control 24, staging 0, symmetric buffers 348)", results/NIGHT_RUNS.md §NIGHT failure A) while 2 nodes pass? Branch `shm` (c5aae92c, 30891e2b), no new switch (model output only);
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
