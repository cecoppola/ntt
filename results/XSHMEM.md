# XSHMEM: target-shaped communication (Cray SHMEM 12.1 style) over libfabric on aac7, plan TARGET1, 2026-10-21

Scope: the xshmem layer (API, backends, boot and topology, tests), what Cray SHMEM 11.8 and the cxi provider were measured to do, the engine
`comm_xshmem.c` behind `COMM_TRANSPORT=xshmem`, its gates and first timing, and the open items for the target port. Merged from the former reports
B1-B4 and S3A_A1/A2 (the six `XSHMEM_*.md` files). Labels: m measured, mod modelled, I inferred, a assumed. All numbers are single aac7 runs unless a count is given.
The sections PPG (one process per GPU, S2d timing) and BSPLIT (`RNS_BATCH_SPLIT`) follow the craylane section. Settings: `ecalc/README.md` (rows `XSHMEM*`, `COMM_TRANSPORT`; `ECALC_XEX_THREAD` was removed 2026-10-25, the exchange worker always runs). Design source and trace data are kept outside the repository.

Sections moved to `archive:results_merged_2026-10-24/XSHMEM.md` (2026-10-24 cleanup): §3 (Cray SHMEM 11.8 on cxi), §4 (cxi probe facts), §9 (open items of 2026-10-21), the full PPG and BSPLIT texts and the RESUME block.

## 1. Purpose and shape

After the aac7 Cray SHMEM tuning phase (closing record in `results/SHMEM.md`) the project builds the communication the target will have:
cray-shmem 12.1 calls, GPU-memory buffers, several NICs per node. 12.1 cannot be run on aac7 (11.8 only), so the same calls are emulated with libfabric
(cxi) behind one header, `ecalc/xshmem.h`, a 1:1 mapping of the 12.1 calls the engine needs (23 calls: init, contexts, heap alloc, put_nbi, quiet,
signal_set / put_signal / signal wait and test, barrier, topology and capability queries).
- `XSHMEM=ofi` (aac7): `xshmem_ofi.c` over libfabric, `xshmem_boot.c` (PMI2 address and blob exchange), `pe_geo.c` (GPU, NUMA and NIC map).
- `XSHMEM=cray` (target): `xshmem_cray.h`, static inline forwarding to cray-shmem; no libfabric. The four places that differ in 12.1 (device-heap allocator,
  signal set, signal wait, context creation) are selector blocks (`XSHMEM_CRAY_DEFS=-DXSHMEM_CRAY_HEAP|_SIGSET|_WAIT|_TEST|_PUTSIG_SPLIT`); with the 11.8 headers it compiles and
  links with a host-heap fallback (m, aac7 login node). The engine and the tests include only `xshmem.h`, so the port changes no engine file.
- Build default (since 2026-10-25, Q103): `XSHMEM=ofi` wherever libfabric and the PMI2 header are found, else `XSHMEM=0` (no engine; `COMM_TRANSPORT=xshmem` is then a fatal at start). When this section was written the engine was opt-in and `make -n` of the default build was byte-identical to before (m).

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

## PPG — one process per GPU, `ECALC_PE_PER_GPU` (TARGET1 stage S2; condensed 2026-10-24, full text `archive:results_merged_2026-10-24/XSHMEM.md`; "PPG.md S2d" citations mean the S2d timing subsection)
**Change.** Switch `ECALC_PE_PER_GPU` (default 0): one process, one xshmem PE and one device per GPU (the Cray layout, K = 1); the gate was 9v8 on aac7 (3 nodes): component tests (`t_vpe all --ppg` 4 / 8 / 12 PASS at 1 / 2 / 3 n, `t_out --ppg`, unit set), off path unchanged, digits identical off vs on (m). The virtual-PE thread mode that was carried with it was removed 2026-10-23 (Q86: 4-14 % of 1 n 1e8 thread-mode runs gave wrong digits, root cause not found).
**Launcher.** `mnrun.sh` plans P runs with the per-node PE count (FIT refuses a run that does not fit before launch; pool and `XSHMEM_HEAP_MB` come from the plan at 1 / 2 / 3 n); on one node P picks `COMM_TRANSPORT=xshmem`. A division counter written by PE 0 only (`newton_x_dx`) failed PEs 1-3 on a late carry; every PE now writes its own (`NST`, `newton_db.c`; P 1 n 3e10 VERIFY OK on 4 of 4 PEs, sha equal to D e050c5ae142ae995).
**S2d timing (aac7, ABBA, m).**

| size, nodes | D s | P s | P - D, 95 % CI | note |
|---|---|---|---|---|
| 1e9, 1 n | 9.07 | 4.90 | -4.17 [-4.95, -3.39] | 6 pairs |
| 1e10, 1 n | 19.74 | 20.38 | +0.63 [-0.41, +1.68] | init 5.87 vs 11.62 but dm 7.17 vs 1.85, recip 2.59 vs 0.90 (one node's exchange goes through xshmem between 4 processes instead of shared memory) |
| 1e10, 3 n | 21.97 (sd 0.43, n 10) | 14.99 (sd 0.52, n 8) | -6.92 [-7.57, -6.28] | 8 pairs; init 13.23 vs 4.53, bs and dm each about 1 s slower |
| 1e11, 3 n (PG settings: DIST_CHUNKS=8, MN_T_CHUNK_MB=512, BS_SEED_CHUNK_MB=1024, COMM_SHMEM_POOL_MB=512, `ECALC_NODE_GB=420`) | 83.36 (sd 1.18) | 84.32 (sd 4.43) | +0.96 [-5.54, +7.47] | 4 pairs; init 30.40 vs 10.03, bs 24.80 vs 36.95, dm 25.53 vs 35.05, recip 9.85 vs 13.11 |

Reading: P saves start-up (-8.7 s at 3 n 1e10, -20 s at 3 n 1e11) and loses most of it in bs and dm at 1e11; equal within noise at 3 n 1e11. At 1.9323e11 P needs `RNS_BATCH_SPLIT` (a PE's leaf top product exceeds one device's plane pool and the striped tier needs all 4 devices; `MN_PLAN_ONLY` now prints the batch-tier check). Default stays D.

## BSPLIT — own-device split of oversized batch products under one process per GPU, `RNS_BATCH_SPLIT` (condensed 2026-10-24; full text in the archive file above)
**Change.** Switch `RNS_BATCH_SPLIT` (0 off, byte-identical default; 1 = on under P when the local tier cannot hold the product; 20..29 forced for tests): the operands are cut into a ka x kb grid of pieces that fit one device, pieces sharing a B piece go through the local tier as one batch, outputs are added into C with carries in own kernels; staging rises to 2 GiB per PE. Cost: about 4.3 GB pinned host per node (not in the fit model); the B transform is not shared across A pieces.
**Gates (m).** `t_mul 0 split` VERIFY OK; planner with the switch off: 12 outputs identical to main, with 42-case comparisons only P + split plans move; PG 3 n 1.9323e11 at cap 2^30 plans FITS with the switch (device 327.23 / 373.44 GB, node 392.65 / 420 GB), REFUSED without. Digits identical to D: 1 n 1e9, 1e10 and 3e10 (sha e050c5ae142ae995), 2 n 1e11 (sha 578f5efb0ff2b9af, 307.85 s against D 264.73 s, single runs), 3 n 1.9323e11 (sha e38f16dac94299b8: PGS 437.72 s, D 378.07 s, single runs; the P and split cost at 3 n full size is large, not a speed claim).
**Planner heap model.** The first two gate runs died on the xshmem DATA heap: under P without rounds a tree level stages the PE's whole share of the operand and the receive side at once (`xex_post`), which `binsplit_shmem_pool_need` did not model. The model now uses the largest node's share (lgamma tail of the term ranges), a margin of max(256 MiB, staged / 16) and the allgather scratch; planned heaps: 2 n 1e11 8960 MiB, 3 n 1.9323e11 10240 (modelled); measured need 6206-6416 MiB at 2 n (conservative) and 9216 at 3 n. Open: a 1.3 % excess and the 2 n receive ratio are unexplained.
**Follow-ups.** Single-plane arm, writing pieces directly into C, ABBA timing pairs (none yet).

## XWLAT: exchange-worker latency and the heap-tail test (branch xwlat, 2026-10-24, measured, aac7 2 n)

**Heap-tail test.** `t_xshmem tail` (xshmem tests, part of `correct`): the DATA heap is rounded to 2 MiB so the request end is the heap end; a HIP kernel reads and writes the first and the last 8 B (and the last byte), the host copy-out agrees, 8 B puts go to the next PE's first and last words and a span ends 32 B before the heap end; a kernel then reads the received tail. Items 9zw7_xwxt1: path 1 (ofi) and path 2 (cray, lanes 2), 8 PEs: `SEC tail: PASS`, XT_RESULT PASS (basic also PASS). (A get does not exist in xshmem.h: the peer's put is the test.)

**Where the 30 ms went.** The worker (`comm_xshmem.c` worker(), xex_send) never sleeps while an exchange is pending (it polls with `sched_yield`); the condvar is used only when it is idle. The evidence (9q6g/9q6h) was the removed thread mode (4 PE threads, one process); with one process per GPU the numbers are the reverse.

**Measured (2 n 1e10, `ecalc total`, ABBA, 4 pairs each; 9zw6_xwab1, b47edce):**

| arm A vs B | A s | B s | B - A s | 95 % CI |
|---|---|---|---|---|
| P poll-only (default then) vs `ECALC_XEX_THREAD=1` | 40.17 | 31.96 | -8.21 | [-9.43, -6.99] |
| D default vs `XSHMEM_WORKER_SPIN=1` | 39.23 | 38.50 | -0.73 | [-1.53, 0.08] |

P = `ECALC_PE_PER_GPU=1 XSHMEM_LANES=2` (path 2). Single screening runs (9zw4_xwprobe3): `XSHMEM_YIELD=0` D 38.62 s, P+worker 30.67 s; spin+yield0 D 40.53, P 30.63: all within the noise of the arms above. The poll split at P shows 26.9 ms in "peers posted" (the receiver waits for the slowest peer: skew, not worker cost). Digits: gate 1e9 identical for P + worker (9zw6 g9_thr) and D + spin (g9_spin). Gate 9zw5_xwlat1 (wave3 gate on the branch): gd1, gd2, unit 9 PASS, gc1, gc2 PASS. Note: 1e10 arms print "digits DIFFERS": gate.sh compares with the 1e9 reference only; VERIFY OK on all.

**Switch.** `XSHMEM_WORKER_SPIN` (an idle worker polls the queue instead of sleeping on the condvar): tried, no gain, removed again (commit after b47edce). Default `ECALC_XEX_THREAD=1` under P: adopted (Q96), see the recommendation below.

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

**Recommendation.** Adopt `ECALC_XEX_THREAD=1` as the default under P on path 2: a robust 6.5-7.5 s start-up saving and half the host memory, digits identical (sha 578f5efb0ff2b9af at 1e11, 1e9 gates), bs / dm neutral within noise. Adopted by the user (Q96, 2026-10-24): the worker is now the default in every layout (branch xexdef).

**Check on main after the merge (item 9zz5_main11, main bef6a16, 2 n 1e11, measured 2026-10-24 21:18-21:57 ET).** Digit identity at production
size: path 1 (layout D, default launch) and path 2 (cray, P, lanes 2, BSPLIT settings) both give sha 578f5efb0ff2b9af, the reference of
xwconf3. Q96 at 1e11 on path 2 (no digit file, 3 ABBA pairs): worker (default) 137.16 / 136.68 / 158.40 s, `ECALC_XEX_THREAD=0` 145.92 /
144.91 / 153.79 s; poll - worker +4.13 s, 95 % CI [-14.68, 22.94] (two pairs +8.2 to +8.8 s, one -4.6 s): same direction as the start-up
gain, not resolved at n = 3.

**Best SHMEM against best libfabric after Q96, 2 n (item 9zz7_gap2n, main b47d557, same hold, measured 2026-10-24 22:05-22:29 ET).**
Both in layout P (one process per GPU; the fastest libfabric layout at 2 n), the same BSPLIT settings, no digit file, ABBA. 2 n 1e10, 4 pairs:
ofi P 14.79 s, Cray P lanes 2 15.11 s, Cray - ofi +0.32 s, 95 % CI [-0.51, 1.15]. 2 n 1e11, 4 pairs: ofi P 156.13 s, Cray 146.44 s,
-9.69 s, CI [-27.90, 8.52] (Cray bimodal: 135.8 / 136.2 against 154.8 / 159.1 s). At 2 n the two paths are at parity after Q96; the 3 n
+7.5 % above predates Q96 (poll-only under P) and is not re-measured (only 2 NIC nodes on aac7).


## XDIAG: day-one checks T1-T8 triage and diagnostic arms (branch xdiag, 2026-10-24, aac7 2 n, measured)

Items 9zz2_xdiagt (`t_xshmem`, `t_comm`; REF fba18db), 9zz3_xdiage (ecalc, 2 n 1e10; fba18db), 9zz4_xlane (REF bef6a16, main with Q96); all rc 0; ecalc runs VERIFY OK on every PE, no digit file. aac7 has libsma 11.8, one NIC per PE and no xpmem, so T1-T8 split as follows.

| Check | On aac7 | Reading |
|---|---|---|
| T1 context l on NIC l | target-only | aac7 lanes are virtual (one NIC per PE) |
| T2 host `signal_wait_until` on a device-heap word | target-only | aac7 cray heap is host memory (`caps: dev_heap 0 host_pollable 1`) |
| T3 allocator names, `SHMEM_ENV_DISPLAY` | partial | the 11.8 settings dump is recorded (`c_env`, `correct` PASS, 8 PEs); device-heap names target-only |
| T4 put_signal vs put+quiet+signal | run (ofi) | `XT_PUTSIG=nbi`: lat 8 B hop 14.9 us (same as split), a2a 64 MiB 10.3 GB/s per PE against 13.8 (split), stream equal 23.2; the split form stays |
| T5 `shmem_ptr` | target-only | ofi `xshmem_ptr` is NULL (the `XSHMEM_LOCAL`/`XSHMEM_IPC` warning is printed); cray `XSHMEM_LOCAL=1` in ecalc: no change (below) |
| T6 NIC count / bandwidth 1 vs 2 lanes | bandwidth only | stream 23.3 GB/s per PE in every lane count; NIC-APU map target-only |
| T7 thread levels, quiet cost, 4 PEs per node | run | thread level 3; 8 B pingpong 14.9 us (ofi x4), 8.3 us (cray x4), 11.0 (ofi x1), 7.3 (cray x1) |
| T8 translation misses | host heap only | ofi misses about 1e3 per section; cray host heap 1e7 per section (hipHostRegister'd libsma heap); the device-heap figure is target-only |

**Transport-level tables** (`t_xshmem bw`, 2 n x 4, GB/s per PE, single runs): ofi a2a 16 / 64 MiB 13.75 / 13.80, stream 23.30; cray a2a (L1 runs, see lanes below) 11-17 at 64 MiB. 2 n x 1: ofi a2a 22.2 / 22.95, cray 8.9 / 8.6 (single runs; cray with one PE per node pays the host-heap path, 9zz2 c_bw1). `comm_ofi` (`t_comm --bw 1 5 64`, 2 n x 1): 16 MiB 7.6, 64 MiB 9.8 GB/s per thread against xshmem 22.2 / 22.95. 2 n x 4: 9zz2 died at 64 MiB slabs (device 0 pool 1024 MiB cannot hold 512 MiB more; a test-configuration limit, 4 ranks share device 0); 9zz4 with `COMM_OFI_POOL_MB=2048` ran: 16 MiB 5.33, 64 MiB 3.67 GB/s per thread against xshmem 13.75 / 13.80.

**Flush, waw.** `XSHMEM_OFI_SIG=flush` passes `correct` at 2 n x 4; a2a / stream equal to dc (13.72 / 13.81, 23.29), 8 B hop latency 22.2 us against 14.9 (+7 us, flush chain per signal); ecalc 1e10 2 n D: +0.13 s, CI [-1.35, 1.61] (3 pairs): neutral, dc stays default. `waw` aborts "not yet implemented" (the switch is unbuilt; nothing to measure).

**ecalc arms** (2 n 1e10, path 1 = layout D, 3 rounds, arm minus base, 95 % CI t(df 2) 4.303): `ECALC_XEX_THREAD=0` +1.48 s [0.29, 2.66] (base 22.35 s): the worker wins, in line with XWLAT. `XSHMEM_LOCAL=1 XSHMEM_IPC=1` (ignored by ofi, warning seen 2 per run) +0.03 s [-1.56, 1.61]: the A/A control, so the noise floor is about 1.5 s. comm_ofi (`COMM_TRANSPORT=shmem COMM_OFI=1`) +2.89 s [0.72, 5.06]: xshmem 11 % faster at 2 n 1e10. Path 1P (ofi, `ECALC_PE_PER_GPU=1`, 2 pairs): `ECALC_XEX_THREAD=0` +1.17 s [-3.1, 5.4] (base 15.0 s), not resolved. Path 2 (cray, P, L2, 2 rounds): poll-only 23.7 s against 15.8 s base (23.1 and 24.3 against 14.9 and 16.8; the CI [-11.9, 27.7] is wide at 2 pairs but every poll-only run is 6-9 s above every base run); `XSHMEM_LOCAL=1` (cray `shmem_ptr`) -0.28 s [-4.35, 3.79]: no effect, as expected without xpmem.

**Lanes (9zz4, cray, 4 ABBA pairs from the per-run XT lines; the item's own XTSTAT lines are wrong and not cited).** Per pair L1 and L2 (a2a 64 MiB, GB/s per PE): L1 17.35, 11.29, 17.31, 11.59 (mean 14.39), L2 14.05, 15.39, 14.44, 15.41 (mean 14.82): L2 - L1 +0.44, CI [-6.04, 6.92]. a2a 16 MiB L1 13.59, L2 13.34 (-0.24, [-4.33, 3.85]); stream 16 MiB 21.85 / 21.55, 64 MiB 21.96 / 21.79 (-0.17, [-1.53, 1.19]). Whole run (path 2, 1e10, 3 ABBA pairs): L1 16.20 s, L2 16.65 s, L2 - L1 +0.45 s, CI [-1.43, 2.32]. Reading: lanes 1 vs 2 neutral. Cray L1 is bimodal in the microbenchmark (17.3 or 11.3-11.6 GB/s per PE, alternating by run); the 9zz2 single pair (L2 10.9, L1 17.5) was this bimodality, not a lane effect; L2 is steadier (14.0-15.4). The cause of the 11 / 17 split is not known (possibly which NIC / NUMA placement a run lands on).
Stat-code bug (item 9zz4_xlane): the awk block reused the variable `k` as both the pair counter and the result of `split`, so the XTSTAT lines report 11 pairs and wrong means; the whole-run STAT line was computed separately and is correct.

**Target-only list:** T1, T2, T5 on a device heap, T6 NIC-APU map and 1 vs 2 real NICs, T8 device-heap counters, the device-heap part of T3; `waw` once built.
