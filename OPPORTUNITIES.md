# OPPORTUNITIES — every performance idea for ecalc, open or tried

Scope: every known opportunity to improve performance (time, memory, concurrency, algorithm), open or closed, with its evidence. | Maintained: updated when an idea is found, tried, adopted or rejected.

Rebuilt 2026-10-12 from the archived ranked list (`archive:docs/OPPORTUNITIES.md`, 2026-10-10..12), the old task list
(`archive:TASKS_HISTORY_2026-10-12.md`), the target notes (technical ideas only), the TASKS "Ideas" section and the
follow-ups in `results/`. Status is checked against the code after the wave 2 settings removal (`ecalc/switches.tsv`,
`results/W2A.md`, `results/W2B.md`). Old IDs are kept (G = robustness, C = fabric/exchange, K = per-node kernels,
M = memory/model/target levers, S-/A37- = idea sweeps); new IDs are N1, N2, ….

Labels: **m** measured, **mod** modelled, **a** assumed. Scale: **1 n** one node at 6.441 × 10¹⁰ digits, **2–6 n** aac7
at 6.441 × 10¹⁰ digits per node, **576 n** the target run (3.82 × 10¹³ digits). Effort: S < 1 day, M 1–3 days,
L > 3 days. Needs: **local** (no GPU or any one node), **aac7** (≤ 6 nodes), **target** (only the target can show it).

Where the time goes today (m, `results/M2a.md`, before the 2026-10-12 defaults): 1 / 2 / 4 / 6 nodes take 74.4 / 138.9 /
188.2 / 246.4 s; at 6 nodes the growth over 1 node is +172 s, of it division (dm) +94 s, binary splitting (bs) +60 s,
reciprocal +22.5 s, init +14 s. Target standing estimate: about 182 s without the write (mod, fabric a; `results/L4.md`).

## Open

| ID | area | idea | expected payoff (label, scale) | effort | needs | evidence |
|---|---|---|---|---|---|---|
| N1 | fabric/exchange | Distributed division multiply: find why dm grows +94 s from 1 to 6 nodes (wait vs volume vs skew) and cut it | the largest multi-node cost: +94 s at 6 n (m); 576 n share unknown | M | aac7 | `results/M2a.md`; TASKS P (running) |
| C7 | division | Division and reciprocal overlap (S-2): overlap the division's exchanges with its compute; gate = target wait-stats run shows > 30 s exposed wait per APU thread | 576 n ceiling 25–34 s (mod), realistic 6–17 s (a); 10-node ceiling 43 s (mod) | M | target (gate), aac7 (prototype) | `results/S22.md` §2; kit stage `s2chk` |
| M7c | fabric/exchange | Two NIC endpoints per APU (`COMM_OFI_NICS`) on the target | up to 2× injection per APU if one context does not already fill both (a); 576 n −5…−50 s range for all M7 levers (mod) | S | target | `archive:docs/OPPORTUNITIES.md` M7; target task list T8 (private) |
| M7d | build/target | Run on the newer ROCm release on the target (7.2.4 vs 7.0.3) | +46 s at 576 n if left on the older release (mod); 22 % on local passes (m) | S (request) | target | `archive:docs/OPPORTUNITIES.md` M7 |
| M7a | fabric/exchange | 9-way top schedule `MN_GROUPS=…,64,576` instead of the 3·3 top `…,64,192,576` | −4.7 s at 576 n (mod, 5.276 × 10¹³ era) | S | target | `archive:docs/OPPORTUNITIES.md` M7; target task list T5 (private) |
| M7b | output | Output file striping and write waves (`MN_OUT_STRIPE`, `MN_OUT_WAVES`) tuned to the target's parallel file system | exposed write at 576 n unknown; packed write already −6.25 % bytes (m) | S | target | `results/O4.md`; target task list T10 (private) |
| C3 | fabric/exchange | Run the reciprocal's small doublings (and the division's small products) on a subgroup instead of all 576 nodes, cutting the 1.10 M reciprocal messages per APU | 0 … −18.5 s at 576 n, depends on the target's per-message cost (mod); 1 n part ≈ 0.02 s (m) | M–L | target | `results/S52.md`, `results/S21.md` (A37-R6) |
| A37-R1 | init | GPU seed kernel (the CPU seed thread binds init on aac7 1 n); gate = target init timeline shows the seed ending > 18 s and > 2 s after the last mapping | 0–1 s at 576 n (mod, S22), 0–6 s (mod, S21) | L | target (gate) | `results/S22.md` §3, `results/S21.md` |
| N2 | per-node compute | Gather layout: each rank holds its own rows so the 3/4 remote pull in `k_gather` / `k_bload` disappears (the kernel floor is the remote read) | `k_gather_raw` 2.0 s + `k_bload` 1.2 s per APU at 1 n (m); at 576 n only the per-node leaf, ≈ 1 s (mod) | L (needs scoping) | aac7 1 n | `results/P2.md`, `results/K1b.md` §6; TASKS "Proposed" |
| N3 | per-node compute | Host gaps in the batch tier: overlap host layout and add+norm (1.4 s, m) and sync points; kernel-busy only 62–88 % in the active window | 1–3 s at 1 n (mod, half the 3.5–6 s gap removable, a) | M | aac7 1 n | `archive:results/O7b.md` §D |
| K4b | per-node compute | `k_crt_batch` C form: restructure the serial carry chain per chunk (B-form occupancy was K4, no gain) | up to 2.0 s at 1 n (a); 4.5–4.8 s per APU today (m) | M | aac7 1 n | `archive:results/O7b.md` §D, `results/S39.md` |
| N4 | fabric/exchange | Slow launch mode: the inter-node all-to-all rate is set per launch (NIC counter group, ≈ 50 % of launches); find what picks it, then retry at init when the first exchange measures slow | ≈ 20 s in ≈ 30 % of 4 n runs (mod); −12 % exchange rate in slow launches (m); 576 n unknown | M | aac7 4 n | `results/A13.md`, `results/A14.md`, `results/T05.md` |
| C9 | fabric/exchange | dm "blow-ups" (dm > 290 s in 1 of 8–16 ten-node runs) and ±12–25 % job-to-job fabric noise: diagnose the cause | 50–100 s per affected 10-node run (m); 576 n unknown | M | aac7 (if it shows at ≤ 6 n) | `results/S31.md`, `results/S19B.md` |
| N5 | division | One single-node division flow: below about 1.9 × 10¹⁰ digits size 1 takes the host flow with the full-length reciprocal, not DKM; route it through the device DKM flow (AUD3 item 1) | runs below 1.9 × 10¹⁰ get the DKM gain (DKM was −25.7 s at 10¹¹, m); 0 at 6.441 × 10¹⁰ and at 576 n | S | aac7 1 n | `results/AUD3.md` item 1, `results/DKM15.md` |
| N6 | fabric/exchange | RCCL or MPI all-to-all across nodes as an alternative transport | worth building only if > 1.2× over `comm_ofi` at scale (a); in-node RCCL is 0.24× our push kernel (m) | L | target | target measurement list §1.8 (private) |
| M2 | model | Refit the time model to the current code with the M2a scaling points (1/2/4/6 n, stage split) so 576 n rankings rest on current data | no seconds; the model was 1 n 109.5 vs 78 s (m) before K2/K3 entered it | M | local + aac7 | `results/M2a.md`, `results/O8.md` |
| M4 | memory | Re-check the old Phase-12 memory items: `mdb_shift` exchange scratch (copies Q at g = 1, 29 GB at 6.44 × 10¹⁰), the O(share) window temporary | unknown; probably absorbed by DM_MN_LEAN and T_CHUNK (a) | S | local | `archive:docs/OPPORTUNITIES.md` M4, `results/AUD3.md` item 5 |
| AUD3-6 | per-node compute | P24 (four primes, 24 digits per point) as the only multi-node product form, so aac7 runs the target's form | time ≈ equal per product (mod, unmeasured at aac7 sizes); +4 GiB per APU in pool 0 at 2–16 n (mod) | M | aac7 2 n | `results/AUD3.md` item 6, `results/P2415.md` |
| AUD3-10 | memory | One transform cache (partial slots for `mul_grid` too) | unknown at 1 n (needs ABBA); at 576 n the full slots already get 0 slots (m) | M | aac7 1 n | `results/AUD3.md` item 10, `results/TC15.md` |

## Tried — adopted

| ID | idea | measured result | evidence |
|---|---|---|---|
| CAND (O2, K1b, K8b) | Three speed defaults 2026-10-12: `DBIG_SMALL_BOUNCE=1` (no pinning of 4 × 2 GiB on the first `db_to_bi`; fixes the 15–19 s chain stall), `DIST_GATHER_RAW=2` (one raw pull per operand), `BS_SEED_PULL=1` (seed DMA by pull kernels) | 1 n −7.16 s [−8.08, −6.23] (aac6); 2 n −5.58 s [−10.54, −0.63], 12 pairs; chain 4.57 → 0.06 s; O2 alone −30.1 s total at 4 n (m) | `results/CAND.md`, `results/O2.md`, `results/K1b.md`, `results/K8b.md` |
| D5 / M5 | 60-bit packed output (`ECALC_OUT_PACKED=2`) as the default 2026-10-12 | −6.25 % bytes; 10¹⁰ write 18.6 vs 27.9 s ASCII; digits identical (m) | `results/O4.md`, `results/C05.md`, `results/W2A.md` |
| H4 | Decimal limbs only (binary pipeline removed) 2026-10-12 | binary 1.9–2.25× slower (+32.47 s at 10¹⁰, 2 n); removal neutral +0.09 s [−2.08, +2.27] (m) | `results/H5.md`, `results/H4.md` |
| K2, K3 | `k_addsub2` wave-scan (ADDSUB3) and Montgomery `k_modq` (built in) | 1 n −2.03 ± 1.38 s aac7, −1.22 ± 0.84 s aac6; 4 n not significant (m) | `results/S51.md`, `results/T08.md` |
| S38–S41 | `DBIG_ADDSUB2`, `DBIG_MAXIDX_TOP` (scan from the top), `DBIG_QSEL` (built in) | 4 n −50.0 s [−71.7, −28.3]; 1 n 94 → 78 s (m) | `results/S43.md`, `results/S38.md`, `results/S41.md` |
| C1 | `MN_T_CHUNK_MB=2048` on the aac7 line and the target template | 10 n −42.3 s, 24 pairs, p ≈ 0.04; 4 n recheck +7.0 s [−10.3, +24.3] (m); target A/B still to run | `results/S22.md`, `results/S47.md` |
| C2 | X2 (`COMM_LAYER_INTER2`, `COMM_LAYER_VSLOT_POOL`), code default since wave 2 | 10 n −31.7 s [−55.7, −7.8] (m); target try still to run | `results/S31.md`, `results/W2B.md` |
| M3 / S-1 | `COMM_LAYER_VSLOT_SHARE` and `COMM_LAYER_VSLOT_RELEASE` (code defaults since wave 2) | SHARE −8.4 GB per node at 4 n (m); RELEASE −8.60 GB device peak at 4 n (m), −8.59 GB at 576 n (mod) | `results/S36.md`, `results/C06.md`, `results/L1.md` |
| M1 | Spend the memory margin on digits: target size 3.82 × 10¹³ (+3.0 %) | rank-0 margin 12.9 GB (mod); 3.83 × 10¹³ and up change the plan (+20 s), ≥ 3.95 × 10¹³ unsafe (mod) | `results/S52.md`, `results/L1.md` |
| G1 | `FI_MR_CACHE_MONITOR=userfaultfd` against the mapper deadlock / segfault | reproducer hangs 22/105 → 0/104; 46 four-node runs, 0 segfaults (m) | `results/S46.md`, `results/T12.md` |
| DC8 | `DIST_CHUNKS=8` | device −13.4 GB per node at 576 n for +2.4 s (mod) | `results/S45.md` |
| OFI | `comm_ofi` transport where a cxi NIC exists | × 2.1 over SHMEM at 10 n (1670 vs 3532 s, m) | `results/OFI17.md` |
| DKM | Division in two quotient halves | −25.7 s at 10¹¹ (m) | `results/DKM15.md` |
| P24 | 24 digits per transform point in multi-node products | −38 % at 10¹⁰ on 2 processes (m) | `results/P2415.md` |
| IO | Write overlapped with the division, corrections patched (`ECALC_CORR_PATCH=2`, `MN_OUT_EARLY`, `MN_OUT_DKM_HI`) | write hidden under the division (m) | `results/IO215.md` |
| NP | Three primes at size 1, four above the bound (`ECALC_NP=auto`) | three primes −17 % wall, −25.8 GB (m) | `results/B13b.md` |
| K5 | Scratch removal in `k_gather_shift` / `k_addsub2` | already scratch-free since S41 (closed as done) | `archive:results/A08.md` |
| G3 | Slow-host policy: post-run check of the first device upload, exclusion only | slow host stalls 21–25× median at first upload (m) | `archive:results/A03.md`, `results/A06.md` |

## Tried — not adopted / rejected

| ID | idea | measured result (why not) | evidence |
|---|---|---|---|
| K1 / P2 | `k_gather` tiled / coalesced write (`DIST_GATHER_TILE`, removed) and a kernel rewrite | S50 −0.08 ± 0.88 s at 1 n; P2 probe: floor is the remote read, ≤ 0.6 s headroom (m, mod) → N2 | `results/S50.md`, `results/P2.md` |
| P3 / K8 | Shorter init mapping: early publish of mapped chunks to the seed DMA (`ECALC_VMM_PUBLISH`, not built); K8 init reorderings | P3: ≤ 1–1.5 s at 1 n, 0 at 2–6 n and at the target (mod); K8: each reordering +2.0 … +11.5 s (m) | `results/P3.md`, `results/K8.md` |
| S52 / C3 | `DIST_COALESCE` message aggregation | 1.0–4.5 s at 576 n (mod); dropped for now by the user; reopen if the target's per-message cost is ≥ 8.5 µs | `results/S52.md` |
| K6 | `RNS_DIST_OVERLAP` (overlap the single-node C-tier all-to-all across primes; removed) | −0.11 ± 0.15 s of 73.4 s (m) | `results/K6.md` |
| K7 / O5 | B5 balanced 4-way B form (APU 3 rebalance) | +0.08 s [−0.75, +0.91] at 10¹⁰ (m) | `results/O5.md` |
| K4 | `k_crt_batch` B-form occupancy (`RNS_CRT_SUB`, removed) | no gain (m) → K4b for the C form | `results/S51.md`, `results/W2B.md` |
| C4 | `COMM_SHMEM_RPIPE` two-deep rounds (removed) | +1.82 s [−14.35, +17.99] at 4 n (m) | `results/O2.md`, `results/W2B.md` |
| C5 | `MN_REDIST_FUSE` one all-to-all for A, B, X (removed) | −0.78 s [−40.89, +39.34] at 4 n (m) | `results/O2.md`, `results/W2B.md` |
| C6 | Result-exchange rank-gradient rebalancing | ≈ −7 s at 10 n (mod), below noise; needs a re-layout | `results/A07.md` |
| C8 | Peer schedules to close the layered-vs-NIC gap (`rot`, `rr`, `winK`) | `rot` null (+3.1 s, S27); the others no gain, not built | `results/C02.md`, `results/S27.md` |
| O1 | `MN_GROUPS=6` instead of `2,6` at 6 n | +2.4 s [−3.9, +8.7], p 0.35 (m) | `results/O1.md` |
| B06 | Uneven node groups | correct, not faster (m) | `results/B06.md` |
| layv | Non-power-of-two exchange path | no path penalty (−0.36 s, m); the 6-node slowdown is the group size | `results/S48.md`, `archive:results/O9.md` |
| VMM_BG | `ECALC_VMM_BG=0` (synchronous mapping), `=2` (lock, removed), `ECALC_VMM_SAFE` (removed), `=auto` | `=0` +20 s at 2 n; `=2` 9/9 reproducer fails; `auto` saves ≈ 0 at the target map rate (m, mod) | `results/S34.md`, `results/S46.md`, `archive:results/A09.md` |
| A5 | Device overlap flow `ECALC_OVERLAP` and device Newton `NEWTON_DEVICE` (removed as losing flows) | losing flows; removed 2026-10-12 | `results/W2A.md` |
| RNS_PLANES_3Q30 | 3·2^k planes in the distributed tier | 80.8 ± 1.3 vs 82.0 ± 0.3 s at 4 × 10¹⁰ (m), within noise; mapping cost | `results/I.md`, `results/AUD3.md` item 3 |
| NTT3P | 3-pass 2^31 NTT (13/9/9) | 90.6 vs 89.9 ms (m); gate ≤ 80 ms; shelved | `results/NTT3P.md`, `results/S22.md` |
| S-3 / S-6 | Barrett constant for decimal seeds; HIP graphs for small dispatches | dropped after measurement | `archive:results/S37.md` |
| S-5 | Push-form broadcast for B | below 1 s (S38) | `results/S38.md` |
| A37-R2 | base-2^64 sub-range seed on the CPU | no one-thread gain (ratio 0.86–1.07, m) | `results/S21.md` |
| A37-R4 | `fmt18` two-digit formatting | packed output needs no formatting on the clock | `results/S21.md` |
| A37-R5 | Batch tile pipeline (two streams) | realistic −1.5 … −3 s, not worth it | `results/S27.md` |
| X1 / X3 / CHAIN_BCAST | Rotated start, OFI signalling, chain broadcast | null or rejected | `results/S27.md`, `results/S30.md` |
| E9 | Karatsuba over the device grid | +68–89 GB memory; rejected | `archive:docs/OPPORTUNITIES.md` |
| E6 | Transform lengths 5·2^k, 7·2^k | needs a prime change; dropped | `archive:docs/OPPORTUNITIES.md` |
| E5 | Spills to the file system | file system at 0.6 GB/s; not pursued | `archive:docs/OPPORTUNITIES.md` |
| 4PE | 4 processes per node (one per APU) | 3.7× slower at 8 n (m) | `archive:docs/OPPORTUNITIES.md` |
| spill | `hipMallocManaged` host spill at the device edge | host 21 GB/s vs HBM 3.2–3.8 TB/s (m) | `results/TGTBENCH2.md` |
| CPX | CPX partition mode | 0.2–0.4 s gains (m); the target runs SPX | `results/S44.md` |

## Ranking

Top 10 open for the target run, by payoff per effort (payoffs as in the Open table):

| # | ID | why here |
|---|---|---|
| 1 | M7d | +46 s (mod) for a request, no code |
| 2 | M7c | possibly up to 2× injection per APU for one kit line (a) |
| 3 | N1 | the largest measured multi-node cost (+94 s at 6 n); running |
| 4 | M7a | −4.7 s (mod) for one launch-line value |
| 5 | C7 | −6 … −17 s realistic (mod/a); built only if the target gate opens |
| 6 | C3 | 0 … −18.5 s (mod), depends on the target's per-message cost |
| 7 | M7b | the write is the one stage not yet measured on the target |
| 8 | N4 | ≈ 20 s in a third of 4-node runs (mod); unknown at 576 |
| 9 | M2 | no seconds, but every 576 ranking depends on it |
| 10 | A37-R1 | 0–6 s (mod), gated on one target init timeline |

Top 5 testable on ≤ 6 aac7 nodes now:

| # | ID | test |
|---|---|---|
| 1 | N1 | division multiply: stage and wait split at 2/4/6 n with `MN_WAIT_STATS`, then a fix and ABBA |
| 2 | N3 | host gaps in the batch tier: profile and overlap at 1 n, ABBA |
| 3 | K4b | `k_crt_batch` C-form carry chain at 1 n, ABBA |
| 4 | N4 | slow-mode cause: many short `t_comm --bw` launches at 4 n, then retry-at-init |
| 5 | M2 | add 4/6-node points on the current defaults and refit the model |
