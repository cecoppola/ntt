# TGTBENCH2 — the user's target-system test summary (raw, as received 2026-10-06 ≈18:40 EDT)

Provenance: tests run by the user on the target system and pasted into the session as `tests.md`. Kept verbatim below;
the assessment against the ecalc design is in results/TGTBENCH2.md. Numbers here are the user's, labelled as given.

---

Breakdown: Compute 250.0 s (99.4%), Communication 1.0 s (0.4%), Init 0.5 s (0.2%). Confidence: HIGH (critical path validated C1 C3 NTT A6 within 20). Uncertainty: factor 6.3 ±3.6% (86x improvement). Conservative injection 25 GB/s adds 0.4% model uncertainty (A3 unmeasured).

#### M2 Updated constants

* **DEVICE_MEM**: 93.36 GB (-21%)
* **VMM_MAP_RATE**: 0.010 s/GB (7x faster)
* **DEVICE_MEM_PER_APU**: 93.36 GB (was 117.975 GB assumption, measured 21% lower, 0% variance).
* **HOST_MEM_PER_APU**: 256.0 GB (measured hipHostMalloc limit).
* **VMM_MAP_RATE**: 0.010 s/GB (was 0.070 s/GB aac7, 7x faster, init $6.9\text{s} \rightarrow 1.0\text{s}$ impact).
* **PROCESS_OVERHEAD_1PE**: 28.0 GB (was 86.0 GB modelled, 3x lower).
* **NTT_THROUGHPUT_PER_APU**: 655.21 GB/s (measured $n=20$, was estimate).
* **TRANSPOSE_THROUGHPUT_PER_APU**: 2601.54 GB/s (measured $n=20$).
* **INJECTION_BW_PER_APU**: 25.0 GB/s (conservative, A3 unmeasured).
* **XGMI_EFFICIENCY**: 1.0 (all direct links, no multi-hop penalty).

#### M3 Memory constraint

* **V1 / V2**: 265.4 TB required vs 210.1 TB device (26% oversubscribed)
* **Layout requirement**: 421.5 GB device per node (C layout: planes 120.9 GB + arena 300.7 GB). Available: 93.36 GB/APU x 4 x 576 nodes = 210.1 TB device total. Required: 421.5 GB/node x 576 = 265.4 TB.
* **Oversubscription**: 26% (55.3 TB deficit).
* **Mitigation**: Host spill (`hipMallocManaged`) enables VMM to map device+host.
* **Overhead**: 1.0s init (0.4% of 251.5s runtime, acceptable).
* **Timeline**: 1-2 weeks implementation.

#### Smoke tests

* **3/3 PASS** (C1 C3 A6 within 1-20 of established baselines)
* **C1 baseline**: Within 10 of $n=10$ mean (0.801s).
* **C3 NTT 65536**: Within 20 of baseline (607.9-616.8 GB/s vs 655.21 GB/s mean).
* **A6 device memory**: 93.36 GB (0% variance, perfect stability).

#### Regression detection

* **21/28 PASS (75%) | 6 WARN (21%) | 1 FAIL (4%)**
* **PASS (21)**: C1 baseline, C3 NTT, A6 device, B6 barriers, B9 collectives.
* **WARN (6)**: C3 transpose (thermal), A6 VMM variance, D5 atomics favorable.
* **FAIL (1)**: B1 4PE anomaly (isolated, not blocking).
* **Thresholds**: PASS within ±10% or improved, WARN 10-20% deviation, FAIL >20% deviation.

#### V3 Data integrity

* **309/309 CSV files validated** (100% parseable, 0 corrupt)
* **Parseable**: 100% (309/309).
* **Empty**: 27 files (expected blocked multi-node tests, timeouts).
* **Truncated**: 0.
* **Corrupt**: 0.
* **Quality checks**: Row counts match expected test matrices. No negative bandwidth, no zero values where non-zero expected, timestamp ordering preserved.

#### V4 Cross-validation

* **V5 Model predictions within CI bounds correlations $r > 0.97$**
* **576-node runtime**: 251.55 ±3.6% validated (all components within measured ranges).
* **Compute 250.0s**: Based on C3 $n=20$ statistics (NTT 655 GB/s, Transpose 2602 GB/s within CI bounds).
* **Communication 1.0s**: Conservative 25 GB/s injection (A3 unmeasured, 0.4% uncertainty).
* **Init 0.5s**: VMM 0.010 s/GB x 98.6 GB host spill.
* **Correlations**: C3 NTT $r=0.9987$, Transpose $r=0.9791$ (excellent).
* **Scaling predictions**: B6 1.46x within 20% of 1.6x lower bound (favorable).

#### Production readiness

* **GO** (critical path validated, memory mitigation identified)
* **Critical path**: 100% validated (C1 C3 NTT A6 within 20).
* **Statistical confidence**: HIGH ($n=5$ to $n=20$ validation, CV <10% for kernels).
* **Model uncertainty**: 86x reduction.
* **Deployment blockers**: None (memory mitigation identified and scoped, 1-2 weeks timeline).

#### Issue Log / Action Items

* **B1 Memory constraint (CRIT)**: 26% oversubscribed host spill mitigation identified (1-2wk, H priority).
* *Issue*: 265.4 TB required vs 210.1 TB device available. Cannot fit layout in device memory at 576 nodes.
* *Mitigation*: Host spill using `hipMallocManaged` (VMM device+host mapping). Overhead: 1.0s init (0.4% of 251.5s runtime, acceptable). Timeline: 1-2 weeks implementation. Status: Mitigation identified and scoped, not blocking with timeline commitment.

* **B2 Multi-node access (gate)**: A4 @ 64n CRITICAL 6 tests blocked (29% of total) (2-8wk, H priority).
* *Blocked tests*: A3 A4 A5 A7 B5 B7 D1 D4.
* *Impact*: A3 fabric injection unmeasured (0.4% model uncertainty, conservative 25 GB/s used). A4 scaling @ 64n: Fall-off risk unmeasured (aac7 10x fabric-bound scenario possible).
* *Escalation*: Request 2-576 node allocation. Timeline: 2-8 weeks. Criticality: A4 @ 64n is CRITICAL gate for production deployment confidence.

* **B3 B1 4PE anomaly (invest)**: 1.00x vs 4.00x expected (75% deviation) (1-2wk, M priority).
* *Issue*: Measured 35.4 GB @ 4PE vs 172.9 GB @ 2PE (1.00x scaling vs 4.00x linear expected).
* *Impact*: Blocks production 4-PE deployment pending investigation.
* *Workaround*: Use 2-PE configuration (validated 6.2x scaling, 1.46x sub-linear acceptable for barriers/collectives). Timeline: 1-2 weeks analysis. Criticality: Not blocking (not on critical path, 2-PE workaround validated).

#### Detailed Findings

* **D1 LIBSMA launcher method**: Root cause: Not kernel modules, launcher choice (`oshrun` vs `ntasks`).
* *Solution*: Slurm-native multi-task execution (`ntasks` parameter) vs external SHMEM launcher. Impact: 100% success rate, 7 tests unblocked immediately. Scaling rule: `FI_UNIVERSE_SIZE` $\ge$ 4 x `ntasks`.

* **D2 Transport asymmetry**: SDMA 0.4% asym (50-58 GB/s) vs SHMEM 21x asym (1.8-38 GB/s) (H priority).
* *Quantification*: SDMA uniform across all 12 GPU pairs (optimal for XGMI). SHMEM avoid for XGMI (use only for multi-node fabric). Decision: 3x throughput gain by using SDMA for all cross-GPU.

* **D3 Memory constraint ID**: 26% oversubscribed (first quantified this session).
* *Identification*: 265.4 TB required vs 210.1 TB device available. Mitigation: Host spill (`hipMallocManaged`) with 0.4% overhead. Timeline: 1-2 weeks implementation (not blocking, scoped).

* **D4 VMM map rate improvement**: 0.010 s/GB (7x faster than 0.070 s/GB aac7 estimate) (H priority).
* *Impact*: Init overhead reduced from 6.9s to 1.0s @ 576 nodes. Uncertainty reduction: 86x improvement in runtime model confidence ($\pm\text{factor } 6.3 \rightarrow \pm3.6\%$).

* **D5 C3 NTT 4096 bimodal dist**: Fast 31.8 GB/s (28.3%) Slow 15.3 GB/s (66.7%) (M priority).
* *Root cause*: Cache boundary @ 32 KB (L1 capacity limit). Mitigation: Warm-up (skip first 2-3 iterations). Implementation: Regression thresholds with bimodal detection (v2.2 enhancement).

* **D6 XGMI topology validation**: Fully-connected mesh all 6 GPU pairs direct links (weight=15) (H priority).
* *Consistency*: 100% match between B8 topology (`rocm-smi`) and A8 SDMA measurements (50-58 GB/s uniform). No multi-hop paths: 0% degradation from topology. Validates model assumption.

* **L1 Root cause identified**: `fi_enable(-28)` NOT kernel module issue, launcher methodology (Priority: H).
* Error "No space left on device" on all 19 multi-PE tests. Initial hypothesis: missing `cxi_core` module, endpoint limits. ACTUAL: `oshrun` launcher not available in MCP sandbox. Solution: Use Slurm-native `ntasks` parameter instead of external SHMEM launcher (`oshrun`/`mpiexec`).

* **L2 Environment tuning**: `SHMEM_SYMMETRIC_SIZE=512M`, `FI_UNIVERSE_SIZE=4` (Priority: H).
* Scaling rule discovered: `FI_UNIVERSE_SIZE` $\ge$ 4 x `ntasks` for multi-PE stability. Baseline config works perfectly (no special env vars required). Optional: `FI_LOG_LEVEL=warn` (reduce 1000x output).

* **L3 Validation results**: 12/12 configurations 100% success | 2-PE barrier 4.457 $\mu$s (Priority: V.HIGH, H).
* Tested: `FI_UNIVERSE_SIZE` 512/1024/2048/4096, `FI_CXI_DEFAULT_CQ_SIZE` 65536/131072/262144, combined, `disable-host-register`, RX-match hybrid/software. All passed. 4-PE XGMI topology running successfully (all 6 GPU pairs validated).

* **L4 Impact**: Unblocked 7 tests (B2 B6 B8 B9 D5 A8 partial-A3) (Priority: V.HIGH, H).
* Coverage improvement: 57% $\rightarrow$ 71% (7 tests enabled immediately). Multi-PE patterns validated: 2 PE intra-node, 4 PE full-node. Sequential HIP init pattern (100ms stagger per PE) prevents endpoint exhaustion. 100% success rate sustained across all Phase 2-3 multi-PE execution.

* **A6 Memory edge extended**: Device 93.36 GB/APU | Host 256 GB/APU | VMM 0.010 s/GB (Priority: H).
* $n=5$ validation. Device variance 0.0% (perfect stability across all runs, 373.44 GB/node total). VMM map rate 0.010 s/GB (was 0.070 s/GB aac7 estimate, 7x faster). Process overhead 28.0 GB @ 1 PE (was 86.0 GB modelled, 3x lower). Impact: Init overhead $6.9\text{s} \rightarrow 1.0\text{s}$ @ 576 nodes. Memory constraint identified: 265.4 TB required vs 210.1 TB device (26% oversubscribed).

* **A8 Bandwidth matrix**: SDMA 50-58 GB/s (0.4% asym) | SHMEM 1.8-38 GB/s (21x asym) (Priority: HIGH, H, H).
* Transport comparison across 12 GPU pairs (all directed paths in 4-GPU system). SDMA optimal for XGMI: uniform 50-58 GB/s, 0.4% asymmetry (negligible). SHMEM avoid for XGMI: 1.8-38 GB/s range, 21x max/min asymmetry. HIP implementation bug (measuring local copy not xGMI). Decision: Use SDMA (`hipMemcpy`) for all cross-GPU 3x throughput gain immediate ROI.

* **B1 Process memory multi-PE**: 1PE 28.0 GB | 2PE 172.9 GB (6.2x) | 4PE 35.4 GB (1.00 ANOM) (Priority: MOD, M).
* Scaling anomaly: 4 PE measured 1.00x vs 4.00x expected (75% deviation). Investigation required before production 4-PE deployment. Hypothesis: SHMEM memory model depends on PE configuration. Workaround validated: Use 2-PE configuration (6.2x scaling acceptable, sub-linear favorable).

* **B2 Device heap GPU-Direct**: 16MB +62% device-direct | 64MB+ -25% host-staged (Priority: HIGH, M).
* Small messages (16 MB): Device-direct advantage +62% (latency-sensitive). Large messages (64 MB+): Host-staged advantage -25% (device-direct slower, throughput). Crossover 16-64 MB identified for algorithm tuning (message-size dependent transport selection).

* **B6 Barrier multi-PE**: 2PE 3.89 $\mu$s | 4PE 5.67 $\mu$s (1.46x sub-linear scaling) (Priority: HIGH, H).
* Performance excellent (<5 $\mu$s target @ 2PE). 4PE scaling 1.46x vs 1.6-2.0x expected (tree depth), favorable (within 20% of lower bound). 160 sync points on critical path @ 5.67 $\mu\text{s} = 0.9\text{ ms}$ total (noise level, no mitigation required).

* **B8 XGMI topology mapping**: Fully-connected mesh all direct links (weight=15) (Priority: HIGH, H).
* Method: `rocm-smi` introspection (benchmark 1 measurement/hour, too slow). Result: 6 unique GPU pairs all direct XGMI links, no multi-hop paths (0% degradation from topology). Symmetry 100% (all pairs identical). Consistency: 100% match with A8 SDMA measurements (50-58 GB/s uniform).

* **B9 SHMEM collectives scaling**: 2PE 4PE: alltoall 1.46x | allgather 2.46x | allreduce 1.8x (Priority: HIGH, M).
* Message-size dependent: <16 KB alltoall fastest (4-10 $\mu$s latency-optimized), >64 KB allgather optimal (bandwidth-bound). Sub-linear scaling favorable for alltoall (better than 2x expected), near-linear for allgather. Design guideline: small messages use alltoall, large use allgather.

* **B4 Synchronization**: 1 PE baseline measured (Priority: MOD).
* **C1 Single-node baseline**: $n=10$: $\mu=80.801$, $\sigma=5.0\%$ CV @ $10^{10}$ digits (Priority: V.HIGH, M).
* Per-APU variance $14.3\% \rightarrow 5.8\%$ (statistical validation improved). 95% CI established. Production-ready quality (CV <10% threshold). Validates local-factor 1.22 for ROCm 7.0.3 vs 7.2.4.

* **C2 Sustained thermal**: $1^\circ\text{C}$ rise @300s no throttling clocks stable (Priority: HIGH, H).
* Duration: 300s continuous NTT+Transpose full load. Temperature rise $1^\circ\text{C}$ under sustained load. No throttling events detected. Clocks stable throughout. Projection: 5-minute headline (251.5s @ 576 nodes) thermally safe with margin.

* **C3 NTT/Transpose kernels**: $n=20$: NTT 655±7 GB/s (CV 2.34%) | Transpose 2602±43 GB/s (3.41%) (Priority: V.HIGH, H).
* Production-ready: 90% pass CV <10% threshold. NTT @ 65536: 655.21 GB/s ±2.34% (excellent). Transpose @ 4096: 2601.54 GB/s ±3.41% (excellent). 576-node projections: NTT 1.51 PB/s ±1.1%, Transpose 5.99 PB/s ±1.7%. Statistical correlations: NTT $r=0.9987$ ($p<0.001$), Transpose $r=0.9791$ ($p<0.01$). Bimodal distribution discovered @ NTT 4096: extended to $n=60$ for investigation.

* **C3a NTT 4096 bimodal analysis**: $n=60$: Fast 31.8 GB/s (28.3%) Slow 15.3 GB/s (66.7%) (Priority: MOD).
* Root cause: Cache boundary @ 32 KB (L1 capacity limit). Fast mode 31.8 GB/s (28.3% of runs), Slow mode 15.3 GB/s (66.7% of runs). CV 19.67% (flagged in $n=20$, extended for investigation). Mitigation: Warm-up recommended (skip first 2-3 iterations). Implementation: Regression thresholds with bimodal detection (v2.2 enhancement).

* **D5 Atomic operations**: 2PE 4PE CAS penalty 4.07x (super-linear contention confirmed) (Priority: HIGH).
* Measured: 4.07x contention penalty @ 4 PE. Expected: 12-18x super-linear (theoretical worst-case). Status: Lower than expected (favorable but unexpected, requires investigation). Design guideline: Spread CAS operations across $\ge 1024$ locations to mitigate contention. Not blocking (favorable).

* **D2 Memory footprint**: 28.0 GB @ 1 PE (measured via B1) (Priority: MOD).
* **D3 Baseline stability**: 5.0% per-APU variance (measured via C1 $n=10$) (Priority: V.HIGH).
* **M1 576-node runtime estimate**: $251.5 \pm 3.6\%$ s (4.2 min ±9s) 86x uncertainty reduction (Priority: M, V.HIGH, H, M).
