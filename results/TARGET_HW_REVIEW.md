# TARGET_HW_REVIEW — the target's microbenchmark report against the model (desk review, 2026-10-03, 22:52 EDT)

Source: `/home/machinus/apucode/TARGET_HARDWARE.txt` (564 lines; one node, `baryon-cn0059`, 4 × MI300A, ROCm 7.0.3,
XNACK off; 157 binaries, 13,160 measurements). Compared with `ecalc/mn_model.py`, `ecalc/mem_model.py`,
`docs/TARGET.md` §6, `docs/TARGET_TASKS.md` T1, `results/AAC7_survey.md` and the memory file of measured MI300A facts.
No code was read or edited; the only runs were `ecalc/estimate.py` on this workstation (main a24ef3c).
Every number is labelled **measured** (the report, or our aac6 runs), **modelled** (`estimate.py`) or **assumed**.

## 1. What the report is, and what it is not

The report is a **single-node, in-node** characterization: launch overhead, the memory hierarchy (L2 / Infinity
Cache / HBM), MFMA throughput, xGMI P2P via RCCL, precision conversion, kernel fusion, atomics, per-APU binning,
thermals. Its coverage matrix lists "MPI collectives", "I/O buffering", "PCIe bandwidth", "NUMA effects" and
"Advanced (persistent/UM)" as benchmarked categories, but the report **prints no number for any of them**.

Therefore it says **nothing** about the inputs the 576-node estimate is most sensitive to: the Slingshot injection
bandwidth per APU, the per-message cost of a SHMEM put, the NIC count and NUMA affinity, libfabric / cxi behavior,
GPU-initiated transfers, the chunk-round cost `T_ROUND`, Lustre (per-node rate, aggregate with 576 writers, OSTs,
striping), the device-memory mapping rate, page sizes, or the node's memory edge. Of TARGET_TASKS T1's list, the
report closes **none** of items 1–9 outright; it confirms the silicon-level facts the model already carries from aac6.

## 2. Assumption by assumption

| # | input | ours (source) | the target report | effect on 5.276 × 10¹³ on 576 (284.2 / 299.3 s standing) |
|---|---|---|---|---|
| 1 | Fabric injection per APU | **100 GB/s assumed** (`mn_model.TARGET`, two 400 Gb/s NICs per APU, PLAN §25) | not measured (no network benchmark) | unchanged. Sensitivity, modelled on the base row (292.3 / 306.5 s without the partial cache): 50 GB/s → 352.3 / 360.1 s (+60 s); 25 GB/s → 472.2 / 467.0 s (+180 s). One 200 Gb/s NIC per APU (aac7's class) on the target would cost +180 s |
| 2 | Per-message cost | **2 µs assumed** (`--lat`) | not measured for the NIC; xGMI P2P latency 3.75 µs (<1 KB) … 6.33 µs (>256 KB), measured, RCCL | if the NIC's put cost resembled xGMI's: `--lat 3.75e-6` → +4.5 s (296.8 / 310.2 s on the base row), `--lat 6.33e-6` → +10.9 s (303.2 / 315.3 s). Applied to the standing figure: ≈ 288.7 / 303.8 and 295.1 / 310.2 s (modelled; the `RNS_DIST_CACHE_PARTIAL` row of `estimate.py --target` does not take `--bw` / `--lat`, see §5) |
| 3 | `T_ROUND` (one chunk round) | **0.030 s fitted on aac6 loopback ± 100 %** (`MN_MODEL_T_ROUND`) | not measured; the report's launch figures bound the launch part of it: async 0.91 µs, steady sync 7–8 µs (measured) — launches are ≪ 1 ms of the 30 ms | unchanged. `MN_MODEL_T_ROUND=0.01` → **271.1 / 288.2 s**; `0.1` → **329.7 / 338.1 s** (modelled, the standing row). Still the largest open input after the fabric; aac7 can measure it (T11's procedure at 8–12 nodes) |
| 4 | Overlap / `GEN_HIDE` | `HIDE_POW2` 0.75, `GEN_HIDE_DEPTH` {1: 0.011, 2: 0.74} measured on aac6 (xGMI under loopback / 1 GbE) | not measured; the report's "P2P" is RCCL inside the node, no overlap test | unchanged; aac7 measures it at Slingshot rates (T1 item 2, `COMM_LAYER_STATS=1 COMM_XGMI_STATS=1`) |
| 5 | Part-file write | **1.0 GB/s per node assumed** to hold with 576 writers (`TARGET_WRITE_BW`; the user's Lustre test 2026-09-29) | nothing on Lustre or I/O | unchanged: 299.3 s at 1.0, 326.4 at 0.6, 278.9 at 2.0 (modelled). Aggregate, striping, waves (T1 item 5b/c, T10) stay target-only; aac7 has no Lustre |
| 6 | xGMI per APU | 3 × 91 GB/s links (RESULTS 11); all-to-all push kernel **909 GB/s per node, 64-bit stores** (measured aac6); `T_XGMI_31` 0.019 s per transform per prime measured; 84 % hidden | **1.6 TB/s aggregate, RCCL ring AllReduce 1 MB** (bus-bandwidth convention, × 1.5 over algorithm bandwidth at 4 ranks → ≈ 1.07 TB/s of data ≈ 267 GB/s per APU); topology symmetric, < 5 % pair variance; `hipMemcpyPeerAsync`-class copies not reported | none: our push reaches ≈ 227 GB/s per APU = 83 % of the report's link class, on identical silicon; the model's xGMI terms are measured kernel times. The symmetric topology confirms no pair-specific placement is needed |
| 7 | HBM / LDS / compute | NTT kernels calibrated on aac6 MI300A (2325 Gbfly/s; `hipHostMalloc` streams 14.0 TB/s per node = 3.5 TB/s per APU) | HBM 3.2 TB/s sustained per APU (STREAM Triad, 60 % of peak), 3.96 TB/s max at 64 MB; IC 1.2 TB/s, 256 MB; L2 800 GB/s; sclk 1,595–1,626 MHz, mclk 1,300 MHz | none to the calibrated terms (same silicon, same ROCm major). The report's per-APU spread (U4) is the one new risk: see row 10 |
| 8 | Node memory | `NODE_GB` 502, budget **480 GB**; edge **524 GB measured on aac6** (Slurm RealMemory 514,000 MiB); node 471.9 GB at the target (modelled) | "501 GB HBM3, shared CPU+GPU, 4 NUMA × ~128 GB, no host DRAM; hipMalloc carves from the same pool" (the unit is loose: 4 × 128 GiB = 549.8 GB; aac7's `free -g` shows 501 GiB = 538 GB, RealMemory 500,000 MiB = 524 GB) | unchanged: 480 GB holds under every reading. The target's edge (T1 item 9) is still unmeasured; aac7 (same RealMemory class as the report's node, probably) can stand in for it |
| 9 | Mapping rate / VMM / pages | `MAP_RATE` 0.065 s/GB measured on aac6 (ROCm 7.2.4) | no VMM, page-size or allocation numbers; ROCm **7.0.3**, XNACK off | unchanged in the model; the ROCm version is the one target fact we can match: aac7's default is 7.0.3 |
| 10 | Per-APU variance | the critical path = the top node's leaf (1.036 × average digits), APUs taken as equal | sclk spread 5 %, HBM 2,780–2,971 GB/s (7 %), FP64 86–92.5 TF across the four APUs (measured, one node) | **assumed +0 … +10 s**: the local terms (≈ 200 s of the 284) × up to 5 % if every level's barrier waits on a slow-bin APU among 2,304; not in the model. Worth a `rocm-smi --showclocks` line in every log |
| 11 | Thermal / power | no throttling in 60 s continuous NTT (measured aac6) | 416 W of 2,200 W, 28–33 °C, 62–65 °C headroom (measured; bursts) | none |
| 12 | Launch overhead | async launches throughout | 0.91 µs async; first launch 354 ms cold, sync 8–22 ms cold then 7–8 µs | none (init absorbs the cold start) |

### Re-run of `estimate.py` with the report's values (modelled, 5.276 × 10¹³ on 576, `MN_OUT_DKM_HI=1`)

| run | no write | with the write @1.0 GB/s | note |
|---|---|---|---|
| standing (`./estimate.py --target`, the `RNS_DIST_CACHE_PARTIAL` row) | **284.2 s** | **299.3 s** | base row without the partial cache: 292.3 / 306.5 s |
| `--lat 3.75e-6` (the report's small-message xGMI latency, as a proxy) | 296.8 (base row) ⇒ ≈ 288.7 | 310.2 ⇒ ≈ 303.0 | +4.5 / +3.7 s |
| `--lat 6.33e-6` (its large-message latency) | 303.2 ⇒ ≈ 295.1 | 315.3 ⇒ ≈ 308.1 | +10.9 / +8.8 s |
| `--bw 50` | 352.3 ⇒ ≈ 344 | 360.1 ⇒ ≈ 353 | the fabric's exposed share at 100 GB/s is ≈ 60 s |
| `--bw 25` (one 200 Gb/s NIC per APU) | 472.2 ⇒ ≈ 464 | 467.0 ⇒ ≈ 460 | the aac7 class of NIC on the target |
| `MN_MODEL_T_ROUND=0.01` | 271.1 | 288.2 | the standing row takes the env |
| `MN_MODEL_T_ROUND=0.1` | 329.7 | 338.1 | |
| `--write-bw 0.6` / `2.0` | 284.2 | 326.4 / 278.9 | |

No input in the report moves the standing estimate; the report's only numeric effect is a latency proxy worth
+4 … +11 s if the NIC put behaved like xGMI P2P, and an unmodelled APU-binning tail of +0 … +10 s (assumed).

## 3. What in the report touches code design

1. **NIC count / affinity: nothing.** The report has no NIC. The design facts stay PLAN §25's (two 400 Gb/s per APU);
   aac7 has one 200 Gb/s Cassini per APU socket (`cxi0..3`, PCI domains 0000–0003), so on aac7 the thread-to-NIC
   affinity (APU i's host thread and staging buffers on NUMA node i, the endpoint on `cxi<i>`) can be checked and the
   model's `--bw` set to 25. T8 (two endpoints per APU) cannot be measured as designed on aac7 (one NIC per APU); a
   two-contexts-on-one-NIC run only says whether one context saturates 200 Gb/s.
2. **SHMEM / libfabric: nothing.** Cray OpenSHMEMX over cxi is unmeasured anywhere; aac7 is the first chance.
3. **GPU-initiated vs host-initiated: indirectly host.** The report's `atomicCAS_i32` at **34 µs** (measured, A-table)
   argues against any device-side polling or signaling loop; our transport is host-initiated SHMEM (T6's rocSHMEM
   put-with-signal stays conditional). Keep the host path; do not add device-side waits.
4. **xGMI: confirmed, no change.** Symmetric topology, 1.6 TB/s RCCL bus bandwidth ≈ our push kernel's class. A cheap
   A/B on aac7 (`rccl-tests/7.0.3` `all_to_all_perf` against `t_comm`'s push) says whether RCCL beats 909 GB/s; only if
   it does by > 1.2× is a transport change worth a switch.
5. **Infinity Cache 256 MB, 16–64 MB sweet spot (1.3× over HBM, measured).** The NTT row kernels stream 2²⁹-point
   planes; the transposes (16 % of the multiply, measured aac6) could be tiled to 16–64 MB working sets. Upper bound,
   modelled: 16 % × (1 − 1/1.3) ≈ 3.7 % of the local passes ≈ −5 … −7 s at the target; a single-node code test (aac6 or
   aac7), behind a switch.
6. **HBM / page sizes / VMM / hipMalloc: nothing measured.** `MAP_RATE` and the 460 GiB `hipHostMalloc` trick remain
   aac6 facts; the only actionable match is **ROCm 7.0.3** — build on aac7 with the system 7.0.3 (the target's version),
   not the 7.2.4 module the survey proposed for aac6 parity, and record init's `pools … s` line at 7.0.3 as the target's
   mapping rate.
7. **Lustre: nothing**, and aac7 has none. Items 5a–d and T10 stay target-only; aac7 can only exercise the writer's
   mechanics (`ECALC_OUT_MODE`, `MN_OUT_THREADS`, waves) over NFS, which says nothing about Lustre.
8. **Memory limits: consistent with 480 GB.** 501 GB/GiB shared pool, no host DRAM, `/tmp` is tmpfs on aac7 (counts
   against the pool: TARGET §6 item 10 holds). The edge is unmeasured on the target; aac7's edge is the proxy.
9. **APU binning (5–7 %)**: log `rocm-smi --showclocks` and the per-APU `dist_mn` times on every run; if the slow APU
   is systematic, the leaf layout's 1.036× top-node factor is the place a per-APU weight would go (design only; no
   change proposed now).

## 4. The revised aac7 plan

| phase | keep / change / drop / new | why | time (EDT, elapsed) |
|---|---|---|---|
| A — port to Cray OpenSHMEMX | **keep, change the toolchain**: ROCm **7.0.3** (the target's, the report's; aac7's default), `module load cray-dsmml cray-openshmemx`, `-lsma`, `srun`; a 7.2.4 build only as a one-off A/B | the report fixes the target's ROCm; nothing else in A changes | 1 session (2–4 h) + bundle copy |
| B — bring-up 2 → 12 nodes | **keep**; add `-c 192 --exclusive`, and `rocm-smi --showclocks` + NIC affinity (`cxi<i>` per APU thread) in every log | nothing in the report pre-empts it; the clocks line prices the binning tail (§2 row 10) | 1 session (modelled walls at 25 GB/s: 10¹⁰ per node 27.5–34.1 s at 2–12 nodes; 9.16 × 10¹⁰ per node 190.9 s at 2, 323.3 s at 12) |
| C — measure the model's inputs on Slingshot | **keep, narrowed**: injection GB/s per APU (expect ≈ 25 on aac7), per-message cost, overlap at depth 1 / 2, `T_ROUND` (T11's procedure), the mapping rate at 7.0.3, the memory edge; **drop from C** nothing the report measured — it measured none of these | the report closes no T1 item; aac7 is the first fabric that can | 1–2 sessions |
| D — A/B at 8–12 nodes | **keep** `MN_GROUPS`, `MN_T_CHUNK_MB` 1024 / 2048, overlap stats, the cache, EW / PC; **change** "two SHMEM endpoints per APU" to "two contexts on one NIC" (aac7 has one NIC per APU); **new** rccl-tests all-to-all against the push kernel | the endpoint test as designed needs two NICs per APU | 1–2 sessions |
| E — scaled headline at 12 nodes at the target per-node share | **keep**: 1.099 × 10¹² digits, modelled 323.3 s without / 335.8 s with the write at 25 GB/s and 0.6 GB/s (216.8 / 253.2 s at 100 GB/s); node 445 GB (modelled) + the verification chain | unchanged by the report | 1 session |
| F — re-estimate | **keep, extend**: feed `--bw`, `--lat`, `MN_MODEL_T_ROUND`, `--hide-pow2`, `--gen-hide2` from C; add the binning tail as an explicit assumed term; note the `RNS_DIST_CACHE_PARTIAL` row's missing `--bw` / `--lat` (§5) | the report adds the latency proxy (+4 … +11 s) and the binning tail (+0 … +10 s) as bands, not inputs | 0.5 session |
| G — the target package | **keep, paused** | unchanged | — |
| **new N1** — IC-tiled transposes | single-node code test behind a switch (aac6 or aac7) | the report's 1.3× IC figure; −5 … −7 s upper bound, modelled | 1 session |
| **new N2** — RCCL vs push kernel | `rccl-tests` on aac7, 30 min | the report's 1.6 TB/s is RCCL; only a > 1.2× gap would justify a transport switch | 0.5 h |
| **new N3** — clocks and NIC affinity in the logs | a log line per run, no algorithm change | the 5–7 % APU spread is the one unmodelled term the report introduces | in B |

Phases the report makes unnecessary: **none** (it measured nothing off the node). Phases it changes: A (ROCm 7.0.3),
D (the endpoint test), F (two new bands). New: N1–N3.

## 5. Open items and model notes

- `estimate.py --target`'s standing row (the `RNS_DIST_CACHE_PARTIAL` line, 284.2 / 299.3 s) is computed by
  `mn_model.cache_partial` on `mn_model.TARGET` and **ignores `--bw` and `--lat`** (it does take `MN_MODEL_T_ROUND`
  and `--write-bw`); the sensitivities above are therefore taken on the base row and transferred as deltas. Worth a
  one-line fix in F so the row follows the CLI fabric.
- `estimate.py`'s "per NIC" column divides by 8 NICs; on aac7 (4 NICs) read it × 2.
- The report's "501 GB" node memory is unit-ambiguous (GB vs GiB); nothing in the budget depends on which.
- Items still only the target can give: the aggregate Lustre rate with 576 writers, striping and waves (T10), the
  576-node global-link taper, two NICs per APU (T8), the real per-message cost on Slingshot-2 (aac7 is Slingshot-11).
