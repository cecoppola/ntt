# TRANSPORT — Transport: SHMEM, exchange, fabric, diagnostics

Scope: the XSHMEM/SHMEM transport, the exchange and its settings, the OFI fabric, init wait and cache effects, slow-node modes, the aac7 port. Current conclusions and live numbers (labeled m measured / mod modeled / a assumed) condensed from the former per-topic files; the removed text of each is unchanged in `archive:cleanup_1103/results/<OLD>.md`. Every former section keeps its `§ID` heading.

## XSHMEM: target-shaped communication (Cray SHMEM 12.1 style) over libfabric on aac7, plan TARGET1, 2026-10-21

Scope: the xshmem layer (API, backends, boot and topology, tests), what Cray SHMEM 11.8 and the cxi provider were measured to do, the engine
`comm_xshmem.c` behind `COMM_TRANSPORT=xshmem`, its gates and first timing, and the open items for the target port. Merged from the former reports
B1-B4 and S3A_A1/A2 (the six `XSHMEM_*.md` files). Labels: m measured, mod modeled, I inferred, a assumed. All numbers are single aac7 runs unless a count is given.
The sections PPG (one process per GPU, S2d timing) and BSPLIT (`RNS_BATCH_SPLIT`) follow the craylane section. Settings: `ecalc/README.md` (rows `XSHMEM*`, `COMM_TRANSPORT`; `ECALC_XEX_THREAD` was removed 2026-10-25, the exchange worker always runs). Design source and trace data are kept outside the repository.

### 1. Purpose and shape

After the aac7 Cray SHMEM tuning phase (closing record in `results/SHMEM.md`) the project builds the communication the target will have:
cray-shmem 12.1 calls, GPU-memory buffers, several NICs per node. 12.1 cannot be run on aac7 (11.8 only), so the same calls are emulated with libfabric
(cxi) behind one header, `ecalc/xshmem.h`, a 1:1 mapping of the 12.1 calls the engine needs (23 calls: init, contexts, heap alloc, put_nbi, quiet,
signal_set / put_signal / signal wait and test, barrier, topology and capability queries).
- `XSHMEM=ofi` (aac7): `xshmem_ofi.c` over libfabric, `xshmem_boot.c` (PMI2 address and blob exchange), `pe_geo.c` (GPU, NUMA and NIC map).
- `XSHMEM=cray` (target): `xshmem_cray.h`, static inline forwarding to cray-shmem; no libfabric. The four places that differ in 12.1 (device-heap allocator,
  signal set, signal wait, context creation) are selector blocks (`XSHMEM_CRAY_DEFS=-DXSHMEM_CRAY_HEAP|_SIGSET|_WAIT|_TEST|_PUTSIG_SPLIT`); with the 11.8 headers it compiles and
  links with a host-heap fallback (m, aac7 login node). The engine and the tests include only `xshmem.h`, so the port changes no engine file.
- Build default (since 2026-10-25, Q103): `XSHMEM=ofi` wherever libfabric and the PMI2 header are found, else `XSHMEM=0` (no engine; `COMM_TRANSPORT=xshmem` is then a fatal at start). When this section was written the engine was opt-in and `make -n` of the default build was byte-identical to before (m).

### 2. API contract and switches (frozen in B1)

- `xshmem_heap_alloc(bytes, space)`: at most once per space (`XSHMEM_SPACE_DATA`, `_CTRL`), collective, zeroed, 2 MiB aligned. `xshmem_off(p)` is relative to the space;
  CTRL offsets carry bit 62; only offsets cross PEs. `xshmem_ctx_t` is `void *`; compare `GE` and signal `SET` only, signals are plain 8 B writes.
- Lane contract: N contexts are created right after `xshmem_init_thread`, in order, before `xshmem_heap_alloc`. `xshmem_topo_t` mirrors `pe_topo` (`pe_geo.h`).
  `xshmem_cfg_load()` is the only reader of the `XSHMEM_*` switches. `xshmem_ofi_counters` (writes, flush chains, quiets, wait spins) is ofi-only.
- Switches (all off or default-valued; rows in `ecalc/README.md`): `XSHMEM_OFI_SIG` dc (default) | flush | waw, `XSHMEM_LANES` 1, `XSHMEM_HEAP_MB` 0, `XSHMEM_MRMODE` libsma | ofi,
  `XSHMEM_CTRL` heap | host, `XSHMEM_LOCAL`, `XSHMEM_IPC` (0), `XSHMEM_PES_PER_GPU` 1, `PE_GEO_SYSFS_ROOT`, `XSHMEM_LOOPBACK_PES` 1, `XSHMEM_ROUND_MB` 0, `XSHMEM_ORDER` rot | seq,
  and `COMM_TRANSPORT=xshmem` (the only multi-node transport since Q73; `ECALC_XEX_THREAD` was removed, the worker always runs). `tools/xshmem_api_check.py` compares the two backends' API tables (23 calls, 0 problems, m).
- Backend facts (m, aac7): dc mode, putmem_nbi, ctx_quiet (counter poll, 130 s deadline), signal_set, putmem_signal_nbi (blocks inside), virtual lanes on one NIC, two heaps. flush and waw modes
  are unproven arms; `xshmem_ptr` returns NULL (on-node peers go over the NIC).
- Boot (`xshmem_boot`): `boot_init/allgather/fence/finalize` over PMI2 (plain `srun`, no `--mpi`); cross-checks the Slurm rank and size; values chunked at 900 chars. `pe_geo_init` gives
  gpu_node = local / pes_per_gpu, device, NUMA of the GPU's PCI function, the cxi NICs on that NUMA node (`COMM_OFI_NICS` overrides), `nlanes`. `t_pe_geo` (mock sysfs trees: aac7 shape 4 NUMA x 1 cxi,
  target shape 4 NUMA x 8 cxi, 1 and 4 PEs per node, 2 PEs per GPU, overrides, bad values): 79 checks, 0 failed (m).
- Live check (`tests/t_pe_geo live`, one hold): `srun -N1 --ntasks-per-node=4 --cpus-per-task=48 --cpu-bind=map_ldom:0,1,2,3 --gpus-per-node=4 --gpu-bind=none ./tests/t_pe_geo live`; expected on aac7
  per PE i: `gpu i numa i nics 1 lanes 1 cpus <48i>-<48i+47>`, the NUMA of GPU i equal to that of its NIC, no BAD marker, last line `t_pe_geo live: 4 PEs, rank 0 ok`; at -N2 the same with node 0/2 and 1/2.
  With `--gpus-per-task=1` each PE sees one device (`gpu 0`), same NUMA and NIC. Ran ok at 1 and 2 n (m).
- Loopback PEs (S3a): the backend state is `pe_t G[K]`, K = `XSHMEM_LOOPBACK_PES`; each PE has its own NIC domains, AV, endpoints, heaps, peer table and mutex; calls with a context take the PE from it,
  the others use a thread-local current PE (`XSHMEM_SELECT(l)`). PE = K*rank + l; one boot allgather of K address blobs and one of K heap blobs; the K local PEs call `heap_alloc` in order.
  K = 1 keeps the earlier call order.

### 5. Unit tests (t_xshmem, m, single runs, dc + SPLIT, N = 1)

`ecalc/tests/t_xshmem.c`, one source for both backends. Sections: `basic` (ring put 8 B..64 MiB in three arms), `exch` (the `xex_*` exchange: random alltoallv, equal alltoall, small rounds, pairs, node-local and strided
communicators), `deadlock` (poll-only regression, a hang is a FAIL), `layer` (layered exchange, one in flight), `limit` (2^22 exchange limit, negative tests), tables `lat`, `stream`, `a2a`, each bracketed by cxi telemetry.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/XSHMEM.md`.)

### 6. The engine and its gates (S3a)

`ecalc/comm_xshmem.c` (about 400 lines): the t_xshmem exchange behind `struct comm_ops`, written only against `xshmem.h`; inter-node meshes only (intra stays `comm_xgmi`, `comm_layered.c` unchanged).
- Model: one node-process hosts K PEs (K = `XSHMEM_LOOPBACK_PES`, 4), PE = K*node + d, a lane context per PE, then the CTRL heap, then the DATA heap. DATA heap = `XSHMEM_HEAP_MB` (the planner sets it from the comm_ofi pool rule).
  CTRL words per communicator id: ROFF SIG VAL0 VAL1 VSEQ0 VSEQ1 SOFF.
- Exchange: post stages non-heap buffers into the heap, publishes ROFF per peer (order `rot` or `seq`), hands over to the worker; the worker (always on, one thread per PE) does the puts, one quiet, the signals;

### 7. First timing (item 9p5, m)

3 n, 1.9323e11 digits, 17 runs (one L warm-up, then 8 ABBA pairs L/XRD), 17/17 VERIFY OK. L = default libfabric reference: 157.3 s, sd 6.8 (n 9). XRD = xshmem, `XSHMEM_ROUND_MB=128`, rot order, dc, worker on: 149.3 s, sd 1.2 (n 8).
Paired difference XRD - L: -8.5 s, 95 % CI [-14.0, -3.0], 8 of 8 pairs negative. Reference blocks lref3b and lref3c pooled: n 23, mean about 156.7 s, sd about 5.

### 8. Target port checklist

1. `make XSHMEM=cray`: compiles `xshmem_cray.h` with the 12.1 header and `-lsma`; no libfabric, no `xshmem_boot.c`. Edit only the four selector blocks of `xshmem_cray.h`; the engine is unchanged.
2. Launch: the SHMEM environment per `pe_geo` (symmetric size, NIC policy); the planner's heap rule must read the variable 12.1 actually uses (unknown).
3. Proof on aac7 before the target (P5, branch p5): `make SHMEM_CRAY=1 XSHMEM=cray` builds `t_xshmem` and the whole ecalc (engine `comm_xshmem.c` unchanged, no `xshmem_ofi.c` / `xshmem_boot.c` / `pe_geo.c`) against 11.8 with the host heap (`shmem_malloc`, registered with `hipHostRegister` by `xshmem_cray.h`; `XSHMEM_CRAY_HOSTREG=0` switches it off): measured, both build and link on the login node. One PE per process: the engine runs with K=1 (the 4 APU meshes share the PE's heap; no loopback PEs), so give `XSHMEM_HEAP_MB` the node's whole heap (4 times the per-PE value of the ofi runs). Run (aac7, 3 nodes, item `9r0_p5cray3`, 2026-10-21, measured): `t_xshmem` built `XSHMEM=cray` against libsma 11.8 (host heap, `hipHostRegister`) correctness PASS at 1/2/3 n x 4 PEs (sections basic, exch, deadlock, layer, limit); ecalc `XSHMEM=cray COMM_TRANSPORT=xshmem` at 1e9 digits on 2 and 3 n VERIFY OK, digits identical; default gates 1 and 3 n PASS; zero engine changes (K=1 fallback). Gate record (aac7, 2026-10-21, measured): 9s1_xgate3p5b (6872b50, `XSHMEM=ofi`): g1, g2x, g3x, g3 PASS at 1e9 with identical digits, smoke 1/2/3 n 0 FAIL, mnaccept `--only mn` with xshmem 5/5 PASS, 3 n 1.9323e11 VERIFY OK 159.8 s. 9s2_p5cray3c (6872b50, `XSHMEM=cray`): g2x FAILED with a GPU memory access fault at the heap tail on both nodes. Cause: the registration covered only the whole pages inside the non-page-aligned DATA heap, so the tail page stayed pageable. Fix 0dd3627: register the enclosing page range. 9s3_p5cray3d (0dd3627, `XSHMEM=cray`) SUCCESS: `t_xshmem` PASS at 1/2/3 n (4/8/12 PEs, all sections); g1, g2x, g3x, g3 PASS at 1e9, VERIFY OK, digits identical (ecalc total 10.4 / 20.3 / 20.7 / 17.9 s); 3 n 1e9 VERIFY OK 20.55 s. Lesson for the target port: the DATA heap is not page-aligned, so register (or pin) the page range that encloses it, never only the whole pages inside; a missed tail page faults only when a GPU touches it, so the small tests can pass.
4. Day-one checks on the target with the same `t_xshmem` source: T1 can context l use NIC l (per-context NIC policy, counters per NIC; decides lanes against 2 PEs per GPU); T2 host `signal_wait_until` on a
   device-heap word and on a host word; T3 device-heap allocator names and `SHMEM_ENV_DISPLAY`; T4 put_signal against put + quiet + signal (16 MiB, ordered and flush counters); T5 put from a non-heap device source,
   `shmem_ptr` on a node-local PE; T6 NIC count, NIC-APU map, bandwidth with 1 and 2 lanes; T7 thread levels, progress, `shmem_ctx_quiet` cost with 4 PEs per node; T8 translation-miss counters of the device heap
   (expect below 10^3 per group; 10^5 or more means a registration problem).

### 10. Adopted as the default multi-node transport (Q73, user decision 2026-10-21)

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

### Target shape (tgtshape, 2026-10-22; measured on aac7, one run per cell)
One process per GPU (P), 2 lanes (L2), Cray backend and the 2-PEs-per-GPU fallback (Q2) all give the digits of the default layout D (1e9 bf9f4b08758b40d9, 1e10 3fc025473586ed22; 1 n 1e9/1e10, 2 n and 3 n 1e10; 3 n 1e11 VERIFY OK on both backends). XSHMEM_LANES was ignored by the engine before: now N > 1 stripes each payload put over N lane contexts (XSHMEM_LANE_MIN_KB), quieting all used lanes before the signals on lane 0; both lanes carry equal bytes; digits identical with 2 lanes (D, P) at 1/2/3 n on ofi and cray; lanes are not slower (aac7 has one NIC per PE, so no gain is expected): 3 n 1e10 total s ofi D 31.7 / DL2 33.4, P 25.2 / PL2 25.3, cray D 51.2 / DL2 52.9, P 31.6 / PL2 31.1. Cray D needs XSHMEM_HEAP_MB = 4 x the plan figure (the 4 APU threads share one heap). Cray Q2 needed the topo fix (XSHMEM_PES_PER_GPU). Details: REPORT_tgtshape (private).

### craylane (Cray stalls, NUMA placement, Cray vs libfabric; 2026-10-23, aac7, measured)
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

### PPG — one process per GPU, `ECALC_PE_PER_GPU` (TARGET1 stage S2; condensed 2026-10-24, full text `archive:results_merged_2026-10-24/XSHMEM.md`; "PPG.md S2d" citations mean the S2d timing subsection)
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

### BSPLIT — own-device split of oversized batch products under one process per GPU, `RNS_BATCH_SPLIT` (condensed 2026-10-24; full text in the archive file above)
**Change.** Switch `RNS_BATCH_SPLIT` (0 off, byte-identical default; 1 = on under P when the local tier cannot hold the product; 20..29 forced for tests): the operands are cut into a ka x kb grid of pieces that fit one device, pieces sharing a B piece go through the local tier as one batch, outputs are added into C with carries in own kernels; staging rises to 2 GiB per PE. Cost: about 4.3 GB pinned host per node (not in the fit model); the B transform is not shared across A pieces.
**Gates (m).** `t_mul 0 split` VERIFY OK; planner with the switch off: 12 outputs identical to main, with 42-case comparisons only P + split plans move; PG 3 n 1.9323e11 at cap 2^30 plans FITS with the switch (device 327.23 / 373.44 GB, node 392.65 / 420 GB), REFUSED without. Digits identical to D: 1 n 1e9, 1e10 and 3e10 (sha e050c5ae142ae995), 2 n 1e11 (sha 578f5efb0ff2b9af, 307.85 s against D 264.73 s, single runs), 3 n 1.9323e11 (sha e38f16dac94299b8: PGS 437.72 s, D 378.07 s, single runs; the P and split cost at 3 n full size is large, not a speed claim).
**Planner heap model.** The first two gate runs died on the xshmem DATA heap: under P without rounds a tree level stages the PE's whole share of the operand and the receive side at once (`xex_post`), which `binsplit_shmem_pool_need` did not model. The model now uses the largest node's share (lgamma tail of the term ranges), a margin of max(256 MiB, staged / 16) and the allgather scratch; planned heaps: 2 n 1e11 8960 MiB, 3 n 1.9323e11 10240 (modeled); measured need 6206-6416 MiB at 2 n (conservative) and 9216 at 3 n. Open: a 1.3 % excess and the 2 n receive ratio are unexplained.
**Follow-ups.** Single-plane arm, writing pieces directly into C, ABBA timing pairs (none yet).

### XWLAT: exchange-worker latency and the heap-tail test (branch xwlat, 2026-10-24, measured, aac7 2 n)

**Recommendation.** Adopt `ECALC_XEX_THREAD=1` as the default under P on path 2: a robust 6.5-7.5 s start-up saving and half the host memory, digits identical (sha 578f5efb0ff2b9af at 1e11, 1e9 gates), bs / dm neutral within noise. Adopted by the user (Q96, 2026-10-24): the worker is now the default in every layout (branch xexdef).

### XDIAG: day-one checks T1-T8 triage and diagnostic arms (branch xdiag, 2026-10-24, aac7 2 n, measured)

Items 9zz2_xdiagt (`t_xshmem`, `t_comm`; REF fba18db), 9zz3_xdiage (ecalc, 2 n 1e10; fba18db), 9zz4_xlane (REF bef6a16, main with Q96); all rc 0; ecalc runs VERIFY OK on every PE, no digit file. aac7 has libsma 11.8, one NIC per PE and no xpmem, so T1-T8 split as follows.
**Target-only list:** T1, T2, T5 on a device heap, T6 NIC-APU map and 1 vs 2 real NICs, T8 device-heap counters, the device-heap part of T3; `waw` once built.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/XSHMEM.md`.)

### COMM: start-up timeline lines (A4) and tree collectives (B1) -- branch comm, 2026-10-27, not yet measured

**A4, `ECALC_INIT_TL=1` (print only, no new switch).** `comm_xshmem.c` prints on rank 0 (`tl <s since the engine init began>  xshmem: ...`, stdout): the init split (boot + library init = PMI and fabric open, lane contexts, CTRL heap, DATA heap, workers, total), one line per communicator created (id, members, pe, seconds; the first 12 only), one line per first-call allgather handshake (SOFF; the first 12; partners; seconds), and at finalize a summary (communicators, summed and maximum creation seconds, handshakes, summed and maximum seconds). `mn.c` adds one `tl ... mn_init:` line (transport init and mesh creation seconds; layout P: the whole mn_init). Bounded: a 576-node run prints about 30 lines in total. Question it answers: where the 111 s (576 n) against 15.8 s (64 n) start-up goes: the DATA heap's registration and the fabric open are collective and scale with the PE count; communicator creation is local (O(n) calloc) and should show as milliseconds.

**B1, `XSHMEM_COLL=tree` (default `flat`, experiment).** Dissemination for `x_barrier`, `x_max` and `x_allgather_host`: round k (d = 2^k < n) sends to member (me + d) % n and waits for member (me - d) % n; ceil(log2 n) rounds (576 members: 10; 124 distributed shifts x 575 messages become x 10). Barrier and max: after the last round every member's entry is in the causal past of every member's exit (any n); max is idempotent so the overlapping windows are harmless. allgather_host: Bruck: G[j] = block of member (me - j) % n, round k puts G[0, m) to the partner's G[d, d + m), m = min(d, n - d); the receive region is the flat scratch's parity region (at most n x GBLK), blocks <= 4 KiB. Chosen over recursive doubling because it needs no non-power-of-two fix-up step and every round has exactly one send and one receive partner. Offsets (first call, lazy as before): a member publishes its scratch offset only to its ceil(log2 n) senders and reads it only from the members it puts to (flat: n - 1 each way; this was the 2e13 HANG site). Words: the flat words VAL/VSEQ[parity][sender] are reused; a pair meets in at most one round, the parity slot is rewritten two collectives later by a member that has exited the collective in between, which implies every member entered it and so finished this one (no signal word is reused before its consumer read it; back-to-back collectives on one communicator and communicators of different ids use disjoint words as before). The 900 s HANG names comm, round, the awaited member and its PE; `COMM_HANG_DETAIL=1` lists only the missing partners in tree mode. `x_destroy` works in both modes (it calls `x_barrier`). Flat code paths are unchanged (log text, words and digits identical with the switch off).

**Test.** `tests/t_vpe coll` (one process per PE, the engine in process): flat communicator and runs of 2, 3, 5, 6, 7 PEs, `XT_COLL_ITERS` (200) random back-to-back barrier / allreduce_max / allgather_host with random block sizes, all results checked, then `T_VPE_COLL_TIME` (us per call, PE 0, flat communicator). Run with `XSHMEM_COLL=flat` and `=tree`. (`tests/t_xshmem` is written only against xshmem.h and does not link the engine, so the collectives are tested here.)

### COMM2: handshake audit (T2), stress test, init audit and timeline (T3), sparse exchanges (T4a) -- branch comm2, 2026-10-27, code only, not yet built or measured

Status 2026-10-31: built and gated at 1 node (2026-10-27) and 2 nodes (§G2N); merged into main. Written without a build (no local builds); every item below is **code reading or modeled** until the aac7 commands at the end have run. Base: `cand` = remote-v4 b952f73.

**T2a, audit of the 2e13 hang (576 nodes, layout D: one process per node, 4 loopback PEs).**
- Mesh communicators are node sets: a flat handshake is n - 1 = 575 signals per process and communicator (not 2303), 575 x 576 = 331 k signals job-wide per first-call communicator. The tx queue (4096) exceeds 575 and EAGAIN is retried, so a queue overflow is not the cause by code (modeled).
- Every collective traced (`tree_level`, `tree_level_k`, `node_scan`, `mdb_cmp`, `mdb_limb`, `mdb_mod_qs`, `mn_modm1`, `mn_fold`, `mn_wrap_*`, `x1_regroup`, `grp_max`) is called on all members of its communicator under conditions that depend on global state (`Xh.n`, `Y->n`, allreduced values): no per-node branch that could make members diverge was found (not a proof). Communicator ids do not collide (base 0..3; `NA + NA*slot + d`; `NA + NA*(2*MN_MAXL+l) + d`).
- The census (planner, measured) first shows at 2e13 the DM_MN_WRAP path, division grid products with the mn transform cache, a leaf dist_db at level 1, and a level 7 with 3 groups of 192 (largest group 6 pieces vs 4). The wrap path adds many `mdb_limb` / allgather calls on communicators already used; the new first calls are the level-7 / level-8 group communicators of non-power-of-two size (192, 576), created lazily and locally.
- The two reported PEs (nodes 530 and 562, loopback PE 0) are both in node range [512, 576): the last level-6 group and the last level-7 child. Most likely cause (hypothesis): not a lost signal but a straggler: the first collective of a group communicator waits for children still working, and the fixed 900 s limit reports it as HANG. Second: a call-order divergence across overlapping communicators. Third: a flat burst (against: quiet deadline message differs, queue size, EAGAIN handled).
- Discriminating switches (all default off): `XSHMEM_HANG_S=3600` (the run completes if it was imbalance), `COMM_SEQ_CHECK=1` (every collective writes call number, kind and size; a mismatch is fatal naming both calls; PE waiting in the handshake for a member that is inside another collective is fatal too), `COMM_HANG_DETAIL=1` (names the missing members), `XSHMEM_HS_BATCH=k` (rotated order and a quiet every k signals: no common first target), `XSHMEM_COLL=tree` (12 signals per member). `ECALC_INIT_TL` handshake lines now split signal + quiet time from wait time.

**T2b, `tests/t_vpe hs`** (one process per PE; at most 8 PEs per node: `MNRUN_PPG_PPN=8 XSHMEM_PES_PER_GPU=2`): first allgather of NEW communicators for sizes 2, 4, 8 on `XT_HS_SLOTS` (12) slots and 3, 5, 6, 7 on 4 slots (at most 52 per PE), every result checked, 20 steady calls each (`XT_HS_REPS`); then `comm_xshmem_burst`: N = 64 .. 4608 signals per PE (beyond the 2303 of a 2304-PE flat handshake) unpaced and paced (`XT_HS_BATCH`, 64). Prints `T_VPE_HS_FIRST` per size and `T_VPE_HS_BURST` per N. `tests/t_vpe seqdiv` / `seqdiv2` with `COMM_SEQ_CHECK=1`: PEs 0 and 1 call different collectives; the step must end nonzero with the `COMM_SEQ_CHECK` fatal and no `passed the diverging collective` line.

### G2N — 2-node merge gate of cand3 = remote-v12 (item e8b inside e31n, 2026-10-31 04:35-05:26 ET, aac7 2 n, measured)

**Results** (m; every full run VERIFY OK; the 1.6e10 and 1e11 runs also with the expected last digits `...29680671395813714711` and `...37703810896884928977`; the 1.3264e11 series arm has no stored tail):

**Verdict.** Gate PASS: digits identical, 0 P24 flags, every injection caught; cand3 merged into main 2026-10-31. **Open issues.** The
series arm is not analyzed; the off / on single runs are not timing evidence (path 2 on 28.20 vs 26.29 s needs a paired run if it matters).

### N1: rounds sized by the active pairs, `XSHMEM_ROUND_ACTIVE=1` (branch roundsize, 2026-11-01; aac7 1-3 nodes, measured)

**Results, 2 nodes (aac7, item roundsize_a, 2026-11-01, measured, single runs; code at 852eaab, before the merge of main).**
- Correctness, all PASS: `t_vpe all` (with the new sparse cases) at `XSHMEM_ROUND_MB=0.01`: path 1 at 1 node (4 PEs, switch unset and set) and 2 nodes (8 PEs, unset and set); path 2 with `XSHMEM_LANES=2` at 2 nodes unset, 1 and 2 nodes set, and path 2 without lanes at 2 nodes set. `t_xshmem` (does not run the round code of `comm_xshmem.c`): PASS in the same arms.
- Gates, all PASS with digits identical: path 1 at 1 and 2 nodes, unset and set, and 2 nodes set with `XSHMEM_ROUND_MB=0.222609`; path 2 (`ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`) at 2 nodes unset and set, 1 node set.
- Digits, 2 nodes: 1e10 sha bb6e0f867a5f5d91 in all three arms; 1.6e10 (written 16000000000, `MN_TREE_LOGN_TEST=26`) sha 5bc4211dbd24efd1 for default (61 s), switch set (59 s) and set with `XSHMEM_ROUND_MB=0.222609` (76 s).
- Rounds per tag (maximum over the 8 PEs, 1e10, `XSHMEM_ROUND_STATS=1`):

**Results, 3 nodes (aac7, item roundsize_c, merged code 6f0febee, 2026-11-01, measured, single runs).**
- Correctness, all PASS: `t_vpe all` at 3 nodes (12 PEs) path 1 unset and set, path 2 with `XSHMEM_LANES=2` unset and set; path 1 set at 2 nodes. Gates (digits identical): path 1 at 1, 2, 3 nodes unset and set, 3 nodes set with `XSHMEM_ROUND_MB=0.222609`; path 2 at 2 nodes unset and set, 3 nodes set. 1e10 at 3 nodes: sha e7aac3a3fe898061 in all nine runs (unset, `XSHMEM_SPARSE=1` alone, switch set; with and without `XSHMEM_ROUND_MB=0.222609`); 1e10 at 2 nodes sha bb6e0f867a5f5d91 unset and set.
- Rounds per tag (maximum over the 12 PEs, 1e10, `XSHMEM_ROUND_MB=0.222609`):

**Verdict.** Correctness and digits PASS at 1, 2, 3 nodes on both paths; the round drop appears where the model says (only the shifts, only with a group of 3 or more members) and is small at 3 nodes. Adoption needs the 576-node (or 6-node D = 1 vs 5) measurement of the rounds and the partner-poll time; default stays 0.

### N4: `XSHMEM_LANES=2` on path 1, layout D (branch netmeas, 2026-11-01 19:40-20:33 ET, aac7, measured)

Gate of the plan item N4 (two NICs per APU on path 1). Build: main c86e5fcc, path 1 (`XSHMEM=ofi`), separate worktree. aac7 has one NIC per APU, so lane 2 is a virtual lane: this is a digits and no-crash gate, no gain claim. Launch line of the aac7 profile plus `XSHMEM_ROUND_STATS=1`; digits hashed (sha256 of the part files in name order) and deleted; one run per cell.

### N3: round size and the per-tag round counts (branch n3, 2026-11-01, aac7 3 nodes s1b0 s5b0 s6b0, measured; 576-node part modeled)

**Verdict (measured).** (1) The round count of every tag with bytes on all pairs (redist, result, addsh, layv) scales as 1/chunk, within 2 % per doubling for redist, result and addsh (7.7-7.9 of 8.0 over the 8-fold range); layv is the one divergence, 6.8 of 8.0, because its many small exchanges (3,928) reach the one-round-per-exchange floor in part of their pairs. (2) Tags whose pairs are smaller than the chunk sit on their floor from the smallest size: other (12), layeq (409 = its exchanges), spill (62 = its exchanges from 1.781 MiB on). So the model's "at least one round per exchange" floor is real, and which tags are on it at 576 nodes depends on the pair sizes there, which 3 nodes cannot show. (3) The N1 shift effect is constant, about -20 % of addsh, at every chunk: it removes rounds in proportion, not a fixed number, as the model's shift term assumes; no other tag moves, so the model's "N1 on (+other)" column (all sparse rounds removed) has no support. (4) Not shown: the time per round at 575 peers, the zero-pair effect at D far below n - 1, and the tag mix at 576-node pair sizes. Digits are unchanged by the round size and the switch.

### M2: time per round fitted from a chunk-size sweep (aac7 campaign M2, 2026-11-01 to 11-02 ET, main d7c7f28, path 1, measured)

**Question.** What does one exchange round cost (the model's `alpha`), as seen by fitting run time against the number of rounds while `XSHMEM_ROUND_MB` changes the count by up to 280x? N3 above had four sizes and single runs; this sweep has eight sizes and repeats.

**Method.** Path 1 (`XSHMEM=ofi`), 2 members (2 nodes, 8 PEs) and 3 members (3 nodes, 12 PEs), `MN_TREE_LOGN_TEST=26`, `XSHMEM_ROUND_STATS=1`. At 1e10 digits: `XSHMEM_ROUND_MB` 0.056, 0.111, 0.222, 0.445, 1.781, 8, 32, 128 (0.445 = the 576-node 128 MiB chunk per pair at 3 members), each with `XSHMEM_ROUND_ACTIVE` unset and `=1`, 3 repeats per size (queue items r1-r3). At 4e10 digits: 0.111, 0.445, 8, 128 with the same two settings, 2 repeats at 2 nodes and 3 at 3 nodes. 136 runs, all rc 0 and VERIFY OK. Rounds = the sum over tags of the per-tag maximum over PEs (`XSHMEM_TAGMAX` lines), the same count as N3; in the tag lines a round count is the third number after the tag name (column r[2] in `ecalc/comm_xshmem.c`). Fit: ordinary least squares of the ecalc total time (and of the wall) on that round count, separately for each member count and size; the interval is the 95 % t-interval of the slope.

| members | digits | runs | intercept a (s) | b = seconds per round (ecalc total) | R^2 | b from the wall (R^2) | rounds range |
|---|---|---|---|---|---|---|---|
| 2 | 1e10 | 48 | 19.1 | 50.7 us +-1.0 | 0.996 | 50.9 us +-1.5 (0.990) | 2.9e3 - 7.8e5 |
| 2 | 4e10 | 16 | 48.9 | 55.6 us +-2.1 | 0.996 | 56.0 us +-2.3 (0.995) | 3.7e3 - 1.6e6 |
| 3 | 1e10 | 48 | 18.8 | 38.9 us +-2.0 | 0.970 | 40.8 us +-3.2 (0.933) | 2.8e3 - 8.4e5 |
| 3 | 4e10 | 24 | 46.7 | 38.1 us +-1.3 | 0.994 | 38.3 us +-1.5 (0.992) | 3.6e3 - 1.7e6 |

### M6: paired timings of `XSHMEM_ROUND_ACTIVE` and `XSHMEM_LANES=2` (aac7 campaign M6, 2026-11-01/02 ET, main d7c7f28, path 1, measured)

ABBA order, 95 % interval on the mean of the per-pair differences (t distribution), all runs rc 0 and VERIFY OK; the numbers are the items' own `paired` lines.

| comparison | setup | pairs | mean A | mean B | B - A, 95 % interval |
|---|---|---|---|---|---|
| `XSHMEM_ROUND_ACTIVE` unset (A) -> 1 (B) | 3 nodes, 1e10, ecalc total | 8 | 21.10 s | 21.42 s | +0.33 s [-0.11, +0.77] |
| `XSHMEM_LANES` unset (A) -> 2 (B) | 2 nodes, 6.441e10, ecalc total, item 78_m6_ln2 | 6 | 67.53 s | 65.25 s | -2.28 s [-8.68, +4.11] |

ROUND_ACTIVE is not distinguishable from zero (+1.6 % at the 576-node chunk setting, interval -0.5 to +3.6 %). Its 20 % fewer addsh rounds (N1) are about 2,300 rounds, 0.09 s at the M2 cost of 38 us per round (modeled), far below the noise. The LANES=2 mean is pulled by the first pair (A 79.15 s, the first run of the item, no warm-up); without it the difference is +0.15 s [-1.58, +1.88] (5 pairs, from the items' per-run totals). aac7 has one NIC per PE, so lane 2 is virtual and a gain could only appear with two NICs per APU (the target): here it is "no cost, no gain". The first attempt (78_m6_ln) timed out in the queue and has no data.

## EXCHANGE — the all-to-all exchange in every transport

Scope: the all-to-all exchange in every transport: all-gather, layered exchange two-deep, slab pipelining, the rank gradient, gather kernels. Condensed 2026-10-19 from the per-item reports (originals: `archive:results_merged_2026-10-19/`; raw data and drivers: `archive:results_raw_2026-10-19/`). Old item IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`. Labels: m measured, mod modeled, a assumed.

### Topic summary

What this is: how data moves between ranks (APUs over xGMI, nodes over the fabric, the layered combination) and how that movement is hidden under compute, plus the gather kernels that feed the single-node product.

Current state (cross-checked with `ecalc/README.md` and `RESULTS.md`):
- Built in, no switch: all-gather and `alltoallv` in every transport (§A-comm, §C); the K-chunk slab pipeline (`DIST_CHUNKS`, now default **8**, was 4 when §A-comm was written; README row owns it); the push kernel with 64-bit remote stores and 76 blocks per peer (§C: 2^31 conv 0.1737 -> 0.1577 s, -9 %, m); the layered v-exchange two deep, `COMM_ALLTOALLV_DEPTH=2` (§X13b; RESULTS lists it as built in; xGMI link time hidden on the general map 1.4 -> 74 %, m, two real nodes included).
- Adopted by the user: `DIST_GATHER_RAW=2` (default since 2026-10-12): -3.8 s of ~73 s at 1 node (m), ~-1.0 s of leaf mdev at 2 nodes (m, below the noise of the whole run) (§K1b).
- Measured, not adopted, switch or code removed: two-plane pipeline `dist_fwd2/inv2` (3-6 % slower, §C C4); `DIST_GATHER_TILE` (-0.08 +- 0.88 s, §S50); `RNS_DIST_OVERLAP` (-0.11 +- 0.15 s of 73 s, §K6); rebalancing the result-exchange gradient (NO-GO, ~2 % mod, §A07). §P2 closes the k_gather line: the floor is the remote read (~24 ms of a 35 ms call), <= 0.6 s per run headroom.
- `DIST_TWREC` (twiddle-pack by row recurrence, §X13b): bit-identical, kernels x1.4-1.65, no end-to-end effect at size 1; later adopted (KERNELS.md owns it, see its `DIST_TWREC_G` item). `COMM_LAYER_TKERNEL` (one-kernel transposes): 662x at g = 576 for small blocks (m, kernel alone), needed at scale.
- Live conclusions: (1) one push kernel drives all three xGMI links at once, 244 GB/s per APU (m) vs 273 assumed in E9; (2) the host-visible xGMI stage is 40-100x the link time, so overlap must be counted at link level; (3) the pow2 path hides 3/4 of the xGMI link time (structural ceiling with K = 4), depth 2 gives the general map the same; (4) one inter exchange in flight does not bind while the fabric is the bottleneck (fabric idle while held <= 2 %); (5) XGMI-pipeline gains at 2^31 are small (2.5-5 %) because the exchange is HBM/CU bound against the row pass; (6) kernel-level tweaks of the gather are below what ABBA can resolve; the real lever is layout (avoid the 3/4 remote pull).
- Superseded: A-comm's "default 4 chunks" (now 8); C's push defaults of 228 blocks / 16 B (now 76 / 64-bit); X13 §5 part 2 ("pipeline the v-exchange") is done in X13b.

### X13 — xGMI and the fabric at the same time (PLAN.md §29 E9 part 1, H4), Phase 13a

Branch `p13-X` from `main` 203733b. Files: `comm_layered.c`, `comm_xgmi.c`, `t_dist.c`. Defaults unchanged: everything is behind `COMM_LAYER_STATS`, `COMM_XGMI_STATS`, `DIST_H4`, `DIST_LBENCH` (off). The 3-node slot was never available (admin CI array, then a node down).

##### 1. What was built
- `COMM_LAYER_STATS=1|2` (`comm_layered.c`; removed 2026-10-19, D56): per exchange host stamps for the xGMI stage, ready-for-fabric, fabric stage, complete. Mode 1 passive (upper bound of fabric completion), mode 2 exact (watcher thread; perturbs slightly: use 1 for timings). Report at exit: span, xGMI-busy, fabric-busy, both-busy, held (ready exchange waiting for the previous inter stage), fabric idle while held, NIC balance, node view. With `COMM_XGMI_STATS=1` also the link-active interval of each push kernel, giving **both-busy at link level**.
- `COMM_XGMI_STATS=1` (`comm_xgmi.c`; removed 2026-10-19, D56, the `comm_xgmi_h4()` bench stays): `k_push3_ts` stamps each block; per sender-peer bytes and link-active time. `comm_xgmi_h4()` benchmark with nine modes.
- Drivers: `DIST_H4=1 [DIST_H4_MB=256] ./tests/t_dist`; `DIST_LAYERED=1 DIST_LBENCH=24,26,28 [DIST_LREPS] mnrun.sh <g> ./tests/t_dist`.

##### 2. H4 — does the push saturate all three K4 links? (m, one node)
256 MiB per (sender, peer), 10 reps:

### A07 (N7 / opportunity C6): the result-exchange rank gradient in the dm / bs products

**Recommendation: NO-GO now** (gain ~2 % mod, below single-run noise sd 8-10 s and below what an ABBA resolves in < 8 rounds at 10 nodes; any fix touches the layout of C used by carries, node scans and digit conversion). Revisit if a 20+ node XSTATS run shows max/mean recv above 1.6 or the result exchange above 15 % of wall.
If wanted later: (1) weighted shares in `mdb_share` (needs a re-layout exchange between products, probably cancels the gain; not recommended); (2) better, shrink the volume: where pa = pb, sum the transform-domain products of one anti-diagonal before the inverse transform and CRT (needs a CRT bound check, `P24` limit 10^60; larger project, unevaluated idea); (3) cheap probe first: a per-round arrival timestamp in `COMM_XSTATS` for `XK_RESULT` plus one 10-node run.

### K1b: raw-once gather (S39 candidate A, O7b lever 3) -- `DIST_GATHER_RAW`

Branch k1b from main a448d73. Status now: **adopted, default 2** since 2026-10-12 (README row owns the semantics; `results/SINGLE_NODE.md §CAND`).

##### 1. Question
O7b (`archive:results/O7b.md`): `k_gather` is the largest kernel at 6.441e10 on one node (6.34 s APU0, 174 calls x 36.5 ms per APU, 15 % of GPU time, m) plus `k_bload` 1.2 s. S39 candidate A: gather each operand once without the canon, then canonicalise per prime with a local elementwise pass; O7b modeled 2.5-3.6 s. S40 (aac7) found the gather is not bound by the remote pull (remote 36.1 ms vs local 35.9 ms with the select kernel, m), which only removes S39's reason; raw-once saves whenever the gather costs much more than a coalesced canon pass, whatever bounds it. S50's tile kernel bought only 5 ms of 35 ms, so the store alone is not the bound either.

##### 2-3. Probe and implementation
Micro-test `tests/t_gather_raw_probe.c`: V0 gather + canon (product), V1 raw gather, V2/V3 canon pass plane->plane / in place, V4 pow2 index, V5 local source; shapes rows 8192 x C 65536 and 1024 x C 524288; all variants bit-identical. Stop rule: continue only if modeled saving = 174 x 36.5 ms x (1 - RATIO4) >= 1.5 s.
Implementation (`rns_dist.c`, `dist_core` only: the single-node C-form product; the multi-node tier's `k_gather_mn24_np` already reads once for all planes): `k_gather_raw<QS>`, `k_canon_pass`, `gather_raw_all()`. `=1`: A through its own plane xa[np-1], B through its cache plane when it has one (no extra memory). `=2`: additionally B without a cache plane through one extra q-limb plane (4.29 GB per APU at 2^29 points, mod) taken only from the block pool's free extents (no growth, no hipMalloc) and only if `DIST_GATHER_RAW_MARGIN_GB` (default 2) stays free; else B per prime. Digits identical by construction. `k_bload` unchanged.

### S50: k_gather coalesced write (`DIST_GATHER_TILE`) -- measured, not adopted, removed

**Do not adopt**: no measurable gain (0.0 +- 0.9 s at 1 node) and shape-dependent (tile 32 slower at rows 1024 x C 524288). Disposition: the code stays off main (archive tag `archive/s50-gather-tile`); results and drivers (`archive:drivers/s50/`) kept. A gain would need to be >= 0.5 s at 1 node to show in 3 rounds.

### P2: speed up k_gather -- stopped after a micro-probe; no switch added

- Verdict: remaining headroom 35 -> ~25 ms x 58 calls = **~0.6 s per run** (mod, upper bound; S50's tile gets ~0.3 s and measured -0.08 +- 0.88 s, `results/EXCHANGE.md §S50`); four ABBA rounds resolve ~+-1 s, so no change can be shown. No switch added; no gates (no product code changed). The real lever is layout (produce the operand already in the rank's rows or fold the pull into the multiplications' all-to-all): a design change (ALGORITHM.md), proposed for the user's priority.

(Details moved to `archive:repo_cl6_2026-10-25/results/EXCHANGE.md`, 2026-10-25.)

### K6: single-node C-tier all-to-all — overlap across primes (`RNS_DIST_OVERLAP=1`) -- measured, not adopted, setting removed

- Verdict: the switch is correct (digits identical) but there is no measurable gain: -0.11 +- 0.15 s of 73 s (0.15 %), C-tier time not reduced. **Do not adopt**; the setting was removed (RESULTS, OPPORTUNITIES §K6).

## OFI_FABRIC — comm_ofi (libfabric multi-NIC path), NIC use, the fabric limit and the exchange rate versus the NIC rate

Condensed 2026-10-19 (task 8b) from the merged per-item reports; originals in `archive:results_merged_2026-10-19/`, raw data and
drivers in `archive:results_raw_2026-10-19/`. Old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`. Labels: **m** measured, **d** derived
from measured numbers, **mod** modeled, **a** assumed. Times Eastern.

### Topic summary

- **Superseded in part (2026-10-25, Q103).** `comm_ofi.c` (`COMM_OFI`, `COMM_OFI_POOL`, `COMM_XSTATS`) was removed; the libfabric data plane is now `xshmem_ofi.c` (path 1, `README.md`). The fabric measurements below stay valid; the switch names in them refer to the removed transport, and the bullets "Why comm_ofi exists", "Adopted and current" and "Memory" describe it as it was (`COMM_OFI` no longer exists; `COMM_OFI_POOL_MB` and `COMM_OFI_NICS` remain, README).
- **Why comm_ofi exists.** Cray OpenSHMEMX binds one NIC per PE, and ecalc runs one process per node with four APU threads, so
  all of a node's exchange data went through one 200 Gb/s NIC (23.3 GB/s raw, m). One process can drive all four cxi NICs through
  libfabric (93.3 GB/s per node, m, §NIC16_experiments). `comm_ofi` does that for the data of every device exchange of
  `comm_shmem.c`; SHMEM keeps init, control words, signals and barriers.
- **Adopted and current.** `COMM_OFI` defaults to 1 wherever a cxi NIC is present (user decision 2026-10-06; `COMM_OFI=0` restores
  the SHMEM-only path exactly). Chunk 4 MiB and window 64 per NIC stay (tuning is flat, §TUNE17). Digits identical everywhere tested.
  Bench: all-to-all aggregate per node 44-48 GB/s at 4-10 nodes vs SHMEM 12-15 (m, §OFI17); full 10-node run 3532 s -> 1670 s
  (-52.7 %, m, §STD17), after which the part-file write to NFS (897.6 s) is the largest phase.
- **Memory.** Under comm_ofi staging lives in a per-device comm pool (`COMM_OFI_POOL_MB`); the planner, `mem_model.py` and `mnrun.sh`
  count it since OFIMEM (§STD17). B7 (the general map's v-slots, about 13 GB per node outside the layout) was confirmed there; since
  2026-10-12 the v-slots come from the comm pool by default (`COMM_LAYER_VSLOT_POOL`, §XEFF item 2), see ecalc/README.md.
- **Where the rate is still lost.** Layered (inter-stage) exchanges run at 78-82 % of the NIC at 4 nodes but 10-11 GB/s per APU thread
  at 10 nodes against 23.3 raw; direct exchanges (redistribution, results, spills) run near 1.9 GB/s and absorb skew (§XEFF, §C02).
  Of the items proposed in §XEFF: peer order (`rot`, now spelled `shift` in `COMM_SHMEM_PEER_ORDER`; wall-neutral, SHMEM_ITEMS),
  `COMM_XSTATS` (built), VSLOT_POOL + INTER2 (built; INTER2 default on until 2026-10-21, now 0, results/SHMEM.md); item 4 (OFI-native signalling) was shown NOT worth building (X3).
  §C02's `rr`/`winK` schedules were designed and a probe queued; the engine switch was not built.
- **Four PEs per node** (§P16) wins at 4 nodes / 10^10 (26 s vs 35-45 s, m) but loses 3.7x at 8 nodes, 2.4x at 4 x 10^10, does not
  fit memory at the target and needs per-PE plumbing; superseded by the proxy-PE work (`COMM_SHMEM_PE_PER_APU`, `archive:results_merged_2026-10-23/SHMEM_ITEMS_full.md`,
  results/SHMEM.md). The one-PE-per-APU rewrite was not built.
- **uffd.** `FI_MR_CACHE_MONITOR=userfaultfd` is the `mnrun.sh` default (S46, results/CRASHES_HANGS.md §S46); the 46-run soak (§T12)
  found 0 crashes, which is consistent with but does not prove removal.
- **Superseded/dated:** NIC16's own 2-node confirmation (its probes failed on a gres option; the `t_comm --bw` drivers of §OFI17
  replace it); §TUNE17 two-NICs-per-device has no gain on aac7 (one NIC per APU) and is correctness-only there; §P16's open items 1-9
  are history except the `MN_SELFTEST_GROW` finding (a launch of 32 or more node-processes stops at the layered self-test unless the
  switch is set; see ecalc/README.md for its current default).

The per-item sections (tables, drivers, run lists) were moved to `archive:repo_cl6_2026-10-25/results/OFI_FABRIC.md` (2026-10-25); section anchors that cited them now point to this summary.

## SHMEM — the plain OpenSHMEM transport: pool sizing, the closing record, the stall checklist

Scope: what is established about the plain OpenSHMEM transport (`COMM_TRANSPORT=shmem`, `comm_shmem.c`, with `comm_ofi.c` as its data plane), which was removed 2026-10-25 (Q103;
the transport is xshmem on both paths, `results/XSHMEM.md`; `README.md` describes the two paths): the symmetric pool and its sizing law (§P214, §V114), the Cray SHMEM phase on aac7
(closing record), and the checklist for node-wide stalls. The proxy-mode records and the stall investigation (items 37-99, 97a-97t) are in `archive:results_merged_2026-10-23/`
(`SHMEM_full.md`, `SHMEM_ITEMS_full.md`, `SHMEM_STALLS.md`); §S, §S12 and §SHM of the former SHMEM_ITEMS are in `archive:results_merged_2026-10-24/SHMEM_ITEMS.md`. Old IDs resolve
through `archive:results_merged_2026-10-24/INDEX.md`. Labels: (m) measured, (mod) modeled, (a) assumed.

### Summary (what the records establish)

- **Pool law.** Pool = 4 APU threads x (send + receive) of the largest staged exchange; exact (§P214); rounds cut the second copy (§V114: 1e10 on 2 nodes pool 8194 -> 2050 MiB, total not slower, m).
- **Stalls.** The random multi-second stalls of the node-process layout were kernel proactive compaction of its pinned 4 KiB symmetric heap, which evicts the GPU queues; a 2 MiB-page heap (`COMM_SHMEM_NP_HEAP_THP=1`, D48) gave 0 stalls in 16 runs at 3 nodes against 9 in 24 (m). The checklist for a recurrence follows.
- **Gap to libfabric.** 3 n 1.9323e11: SHMEM 157.0 against libfabric 143.0 s, all of it waiting in the layered variable-count exchange (closing record section 1); proxy mode (one helper PE per APU) never closed it and was removed (Q78); SHMEM 3.0 times comm_ofi at 6 nodes with the earlier best settings (m).
- **One exchange in flight** (`COMM_LAYER_INTER2` removed, 2026-10-21): 3 n 1.9323e11 SHMEM 149.9 -> 143.2 s, libfabric 140.2 -> 137.2 s (m, 14 runs, digits unchanged).

### If compaction stalls come back, and what the target needs (condensed from the archived stall report)

- **Node-wide?** All four APU threads of one node stop at the same instant while the heartbeat keeps ticking; if only some threads stop, look at the intra rendezvous, GPU clocks or one NIC.
- **Counters per second on the late node** (/proc/vmstat): `compact_stall` and `allocstall_*` up = direct reclaim or compaction in an allocation; `compact_stall` 0 with `compact_isolated`
  and `pgmigrate_fail` rising and millions of `compact_migrate_scanned` = the proactive pass of this report; kcompactd ticks and the fragmentation score (above 90 on a NUMA node starts a pass);
  KFD `evicted_ms` per GPU rising by seconds across the storm. `COMM_TRACE` (v2) with `COMM_TRACE_HB_MEM` records these (`tools/trace_report.py`).
- **Candidates:** a large 4 KiB shared or NIC-registered mapping (another pool, a registered host buffer without `MADV_HUGEPAGE`, a larger `SHMEM_SYMMETRIC_SIZE`, a library that remaps its heap); check the
  `COMM_SHMEM_NP_HEAP_THP=1: heap ... PMD-mapped after` log line first. Repeat on a second node set with an A/A arm; do not infer from single runs (6-node runs differ by 10 % or more).
- **Target (a, until measured there):** a hugetlb-backed symmetric heap is skipped by compaction and gives the NIC 2 MiB translations (the cxi driver sizes NIC pages from hugetlb VMAs only); on the target measure the stall rate per
  node-hour first, keep AMD's `compaction_proactiveness=20` and `compact_unevictable_allowed=1` unless an A/B shows otherwise, set the NIC count to the node's NICs, keep all-to-all on the best-effort class with adaptive
  routing, and keep ODP and ATS off. `COMM_SHMEM_HUGETLB=1` (legacy `comm_shmem.c` heap from the hugetlb pool, off by default) is not the target path: the target runs `xshmem_cray.h`, where the libsma heap on hugetlb is
  the test (`TASKS.md`, target session).

## DIAG — diagnostics for the exchange engine and the init timeline
**Question.** Which exchange tags produce the rounds (165,300 per PE at a mean 14 MB per round at 576 nodes, measured on the target, with `XSHMEM_ROUND_MB=128`), why does the `XSHMEM_ROUND_STATS=2` output cost about 109 s after the program end (measured on the target), why can `gpu_ms` be negative, and what do the mapping count and the pool peak look like?
**Change / branch.** Branch `diag` (diagnostic output only; no new switch; digits untouched). Final code commit `adca36d6`, driver run `diag6`.
- I1: at `XSHMEM_ROUND_STATS>=1` a per-tag table (exchanges, rounds, bytes received and sent, largest pair, and since `tagsec` two seconds columns: `poll_s` = ROFF + signal poll, EXCLUSIVE worker time, and `other_s` = ROFF post, staging copy, copy issue, quiets, signal post and receive copy; a second TOTAL line compares their sums with the PE totals of the global line) prints on stderr: node-process 0 summed over its PEs (its TOTAL row is compared with the existing totals) and the maximum over all PEs (`comm_xshmem.c`, `xtag_report`).
- I2a: the `=2` series goes to `xshmem_series_pe<rank>.txt`, and (Q139) the per-PE statistics of level >= 1 (final and every 5000 rounds) go to `xshmem_stats_pe<rank>.txt`, both in the working directory; stderr keeps the combined tables and one line each.
- I2b: each communicator has its own post and end event (destroyed with it); the last-ended exchange's event and stream are taken under a lock before the post's event is recorded; a pair on different streams or with a negative time is skipped.
- I2c: `mem.c` prints a second table `pool:pk/APU`, `pool:pk/all` (pool peak-live per phase, largest APU / all APUs).
- K5: `ECALC_INIT_TL=1` prints the `/proc/self/maps` line count at the end of init and of the run.
**Cause of the negative gpu_ms (analysis, not reproduced on the old binary).** One shared event pair per PE: the end event was re-recorded by the worker thread whenever any pending exchange finished, possibly after the next post had recorded its event (end later than post: negative) and while the caller read it. Tag 2 (result) exchanges overlap most.
**Method.** aac7, 2 nodes, layout D (path 1), driver `diag6`. Run: `ecalc 132640000000` with the aac7 launch line and `ECALC_INIT_TL=1`, no digit file, `XSHMEM_ROUND_STATS=0` then `=2`, output lines time-stamped to the second. Gates: `tools/gate.sh --nodes N --hold` at 1e9 digits after a driver build (`make -B`), path 1 (`XSHMEM=ofi`) and path 2 (`XSHMEM=cray`, `ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`, 2-node gate with `XSHMEM_ROUND_STATS=1`), then `tests/t_xshmem` at 1 and 2 nodes. The driver built because `gate.sh --build` stopped on two Python tests that failed on unmodified main at that time (fixed on main since). One pair per size, no ABBA.
**Results (all measured, aac7, 2026-11-01).**

| check | result |
|---|---|
| gate path 1, 1 node / 2 nodes | PASS / PASS (VERIFY OK, digits identical) |
| gate path 2, 1 node / 2 nodes (2 nodes with `ROUND_STATS=1`) | PASS / PASS |
| `t_xshmem` path 2, `XSHMEM_LANES=2`, 1 and 2 nodes | `XT_RESULT PASS` (printed by PE 0) |
| negative `gpu_ms`, 8 PEs x 1024 series entries | 0 of 8,192 (796 are -1: no valid pair); old rate 4 % / 50 % was measured on the target, not here |
| per-tag sums against the totals line | EQUAL: 16,783 exchanges, 27,107 rounds, 2,140,135 MB |
| stderr lines from comm_xshmem, `=2` vs `=0` | 22 vs 3 (both 2 nodes); files per `=2` run: 8 series + 8 statistics |
| teardown, total line to step end | `=0` 6 s, `=2` 4 s (earlier runs of the same code family: 5, 3, 3 and 5, 3, 4 s); at scale not remeasured |
| total time, one pair (`=0` vs `=2`) | 153.46 vs 127.74 s (init 71.0 vs 46.9 s: node-speed noise); earlier pairs 129.46 vs 126.64 and 127.54 vs 126.22 s; not a timing claim |
| `/proc/self/maps` lines (limit 65,530) | end of init 1,562; end of run 3,344 (rank 0, 2 nodes) |
| pool peak-live, largest APU / all 4 APUs, GB (final run `diag6`) | tree 54.1 / 216.3; bs 61.4 / 245.6; end 61.4 / 245.6 |

Per-tag table, node-process 0 (4 PEs summed), 2 nodes, 1.3264e11 digits, `XSHMEM_ROUND_MB` 128 (measured):

| tag | exchanges | rounds | rounds per exchange | recv MB | largest pair MB | MB per round |
|---|---|---|---|---|---|---|
| other | 103 | 103 | 1.0 | 101 | 1.05 | 0.98 |
| redist | 348 | 2,028 | 5.8 | 167,972 | 3,685 | 82.8 |
| result | 320 | 1,536 | 4.8 | 146,920 | 1,074 | 95.7 |
| spill | 320 | 320 | 1.0 | 66 | 0.67 | 0.21 |
| addsh | 776 | 2,988 | 3.9 | 218,624 | 2,147 | 73.2 |
| layeq | 14,916 | 20,132 | 1.35 | 1,606,452 | 268 | 79.8 |
| layv | 0 | 0 | | 0 | 0 | |
| total | 16,783 | 27,107 | 1.6 | 2,140,135 | | 79.0 |

Maximum over the 8 PEs (rounds): layeq 5,033, addsh 747, redist 507, result 384, spill 80, other 31.
**Verdict.** Diagnostics only; digits identical in every gate; stderr is a few lines per run. `layeq` (the layered inter-node equal-size exchange) is 89 % of the exchanges, 74 % of the rounds and 75 % of the bytes; `addsh` is 4.6 % of the exchanges and 11 % of the rounds. At 2 nodes the mean round is 79 MB: the round size follows the pair size (layeq pairs are 268 MB), not the 128 MB cap. The 576-node table (`XSHMEM_ROUND_STATS=1`) will show whether the 14 MB mean there is the same effect (hypothesis, not yet measured). This table is the input for the task "rounds sized by active pairs".
**Open issues.** The old negative-sample rate was not re-measured on the old binary. Teardown at 576 nodes not remeasured. The per-tag maximum over PEs uses offset 0 of the DATA heap at finalize (all exchanges complete, barriers around it). `gate.sh --build` was not used for the gates (see Method). Not run: 3-node checks.
**RESUME.** Branch `diag` on origin; the aac7 worktree of the branch and the driver folder `diag6` are left for the coordinator. Next: coordinator review and merge; remove both worktrees; run `XSHMEM_ROUND_STATS=1` at 576 nodes for the per-tag table.

### N1 step 1: per-tag round counters at the 576-node chunk size, 2 nodes (branch netmeas, 2026-11-01 19:44-20:33 ET, aac7, measured)

**Verdict.** The E10 test bed reproduces the round-count inflation but not the 575-peer handshake cost and not a shift-dominated mix. N1 (chunk by active pairs, zero-byte peers skipped) cannot be validated for gain at 2 nodes; its digits gate (1 / 2 / 6 nodes) and the round count at the E10 setting remain the checks. **Open issues.** No per-tag time; the 576-node per-tag table (`XSHMEM_ROUND_STATS=1`) is still the test of the shift hypothesis. **RESUME.** Next: N1 design/code as planned, with a 3-node arm so that zero-byte pairs exist.

### I3 — host timeline between exchanges (`ECALC_HOST_TL`, branch `hosttl`)

**What it is.** `ECALC_HOST_TL=1` (default 0) records, for the traced PE (`ECALC_HOST_TL_PE`, default 0, -1 = every PE), the wall time from the end of the last pending exchange to the next post ("host"), attributed to the phase tag of that next post, plus the wait inside the post (the caller's stream sync) and the number of posts made while another exchange was pending ("ovl"). It is header-only (`ecalc/hosttl.h`) with hooks in `ecalc/comm_xshmem.c` only; one file `xshmem_hosttl_pe<rank>.txt` per traced PE at finalize. Check printed per file: span (first post to last end) = busy + sum(host), exact. Unit test `tests/t_hosttl` (9 checks).

**Gates (measured, aac7 2026-11-01, path 1 unless noted).** 1e9 digits, switch unset and set (1 node; 2 nodes with `ECALC_HOST_TL_PE=-1`): VERIFY OK, digits identical, all PASS; path 2 (Cray SHMEM, `ECALC_PE_PER_GPU=1 XSHMEM_LANES=2`, 2 nodes) on and off: PASS. After merging main (C3, `comm_xshmem.c` untouched by main) the 1-node gate with the switch unset passed again.

**Big runs (measured; 2 nodes, 1.6e10 digits, `MN_TREE_LOGN_TEST=26`, path 1, 8 PEs, 2026-11-01 19:55-20:03 ET; single runs).** All four VERIFY OK with the same digit hash (sha256 of the digit files, first 16 hex 5bc4211dbd24efd1): off, on (`ECALC_HOST_TL=1 ECALC_HOST_TL_PE=-1`), on + `XSHMEM_ROUND_STATS=1`, off again. (A first attempt passed the digit count as `1.6e10`, which ecalc reads as 1 digit; see TASKS "Proposed".)

## INITWAIT — the seed thread's wait for the region pools, and the layered self-test line (2026-11-01)

**Question.** In the 2026-10-29 run (2e13 digits, 576 nodes, 206 s) init was 36.6 s and the seed thread waited 8.6 s for the region pools; the layered transport self-test took 4-13 s. Can the wait be shortened, and can the self-test time be seen on its own?

**S3 (print only, always on, multi-node).** Rank 0 prints `init: layered transport self-test <s> s (rank 0; part of the init figure; mn_selftest <s> s)`. The `ECALC_INIT_TL=1` line `init = ...` now has a `layered self-test` term (before, that time was hidden in `other`). Digits and behavior unchanged. Measured at 2 nodes on aac7: 0.16-0.30 s (the 4-13 s was a 576-node figure; the user's rule, keep it while under 15 s, is not touched).

**S2, why the seed thread waits (code reading).** `rns_init` maps staging, then the plane pools, then calls the hook `binsplit_seeds_begin`. The hook starts the seed thread and then the main thread builds the region pools (`binsplit_pregrow`, first half of each VMM arena synchronously, the rest in the background). The seed thread has two pinned buffers of one chunk each (8 GiB with VMM arenas); it fills both in about 2.7 s and then must wait for the pools, because a VMM range takes no CPU stores and the DMAs need the pool addresses. So the wait is the mapping time of the first arena halves, done by the main thread through the HIP runtime. The pools are created by the main thread, after the plane pools, in `rns_init`.

**Measured switch `ECALC_INIT_EARLY_POOLS=1` (default 0; measured at commit 2b4462f and not kept, removed by the coordinator's decision of 2026-11-01).** The hook ran before the plane pools (same bytes, same order inside the hook). Risk: plane pools mapped last may get small pieces near the node memory edge (N414 effect; the earlier `RNS_REGIONS_FIRST` at 1 node 1e11 digits made the run 2 x slower, archive:results_merged_2026-10-24/TESTING_VERIF.md). Not tested near the edge here. Earlier experiments with the other two ways (plane pools in a helper thread, a seed prebuffer) gave nothing (results/APUMULT.md, section INIT).

**Method.** aac7, 2026-11-01, branch initwait 161bf16, items A_1n and B_2n, aac7 launch line + `ECALC_INIT_TL=1`, no digit file, 3 runs per arm, arms interleaved off/on, all VERIFY OK. Gates: 1 node (unset, set), 2 nodes path 1 (unset, set), 2 nodes path 2 (set): all PASS, digits identical.

**Results (measured; init in s, rank 0, median of 3 with the range).**

| nodes | digits | switch unset | switch set | seed thread ends (tl s) unset / set |
|---|---|---|---|---|
| 2 | 1000000000 | 8.43 (7.95-8.97) | 7.89 (7.82-8.27) | 6.0 / 1.6 |
| 2 | 16000000000 | 13.20 (13.07-13.53) | 12.49 (12.01-12.54) | 10.6 / 7.6 |
| 1 | 1000000000 | 5.14 (4.85-5.30) | 6.17 (5.91-6.23) | 5.2 / 2.2 |
| 1 | 16000000000 | 15.02 (12.45-20.95) | 16.61 (15.80-16.68) | 16.5 / 13.6 |

Reading. At 2 nodes the switch lowers init by 0.5-0.7 s (4-5 %) and the seed thread ends 3-4 s earlier, but the seed does not bind init there. At 1 node init gets 1 s worse at 1000000000 digits (ranges do not overlap) and about 1.6 s worse at 16000000000 (ranges overlap; the unset arm is bimodal, 12.5-21 s). At 576 nodes the seed work is far larger, so the earlier seed end could matter, but it was not run at that scale; the node memory there (416 of 480 GB) is near the edge where the fragmentation risk applies.

**Conclusion.** The switch is not kept (no gain at 1 node, under 1 s at 2 nodes; fewer switches). The wait is the VMM mapping of the region pools through the HIP runtime in one process driving four GPUs; it overlaps only partly, and the real fix is per-device allocator processes (plan item S1).

**RESUME.** Branch initwait: S3 kept, switch removed. Cleanup done: /shareddata folder and aac7 worktrees removed.

## CACHE — the multi-node transform cache

Owner of the items below (condensed 2026-10-19; full originals in `archive:results_merged_2026-10-19/`, raw data and drivers in `archive:results_raw_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Labels: **(m)** measured, **(mod)** modeled, **(a)** assumed, **(f)** fitted. Times Eastern.

### Topic summary

The multi-node (mn) tier can keep an operand piece's forward transform in a "slot" across the pieces of a grid product, so a later piece that reuses that operand skips its gather and forward transform. This topic is about what that is worth and how it may use memory.

- **Per hit** (all measured on aac6 loopback, node s24-16): a hit saves one of the three transforms per prime (`ntt` 0.859 -> 0.574 s at 4 node-processes) and about 45 % of the operand's redistribution: 24-33 % of the piece (m) against 31-32 % modeled; fitted `CACHE_HIT_F = 0.96`. A partial hit (k of np primes in the slot) saves 7 % (1 prime) / 14 % (2 primes) of the piece, a full one 31 % (m); `CACHE_PART_F = 1.08` (f). A miss into a slot costs nothing measurable. Digits are identical in every run of every test.
- **The old default harmed**: `RNS_DIST_CACHE_MN=2` took 2 slots sized 2^(dist_logn_max-2) per prime whatever `POOL_LOG` (16 GiB per APU at four primes), allocated inside the first grid product (8.9-16.8 s) and invisible to the memory report, `ECALC_BUDGET_CHECK` and `mem_model.py`; at 10^10 on 2 processes it made the run +37.5 % slower (memory pressure slows the host-staged exchanges 12-19x), at 4 processes it ran out of memory, and at the target it would take 137.4 GB per node that 480 GB does not have (node 592.8 GB mod). The standing 5.1e13 time (346.6 s mod) credited that cache (+87.5 s) with no memory behind it.
- **Current state (ecalc/README.md)**: `RNS_DIST_CACHE_MN` slots are sized to the cap and budget-bounded (the `FIT` rule of §TC15, built in since 2026-10-12; at the target the budget room gives 0 hipMalloc'd slots); `RNS_DIST_CACHE_PARTIAL` is the **built-in default 1** since 2026-10-12 (was on both launch lines): per grid product the slots are taken from the block pool (inside the arena, no byte added to the node), as many primes as the pool's free extents allow, P24 products cached too, and the loop along the longer axis. Its modeled gain at the target is -14.1 s (about 5 %: 288.2 -> 274.0 s without the write at 5.1e13 with `MN_P24=2 NEWTON_DKM=1`), only about -6 s if `BS_ARENA_ROOM=0`.
- **End to end (m, aac6, 10^10 with target-like grids)**: cap-sized slots -14.4 % (1 slot) / -20.4 % (2 slots) at 2 processes, -10.8 % / -17.7 % at 4; the pool-rule partial cache -21.8 % (18-digit) and -16.3 % (P24 + DKM) at 2 processes. These are full slots on a small run; at the target the pool holds 1-2 primes.
- **Caveat that stays open**: the fits are on aac6's loopback (pieces about 90-99 % exchange); at the target the saving is mostly the local forward transform (about 80 % mod, single-node measurement `archive:results_merged_2026-10-24/GRID_GROUPS.md §G`: 1.11 -> 0.72 s at 2^31). The target rooms (tree 36.5 GB free in the arena = 2 primes, division 1 prime) are modeled, not measured.
- **Superseded**: CX15's full-slot and k-prime proposals (nothing but the budget-fitting ones was buildable; PC15 built the pool form); TC15's recommendation to assume no mn cache (434.1 s) is superseded by the PC15 pool rule (the estimate then is the 274-289 s class with P24 + DKM); the old estimates 346.6 / 353.3 s.

The per-item sections (tables, drivers, run lists) were moved to `archive:repo_cl6_2026-10-25/results/CACHE.md` (2026-10-25); section anchors that cited them now point to this summary.

## NODE_SLOWMODE — the bimodal 4-node totals (slow mode), what picks it, and the node screens (nodechk, NIC count, slow hosts)

Scope: the bimodal 4-node totals (slow mode), what picks it, and the node screens (nodechk, NIC count, slow hosts). Condensed from the former per-item files (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`); original text, including the per-run table of all 93 four-node runs, in `archive:results_merged_2026-10-19/NODE_SLOWMODE.md`; raw data and drivers in `archive:results_raw_2026-10-19/`. Labels: m = measured, mod = modeled, a = assumed.

### Topic summary

- **What this is.** Two linked questions on aac7: (1) why 4-node runs at the same size come out either fast (~181-185 s) or slow (~208-214 s) (§T05, §T09, §A13, §A14), and (2) how to screen nodes before a long run (§B04, §T10, §S49, §NIC), including the known slow host aac7-n01.
- **Slow mode, current state (matches RESULTS.md).** A per-launch effect on the inter-node all-to-all: -21 % rate (10.9 vs 13.8 GB/s per APU thread, m), every network-bound dm product 25-30 % longer, compute and init unchanged, ~29-52 % of launches slow depending on the sample, memoryless between runs, independent of arm, node, rank order, hold, time of day, MemAvailable and other jobs (a quiet cluster still gave 2 of 6, §A13). The NIC traffic lands in one of two `oxe_mcu_meas` groups (A 18-25 slow, B 28-33 fast), drawn per launch; at 4 nodes the split is clean (A 10.62 vs B 12.02 GB/s per thread, 31 launches, §A14) and at 2 nodes the groups do not differ. **The cause that picks the group is not established**: no tested setting (traffic class, NUM_NICS, NIC policy, SEP, startup connect) pins it, and detect-and-relaunch loses (mod). Nothing adopted; for timing, run ABBA and record each run's mode.
- **Separate effects.** A reciprocal-chain spike (3-89 s instead of ~2.3 s, cold host state, 29 of 93 runs) and slow init in the first runs of a fresh hold explain the outliers above 213 s; they are not the bimodality (§T05).
- **Node screens.** `tools/nodescreen.sh` (+ `nodescreen_tab.py`, `initchk.py`) is the fast pre-run screen (44-58 s at 2 nodes, lean 25 s); it excludes nodes that fail, run >3x the median init or first-upload time (and >= 2 s above it), or see fewer cxi NICs than `--want-nics` (§NIC). The 1 GB upload probe and the cold-path probe do not catch the aac7-n01 stall because the host was healthy when tested (no positive control, §S49, §T10); the adopted slow-host policy is the 3x init/upload check of `initchk.py`. Not caught by any quick screen: slow mode, minutes-in stalls, intermittent `shmem_init` segfaults.
- **Superseded or open.** §T05's "mechanism unidentified" is narrowed by §A13/§A14 to a launch-time NIC state. The "65 corrections" report (§A13 part B) is not reproducible on main (settle with an aac6 TCP 4-process rerun). Uneven `MN_GROUPS` for excluded nodes (§B04) is modeled only.

### §T05 - why 4-node run totals are bimodal (fast ~181 s / slow ~208 s)

Analysis of existing logs only (no cluster jobs), session 2026-10-11: 93 four-node runs at 6.441e10 digits per node on aac7 (same 4 hosts in every run; sources: 40 runs of hold
12471, 2 of S48, 16 of S50, 12 + 23 of S51; the full per-run table is in the archive original). All numbers m.
**Answer.** The slow mode is a per-run communication slowdown: the inter-node all-to-all runs at 10.9 instead of 13.8 GB/s per APU thread (-21 %) on all four PEs at once, and every
network-bound dm product takes 25-30 % longer (constant ratio from first to last stage). GPU compute and init are unchanged. It is a two-state switch redrawn independently per run (~29 %
of runs slow): independent of arm, node, rank, ABBA position, preceding run, time of day, hold or MemAvailable.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/NODE_SLOWMODE.md`.)

### §T09 - the bimodal 4-node totals (2026-10-11, aac7, session task t09)

Question: why do 4-node runs at 6.441e10 digits per node (2.5764e11 digits) come out at 180-186 s in one run and 207-213 s in the next, on the same hosts? Code origin/main ceb94c1 (`make -B SHMEM_CRAY=1`),
no code change; hold 12507 (4 nodes, 11:30-12:18 EDT), aac7 launch line + `COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1`, no digit output, reference files evicted before each run. 10 runs VERIFY OK, 2 aborted by
the memory guard. Logs of runs 1-2 lost (label bug, fixed in the driver); totals kept.

### §A13 - 4-node slow mode on a quiet aac7 (part A) and the DIST_LOGN_TEST 4-node "65 corrections" (part B)

2026-10-11, aac7, main 6d80ef1, production defaults, no code change; gate `--nodes 1` PASS. One 4-node hold (job 12531, 55 min). Drivers `archive:drivers/a13/`; raw NIC snapshots stayed on aac7.

**Part A: is the slow mode fabric state or set at process start? (m)** 6 back-to-back real runs (4 x 6.441e10), `FI_MR_CACHE_MONITOR=userfaultfd COMM_XSTATS=1 MN_WAIT_STATS=1`, a stand-alone
`t_comm --bw 4 250 256` probe launched like a real run between runs, NIC telemetry counters (`oxe_*`, ~1900 per NIC) and `ethtool -S` read before and after everything. Nothing else of anyone's ran.

| run | total s | init | bs | dm | all-to-all GB/s per thread | mode | OXE MCU group |
|---|---|---|---|---|---|---|---|
| r001 | 220.6 | 47.6 | 78.2 | 91.6 | 12.00 | mid | B |
| r002 | 197.6 | 35.7 | 71.0 | 86.1 | 13.10 | fast | B |
| r003 | 196.8 | 37.7 | 70.7 | 84.6 | 13.07 | fast | B |
| r004 | 220.0 | 34.9 | 76.0 | 104.0 | 10.99 | slow | A |
| r005 | 200.2 | 33.9 | 75.5 | 87.5 | 11.60 | slow | A |
| r006 | 182.0 | 32.1 | 69.4 | 77.0 | 13.68 | fast | B |

### §A14 - what picks the "slow mode" NIC group at launch? (aac7, 2026-10-11)

**Result.** At 2 nodes the MCU group (A = `oxe_mcu_meas` 18-25, B = 28-33) is drawn per launch with p(A) ~0.5 (44 A of 86 launches, m) but does not change the 2-node probe rate (64 MiB slabs: A 6.59 +- 0.42, B 6.59 +- 0.26 GB/s per thread, n = 44 / 42).
**No setting pins the group (m, `t_comm --bw 4 100 64`, ~13 s per launch):** none (22 launches: 9 A, 13 B; 6.53 / 6.65), `SHMEM_OFI_DEFAULT_TCLASS=TC_BULK_DATA` (8 A / 4 B), `TC_LOW_LATENCY` (8 / 4), `SHMEM_OFI_STARTUP_CONNECT=1` (3 / 5), `NUM_NICS=1` (4 / 4), `NIC_POLICY=ROUND-ROBIN` (4 / 4), `USE_SEP=1` (5 / 3);
`CXI_PID_BASE=64` failed to launch. Traffic-class runs A 16/24 vs 9/22 without (Fisher ~0.1, n.s.). Source reading: ecalc (Cray OpenSHMEMX 11.8, libfabric 2.3.1 CXI) sets no tclass/VNI/service; no documented knob selects the MCU; assumed: the group follows which NIC command-queue/pid resources the driver's free list hands out.
**4-node probe (queue item 40_a14_probe4, 31 launches, m):** group A 16 launches mean rate64 10.62 (8.81-11.79), group B 15 launches 12.02 (11.86-12.29); the split is clean (one A at 11.79 above the B minimum 11.86); A is 1.4 GB/s (~12 %) slower, ~52 % of launches are A. **At 4 nodes the group sets the all-to-all speed (A also has 2x fgfc stalls); at 2 nodes it does not.**
**P5 (item 80_f1_a14cfg4, 32 launches, shuffled rounds of n1 = no env, n2 = identical no env, tcll = TC_LOW_LATENCY, nn1 = NUM_NICS=1; m):** n1 5 A (10.78) / 3 B (11.97), n2 4 A (10.48) / 4 B (11.97), tcll 5 / 3 (10.10 / 12.02), nn1 4 / 4 (10.94 / 12.01); identical settings land in both groups and can differ within a round (18 A, 14 B in total); the P4 pattern was chance. Group B 11.97-12.02, group A 10.1-10.9 GB/s: A costs 1.1-1.9 GB/s (10-15 %) per thread.
**Detect-and-relaunch (mod from A13):** init 32-48 s (mean ~37) vs ~20 s lost per slow run (2 of 6), so a relaunch after init loses; a 15-20 s pre-probe on every launch saves 20 s on ~40 %: break-even at best. **Verdict:** no tested setting pins or avoids the slow group; nothing to adopt; relaunch not worthwhile.

### §NIC - node screen catches nodes with fewer NICs than the launch asks for

Done 2026-10-13, branch `nicchk`, merged to main. **Problem (m).** 2026-10-13 12:55 three SHMEM runs (`COMM_OFI=0`, `SHMEM_OFI_NUM_NICS=4`) aborted within seconds (rc 141) on one node: "invalid number of nics 4 ... There is only 1 NIC available"; later runs on the same allocation passed (21 of 24); the screen checked only times.
**Change.** `tools/nodescreen.sh` local stage prints `NS nics have=<n> want=<w>` per node (`have` = distinct `fi_info -p cxi` domains the runtime sees after the SHMEM launch's module load; `want` = `--want-nics N`, default `$SHMEM_OFI_NUM_NICS`, 0 = report only); `nodescreen_tab.py` flags `have < want` as a failed step so the node lands in EXCLUDE. An option of the tool, no new setting; README tool row updated.
**Verification (m, 4 nodes, one queue item, local stage).** want 4: `have` 4, 4, 4, 4 (`/sys/class/cxi` also 4), EXCLUDE empty, rc 0; want 8 (positive control): all four flagged `NICs 4 < 8 wanted`, rc 3. Not reproduced on a real 1-NIC node; the login node has no cxi device (count 0), so the check is meaningful only on compute nodes. Use `tools/nodescreen.sh --hold JOB --want-nics 4` before SHMEM runs that set 4 NICs.

### §BM - the 3 n bimodal run times are not a node-local compute mode (items 9zs19-9zs29 data; 9zv3_bm2d, 2026-10-24; m)

**Existing data (59 runs at 3 n 1e11, 9zs19, 9zs21-9zs25, 9zs29).** 26 of 59 runs slow (ofi 7/21, Cray 19/38); slow runs +5.3 % total on ofi
(+6.5 % bs, +5.4 % dm) and +7.6 % on Cray (+9.1 % bs, +10.6 % dm); init equal. Not time-correlated: P(slow | previous slow) = P(slow | previous
fast) = 0.46 (Fisher p = 1.0), runs test z = -0.29. All 59 runs used the same three nodes, so a node property could not be tested.

**Node-local test (9zv3_bm2d).** Four nodes without cxi NICs, one stream each, layout D 1-node runs of 3.3e10 digits (the per-node work of
the 3 n 1e11 run, no network), 271 runs, all VERIFY OK. Compute time (total - init) per node: median 21.7 / 21.9 / 21.8 / 22.5 s, sd 0.36-0.49 s,
max 23.1-23.2 s; the distribution is continuous and unimodal (19 of 271 runs more than 4 % above the pooled median, most of them on the
node that is 3 % slower in every run). **No 5-8 % slow mode without the fabric**: the 3 n effect needs the network (the per-launch NIC state
of §L1, or fabric load) or more than one node. Next test when three nodes with working NICs exist: the 3 n ABBA with per-launch NIC
counters (plan item 9zv1_bm1); until then pair every A/B run (already the rule).

## AAC7_PORT — the aac7 port (Phase 16)

Scope: the aac7 port (Phase 16): bring-up at 2 to 10 nodes, the A/Bs, the scaled headline, the fastest software stack. Each section below condenses the former `results/<ID>.md` (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; the unabridged text: `archive:results_merged_2026-10-19/`. Times Eastern; every number is **measured** unless marked modeled (mod) or assumed (a).

### Topic summary

- **Superseded in part (2026-10-25, Q103).** `comm_shmem.c` and its `COMM_SHMEM_*` switches were removed (archived in `archive:code_2026-10-25/`); the Cray SHMEM path is now `XSHMEM=cray` behind `xshmem.h` (`README.md`, path 2). The port facts below about libsma itself (host heap, THREAD_MULTIPLE, init segfaults) still hold.
- **What it is.** ecalc runs on aac7 (MI300A nodes, 4 APUs per node, 4 Cassini NICs, Slingshot-11, Cray OpenSHMEMX 11.8.0 over cxi) through `make XSHMEM=cray GMP_HOME=<gmp>` (path 2; at port time `make SHMEM_CRAY=1`, which no longer exists; the default build is path 1, libfabric); the launch wrappers are `ecalc/mnrun.sh` and `ecalc/aac7env.sh`; switch and variable rows are in `ecalc/README.md` (authoritative for defaults).
- **Port facts.** Cray SHMEM is OpenSHMEM 1.5 with THREAD_MULTIPLE, contexts and put-with-signal: the generic SHMEM branch of the removed comm_shmem.c applied unchanged (at port time). Its symmetric heap is **host memory** (no HIP in libsma), so `COMM_SHMEM_DEVHEAP=1` falls back to the HIP-registered host pool (same HBM on the APU). One NIC per PE by default; `SHMEM_OFI_NUM_NICS=4` vs 1 made no measurable difference (2 and 4 nodes). Allocations are `--exclusive -c 192`.
- **Digits.** Identical to the references at every size tried: 10^9 and 10^11 on 1 node (sha1 of the 10^11 file equals aac6's), 10^9 .. 10^11 on 2 / 4 / 8 / 10 nodes, and 8.4 x 10^11 on 10 nodes (in-run VERIFY + packed RECHECK + leading 10^11 prefix).
- **Default stack now: ROCm 7.2.4** (`AAC7_ROCM`, §V16); newest version that builds (7.12+ fail the HIP device link with a -fPIC error); 7.14.0 builds only in a later test and shows no detectable speed difference (§ROCM). Cray OpenSHMEMX 11.8.0, libfabric 2.3.1, cray-mpich 9.1.0 are already the newest on aac7. Every `rocm/*` module carries `conflict rocm`: unload `rocm/7.0.3` by exact name before loading another (silent failure otherwise).
- **Fabric rate is the wall.** Post-to-completion all-to-all per APU thread: 3.7 GB/s at 2 nodes, 2.8 at 4, 1.3-2.0 at 8, 1.3-1.6 at 10 (10^10 per node), 1.31 at 10 nodes x 8.4 x 10^10; the model (`estimate.py`) with the measured rate matches the 10-node record within -6 % / -5 %; with an assumed 25 GB/s it is 6-9x too optimistic. The compute side of the model holds.
- **Walls and noise.** Same configuration spans +-25 % job to job at 4 nodes; 8 nodes showed within-allocation drift (ranks 0-3, 2-10x slower), 10 nodes did not; a later 10-node run saw level 2 5-7x slower than 80 min earlier (shared fabric / NFS, cause unmeasured). NFS (about 0.1 GB/s per writer) puts the part-file write inside `total` on aac7 unless parts go to RAM `/tmp` or `ECALC_CHECKPOINT=0`.
- **Open / dropped.** Open: intermittent `shmem_init_thread` segfault of task 1 at 2 nodes x 10^10 (3 in 23 launches, §A16; 0 in 12 / 12 launches at 4-10 nodes); wrap launches in `timeout` and retry. Dropped: RCCL all-to-all (0.24x the push kernel), `COMM_SHMEM_SERIAL=1` (unstable), `RNS_DIST_CACHE_PARTIAL=0` (does not fit memory at 4 x 10^10 per node), FI_CXI / NIC-policy tuning (no gain >5 %), 4 PEs per node at 2 nodes (memory guard without `ECALC_PLANE_CAP`).
- **Headline limit.** The target share 9.16 x 10^10 digits per node does not fit aac7 now: each rebooted node copies a 77 GB model cache into RAM (MemAvailable about 435-445 GB against a 454 GB layout); 12 nodes are never free (10 usable). E ran at 8.4 x 10^10 per node (§D16).
- **Superseded numbers.** The 12-node modeled column and §5 "memory delta 0" for the full cache are superseded by §D16 s2; A16's 1-node 10^11 (206.7 s at ROCm 7.0.3) is superseded by the 7.2.4 figure 168.5 s quoted from C16 in §V16.

The per-item sections (tables, drivers, run lists) were moved to `archive:repo_cl6_2026-10-25/results/AAC7_PORT.md` (2026-10-25); section anchors that cited them now point to this summary.

