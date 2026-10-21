# XSHMEM: target-shaped communication (Cray SHMEM 12.1 style) over libfabric on aac7, plan TARGET1, 2026-10-21

Scope: the xshmem layer (API, backends, boot and topology, tests), what Cray SHMEM 11.8 and the cxi provider were measured to do, the engine
`comm_xshmem.c` behind `COMM_TRANSPORT=xshmem`, its gates and first timing, and the open items for the target port. Merged from the former reports
B1-B4 and S3A_A1/A2 (the six `XSHMEM_*.md` files). Labels: m measured, mod modelled, I inferred, a assumed. All numbers are single aac7 runs unless a count is given.
Settings: `ecalc/README.md` (rows `XSHMEM*`, `ECALC_XEX_THREAD`, `COMM_TRANSPORT`). Design source and trace data are kept outside the repository.

## 1. Purpose and shape

After the aac7 Cray SHMEM tuning phase (closing record in `results/SHMEM.md`) the project builds the communication the target will have:
cray-shmem 12.1 calls, GPU-memory buffers, several NICs per node. 12.1 cannot be run on aac7 (11.8 only), so the same calls are emulated with libfabric
(cxi) behind one header, `ecalc/xshmem.h`, a 1:1 mapping of the 12.1 calls the engine needs (23 calls: init, contexts, heap alloc, put_nbi, quiet,
signal_set / put_signal / signal wait and test, barrier, topology and capability queries).
- `XSHMEM=ofi` (aac7): `xshmem_ofi.c` over libfabric, `xshmem_boot.c` (PMI2 address and blob exchange), `pe_geo.c` (GPU, NUMA and NIC map).
- `XSHMEM=cray` (target): `xshmem_cray.h`, static inline forwarding to cray-shmem; no libfabric. The four places that differ in 12.1 (device-heap allocator,
  signal set, signal wait, context creation) are selector blocks (`XSHMEM_CRAY_DEFS=-DXSHMEM_CRAY_HEAP|_SIGSET|_WAIT|_TEST|_PUTSIG_SPLIT`); with the 11.8 headers it compiles and
  links with a host-heap fallback (m, aac7 login node). The engine and the tests include only `xshmem.h`, so the port changes no engine file.
- Default build (`XSHMEM` unset or 0): nothing is built or linked; `make -n all|ecalc|tests/t_comm` are byte-identical to before (m).

## 2. API contract and switches (frozen in B1)

- `xshmem_heap_alloc(bytes, space)`: at most once per space (`XSHMEM_SPACE_DATA`, `_CTRL`), collective, zeroed, 2 MiB aligned. `xshmem_off(p)` is relative to the space;
  CTRL offsets carry bit 62; only offsets cross PEs. `xshmem_ctx_t` is `void *`; compare `GE` and signal `SET` only, signals are plain 8 B writes.
- Lane contract: N contexts are created right after `xshmem_init_thread`, in order, before `xshmem_heap_alloc`. `xshmem_topo_t` mirrors `pe_topo` (`pe_geo.h`).
  `xshmem_cfg_load()` is the only reader of the `XSHMEM_*` switches. `xshmem_ofi_counters` (writes, flush chains, quiets, wait spins) is ofi-only.
- Switches (all off or default-valued; rows in `ecalc/README.md`): `XSHMEM_OFI_SIG` dc (default) | flush | waw, `XSHMEM_LANES` 1, `XSHMEM_HEAP_MB` 0, `XSHMEM_MRMODE` libsma | ofi,
  `XSHMEM_CTRL` heap | host, `XSHMEM_LOCAL`, `XSHMEM_IPC` (0), `XSHMEM_PES_PER_GPU` 1, `PE_GEO_SYSFS_ROOT`, `XSHMEM_LOOPBACK_PES` 1, `XSHMEM_ROUND_MB` 0, `XSHMEM_ORDER` rot | seq,
  `ECALC_XEX_THREAD` 1 | 0, and `COMM_TRANSPORT=xshmem` (default transport stays `shmem`). `tools/xshmem_api_check.py` compares the two backends' API tables (23 calls, 0 problems, m).
- Backend facts (m, aac7): dc mode, putmem_nbi, ctx_quiet (counter poll, 130 s deadline), signal_set, putmem_signal_nbi (blocks inside), virtual lanes on one NIC, two heaps. flush and waw modes
  are unproven arms; `xshmem_ptr` returns NULL (on-node peers go over the NIC).
- Boot (`xshmem_boot`): `boot_init/allgather/fence/finalize` over PMI2 (plain `srun`, no `--mpi`); cross-checks the Slurm rank and size; values chunked at 900 chars. `pe_geo_init` gives
  gpu_node = local / pes_per_gpu, device, NUMA of the GPU's PCI function, the cxi NICs on that NUMA node (`COMM_OFI_NICS` overrides), `nlanes`. `t_pe_geo` (mock sysfs trees: aac7 shape 4 NUMA x 1 cxi,
  target shape 4 NUMA x 8 cxi, 1 and 4 PEs per node, 2 PEs per GPU, overrides, bad values): 79 checks, 0 failed (m).
- Live check (`tests/t_pe_geo_live`, one hold): `srun -N1 --ntasks-per-node=4 --cpus-per-task=48 --cpu-bind=map_ldom:0,1,2,3 --gpus-per-node=4 --gpu-bind=none ./tests/t_pe_geo_live`; expected on aac7
  per PE i: `gpu i numa i nics 1 lanes 1 cpus <48i>-<48i+47>`, the NUMA of GPU i equal to that of its NIC, no BAD marker, last line `t_pe_geo_live: 4 PEs, rank 0 ok`; at -N2 the same with node 0/2 and 1/2.
  With `--gpus-per-task=1` each PE sees one device (`gpu 0`), same NUMA and NIC. Ran ok at 1 and 2 n (m).
- Loopback PEs (S3a): the backend state is `pe_t G[K]`, K = `XSHMEM_LOOPBACK_PES`; each PE has its own NIC domains, AV, endpoints, heaps, peer table and mutex; calls with a context take the PE from it,
  the others use a thread-local current PE (`XSHMEM_SELECT(l)`). PE = K*rank + l; one boot allgather of K address blobs and one of K heap blobs; the K local PEs call `heap_alloc` in order.
  K = 1 keeps the earlier call order.

## 3. What Cray SHMEM 11.8 does on cxi (m, trace of one aac7 hold; defaults, 1 NIC per PE; single samples)

- Init: 129 RDM endpoints per PE (one `fi_getinfo` each, own cxi PID), one domain, one shared AV (table), addresses by `fi_getname` and a PMI allgather; scalable startup fails here. `ctx_create` is a pool pop (no
  libfabric call, 2 creates 57 us); `shmem_malloc` registers nothing (barrier only). Two MRs (data segment, heap), client keys 1 and 2, bound to endpoint 0; mode ALLOCATED|ENDPOINT, offset addressing.
- Build "SMP(OFI+XPMEM)" but no `/dev/xpmem`: on-node traffic also goes through the NIC with the same flags and flush.
- `putmem_nbi`: one `fi_writemsg`, FI_TRANSMIT_COMPLETE, no FI_COMPLETION, +FI_INJECT up to 192 B; one writemsg even at 64 MiB (no pipeline split). Blocking `putmem` above 192 B adds FI_COMPLETION and a local wait.
- `putmem_signal(_nbi)` (default, `_ENABLE_PUT_SIGNAL=0`): payload with FI_DELIVERY_COMPLETE, counter wait, then the 8 B signal write: not hardware-ordered; two round trips.
- Quiet: `fi_cntr_wait` (130 s), then per touched target NIC a chained flush (8 B writemsg FI_MORE|DELIVERY_COMPLETE + 0 B inject write) and a second counter wait. `shmem_fence` = quiet.
- NIC counters per 16 MiB (node sum): transmit-complete puts are restricted (unordered) packets with ~26 flush requests per node; delivery-complete puts make every 2 KiB packet an ordered request with its own flush request (16418 ordered, 16410 flush requests).
- Times (single samples, indicative): 16 MiB put + quiet 0.82 ms (about 20 GB/s over one NIC); 8 B put + quiet 0.20 ms.

## 4. Probe facts, cxi through libfabric (m, aac7)

- cxi grants no FI_MR_VIRT_ADDR: offset-only addressing with client keys (key < 100 is the optimized-MR range; provider keys are slower: 8 B put + wait 6.1 against 3.15 us).
- One MR cannot be bound to a second endpoint (`fi_mr_bind` -22): one MR per endpoint and heap. The same client key on two endpoints of one domain is refused: the backend uses distinct keys per endpoint
  (2 + space + 4*e) and the heap blob carries one key per endpoint. Mode in force 0x220.
- Explicit dmabuf registration of a 1 GiB device heap succeeds, but the first RMA op fails (CQ error 5, NO_SPACE). Plain FI_HMEM (ROCR iov) registration works at 1 GiB on coarse, fine-grained and host heaps; the backend uses it.
- Host polling of a 64-bit signal word in the GPU heap with a plain load works (coarse and fine-grained, 0 stale reads in 2000): 2.1-3.0 us one way, 0.2 us above a host-region word; hipMemcpy polling costs 11-14 us.
  Staleness after long idle or under heavy GPU traffic is not tested.
- Cray PMI2 KVS default is 30 entries: 8 PEs abort. `xshmem_boot.c` sets `PMI_MAX_KVS_ENTRIES` = 16 * tasks + 256 before `PMI2_Init` when unset; the address blob is 328 B.
- Latency floors: inject 3 us, DMA put 5-8 us, signalled put 7-10 us; quiet 4.5-11 us (about +0.8 us per further target). Translation-miss counters on device heaps: 2-2400 per group (host heap: 10^5-10^7).
- Off-node all-to-all, probe: 21-23 GB/s per PE (87-91 GB/s per node, 2 and 3 n). With on-node peers in the mix 14.3 GB/s per PE (device heap through the NIC); a host heap does not show this (21.9). Cause not established.

## 5. Unit tests (t_xshmem, m, single runs, dc + SPLIT, N = 1)

`ecalc/tests/t_xshmem.c`, one source for both backends. Sections: `basic` (ring put 8 B..64 MiB in three arms), `exch` (the `xex_*` exchange: random alltoallv, equal alltoall, small rounds, pairs, node-local and strided
communicators), `deadlock` (poll-only regression, a hang is a FAIL), `layer` (layered exchange, one in flight), `limit` (2^22 exchange limit, negative tests), tables `lat`, `stream`, `a2a`, each bracketed by cxi telemetry.
- Correctness `basic`, `exch`, `deadlock`: PASS at 1, 2 and 3 n x 4 PEs; `layer` failed first from a test bug (counts array kept by pointer), fixed by copying the counts. Tables ran with 0 failures.
- Tables (8 B hop latency, 64 MiB stream, 64 MiB all-to-all, per PE): 1 n x 4: 10.6-10.9 us, 23.3, 21.3 GB/s; 2 n x 4: 15.0-15.5 us, 23.3, 13.75; 3 n x 4: 13.5-14.4 us, 23.3, 9.59; 2 n x 1: 9.9-10.3 us, 23.3, 22.97.
  The stream is at one NIC's rate in all shapes; the all-to-all rate falls with the PE count (concurrent senders, ordered packets: `restricted_pkts` = `flush_reqs`).
- All-to-all 64 MiB per PE at 3 n x 4: sequential peer order about 9.3 GB/s, rotated order about 12.2 GB/s (+30 %, hence `XSHMEM_ORDER=rot` default).
- The `putmem_signal_nbi` arm and 2 virtual lanes pass the correctness sections (3 n x 4 and 2 n x 1; m). The nbi latency arm: 2 n x 4 14.5-14.9 us (m).
- `comm_ofi` in the same one-PE-per-GPU shape (`t_comm --bw 1 5 64`, host-pool buffers, built `make SHMEM_CRAY=1 OFI=1 tests/t_comm`, `COMM_OFI_POOL_MB=2048`): 3.2 GB/s per thread at 64 MiB slabs, 3 n x 4 (229 ms per exchange vs 58-75 ms on xshmem); 2 n x 1 PE 9.6 vs 23.0 GB/s (m, single runs). `comm_ofi` is built for one process per node, so this is not the adoption comparison (that is §7, ecalc timing).
- Build note: the login node is Zen3 while the compiler wrapper targets the compute CPU; run login-node binaries built with `-march=native` or on compute nodes.

## 6. The engine and its gates (S3a)

`ecalc/comm_xshmem.c` (about 400 lines): the t_xshmem exchange behind `struct comm_ops`, written only against `xshmem.h`; inter-node meshes only (intra stays `comm_xgmi`, `comm_layered.c` unchanged).
- Model: one node-process hosts K PEs (K = `XSHMEM_LOOPBACK_PES`, 4), PE = K*node + d, a lane context per PE, then the CTRL heap, then the DATA heap. DATA heap = `XSHMEM_HEAP_MB` (the planner sets it from the comm_ofi pool rule).
  CTRL words per communicator id: ROFF SIG VAL0 VAL1 VSEQ0 VSEQ1 SOFF.
- Exchange: post stages non-heap buffers into the heap, publishes ROFF per peer (order `rot` or `seq`), hands over to the worker; the worker (`ECALC_XEX_THREAD=1`, one thread per PE) does the puts, one quiet, the signals;
  wait tests signals (same fatal texts as `comm_shmem`), copies staged slabs out. A second post while pending is fatal; 900 s hang watchdog. Barrier and allreduce_max by parity words; allgather_host blocks up to 4 KiB.
- Rounds: `XSHMEM_ROUND_MB` > 0 carries a pair larger than the bound in rounds (one signal per round and pair), run in the worker; 0 = whole-exchange staging (needs the whole exchange to fit in the heap).
- Gates (items 9p1, 9p4, m): `tools/gate.sh` at 1 n and 3 n with the default transport PASS (the XSHMEM=ofi binary); 2 n and 3 n with `COMM_TRANSPORT=xshmem` at 1e9 digits: VERIFY OK, digits identical, with and without `XSHMEM_ROUND_MB=128`;
  `mnaccept.sh <hold> --only mn` with `MNACCEPT_ENV="COMM_TRANSPORT=xshmem"`: 5/5.
  Default build of the merge (item 9p7, m): `tools/gate.sh` 1, 2 and 3 n PASS (VERIFY OK, digits identical); one L run at 3 n, 1.9323e11: 152.8 s, VERIFY OK.
- Memory: whole-exchange staging at 3 n, 1.9323e11 fails the pre-launch budget (peak 448.5 GB per node, above MemAvailable; planner heap 10752 MiB per PE): rounds are required at that size.

## 7. First timing (item 9p5, m)

3 n, 1.9323e11 digits, 17 runs (one L warm-up, then 8 ABBA pairs L/XRD), 17/17 VERIFY OK. L = default libfabric reference: 157.3 s, sd 6.8 (n 9). XRD = xshmem, `XSHMEM_ROUND_MB=128`, rot order, dc, worker on: 149.3 s, sd 1.2 (n 8).
Paired difference XRD - L: -8.5 s, 95 % CI [-14.0, -3.0], 8 of 8 pairs negative. Reference blocks lref3b and lref3c pooled: n 23, mean about 156.7 s, sd about 5.
Diagnostic arms (item 9p6, m, 3 runs each, all with `XSHMEM_ROUND_MB=128`): X (rot, dc) 146.8 s sd 1.0; XF (rot, `XSHMEM_OFI_SIG=flush`, the libsma-like completion) 148.8 sd 1.6;
XS (`XSHMEM_ORDER=seq`) 155.0 sd 3.6; L 154.6 sd 4.0; paired XS - X +8.2 s, 95 % CI [+1.4, +15.0]. The gain over the reference comes from the rotated peer order (XS is level with L);
the target-like flush completion costs about 2 s.
Not adopted: the default stays `shmem`; adoption is the user's decision (TASKS).

## 8. Target port checklist

1. `make XSHMEM=cray`: compiles `xshmem_cray.h` with the 12.1 header and `-lsma`; no libfabric, no `xshmem_boot.c`. Edit only the four selector blocks of `xshmem_cray.h`; the engine is unchanged.
2. Launch: the SHMEM environment per `pe_geo` (symmetric size, NIC policy); the planner's heap rule must read the variable 12.1 actually uses (unknown).
3. Proof on aac7 before the target: `XSHMEM=cray` against 11.8 with the host heap, digits identical (the build is shown to link; the engine run is not done).
4. Day-one checks on the target with the same `t_xshmem` source: T1 can context l use NIC l (per-context NIC policy, counters per NIC; decides lanes against 2 PEs per GPU); T2 host `signal_wait_until` on a
   device-heap word and on a host word; T3 device-heap allocator names and `SHMEM_ENV_DISPLAY`; T4 put_signal against put + quiet + signal (16 MiB, ordered and flush counters); T5 put from a non-heap device source,
   `shmem_ptr` on a node-local PE; T6 NIC count, NIC-APU map, bandwidth with 1 and 2 lanes; T7 thread levels, progress, `shmem_ctx_quiet` cost with 4 PEs per node; T8 translation-miss counters of the device heap
   (expect below 10^3 per group; 10^5 or more means a registration problem).

## 9. Open items

- Rounds are needed for memory at the larger sizes (section 6); the default of `XSHMEM_ROUND_MB` when xshmem is adopted is not chosen.
- Two NICs per PE: unknown in 12.1 (T1); the fallbacks are 2 PEs per GPU (`XSHMEM_PES_PER_GPU`) or one NIC.
- On-node traffic goes through the NIC on aac7 (no `/dev/xpmem`): 14.3 against about 22 GB/s per PE in the probe; the target's on-node path should differ. `XSHMEM_LOCAL`/`XSHMEM_IPC` are unmeasured.
- `XSHMEM_LANES` above 1 is untested at scale (virtual lanes only ran in t_xshmem).
- The one-process-per-GPU restructure (stages S1 and S2) is not started; S3a answers only the engine-against-reference speed, with loopback PEs inside the present process model.
- dmabuf at smaller sizes, staleness of host loads of GPU words under load, flush and waw signal modes, the comparison against `comm_ofi` at transport level, no COMM_TRACE / XSTATS hooks in the engine.
- Poll-only mode (`ECALC_XEX_THREAD=0`) loses the layered overlap by design and does not work with `XSHMEM_ROUND_MB`.

## RESUME

State: engine, backends, tests and docs on branch `xshmem`; gates 9p1/9p4 PASS, timing 9p5 done. Collect on aac7: `ssh <cluster> '<xtest or xloop summary file>'` for the raw tables.
Next: user decision on adoption and on the open items above (TASKS "Proposed").
