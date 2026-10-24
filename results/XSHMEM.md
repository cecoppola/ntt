# XSHMEM: target-shaped communication (Cray SHMEM 12.1 style) over libfabric on aac7, plan TARGET1, 2026-10-21

Scope: the xshmem layer (API, backends, boot and topology, tests), what Cray SHMEM 11.8 and the cxi provider were measured to do, the engine
`comm_xshmem.c` behind `COMM_TRANSPORT=xshmem`, its gates and first timing, and the open items for the target port. Merged from the former reports
B1-B4 and S3A_A1/A2 (the six `XSHMEM_*.md` files). Labels: m measured, mod modelled, I inferred, a assumed. All numbers are single aac7 runs unless a count is given.
The sections PPG (one process per GPU, S2d timing) and BSPLIT (`RNS_BATCH_SPLIT`) follow the craylane section. Settings: `ecalc/README.md` (rows `XSHMEM*`, `ECALC_XEX_THREAD`, `COMM_TRANSPORT`). Design source and trace data are kept outside the repository.

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

9p8 (m): 3 n, 1.9323e11 digits, all arms with `XSHMEM_ROUND_MB=128`, 14 runs, all VERIFY OK. XL (`XSHMEM_LANES=2`, both lanes on the PE's one NIC) 147.6 s sd 2.1 (n 4); XF (flush completion) 148.8 sd 2.2 (n 4);
X (rot, dc) 150.1 sd 3.5 (n 4); L (libfabric reference) 151.8 sd 0.5 (n 2). Paired XL - X per round: -2.3, -8.5, -2.8 s (n 3): a lead for 2 lanes, not yet significant.

9p9 (m): transport-only test `t_xa2a` (branch `xdiag`), equal all-to-all, GB/s per PE. A = 1 process per node with 4 loopback PEs; B = 4 processes per node with 1 PE each.
- 64 MiB: 1 n A 21.32 / B 21.33; 2 n A 14.39 / B 15.09; 3 n A 14.08 and 14.07 (repeat) / B 13.96 and 12.62 (repeat).
- 256 KiB (the layered slab), 3 n: A 13.03, 13.22 / B 13.94, 13.59.
Reading: at 1 n and 2 n the layout costs nothing (B level or faster). At 3 n the two 64 MiB repeats disagree (B -1 % and -10 % against A), so a layout cost at 3 n is not excluded; at 256 KiB B is about 5 % faster on the mean, not slower.
Coordinator conclusion (assumed from the P16 record, not measured here): the old 3.7x per-process slowdown is above the transport (its 4 PEs per node each drove all 4 APUs, and the replicated single-node chain was the largest term), so the one-process-per-GPU split (stage S2) is not blocked by the transport.

## 8. Target port checklist

1. `make XSHMEM=cray`: compiles `xshmem_cray.h` with the 12.1 header and `-lsma`; no libfabric, no `xshmem_boot.c`. Edit only the four selector blocks of `xshmem_cray.h`; the engine is unchanged.
2. Launch: the SHMEM environment per `pe_geo` (symmetric size, NIC policy); the planner's heap rule must read the variable 12.1 actually uses (unknown).
3. Proof on aac7 before the target (P5, branch p5): `make SHMEM_CRAY=1 XSHMEM=cray` builds `t_xshmem` and the whole ecalc (engine `comm_xshmem.c` unchanged, no `xshmem_ofi.c` / `xshmem_boot.c` / `pe_geo.c`) against 11.8 with the host heap (`shmem_malloc`, registered with `hipHostRegister` by `xshmem_cray.h`; `XSHMEM_CRAY_HOSTREG=0` switches it off): measured, both build and link on the login node. One PE per process: the engine runs with K=1 (the 4 APU meshes share the PE's heap; no loopback PEs), so give `XSHMEM_HEAP_MB` the node's whole heap (4 times the per-PE value of the ofi runs). Run (aac7, 3 nodes, item `9r0_p5cray3`, 2026-10-21, measured): `t_xshmem` built `XSHMEM=cray` against libsma 11.8 (host heap, `hipHostRegister`) correctness PASS at 1/2/3 n x 4 PEs (sections basic, exch, deadlock, layer, limit); ecalc `XSHMEM=cray COMM_TRANSPORT=xshmem` at 1e9 digits on 2 and 3 n VERIFY OK, digits identical; default gates 1 and 3 n PASS; zero engine changes (K=1 fallback). Gate record (aac7, 2026-10-21, measured): 9s1_xgate3p5b (6872b50, `XSHMEM=ofi`): g1, g2x, g3x, g3 PASS at 1e9 with identical digits, smoke 1/2/3 n 0 FAIL, mnaccept `--only mn` with xshmem 5/5 PASS, 3 n 1.9323e11 VERIFY OK 159.8 s. 9s2_p5cray3c (6872b50, `XSHMEM=cray`): g2x FAILED with a GPU memory access fault at the heap tail on both nodes. Cause: the registration covered only the whole pages inside the non-page-aligned DATA heap, so the tail page stayed pageable. Fix 0dd3627: register the enclosing page range. 9s3_p5cray3d (0dd3627, `XSHMEM=cray`) SUCCESS: `t_xshmem` PASS at 1/2/3 n (4/8/12 PEs, all sections); g1, g2x, g3x, g3 PASS at 1e9, VERIFY OK, digits identical (ecalc total 10.4 / 20.3 / 20.7 / 17.9 s); 3 n 1e9 VERIFY OK 20.55 s. Lesson for the target port: the DATA heap is not page-aligned, so register (or pin) the page range that encloses it, never only the whole pages inside; a missed tail page faults only when a GPU touches it, so the small tests can pass.
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

## 10. Adopted as the default multi-node transport (Q73, user decision 2026-10-21)

`COMM_TRANSPORT=xshmem` with `XSHMEM_ROUND_MB=128` and `XSHMEM_ORDER=rot` is the default for more than one process (branch `xdef`).
Basis (m, §7): 3 n, 1.9323e11 digits, XRD 149.3 s vs L 157.3 s; 2 n, X 111.6 s vs L 114.1 s; plain xshmem without rounds fails the memory
guard at full size, hence the 128. Mechanics: `mnrun.sh` picks xshmem when `COMM_TRANSPORT` is unset and the launched executable has the
engine (else shmem); `XSHMEM_ROUND_MB` unset means 128 in the engine and both planners (`xshmem_round.h`; explicit 0 = off); the Makefile
defaults to `XSHMEM=ofi` where libfabric and the PMI2 header are found (`XSHMEM=0` builds without the engine). Fallback and reference:
explicit `COMM_TRANSPORT=shmem` (with `COMM_OFI=1` for arm L), unchanged. Gate: item 9t9_xdef3 (gate at 1, 2, 3 n default; 2 and 3 n explicit shmem;
mnaccept; units; 2 pairs D/L at full size).
Gate record (aac7, 3 healthy nodes, item 9t9_xdef3 on 501bcda, SUCCESS 20:19 EDT 2026-10-21, all measured): `gate.sh` default env at 1, 2, 3 n PASS (1e9, VERIFY OK,
digits identical; ecalc total 10.7 / 16.3 / 14.6 s); explicit `COMM_TRANSPORT=shmem COMM_OFI=1` at 2 and 3 n PASS, digits identical; `mnaccept --only mn` default
env 5/5 PASS (all logs over xshmem); `--only unit` 9/9 PASS. Full size 1.9323e11 at 3 n, order D L L D, all VERIFY OK: D (default) 155.38 and 151.06 s (mean 153.2);
L (explicit shmem) 157.87 and 164.77 s (mean 161.3); paired D-L -2.5 and -13.7 s, mean -8.1 s (n = 2). Engine check: D ran over xshmem with the rounds-128 line, L over SHMEM.

## Target shape (tgtshape, 2026-10-22; measured on aac7, one run per cell)
One process per GPU (P), 2 lanes (L2), Cray backend and the 2-PEs-per-GPU fallback (Q2) all give the digits of the default layout D (1e9 bf9f4b08758b40d9, 1e10 3fc025473586ed22; 1 n 1e9/1e10, 2 n and 3 n 1e10; 3 n 1e11 VERIFY OK on both backends). XSHMEM_LANES was ignored by the engine before: now N > 1 stripes each payload put over N lane contexts (XSHMEM_LANE_MIN_KB), quieting all used lanes before the signals on lane 0; both lanes carry equal bytes; digits identical with 2 lanes (D, P) at 1/2/3 n on ofi and cray; lanes are not slower (aac7 has one NIC per PE, so no gain is expected): 3 n 1e10 total s ofi D 31.7 / DL2 33.4, P 25.2 / PL2 25.3, cray D 51.2 / DL2 52.9, P 31.6 / PL2 31.1. Cray D needs XSHMEM_HEAP_MB = 4 x the plan figure (the 4 APU threads share one heap). Cray Q2 needed the topo fix (XSHMEM_PES_PER_GPU). Details: REPORT_tgtshape (private).

## craylane (Cray stalls, NUMA placement, Cray vs libfabric; 2026-10-23, aac7, measured)
**Stall cause.** The multi-second stalls of the Cray backend (PL2 at 3 n, intermittent 2 n 1e9 stalls, first-exchange waits of 75 to 160 s at 3 n 1e11) were not SHMEM traffic: per-lane quiet, signals and payload times were 0.000 s; the waiting PEs
all waited for one or three peers that were still in the stream sync of their post (GPU work before the first exchange). Two facts explain them: (1) the Cray topology had no cpu list (`xshmem_topo()` never filled `cpus`; `mn.c` printed "launch mask and the NUMA node's cpus '' do not
intersect"): every process had 192 threads on all cpus and first-touched its heap anywhere; (2) the libsma heap is a shared `/dev/zero` mapping on 4 KiB pages (THP `shmem_enabled=never`; `MADV_HUGEPAGE` has no effect, `MADV_COLLAPSE` gives EAGAIN; no hugetlb pages), so a node
with fragmented memory shows direct compaction (`compact_stall` +85 and +1083 in the two slowest 9zs10 runs, +0 in the clean ones). `XSHMEM_THP_DISABLE=1` did not cure the stalls (9zs12: 78 s run). Pinning each process to its NUMA node did:
`XSHMEM_CRAY_NUMA=1` (pe_geo cpu list, pinned before libsma init) and `MNRUN_PPG_CPUBIND=ldom` (srun map_ldom, mem-bind local), together "LN": 3 n 1e10 NU 38.4/36.6 s, LD 36.2/35.1 s against 40 to 107 s without (9zs12, 24 runs, digits identical); 3 n 1e11 P control 115.5 s mean, NUMA 104.9, ldom 103.9, both 96.7 (9zs14).
`mnrun.sh` now sets both by default for a Cray build under `ECALC_PE_PER_GPU=1` (explicit values win; ofi and layout D unchanged). Not helpful (9zs12, 9zs17): thread level 2, `SHMEM_MAX_CTX`, signal set 2, signal test 1, split put+fence+signal, first-touch off, startup connect, no memset, GPU pretouch of the heap (`XSHMEM_CRAY_GPU_PRETOUCH`, 0.1 s, no change to the 4.5 s first-exchange post).
**Gates (measured).** Digits identical to D: 1e9 `bf9f4b08758b40d9`, 1e10 `3fc025473586ed22` at 1, 2, 3 n for D, P, PL2 on Cray (9zs19); 3 n 1e11 P 92.74/94.61 s, PL2 91.95/92.61 s, D 194.59 s, VERIFY OK on every PE; Q2 on Cray (2 PEs per GPU, NUMA pin): 2 n 1e9 11.82 s, 3 n 1e9 14.36 s, 3 n 1e10 39.36 s, digits identical (9zs20). Path 1 (ofi, Q85): main 25dcd49 vs this branch, ofi D at 3 n 1e11 interleaved, 5 pairs (9zs18): 81.22/81.74/81.24/82.85/81.25 vs 82.07/82.44/81.70/80.81/79.92 s, paired branch minus main mean -0.27 s, no regression; ofi 1 n and 2 n 1e9 digits identical.

**Re-gate on the merged tip (measured, 9zs24-9zs28, 2026-10-23).** ofi D 1 n and 2 n 1e9 digits `bf9f4b08758b40d9`; ofi P at 3 n 1e11, main 25dcd49 vs the merged tip, 5 interleaved pairs: 83.51 vs 81.94 s, paired -1.61 s (sd 3.50, 95% CI [-5.96, +2.74]), no regression. Cray D/P/PL2 at 1 n and 2 n 1e9 identical digits; Cray PL2 3 n 1e11 86.24 s, 13 VERIFY OK; t_xshmem LANES=2 on Cray passes at 1 and 3 n.
**Cray vs libfabric, 3 n 1e11, same hold, interleaved (9zs21, 5 pairs, measured).** ofi P 82.46/84.94/81.41/80.93/81.36 s (mean 82.22); Cray PL2 91.82/86.86/93.11/91.39/90.53 s (mean 90.74); paired PL2 - ofi P: 9.36/1.92/11.70/10.46/9.17 s, mean +8.52 s (sd 3.83, 95 % CI +3.8 to +13.3 s), +10.4 % (CI +4.6 to +16.2 %). Phases (s, means): init 18.3 vs 9.6 (+8.7), bs 36.3 vs 36.0, dm 34.8 vs 34.9, dc 1.4 vs 1.4, recip 13.0 vs 12.9: the compute phases are at parity, the gap is init.
**Second batch on main 2ec7d53 (9zs29, 6 pairs, same hold 13646, measured).** ofi P 89.03/81.85/81.80/86.11/82.30/85.07 s (mean 84.36, sd 2.91); Cray PL2 86.28/92.60/92.49/89.97/85.53/85.12 s (mean 88.67, sd 3.46); +4.31 s, +5.1 % (both backends show the slow mode in this batch). **Pooled, 11 interleaved pairs (9zs21 + 9zs29): ofi P 83.39 s, Cray PL2 89.61 s, paired mean +6.22 s (sd 5.09, 95 % CI +2.8 to +9.6 s), +7.5 % (CI +3.4 to +11.5 %).** This is the final Cray-vs-libfabric figure (Q81); init +8.2 s accounts for all of it (compute phases: Cray bs 35.6 / dm 33.7 vs ofi 37.2 / 36.0 in 9zs29).
Cray runs are bimodal at equal settings (fast mode 85.1 to 86.9 s, slow mode 90.5 to 94 s, bs/dm/recip each about 8 % longer; 16 of 31 Cray P/PL2 runs of 9zs19 to 9zs23 were in the slow mode, 15 in the fast mode (the PL2X pretouch arm of 9zs23, 85.5 to 92.7 s, shows no gain either); not the arm, not compaction (compact_stall unchanged in the slow 9zs22 runs), not the pin): the fast mode is +4.0 to +5.0 % over the ofi P mean (target 86.3 s met), the slow mode +10 to +14 %; the cause is open. 12-run PL2 mean 90.07 s (+9.5 %).
**Init (9zs17, 12 runs, trace on).** `shmem_init_thread` 6.2 s on every PE (libsma registers the 9.9 GB heap with the NIC), our zeroing 0.22 s, `hipHostRegister` 1.5 s (3.1 s on some PEs), first barrier 1.4 s: 18 s init against 9.6 s with libfabric. `SHMEM_OFI_DISABLE_FIRST_TOUCH`, `SHMEM_OFI_STARTUP_CONNECT` and skipping our memset change none of it.
**aac7-specific blockers (documented, not fixed).** The libsma heap is a 4 KiB shared mapping (no huge pages, NIC registration of 9.9 GB at init: 6.2 s); one NIC per PE (lanes give no bandwidth, only the 12.1 two-NIC target can use them); 11.8 has a blocking put_signal and no `shmem_ctx_signal_set`; dynamic endpoints are opened at init; `SHMEM_OFI_PROGRESS=AUTO` aborts libsma init (no matching cxi provider), the library progress thread stays; the intermittent slow mode above. Cray layout D is 2.4 times slower than P at 3 n 1e11 (194.6 s: four threads share one heap and one process).
**Q2.** Works on Cray with the NUMA pin (needed the topo fix for `XSHMEM_PES_PER_GPU`); the planner per-process host term (about 5 GB per PE) is in TASKS Proposed.
**Switches (all off by default except as noted).** `XSHMEM_LANE_TRACE` (any layout), `XSHMEM_LANE_POLL`, `XSHMEM_SIG_LANES`, `XSHMEM_LANE_CTX`, `XSHMEM_CRAY_HEAP_THP`, `XSHMEM_THP_DISABLE`, `XSHMEM_CRAY_NUMA` (mnrun default on for Cray builds under P), `XSHMEM_THREAD_LEVEL`, `XSHMEM_CRAY_SIGSET`, `XSHMEM_CRAY_TEST`, `XSHMEM_CRAY_PUTSIG_SPLIT`, `XSHMEM_CRAY_NOMEMSET`, `XSHMEM_CRAY_GPU_PRETOUCH`; rows in ecalc/README.md. Build and run lines of both paths: README.md.

## PPG — one process per GPU, `ECALC_PE_PER_GPU` (TARGET1 stage S2; merged 2026-10-23 from PPG.md; "PPG.md S2d" citations mean the S2d timing subsection)
**Question.** Does one process per GPU (the Cray layout, xshmem K=1) give the same digits as the node-process default, and what does it cost or save?
**Change / branch.** Branch `ppg-int` (head 31d566a, squash-merged to main). Switch `ECALC_PE_PER_GPU` (default 0, off). Also carried: the S1 vPE stages b-e (`ECALC_VPE`); thread-mode full runs abort with `stages e not merged` (`VPE_STAGES_MERGED` lacks S1e: the S1e flow is not end-to-end correct); component tests still run.
**Method.** aac7, 3 healthy nodes, item 9v8_ppgint3i on 31d566a (SUCCESS 2026-10-21 23:46 EDT). Component tests: `t_vpe` host and stages with and without `--ppg`; `t_vpe all --ppg` at 1/2/3 n; `t_out --vpe` and `--ppg`; `t_bs`; `t_mn_grid --vpe` in process and thread mode. Guard: thread-mode e8 abort check. `mnaccept` unit set. Off path v0off1/2/3. End to end off vs on (`ECALC_PE_PER_GPU=1`), digits compared.
**Results.** All (measured).

| check | result |
|---|---|
| `t_vpe all --ppg` at 1 / 2 / 3 n | 4 / 8 / 12 PASS |
| thread-mode e8 abort | rc 3, `stages e not merged` |
| off path v0off1/2/3 | PASS, digits identical |
| mnaccept unit set | PASS |

| size | nodes | off (s) | on (s) | digits |
|---|---|---|---|---|
| 1e9 | 1 | 9.73 | 8.94 | identical |
| 1e9 | 3 | 14.27 | 5.42 | identical |
| 1e10 | 3 | 32.83 | 24.68 | identical |

One run each: not a speed claim. Earlier gate 9v5 (4e32c99): same result plus XRD 3 n 1.9323e11 VERIFY OK 160.1 s (m).
Bugs found and fixed on the way: `rns_init` set up all 4 devices per process (OOM); the seed copy device was `region % nd` (device 0 for every PE, SEGV); the batch tier chose striped over 4 devices on a one-device process; the vPE exchange worker cost about 35x the poll mode, so poll is the default under vPE and process mode.

Launcher fixes (branch `fitppg`, gates 9w2x/9w2w on f212a9e/6aab0bc, 2026-10-22, measured): mnrun's FIT pre-check planned `ECALC_PE_PER_GPU=1` runs without the per-node PE count, so the plan died and a run that does not fit launched (9w2_s2dfeas3: P 1.9323e11 3 n killed after 16 s); now refused before launch (no node time). On one node, P now picks `COMM_TRANSPORT=xshmem` as for more than one process. Gate 9w2w: P full REFUSED before launch; 1e9 D vs P digits identical at 1 / 2 / 3 n (single runs D / P: 10.86 / 8.28, 15.21 / 6.64, 16.07 / 6.41 s).

Pool plan (branch `poolppg`, gate 9w2u on 5b4ec8a, 2026-10-22, measured): the launcher's pool-sizing plan had the same gap, so P runs kept `COMM_SHMEM_POOL_MB=8192` and no `XSHMEM_HEAP_MB` from the plan; now the plan sizes them at 2 / 3 n (pool 512 MiB from the plan; digits identical at 1 / 2 / 3 n). On one node the engine's pool rule still returns no pool line (TASKS Proposed).

### S2d timing
3 n 1e10 (item 9x0_s2d1e10_3n on s2dtest c96cb09e, 2026-10-22, measured): D 21.97 s (n 10, sd 0.43), P 14.99 s (n 8, sd 0.52); P - D over 8 ABBA pairs -6.92 s, 95 % CI [-7.57, -6.28]; A/A D2 - D +0.60 s (n 2). Phases D / P: init 13.23 / 4.53, bs 3.68 / 4.97, dm 4.11 / 4.96 s: the gain is all start-up; bs and dm are each ~1 s slower. 1 n (item 9x4_s2dtime1nb on s2fix 6b51c6c, 6 ABBA pairs each, measured): 1e9 D 9.07 / P 4.90 s, P - D -4.17 s [-4.95, -3.39]; 1e10 D 19.74 / P 20.38 s, P - D +0.63 s [-0.41, +1.68] (P init 5.87 vs 11.62, but dm 7.17 vs 1.85, recip 2.59 vs 0.90, bs 6.63 vs 5.70: one node's dm exchange goes through xshmem between 4 processes instead of shared memory in one). 3 n 1e11 (item 9x5_s2dfull3c, 4 ABBA pairs, all arms ECALC_NODE_GB=420; PG = P + DIST_CHUNKS=8 MN_T_CHUNK_MB=512 BS_SEED_CHUNK_MB=1024 COMM_SHMEM_POOL_MB=512 at cap 2^31, measured): D 83.36 s (sd 1.18), PG 84.32 s (sd 4.43; first run 90.95 s), PG - D +0.96 s [-5.54, +7.47]; phases D / PG: init 30.40 / 10.03, bs 24.80 / 36.95, dm 25.53 / 35.05, recip 9.85 / 13.11 s. 1.9323e11 at 3 n: P is refused (a PE's leaf top product exceeds one device's plane pool; decision Q75 / design batch split). Reading: one process per GPU saves start-up (-8.7 s at 3 n 1e10, -20 s at 3 n 1e11) but loses it in bs and dm at large sizes; no gain at 1e11; not a default candidate on aac7 as is.

S2fix (branch `s2fix`, 2026-10-22; plan outputs measured on the aac7 login node, gates 9x3b on 6b51c6c and 9x3c on dd8d821 PASS: P full size refused before launch, 1e9 D vs P digits identical at 1 / 2 / 3 n, pool from the plan at 1 / 2 / 3 n, 1 n P 1e10 runs (19.79 s); dd8d821 fixes review finding F1, the batch-tier check now reads the plane pool at the run's ECALC_PLANE_CAP): two P failures of the S2d runs. (1) 1 n 1e10 P died with rc 6 (`comm_xshmem: the DATA heap of PE 2 ... XSHMEM_HEAP_MB >= 4098 needed`): the pool rule returned 0 below 2 nodes, so a 1-node P plan printed no `plan pool` line and mnrun left the default heap. The rule now models the node's PE group (size = PEs per node) when `ECALC_PE_PER_GPU=1` and the transport is xshmem: 1 n 1e10 plans `XSHMEM_HEAP_MB=8448`, 1e9 `768`; default plans unchanged (md5 of the plan pool/fit lines equal to the old build at 1e11, 1 and 3 n). (2) 3 n 1.9323e11 PG died with rc 3 (`rns_mul_batch: the striped tier needs all 4 devices`): a PE's leaf top product (1.02e9 limbs = 2^30 points x 3 primes) exceeds one device's plane pool (3 x 2^29 limbs at cap 2^31, 3 x 2^28 at 2^30), so rns_mul_batch falls to the striped tier, which a one-device process cannot run; the planner did not model it. `MN_PLAN_ONLY` now prints `plan fit    batch tier ...` under P and refuses (`plan REFUSED`, mnrun rc 4 before launch) when the leaf's largest batch product does not fit; at 3 nodes cap 2^31 allows up to about 1.0e11 digits (1e11: 5.3e8 limbs, local tier OK, node 370.9 GB modelled with `DIST_CHUNKS=8 MN_T_CHUNK_MB=512 BS_SEED_CHUNK_MB=1024`; 1.2e11 refused), 1.9323e11 needs 6+ nodes or P is not usable. Two log artefacts fixed: the `BS_ARENA_ROOM ... is dropped` message came from the pool rule's room probe seeing no term range under P (arena 1726 GB instead of 197), and the `bs: arenas ... tails` figure summed uninitialised regions 1-3.

**Verdict.** Gate passed: merged off by default. S2d (2026-10-22): faster only at small sizes (start-up); equal at 3 n 1e11; full size needs Q75. Keep off by default.
**Open issues.** S2d. An omp team left PEs on device 0 (`vpe_rebind`) and per-vPE macros read inside omp regions: both fixed on `vpe-int`, not here. The thread mode (S1, `ECALC_VPE`, with the S1e race: 4-14 % of 1 n 1e8 thread-mode runs gave wrong digits) was removed from main on 2026-10-23 (Q86); the code and the investigation stay on branch `vpe-int`.
**RESUME.** Merged from `ppg-int`; next: S2d paired timing, then S1e on `vpe-int`.

S2 NST fix (branch `tgtshape`, 2026-10-22, measured): in process mode (`ECALC_PE_PER_GPU=1`) the division's process-wide counters, among them the deferred-correction value `newton_x_dx`, were written by vPE lane 0 only, which in process mode is PE 0 alone; a late carry in X's last limb (a data-dependent event, seen at 1 n 3e10) was then patched on PE 0 only and PEs 1-3 failed `digits == X mod q` (1 of 4 PEs VERIFY OK, 152 s). Every PE process now writes its own counters (`NST` in `newton_db.c`); D is untouched. Item 9zf1a on c78994e: P 1 n 3e10 VERIFY OK on 4 of 4 PEs, digit sha equal to D (e050c5ae142ae995; P 149.4 s, D 89.3 s); 2e10 sha equal to D.

## BSPLIT — own-device split of oversized batch products under one process per GPU, `RNS_BATCH_SPLIT` (merged 2026-10-23 from BSPLIT.md)
**Question.** Under `ECALC_PE_PER_GPU=1` the batch tier has no striped tier, so a batch product larger than the local tier fatals (S2c). Can the product be cut into pieces that fit one device, with identical digits?
**Change / branch.** Branch `batchsplit`. Switch `RNS_BATCH_SPLIT` (0 off, byte-identical default; 1 = on under P when the local tier cannot hold the product; 20..29 = forced in any mode, pieces of at most 2^k limbs, for tests). The operands are cut into a ka x kb grid; pieces sharing one B piece go through the local tier as one batch; outputs are added into C with carries in own kernels. With the switch at 1 under P the staging is raised to 2 GiB per PE (pieces up to 2^28 limbs).
**Method.** aac7 items 9zd_bsplit1c (1 n, commit fb09295), then 2 n and 3 n items. Gates per TESTING.md.
**Results.** (measured unless noted)

| check | result |
|---|---|
| `t_mul 0 split` (6 forced-split cases vs unsplit tier, limbs, lengths, residues), fb09295 | VERIFY OK, 10 split lines |
| planner, switch off, 12 outputs (1/2/3/6/576 n, with and without P) vs main | 0 differ |
| plan PG 3 n 1.9323e11 cap 2^30, switch on | FITS (device 327.23 / 373.44 GB, node 392.65 / 420 GB); off: REFUSED |
| plan PG cap 2^31 | does not fit even on main (node 564 / 420 GB) |
| split shapes at cap 2^30 (modelled) | level 20: 2x1, x0.90; level 21: 4x2, x1.70; level 22: 4x4 pieces of 2^28 points, x3.40 transform points |

Earlier run at 4e94a12: mixed small + large case failed on a wrong output length (striped tier leaves ncn 0); fixed in fb09295.
Cost: real pinned host memory grows by about 4.3 GB per node with the switch at 1 (not in the fit model). With one product per chunk the B transform is not shared across A pieces.

### 2 n gate (9zc_bsplit2y, commit 0414d1d, measured)
v2off2 PASS; 1e9 D vs forced pieces of 2^20 (49 split lines): digits identical; 1e11 D vs PGS (PG chunk settings, cap 2^30, RNS_BATCH_SPLIT=1, XSHMEM_HEAP_MB=8704; 16 split lines): digits identical (sha 578f5efb0ff2b9af), total 318.72 s against D 264.73 s (one run each, not a speed claim). The xshmem DATA heap peaked at 6416 MiB of 8704, below the new plan's 6912 MiB (the plan's value that was refused before: 4864).

### Planner heap model (xshmem DATA heap under one process per GPU)
Gate 9zc_bsplit2x (2 n 1e11, PG chunk settings, cap 2^30, RNS_BATCH_SPLIT=1, commit 0414d1d, measured): the run died after init with `the DATA heap of PE 0 (6144 MiB) cannot hold 1241 MiB more (in use 4965 MiB): XSHMEM_HEAP_MB >= 6209 needed`; the plan had given 4864 MiB (shortfall 1345 MiB, 28 %; the first try at the plan's value stopped at 4864 with 4967 needed). The two blocks are the tree level's child-share exchange (xex_post): the PE's share of P, 650820636 limbs = 4965 MiB, and a quarter of it, 1241 MiB, both staged at once and whole (one process per GPU has no rounds). `binsplit_shmem_pool_need` modelled only the products' internal exchanges (4542 MiB). Fix (commit 967d43c): for COMM_TRANSPORT=xshmem without rounds the model adds nq_leaf / NL limbs plus a quarter of it per level; 2 n 1e11 now plans 6622.7 MiB, XSHMEM_HEAP_MB=6912 (modelled; measured need 6206 + alignment). Planner outputs before and after (aac7 login node, 42 cases: 1e9 to 3.82e13 digits at 2, 3, 6, 576 nodes and 1 node, each with default, one process per GPU, xshmem with and without rounds, shmem, and the PG chunk settings): 2 move, both the PG chunk settings under xshmem (2 n 1.288e11: heap 4608 to 8960 MiB; 3 n 1.9323e11: 8704 to 8960 MiB); both still FITS (node 367.31 and 393.73 of 420 GB). D (default layout) and every shmem or rounds-bounded plan are byte-identical.

### 1 n 3e10 and the P digit bug (9zd_bsplit1w, commit a040a7b, measured)
D, P and PS30 (P, cap 2^30, RNS_BATCH_SPLIT=1; 4 split lines) all give sha e050c5ae142ae995. An earlier PS30 failure was not the split: plain P at 3e10 failed the same way (NST wrote newton_x_dx on PE 0 only; fixed by a040a7b, cherry-picked from tgtshape). Times 89.44 s D, 148.80 s P, 161.81 s PS30 (single runs; the P cost is P's, not the split's). Earlier 1 n gates (65d76ec): 1e9 D/P/PS20 identical, 1e10 D/PS22 identical (133 split lines), planner vs main 12 outputs 0 differ.

### Planner heap model, second fix (9zd_bsplit3zb died, measured; model f34abe1, a8a87c6, 501a6af)
3 n 1.9323e11 PGS with the first fix (heap 8960 MiB): PEs 4-7 stopped with `cannot hold 2304 MiB more (in use 6912 MiB): XSHMEM_HEAP_MB >= 9218`. The 1/4 receive term was a fit to the 2 n case. From xex_post: a whole exchange stages sst (the PE's part of the operand) and rst (its part of the receiving sequence) at once; a sending PE stages s = nq_leaf / NL, a receiver s prev / S (S nodes in the level's group, prev nodes per child), the running Q of c children 2 s prev c / S. Only the node that holds the sent child stages the full s (node 1 in the 3-way level). 3 n: modelled 4/3 s = 9100 MiB against measured 9216 (s is 1.3 % above nq_leaf / NL, unexplained); 2 n: modelled 7947 against measured 6206 (conservative; rst is sst/4 there, unexplained). Planned heaps: 2 n 1e11 8448, 3 n 9472 (covers 9218), 6 n 3.86e11 15104, 576 n 30720 MiB (the last two REFUSED on host fit under the PG settings). The comm_ofi / xshmem pool law also adds the control words per PE and 1 MiB (Cray backend 2 n 1e10 D needed 514 of 512): D xshmem heaps rise by 256 MiB at 1.288e11 on 2 n and 576 n, no fit verdict changes (42 cases, 8 moved).

### 3 n gate (9zd_bsplit3zc and 3zd, commit 501a6af, measured)
1.9323e11 digits on 3 n: PGS (PG chunk settings, cap 2^30, RNS_BATCH_SPLIT=1, planner heap 9472 MiB, no pin) 437.72 s, 13 VERIFY OK, sha e38f16dac94299b8. D (ECALC_NODE_GB=480, as D has always run this size; under 420 it is refused by the plan) 378.07 s, sha e38f16dac94299b8: digits identical. Single runs, no ABBA pairs (D runs without digit output were about 160 s; the P and split cost at 3 n is large, not a speed claim).

### Review fixes and 2 n re-gate (9zd_bsplit2z, commit ec005a5, measured)
Heap model from the largest node's share (lgamma tail of the term ranges; node shares are not uniform: PE 0 sat 6.4 % below the average at 2 n, node 1 1.3 % above at 3 n), a margin of max(256 MiB, staged/16) and the allgather scratch under P (the control words are in the CTRL heap, not the DATA heap), the 2 GiB staging in the node fit. Planned heaps (modelled): 2 n 1e11 8960 MiB, 3 n 1.9323e11 10240. 42-case planner comparison: D plans unchanged; only PG + RNS_BATCH_SPLIT=1 plans move (6 cases), no verdict changes (3 n node 403.39 of 420 GB). 2 n 1e11 PGS with the plan's heap and no pin: digits identical to D (sha 578f5efb0ff2b9af, 16 split lines), 307.85 s, DATA heap peak 6416 MiB of 8960 on the reporting PE (single run).

### BSPLIT RESUME
Gates done (1 n, 2 n, 3 n digits identical). Follow-ups: single-plane arm, mdev route, Karatsuba, larger staging or writing even pieces directly into C, ABBA timing pairs, and the unexplained 1.3 % and 2 n receive ratio in the heap model.

## RESUME

State: engine, backends, tests and docs on branch `xshmem`; gates 9p1/9p4 PASS, timing 9p5 done. Collect on aac7: `ssh <cluster> '<xtest or xloop summary file>'` for the raw tables.
Next: user decision on adoption and on the open items above (TASKS "Proposed").

## XWLAT: exchange-worker latency and the heap-tail test (branch xwlat, 2026-10-24, measured, aac7 2 n)

**Heap-tail test.** `t_xshmem tail` (xshmem tests, part of `correct`): the DATA heap is rounded to 2 MiB so the request end is the heap end; a HIP kernel reads and writes the first and the last 8 B (and the last byte), the host copy-out agrees, 8 B puts go to the next PE's first and last words and a span ends 32 B before the heap end; a kernel then reads the received tail. Items 9zw7_xwxt1: path 1 (ofi) and path 2 (cray, lanes 2), 8 PEs: `SEC tail: PASS`, XT_RESULT PASS (basic also PASS). (A get does not exist in xshmem.h: the peer's put is the test.)

**Where the 30 ms went.** The worker (`comm_xshmem.c` worker(), xex_send) never sleeps while an exchange is pending (it polls with `sched_yield`); the condvar is used only when it is idle. The evidence (9q6g/9q6h) was the removed thread mode (4 PE threads, one process); with one process per GPU the numbers are the reverse.

**Measured (2 n 1e10, `ecalc total`, ABBA, 4 pairs each; 9zw6_xwab1, b47edce):**

| arm A vs B | A s | B s | B - A s | 95 % CI |
|---|---|---|---|---|
| P poll-only (default) vs `ECALC_XEX_THREAD=1` | 40.17 | 31.96 | -8.21 | [-9.43, -6.99] |
| D default vs `XSHMEM_WORKER_SPIN=1` | 39.23 | 38.50 | -0.73 | [-1.53, 0.08] |

P = `ECALC_PE_PER_GPU=1 XSHMEM_LANES=2` (path 2). Single screening runs (9zw4_xwprobe3): `XSHMEM_YIELD=0` D 38.62 s, P+worker 30.67 s; spin+yield0 D 40.53, P 30.63: all within the noise of the arms above. The poll split at P shows 26.9 ms in "peers posted" (the receiver waits for the slowest peer: skew, not worker cost). Digits: gate 1e9 identical for P + worker (9zw6 g9_thr) and D + spin (g9_spin). Gate 9zw5_xwlat1 (wave3 gate on the branch): gd1, gd2, unit 9 PASS, gc1, gc2 PASS. Note: 1e10 arms print "digits DIFFERS": gate.sh compares with the 1e9 reference only; VERIFY OK on all.

**Switch.** `XSHMEM_WORKER_SPIN` (an idle worker polls the queue instead of sleeping on the condvar): tried, no gain, removed again (commit after b47edce). **Open for the user:** default `ECALC_XEX_THREAD=1` under P (-20 % at 2 n 1e10); not changed here (default change).

**1e11 confirmation (2 n, PG settings of the BSPLIT section: `ECALC_NODE_GB=420 ECALC_PE_PER_GPU=1 DIST_CHUNKS=8 ECALC_PLANE_CAP=2^30 MN_T_CHUNK_MB=512 BS_SEED_CHUNK_MB=1024 COMM_SHMEM_POOL_MB=512 RNS_BATCH_SPLIT=1`, path 2 adds `XSHMEM_LANES=2`; `ecalc total`, measured; items 9zwa_xwconf3, 9zwb_xwconf4, commit 75d4ea4).** Plain P with the default plan was refused before launch (node fit; with `ECALC_PLANE_CAP=2^30` also the largest batch product) until `RNS_BATCH_SPLIT=1` was added.

| run (path 2, digit file written) | total s | sha |
|---|---|---|
| poll 1 / worker 1 / worker 2 / poll 2 (ABBA) | 316.09 / 291.92 / 303.53 / 320.83 | all 578f5efb0ff2b9af (identical) |

Paired worker - poll with the digit file: -24.17 s, -17.30 s (mean -20.7 s, n = 2 pairs, 95 % CI about +-44 s: not significant by itself; both pairs agree in sign and with the 1e10 result -8.21 s [-9.43, -6.99]). The run was stopped after 4 runs because writing and hashing the digit file took about 400 s of 710 s per run.

Without a digit file (VERIFY only, 9zwb): path 2 worker 154.49 s, poll 145.06 s (one pair, worker +9.4 s: the opposite sign; the totals are not comparable with the runs that write digits). Path 1 (ofi) under P (default lanes), 2 pairs: poll 156.32 s, worker 159.66 s, +3.34 s, 95 % CI [-85, +92] (n = 2): no effect shown. So the worker gain under P at 1e11 appears only in the runs that write the digit file, and is not confirmed without it. 

**Where the worker wins (phase split of the five path-2 1e11 runs, measured, log lines `total ... (bs + dm + dc ...; init)`):**

| run | init s | bs s | dm s | dc s | DATA heap per PE | VmHWM |
|---|---|---|---|---|---|---|
| poll 1 / 2 / 3 | 19.9 / 19.1 / 19.3 | 74.0 / 78.6 / 68.9 | 70.3 / 71.8 / 54.7 | 151.8 / 151.3 / 2.1 | 9408 MiB | 16.6 GB |
| worker 1 / 2 / 3 | 12.4 / 12.5 / 12.9 | 67.1 / 71.5 / 72.1 | 55.5 / 67.0 / 67.3 | 156.9 / 152.5 / 2.1 | 960 MiB | 8.0 GB |

The worker's gain is the start-up: init is 6.5-7.5 s shorter in all five pairs' runs (no overlap between the groups), because the rounds path (128 MiB) needs a 960 MiB DATA heap per PE where the poll-only whole-exchange path needs 9408 MiB (heap allocation, registration and first touch) and half the host memory. bs and dm differ by less than their run-to-run spread (about +-7 s; worker dm 55.5 / 67.0 / 67.3 against poll 70.3 / 71.8 / 54.7), so no per-exchange gain is shown at 1e11; the 1e10 gain of 8.2 s is the same init effect. Pair 3 (worker +9.4 s) is dm 12.6 s slower in that run plus bs +3.2 s, against init -6.4 s: dm noise, not a warm-up effect (it was the first run after the build, but init and bs behave like the others). The same worker runs on ofi P did not gain (default lanes, 2 pairs, CI [-85, +92]).

**Recommendation.** Adopt `ECALC_XEX_THREAD=1` as the default under P on path 2: a robust 6.5-7.5 s start-up saving and half the host memory, digits identical (sha 578f5efb0ff2b9af at 1e11, 1e9 gates), bs / dm neutral within noise. It is a default change and stays the user's decision; not made on this branch.

