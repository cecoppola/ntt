# ALGORITHM.md — master guide

Computing **e** to the maximum number of decimal digits on one node of
4 × AMD Instinct MI300A.

**How to read this.** Every figure is marked:

| mark | meaning |
|---|---|
| **✔** | measured on this node — see `RESULTS.md` for the run |
| *est.* | estimated from measured primitives or from the literature |
| — | not applicable |

`M(N)` is the cost of an N-bit multiply, ≈ O(N log N) here.
`W` is the size of the final number in bytes = 0.4152 bytes per decimal digit.
Speedups are against the **baseline** row of each segment, which is the
simplest correct implementation, not the worst imaginable one.

Companion documents: `DESIGN.md` (why the architecture is what it is),
`RESULTS.md` (the measurement campaigns), `bench/` (13 programs, all C).

---

## Part 0 — the algorithm at a glance

```
  e = 1 + P/Q,   P/Q = sum_{k>=1} 1/k!   via 2-variable binary splitting

  PHASE 1  binary splitting          -> P, Q          ~57% of run   est.
  PHASE 2  Newton division P/Q       -> e in binary   ~9%           est.
  PHASE 3  scaled remainder tree     -> decimal       ~34%          est.

  all three phases rest on ONE primitive: a big-integer multiply
```

Cost split **inside** one multiply, measured on 1 APU at L = 2048², 2 primes
(`bench/13`, ✔):

| component | share |
|---|---:|
| NTT transforms (4 forward + 2 inverse per prime) | **54 %** |
| intra-APU transposes | 16 % |
| four-step twiddle | 14 % |
| operand split | 10 % |
| pointwise product | 5 % |

The three governing facts, all measured:

1. **Memory capacity sets the digit count; time does not.** 460 GiB claimable ✔;
   the arithmetic for 10¹¹ digits is minutes against an 8-hour cap.
2. **The fabric is not the bottleneck.** Only level 0 of the recursion must be
   distributed; a push corner turn runs at 909 GB/s ✔ and overlaps compute at
   74 % ✔.
3. **The butterfly is issue-bound**, 28 of 34.7 VALU instructions irreducible ✔.
   Every remaining kernel trick buys speed with density — the wrong currency.

---

## Part 1 — segments

### S1. Series and recursion

Baseline: 3-variable `CommonP2B3` recursion, the general hypergeometric form.

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---|---|
| 3-var CommonP2B3 | 120 | O(M(N)·log²N) | 1.00× | 3 vars | General form; what π/Chudnovsky needs. Unnecessarily general for e. |
| **2-var hyperdescent** ✅ | **60** | **O(M(N)·log N)** | **~log N** | **2 vars** | `P(a,b)=P(a,m)Q(m,b)+P(m,b)`, `Q(a,b)=Q(a,m)Q(m,b)`. R(k)≡1 makes the series superlinearly convergent. **This is why e is the right target for a memory-limited node.** |
| accelerated e-series | +40 | same | 1.2–1.4× *est.* | same | Group terms, e.g. Σ(2k+2)/(2k+1)!, halving the term count. Worth a look; not on the critical path. |
| + GCD factorisation | +250 | same | 1.0–1.1× *est.* | +prime factorisations | Remove common factors across distant k. Yee reports 20–30 % for π; e's Q(a,b)=b!/a! has far less to remove. **Low value here.** |

At D = 10¹¹, n ≈ 5.4 × 10⁹ terms, recursion depth ≈ 33.

### S2. Binary-splitting driver

Baseline: split at the midpoint index, single-threaded recursion.

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| midpoint split | 60 | — | 1.00× | — | Terms grow with k, so halves are unbalanced (Yee: 215 vs 311 digits for 100!). Wastes work *and* unbalances the APUs. |
| **size-balanced split** | **+80** | +O(log n) per node | **~1.3×** *est.* | — | Choose m by binary search on a Stirling-based `size()` so both children produce equal-sized results. Minimises total multiply work because M is superlinear, and makes the fork/join heap split exact. |
| + task-parallel dispatch | +90 | — | **~4×** on levels ≥2 | ×4 live subtrees | Levels ≥2 have ≥4 independent merges; one per APU, no fabric traffic at all (see S16). |
| + over-decomposition | +30 | — | 1.0–1.05× *est.* | — | 2–4× more tasks than devices to fill scheduling gaps. Yee reports >99 % utilisation. |

### S3. Parameter selection — primes, k, b, L

Baseline: one 62-bit prime, power-of-two transform length.

| option | LOC | — | density | memory | description |
|---|---:|---|---:|---:|---|
| k=1, 62-bit | — | — | 1.75 bits/byte | 4.6× expansion | Convolution range forces b ≤ 14. Wasteful. |
| **k=2, 62-bit** ✅ ✔ | **20** | — | **2.81 bits/byte** | **2.85×** | `1 + log₂L + 2b ≤ k·w` gives b = 45 at L = 2³³. Optimal: k≥3 is capped by b < p and falls to 2.58. |
| k=3, 31-bit | 20 | — | 2.50 bits/byte | 3.2× | **1.37× better compute** ✔ but 11 % worse memory. Rejected — memory binds. |
| k=3, 50-bit FP64 | 20 | — | 2.13 bits/byte | 3.8× | Needed if the FP64 butterfly were used. 24 % worse than the integer path. |
| L = 2ᵏ only | — | — | — | up to 2× waste | The "jump" the TFT literature exists to fix. |
| **7-smooth L + tunable b** ✅ | **+30** | — | — | **≤5 % waste** | Primes chosen with 2⁴⁰·3·5·7 \| p−1 so L = 2^a3^b5^c7^d. Two continuous knobs (L and b) size the transform to the operand. **Makes a truncated Fourier transform unnecessary.** |

Chosen primes: `p1 = 39943·105·2⁴⁰+1`, `p2 = 39922·105·2⁴⁰+1`, both primitive root 11.

### S4. Modular arithmetic primitive (the butterfly)

Baseline: Montgomery butterfly, 62-bit. All rows measured ✔ (`bench/01`).

| option | LOC | cyc/butterfly ✔ | Gbit-bfly/s ✔ | speedup | description |
|---|---:|---:|---:|---:|---|
| Montgomery, p<2⁶² | 12 | 25.5 (modmul) | 193 038 | 1.00× | Needs both halves of the product plus the reduction. |
| Goldilocks 2⁶⁴−2³²+1 | 14 | — | 173 653 | 0.90× | Cheap Solinas reduction, but no precomputed quotient. |
| FP64 Shoup, p<2⁵⁰ | 16 | — | 210 602 | 1.09× | **Most butterflies/s of any option** (4 212 vs 3 886 Gbfly/s) and still loses: 19 % fewer bits each, and needs 3 primes. |
| **Shoup / Harvey Alg.4** ✅ ✔ | **10** | **15.2 (modmul), 27.7 (bfly)** | **240 949** | **1.25×** | Precomputed `w' = ⌊w·2⁶⁴/p⌋`; lazy [0,4p) representation removes the correction step. 28 VALU instructions at 88 % issue efficiency ✔. |
| deferred lazy reduction | +15 | 22.8 *est.* | — | rejected | Needs p<2⁵⁹ → 6.7 % density loss for 10.4 % speed. **Rejected: wrong currency.** Also impossible for the inverse (Gentleman-Sande doubles the range per stage). |
| int8 matrix-core (MFMA) | ~400 | — | — | rejected | 8× the arithmetic as a dense matmul × 64 digit-products per modmul ≈ 2× net, on a component that is 54 % of the multiply → ≤12 % overall. **Optimises the resource in surplus.** |

### S5. Operand split (packed limbs → b-bit points mod p)

Baseline: standalone kernel, one pass.

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| standalone pass | 18 | O(L) | 1.00× | +1 plane | Bit-extract from the limb array, write a plane. |
| **fused into fwd1 prologue** ✅ ✔ | **20** | O(L) | **saves 1 plane-pass** | 0 | Points are stored **column-major** (offset `r·N2+c` holds point `c·N1+r`) because Bailey's first transform must run over the stride-n₂ index. Reads are one cache line per lane, but the packed operand fits the 256 MB Infinity Cache, so it costs only 0.032 ms ✔ — cheaper than a coalesced pass plus a transpose. |

### S6. Transform kernel (forward DIT / inverse Gentleman-Sande)

Baseline: simple LDS radix-2, one read-modify-write and one barrier per stage.
All measured ✔ (`bench/04`–`07`, `bench/12`), node totals at N = 2048.

| option | LOC | complexity | fwd Gbfly/s ✔ | inv Gbfly/s ✔ | speedup | LDS | description |
|---|---:|---|---:|---:|---:|---:|---|
| simple LDS radix-2 | 25 | O(L log L) | 1 554 | 1 522 | 1.00× | 16 KiB | 11 LDS round trips, 11 barriers. 40 % of the register-resident bound. |
| **register-blocked** ✅ | **60** | same | 2 211 | — | 1.42× | 16 KiB | 8 points/thread makes each group of 3 stages register-resident; LDS becomes a transpose buffer only — 3 round trips, 3 barriers. |
| **+ XOR swizzle** ✅ | **+5** | same | **2 325** | **2 569** | **1.50 / 1.69×** | 16 KiB | Exchange phases have 16- and 32-way bank conflicts (class = e mod 16); `e ^ ((e>>4)&15)` restores the floor. Also cuts APU spread 12.4 %→1.8 %. |
| + interleaved ulong2 twiddle | +8 | same | +6 % | — | 1.06× | — | One 16-byte load instead of two 8-byte. Twiddle traffic is 108 KiB per 16 KiB of data. |
| non-temporal data loads | +4 | same | **0.66×** | — | rejected | — | Defeats coalescing on the strided 8-per-thread pattern ✔. The node report's 1.10× does not transfer. |
| LDS-staged twiddle table | +20 | same | 0.96× | — | rejected | 24 KiB | Occupancy loss (2 blocks/CU) exceeds the L1 saving ✔. |
| cross-lane shuffle exchange | ~120 | same | — | — | rejected | 0 | A 64-bit `__shfl_xor` costs 12.18 cyc = exactly an LDS write+read pair ✔, and a register transpose needs several rounds. Sugizaki's "eliminate SMEM" result does not transfer to CDNA3. |
| trivial-twiddle specialisation | +10 | same | **0.967× — rejected** ✔ | — | — | — | `table[m+0] = root⁰ = 1`, so 7 of 44 butterflies in group A are pure add/sub. Removing them cuts **1510 → 1364 VALU instructions** ✔ at identical VGPR count — and the kernel gets **3.3 % slower** ✔. The removed arithmetic was hiding twiddle-load latency. See Part 5, R9. |

N = 2048 with 256 threads is the largest sub-transform that keeps 16 waves/CU
(4 blocks × 16 KiB = 64 KiB LDS), which the node report identifies as the
bandwidth optimum.

### S7. Four-step twiddle

Baseline: full L-entry table, general modular multiply.

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| L-entry table | 10 | O(L) | 1.00× | **+1 plane (~12 % of budget)** | Simple and unaffordable. |
| on-the-fly `powmod` | 12 | O(L log L) | 0.1× *est.* | 0 | Correct, far too slow. |
| **two-level table + 2 Shoup muls** ✅ ✔ | **14** | O(L) | **~1×, −1 plane** | **2√L entries** | `m = r·brv(s)` splits as `q·N2+rr`, so `ω^m = t1[q]·t2[rr]` — two Shoup multiplies, no general modmul, no 128-bit divide. |
| **+ fused into transform epilogue** ✅ ✔ | **+6** | — | **saves 1 plane-pass** | 0 | Applied in `fwd1`'s epilogue and `inv2`'s prologue. Costs 0.029 ms ✔. |

### S8. Transpose (intra-APU)

Baseline: out-of-place LDS-tiled transpose.

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| out-of-place tiled | 15 | O(L) | 1.00× | **6 planes** | 32×33 LDS tile, coalesced both ways. |
| **in-place tile swap** ✅ ✔ | **20** | O(L) | 0.97× | **3 planes** | N1 = N2, so tiles (i,j) and (j,i) swap through LDS. **Halves the workspace for 3 % more time — the best trade in the design**, since memory sets the digit count. 1.66 bytes/digit ✔. |
| fuse into next transform's load | +10 | O(L) | **loses** | 3 planes | Replaces two coalesced plane-touches with a stride-2048 read: one useful element per 64-byte line. Analysed, rejected. |

### S9. Corner turn (inter-APU, level 0 only)

Baseline: `hipMemcpyPeerAsync` all-to-all. All measured ✔ (`bench/03`, `bench/11`).

| option | LOC | complexity | node GB/s ✔ | speedup | description |
|---|---:|---|---:|---:|---|
| `hipMemcpyPeerAsync` | 8 | O(L) | 418 | 1.00× | Serviced by a blit kernel that occupies CUs; overlaps compute at only 16 %. |
| kernel pull, 128-bit | 12 | O(L) | 399 | 0.95× | Remote loads. |
| kernel push, 128-bit | 12 | O(L) | 699 | 1.67× | Remote stores. |
| **kernel push, 64-bit** ✅ ✔ | **12** | O(L) | **909** | **2.17×** | **The fabric prefers narrow accesses — opposite to local memory** (256-bit gives only 368). Stable at 4 and 8 GiB. Overlaps butterflies at **74 %** ✔. Message size is irrelevant from 128 B up, so the four-step tile size is unconstrained. |
| scattered remote stores | — | — | 227 (stride 2) | 0.25× | Never do this. |

### S10. Pointwise product

Baseline: `__uint128_t % p`.

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| 128-bit modulo | 4 | O(L) | 1.00× | — | A software 128-bit divide. **Cost 3.1× on the whole multiply** ✔ across all glue passes. |
| **Montgomery REDC** ✅ ✔ | **8** | O(L) | **~10×** on this pass | — | Both operands vary, so no precomputed quotient is possible. The 2⁻⁶⁴ folds into the final 1/L scaling, so nothing else has to know. |
| fuse into last forward transform | +15 | O(L) | saves 1 pass *est.* | — | gpupi does this. **Not yet implemented.** |

### S11. CRT + carry propagation

Baseline: host-side Garner + sequential carry. **Current implementation.**

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| **host Garner + carry** ⚠️ | **35** | O(L) | 1.00× | — | Fine at 60 M digits. **At 10¹¹ the packed result is 41.5 GB and single-threaded carry would cost more per multiply than the entire NTT.** The one correctness-preserving performance cliff on the list. |
| GPU CRT + blocked carry scan | ~90 | O(L) | **~50×** *est.* | +1 small plane | Coefficients are < 2¹¹⁸; sum contributions per output limb on the GPU, then a two-level block-carry scan. Standard, but must be written. |
| + overlap with CPU | +20 | — | 1.0× | — | CPU/GPU interference is only 7.7 % (node report), so the CPU can carry while the GPU transforms. |

### S12. Division (Phase 2)

Baseline: schoolbook. **Not implemented.**

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| schoolbook | 40 | O(N²) | — | 2W | Unusable at scale. |
| Newton reciprocal, full precision | 90 | ~5·M(N) | 1.00× | 3 planes + 3W | Iterate x ← x(2−Qx) at full precision throughout. |
| **+ precision doubling** | **+30** | **~3·M(N)** | **1.7×** *est.* | same | Work at the precision each iteration actually needs. Standard. |
| + middle product | +60 | ~2.5·M(N) | 2.0× *est.* | same | Hanrot–Quercia–Zimmermann; discards the half of each product that is not needed. Shares machinery with S13. |

### S13. Radix conversion (Phase 3) — **the long pole**

Baseline: repeated division by 10^k. **Not implemented.**

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| basecase repeated division | 40 | O(N²) | — | W | Unusable. |
| D&C integer binary→radix | 180 | O(M(N)·log N) | 1.00× | 3 planes + power tree + reciprocals | Two N/2 multiplies per node, plus precomputed powers **and** their reciprocals. |
| **scaled remainder tree** ✅ | **~350** | O(M(N)·log N) | **~2.5×** (Yee, measured for y-cruncher) | 3 planes + **1W** power table + W | Bernstein's fractional-part D&C: multiply by a power, keep the fractional part. Replaces division with multiplication and needs **no reciprocals**. **Corrected:** a node at level j is split using a power of W/2^(j+1) bytes, so the table sums to **W, not 2W** — the original draft over-counted. |
| + streamed digit output ⭐ | +20 | same | — | **−1.2W** | Leaves are emitted left to right, so digits go to disk as they are produced and the output buffer leaves the peak entirely. |
| + squaring path for the power table ⭐ | +40 | same | **1.33×** on table construction | — | The table is built by repeated squaring; a squaring needs 2 transforms, not 3. |
| + middle-product wraparound | +120 | same | included above | same | Reduces the (3/4)N multiply per node to N/2 effective. Requires middle-product support from the multiply layer. |
| + trailing-zero removal | +60 | same | 1.1–1.2× *est.* | — | Sizeable for base 10; complicates the middle product. |
| + precomputed forward transforms of powers | +40 | same | 1.3× *est.* | **+2.8× on the power table** | **Rejected: memory binds.** |

Yee is explicit that this is hard: inexact arithmetic with carry corner cases,
and a naive parallelisation costs O(N log p) memory unless designed for bounded
memory from the start.

### S13a. Execution order and batching — **MISSING FROM THE ORIGINAL DRAFT**

Baseline: recursive descent, one multiply issued at a time.

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| recursive descent | 0 | — | 1.00× | 2W | **Catastrophic.** At D = 10¹¹ the recursion has 1.04 × 10¹⁰ terms. Even with a 1024-term basecase that is 2.0 × 10⁷ multiplies; at ~10 kernels each and 4.0 µs dispatch ✔ that is **815 s of pure launch overhead** — more than the entire arithmetic budget. With a 32-term basecase, 7 hours. |
| **level-synchronous batching** ⭐ | **~150** | same arithmetic | **>10³×** on overhead | **2W — identical** | Execute the recursion level by level, bottom-up. Level j has 2ʲ *independent* merges of *identical* size, so they batch into one kernel launch per stage per level: **33 levels × ~10 kernels = 333 launches = 1.33 ms.** |
| + CUDA-graph-style capture | +40 | — | marginal | — | Only worth it if launches ever become visible again. |

**Memory is unchanged**, which is what makes this a pure win: a depth-first
stack holds one pending pair per level (sizes W/2ʲ, geometric sum 2W); a
level-synchronous sweep holds 2ʲ pairs of size W/2ʲ at one level — also 2W.
The batched-linear-algebra literature reaches the same conclusion for
hierarchical GPU algorithms: a batched formulation needs only O(log N) launches.

This changes the architecture from *recursive* to *level-synchronous*, and it
also makes the batched transform the natural kernel shape: at level j, 2ʲ
transforms of length L/2ʲ, total points L — the same work as one full-size
transform, at every level.

### S13b. Recursion basecase — **MISSING FROM THE ORIGINAL DRAFT**

Baseline: recurse to single terms.

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| recurse to one term | 0 | — | 1.00× | — | 1.04 × 10¹⁰ leaves, each `P=1, Q=a+1`. |
| **batched leaf kernel** | **60** | O(n) | large | — | One thread per leaf group; the bottom ~20 levels are word-sized arithmetic. |
| **batched schoolbook / Karatsuba** | **150** | O(m²) / O(m^1.58) | — | in-register | For products below the NTT threshold. gpupi switches to NTT above 512 product limbs (32 768 bits); the same crossover should be measured here, not assumed. |
| FP64 FFT for the middle range | ~250 | O(m log m) | 1.5–3× *est.* | more per point | y-cruncher finds FP FFT 5–10× faster than NTT for cache-resident sizes. **Worth measuring**, but it only touches the ~17 % of Phase 1 work below the NTT's efficient range. |

### S14. Memory management

Baseline: `hipMalloc`/`free` per operation. **Not implemented.**

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| per-op HIP allocation | 0 | — | 1.00× | — | **80–170 ms per GiB** (node report). A GiB-scale allocation per iteration would dominate everything. |
| **single arena + resource map** | **~180** | O(active) per alloc | **large** | one 460 GiB block | Allocate once (~63 s + 40 s first touch ✔), sub-allocate with a resource map: allocations from the bottom, table from the top. |
| + `size()` / `space()` bounds | +120 | — | — | — | Prove the peak before Phase 1 starts. **This is what converts the bytes/digit estimate into a number.** |
| + fork/join heap splitting | +80 | — | — | — | Split the free region when the recursion forks; no synchronisation, deterministic. |
| + explicit defragmentation | +50 | — | — | — | Needed where the upper child's allocations survive the join. |
| segmented convolution knob | +70 | ×s work | — | **planes ÷ s** | **The memory lever.** s = 2 halves plane memory for ~25 % more time. Time is in surplus by three orders of magnitude. |

`hipHostMalloc` is mandatory: it defeats the 96 GiB/APU `amdttm.pages_limit`
device cap at no bandwidth ✔ and no fabric ✔ penalty.

### S15. Verification and fault tolerance

Baseline: none.

| option | LOC | complexity | cost | description |
|---|---:|---|---:|---|
| **hash61 homomorphism** ✅ ✔ | **20** | O(N) amortised O(1) | negligible | `hash61(a)·hash61(b) = hash61(a·b)` over 2⁶¹−1. Mersenne form needs no division. **Already the full-size verification in `bench/08`/`13`.** Propagate with every big integer, check at recursion joins. |
| coefficient magnitude check | 10 | O(L) | negligible | Any coefficient above L·2^(2b−2) before carry propagation proves a fault. |
| schoolbook cross-check | 25 | O(n²) | small n only | Used at small operand sizes ✔. |
| automatic repeat request | 40 | — | — | Recompute the failed subtree. Yee: ~80 % of observed hardware faults are silent. |
| checkpointing | 90 | — | — | Rename swap files at recursion joins. Low priority: the run is minutes, not days. |

### S16. Multi-APU orchestration

Baseline: one APU.

| option | LOC | complexity | speedup | memory | description |
|---|---:|---|---:|---:|---|
| single APU | 0 | — | 1.00× | 115 GiB | |
| **task-parallel, levels ≥2** | **~120** | — | **~4×** | 4 × subtree | Level j has 2ʲ independent merges. At D = 10¹¹, level 1 already fits one APU (82.5 GiB for 3 planes) ✔, so **only level 0 needs distribution**. |
| **data-parallel level 0** | **~180** | one corner turn per transform | — | full node | Four-step split over 4 APUs; A, B and C each cross once. 3 turns per prime — the minimum. |
| CPU as a co-worker | +60 | — | — | — | 96 Zen 4 cores, 7.7 % interference (node report). Recursion bookkeeping, small base cases, hash verification, digit output. |

---

## Part 2 — improvement plan

Ordered by value. **Value here means digits, then correctness, then speed** —
in that order, because memory sets the answer and time does not.

### Tier 1 — required to produce any digits of e

| # | Work | Segments | LOC *est.* | Why |
|---|---|---|---:|---|
| 0 | **Level-synchronous batched execution** ⭐ | S13a | 150 | **Promoted to first by the review.** Without it, launch overhead alone is 815 s–7 h. Free in memory. Decide this before writing the driver, not after. |
| 1 | **Variable transform length + batched shape** | S3, S6, S13b | 250–400 | `bench/13` is hard-wired to L = 2048². Needs 7-smooth L from 2¹⁶ to 2³⁴, sub-2048 transforms, and a *batch* dimension so 2ʲ transforms of length L/2ʲ run in one launch. |
| 2 | **Arena allocator + `size()`/`space()`** | S14 | 300 | Gates everything, and **measures the bytes/digit that sets the ceiling**. Deliberately before Phase 1, not after. |
| 3 | **GPU CRT + carry scan** | S11 | 90 | The performance cliff. Must land before scale-up. |
| 3b | **Recursion basecase** ⭐ | S13b | 210 | Batched leaf kernel plus schoolbook/Karatsuba below the NTT threshold; measure the crossover rather than assume it. |
| 4 | **Binary splitting** | S1, S2 | 250 | → e to 10⁹ digits, verified. First real digits. |
| 5 | **Newton division** | S12 | 120 | → e in binary. |
| 6 | **Scaled remainder tree** | S13 | 350–500 | → decimal digits. The long pole; expect a surprise here. Stream the output and use the squaring path for the power table. |

Target after Tier 1: **e to 10⁹ digits, end to end, on one APU, verified.**
Total new production code ≈ 1 750–2 050 lines on top of ~450 existing.

### Tier 2 — required to reach the memory ceiling

| # | Work | Segments | LOC *est.* | Gain |
|---|---|---|---:|---|
| 7 | Task-parallel dispatch across APUs | S16 | 120 | ~4× on levels ≥2 |
| 8 | Distributed level-0 multiply | S9, S16 | 180 | Enables the top merge at full size |
| 9 | Size-balanced splitting | S2 | 80 | ~1.3× and exact heap splits |
| 10 | **Segmented convolution knob** | S14 | 70 | **planes ÷ s → 1.9 × 10¹¹ digits at s=2** |
| 11 | Scale-up and debugging at 460 GiB | all | — | Least predictable |

### Tier 3 — speed only, take if free

| # | Work | Gain | Note |
|---|---|---:|---|
| 11b | **Trivial-twiddle specialisation** ⭐ | **~5 %** | **The only free speed left** — no density cost |
| 12 | Fuse pointwise into last forward transform | ~5 % | gpupi does it |
| 13 | Interleaved `ulong2` twiddle in production kernels | ~6 % ✔ | Already measured |
| 14 | Squaring path + transform reuse | ~17 % | The merge multiplies by the same Q₂ twice |
| 15 | Middle product in Newton | ~1.2× on Phase 2 | Shares machinery with S13 |
| 16 | Accelerated e-series | 1.2–1.4× | Reduces term count |
| 17 | In-place progressive merge | ~15 % more digits | Overwrite consumed inputs node by node; would take Phase 1 from 7.27 W to ~6.3 W |

### Explicitly not doing, and why

| rejected | reason |
|---|---|
| matrix-core NTT | ≤12 % overall on a resource already in surplus; ~400 LOC |
| truncated Fourier transform | 7-smooth L + tunable b already remove the size jump |
| 32-bit primes | 1.37× compute ✔ for 11 % worse memory ✔ |
| deferred lazy reduction | 10.4 % speed for 6.7 % density; impossible for the inverse |
| non-temporal loads | 0.66× measured ✔ |
| cross-lane shuffle exchange | identical cost to LDS ✔ |
| precomputed transforms of SRT powers | 1.3× speed for 2.8× on the power table |
| out-of-core to NFS | ~5 000× slower than HBM; weeks per run |
| Goldilocks shift-twiddles | For p = 2⁶⁴−2³²+1 every root of order ≤64 is a power of 2, so 6 of 11 stages could shift-and-fold instead of multiply. Estimated 31.4 cyc/butterfly against Shoup's 27.67 ✔ — still loses, and caps L at 2³². Analysed, recorded, not built. |

The pattern: **every rejected item trades memory for speed.** That is the wrong
direction on this machine, and it is the single most useful heuristic in this
document.

---

## Part 3 — current state

### Code

| component | file | LOC | status |
|---|---|---:|---|
| butterfly + group macros | `bench/13_multiply2.c` | 22 | ✅ verified |
| transform bodies (fwd + inv) | " | 36 | ✅ verified |
| 4 transform kernels | " | 95 | ✅ verified |
| transpose + pointwise | " | 27 | ✅ verified |
| host plan / twiddle tables | " | 38 | ✅ verified |
| CRT + carry (host) | " | 35 | ⚠️ works, will not scale |
| hash61 verification | " | 20 | ✅ |
| **production total** | | **~273** | **a complete, verified multiply** |
| characterisation suite | `bench/01`–`12` | ~2 400 | ✅ 12 programs |
| Phases 1, 2, 3 | — | 0 | ❌ not started |

### Measured

| quantity | value | source |
|---|---:|---|
| multiply, 60.6 M-digit product, 1 APU | **1.23 ms** ✔ | `bench/13` |
| transform workspace | **1.66 bytes/digit** ✔ | `bench/13` |
| verification | schoolbook + hash61 homomorphism ✔ | |
| butterfly engine | 240 949 Gbit-bfly/s ✔ | `bench/01` |
| NTT kernel, forward / inverse | 2 325 / 2 569 Gbfly/s ✔ | `bench/06`, `bench/12` |
| node memory claimable | **460 GiB** at 14.0 TB/s ✔ | `bench/02` |
| corner turn | **909 GB/s**, 74 % overlap ✔ | `bench/11` |

### Projected for the full computation

| quantity | value | confidence |
|---|---:|---|
| **maximum digits** | **≈ 1.2 × 10¹¹** (was 1 × 10¹¹) | moderate |
| range | 0.6 × 10¹¹ (if bytes/digit lands at gpupi's 6.14) to 1.6 × 10¹¹ | |
| with segmented convolution s=2 | 2.3 × 10¹¹ *est.* | speculative |
| **peak memory** | **3.02 bytes/digit** *est.* (was 3.52), 4.2 after 1.4× derating | modelled |
| — dominated by | **Phase 1**, not Phase 3 (the review moved it): 3 planes + 2W splitting stack | |
| **total compute time** | **5–30 min** *est.* | moderate |
| — arithmetic model | ~1 min (58 full-multiply equivalents × 0.76 s on 4 APUs) | |
| — plus | ~100 s allocation ✔, CRT/carry, uncounted passes | |
| walltime cap used | **< 1 %** of 8 hours | |

**The number that matters most is the one not yet measured.** 1.66 bytes/digit
is the transform workspace of a bare multiply; the packed operands, the
binary-splitting stack and the radix-conversion power table are extra, and
they are modelled, not measured. Item 2 of Tier 1 exists specifically to
replace that estimate with a number — which is why the allocator comes before
binary splitting rather than after it.

For calibration: y-cruncher achieves 4.8 bytes/digit for π on CPU, gpupi 6.14
for π on an RTX 5090. Landing near 4.9 after derating would put this work level
with a mature CPU implementation, on a constant (e) whose 2-variable recursion
is intrinsically cheaper than π's 3-variable one.

---

## Part 4 — review, 2026-09-09

A deliberate hunt for what the first draft missed, rather than a restatement of
what it chose. Seven findings; two change the architecture, one changes the
headline number, one is free speed.

### R1 ⭐ Missing segment: execution order. **A recursive descent is unusable.**

The draft described the recursion but never said *how* it is issued. Issued as
a descent — one multiply at a time — kernel launch overhead alone is:

| basecase | multiplies | launch time |
|---:|---:|---:|
| 1 term | 2.1 × 10¹⁰ | 10 days |
| 32 terms | 6.5 × 10⁸ | 7 hours |
| 1 024 terms | 2.0 × 10⁷ | **815 s** |
| 32 768 terms | 6.4 × 10⁵ | 25 s |

against a total arithmetic budget of minutes. **Level-synchronous batched
execution** — process the recursion level by level, batching the 2ʲ identical
independent merges of each level into one launch per stage — gives 333 launches
and 1.33 ms, and costs **exactly the same memory** (a depth-first stack and a
level sweep both hold 2W). This is now item 0 of the plan, ahead of everything.
It also fixes the natural kernel shape: batched transforms, 2ʲ of length L/2ʲ.

### R2 ⭐ Missing segment: the basecase. 10¹⁰ leaves were unaccounted for.

At D = 10¹¹ the recursion has 1.04 × 10¹⁰ terms, and the draft had no batched
leaf kernel, no schoolbook/Karatsuba path, and no NTT crossover threshold.
Roughly 17 % of Phase 1's work sits below the size where a 2048²-based NTT is
efficient. Added as S13b.

### R3 ⭐ Ledger error: the radix-conversion power table is **W, not 2W**.

A scaled-remainder-tree node at level j is split using a power of
W/2^(j+1) bytes, so the table sums to W. The draft assumed each level needed a
power the size of its own node. Combined with streaming the digits to disk as
leaves are emitted (removing the 1.2W output buffer):

| | draft | corrected |
|---|---:|---:|
| Phase 3 peak | 8.47 W | **6.27 W** |
| binding phase | Phase 3 | **Phase 1** |
| peak | 3.52 B/digit | **3.02 B/digit** |
| **maximum digits** | 1.4 × 10¹¹ | **1.64 × 10¹¹** (+17 %) |

### R4 ⭐ Free speed: 18 % of butterflies multiply by 1.

`table[m+0] = root⁰ = 1` for every stage m, so Σ (N/2)/m ≈ N of the
(N/2)log₂N butterflies have a trivial twiddle. Most are only known at runtime,
but in the register-blocked kernel group A's first stage is *statically* known:
replacing 4 of 44 butterflies per thread with plain add/sub is **~5 % at zero
memory cost**. Given that every other remaining optimisation buys speed with
density, this is the only free one left.

### R5 Under-exploited: squaring.

The SRT power table is built by repeated squaring, and Newton's iteration
contains squarings. A squaring needs **2 transforms, not 3** — 33 % off those
operations. The draft listed squaring only as a Tier-3 item for the merge.

### R6 Analysed and still rejected: Goldilocks shift-twiddles.

For p = 2⁶⁴−2³²+1, 2 has order 192, so *every* root of unity of order ≤ 64 is a
power of two — ω₆₄ = 8, ω₃₂ = 2⁶, … ω₄ = 2⁴⁸. The first 6 of 11 stages could
therefore shift-and-fold instead of multiply. Estimating from the measured
Goldilocks butterfly (45.2 cyc ✔) with 6 stages at ~20 cyc gives ~31.4 cyc
average, still worse than Shoup's 27.67 ✔ — and Goldilocks caps L at 2³².
Recorded so it is not re-litigated.

### R7 In-place progressive merge.

Merging node by node and overwriting the two consumed inputs (whose combined
size exactly equals the output) could take Phase 1 from 7.27 W to ~6.3 W — a
further ~15 % in digits. Requires allocator support; listed as Tier 3 item 17.

### What the review did not find

No better series for e, no way to avoid the single full-precision division, no
saving from negacyclic or right-angle convolution in the NTT setting (unlike
the FP-FFT case, multiplication by a fourth root of unity is a full modular
multiply), and no evidence that the Infinity Cache is being left on the table —
the multiply is compute-bound at 1.09 TB/s per APU, well under even HBM, so
MALL residency currently buys nothing.

The draft's central judgements survive: two 62-bit primes, Shoup butterflies,
register blocking, in-place transposes, push corner turns, and the rule that
**an optimisation which trades memory for speed is going the wrong way on this
machine.** R1 and R3 are the exceptions that prove it useful to look again —
both are free in memory, and one of them was worth 17 % of the answer.

---

## Part 5 — second review, 2026-09-09 (architecture focus)

Seven findings. Two are new measurements, one overturns a conclusion from the
first review and a claim from campaign 2, and one closes the node report's own
largest gap.

### R8 ⭐ measured: **there is no throttling.** Burst numbers are valid.

The node report states plainly that it has no power, clock or sustained
throughput data, that every figure is "a burst of seconds", and that if the
four APUs share a package power budget then node aggregates "would degrade
under sustained load in a way this report cannot see". The real computation
runs for minutes at high VALU utilisation on all four APUs, so this was a live
threat to every time estimate in this document.

Running the NTT kernel continuously on all four APUs for 60 s (`bench/14`):

| window | node Gbfly/s | vs first |
|---:|---:|---:|
| 5 s | 2 339.9 | 1.000× |
| 20 s | 2 337.7 | 0.999× |
| 40 s | 2 333.3 | 0.997× |
| **60 s** | **2 327.4** | **0.995×** |

**0.5 % decay over a minute.** No thermal or power wall for this workload.
Every burst measurement in RESULTS.md transfers to a minutes-long run, and the
5–30 minute estimate stands. (Scope: 60 s at one kernel shape. A 30-minute run
at full memory footprint is still unmeasured, but a package-power cliff would
have shown up well inside a minute.)

### R9 ⭐ measured, negative: **fewer instructions, slower kernel.**

Review 1 (R4) identified that `table[m+0] = root⁰ = 1` makes 7 of the 44
butterflies per thread pure add/sub, and predicted ~9 % for free. Implemented
and verified bit-identical:

| | VALU instr ✔ | multiply-class ✔ | VGPRs | Gbfly/s ✔ |
|---|---:|---:|---:|---:|
| baseline | 1 510 | 440 | 54 | 2 225 |
| trivial-twiddle | **1 364** | **370** | 54 | **2 152** |

**10 % fewer instructions, 16 % fewer multiplies, 3.3 % slower** — and slower on
all four APUs individually, so it is not spread. Same register count, so it is
not occupancy.

**This corrects campaign 2.** That campaign measured latency/throughput ≈ 1.0
for the butterfly and concluded "the NTT kernel is issue-bound, not
latency-bound". That measurement was of *register-resident* operations with no
memory traffic. In the real kernel there are global twiddle loads and LDS
exchanges to hide, and group A's arithmetic was hiding them for free. Removing
it exposed the stalls.

The practical consequence is larger than the 3.3 %: **instruction count is not a
reliable proxy for time in this kernel**, so the whole line of attack from
campaign 2 — shave VALU instructions — is a dead end. It also retroactively
explains why campaign 1's twiddle experiments misbehaved (LDS-staged table
0.96×, non-temporal loads 0.66×): all three perturbed the memory path, not the
issue path.

### R10 Matrix cores: rejection confirmed, now by arithmetic rather than assertion.

| | measured |
|---|---:|
| int8 MFMA | 880.4 T MAC/s per APU ✔ |
| VALU Shoup modmul | 2 021 G modmul/s per APU ✔ |
| 62-bit product = 8×8 = 64 int8 MACs | 13.76 T/s → **6.81× the VALU** |

So the matrix cores really are ~7× better at modular products. But a DFT as a
dense matrix multiply does far more arithmetic than a butterfly network:

| radix | network modmuls | dense matmul | extra work |
|---:|---:|---:|---:|
| 4 | 4 | 16 | 4.0× |
| 8 | 12 | 64 | 5.3× |
| **16** | **32** | **256** | **8.0×** |
| 32 | 80 | 1 024 | 12.8× |

Best case radix-16: **8.0× the work against a 6.81× advantage = 0.85×, slower
than the VALU** — before the modular reduction that Sugizaki reports as the
dominant remaining cost, and before the layout conversions. Smaller radices
cannot use a 16×16 MMA tile without wasting most of it. The two factors very
nearly cancel, which is why this looked close and is not.

### R11 The real target is twiddle **traffic**, not instructions.

Given R9, the remaining opportunity is memory-side. Campaign 1 measured twiddle
loads at 21 % of the kernel, with a 32 KiB table against a 32 KiB L1 that four
blocks/CU are simultaneously streaming 128 KiB of data through.

**Top testable hypothesis: N = 512 sub-transforms in single-wave (64-thread)
blocks.**

| | N = 2048, 256 threads | N = 512, 64 threads |
|---|---:|---:|
| twiddle table | 32 KiB = **exactly L1** | **8 KiB** |
| LDS per block | 16 KiB | 4 KiB |
| blocks/CU at 16 waves/CU | 4 | 16 |
| exchanges | cross-wave, need `s_barrier` | **intra-wave, none needed** |
| passes for L = 2³³ | 3 | 4 |

The extra HBM pass is affordable — the multiply runs at 1.09 TB/s per APU
against 3.5 available — and it buys a table that fits L1 with room to spare
plus the removal of all six barriers. This is the one structural change with a
mechanism consistent with R9. It is a measurement, not a claim.

### R12 CPX: rejected again, but now with a stated condition.

CPX gives +17 % aggregate memory bandwidth (node report). Worthless here: the
multiply uses 1.09 of 3.5 TB/s per APU, and cross-XCD communication inside a
CPX APU is 58.2 GB/s — **slower than xGMI between separate APUs** (91.4), which
would wreck any distributed transform. Revisit only if a kernel ever becomes
genuinely bandwidth-bound; nothing in the current design is.

### R13 The CPU is ~3 % of the node and should stay out of the arithmetic.

96 Zen 4 cores give 390 GB/s and 5.1 TFLOP/s against the GPUs' 14 TB/s and
170 TFLOP/s. Even at 7.7 % interference it cannot carry a share of the
transforms. The CRT and carry are bandwidth-bound, where the GPU is 36× faster,
so those belong on the GPU too. Leave the CPU the recursion bookkeeping, split-
point precomputation, hash verification and digit output — genuinely useful,
and free.

The APU's real advantage is already banked and is not about the CPU cores at
all: **one physical memory pool, so 460 GiB is addressable with no staging and
no device-memory cap.** That is worth far more than the CPU's FLOPs.

### R14 Scalar twiddle loads: deprioritised by R9.

Group A's seven twiddles are wave-uniform and could be `s_load`ed into SGPRs
(the kernel uses 30 of ~102 available), relieving VALU and VGPR pressure. But
R9 shows group A is not where time is spent — its arithmetic is free, hiding
latency. Not worth doing.

### Revised optimisation posture

The kernel is at a **local optimum at ~2 325 Gbfly/s**: instruction count and
load latency are balanced, and perturbing either direction loses. Reducing
instructions is measured to hurt; the only untested lever with a plausible
mechanism is reducing twiddle *traffic* (R11).

Nothing in this review changes the digit ceiling or the memory ledger. It
changes what to work on: **stop optimising the butterfly, and go build Phases 1
to 3**, which do not exist yet and are worth far more than another 5 % on a
kernel that is 54 % of a multiply.

## Part 6 — CORRECTIONS from the e-paper reproduction (2026-09-15; PLAN.md §8–11, RESULTS.md §38–48)

The paper's pipeline was built faithfully in `ecalc/`, verified to 10⁹ digits
against GMP and to 5 × 10¹⁰ by residues, and run at the paper's speed
(4 × 10¹⁰ in 281–290 s vs 285.7) on the paper's memory budget (248 GB vs
256). Every Phase 5 experiment was then run against it. What that changes
in this document:

1. **Memory ceiling (Part 0, "3.4 bytes per digit → ~10¹¹ digits").** That
   figure assumes memory-resident NTT planes. In the paper's structure the
   numbers live on the host as 64-bit limbs and the planes are transient;
   the measured footprint is 6.2 bytes per digit (248 GB at 4 × 10¹⁰) plus a
   fixed 192 GB of pools and staging, and **the ceiling on one node is
   ≈ 5 × 10¹⁰ digits** (measured: 5 × 10¹⁰ verified at 375 GB host + 128 GiB
   pools). Reaching 10¹¹ needs a memory-resident design, not a denser prime
   set (RESULTS §44–45).
2. **S3, 2 × 62-bit primes at 45 bits per point (2.81 vs 2.0 bits/byte).**
   Built as engine 2; exact; 2.2× slower as built and a *higher* peak,
   because a plane holds fewer limbs and more products split. The density
   advantage is real only inside a memory-resident pipeline (§44). Engine 1
   (4 × 52-bit, FP64 Barrett) stays.
3. **Arithmetic (Part 1, Shoup vs FP64).** bench/01's 1.14× for Shoup per
   butterfly does not survive in a real pass: measured 2–3 % *slower* in
   the b1 pass, and integer Montgomery chains run 25 % below FP64 Barrett
   (§47–48). The FP64 modmul co-issues with the integer adds; use it.
4. **S9/S16, one prime per device vs the four-step corner turn.** The device
   memory per product is the same under any distribution; the corner turn
   only adds fabric time on this node (§46). Four-step is the multi-node
   path only.
5. **Kernel body.** A register-blocked 7-stage pass body (8 rows per thread,
   two LDS exchanges) is +19 % forward / +15 % inverse over the paper's
   tile kernel at 2³¹ and bit-identical (§43); radix-4 grouping on top adds
   nothing once the rows are in registers. The LDS is the limiter: 36 TB/s
   at 3 blocks/CU, half that at 1 block/CU (§48).
6. **CRT.** A striped, coalesced GPU CRT (peer reads of all planes, results
   stored straight into registered host memory) does mdev's CRT in ≈ 0.5 s
   per 2³¹ including the copy back, against 0.8–1.5 s for the 192-thread
   CPU Garner (§39, §43); it also lifted the batch tier from 1.2 to
   4.3 Gpoint/s. The CPU CRT (S11) is no longer the cliff.
7. **Newton / division.** The paper's truncated-r² step is not sound as
   printed (§39); a correction-form iteration (r' = r ≪ 64j + (r·d ≫ 64j))
   with a self-correcting doubling, the high half of A·μ, and a low product
   for X·Q (`rns_mul_low`) match its dm time at 1.04× (§40).
8. **What the paper leaves out that matters** (rules 1–7 of PLAN §8): the
   FP64 modmul takes at most one lazy operand; butterfly adds must be
   integer; twiddles from two-level tables; NUMA-pinned first touch of the
   staging (55× otherwise); kernel stores instead of `hipMemcpy` D2H;
   report rates, derive cycles only from in-kernel clocks.
