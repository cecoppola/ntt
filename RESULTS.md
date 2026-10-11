# Measured results (live)

Measured results of the ecalc project, newest at the end. Every number is labelled measured, modelled or assumed in its section.
Sections 1-114 (campaigns 1-4 and Phases 1-17 up to 2026-10-06) are in `archive/RESULTS_ARCHIVE_1-114.md`, moved verbatim;
the full text of section 115 onward follows the index below. "RESULTS §NN" citations in other notes refer to these section
numbers wherever the section lives. Agent reports are in `results/`.

## Index of all sections

| § | title | date | where |
|---|---|---|---|
| 1 | Where the design stands |  | archive |
| 2 | Arithmetic engine (bench/01) |  | archive |
| 3 | The NTT kernel: 1 554 → 2 325 Gbfly/s |  | archive |
| 4 | Capacity and fabric (bench/02, bench/03) |  | archive |
| 5 | CORRECTION: the fabric is not the dominant cost |  | archive |
| 6 | The full multiply (bench/08) |  | archive |
| 7 | Calibration against known implementations |  | archive |
| 8 | Next |  | archive |
| 9 | Logic units (bench/09) |  | archive |
| 10 | LDS and cross-lane units (bench/10) |  | archive |
| 11 | Interconnect (bench/11) |  | archive |
| 12 | Revised model |  | archive |
| 13 | Register-blocked inverse (bench/12) |  | archive |
| 14 | The fused multiply (bench/13) |  | archive |
| 15 | Rejected: deferred lazy reduction |  | archive |
| 16 | Where the project stands |  | archive |
| 17 | Phase 0 — environment | 2026-09-12 | archive |
| 18 | Harness | 2026-09-12 | archive |
| 19 | B1 — the paper's FP64-Barrett modmul (`bench/15_barrett_f64`) |  | archive |
| 20 | B3 — `hipHostRegister` staging (`bench/17_hostreg`), 16 GiB per APU |  | archive |
| 21 | B5 — 4-way interleaved peer gather (`bench/19_peer_gather`), 2 GiB planes |  | archive |
| 22 | B8 — allocation costs (`bench/21_alloc`), APU0 |  | archive |
| 23 | B7 — 300 s sustained at 256 GiB resident (`bench/14_sustained 300 64`) |  | archive |
| 24 | Burst clock (`bench/22_clock`) and the campaign-4 baseline |  | archive |
| 25 | B2 — the paper's tiled DIF NTT (`bench/16_ntt_tile`) |  | archive |
| 26 | B4 — kernels on pinned host staging (`bench/18_staging`), NUMA-local |  | archive |
| 27 | B6 — the CPU side (`bench/20_cpu`), 192 threads on 96 Zen4 cores |  | archive |
| 28 | B9 — component model of one mdev multiply at 2^31 points, 4 primes |  | archive |
| 29 | Phase 1b — node partition survey (`nodecheck.sh`, `results/0_node_*.txt`, `results/1b_*.txt`) |  | archive |
| 30 | Phase 1b — the short items (`results/1b_20260912/`) |  | archive |
| 31 | Phase 1b — D1: the modmul rate (`make isa`, `bench/15_barrett_f64` D1 variants) |  | archive |
| 32 | Phase 1b — D4: twiddle base strategy (`bench/16_ntt_tile`, `g_twmode`) |  | archive |
| 33 | Phase 1b — D5: the NTT bandwidth gap (`bench/16_ntt_tile`, `g_tmpl`, `g_lgl`) |  | archive |
| 34 | Phase 1b — D8: where the CPU CRT time goes (`bench/20_cpu`, D8 modes) |  | archive |
| 35 | Phase 1b — D12 / Q1: how does 10^d·P fit a 2^31-point pool? (desk) |  | archive |
| 36 | Campaign 4 summary — the paper against this node, after Phase 1b |  | archive |
| 37 | Phase 2 close-out (`results/1b_20260912/{09_p2,infcache,mfma}.log`) |  | archive |
| 38 | Phase 3 — steps 0–4: reference digits, arithmetic, NTT, multiply tiers, CRT | 2026-09-13 | archive |
| 39 | Phase 3 — steps 5–8: Newton division, binary splitting, radix conversion, verification, driver | 2026-09-13 | archive |
| 40 | Phase 4 — verification and acceptance | 2026-09-14 | archive |
| 41 | Phase 5 — experiments | 2026-09-14 | archive |
| 42 | Phase 5 task 1 — run-to-run variance at 4 × 10¹⁰ | 2026-09-14 | archive |
| 43 | Phase 5 tasks 2–3 — register-blocked b16 body and radix-4 stages | 2026-09-14 | archive |
| 44 | Phase 5 item 5 — engine 2: two 62-bit primes, 45-bit points | 2026-09-15 | archive |
| 45 | Phase 5 task 2 — past the paper: e to 5 × 10¹⁰ digits | 2026-09-15 | archive |
| 46 | Phase 5 item 4 — one prime per device vs four-step corner turn | 2026-09-15 | archive |
| 47 | Phase 5 item 1 — Shoup integer modmul in the b1 pass | 2026-09-15 | archive |
| 48 | Phase 6 — MI300A capability benchmarks | 2026-09-15 | archive |
| 49 | Phase 6 — the remaining items | 2026-09-15 | archive |
| 50 | Phase 7 WP1 — decimal limbs as a run-time switch | 2026-09-16 | archive |
| 51 | Phase 7 WP5 — the distributed four-step transform | 2026-09-16 | archive |
| 52 | WP1 attribution, item 1 — where the decimal bs time goes | 2026-09-17 | archive |
| 53 | WP1 attribution, items 2–3 — the reciprocal and the 352 GB peak | 2026-09-17 | archive |
| 54 | WP1 assessment — measured with the two experimental fixes | 2026-09-17 | archive |
| 55 | WP3 go/no-go — where the pools should live | 2026-09-17 | archive |
| 56 | WP3 — device-resident level pools and the locality-aware batch tier | 2026-09-17 | archive |
| 57 | WP8 — transform lengths 3·2ᵏ | 2026-09-17 | archive |
| 58 | WP4 — compute tuning on the final layout | 2026-09-17 | archive |
| 59 | WP5 (in progress) — the distributed transform on real APUs, and the ownership question settled | 2026-09-17 | archive |
| 60 | Design choices, as measured | 2026-09-18 | archive |
| 61 | WP7 — checkpoint and restart of the binary-splitting phase | 2026-09-18 | archive |
| 62 | Five-run variance on the final single-node code | 2026-09-18 | archive |
| 62b | Five-run variance, everything on device | 2026-09-18 | archive |
| 63 | The two pipelines, final single-node comparison | 2026-09-18 | archive |
| 64 | The top bs levels on the device tier — measured, not yet adopted | 2026-09-18 | archive |
| 65 | WP6 — the communicator and the distributed transform across nodes | 2026-09-18 | archive |
| 66 | The device product split as a grid | 2026-09-18 | archive |
| 67 | Decision: the decimal final version is the code | 2026-09-18 | archive |
| 68 | Phase 8 — overlap of disjoint work | 2026-09-18 | archive |
| 69 | Phase 8 I2 — the seeds computed during init | 2026-09-18 | archive |
| 70 | Phase 8 I3 — the decimal division entirely on the device | 2026-09-18 | archive |
| 71 | Phase 8 — the new baseline pinned, and the single-node ceiling | 2026-09-18 | archive |
| 72 | Phase 8 step 3 — init: the pinned staging sized to its use | 2026-09-18 | archive |
| 73 | Phase 8 M3 — the top levels of the tree as distributed products over node groups | 2026-09-18 | archive |
| 74 | Phase 9 — seven agents in parallel: what landed | 2026-09-19 | archive |
| 75 | Phase 10 — five agents on the §20 backlog: what landed | 2026-09-20 | archive |
| 76 | Phase 11 — six agents on the three priorities, for the 576-node target | 2026-09-20 | archive |
| 77 | Phase 12 — the complete solutions to the open design choices | 2026-09-21 | archive |
| 78 | Phase 13a — the first slice of the design-space campaign | 2026-09-22 | archive |
| 79 | Phase 13b — the conclusive design table for the 576-node target | 2026-09-23 | archive |
| 80 | Phase 13c — the chosen design as the default; the target 4.4 × 10¹³ digits | 2026-09-23 | archive |
| 81 | Phase 13d — the target's grid step, the model recalibrated, SHMEM on real nodes | 2026-09-23 | archive |
| 82 | The target changed to 4.25 × 10¹³ digits | 2026-09-23 | archive |
| 83 | Phase 14 (so far) — the apumult optimizations | 2026-09-24 | archive |
| 84 | Phase 14 — PLAN §33 Phase A (the target blockers), done | 2026-09-25 | archive |
| 85 | Phase 14 close — the nine defaults, the 10¹¹ standard and reference, V1–V3 | 2026-09-26 | archive |
| 86 | Phase 15 Batch 1 — integration and final verification | 2026-09-27 | archive |
| 87 | Phase 15 — the user's decisions applied; main = B1 | 2026-09-27 | archive |
| 88 | Phase 15 Batch 2, the 5.1 × 10¹³ target, and the transform cache | 2026-09-27 | archive |
| 89 | B2 merged into main | 2026-09-28 | archive |
| 90 | Fix on main: the correction patch on packed output | 2026-09-28 | archive |
| 91 | The target's write rate: ≈ 1 GB/s per node | 2026-09-29 | archive |
| 92 | B3: Batch 3 merged | 2026-09-29 | archive |
| 93 | int15j: the owed tests, EW and PC adopted on the launch line, `ECALC_FAST_EXIT`, the model's chunks | 2026-09-29 | archive |
| 94 | int15k: the user's decisions of 2026-10-03 — `DM_MN_LEAN` built and measured, the −15 s items modelled and tested, the clean-up, the 10¹¹ baseline | 2026-10-03 | archive |
| 95 | Phase 16 A: ecalc ported to aac7 — Cray OpenSHMEMX 11.8.0 over Slingshot-11, ROCm 7.0.3 | 2026-10-03 | archive |
| 96 | Phase 16 N1: Infinity-Cache-tiled transposes — measured, no gain, rejected | 2026-10-04 | archive |
| 97 | Phase 16 C: the model's inputs measured on aac7 — Slingshot-11 + Cray OpenSHMEMX, ROCm 7.0.3 vs 7.2.4 | 2026-10-04 | archive |
| 98 | Phase 16 R: the region-pool sizing abort at 10¹¹ on 4 nodes — `BS_POOL_RULE` | 2026-10-04 | archive |
| 99 | Phase 16 S: the Cray startup segfault — libfabric memhooks vs the HSA threads; `COMM_INIT_EARLY` | 2026-10-04 | archive |
| 100 | Phase 16 V: the fastest aac7 stack — ROCm 7.2.4 the aac7 default | 2026-10-04 | archive |
| 101 | Phase 16 P: four PEs per node on aac7 — faster at 4 nodes, does not fit the target as is | 2026-10-04 | archive |
| 102 | Phase 16 B, ACC, P follow-up: 10-node bring-up; acceptance on 7.2.4; 4 PEs/node does not scale; the ≥ 32-PE self-test stop | 2026-10-05 | archive |
| 103 | Phase 16 D / E: the 10-node headline on aac7, its step below, and the pairs | 2026-10-05 | archive |
| 104 | Corrections | 2026-10-05 | archive |
| 105 | Phase 16 RUN16: the optimized configuration at near-limit sizes on aac7, 1 node then 10 nodes | 2026-10-05 | archive |
| 106 | Phase 17: `comm_ofi`, a multi-NIC data plane for the SHMEM transport — adopted | 2026-10-06 | archive |
| 107 | Phase 17 fixes (fix1, fix2) and the t_edge SPX/CPX comparison | 2026-10-05 | archive |
| 108 | Phase 17 STD17: OFI memory accounting (OFIMEM) and the standard 10-node run with all NICs | 2026-10-06 | archive |
| 109 | EST17: the 576-node estimate re-run for `comm_ofi` | 2026-10-06 | archive |
| 110 | CAP17: the device-memory edge (373 GB/node) against the layout — login-node sweep, no digit cut required | 2026-10-06 | archive |
| 111 | TGT17: the target moves to 4.08 × 10¹³ digits — chosen to fit the device-memory edge COMFORTABLY | 2026-10-06 | archive |
| 112 | Phase 17: comm_ofi tuning and the two-NICs-per-device form | 2026-10-06 | archive |
| 113 | B7ACCT: the general map's v-exchange slots counted in the layout (2026-10-06; results/B7ACCT.md, branch `b7-vslot` | 2026-10-06 | archive |
| 114 | B7V17: the v-slot layout verified on real hardware, aac7 job 12287 | 2026-10-06 | archive |
| 115 | TGTBENCH2 and the target-size / launch-flag decisions | 2026-10-06 | this file |
| 116 | S18 runs on aac7, 2026-10-06/07 (job 12287; results/S18M.md) | 2026-10-06 | this file |
| 117 | The comm_ofi fall-off sweep on aac7, 2 → 10 nodes | 2026-10-07 | this file |
| 118 | Phase 2: crash soak, uneven group steps, the 3.71e13-share rerun — and a CXI queue collision | 2026-10-07 | this file |
| 119 | S19B: profile of the 3.71e13 share, 2 → 10 nodes, the switch A/B, and the 576 projection | 2026-10-07 | this file |
| 120 | S20: T2048 paired A/B (8 rounds) and the MN_WAIT_STATS profile at 10 nodes | 2026-10-07 | this file |
| 121 | S21: single-node A37 measurements | 2026-10-07 | this file |
| 122 | S22 / S23 / S24: T2048 sequential A/B, 10-node wait sweep, Q1 seed sensitivity, NTT sweep, 13/9/9 prototype | 2026-10-07 | this file |
| 123 | S27 / S25 / S26: COMM_XSTATS decomposition, X1 rot ABBA, share ladder, A37-Q9, soak counts | 2026-10-08 | this file |
| 124 | S30 / S31 analysis: the null cause (node 1 stalls ~20 s before the chain) and two 10-node segfaults | 2026-10-08 | this file |
| 125 | S31 / S32 / S33: X2 at 10 nodes, the stall follows the host, VMM_SAFE=2 soak | 2026-10-08 | this file |
| 126 | S27-S34 wrap-up: merges, X2 adopted on aac7, final S34 soak, host note, kit nodechk | 2026-10-09 | this file |
| 127 | S35-S45: crash-fix build, VSLOT_SHARE, kernel defaults (ADDSUB2, MAXIDX_TOP, QSEL), multi-node check, 576-node plan | 2026-10-10 | this file |

## 115. TGTBENCH2 and the target-size / launch-flag decisions (2026-10-06; results/TGTBENCH2.md)

Second round of the user's target tests, assessed item by item against the ecalc design (full detail in results/TGTBENCH2.md).
Headline confirmations: the device edge **93.36 GB/APU = 373.44 GB/node** (A6, m, target, n = 5, 0 % spread) — already adopted
by CAP17/TGT17/B7ACCT, now at n = 5; several of the harness's headline claims (the "251.5 s / 1.0 s" communication split, the
managed-memory host-spill mitigation, specific C3 kernel rates, SDMA attribution) were examined item by item and **rejected**
as applying to a stale or different configuration (the arithmetic of the 421.5 GB figure does not close against either
576 × 373.44 or the claimed 265.4 TB; see results/TGTBENCH2.md §1 for the M2/M3/V4/C3 items individually).

**Decisions the user made:**
- **Target size: 3.71 × 10¹³ digits** (the user, 2026-10-06 ≈ 19:30 EDT: "3.71 × 10¹³ is fine" — the size is not important while
  the implementation is built). This replaces TGT17's 4.08 × 10¹³ and sidesteps B7ACCT's device-edge failure at that size (§113)
  without a code change: at 3.71 × 10¹³ the device layout is 363.53 GB, 9.91 GB under the 373.44 GB edge. TGTBENCH2 itself had
  also proposed 3.76 × 10¹³ (367.82 GB, 5.62 GB under the edge, the largest layout tier ≤ 368 GB) as a faster alternative; the
  user chose the extra margin instead. See s18-target Part 1 (this branch) for the adopted constants.
- **Launch flags adopted as overridable `mnrun.sh` defaults** (the user: "use the flags that optimize performance but allow us
  to adjust to a different system later"): `FI_UNIVERSE_SIZE` (≥ 4 × ntasks, fixed multi-PE `fi_enable`'s −28 failure under the
  provider's default count) and `FI_LOG_LEVEL=warn` (cuts libfabric's log volume ~1000×) — both exported by `mnrun.sh` only
  when unset, so a site's own profile is never shadowed (s18-target Part 2). `FI_CXI_DEFAULT_CQ_SIZE` was not proposed:
  comm_ofi's own 8192 CQ size already passed at the target's 65 536–262 144.
- **Rejected / not adopted:** the harness's `hipMallocManaged` host-memory spill for the device edge (ME24, PROPOSED: reject —
  host-backed memory is 21 GB/s vs HBM's 3.2–3.8 TB/s, and 4.08 × 10¹³ already fit at 372.12 GB without it); the "251.5 s
  compute / 1.0 s communication" split (stale configuration); the C3 kernel-rate figures and SDMA attribution as stated (not
  reproduced against this design's own measurements).
- **Unknowns the user could not answer**, left for this project's own target kit to measure directly rather than carried as
  assumptions: the 28 GB unexplained device-memory gap (B7ACCT inferred ≈ 0.6 GB/APU of it), the units behind the quoted "map
  rate" (TGTBENCH2 measured **MAP_RATE 0.010 s/GB on the target itself**, used by `--fabric target-m`; TGT17's prior aac7-only
  estimate used 0.070), and the target's actual ROCm version — estimates keep **both** the 7.0.3 and 7.2.4 rows rather than
  assume one.

## 116. S18 runs on aac7, 2026-10-06/07 (job 12287; results/S18M.md)

**The 21:13 EDT segfault** (rank 3, x9000c1s3b0n0, `srun: error: ... task 3: Segmentation fault (core dumped)`, during the
first 10-node 8.1e11 LINE10 run, right after the 8-node layered self-test's rank-29..31 checks finished OK) — no backtrace
available (root-only core, no gdb on the node). Nodes came back clean after killing by PID; no other rank showed anything
abnormal. **The A/B crash test that followed (10 × 10 nodes, 1e11, 150–155 s each, all VERIFY OK) came back 0/4 crashes vs
0/4 crashes — inconclusive, flags kept as default (B).** Counting every run attempt after the segfault (A1–A4/B1–B4 = 8, the
10-node (a)/(b)/(b')/(b0)/(c) series = 5, the kit rehearsal's stages and sub-runs, pre- and post-fix = 7): **0 further
segfaults in ≈ 20 runs.** Two non-segfault failure modes did occur and are under "lessons" below (NFS ENOSPC, a driver
SIGTERM on a write timeout) — neither is a crash of the compute.

**The three 10-node 8.1e11/6.441e11-digit runs with the flags** (`FI_UNIVERSE_SIZE=4096 FI_LOG_LEVEL=warn`, `MNRUN_FI_DEFAULTS=1`):
run **(a)** 1259.28 s, VERIFY OK, written to NFS — peaks 406–424 GB vs the b3a layout's 434.44 GB/node (margins -10 to -28
GB). Run **(b)** (same config, no output-write bug yet applied) **finished computing** and was writing parts to NFS when the
driver's 1280 s hard timeout (1× expected wall, not 2×) SIGTERM'd it mid-write — **not a compute failure**; the write itself
measured ≈ 34 MB/s/node on the 95%-full NFS home (an 18 GB part in 530–543 s). With no-write adopted for all further 10-node
runs: **(b′)** `ECALC_VSLOT_BUDGET=1`, 679.16 s, VERIFY OK, vs **(b0)** (budget off) 721.90 s, VERIFY OK — both at 8.1e11;
budget peaks 428.4 vs 414.7 of 480 GB — **no measurable cost from the switch** (aac7 walls vary ±25% between jobs at
identical config, so the 6.3% gap between b′ and b0 is noise, not signal). **(c)** 6.441e11 digits (the 3.71e13/576 target's
per-node share), no write: 457.15 s, VERIFY OK; rank 0 peak 389.70 vs layout 391.49 GB (**-1.79 GB, the tightest margin seen
in this family**), ranks 1–9 ≈ -28 GB each.

**Kit rehearsal** (`target_kit.sh`, `~/s18ab3/kit`): stage **env** PASS; stage **build** PASS (rocm/7.2.4); stage **edge**
(one node, 1e10 digits) PASS, VERIFY OK, 27.32 s / 25.43 s between the two rehearsal passes — device and VMM edges both
400.0 GB (the kit's test cap), VMM map rate 0.2542 s/GB per APU-GB or 0.2039 s/GB per node-GB (both forms reported, the unit
still unsettled per TGTBENCH2 Q5). Stage **a3** (2 nodes, fabric injection) **FAILED on the first rehearsal pass** (a
config bug, not a crash) and **PASSED after the three kit bugs were fixed (commit ef22f22)**: 1 NIC/APU (`COMM_OFI_NICS=0;1;2;3`)
peaks at 25.71–25.73 GB/s/thread aggregate (23.17–26.61 at 4 MiB messages) vs 2 NICs/APU (`0,1;1,2;2,3;3,0`) at 26.35–27.45
GB/s/thread aggregate at the same sizes — a small (≈ 2–6%) gain on aac7's 1-NIC-per-APU hardware, confirming the mechanism
works for the target's real 2-per-APU form without yet showing a large win here. Stage **a4** (`MN_COMM_MARK=1`, 1e10
digits/node): n=2 total 74.37 s (tree level 1 11.52, reciprocal 12.66, division 17.93 GB/s per APU thread), n=8 total
128.06 s (tree level 1 3.03, level 2 14.79, level 3 11.00, reciprocal 9.14, division 10.83 GB/s per APU thread) — both
VERIFY OK on every node.

**Lessons:** NFS home at 95–96% full and slow — the kit driver's own rule from here on is no digit-output writes on any
10-node run (write phases exposed separately, run (a)/(c) excluded by design); the false `DIFFERS` on run (a)'s 1e11-prefix
check was `unpack_digits` hitting ENOSPC writing an ASCII copy, not a digit mismatch; two driver timeouts (run (b)'s SIGTERM,
s18ab2's SIGKILL) were both write-phase/NFS-rate issues, not compute failures; the coordinator's monitoring gap that night
only watched done-markers, not live progress — `tools/rundriver.sh` (in progress on branch `s18-w`) is meant to close that.

**Model calibration** (`mn_model.AAC7` checked against b0/b′/c's no-write walls, a new `aac7_s18` profile): results/S18M.md.

## 117. The comm_ofi fall-off sweep on aac7, 2 → 10 nodes (2026-10-07 01:43–01:47 EDT; job 12287)

`tests/t_comm --bw 4 5 256` through `mnrun.sh` (COMM_OFI=1, one NIC per APU, `COMM_SHMEM_POOL_MB=4608 COMM_OFI_POOL_MB=1280`),
3 repetitions, packed (consecutive hold nodes) vs spread (alternating chassis slots). Aggregate per node at 16 MiB slabs, the mean
of the PEs, median of 3 (**measured**):

| nodes | packed GB/s | spread GB/s |
|---|---|---|
| 2 | 25.8 | 26.0 |
| 4 | 42.2 | 38.1 |
| 6 | 39.0 | 44.0 |
| 8 | 39.0 | 45.4 |
| 10 | 39.1 | — |

**No fall-off from 4 to 10 nodes** (38–45 GB/s per node; 2 nodes have one peer), and no consistent placement effect — as OFI17's
44–48 GB/s, against SHMEM's one-NIC 19.8 → 11.7 GB/s over the same range (§106). Caveat: at ≥ 4 nodes each run then stopped with
rc 6 at the larger slabs — the 1280 MiB comm_ofi pool (the kit's 2-node a3 sizing) cannot hold a 1024 MiB block at ≥ 4 peers; the
16 MiB figures above completed before it. A future sweep should size `COMM_OFI_POOL_MB` by the node count. (A second, stray copy
of this sweep was later re-armed by a late agent into the same directory; its output is not used here — see §118.)

## 118. Phase 2: crash soak, uneven group steps, the 3.71e13-share rerun — and a CXI queue collision (2026-10-07 02:30–04:07 EDT; job 12287)

First driver built on `tools/rundriver.sh` (watchdog) with `ECALC_SEGV_TRACE=1` (both merged at c582b92; the trace was tested on the
aac7 login node: signal, faulting address, backtrace, maps lines; nothing printed with the switch off). Launch flags on
(`FI_UNIVERSE_SIZE=4096 FI_LOG_LEVEL=warn`). All **measured**.

- **Gate** (twice): t_mul, t_newton, e9 identical to the reference.
- **Crash soak, 10 nodes × 10¹¹ digits, no write:** first driver (~/s18p2) runs 1–24 and 26–28 rc 0 VERIFY OK (60–80 s); runs 25
  (03:04) and 29 (03:08) aborted at startup: libfabric cxi `Unable to allocate CMDQ, ret: -28` → `fi_enable(endpoint)` −262 → LIBSMA
  abort, on x9000c1s0b0n0 and x9000c1s1b1n0 — the two nodes where a late-returning agent was running its own comm_ofi `t_comm`
  tests in the same hold at that moment (its leftover `t_comm` is what the health check then flagged). Clean relaunch (~/s18p2b):
  **20 of 20 rc 0 VERIFY OK** (60–81 s). Tally since the one unexplained segfault of 21:13 EDT (§116): **0 crashes in 59 clean
  10-node runs** (8 A/B + 4 S18 10-node + 27 + 20 soak; the 2 contaminated runs excluded), so that segfault stays a rare, unreproduced event;
  the trace switch is ready for the next one.
- **Lesson (relevant to the target):** two comm_ofi/SHMEM processes on the same node exhaust the NIC's CXI command queues (−28,
  the same errno class as the target's original `fi_enable(-28)`, WISHLIST §0.1). One process per node, as the launch line has it,
  is required; drivers must never overlap network programs on a node.
- **Uneven group steps (10¹⁰ digits per node, no write):** 9 nodes default schedule 80 s, 9 nodes `MN_GROUPS=3,9` (general map at
  level 1) 60 s, 10 nodes `MN_GROUPS=4,10` (the cut-group branch: level 2 "3 children of 4" = 4+4+2) 80 s — **all VERIFY OK**.
  This is the first run of a non-dividing step (B7ACCT open item 4; the target's 192 → 576 is the dividing case, 3 × 192).
- **The 3.71e13 share rerun (10 nodes × 6.441e10, no write):** 440 s VERIFY OK (457.15 s in §116); rank 0 peak 389.40 GB vs the
  layout's node_with 391.49 GB (**−2.09 GB**; §116 −1.79), ranks 1–9 362.8–363.2 GB (≈ −28.4). The top node's thin margin
  repeats: it holds ≈ 3.6 % more digits than the average (DT15) and peaks in the bs phase. The layout holds on every node in
  both runs; at the target the top node's share is the same per-node size, so this is the tightest point of the 3.71e13 plan
  (still under the layout, and the layout is 9.91 GB under the 373.44 GB device edge).

## 119. S19B: profile of the 3.71e13 share, 2 → 10 nodes, the switch A/B, and the 576 projection (2026-10-07 09:52–12:11 EDT; job 12287; results/S19B.md)

**Runs.** S19A profiled 6.441e10 digits/node at 2/4/6/8/10 nodes, with a 10-node repeat and a 10-node run without stats.
S19C ran a 10-node A/B of base / `MN_T_CHUNK_MB=2048` / `DM_MN_LEAN=1` / `DIST_CHUNKS=8`: 3 rounds, fixed order, base first.
All 19 runs VERIFY OK (m). The S19 base line lacked `DM_MN_LEAN=1`, which is on the target's launch line. Analysis was
read-only.

- **Walls (m):**
  - totals: 202 / 283 / 412 / 363 s at 2 / 4 / 6 / 8 nodes; 10 nodes 416–522 s over 6 base-like runs (mean 462, sd 39);
  - init (25 s), the leaf (36 s) and dc (3 s) are flat in n;
  - the tree levels cost ≈ 30 s plus ≈ 48 s per further binary level;
  - a division over a non-power-of-two group (n = 6, 10) costs 1.4–1.5× the n = 8 one;
  - the stats have no measurable cost.
- **Exchange vs other (m):**
  - in-flight exchange per APU thread is 40 → 136–151 s from 2 to 10 nodes, an upper bound on exposed exchange; the fabric is
    busy 95 % of that span at ≈ 10.2 GB/s per APU thread (the raw comm_ofi rate, §117);
  - non-exchange time in the distributed phases is 94 → 215–250 s, the largest uninstrumented block;
  - the per-exchange rate falls from ≈ 11 GB/s at g = 2 to 5.3–6.8 at g = 8–10;
  - node fabric bytes are 2.3 → 5.7 TB per node;
  - the reciprocal's single-node chain waits ≈ 10 s per run on average (2.28 s minimum, mean 12.4 s).
- **Memory (m):**
  - it is deterministic across rounds;
  - the base top node peaks 389.1–389.6 GB vs node_with 391.49 (−1.9 to −2.4); its top APU reaches 92.3 GB driver-used;
  - **LEAN: top node 363.2 GB (−26.2), others −8.4 GB**;
  - **DC8: −7.0 GB on every node**, from the general-map v-slots halved (−6.87 mod);
  - T2048: no change.
- **576 projection (mod):**
  - one fitted effective rate, 6 GB/s per APU, reproduces 2–10 nodes within −8.6 … +3.9 % (`aac7_s18` at 4.25: +18 … +30 %);
  - at aac7-class fabric 3.71e13 on 576 nodes takes ≈ 737 s no-write (656–851 s for 5–7 GB/s), 56 % of it in the tree
    levels;
  - target standing estimate 216.5 s; ≈ 266 s with the target's rate derated by aac7's effective/raw ratio (a);
  - the v-slots are 26.8 GB per node at 576 vs 13.8 at 10 (not O(1) in g); DC8 makes them 13.5 (−13.4 GB, device margin
    9.9 → ≈ 23 GB).
- **A/B verdicts** (paired Δ vs base; noise ±12 %, and the order was confounded):
  - T2048 −91 / +49 / −139 s: promising, unproven (its two fast runs are faster than every base-like run);
  - LEAN −59 / +18 / −70 s: time-neutral; keep it in every base;
  - DC8 −40 / +2 / −86 s: time-neutral, memory win.
- **Decisions for the user (results/S19B.md §7):**
  - D1: adopt `DIST_CHUNKS=8` after a `:576` layout check;
  - D2: an ABBA retest of T2048 over ≥ 6 rounds;
  - D3: instrument the waiting-for-peers time and the reciprocal chain's wait;
  - D4: refit `aac7_s18` to 6 GB/s.

## 120. S20: T2048 paired A/B (8 rounds) and the MN_WAIT_STATS profile at 10 nodes (2026-10-07 18:13-21:13 EDT; holds 12287, 12331; results/S20.md)

Labels: measured (m), modelled (mod), assumed (a). 10 nodes, 6.441e10 digits/node, no write; 18 runs, all VERIFY OK, digits identical. A = `DM_MN_LEAN=1` + `DIST_CHUNKS=8`; B = A + `MN_T_CHUNK_MB=2048`. Round 1 ran on 12287; rounds 2-8 on 12331; no run spanned the switch.

- **D2 paired A/B (m), B - A total per round:** -92.6, +42.8, +281.3, -56.1, -34.7, -332.2, -95.3, -78.7 s.
  - mean -45.7 s, median **-67.4 s** (-15 %), sd 170.2, t = -0.76 (p about 0.47); sign 6 of 8 negative (p 0.29); Wilcoxon p about 0.25: **not significant**;
  - without the two fabric blow-ups (r3 B dm 536 s, r6 A dm 502 s): mean -52.4, t = -2.47 (df 5, p about 0.056), 5 of 6 negative;
  - A: 7 of 8 runs 451.5-478.2 s (mean 457.6); B: 6 of 8 runs 359.9-416.9 s (mean 387.4) plus 498.9 and 733.6; median phases bs -23 s, dm -41 s;
  - memory unchanged: top node 355.9-357.0 (A) vs 356.2-357.0 GB (B).
  - **Recommendation: do not adopt as a default yet** (medium confidence the typical gain is real; the model gives no mechanism; no target evidence).
- **DIST_CHUNKS=8 at 10 nodes (m):** LEAN + DC8 peak 355.9-357.0 GB vs LEAN 362.9-363.5: **-6.9 GB** (additive with DC8 alone, -7.0 vs base); -33 GB vs base. Time 457.6 s (7 runs) vs S19C LEAN 437.5 +- 18.1: neutral within noise (different node set).
- **D3 wait stats (m, node sums; per thread assumes 4 APU threads):**
  - bs wait 306 / 315 s (A / B mean; max 339.5, min 278.7 in A), dm 392 / 399 s (max 460.5, min 341.2), recip 96 / 112 s (max 102, min 91), other 8.4 / 6.5 s; barriers 0;
  - about 200 s per thread (a), which is 43 % of bs wall and 55 % of dm+recip wall (A);
  - skew bound from the node spread: 13-30 s per thread in dm, 7-15 in bs, about 0 in recip; the same ranks are high in both arms;
  - it does not reproduce S19B's 215-250 s "other" block as a comm_wait wait; most of S19B's "waiting for peers" upper bound is the exchange itself (136-151 s per thread there); the rest of "other" is unresolved;
  - **S-2 division overlap: GO on a prototype** (exposed wait about 200 s per thread against a 30 s threshold; realistic hidden share 30-60 s, assumed). A ready counter in wait_ge/wait_ne is not needed first.
- **Decisions for the user (results/S20.md §4):** T2048 default (keep off / aac7 arm / 8 more rounds / target test); S-2 prototype now or after target fabric numbers.

## 121. S21: single-node A37 measurements (2026-10-07 21:02-21:13 EDT; hold 12377 + CPX node of 12294; results/S21.md)

Labels as above. All runs rc 0, VERIFY OK, gates passed. The earlier attempts (prev1 build error, prev2 false DIFFERS from a missing tools/unpack_digits) are ignored.

- **A37-R2 seed microbench (m, CPX node):** correctness identical on 451 leaves x 4 schemes. One-thread ns/leaf dec18 vs b64k32: 36.3 k vs 34.1 k (rank 0, 10 nodes), 35.1 k vs 35.4 k (rank 5), 36.4 k vs 33.9 k (576, rank 0), 30.5 k vs 35.3 k (576, rank 288): ratio 0.86-1.07, **no CPU gain**. Projected seed (1t x nspan / 192) 4.3-5.2 s, but the all-core wall scaled to nspan is 9-15 s (dec18) and 6-8 s (b64k32): the 1-thread projection is 2-3x too optimistic. b64k32 time: combine 81-83 %, convert 11 %, accumulate 5 %. The A37-R1 gate stays open.
- **A37-Q1 init timeline (m, 1 node, 6.441e10, aac7 mapping):** seed thread ends 25.5 / 24.9 s vs last mapping 27.8 / 24.0 s (they finish together; 2.3 s before and 0.9 s after); seed thread wall 18-18.9 s of which 3.1-3.3 s waiting for the pools, so about 15 s, spans 10.7 s (A37CMP's 22 s model is high by about 7 s). Target at 7x mapping (mod, low confidence): seed ends about 16-17 s vs the 16.5 s init floor: a GPU seed saves about 0-6 s, not 7-12. **No-go recommended for A37-R1**; measure `ECALC_INIT_TL=1` once on the target first.
- **A37-R4 dc split (m):** loop 4.10 s = fetch 1.03-1.06 (26 %) + limb reversal 0.48 (12 %) + T1 digit residues 2.26 (55 %) + T2 0.32 (8 %) + writer wait 0; on-the-clock dc 0.02-0.03 s at 1 node (streamed). No ASCII formatting in the run (packed output), so **fmt18 is not worth building**.
- **A37-R6 doublings (m):** j < 34126 total about 0.02 s; last four doublings 0.4, 0.6-1.2, 1.2-1.3, 3.8 s of a 6.9-11.2 s chain; chain start-up 4.14 s in run 1 vs 0.43 s in run 2 (cause unknown). A one-APU small-j path is not supported by this single-node data; the 10-node floor (2.28 s) needs the per-doubling clocks there (A37-Q8).
- **A37-Q4 NTT at 2^31 (m):** forward 90.4 ms real, 76.7 ms NOP, so modmul 13.7 ms (15 %); passes (real) 24.0 / 20.8 / 19.4 / 25.8 ms, NOP 17.3 / 19.0 / 17.1 / 24.2. Split (mod, taking a37v1's 51 ms HBM floor): 51 HBM + 17 for the 4th pass + 9 structure + 14 modmul. a37v1: 144 = 51 + 50 + 43. The lever left is the extra memory pass (3-pass plan, about 17-20 ms, mod).

## 122. S22 / S23 / S24: T2048 sequential A/B, 10-node wait sweep, Q1 seed sensitivity, NTT sweep, 13/9/9 prototype (2026-10-07 22:09 - 2026-10-08 03:03 EDT; holds 12331, 12377, CPX of 12294; results/S22.md)

Labels as above. All three batches SUCCESS: S22 42 runs, 0 bad (all VERIFY OK, digits identical); S23 4 chains, gates passed; S24 12 runs OK. Analysis only; nothing launched on aac7.

- **D2: MN_T_CHUNK_MB=2048 (m).** Same A / B as S20 (A = LEAN + DC8, B = A + T2048; S22 adds ECALC_INIT_TL=1 to both arms; newer build 83a124bb; same hold 12331 as S20 rounds 2-8).
  - S22's 16 new rounds, B - A total: **16 of 16 negative**, mean -40.6 s (95 % CI -51.7 .. -29.5), median -33.7, sd 20.8, t = -7.8; A 436.3 s (411-516), B 395.6 s (377-420) mean; bs -15 s, dm -20...-26 s, init unchanged; B's dm sd 10 s vs A's 23 s; memory unchanged (355.9-357.0 GB).
  - Pooled with S20 (24 rounds): mean -42.3, median -35.1, t = -2.17 (p about 0.04, driver: STOP significant), 22 of 24 negative (sign p 4e-5); without S20's two blow-up rounds: -43.8 (CI -57.7 .. -29.9), t = -6.56.
  - Blow-ups (dm > 325 s): 2 of 16 in S20, 0 of 32 in S22 (one mid event, A, dm 309 s); no arm asymmetry.
  - **Recommendation: adopt on aac7 profile lines; keep the target line at the pool-law value (1024) until a target A/B** (about 15 min of target time).
- **S-2 / E1 wait sizing (m, mod).** Scaling n = 2..10 (2 reps each, with MN_WAIT_STATS + the new `ready` counter): wait per APU thread (a: /4) 59 s of 200 (30 %) at n = 2 to 239 s of 453 (53 %) at n = 10 (bs 84, division 113, reciprocal 42); in-flight transfer per thread 93 / 61 / 141 s; spread (skew bound) 14 / 26 / 28 s.
  - `ready` counts the data-arrival signals as well as peer-ready flags, so it is skew plus transfer, not skew.
  - Transfer floor: the fabric is busy 84 % of the division, so the ceiling of a perfect division + reciprocal overlap is **43 s of 453 at 10 nodes (mod)**, realistic 11-22 s (a); at 576 nodes the standing estimate (target-m, `estimate.py`) gives division + reciprocal exposed 34.2 s of 219 s, local 25.4 s: ceiling 25-34 s, realistic 6-17 s, below the 30 s threshold.
  - **S20's GO is revised to NO-GO for now**; a measured target wait run decides. Stat overhead about +3-4 % wall; the reciprocal's wait level (up to 227 s node sum) differs from S20 (96 s): unresolved.
- **A37-R1 GPU seed (m, mod).** Q1 x 6: seed thread wall 17.9 s (14.8 without the 3.1 s pool wait; spans 10.6 s); seed ends 0.55 s after the last mapping on average (-0.06...+1.08): they co-bind. BS_SEED_THREADS=96: spans +2 s, level 1 +1.1 s, total unchanged (92.4 s, sd 1.4); BS_SEED_FILL=0: +7.6 s total.
  - Target (mod): seed ends about 15.8 s against the 16.5 s init floor, a GPU seed gains 0-1 s. CPX `b_seed64` x 3: digits identical (18 of 18), 1-thread no gain, all-core 1.15-3x shorter than dec18 (b64k32 6.1-8.0 s vs 9-23 s).
  - **A37-R1: NO-GO**; one `ECALC_INIT_TL=1` run on the target settles it (build only if the seed ends later than about 18 s).
- **3-pass NTT (m).** S23 Q4 sweep L24-L31 real / NOP: L31 fwd 89.97 / 77.16 ms (modmul 14 %), inv 92.7 / 76.5; per-pass time about equal in the 4-pass range. NTT_SIZE_STATS: no whole 2^31 transform at 6.441e10 per node (largest 2^30, 3*2^29); whole >= 2^27 transforms are 2.5 s per APU at 10 nodes (0.6 % of wall), so a 17 % saving is 0.43 s (NTT3P had assumed 1.4 s); dist rows at n <= 10 are 2^16-2^17 (already 2 passes).
  - **S24 E0 (13/9/9, forward, 2^31, one APU):** 9-stage passes 32.4 / 28.5 ms (model central 22.5), b1-13 29.7 (model 30), total **90.6 ms vs today 89.9**; gate <= 80 ms: NO-GO in 3 of 3 reps; tile copy at s_lo 13 / 22 2.8-2.9 / 2.2-2.4 TB/s (gate 1.6: GO) so the cost is the 9-stage body (3.2-3.6 ms per stage vs 2.2-2.3 for b1), not HBM. Correctness of the new b1r<13> swizzle: 468 checks VERIFY OK, 13/9/9 equals the production plan at 2^22 and 2^31 (0 mismatches).
  - **3-pass NTT: stop** (the 2-pass plans at logn 20-22 would also gain at most 3 %, mod from these pass costs). Side result: NTT_R3_FUSE is 12-21 % faster per 3*2^k transform but those are 1.3 s per APU per run (about 0.2 s).
- **Anomalies:** the S21 Newton start-up 4.14 s vs 0.43 s did not recur (6 runs: 0.44-0.61 s; dm 33.4-34.3 s); S22 stats runs cost about +3-4 % wall; A's dm is clustered (215 / 230-246 s) while B's is tight.
- **Decisions for the user (results/S22.md §6):** (1) T2048 default on aac7 (adopt, target unchanged); (2) S-2 (no build, one target wait run); (3) A37-R1 (no-go, one target `ECALC_INIT_TL=1` run); (4) 3-pass NTT (stop).

## 123. S27 / S25 / S26: COMM_XSTATS decomposition, X1 rot ABBA, share ladder, A37-Q9, soak counts (2026-10-08 10:45 - 13:15 EDT; holds 12331, 12377; results/S27.md)

Labels as above. All runs rc 0, VERIFY OK, digits identical. Analysis only; nothing launched on aac7.

- **X1 `COMM_SHMEM_PEER_ORDER=rot` (m):** ABBA 4 rounds, B - A mean **+3.1 s** (A 378.6, B 381.7), CI -1.0 .. +7.2: null, STOP. Yet with XSTATS the layered rate rose 11.4 -> 13.7 GB/s and exposed layv wait fell 17.8 s per thread, so the gain is absorbed as waiting for stragglers. One XSTATS+rot run (358 s) is an unexplained outlier (check suggested).
- **XSTATS answer to XEFF section 3 (m, per thread, 10 nodes):** direct exchanges 101 s (27 % of 376 s), of which **skew (roff) 61 s (61 %)**, signal/transfer 16 s, staging/flush 24 s. Skew sits in the reciprocal's first addsh (**19.2 s on nine nodes, 1.2 s on node 1: node 1 is ~20 s late into the reciprocal, same in 3 runs**) and in result exchanges (dm 22.3, bs 12.0 s; ranks 0-1 wait twice as long as 4-6). Layered: 109.7 s at 11.4 GB/s, 85 % exposed; no slow mesh (4 APUs within 0.4 %): the mesh-0/NIC hypothesis is not supported.
- **Re-ranked (mod/a, 10 nodes):** (1) recip chain lateness: ceiling -19 s, expected -8..-17; (2) result rank gradient -5..-12; (3) X2 -3..-12 (S28 decides); (4) pipelined rounds -3..-6; (5) fuse redistributions -2..-4; rot, OFI signal, mesh rebinding: dead.
- **S25 ladder (m):** 6.441 / 7.0 / 7.64e10 per node: 410.6 / 464.4 / 528.5 s, all VERIFY OK; 63.8 / 66.3 / 69.2 s per 1e10 (marginal 96-98 s per 1e10, wall ~ share^1.5); peak rank 0 356.5 / 373.6 / 390.5 GB = **28.4 GB per 1e10 + ~173 GB fixed**. 8.1e10 skipped: needs 427.6 GB > 0.95 x 440.7 GB (the least-free of the 10 nodes; idle nodes show 512 GB).
- **A37-Q9 (m):** batch tier 21.3 s of bs 161 s at 10 nodes (scatter 2.3, ntt 10.9, crt 3.5, merge 0.3, 3.6 s gaps; levels 1-3 are 7.4 s); one node 20.6 s; mdev top two levels 12.6 s (level 25 alone 9.0 s). **A37-R5 not worth it** (overlappable 6.1 s, realistic -1.5..-3 s, <1 % of wall).
- **S26 soaks (m, 1 node):** 975 of 975 runs ok (A 470 + C 413 at 1e9 digits, B 92 at share class), 0 bad / diff / hang.
- **Recommendation:** diagnose node 1's reciprocal lateness (one `ECALC_LOG_CLOCKS` run), then replicate or overlap the chain; X2 after S28.


## 124. S30 / S31 analysis: the null cause (node 1 stalls ~20 s before the chain) and two 10-node segfaults (2026-10-08; results/S30.md)

Analysis only; nothing launched on aac7. Labels: m measured, i inferred.
- **S30 null explained (m):** node 1 spends 19.3 .. 25.0 s in the reciprocal's "chain" stage in 16 of 16 timestamped runs, nodes 0, 2..9 spend 1.0 .. 1.2 s. With NEWTON_MN_CHAIN_BCAST=1 node 1 STILL takes 20.7 .. 24.1 s without running the chain (only db_init / db_from_bi remain), and the other nine wait 19.5 .. 23.2 s in the allgather: the wait moved, it did not shrink (ABBA -2.7 s, CI -13.3 .. +7.9). The stall is on node 1 (x9000c1s0b1n0 in every run; rank and host not yet separated), probably in the first device allocation or a host stall (i); not proven.
- **Scale (m):** the one-node ladder is linear (exponent 0.97), the 10-node 1.48 is exchange / scale cost; a 20 s straggler is ~5 % of 394 s.
- **Segfaults (m):** d30_r4s2_A (task 5, x9000c1s2b1n0) and x2_r3s2_B (task 6, x9000c1s3b0n0), both at tl ~31 s, wall 42 s, 0.3-0.5 s before binsplit level 1, at the end of the VMM background mapping; 2 of 54 10-node runs today (3.7 %) vs 0 of 59 before; x9000c1s3b0n0 was also the host of the 10-06 segfault. No trace (ECALC_SEGV_TRACE was off), no core. Hypothesis (i): unsynchronised tail of dbig.c vmm_bg_map (hipStreamDestroy / hipSetDevice outside the mapper lock; db_vmm_arena_wait does not join for the parity-1 wait) overlapping level 1.
- **Next:** ECALC_SEGV_TRACE=1 on all 10-node runs; a host-vs-rank test (permute the nodelist) with print-only db_from_bi timestamps.


## 125. S31 / S32 / S33: X2 at 10 nodes, the stall follows the host, VMM_SAFE=2 soak (2026-10-08 .. 10-09; results/S31.md)

Labels: m measured, i inferred, a assumed.
- **X2 (INTER2 + VSLOT_POOL, pool prealloc fix) (m):** 14 complete ABBA rounds at 10 nodes x 6.441e10: B - A **-34.1 s** (CI -69.7 .. +1.5), without dm blow-up rounds **-31.7 s** (CI -55.7 .. -7.8, 10 of 12 rounds faster). Rank-0 peak +0.81 GB (356.60 to 357.41 GB). Exposed layered wait 93.6 to 68.6 s (i). Blow-ups: 1 per arm (r1 A, r10 B), so no evidence X2 reduces them; median dm A 199 s, B 188 s. Digits identical. 576-node effect not modelled (a).
- **Host x9000c1s0b1n0 (m):** the ~20-25 s db_from_bi stall follows the host in permuted runs (2 of 2 moved with it, 0 with the rank). The dd eviction left the cache unchanged (Cached 113.7 GB, the largest of the ten hosts), so eviction is untested.
- **Segfault soak (m):** 5 parallel 2-node pairs, 590 runs per arm: base 0 crashes, ECALC_VMM_SAFE=2 3 crashes, wall cost +0.5 s. S33: 2 nodes base 1 of 142, VMM_SAFE=2 0 of 141, 1 node base 0 of 515. Pooled 2-node: base 1 of 732, VMM_SAFE=2 3 of 731: no benefit. The soak "rc139" column reads 0 (srun reports rc 1). All traces share one stack inside libamdhip64 under an ecalc pthread (background mapper, i). Ten-node crashes 2026-10-08: 3 of ~85 (3.5 %), 2 in base arms.
- **Decisions:** adopt X2 as an option on the aac7 line; do not adopt VMM_SAFE=2; exclude host x9000c1s0b1n0 from timed holds; add a MemFree/Cached/upload-timing check to the target kit; next diagnostic is addr2line plus a synchronous-mapping soak.


## 126. S27-S34 wrap-up: merges, X2 adopted on aac7, final S34 soak, host note, kit nodechk (2026-10-09/10; results/S34.md)

Labels: m measured, mod modelled. Main now contains branches s34 and s29 (merged --no-ff; every switch off by default, digits unchanged).
- **X2 adopted on the aac7 line** (the user, 2026-10-09): `COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1` in `ecalc/e16_headline.sh`; -31.7 s at 10 nodes (CI -55.7 .. -7.8 s, excl. dm blow-up rounds), -34.1 s (CI -69.7 .. +1.5) over all rounds (m); +0.8 GB/node on aac7 (m), +0.5 GB (mod). Target: optional T13 at the first >= 64-node step. Rejected: X1 rot (+3.1 s null), X3 DC off (0.2 % vs 15 % gate), CHAIN_BCAST (-2.7 s null), VMM_SAFE=1/2 (did not prevent segfaults).
- **Final S34 soak (m, 1e9 digits):** 2 nodes base 4 segfaults of 422 (mean 204.2 s) against `ECALC_VMM_BG=0` 0 of 420 (224.2 s, +20.0 s / +9.8 %); Fisher p = 0.063 one-sided, 0.124 two-sided: suggestive, not conclusive. 1 node 0 of 121 in both arms. The soak ended early (holds cancelled, NODE_FAIL; the "Memory required" step errors were teardown). aac7 QOS is now 6 nodes per user.
- **Host x9000c1s0b1n0 (m):** upload stall 20.7-25.2 s vs ~1 s, follows the host, 113.7 GB cache not cleared by dd. New data: MemAvailable is ~441 GB on 7 of 13 nodes and ~520 GB on the other 6, so low MemAvailable is NOT unique to it; only the stall is. Opt-in `MNRUN_EXCLUDE_HOSTS` / `MNRUN_EXCLUDE_MODE=drop` in `ecalc/mnrun.sh` and `rd_exclude_hosts` in `tools/rundriver.sh` (default off); note for the admins in internal admin note.
- **Kit:** opt-in stage `nodechk` in `ecalc/target_kit.sh` (`tests/t_nodechk.c`; meminfo and 1 GB upload per APU, flags > 3x median upload or MemAvailable < 90 % of median); TARGET_TASKS T14.


## 127. S35-S45: crash-fix build, VSLOT_SHARE, kernel defaults (ADDSUB2, MAXIDX_TOP, QSEL), multi-node check, 576-node plan (2026-10-10; results/S36.md .. S45.md; S35 is on branch s35 only)

Labels: m measured, mod modelled. All new switches digits-identical; adopted ones are defaults since 2026-10-10 (user).
- **S35 (crash fix):** `ECALC_VMM_BG=2` built, gate passed; soak paused at A 18 / C 17 runs, 0 segfaults (no evidence yet). Crash work deferred (TASKS).
- **S36 (VSLOT_SHARE):** `COMM_LAYER_VSLOT_SHARE=1` -8.4 GB per node at 4 nodes with forced DIST_GEN (m); time null. Adopted on the target and aac7 launch lines.
- **S37:** S-3 (Barrett seed division) and S-6 (HIP graphs) dropped (ceilings about 0.4 s and 0.03 s).
- **S38 (ADDSUB2):** `DBIG_ADDSUB2` -12.0..-12.5 s on 4 nodes (m); default 1.
- **S39:** kernel survey (candidates in TASKS).
- **S40/S41:** `DBIG_MAXIDX_TOP` -3.18 / -1.63 / -2.80 / -2.61 s on 4 nodes (m), default 1. `DBIG_QSEL` -2.08 / -0.93 s on aac7; aac6 CPX pooled -0.30 +- 0.19 s at 4e9 (m); default 1. k_gather is scratch-bound, not remote-bound.
- **S43:** multi-node A/B of the three defaults: -50.0 s at 4 nodes (CI -71.7..-28.3, about -18.5 %), -37.7 / -45.3 s at 2 nodes (wide CIs) (m).
- **S44:** aac6 SH5: SH5_MI300A_SPX has one device (ecalc cannot run); CPX nodes (6 XCDs, 22.9 GB each) run up to 4e9 digits; regression passes with the test knob `RNS_INIT_POOL_LOG` (unset = unchanged); gains of 0.2-0.4 s of 17-23 s, no slowdown.
- **S45 (576-node plan):** VSLOT_SHARE -5.79 GB per node, device 357.5 -> 351.7 GB of 373.44 (mod); works under `COMM_OFI=1`; the host harness `ecalc/tests/lay_host` with 576 ranks is correct. Found: an X2 cut-group pool-offset asymmetry (not a target risk).
- **Single node (m):** 6.441e10 digits in 94 s -> about 78 s with the defaults.
