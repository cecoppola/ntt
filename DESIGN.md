# Very-large NTT on one MI300A node — design for computing *e*

**Target** the largest possible number of decimal digits of *e* on a single
node of 4 × AMD Instinct MI300A (`PPAC_MI300A_SPX`), within the 8-hour
Slurm walltime.

**Status** design + measured foundations. Benchmarks 01–03 in `bench/` are
implemented and run; the NTT kernels themselves are not written yet.

**Sources** the node characterisation in `~/apucode/BENCHMARK_REPORT.md`
(52 tests, all four APUs, 2026-09-08) plus the three new measurements in this
directory. Algorithms from Harvey (NTT butterflies), Bailey (four-step),
Yee (y-cruncher internals), Bernstein (scaled remainder trees), and the
GPU-NTT literature — see §11.

---

## 1. Executive summary

### The three constraints, in order

| Rank | Constraint | Value | What it decides |
|---|---|---|---|
| 1 | **Node memory** | 460 GiB claimable | The digit count. Everything else is secondary. |
| 2 | **NTT kernel throughput** | 2 325 Gbfly/s measured | The dominant term: only level 0 of the recursion must be distributed, so fabric is ~21% of the run. See RESULTS.md §5. |
| 3 | **xGMI fabric** | 697 GB/s node all-to-all, **20× below HBM** | The cost of a level-0 multiply only. |

The 8-hour walltime is **not** a constraint. The estimated arithmetic time for
10<sup>11</sup> digits is on the order of a minute; the job will be limited by
memory capacity by a factor of several hundred before it is limited by time.
This inverts the usual optimisation target: **spend compute to buy memory, not
the other way round.**

### Headline design decisions

| # | Decision | Evidence |
|---|---|---|
| 1 | **Harvey/Shoup lazy butterfly, `u64`, p < 2<sup>62</sup>** | 240 949 Gbit-bfly/s node — 14 % over FP64, 39 % over Goldilocks (bench 01) |
| 2 | **Two 62-bit primes, 45 bits/point** | Best transform-domain density available: 2.81 bits per stored byte (§4.2) |
| 3 | **`hipHostMalloc` for every large array** | 460 GiB allocated at 14.0 TB/s read — defeats the 96 GiB/APU device cap at no cost (bench 02) |
| 4 | **Corner turn = push kernel, 128-bit stores** | 697 GB/s vs 399 GB/s pulling, and vs 418 GB/s for `hipMemcpyPeerAsync` (bench 03) |
| 5 | **Keep multiplies on one APU wherever they fit** | Fabric is 20× slower than HBM; a local multiply pays none of it |
| 6 | **No matrix cores** | They optimise the one resource in surplus (§7.1) |
| 7 | **Generate twiddle factors on the fly** | A stored table is a whole extra plane; recomputing costs one modmul (§4.5) |
| 8 | **Primes with 2<sup>40</sup>·3·5·7 \| p−1** | 7-smooth transform lengths remove the power-of-two size jump without a TFT (§4.3) |

### Expected outcome

| Quantity | Estimate |
|---|---|
| Digits of *e* | **≈ 1 × 10<sup>11</sup>** (100 billion), conservative; 1.6 × 10<sup>11</sup> if the memory model holds exactly |
| Peak memory | ≈ 3.0 bytes per decimal digit |
| Core arithmetic time | ≈ 45 s |
| Realistic wall time | 5–20 min, plus ~100 s of one-time allocation |

This will not approach the out-of-core world record (tens of trillions of
digits, set on disk arrays over months). It is what one node can hold.

---

## 2. Why this machine inverts the usual trade

The classical wisdom for record-scale bignum multiplication, from y-cruncher,
is that floating-point FFTs are 5–10× faster than NTTs but consume far more
memory, and that NTTs win once memory or memory bandwidth binds. On MI300A the
memory argument is overwhelming and the speed argument is nearly irrelevant:

```
                          per node        ratio to HBM
  HBM read              14 020 GB/s          1.0
  Infinity Cache        53 025 GB/s          3.8      (256 MiB/APU)
  L2 (16 MiB WS)        55 826 GB/s          4.0
  LDS                  190 748 GB/s         13.6
  xGMI all-to-all          697 GB/s          1/20     <-- measured here, push
  CPU (96 Zen 4 cores)     390 GB/s          1/36
```

A butterfly moves 16 bytes and costs 28 VALU instructions. The machine's
balance is ~12.7 FP64 ops per byte; a butterfly-heavy kernel with 11 stages
fused per pass sits at ~3 ops/byte. So the arithmetic and the memory system are
within a small factor of each other, and **both are 20× faster than the fabric.**

The consequence runs through every decision below: minimise inter-APU traffic
first, minimise bytes resident second, and treat instruction count last.

---

## 3. Algorithm stack for *e*

```
  e = sum_{k>=0} 1/k!

  Phase 1   binary splitting (hyperdescent, 2-variable)   -> P, Q with e ~ 1 + P/Q
  Phase 2   division P/Q by Newton iteration              -> e in binary
  Phase 3   binary -> decimal by scaled remainder tree    -> digits
```

**Phase 1.** *e* is the cheap case. Its recursion needs only two variables:

```
  P(a,a+1) = 1                 Q(a,a+1) = a+1
  P(a,b)   = P(a,m) Q(m,b) + P(m,b)
  Q(a,b)   = Q(a,m) Q(m,b)
```

Because R(k) ≡ 1 the series is superlinearly convergent, giving
**O(N log²N)** rather than the O(N log³N) of Chudnovsky-type π formulas. This
is why *e* records run far ahead of π records at equal effort, and why *e* is
the right target for a memory-limited node.

For N digits, n ≈ N / (log₁₀ n − 0.434) terms; at N = 10<sup>11</sup> that is
n ≈ 5.4 × 10<sup>9</sup>, a recursion ~33 levels deep.

**Split by size, not by index.** The terms grow, so splitting at the midpoint
index leaves the two halves unbalanced (Yee: 215 vs 311 digits for 100!).
Choose *m* by binary search on a `size()` estimate (first-order Stirling) so the
two children produce equal-sized results. This both balances the APUs and
minimises total multiplication work, since M(x) is superlinear.

**Phase 2.** Newton reciprocal, ~3 full-size multiplications plus one to form
P·(1/Q). Standard; use the middle-product optimisation.

**Phase 3.** The scaled remainder tree (Bernstein), i.e. the divide-and-conquer
*fractional-part* algorithm. It replaces division by multiplication, needs one
(3/4)N multiply per node that reduces to N/2 via middle-product wraparound, and
is >2× faster than the integer binary→radix algorithm. Yee measured 2.5× when
y-cruncher switched. Its cost is comparable to all of Phase 1, so it is not an
afterthought.

**Estimated full-size-multiply equivalents:** Phase 1 ≈ 30, Phase 2 ≈ 5,
Phase 3 ≈ 20. Total ≈ 55.

---

## 4. The NTT

### 4.1 Arithmetic engine — measured, not assumed

Four candidate butterflies, 8 independent chains per thread, operands in
registers, all four APUs loaded concurrently (`bench/01_butterfly.c`):

| Engine | Gbfly/s (node) | log₂p | **Gbit-bfly/s** |
|---|---:|---:|---:|
| **`shoup_u64`** Harvey Alg. 4, p < 2<sup>62</sup> | 3 886 | 62 | **240 949** |
| `mont_u64` Harvey Alg. 5, p < 2<sup>62</sup> | 3 114 | 62 | 193 038 |
| `gold_u64` p = 2<sup>64</sup>−2<sup>32</sup>+1 | 2 713 | 64 | 173 653 |
| `shoup_f64` FP64 fma/rint, p < 2<sup>50</sup> | **4 212** | 50 | 210 602 |

Bit-butterflies is the right figure of merit: a larger prime carries
proportionally more bits per point, so it shortens the transform.

**FP64 wins on raw butterfly rate and still loses.** It issues 8 % more
butterflies per second than the integer path but each carries 19 % fewer bits,
and — decisively — three 50-bit primes are needed where two 62-bit primes
suffice, costing 32 % more memory in the transform domain (§4.2). Goldilocks'
famously cheap Solinas reduction does not compensate for having no precomputed
quotient: it needs both halves of the product plus conditional corrections.

**The Shoup kernel is issue-bound and the compiler is already near optimal.**
Disassembling the inner loop gives **28 VALU instructions per butterfly**:

```
 40 v_mad_u64_u32     32 v_mul_lo_u32     24 v_lshl_add_u64
 22 v_sub_co_u32      22 v_subb_co_u32    16 v_add3_u32
 10+6 v_cndmask        8 v_mul_hi_u32     32 v_mov_b32      (per 8 butterflies)
```

At 228 CU × 64 lanes × 2.10 GHz the measured 971 Gbfly/s per APU is 31.6
lane-cycles per butterfly, i.e. **88 % issue efficiency on 28 instructions**.
`v_mad_u64_u32` and `v_mul_lo_u32` are evidently *not* quarter-rate on CDNA3.
There is little left to win here without changing the algorithm.

**Decision: `shoup_u64`, Harvey Algorithm 4, lazy [0,4p) representation.**

```c
/* p < 2^62 = beta/4, wp = floor(w * 2^64 / p), inputs in [0,4p) */
q = __umul64hi(wp, y);        /* floor(w*y/p) or one less        */
T = w * y - q * p;            /* in [0,2p) — no correction step  */
if (x >= 2p) x -= 2p;
X = x + T;                    /* [0,4p) */
Y = x - T + 2p;               /* [0,4p) */
```

Harvey's redundant representation is what removes the conditional subtractions
after the multiply. That matters more on this machine than on a CPU: two-way
divergence within a wave costs 1.94× (report §5.1). These particular conditions
compile to `v_cndmask`, not branches, so they are cheap — but there are fewer
of them this way regardless.

### 4.2 Prime count and bits per point

With *k* primes of *w* bits, balanced digits in [−2<sup>b−1</sup>, 2<sup>b−1</sup>),
and transform length *L*, exactness requires

```
    1 + log2(L) + 2b  <=  k * w
```

Storage is 8*k* bytes per point carrying *b* bits, so density is
`b / (8k)` bits per byte. Maximising over *k* at *w* = 62, *L* = 2<sup>33</sup>:

| k | max b | bits/byte | expansion vs packed |
|---:|---:|---:|---:|
| 1 | 14 | 1.75 | 4.6× |
| **2** | **45** | **2.81** | **2.85×** |
| 3 | 62 (capped by p) | 2.58 | 3.10× |
| 4 | 62 (capped) | 1.94 | 4.13× |

**k = 2 is optimal**, and the cap for k ≥ 3 is structural: a point must be
smaller than the prime, so extra primes buy dynamic range that cannot be used.
Three 50-bit FP64 primes give 51 bits over 24 bytes = 2.13 bits/byte, 24 %
worse than the integer path — the second half of decision #1.

The 2.85× expansion over the packed integer is the irreducible cost of the
transform domain; the information-theoretic floor for a convolution is 2×.

### 4.3 The primes

Two 62-bit primes with 2<sup>40</sup>·3·5·7 dividing p−1, both with primitive
root 11:

```
  p1 = 39943 * 105 * 2^40 + 1 = 0x3FFEDF0000000001
  p2 = 39922 * 105 * 2^40 + 1 = 0x3FF6420000000001
```

The 3·5·7 factor is deliberate. Transform lengths of the form
2<sup>a</sup>3<sup>b</sup>5<sup>c</sup>7<sup>d</sup> (7-smooth) are dense —
consecutive achievable lengths differ by only a few percent in the range of
interest — so the transform can be sized to the operand within ~5 %.

This is **why no truncated Fourier transform is needed.** The TFT literature
(van der Hoeven, Harvey–Roche, Arnold) exists to smooth the factor-of-two jump
at powers of two, worth up to 2× in time and memory. Here two independent knobs
already remove it: *b*, the bits per point, is continuously adjustable, and *L*
is 7-smooth. A TFT would add substantial irregularity to GPU kernels for a
benefit already obtained. **Do not implement a TFT.**

They are also Montgomery-friendly (p ≡ 1 mod 2<sup>32</sup>), which keeps the
option of Sugizaki's one-multiply-per-32-bit-digit Montgomery reduction if the
CRT step ever becomes hot.

### 4.4 Transform structure

Four-step (Bailey) with **decimation-in-frequency forward, decimation-in-time
inverse**, so no bit-reversal permutation is ever performed — for convolution
the frequency-domain ordering is irrelevant as long as forward and inverse
agree.

Per pass, fuse as many radix-2 stages as fit in LDS. At 16 KiB per block
(2048 points × 8 B) a block does **11 stages in one pass** and 4 blocks/CU
gives 16 waves/CU, which the report identifies as the bandwidth optimum
(§4.5 of the report: 16 waves/CU is 5 % *faster* than full occupancy). A
2<sup>33</sup> transform is then three passes.

LDS conflicts must be **derived from the access pattern, not assumed**. A
straightforward radix-2 in-LDS transform is already conflict-free and an XOR
swizzle costs it 3 %; the register-blocked kernel's exchanges have 16- and
32-way conflicts and the swizzle is worth 5 %. See RESULTS.md §3 for the
bank-class counting method.

**Hold 8 points per thread in registers** so each group of 3 radix-2 stages is
register-resident and LDS is only a transpose buffer: 3 round trips instead of
11, worth 1.42×.

### 4.5 Twiddle factors are computed, not stored

A full twiddle table for an L-point four-step transform is L entries — an
entire extra plane, ~57 GB at our sizes, i.e. ~12 % of the digit budget.
Instead store two tables of √L roots and form each twiddle with one modular
multiplication. The cost is one extra `shoup_mul` per point per pass, roughly
+30 % on butterfly instructions in the twiddle pass only, against 12 % more
digits. On a memory-bound machine this trade is not close.

### 4.6 Distribution across APUs, and when to avoid it

A four-step transform distributed over 4 APUs needs exactly **one all-to-all
transpose**, moving 3/4 of the plane. Each of A, B and C crosses once, so a
full multiply costs 3 corner turns per prime — that is the minimum, and no
reordering of the six-step or four-step variants beats it.

At L = 2<sup>33</sup> (plane = 68.7 GB), per full-size multiply:

| Component | Bytes | Rate | Time |
|---|---:|---:|---:|
| Fabric (6 plane-transforms × 0.75 plane) | 309 GB | 697 GB/s | **443 ms** |
| Butterflies (8.5 × 10<sup>11</sup>) | — | 3 886 Gbfly/s | 219 ms |
| HBM (3 passes × r+w × 6) | 2.47 TB | 12–14 TB/s | ~200 ms |

**The fabric costs more than the arithmetic and the HBM combined.** Hence:

* **Run a multiply on a single APU whenever its working set fits in 115 GiB.**
  Three planes plus operands fit for H ≲ 2.6 × 10<sup>11</sup> bits, i.e. any
  product up to ~7 × 10<sup>10</sup> digits. Only the top one or two levels of
  the binary-splitting recursion need a distributed transform.
* Below that, use **task parallelism**: the recursion has 2<sup>j</sup>
  independent children at level *j*, so from level 2 down every APU has its own
  subtree and the fabric is idle.
* This is the same hierarchical structure y-cruncher uses across threads,
  with the distributed/local crossover set by APU memory instead of cache.

### 4.7 The corner turn

Measured on this node (`bench/03_fabric.c`, 8 GiB buffers, 128-bit accesses):

| Pattern | Node GB/s | Note |
|---|---:|---|
| **all-to-all push** (remote stores) | **697** | recommended |
| all-to-all pull (remote loads) | 399 | 1.75× worse |
| single pair pull | 89 | one flow |
| `hipMemcpyPeerAsync` all-to-all (report) | 418 | 1.67× worse than push |

Two findings worth stating plainly:

1. **Push beats pull by 1.75×** for the all-to-all pattern, and a hand-written
   push kernel beats `hipMemcpyPeerAsync` by 1.67×. The report's guidance to
   issue peer copies from the source device generalises: for a transpose, have
   each APU *write* into its peers rather than *read* from them.
2. **The allocator is irrelevant to fabric bandwidth.** `hipMalloc`,
   `hipHostMalloc` coherent and non-coherent are identical to within 1 % on
   every pattern. An earlier reading of 66 GB/s on host memory turned out to be
   a scalar grid-stride kernel, not an allocator penalty — the same buffers
   reach 89 GB/s single-flow with 128-bit accesses. **Always use 128-bit
   accesses across the fabric**, and never scattered ones (the report measures
   13.3× for scattered vs contiguous remote access).

Push asymmetry is real and unexplained: APU3 pushes at 208 GB/s, APU1 at 155
(30 % spread), echoing the report's 40 % broadcast-root asymmetry. It does not
change the design but a static schedule should not assume symmetry.

---

## 5. Memory: the budget that sets the digit count

### 5.1 Claiming the memory

`bench/02_capacity.c` allocated and first-touched **460 GiB** (115 GiB/APU)
with `hipHostMalloc`, then read it at **14.0 TB/s** and ran triad at
**11.8 TB/s** — full HBM speed.

This matters because the HIP *device* allocator is capped at 96 GiB per APU by
`amdttm.pages_limit` unless root raises it; the ROCm MI300A tuning guide states
the cap does not apply to host allocations, and the measurement confirms it.
Using `hipHostMalloc` therefore buys **~76 GiB more usable memory (+20 %
digits)** with no bandwidth penalty and no fabric penalty (§4.7).

Do not use `hipMallocManaged`: the report measures 1 728 GB/s with a 307 %
spread across APUs, with or without XNACK.

**Allocation is expensive and must happen exactly once.** Claiming 460 GiB took
63 s of allocation plus 40 s of GPU first-touch (worst APU, all four
concurrent). Build a single arena at startup and sub-allocate from it with a
resource map, as y-cruncher does — never call the HIP allocator inside the
recursion.

### 5.2 Scheduling the top-level merge for minimum peak

The top merge computes `Q = Q1·Q2` and `P = P1·Q2 + P2`, sharing the transform
of Q2. The obvious schedule keeps three forward transforms and two products
live — six planes. Instead, **stream over primes and recompute the shared
transform**:

```
for target in {T = P1*Q2, Q = Q1*Q2}:
    for i in {1,2}:
        A <- forward(Q2 mod p_i)            # plane 1
        B <- forward(operand mod p_i)       # plane 2
        B <- inverse(A .* B)                # in place
        R[i] <- B                           # plane 3 (i=1) / reuse
    CRT(R[1],R[2]) + carry propagate -> packed result
```

Peak is **three planes**, not six. The price is four forward transforms of Q2
instead of two — roughly +25 % arithmetic on the top merge, which by §4.6 is
already fabric-dominated. Trading 25 % of a resource we have in surplus for
50 % of the resource that sets the answer is the single most valuable
scheduling decision in the design.

### 5.3 The digit budget

With three planes of 8L bytes, L = H/b, plus the binary-splitting operand stack
(≈ H/4 bytes: one pending left sibling per level, geometrically decreasing, for
both P and Q) and the packed results (≈ H/4):

```
    peak(H)  ~=  24H/b + H/2   bytes,     H = 3.3219 * D bits
    b = 45   ->  peak ~= 1.03 * H bytes  =  3.4 bytes per decimal digit
```

Against 460 GiB = 4.94 × 10<sup>11</sup> bytes:

| | Digits |
|---|---:|
| Model, exact | 1.45 × 10<sup>11</sup> |
| **With 1.4× derating for twiddle tables, output buffer, fragmentation, slop** | **≈ 1.0 × 10<sup>11</sup>** |

Phases 2 and 3 have similar peaks — Phase 3's power table is
H + H/2 + H/4 + … = 2H bits — so Phase 1 sets the budget.

### 5.4 A knob if memory binds harder than modelled: segmented convolution

Split each operand into *s* blocks and accumulate the s² block products in the
transform domain, recomputing transforms rather than storing them. Transform
length drops to 2L/s, so plane memory drops by *s*, while total arithmetic
rises only by *s* (not s², because the transforms shrink). At s = 2 the planes
cost half as much for +25 % time; at s = 4, a quarter for +75 %.

Given that arithmetic is ~25 % of a fabric-bound multiply, this is a cheap way
to convert surplus time into digits, and it is the right lever to pull if the
1.4× derating in §5.3 turns out optimistic. **Implement the plane allocator so
that s is a runtime parameter.**

---

## 6. Putting it on the hardware

### 6.1 Kernel and launch rules (all from the node report)

| Rule | Number |
|---|---|
| Launch 2–4 blocks per CU; 1/CU costs 34 % | §4.5 |
| Target 16 waves/CU, not maximum occupancy | §4.5 |
| Do not spend registers to buy occupancy — throughput was flat from 8 to 256 live accumulators | §4.6 |
| Unroll to ~64, never past 256 (55× cliff at 1024) | §5.3 |
| Vectorise to `float4`/128-bit — flat 1.24× | §4.4 |
| Never stride across cache lines — stride 16 costs 12.8× | §4.4 |
| Fuse kernels shorter than ~30 µs (4.0 µs dispatch) | §5.4 |
| Do not rely on more than 4 concurrent streams — hardware queue limit | §5.4 |
| XOR swizzle, not padding, for LDS conflicts | §4.7 |
| Do not swizzle workgroups on faith — measured ±0.3 % | §4.10 |

### 6.2 Use the CPU

96 Zen 4 cores, 5.1 TFLOP/s FP64, 390 GB/s, and the report measures only
**7.7 % interference** when CPU and GPU run concurrently — the GPU keeps 94 %
of its bandwidth. The CPU should never be idle. Give it:

* the binary-splitting recursion bookkeeping and `size()` binary searches;
* small-integer base cases below the GPU threshold;
* GCD/prime factorisation of the recursion polynomials if factor removal is
  added later (Yee reports 20–30 % for Chudnovsky; less applicable to *e*);
* the mod-2<sup>61</sup>−1 checksum verification (§8);
* digit formatting and output, overlapped with the tail of Phase 3.

Use `_mm512_stream_pd` for CPU write streams (1.42–1.65×), pin one thread per
physical core (SMT is worth +0.6 %), and saturate at ~64 threads, not 192.

### 6.3 System settings to request or set

| Setting | Why |
|---|---|
| `GPU_MAX_ALLOC_PERCENT=100`, `GPU_SINGLE_ALLOC_PERCENT=100` | user-settable; needed for large single allocations |
| `amdttm.pages_limit=134217728` | root-only; would let `hipMalloc` reach 128 GiB/APU. Worth asking AAC support for, but §5.1 makes it optional |
| `transparent_hugepage=always` | already default on Ubuntu; report found THP made no measurable difference but the test was inconclusive |
| `numa_balancing=disable`, `compaction_proactiveness=20` | ROCm MI300A tuning guide; prevents fragmentation degradation over a long run |
| `tmux` + checkpointing | 8 h cap, OTP login, no backups |

### 6.4 NUMA placement

Each APU is its own NUMA node and remote CPU access costs 3.4× bandwidth and up
to 2.9× latency, with a visibly non-uniform matrix (report §10.3). First-touch
every plane from the APU that will own it, from a host thread pinned to that
APU's cores.

**Phase 7 WP3 (RESULTS §55–56) sharpened this into the pipeline's layout:**
the GPU side obeys the same rule — own-node memory at ≈ 3.8 TB/s, remote at
≈ 93 GB/s, regardless of whether it is hipMalloc'd or host pages — so the
binary-splitting level pools are four per-APU device regions with subtree
ownership, and the batch tier is *locality-aware*: APU d transforms all four
primes of the products in its own region (instead of one prime for every
product), reading its own node's memory, CRT on its own four planes, results
in place. No operand staging, no peer traffic. The CPU never touches those
pools (the P₁Q₂ + P₂ add and the normalisation are done in the CRT kernel;
copies in and out are DMA). The mdev-tier levels and the dm-phase numbers
are still host-resident until WP5 puts the 4-APU distributed transform on
device-resident, block-cyclic numbers.

---

## 7. What not to do, and why

### 7.1 Do not use the matrix cores

The fashionable result in the recent NTT literature (Sugizaki–Takahashi 2026;
WarpDrive; tcFFT) is to run the small DFTs as tensor-core matrix multiplies
over 8-bit digits. On this machine that is the wrong optimisation:

* A radix-16 DFT as a dense matmul does 256 modular MACs where a radix-16
  butterfly network does 32 — **8× the arithmetic**.
* A 62-bit modulus needs ⌈62/8⌉² = 64 `i8` MMA products per modular product.
* Net: ≈ 2× faster than the VALU path, before the Montgomery reduction that
  Sugizaki reports as the dominant remaining cost.
* Arithmetic is ~25 % of a fabric-bound multiply. A 2× arithmetic win is
  therefore worth **≤ 12 %** overall, for a very large increase in complexity.

The technique is real and would matter for a *cache-resident, compute-bound*
NTT — the FHE setting these papers target. It does not match a memory-capacity-
bound bignum computation. Revisit only if the fabric problem is solved and
single-APU multiplies become the whole runtime.

### 7.2 Do not use a truncated Fourier transform

See §4.3: two continuous size knobs already remove the jump the TFT exists to
remove.

### 7.3 Do not go out-of-core

`/shared` has 27 TB free, which at 3.4 bytes/digit would nominally hold
8 × 10<sup>12</sup> digits. But it is shared NFS at perhaps 1–3 GB/s, roughly
5 000× slower than HBM. Even a well-optimised 3-pass swap-mode multiply would
need ~18 TB of I/O per full-size multiply at that scale; 55 of them is
~10<sup>3</sup> TB, i.e. weeks of wall time against an 8-hour job limit. The
local 293 GB on `/` is too small to matter. **In-memory only.**

### 7.4 Do not use `hipMemcpy` anywhere on the hot path

56× slower than sharing a `hipHostMalloc` pointer (report finding #1), and for
peer transfers 1.67× slower than a push kernel (§4.7).

### 7.5 SUPERSEDED — fabric transfers *do* hide behind compute

Measured at **74 %** overlap for a push kernel on its own stream (RESULTS.md
§11), against the 16 % the node report measured for `hipMemcpyPeerAsync`. The
difference is that the blit kernel occupies CUs and a push kernel does not
contend the same way. §4.6 adds fabric and compute time; with this result the
level-0 multiply is closer to the max of the two than the sum.

---

## 8. Correctness and verification

At 10<sup>11</sup> digits over minutes on 912 CUs, silent hardware error is a
real risk — Yee reports ~80 % of observed faults were silent, and hit a
silent fault 8 days into a 5-trillion-digit π run.

* **Modular hash, p = 2<sup>61</sup>−1.** Attach a checksum to every big
  integer, propagate it through add/sub/multiply, verify at recursion joins.
  O(1) amortised. Chosen for the Mersenne form: reduction needs no division.
* **Coefficient magnitude check.** After the inverse NTT and before carry
  propagation, any coefficient exceeding the theoretical maximum
  L·2<sup>2b−2</sup> proves an error. Nearly free.
* **Automatic repeat request.** On a failed check, recompute that subtree.
* **Independent final check.** Recompute the last few thousand digits with a
  different prime pair, or verify a BBP-style spot check where available.
* **Checkpoint** P and Q at recursion joins so an 8-hour boundary or a node
  failure does not restart from zero.

---

## 9. Implementation plan

| Step | Deliverable | Gate |
|---|---|---|
| 0 ✅ | `bench/01–03`: butterfly engines, capacity, fabric | done |
| 1 | `bench/04`: LDS-resident 2048-point NTT kernel, verified against a reference | ≥ 60 % of the 3 886 Gbfly/s register-resident rate |
| 2 | `bench/05`: full single-APU L = 2<sup>28</sup> forward+inverse, exact round-trip | correctness first, then rate |
| 3 | Single-APU multiply: split, 2 primes, pointwise, CRT, carry. Verify against GMP | exact for random and adversarial inputs |
| 4 | `bench/06`: fused corner-turn kernel; measure overlap with butterflies | beat 697 GB/s or prove it cannot |
| 5 | Distributed 4-APU multiply | ≥ 0.6 of the single-APU rate × 4 |
| 6 | Arena allocator + `size()`/`space()` functions | provable memory bound before Phase 1 starts |
| 7 | Phase 1 binary splitting, checksummed | *e* to 10<sup>9</sup> digits, verified |
| 8 | Phase 2 Newton division | |
| 9 | Phase 3 scaled remainder tree + middle product | *e* to 10<sup>10</sup> digits |
| 10 | Scale to the memory limit, checkpointing | *e* to ~10<sup>11</sup> digits |

Steps 1–3 are the load-bearing ones: everything above them is scheduling, and
everything below is arithmetic already validated.

---

## 10. Open questions

1. ~~Does the corner turn overlap with compute?~~ **Answered: 74 %**
   (RESULTS.md §11).
2. **Why is push 1.75× pull, and why is the push asymmetry 30 %?** Not
   mechanistically isolated. Affects how a static transpose schedule should be
   written.
3. **What does the LDS-resident NTT actually sustain?** The 3 886 Gbfly/s is an
   upper bound with operands in registers. Step 1 above measures the real one.
4. ~~Is 128-bit the best fabric access width?~~ **Answered: no — 64-bit is,
   at 909 GB/s** (RESULTS.md §11).
5. **Can `amdttm.pages_limit` be raised?** Would make `hipMalloc` viable at
   128 GiB/APU; probably unnecessary given §5.1, but worth one email.
6. ~~Is a 32-bit-prime engine worth building?~~ **Answered: no.** Three
   31-bit primes are 1.37× better on compute but 11 % worse on memory
   (RESULTS.md §9), and memory binds.

---

## 11. Sources

**NTT arithmetic** — Harvey, *Faster arithmetic for number-theoretic
transforms* (arXiv:1205.2926): Algorithms 3–5, the redundant [0,2p)/[0,4p)
representation, and the p < β/4 condition this design rests on ·
van der Hoeven, Lecerf, Quintin, *Modular SIMD arithmetic in Mathemagix*:
Function 16, the FP64 fma modular product benchmarked here ·
Bajard–Duquesne, Montgomery-friendly primes · Sugizaki–Takahashi,
*Improved implementation of NTT on NVIDIA GPU with Tensor Cores*
(SCA/HPCAsia 2026) — the matrix-core approach rejected in §7.1, and its
radix-2<sup>32</sup> Montgomery reduction.

**Transform structure** — Bailey, *FFTs in external or hierarchical memory*
(J. Supercomputing 1990): the four-step algorithm and the pass-minimisation
argument that becomes §4.4 · Ozcan, Javeed, Savaş, *High-performance NTT on GPU
through radix2-CT and 4-step algorithms* (IEEE Access 2025): kernel/block
partitioning for large n · van der Hoeven, *The truncated Fourier transform and
applications* (ISSAC '04) and Harvey–Roche, *An in-place truncated Fourier
transform* — the technique §7.2 declines.

**Bignum and constants** — Yee & Kondo, *10 trillion digits of π*: binary
splitting parallelisation, size-balanced splitting, arena allocation,
fault tolerance · Yee, y-cruncher notes on *Large Multiplication*,
*Binary Splitting*, *Radix Conversion* · Haible & Papanikolaou, *Fast
multiprecision evaluation of series of rational numbers* · Bernstein, *Scaled
remainder trees* and *Fast multiplication and its applications* ·
Hanrot, Quercia, Zimmermann, *The middle product algorithm I* ·
Brent & Zimmermann, *Modern Computer Arithmetic* · Percival, *Rapid
multiplication modulo the sum and difference of highly composite numbers* —
rigorous FFT rounding bounds, the reason an exact NTT is preferred here.

**Hardware** — `~/apucode/BENCHMARK_REPORT.md` (this node, 52 tests) ·
AMD *Instinct MI300 CDNA3 ISA* · *CDNA 3 white paper* ·
ROCm *MI300A system optimization* — the `amdttm.pages_limit` cap in §5.1 ·
Schieffer et al., *Inter-APU communication on MI300A via Infinity Fabric*
(arXiv:2508.11298) · Wahlgren et al., *Dissecting CPU-GPU unified physical
memory on MI300A* (arXiv:2508.12743).


---

## 18. Measured revisions

`RESULTS.md` records benchmark campaign 1 and supersedes parts of this
document. In particular: the fabric is not the second constraint (§5 there),
the XOR-swizzle guidance in §4.4 above needed qualifying, non-temporal loads
are harmful here, and a complete verified multiply now exists at
**1.66 bytes per decimal digit** of transform workspace and 1.98 ms per
60.6 M-digit product on one APU.
