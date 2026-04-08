
[1;37m╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗[0m
[1;37m║                         N T T   /   M I 3 0 0 A   —   I M P L E M E N T A T I O N   P L A N                          ║[0m
[1;37m╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝[0m

[1;33m  View with:  cat PLAN.md   or   less -R PLAN.md[0m

  This document tracks algorithm choices for each NTT implementation segment.
  Chosen options are marked ✓. Tables are updated as units are completed.

  Difficulty:  [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m easy   [1;32m●●[0m[1;33m●[0m[1;37m○[0m[1;37m○[0m moderate   [1;31m●●●●●[0m expert
  Seg Perf:   ×1.0 = baseline option within that segment (lower = faster).
  Overall:    estimated effect on total GPU wall-time for n=2²⁰, MI300A.
  LOC:        projected lines of new C/HIP source code for that option.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;36m  RUNTIME DISTRIBUTION  (estimated, n = 2²⁰, single NTT, MI300A)[0m
  ┌─────┬────────────────────────────────┬────────────┬────────────┬──────────────────────────────────────────┐
  │ [1;36mSeg[0m │ [1;36mSegment Name[0m                   │ [1;36mEst. Share[0m │ [1;36mBottleneck[0m │ [1;36mNotes[0m                                    │
  ├─────┼────────────────────────────────┼────────────┼────────────┼──────────────────────────────────────────┤
  │ 1   │ Core NTT butterfly algorithm   │ ~65 %      │ compute    │ Radix-2 stages; Montgomery muls dominate │
  │ 2   │ Modular arithmetic (reduction) │ ~15 %      │ compute    │ Folded into butterfly; reduction cost    │
  │ 3   │ Bit-reversal permutation       │  ~8 %      │ bandwidth  │ Bandwidth-bound; eliminated by Stockham  │
  │ 4   │ Twiddle generation & storage   │  ~5 %      │ bandwidth  │ HBM reads per butterfly; precomputed     │
  │ 5   │ Kernel dispatch & memory mgmt  │  ~4 %      │ latency    │ MI300A: unified HBM, no PCIe overhead    │
  │ 6   │ Multi-NTT batching overhead    │  ~3 %      │ latency    │ Amortized across batch; kernel reuse     │
  └─────┴────────────────────────────────┴────────────┴────────────┴──────────────────────────────────────────┘

  Note: For n=256 (ML-KEM/ML-DSA), all data fits in LDS. Butterfly share rises
  to ~85%; memory and dispatch costs fall to <5% combined.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 1 — CORE NTT BUTTERFLY ALGORITHM[0m  (≈65% runtime)

  The top-level algorithm choice determines butterfly order, memory access pattern,
  whether bit-reversal is needed, and how well the kernel maps to MI300A wavefronts.

  ┌─────────────────────────┬───────┬──────┬──────────┬──────────┬────────┬─────────────────────────────────────────────────┐
  │ [1;36mAlgorithm[0m               │ [1;36mDiff[0m  │ [1;36mLOC[0m  │ [1;36mSeg Perf[0m │ [1;36mOverall[0m  │ [1;36mMemory[0m │ [1;36mNotes[0m                                           │
  ├─────────────────────────┼───────┼──────┼──────────┼──────────┼────────┼─────────────────────────────────────────────────┤
  │ Cooley-Tukey DIT        │ [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~150 │ ×1.0     │ baseline │ O(n)   │ Standard DIT; requires bit-reversed input       │
  │ Gentleman-Sande DIF     │ [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~160 │ ×1.0     │ ~0%      │ O(n)   │ DIF; bit-reversed output; pairs with CT         │
  │ Stockham (self-sorting) │ [1;32m●[0m[1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~260 │ ×0.92    │ −5%      │ O(2n)  │ No bit-reversal; double-buffer; GPU-preferred   │
  │ Mixed-radix r=4         │ [1;32m●●[0m[1;33m●[0m[1;37m○[0m[1;37m○[0m │ ~420 │ ×0.70    │ −20%     │ O(n)   │ Fewer stages; better LDS reuse per wavefront    │
  │ Six-step FFT            │ [1;33m●●●[0m[1;31m●[0m[1;37m○[0m │ ~520 │ ×0.55    │ −29%     │ O(n)+T │ Optimal n>2¹⁸; cache-oblivious; needs transpose │
  └─────────────────────────┴───────┴──────┴──────────┴──────────┴────────┴─────────────────────────────────────────────────┘

  Recommendation: start with Cooley-Tukey DIT (lowest risk, fastest to implement
  correctly). Add Stockham as the primary GPU path once CT is verified.
  Six-step warrants investigation only after smaller-n paths are tuned.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 2 — MODULAR ARITHMETIC & REDUCTION[0m  (≈15% runtime)

  Every butterfly requires two modular multiplications. Reduction strategy directly
  sets the cost floor for the entire NTT. MI300A has no native 64-bit mod instruction.

  ┌───────────────────────────┬───────┬──────┬──────────┬──────────┬─────────────────┬──────────────────────────────────────────────────────┐
  │ [1;36mMethod[0m                    │ [1;36mDiff[0m  │ [1;36mLOC[0m  │ [1;36mSeg Perf[0m │ [1;36mOverall[0m  │ [1;36mConstraint[0m      │ [1;36mNotes[0m                                                │
  ├───────────────────────────┼───────┼──────┼──────────┼──────────┼─────────────────┼──────────────────────────────────────────────────────┤
  │ Lazy / deferred reduction │ [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~40  │ ×1.0     │ baseline │ 64-bit headroom │ Reduce every 2–3 stages; simplest correct impl       │
  │ Barrett reduction         │ [1;32m●[0m[1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~70  │ ×0.82    │ −2.7%    │ any q < 2³²     │ Precomputed reciprocal; no conversion needed         │
  │ Montgomery multiplication │ [1;32m●[0m[1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~85  │ ×0.75    │ −3.75%   │ any odd q       │ Best for sustained chains; twiddles pre-converted    │
  │ Montgomery + lazy combo   │ [1;32m●●[0m[1;33m●[0m[1;37m○[0m[1;37m○[0m │ ~130 │ ×0.60    │ −6%      │ any odd q       │ Montgomery in body, lazy accumulation between stages │
  │ Plantard reduction        │ [1;33m●●●[0m[1;31m●[0m[1;37m○[0m │ ~105 │ ×0.65    │ −5.25%   │ q < 2³¹         │ 64→32 narrowing trick; faster on some pipelines      │
  └───────────────────────────┴───────┴──────┴──────────┴──────────┴─────────────────┴──────────────────────────────────────────────────────┘

  Recommendation: implement lazy reduction first (correctness baseline), then
  Montgomery. ML-KEM q=3329 and ML-DSA q=8380417 both work with Montgomery.
  Plantard worth benchmarking against Montgomery on actual MI300A hardware.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 3 — BIT-REVERSAL PERMUTATION[0m  (≈8% runtime)

  Required by Cooley-Tukey DIT (input) and Gentleman-Sande DIF (output).
  Eliminated entirely if Stockham is chosen for Segment 1.

  ┌──────────────────────────────┬───────┬─────┬──────────┬──────────┬──────────────────────────────────────────┐
  │ [1;36mMethod[0m                       │ [1;36mDiff[0m  │ [1;36mLOC[0m │ [1;36mSeg Perf[0m │ [1;36mOverall[0m  │ [1;36mNotes[0m                                    │
  ├──────────────────────────────┼───────┼─────┼──────────┼──────────┼──────────────────────────────────────────┤
  │ Eliminated (Stockham)        │ N/A   │   0 │ —        │ −8%      │ Only if Seg 1 = Stockham; best outcome   │
  │ Separate kernel (index swap) │ [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~50 │ ×1.0     │ baseline │ Simple; one read + one write per element │
  │ Precomputed index LUT        │ [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~65 │ ×0.90    │ −0.8%    │ LUT eliminates bit-twiddling in hot loop │
  │ Schatzman in-place           │ [1;32m●[0m[1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~85 │ ×0.82    │ −1.4%    │ Cache-friendlier; avoids LUT memory      │
  └──────────────────────────────┴───────┴─────┴──────────┴──────────┴──────────────────────────────────────────┘

  Recommendation: implement the simple separate kernel alongside CT. If Stockham
  is adopted, this segment is retired entirely.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 4 — TWIDDLE FACTOR GENERATION & STORAGE[0m  (≈5% runtime)

  Twiddle factors ω^k must be in Montgomery form for the Montgomery butterfly.
  MI300A HBM3 (900 GB/s) makes precomputed tables cheap to read.

  ┌───────────────────────────────┬───────┬──────┬──────────┬──────────┬─────────────┬────────────────────────────────────────────────┐
  │ [1;36mMethod[0m                        │ [1;36mDiff[0m  │ [1;36mLOC[0m  │ [1;36mSeg Perf[0m │ [1;36mOverall[0m  │ [1;36mHBM use[0m     │ [1;36mNotes[0m                                          │
  ├───────────────────────────────┼───────┼──────┼──────────┼──────────┼─────────────┼────────────────────────────────────────────────┤
  │ Precompute all ω^k, store HBM │ [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~80  │ ×1.0     │ baseline │ O(n) words  │ Best bandwidth; one read per butterfly         │
  │ Partial table + recurrence    │ [1;32m●[0m[1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~120 │ ×1.08    │ +0.4%    │ O(√n) words │ Recompute within stage using recurrence        │
  │ LDS-cached (small n ≤ 2¹⁴)    │ [1;32m●[0m[1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~110 │ ×0.70    │ −1.5%    │ LDS only    │ All twiddles fit in shared mem; zero HBM reads │
  │ On-the-fly (repeated sqr)     │ [1;32m●●[0m[1;33m●[0m[1;37m○[0m[1;37m○[0m │ ~65  │ ×1.40    │ +2%      │ O(1)        │ Minimal HBM; compute-bound; rarely worth it    │
  └───────────────────────────────┴───────┴──────┴──────────┴──────────┴─────────────┴────────────────────────────────────────────────┘

  Recommendation: precompute all ω^k in HBM for general n; switch to LDS-cached
  for the n=256 ML-KEM/ML-DSA path. Both tables must be in Montgomery form.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 5 — KERNEL DISPATCH & MEMORY MANAGEMENT[0m  (≈4% runtime)

  MI300A is an APU: host and device share HBM3. There is no PCIe copy.
  hipMalloc allocates from the same unified pool as malloc; pointers are shared.

  ┌─────────────────────────────────┬───────┬──────┬──────────┬─────────────┬───────────────────────────────────────────┐
  │ [1;36mMethod[0m                          │ [1;36mDiff[0m  │ [1;36mLOC[0m  │ [1;36mSeg Perf[0m │ [1;36mOverall[0m     │ [1;36mNotes[0m                                     │
  ├─────────────────────────────────┼───────┼──────┼──────────┼─────────────┼───────────────────────────────────────────┤
  │ One kernel launch per NTT stage │ [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~80  │ ×1.0     │ baseline    │ Simple; barrier between stages = sync     │
  │ Fused multi-stage kernel (LDS)  │ [1;32m●●[0m[1;33m●[0m[1;37m○[0m[1;37m○[0m │ ~220 │ ×0.60    │ −1.6%       │ Tile log₂(LDS_elems) stages per launch    │
  │ Persistent grid (single launch) │ [1;33m●●●[0m[1;31m●[0m[1;37m○[0m │ ~320 │ ×0.50    │ −2%         │ One launch total; sync via atomics in HBM │
  │ MI300A unified ptr (zero-copy)  │ [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~30  │ —        │ −4% vs dGPU │ APU: allocate once; no hipMemcpy needed   │
  └─────────────────────────────────┴───────┴──────┴──────────┴─────────────┴───────────────────────────────────────────┘

  Recommendation: begin with one kernel per stage. Implement fused multi-stage
  once stage-level kernels are verified. Exploit unified memory from day one —
  use hipHostMalloc with hipHostMallocDefault; pointer is valid on both sides.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 6 — MULTI-NTT BATCHING[0m  (≈3% overhead amortized)

  ML-KEM requires 256-point NTTs; ML-DSA likewise. Batching many small NTTs
  is critical for throughput: a single n=256 NTT uses <1% of MI300A capacity.

  ┌──────────────────────────────────┬───────┬──────┬──────────┬──────────┬──────────┬─────────────────────────────────────────────────┐
  │ [1;36mMethod[0m                           │ [1;36mDiff[0m  │ [1;36mLOC[0m  │ [1;36mSeg Perf[0m │ [1;36mOverall[0m  │ [1;36mTarget n[0m │ [1;36mNotes[0m                                           │
  ├──────────────────────────────────┼───────┼──────┼──────────┼──────────┼──────────┼─────────────────────────────────────────────────┤
  │ Serial: one NTT per launch       │ [1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~10  │ ×1.0     │ baseline │ any      │ No batching; correct reference baseline         │
  │ Grid-stride batch (one kernel)   │ [1;32m●[0m[1;32m●[0m[1;37m○[0m[1;37m○[0m[1;37m○[0m │ ~55  │ ×0.55    │ −1.4%    │ any      │ All NTTs in one launch; hides latency           │
  │ LDS-batched (small n in shmem)   │ [1;32m●●[0m[1;33m●[0m[1;37m○[0m[1;37m○[0m │ ~160 │ ×0.20    │ −2.4%    │ n ≤ 2¹⁴  │ Pack many NTTs per CU; zero HBM spill           │
  │ Warp-level (1 NTT per wavefront) │ [1;33m●●●[0m[1;31m●[0m[1;37m○[0m │ ~210 │ ×0.15    │ −2.55%   │ n = 256  │ wavefront=64 fits half of n=256; optimal ML-KEM │
  └──────────────────────────────────┴───────┴──────┴──────────┴──────────┴──────────┴─────────────────────────────────────────────────┘

  Recommendation: implement grid-stride batching from the start — it subsumes
  the serial case at n_batch=1. Warp-level batching for n=256 is the highest-
  value optimization for PQC workloads and should be a named development target.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;36m  OVERALL ASSESSMENT[0m

  Status: infrastructure complete; no source files written.

  Optimal algorithm stack (aggressive, all segments at best option):
  ┌─────┬─────────────────────────────────────────────┬──────────────────┐
  │ [1;36mSeg[0m │ [1;36mChoice[0m                                      │ [1;36mCombined gain[0m    │
  ├─────┼─────────────────────────────────────────────┼──────────────────┤
  │ 1   │ Six-step FFT (large n) / Stockham (small n) │ −29% / −5%       │
  │ 2   │ Montgomery + lazy combo                     │ −6%              │
  │ 3   │ Eliminated (via Stockham)                   │ −8%              │
  │ 4   │ Precomputed HBM + LDS for n=256             │ −1.5%            │
  │ 5   │ Fused multi-stage + MI300A unified ptr      │ −2%              │
  │ 6   │ Warp-level for n=256, grid-stride otherwise │ −2.55%           │
  │ —   │ Total estimated vs. naive baseline          │ ≈ −49% wall-time │
  └─────┴─────────────────────────────────────────────┴──────────────────┘

  Conservative first-milestone stack (Phase 2 target):
    Seg 1: Cooley-Tukey DIT   Seg 2: lazy reduction   Seg 3: separate kernel
    Seg 4: precomputed HBM    Seg 5: per-stage launch  Seg 6: grid-stride batch
  This is correct, buildable, and measurable — the right foundation.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m
