# OPPORTUNITIES — every performance idea for ecalc, open or tried

Scope: every known opportunity to improve performance (time, memory, concurrency, algorithm), open or closed, with its evidence. | Maintained: updated when an idea is found, tried, adopted or rejected.

Revised 2026-10-19 (SHMEM state). Rebuilt 2026-10-12 from the archived ranked list (`archive:docs/OPPORTUNITIES.md`, 2026-10-10..12), the old task list
(`archive:TASKS_HISTORY_2026-10-12.md`), the target notes (technical ideas only), the old TASKS "Ideas" section and the
follow-ups in `results/`. Status is checked against the code after the wave 2 settings removal (`ecalc/switches.tsv`,
`results/CODE_REDUCTION.md §W2A`, `results/CODE_REDUCTION.md §W2B`). Old IDs are kept (G = robustness, C = fabric/exchange, K = per-node kernels,
M = memory/model/target levers, S-/A37- = idea sweeps); new IDs are N1, N2, ….

Labels: **m** measured, **mod** modelled, **a** assumed. Scale: **1 n** one node at 6.441 × 10¹⁰ digits, **2–6 n** aac7
at 6.441 × 10¹⁰ digits per node, **576 n** the target run (3.82 × 10¹³ digits). Effort: S < 1 day, M 1–3 days,
L > 3 days. Needs: **local** (no GPU or any one node), **aac7** (≤ 6 nodes), **target** (only the target can show it).

Where the time went before the 2026-10-12 defaults (m, `results/TIME_MODEL.md §M2a`; current scaling: `PLAN.md` section 2, libfabric): 1 / 2 / 4 / 6 nodes take 74.4 / 138.9 /
188.2 / 246.4 s; at 6 nodes the growth over 1 node is +172 s, of it division (dm) +94 s, binary splitting (bs) +60 s,
reciprocal +22.5 s, init +14 s. Target standing estimate: about 182 s without the write (mod, fabric a; `results/TIME_MODEL.md §L4`).

Target facts (user-supplied 2026-10-12, re-assessed in `results/TARGET.md §TGT`): 576 nodes × 4 MI300A (2304 APUs); 8 Slingshot NICs
per node at 400 Gb/s (2 per APU: 100 GB/s injection peak per APU); Rocky Linux 9.6, Slurm 25.11.8, ROCm 7.14.0, cray-shmem
12.1.0, CCE 21.0.0, libfabric 2.3.1; file system ≈ 1 TB/s cumulative write into one file at 32 processes per node.

## Open

| ID | area | idea | expected payoff (label, scale) | effort | needs | evidence |
|---|---|---|---|---|---|---|
| N1 | fabric/exchange | Distributed division multiply: find why dm grows +94 s from 1 to 6 nodes (wait vs volume vs skew) and cut it | the largest multi-node cost: +94 s at 6 n (m); 576 n share unknown | M | aac7 | `results/TIME_MODEL.md §M2a`; P1 `DM_MN_WRAP` (low product as low part + one cyclic plane): 2 n `total` −11.1 s [−19.1, −3.1] (m, `results/DIVISION.md §P1`); 6 n with P24 dm −6.25 s [−11.47, −1.03], p 0.032, `total` −2.86 s [−8.92, +3.19] n.s., 4 rounds; +5.4 GB per APU at the division's peak (m, `results/LAUNCH_DEFAULTS.md §DEF`); adopted part: `DM_MN_WRAP=1` is the code default since the user decision D22, 2026-10-12 (`=0` fallback); the rest of N1 (the +94 s growth) stays open |
| C7 | division | Division and reciprocal overlap (S-2): overlap the division's exchanges with its compute; gate = target wait-stats run shows > 30 s exposed wait per APU thread | 576 n ceiling 25–34 s (mod), realistic 6–17 s (a); 10-node ceiling 43 s (mod) | M | target (gate), aac7 (prototype) | `results/SESSIONS.md §S22` §2; kit stage `s2chk` |
| M7c | fabric/exchange | Two NIC endpoints per APU on the target: confirmed hardware (2 × 400 Gb/s per APU, user-supplied). `comm_ofi` already stripes over the cxi NICs of the APU's NUMA node when `COMM_OFI_NICS` is unset, so the gain is built in; the open part is to verify both NICs carry traffic and the striping reaches the peak (kit A3 one- vs two-NIC stage, not rehearsed on aac7) | the model's 100 GB/s per APU rests on it: with one effective NIC (50 GB/s) 222.7 against 185.5 s, +37.2 s at 576 n (mod, `target_l4`) | S | target | `results/TARGET.md §TGT`; `comm_ofi.c` `nic_list`; target task list T8 (private) |
| M7d | build/target | ROCm upgrades on the target: it runs ROCm 7.14.0 (user-supplied; may be upgraded later). 7.14.0 builds ecalc (`make all` + tests, `SHMEM_CRAY=1`, aac7 login node, m 2026-10-12; the `-fPIC` link failure of `results/AAC7_PORT.md §V16` no longer reproduces); its speed against 7.2.4 is not measured: 7.14.0 is installed on aac7, so a 1 n gate and ABBA there gives it; re-measure at each target upgrade | unknown sign; the size a release can move: 7.0.3 was +22 % on the local passes, +46 s at 576 n (m, mod) | S | aac7 1 n (then target per upgrade) | `results/TARGET.md §TGT`, `results/AAC7_PORT.md §V16`, `archive:docs/OPPORTUNITIES.md` M7 |
| M7a | fabric/exchange | 9-way top schedule `MN_GROUPS=…,64,576` instead of the 3·3 top `…,64,192,576` | −4.7 s at 576 n (mod, 5.276 × 10¹³ era) | S | target | `archive:docs/OPPORTUNITIES.md` M7; target task list T5 (private) |
| M7b | output | Output file striping and write waves (`MN_OUT_STRIPE`; the `MN_OUT_WAVES` switch was removed 2026-10-20) tuned to the target's parallel file system; now part of N8's write test | see N8; packed write already −6.25 % bytes (m) | S | target | `results/OUTPUT_IO.md §O4`; target task list T10 (private) |
| C3 | fabric/exchange | Run the reciprocal's small doublings (and the division's small products) on a subgroup instead of all 576 nodes, cutting the 1.10 M reciprocal messages per APU | 0 … −18.5 s at 576 n, depends on the target's per-message cost (mod); 1 n part ≈ 0.02 s (m) | M–L | target | `results/GRID_GROUPS.md §S52`, `results/SINGLE_NODE.md §S21` (A37-R6) |
| A37-R1 | init | GPU seed kernel (the CPU seed thread binds init on aac7 1 n); gate = target init timeline shows the seed ending > 18 s and > 2 s after the last mapping | 0–1 s at 576 n (mod, S22), 0–6 s (mod, S21) | L | target (gate) | `results/SESSIONS.md §S22` §3, `results/SINGLE_NODE.md §S21` |
| N2 | per-node compute | Gather layout: each rank holds its own rows so the 3/4 remote pull in `k_gather` / `k_bload` disappears (the kernel floor is the remote read) | `k_gather_raw` 2.0 s + `k_bload` 1.2 s per APU at 1 n (m); at 576 n only the per-node leaf, ≈ 1 s (mod) | L (needs scoping) | aac7 1 n | `results/EXCHANGE.md §P2`, `results/EXCHANGE.md §K1b` §6; `TASKS.md` Proposed |
| K4c | per-node compute | `k_crt24` (the P24 CRT in `p24_crt.c`, the multi-node default): the per-chunk serial carry chain as K4b's carry-lookahead (switch `P24_CRT_SCAN`, **adopted 2026-10-13, user decision D24: on by default, `=0` the fallback**). Kernel 1.55× at 2^30 points per rank; node-0 CRT time −1.24 s per 2 n run (m) | **2 n, 6.441e10/node, 4 ABBA rounds: total −1.89 s, 95 % CI [−7.51, +3.73] (n.s.); dm −0.55 s [−3.00, +1.90]; digits identical (m)** | S (done) | aac7 2 n | `results/KERNELS.md §K4c`, `results/KERNELS.md §K4b` |
| K4d | per-node compute | Four-prime CRT conversion: `ec_words_to_dec4` (16 divisions per coefficient) limits the four-prime CRT kernel; a cheaper word→decimal conversion | four-prime kernel only 1.46× with K4b (m); payoff unmeasured | M | aac7 1-2 n | `results/KERNELS.md §K4b` follow-ups |
| N4 | fabric/exchange | Slow launch mode (libfabric): the inter-node all-to-all rate is set by a fair, memoryless per-launch draw of the NIC outbound-engine bank group (A or B, ≈ 50 % each, the same bank on all NICs of the job); hard-slow (< 9.7 GB/s per thread) in 6 of 28 = 21 % of 4 n launches (m, items 9cg, 9ch). Not controllable from our side (no launch variable predicts it), so pinning is closed and detect-and-relaunch loses (mod); the SHMEM fixed layout (NIC 1, 2 on bank A, NIC 0, 3 on B) runs at ≈ 9.8 GB/s (m). Report the mode in every 4 n comparison and use interleaved arms | ≈ 20 s in ≈ 30 % of 4 n runs (mod); −12 % exchange rate in slow launches (m); 576 n unknown (the target's Slurm 25.11.8 is aac7's 25.11 series, a; whether the per-launch NIC group exists there is unknown) | M | aac7 4 n | `results/NODE_SLOWMODE.md §A13`, `results/NODE_SLOWMODE.md §A14`, `results/NODE_SLOWMODE.md §T05` |
| C9 | fabric/exchange | dm "blow-ups" (dm > 290 s in 1 of 8–16 ten-node runs) and ±12–25 % job-to-job fabric noise: diagnose the cause | 50–100 s per affected 10-node run (m); 576 n unknown | M | aac7 (if it shows at ≤ 6 n) | `results/SESSIONS.md §S31`, `results/TIME_MODEL.md §S19B` |
| N5 | division | One single-node division flow: below about 1.9 × 10¹⁰ digits size 1 takes the host flow with the full-length reciprocal, not DKM; route it through the device DKM flow (AUD3 item 1) | runs below 1.9 × 10¹⁰ get the DKM gain (DKM was −25.7 s at 10¹¹, m); 0 at 6.441 × 10¹⁰ and at 576 n | S | aac7 1 n | `results/CODE_REDUCTION.md §AUD3` item 1, `results/DIVISION.md §DKM15` |
| N6 | fabric/exchange | RCCL or MPI all-to-all across nodes as an alternative transport | worth building only if > 1.2× over `comm_ofi` at scale (a); in-node RCCL is 0.24× our push kernel (m) | L | target | target measurement list §1.8 (private) |
| M2 | model | Refit the time model to the current code with the M2a scaling points (1/2/4/6 n, stage split) so 576 n rankings rest on current data | no seconds; the model was 1 n 109.5 vs 78 s (m) before K2/K3 entered it | M | local + aac7 | `results/TIME_MODEL.md §M2a`, `results/CODE_REDUCTION.md §O8` |
| M4 | memory | Re-check the old Phase-12 memory items: `mdb_shift` exchange scratch (copies Q at g = 1, 29 GB at 6.44 × 10¹⁰), the O(share) window temporary | unknown; probably absorbed by the lean division set (built in) and T_CHUNK (a) | S | local | `archive:docs/OPPORTUNITIES.md` M4, `results/CODE_REDUCTION.md §AUD3` item 5 |
| AUD3-10 | memory | One transform cache (partial slots for `mul_grid` too) | unknown at 1 n (needs ABBA); at 576 n the full slots already get 0 slots (m) | M | aac7 1 n | `results/CODE_REDUCTION.md §AUD3` item 10, `results/CACHE.md §TC15` |
| N7 | fabric/exchange | SHMEM as the primary transport on the target (the user expects SHMEM to be needed there and possibly primary): close the proxy mode's gap to `comm_ofi`, check that OpenSHMEMX 12.1.0 (untested; aac7 has 11.8.0) runs ecalc's thread-multiple contexts and the device-memory heap, and whether it can use both NICs per PE | risk: device exchanges on SHMEM at the aac7 ratio would at least halve the fabric rate, ≥ +37 s at 576 n (mod, the 50 GB/s row); `COMM_OFI=0` raises the plan's pool from 2304 to 10240 MiB (m, planner). **Status 2026-10-19:** plain SHMEM was 3.0 × `comm_ofi` at 6 n (698.5 vs 230.5 s, m, `results/SHMEM_ITEMS.md §N7`); the proxy mode (one helper PE per APU, NUMA placement, copy kernel, huge-page heaps; N7b-N7G) is the `COMM_OFI=0` default and stable: the stalls were compaction of the pinned 4 KiB node-process heap, cured by `COMM_SHMEM_NP_HEAP_THP=1` (0 stalls in 16 runs at 3 n and 0 in 6 at 2 n, against 9 in 24; m); 2 n SHMEM = libfabric, 3 n about 6 % behind, 6 n before the fix 286.0 s mean (24 runs) vs 184.5 s libfabric, best four 220.5 s (m). Open: 6-n confirmation with the fix, the 3-6 % gap, hugetlb (N9) | M | aac7 2–6 n, target (12.1.0) | `results/SHMEM.md`, `results/SHMEM_STALLS.md`, `results/SHMEM_ITEMS.md §N7`, `results/SHMEM_ITEMS.md §N7B`, `results/SHMEM_ITEMS.md §N7G`, `results/TARGET.md §TGT`, `results/OFI_FABRIC.md §OFI17` |
| N9 | fabric/exchange | Hugetlb backing for the SHMEM node-process and helper heaps (the target has hugetlb pages; aac7 does not): removes the 4 KiB pinned pages that compaction works on and the 2.9 s per helper start-up collapse; new switch, off by default | the start-up collapse (m) and the residual GPU-queue evictions of 1.4-12 s per run (m); unknown at 6 n | M | aac7 (collapsed 4 KiB baseline), target (hugetlb itself) | `results/SHMEM_STALLS.md` |
| N8 | output | Writer design gap: the target's file system reaches ≈ 1 TB/s only with 32 writers per node into ONE file (user-supplied); ecalc writes two part files per node (1152 files) from one process with 8 pwrite threads per 256 MB chunk (`MN_OUT_THREADS=8`, O_DIRECT, `MN_OUT_DKM_HI`). Steps: (a) `tools/wbench` on the target at 64+ nodes with `-t 8` vs `-t 32` per-node files and striping (no code; `MN_OUT_THREADS=32` is a setting); (b) if per-node files fall short, one shared striped file with each node writing its byte range at a computed offset (the packed form makes offsets exact; the 4 KiB part headers move to a sidecar) | 15.9 TB packed: 15.9 s at 1 TB/s, 27.6 s at 1.0 GB/s per node, 63.7 s at 0.43 GB/s per node (8 of 32 writers, linear a); exposed write 1.0 / 12.8 / 48.8 s (mod, `target_l4`): −11.8 s against the 1.0 GB/s prior, up to −47.8 s | S (a), M (b) | target | `results/TARGET.md §TGT`, `ALGORITHM.md` §9, `mn_out.c`, `results/OUTPUT_IO.md §IO15` |

## Tried — adopted

| ID | idea | measured result | evidence |
|---|---|---|---|
| P1 (part of N1) | The division's low product X·Q as a low part + one cyclic plane (`DM_MN_WRAP=1`, the default since the user's decision D22, 2026-10-12; `=0` fallback; counted by `dm_layout` and `mem_model.py`) | 2 n `total` −11.1 s [−19.1, −3.1]; 6 n with P24 dm −6.25 s [−11.47, −1.03], `total` −2.86 s [−8.92, +3.19] n.s.; +5.4 GB per APU (m) | `results/DIVISION.md §P1`, `results/LAUNCH_DEFAULTS.md §DEF` |
| N3 | Host gaps in the batch tier: the level layout and product scans in parallel, each device's tiles back to back (`BS_BATCH_PIPE=1`, the default since the user's decision D20, 2026-10-12; `=0` fallback). Level 1 still host-bound: 2.3 s of host time against 1.3 s of GPU (m); next is node-array reuse and pre-faulting | −0.99 s at 1 n, 95 % CI [−1.18, −0.80], 4 ABBA rounds, digits identical (m); about −1 s per node at 576 n (mod) | `results/KERNELS.md §N3`, `archive:results/O7b.md` §D |
| K4b | `k_crt_batch`: the per-chunk serial carry chain as a carry-lookahead over wave ballot masks (`RNS_CRT_SCAN=1`, the default since the user's decision D21, 2026-10-12; `=0` fallback). C-form kernel 2.21× at three primes, 1.46× at four (m); follow-ups K4c, K4d | −2.52 s at 1 n, 95 % CI [−4.19, −0.85], 4 ABBA rounds, digits identical (m) | `results/KERNELS.md §K4b`, `archive:results/O7b.md` §D, `results/KERNELS.md §S39` |
| CAND (O2, K1b, K8b) | Three speed defaults 2026-10-12: `DBIG_SMALL_BOUNCE=1` (no pinning of 4 × 2 GiB on the first `db_to_bi`; fixes the 15–19 s chain stall), `DIST_GATHER_RAW=2` (one raw pull per operand), `BS_SEED_PULL=1` (seed DMA by pull kernels) | 1 n −7.16 s [−8.08, −6.23] (aac6); 2 n −5.58 s [−10.54, −0.63], 12 pairs; chain 4.57 → 0.06 s; O2 alone −30.1 s total at 4 n (m) | `results/SINGLE_NODE.md §CAND`, `results/DIVISION.md §O2`, `results/EXCHANGE.md §K1b`, `results/MEMORY.md §K8b` |
| D5 / M5 | 60-bit packed output (`ECALC_OUT_PACKED=2`) as the default 2026-10-12 | −6.25 % bytes; 10¹⁰ write 18.6 vs 27.9 s ASCII; digits identical (m) | `results/OUTPUT_IO.md §O4`, `results/OUTPUT_IO.md §C05`, `results/CODE_REDUCTION.md §W2A` |
| AUD3-6 | P24 (four primes, 24 digits per point) as the only multi-node product form, so aac7 runs the target's form; adopted as the default by the user's decision D18, 2026-10-12 (`crt.c ec_np_planes`: pool 0 at four planes at every g > 1; the `ECALC_NP_MN3=1` three-prime path was removed 2026-10-19, D56) | 2 n −8.9 s per run [−13.5, −4.3], 4 pairs, p 0.009 (dm −8.5 s, bs −2.7 s); device memory +8.6 GB per APU (+34.4 GB per node) at init (m, 1.2882e11 digits); gates 1/2/4 n and the three-prime path at 2 n in `results/LAUNCH_DEFAULTS.md §DEF` | `results/PRIMES_STRATEGY.md §MEAS-P24`, `results/LAUNCH_DEFAULTS.md §DEF`, `results/CODE_REDUCTION.md §AUD3` item 6 |
| H4 | Decimal limbs only (binary pipeline removed) 2026-10-12 | binary 1.9–2.25× slower (+32.47 s at 10¹⁰, 2 n); removal neutral +0.09 s [−2.08, +2.27] (m) | `results/CODE_REDUCTION.md §H5`, `results/CODE_REDUCTION.md §H4` |
| K2, K3 | `k_addsub2` wave-scan (ADDSUB3) and Montgomery `k_modq` (built in) | 1 n −2.03 ± 1.38 s aac7, −1.22 ± 0.84 s aac6; 4 n not significant (m) | `results/KERNELS.md §S51`, `results/LAUNCH_DEFAULTS.md §T08` |
| S38–S41 | `DBIG_ADDSUB2`, `DBIG_MAXIDX_TOP` (scan from the top), `DBIG_QSEL` (built in) | 4 n −50.0 s [−71.7, −28.3]; 1 n 94 → 78 s (m) | `results/KERNELS.md §S43`, `results/KERNELS.md §S38`, `results/KERNELS.md §S41` |
| C1 | `MN_T_CHUNK_MB=2048` on the aac7 line and the target template | 10 n −42.3 s, 24 pairs, p ≈ 0.04; 4 n recheck +7.0 s [−10.3, +24.3] (m); target A/B still to run | `results/SESSIONS.md §S22`, `results/LAUNCH_DEFAULTS.md §S47` |
| C2 | X2 (`COMM_LAYER_INTER2`, `COMM_LAYER_VSLOT_POOL`), code default since wave 2 | 10 n −31.7 s [−55.7, −7.8] (m); target try still to run | `results/SESSIONS.md §S31`, `results/CODE_REDUCTION.md §W2B` |
| M3 / S-1 | `COMM_LAYER_VSLOT_SHARE` and `COMM_LAYER_VSLOT_RELEASE` (code defaults since wave 2; switches removed 2026-10-19, task 4: always on) | SHARE −8.4 GB per node at 4 n (m); RELEASE −8.60 GB device peak at 4 n (m), −8.59 GB at 576 n (mod) | `results/GRID_GROUPS.md §S36`, `results/GRID_GROUPS.md §C06`, `results/LAUNCH_DEFAULTS.md §L1` |
| M1 | Spend the memory margin on digits: target size 3.82 × 10¹³ (+3.0 %) | rank-0 margin 12.9 GB (mod); 3.83 × 10¹³ and up change the plan (+20 s), ≥ 3.95 × 10¹³ unsafe (mod) | `results/GRID_GROUPS.md §S52`, `results/LAUNCH_DEFAULTS.md §L1` |
| G1 | `FI_MR_CACHE_MONITOR=userfaultfd` against the mapper deadlock / segfault | reproducer hangs 22/105 → 0/104; 46 four-node runs, 0 segfaults (m) | `results/CRASHES_HANGS.md §S46`, `results/OFI_FABRIC.md §T12` |
| DC8 | `DIST_CHUNKS=8` | device −13.4 GB per node at 576 n for +2.4 s (mod) | `results/GRID_GROUPS.md §S45` |
| OFI | `comm_ofi` transport where a cxi NIC exists | × 2.1 over SHMEM at 10 n (1670 vs 3532 s, m) | `results/OFI_FABRIC.md §OFI17` |
| DKM | Division in two quotient halves | −25.7 s at 10¹¹ (m) | `results/DIVISION.md §DKM15` |
| P24 | 24 digits per transform point in multi-node products | −38 % at 10¹⁰ on 2 processes (m) | `results/PRIMES_STRATEGY.md §P2415` |
| IO | Write overlapped with the division, corrections patched (`ECALC_CORR_PATCH=2`, `MN_OUT_EARLY`, `MN_OUT_DKM_HI`) | write hidden under the division (m) | `results/OUTPUT_IO.md §IO215` |
| NP | Three primes at size 1, four above the bound (`ECALC_NP=auto`) | three primes −17 % wall, −25.8 GB (m) | `results/PRIMES_STRATEGY.md §B13b` |
| K5 | Scratch removal in `k_gather_shift` / `k_addsub2` | already scratch-free since S41 (closed as done) | item A08 (no report file kept) |
| G3 | Slow-host policy: post-run check of the first device upload, exclusion only | slow host stalls 21–25× median at first upload (m) | `archive:results/A03.md`, `results/TOOLS_PROFILES.md §A06` |

## Tried — not adopted / rejected

| ID | idea | measured result (why not) | evidence |
|---|---|---|---|
| N7E | `COMM_SHMEM_PX_SEND=2` + `COMM_SHMEM_PX_INIT=1` on the proxy mode (6 n, 3.8646e11): paired repeat, B − A +129.3 s [+92.9, +165.7] (28 pairs, 1.68 × comm_ofi); the N7D 1.43 × is not reproduced; PX_SEND 2 vs 1 +69.9 s [−217.8, +357.6] (4 pairs); PX_INIT 1 vs 0 +107.2 s [−50.5, +264.9] (3 pairs) | m | results/SHMEM_ITEMS.md §N7E |
| N7G | `COMM_SHMEM_PX_THP=1` + `COMM_SHMEM_PX_PIN=1` on the proxy mode (6 n, 3.8646e11): 3 pairs, B − A −124.9 s [−460.6, +210.8], not significant (m). THP alone then won 4 pairs (−287.8 s, CI [−507, −68], m) and became the default (D36); `COMM_SHMEM_PX_PIN` showed no significant gain and was removed 2026-10-18 | m | results/SHMEM_ITEMS.md §N7G |
| K1 / P2 | `k_gather` tiled / coalesced write (`DIST_GATHER_TILE`, removed) and a kernel rewrite | S50 −0.08 ± 0.88 s at 1 n; P2 probe: floor is the remote read, ≤ 0.6 s headroom (m, mod) → N2 | `results/EXCHANGE.md §S50`, `results/EXCHANGE.md §P2` |
| P3 / K8 | Shorter init mapping: early publish of mapped chunks to the seed DMA (`ECALC_VMM_PUBLISH`, not built); K8 init reorderings | P3: ≤ 1–1.5 s at 1 n, 0 at 2–6 n and at the target (mod); K8: each reordering +2.0 … +11.5 s (m) | `results/MEMORY.md §P3`, `results/MEMORY.md §K8` |
| S52 / C3 | `DIST_COALESCE` message aggregation | 1.0–4.5 s at 576 n (mod); dropped for now by the user; reopen if the target's per-message cost is ≥ 8.5 µs | `results/GRID_GROUPS.md §S52` |
| K6 | `RNS_DIST_OVERLAP` (overlap the single-node C-tier all-to-all across primes; removed) | −0.11 ± 0.15 s of 73.4 s (m) | `results/EXCHANGE.md §K6` |
| K7 / O5 | B5 balanced 4-way B form (APU 3 rebalance) | +0.08 s [−0.75, +0.91] at 10¹⁰ (m) | `results/DIVISION.md §O5` |
| K4 | `k_crt_batch` B-form occupancy (`RNS_CRT_SUB`, removed) | no gain (m) → K4b for the C form | `results/KERNELS.md §S51`, `results/CODE_REDUCTION.md §W2B` |
| C4 | `COMM_SHMEM_RPIPE` two-deep rounds (removed) | +1.82 s [−14.35, +17.99] at 4 n (m) | `results/DIVISION.md §O2`, `results/CODE_REDUCTION.md §W2B` |
| C5 | `MN_REDIST_FUSE` one all-to-all for A, B, X (removed) | −0.78 s [−40.89, +39.34] at 4 n (m) | `results/DIVISION.md §O2`, `results/CODE_REDUCTION.md §W2B` |
| C6 | Result-exchange rank-gradient rebalancing | ≈ −7 s at 10 n (mod), below noise; needs a re-layout | `results/EXCHANGE.md §A07` |
| C8 | Peer schedules to close the layered-vs-NIC gap (`rot`, `rr`, `winK`) | `rot` null (+3.1 s, S27); the others no gain, not built | `results/OFI_FABRIC.md §C02`, `results/SESSIONS.md §S27` |
| O1 | `MN_GROUPS=6` instead of `2,6` at 6 n | +2.4 s [−3.9, +8.7], p 0.35 (m) | `results/GRID_GROUPS.md §O1` |
| B06 | Uneven node groups | correct, not faster (m) | `results/GRID_GROUPS.md §B06` |
| layv | Non-power-of-two exchange path; `COMM_SHMEM_LAYV_ANY` (layered exchange for any group size, removed) | no path penalty (−0.36 s, m); the 6-node slowdown is the group size; LAYV_ANY +4.6 s [−174, +183] (4 pairs) and no effect over 8 pairs (m) | `results/TIME_MODEL.md §S48`, `archive:results/O9.md`, `results/SHMEM.md` |
| VMM_BG | `ECALC_VMM_BG=0` (synchronous mapping), `=2` (lock, removed), `ECALC_VMM_SAFE` (removed), `=auto` | `=0` +20 s at 2 n; `=2` 9/9 reproducer fails; `auto` saves ≈ 0 at the target map rate (m, mod) | `results/CRASHES_HANGS.md §S34`, `results/CRASHES_HANGS.md §S46`, `archive:results/A09.md` |
| A5 | Device overlap flow `ECALC_OVERLAP` and device Newton `NEWTON_DEVICE` (removed as losing flows) | losing flows; removed 2026-10-12 | `results/CODE_REDUCTION.md §W2A` |
| RNS_PLANES_3Q30 | 3·2^k planes in the distributed tier | 80.8 ± 1.3 vs 82.0 ± 0.3 s at 4 × 10¹⁰ (m), within noise; mapping cost | `results/MEMORY.md §I`, `results/CODE_REDUCTION.md §AUD3` item 3 |
| NTT3P | 3-pass 2^31 NTT (13/9/9) | 90.6 vs 89.9 ms (m); gate ≤ 80 ms; shelved | `results/KERNELS.md §NTT3P`, `results/SESSIONS.md §S22` |
| S-3 / S-6 | Barrett constant for decimal seeds; HIP graphs for small dispatches | dropped after measurement | `archive:results/S37.md` |
| S-5 | Push-form broadcast for B | below 1 s (S38) | `results/KERNELS.md §S38` |
| A37-R2 | base-2^64 sub-range seed on the CPU | no one-thread gain (ratio 0.86–1.07, m) | `results/SINGLE_NODE.md §S21` |
| A37-R4 | `fmt18` two-digit formatting | packed output needs no formatting on the clock | `results/SINGLE_NODE.md §S21` |
| A37-R5 | Batch tile pipeline (two streams) | realistic −1.5 … −3 s, not worth it | `results/SESSIONS.md §S27` |
| X1 / X3 / CHAIN_BCAST | Rotated start, OFI signalling, chain broadcast | null or rejected | `results/SESSIONS.md §S27`, `results/CRASHES_HANGS.md §S30` |
| E9 | Karatsuba over the device grid | +68–89 GB memory; rejected | `archive:docs/OPPORTUNITIES.md` |
| E6 | Transform lengths 5·2^k, 7·2^k | needs a prime change; dropped | `archive:docs/OPPORTUNITIES.md` |
| E5 | Spills to the file system | file system at 0.6 GB/s; not pursued | `archive:docs/OPPORTUNITIES.md` |
| 4PE | 4 processes per node (one per APU) | 3.7× slower at 8 n (m) | `archive:docs/OPPORTUNITIES.md` |
| spill | `hipMallocManaged` host spill at the device edge | host 21 GB/s vs HBM 3.2–3.8 TB/s (m) | `results/TARGET.md §TGTBENCH2` |
| CPX | CPX partition mode | 0.2–0.4 s gains (m); the target runs SPX | `results/KERNELS.md §S44` |

## Ranking

Top 10 open for the target run, by payoff per effort (payoffs as in the Open table; re-ranked 2026-10-12 on the user-supplied target facts):

| # | ID | why here |
|---|---|---|
| 1 | M7c | the 100 GB/s per APU of every estimate rests on both NICs working: +37 s (mod) if one does; one kit stage |
| 2 | N8 | write −11.8 … −47.8 s (mod) to reach the file system's 1 TB/s; step (a) is a benchmark and a setting |
| 3 | N7 | SHMEM may become primary: ≥ +37 s risk (mod); OpenSHMEMX 12.1.0 untested |
| 4 | N1 | the largest measured multi-node cost (+94 s at 6 n); open, not started |
| 5 | M7d | ROCm 7.14.0 builds; its speed is one aac7 ABBA away (a release moved +46 s before, mod) |
| 6 | M7a | −4.7 s (mod) for one launch-line value |
| 7 | C7 | −6 … −17 s realistic (mod/a); built only if the target gate opens |
| 8 | C3 | 0 … −18.5 s (mod), depends on the target's per-message cost |
| 9 | N4 | ≈ 20 s in a third of 4-node runs (mod); unknown at 576 |
| 10 | M2 | no seconds, but every 576 ranking depends on it |

Top 5 testable on ≤ 6 aac7 nodes now:

| # | ID | test |
|---|---|---|
| 1 | N1 | division multiply: stage and wait split at 2/4/6 n with `MN_WAIT_STATS`, then a fix and ABBA |
| 2 | M7d | ROCm 7.14.0 (the target's) against 7.2.4 at 1 n: gate, then ABBA |
| 3 | N7 | `COMM_OFI=0` at 2–6 n: proxy mode stable since the NP heap fix (D48); open: 6-n confirmation, the 3-6 % gap at 3-4 n, hugetlb (N9); record: `results/SHMEM.md` |
| 4 | N3 | host gaps in the batch tier: profile and overlap at 1 n, ABBA — done, adopted (D20) |
| 5 | K4b | `k_crt_batch` C-form carry chain at 1 n, ABBA — done: −2.52 s [−4.19, −0.85] (`results/KERNELS.md §K4b`); adopted (D21) |
