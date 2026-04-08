[1;37m╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║    N U M B E R   T H E O R E T I C   T R A N S F O R M   —   T E C H N I C A L   R E F E R E N C E              ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝[0m

[1;33m  View with:  cat REFERENCE.md   or   less -R REFERENCE.md[0m

  Technical reference for the NTT/MI300A project. Five sections: HIP/ROCm C coding,
  NTT mathematics and algorithms, MI300A hardware architecture, post-quantum cryptography
  context, and Claude Code operational efficiency. Update when design decisions are made.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SECTION 1 — HIP/ROCm HIGH-PERFORMANCE C CODING[0m

  HIP provides a C++ API; this project uses only C-compatible constructs in kernel code.
  Compile with hipcc or direct Clang: --offload-arch=gfx942 (MI300A), gfx1100 (6900XT).
  HIPCC is being phased out in ROCm 6+ in favor of explicit Clang invocations. CRITICAL:
  AMD wavefront = 64 work-items, NOT 32. All occupancy and divergence math must use 64.

[1;36m  MEMORY HIERARCHY[0m

  [1;36m┌──────────────────┬──────────┬────────────┬──────────┬──────────────────────┬──────────────────────────────┐
  │ Level            │   Size   │ Bandwidth  │ Latency  │ SW Access            │ NTT Usage                    │
  ├──────────────────┼──────────┼────────────┼──────────┼──────────────────────┼──────────────────────────────┤[0m
  │ HBM3 (unified)   │  128 GB  │  5.3 TB/s  │ ~200 ns  │ hipMalloc / malloc   │ poly buffers, output arrays  │
  │ Infinity Cache   │  256 MB  │ ~1.5 TB/s  │ ~140 ns  │ hardware-managed     │ twiddle table (read-only)    │
  │ LDS (shared mem) │   64 KB  │  ~30 TB/s  │   ~5 cy  │ __shared__ keyword   │ butterfly staging, tiles     │
  │ VGPR registers   │ 512/lane │  reg file  │    1 cy  │ compiler allocates   │ live butterfly operands      │
  │ SGPR registers   │  16–102  │  reg file  │    1 cy  │ compiler allocates   │ loop counters, addresses     │
  [1;36m└──────────────────┴──────────┴────────────┴──────────┴──────────────────────┴──────────────────────────────┘[0m

  MI300A key distinction: HBM3 is physically unified — CPU malloc() and GPU hipMalloc()
  address the same memory. Zero-copy; no hipMemcpy overhead. CPU-prepared data immediately
  visible to GPU kernels. Use hipMemAdvise / hipMemPrefetchAsync to warm cache before launch.

[1;36m  KERNEL OPTIMIZATION TECHNIQUES[0m

  Difficulty:  [1;32m●○○○○[0m easy   [1;33m●●●○○[0m moderate   [1;31m●●●●●[0m expert
  Impact: throughput gain relative to naive implementation when technique is correctly applied.

  [1;36m┌────────────────────────────────┬──────────┬─────────────────┬──────────────────────────────────────────┐
  │ Technique                      │ Diff     │ Perf Impact     │ Guidance                                 │
  ├────────────────────────────────┼──────────┼─────────────────┼──────────────────────────────────────────┤[0m
  │ Memory coalescing              │ [1;32m●○○○○[0m    │ ×2–5 BW         │ Consec threads → consec addresses        │
  │ LDS tiling + reuse             │ [1;32m●●○○○[0m    │ ×5–20           │ Stage operands; pad LDS by +1 element    │
  │ LDS bank conflict avoidance    │ [1;32m●●○○○[0m    │ ×1.5–4          │ __shared__ T tile[M][N+1] (pad stride)   │
  │ Wavefront divergence elim.     │ [1;33m●●●○○[0m    │ ×1.5–3          │ Precompute branch selection; use bitmask │
  │ Register pressure / occupancy  │ [1;33m●●●○○[0m    │ ×1.5–2          │ __launch_bounds__; spill temps to LDS    │
  │ Lazy / deferred reduction      │ [1;32m●●○○○[0m    │ ×1.5–2          │ 64-bit headroom; reduce every ~2 stages  │
  │ Twiddle factor precomputation  │ [1;32m●○○○○[0m    │ eliminates cost │ CPU-side; store in Montgomery form       │
  │ Radix-4 / higher-radix         │ [1;33m●●●○○[0m    │ ×1.5–2          │ Halves stages; halves global mem passes  │
  │ const __restrict__ pointers    │ [1;32m●○○○○[0m    │ ×1.1–1.3        │ All read-only kernel args; enables cache │
  │ Stream overlap (hipStream_t)   │ [1;32m●●○○○[0m    │ hides latency   │ Overlap CPU prep with prior GPU kernel   │
  [1;36m└────────────────────────────────┴──────────┴─────────────────┴──────────────────────────────────────────┘[0m

  [1;32m★ Priority order:[0m  Coalescing → LDS tiling → lazy reduction → register control → radix-4.
    Profile first with rocprofv3 to determine whether kernel is compute-bound or BW-bound.
    Block size: multiples of 64 only. Recommended: 64, 128, 256. Match to n/2 per NTT stage.
    Query all parameters at runtime via hipGetDeviceProperties(); never hardcode arch constants.
    Cross-compilation: RDNA3 (6900XT) warpSize=32; CDNA3 (MI300A) warpSize=64. Use
    prop.warpSize in all grid/block arithmetic; compile with --offload-arch= per target.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SECTION 2 — NUMBER THEORETIC TRANSFORM: MATHEMATICS AND ALGORITHMS[0m

  The NTT is a DFT over Z_q (integers mod a prime q) rather than over the complex numbers.
  All results are exact; no floating-point error. Requirements: q prime, q ≡ 1 (mod n), and
  ω = g^((q-1)/n) mod q where g is a primitive root (generator) of Z_q*. NTT-friendly primes
  have form q = c·2^k + 1, permitting power-of-2 transform sizes up to 2^k.

  Notable NTT-friendly primes:
    q = 3329             = 13·2^8 + 1        ML-KEM / FIPS 203       ζ = 17,   n ≤ 256
    q = 8380417          = 2^23 − 2^13 + 1   ML-DSA / FIPS 204       ζ = 1753, n ≤ 2^23
    q = 998244353        = 119·2^23 + 1       general purpose         n ≤ 2^23
    q = 2^61 − 1         Mersenne prime       ultra-fast reduction     n ≤ 2^61

[1;36m  NTT BUTTERFLY ALGORITHMS[0m

  [1;36m┌────┬────────────────────────┬──────────────────────────┬──────────┬────────────────┬────────────────────────────────┐
  │ #  │ Algorithm              │ Input → Output Order     │ In-Place │ Mem Pattern    │ Best When                      │
  ├────┼────────────────────────┼──────────────────────────┼──────────┼────────────────┼────────────────────────────────┤[0m
  │ 2a │ Cooley-Tukey (DIT)     │ bit-reversed → natural   │ yes      │ strided        │ Forward NTT; pair with GS inv  │
  │ 2b │ Gentleman-Sande (DIF)  │ natural → bit-reversed   │ yes      │ strided        │ Inverse NTT; cancels bit-rev   │
  │ 2c │ [1;32mStockham (self-sorting)[0m │ natural → natural        │ no (2 buf) │ unit-stride  │ [1;32mGPU default; best coalescing  [0m│
  │ 2d │ [1;32mSix-step (cache-obliv.)[0m │ natural → natural        │ no (2 buf) │ block-local  │ [1;32mLarge n; L2/LDS tiling        [0m│
  │ 2e │ Pease                  │ bit-reversed → natural   │ yes      │ blocked        │ GPU-friendly; FPGA/parallel     │
  │ 2f │ Flat-NTT               │ natural → natural        │ no       │ row-major      │ Small n; entire array in LDS   │
  [1;36m└────┴────────────────────────┴──────────────────────────┴──────────┴────────────────┴────────────────────────────────┘[0m

  [1;32m★ Recommended:[0m  [1;32m2c Stockham[0m for GPU default — coalesced access, no bit-reversal permutation.
    [1;32m2d Six-step[0m for large n (n > L2 capacity): reshape to √n × √n matrix → row NTTs → twiddle
    multiply → transpose → row NTTs → transpose back. Each sub-NTT fits in L2 / LDS.
    DIT+DIF paired strategy: CT forward, GS inverse — bit-reversals cancel at the interface,
    saving one full permutation pass. NTTSuite benchmarks confirm Stockham leads on GPU.

[1;36m  MODULAR ARITHMETIC METHODS[0m

  [1;36m┌────┬──────────────────────┬──────────────────────┬──────────────────────────────┬────────────────────────────────┐
  │ #  │ Method               │ Cost                 │ Requirements                 │ Best When                      │
  ├────┼──────────────────────┼──────────────────────┼──────────────────────────────┼────────────────────────────────┤[0m
  │ 3a │ Naive division       │ ~20–40 cy per op     │ None                         │ Prototype / reference only     │
  │ 3b │ Barrett reduction    │ ~4 mul + shifts      │ Precompute k=⌊R²/q⌋          │ One-off reductions; simple     │
  │ 3c │ [1;32mMontgomery multiply  [0m│ ~4 mul + shifts      │ Convert to/from Mont. form    │ [1;32mNTT body; sustained mul chains [0m│
  │ 3d │ [1;32mLazy / deferred      [0m│ 0 (skip reduction)   │ 64-bit headroom; q < 2^30    │ [1;32mEvery ~2 butterfly stages      [0m│
  │ 3e │ Mersenne (q=2^k−1)   │ add + shift only     │ q must be Mersenne prime     │ Maximum speed; restricts q     │
  │ 3f │ Fermat  (q=2^k+1)    │ sub + conditional    │ q must be Fermat prime       │ Power-of-2 operand sizes       │
  [1;36m└────┴──────────────────────┴──────────────────────┴──────────────────────────────┴────────────────────────────────┘[0m

  [1;32m★ Recommended:[0m  [1;32mMontgomery (3c) + lazy reduction (3d)[0m in combination for the NTT inner loop.
    Montgomery: precompute R=2^32, R_inv=R^{-1} mod q, q'=−q^{-1} mod R. Maintain all values
    in Montgomery form throughout; single conversion at entry and exit. On GPU, integer division
    throughput is ~25× slower than multiply — Montgomery eliminates all divisions from hot path.
    Lazy: for q < 2^30, (a+b) < 2q fits in 32 bits; accumulate across ~2 stages before reducing.

  Negacyclic convolution over Z_q[X]/(X^n+1):
    Method A — Pre/post-twist: multiply input by ψ^i (ψ = primitive 2n-th root, ψ²=ω); run
      standard NTT; post-multiply inverse output by ψ^{-i}. Cost: 2n extra multiplications.
    Method B — Kyber split: X^{256}+1 factors into 128 degree-2 polynomials mod 3329. NTT
      outputs 128 pairs; only 7 butterfly layers. Basemul on 128 quadratics mod (X²−ζ^{2i+1}).
      No pre-twist needed. Saves one butterfly layer vs. naive negacyclic approach.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SECTION 3 — AMD MI300A HARDWARE ARCHITECTURE[0m

  The MI300A (CDNA3) is an APU combining 24 Zen 4 CPU cores with 228 CDNA3 GPU compute units
  and 128 GB unified HBM3 on a 3D-stacked chiplet package (6 XCDs + 3 CCDs + 4 IODs). The
  defining feature: no separate GPU VRAM. CPU and GPU share one physical address space with
  hardware cache coherency over Infinity Fabric. No hipMemcpy latency. CPU-prepared data is
  directly GPU-accessible. Sub-250 ns CPU↔GPU atomic latency even cross-socket.

[1;36m  COMPUTE RESOURCES[0m

  [1;36m┌──────────────────────┬────────────────────────────────────────┬──────────────────────────────────────────────┐
  │ Component            │ Specification                          │ NTT Relevance                                │
  ├──────────────────────┼────────────────────────────────────────┼──────────────────────────────────────────────┤[0m
  │ XCDs (GPU dies)      │ 6 dies × 38 CUs = 228 CUs total       │ Total parallelism for butterfly stages       │
  │ Wavefront width      │ [1;32m64 work-items per wavefront[0m            │ [1;32mBlock size MUST be multiple of 64[0m          │
  │ LDS per CU           │ 64 KB shared among workgroup           │ ~8K uint64 operands per CU staging           │
  │ VGPR pool per lane   │ 512 VGPRs (shared occupancy pool)      │ 256 VGPRs → 2 wavefronts/CU; tune carefully │
  │ SGPR per wavefront   │ 16–102 (in units of 16)                │ Loop vars, twiddle pointer, mod constants    │
  │ HBM3 stacks          │ 8 × 16 GB = 128 GB unified             │ No memcpy; CPU alloc GPU-accessible          │
  │ HBM3 bandwidth       │ [1;33m5.3 TB/s aggregate (GPU-side)[0m         │ [1;33mNTT is BW-bound at large n[0m              │
  │ CPU HBM bandwidth    │ ~212 GB/s effective (CPU-side)         │ CPU prep at full speed; hand off to GPU      │
  │ Infinity Cache (LLC) │ 256 MB shared, memory-side             │ Twiddle tables ≤ 256 KB stay warm in L2     │
  │ MFMA matrix cores    │ 912 units; FP64/FP32/FP16/BF16/FP8    │ [1;31mFP-only; int NTT requires workaround[0m      │
  │ Zen 4 CPU cores      │ 24 cores (3 CCDs × 8 cores), 3.7 GHz  │ Input prep, bit-reversal, verification       │
  │ Partitioning SPX     │ [1;32mAll 228 CUs as one device[0m              │ [1;32mUse SPX for maximum single-job bandwidth[0m   │
  │ Partitioning CPX     │ 6 partitions, 38 CUs / 16 GB each     │ Multi-tenant or parallel NTT instances       │
  [1;36m└──────────────────────┴────────────────────────────────────────┴──────────────────────────────────────────────┘[0m

[1;36m  RUNTIME INTROSPECTION  (query at startup — never hardcode architecture constants)[0m

  [1;36m┌────────────────────────────────┬──────────────────────────────┬──────────────────────────────────────────┐
  │ hipDeviceProp_t field          │ MI300A Value                 │ Use In NTT Kernel Configuration          │
  ├────────────────────────────────┼──────────────────────────────┼──────────────────────────────────────────┤[0m
  │ warpSize                       │ 64                           │ Block size multiple; divergence budget   │
  │ sharedMemPerBlock              │ 65536  (64 KB)               │ Max LDS tile size per workgroup          │
  │ regsPerBlock                   │ 65536  (512 per lane × 128)  │ Register budget; tune __launch_bounds__  │
  │ multiProcessorCount            │ 228                          │ Grid size target for full CU occupancy   │
  │ memoryClockRate + memoryBusWidth│ HBM3 clock / 8192-bit bus   │ Theoretical BW; compare to achieved     │
  │ totalGlobalMem                 │ ~128 GB                      │ Maximum NTT problem size without chunking│
  [1;36m└────────────────────────────────┴──────────────────────────────┴──────────────────────────────────────────┘[0m

  [1;32m★ MFMA note:[0m  Matrix cores are FP-only. For integer NTT: FP64 can represent exact integers up
    to 2^52, enabling modular multiply for small q (q < 2^26) via FP64 trick — compute a·b in
    FP64, subtract q·round(a·b/q). Validate carefully. Design kernel dispatch for modularity:
    integer MFMA may arrive in future CDNA generations; NTT kernel should be swappable without
    changing caller code (CLAUDE.md modularity rule §1).

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SECTION 4 — LATTICE-BASED PQC: STRUCTURES AND NTT REQUIREMENTS[0m

  Lattice PQC schemes operate over polynomial rings R_q = Z_q[X]/(X^n+1). The hard problems
  (Ring-LWE, Module-LWE) require polynomial multiplication as their core arithmetic operation.
  NTT reduces polynomial multiplication from O(n²) to O(n log n). For Module-LWE, polynomials
  appear as k×k matrices; k parallel NTTs per matrix row/column. NTT throughput equals KEM and
  signature throughput — it is the primary compute bottleneck in all standardized PQC schemes.

[1;36m  PQC SCHEME NTT PARAMETERS[0m

  [1;36m┌─────────────┬──────┬──────────┬───────────────┬───────────┬───────────┬────────────────┐
  │ Scheme      │  n   │    q     │  Root ζ / ψ   │ Module k  │ NTTs/Op   │ Standard       │
  ├─────────────┼──────┼──────────┼───────────────┼───────────┼───────────┼────────────────┤[0m
  │ ML-KEM-512  │  256 │    3329  │ ζ = 17        │ k = 2     │   6–10    │ FIPS 203       │
  │ ML-KEM-768  │  256 │    3329  │ ζ = 17        │ k = 3     │  10–15    │ FIPS 203       │
  │ ML-KEM-1024 │  256 │    3329  │ ζ = 17        │ k = 4     │  14–20    │ FIPS 203       │
  │ ML-DSA-44   │  256 │ 8380417  │ ζ = 1753      │ 4 × 4     │  ~24      │ FIPS 204       │
  │ ML-DSA-65   │  256 │ 8380417  │ ζ = 1753      │ 6 × 5     │  ~36      │ FIPS 204       │
  │ ML-DSA-87   │  256 │ 8380417  │ ζ = 1753      │ 8 × 7     │  ~50      │ FIPS 204       │
  │ FHE (BFV)   │ 2^13+│ RNS set  │ per prime     │ 1 scalar  │ 2/poly op │ custom HE std  │
  [1;36m└─────────────┴──────┴──────────┴───────────────┴───────────┴───────────┴────────────────┘[0m

  ML-KEM structural detail: q=3329, X^{256}+1 factors into 128 irreducible degree-2 polynomials
  mod q. NTT outputs 128 pairs, NOT 256 scalars. Only 7 butterfly layers (not 8). Multiplication
  in NTT domain = 128 independent basemul ops on (a₀+a₁X)(b₀+b₁X) mod (X²−ζ^{2i+1}).
  This eliminates one butterfly layer vs. naive negacyclic and removes the need for pre-twisting.

[1;36m  NTT ENGINE DESIGN REQUIREMENTS  (derived from all relevant PQC and FHE workloads)[0m

  [1;36m┌──────────────────────────┬──────────────────────────────────────┬────────────────────────────────────────────┐
  │ Feature                  │ Value / Range                        │ Rationale                                  │
  ├──────────────────────────┼──────────────────────────────────────┼────────────────────────────────────────────┤[0m
  │ Transform size n         │ [1;32m256 to 2^17, power-of-2, runtime arg[0m  │ ML-KEM n=256; FHE n up to 131072           │
  │ Modulus q                │ [1;32mAny NTT-friendly prime, runtime arg[0m   │ 3329, 8380417, 998244353, or custom        │
  │ Convolution type         │ [1;32mNegacyclic (X^n+1) primary[0m            │ Required for ML-KEM, ML-DSA, RLWE-FHE     │
  │ Batching                 │ k polys in parallel (k = 2 to 1000+) │ k=2..4 for Kyber; thousands for FHE batch  │
  │ Integer width            │ 32-bit (q<2^30);  64-bit (q≥2^30)   │ 32-bit packs 2 coefficients per register   │
  │ Output domain option     │ NTT-domain or fully reduced           │ Keep NTT-domain for chained poly-mul chains│
  │ Multi-prime (RNS) mode   │ Independent NTT per RNS prime        │ FHE prime ladders; each prime is one NTT   │
  │ Public API               │ forward, inverse, basemul, pointwise │ Caller composes; library stays fully modular│
  [1;36m└──────────────────────────┴──────────────────────────────────────┴────────────────────────────────────────────┘[0m

  [1;32m★ API design rule:[0m  Expose ntt_forward(buf, n, q, w), ntt_inverse(buf, n, q, w), and
    ntt_pointwise_mul(a, b, out, n) as the stable public interface. All parameters (n, q, ω)
    passed at runtime, not compile-time. Internal kernel selection (Stockham vs Six-step) keyed
    on n vs LLC size, chosen automatically at dispatch. CPU kernel and GPU kernel share the same
    signature — switching from CPU to GPU changes only the dispatch layer, not the caller.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SECTION 5 — CLAUDE CODE: TOKEN EFFICIENCY, CONTROL, AND QUALITY PRACTICES[0m

  Claude Code sessions consume tokens proportional to context size: system prompt, loaded
  CLAUDE.md/rules files, all tool results, and conversation history. Performance degrades
  as the context window fills. The mechanisms below give you direct, deterministic control
  over Claude's behavior — not just guidance, but enforcement via hooks and permission rules.

[1;36m  TOKEN CONSUMPTION ARCHITECTURE[0m

  [1;36m┌──────────────────────────────────────┬──────────────┬────────────────────────────────────────┐
  │ Context Component                    │ Typical Size │ Control Mechanism                      │
  ├──────────────────────────────────────┼──────────────┼────────────────────────────────────────┤[0m
  │ System prompt (built-in)             │ ~10K tokens  │ Fixed; cannot reduce                   │
  │ CLAUDE.md files (loaded at startup)  │ variable     │ Keep under 200 lines; use @imports     │
  │ .claude/rules/ (path-scoped)         │ on-demand    │ Load only when matching files opened   │
  │ Auto memory MEMORY.md (first 200 ln) │ ≤200 lines   │ Claude manages; audit with /memory     │
  │ Tool results (Read, Bash, Grep, etc.)│ variable     │ head_limit; hooks prefilter output     │
  │ Extended thinking tokens             │ up to 31K    │ MAX_THINKING_TOKENS=8000 or /effort    │
  │ Conversation history                 │ grows w/ time│ /compact, /clear, /rewind              │
  [1;36m└──────────────────────────────────────┴──────────────┴────────────────────────────────────────┘[0m

[1;36m  CLAUDE.md ARCHITECTURE — BEST PRACTICES[0m

  [1;36m┌─────────────────────────┬──────────────────────────────────────────────────────────────────┐
  │ Principle                │ Implementation                                                   │
  ├─────────────────────────┼──────────────────────────────────────────────────────────────────┤[0m
  │ Size limit               │ Under 200 lines per file; longer files degrade adherence         │
  │ @imports                 │ @path/to/file — expands inline at load time                      │
  │ .claude/rules/ directory │ Path-scoped rules: load only when matching files are opened      │
  │ HTML comments            │ <!-- notes --> stripped from Claude context; visible to humans   │
  │ Hierarchy                │ managed policy > project > user > CLAUDE.local.md (gitignored)   │
  │ Compact instructions     │ Add "# Compact instructions" section to guide summarization      │
  │ Specificity              │ "Use 2-space indent" not "format code nicely"                    │
  │ Skills vs CLAUDE.md      │ Skills load ON DEMAND; CLAUDE.md loads EVERY session             │
  [1;36m└─────────────────────────┴──────────────────────────────────────────────────────────────────┘[0m

  Path-scoped rules (.claude/rules/example.md):  Put YAML frontmatter "paths: [src/**/*.{c,h}]"
  so the rule file only loads when Claude opens a matching .c or .h source file.
  HTML comments in CLAUDE.md (<!-- like this -->) are stripped before context injection —
  use them for maintainer notes that consume zero tokens.

[1;36m  HOOKS — DETERMINISTIC CONTROL LAYER[0m

  Hooks are shell commands that run at lifecycle points regardless of Claude's reasoning.
  They are the most powerful mechanism for enforcing project rules. A PreToolUse hook that
  returns permissionDecision:"deny" blocks the tool even in bypassPermissions mode — it
  cannot be bypassed by the user changing permission settings.
  Location: .claude/settings.json (project, committable) or ~/.claude/settings.json (global).

  [1;36m┌──────────────────────┬──────────┬───────────────────────────────────────────────────────┐
  │ Hook Event            │ Can Block│ When It Fires                                         │
  ├──────────────────────┼──────────┼───────────────────────────────────────────────────────┤[0m
  │ PreToolUse            │ [1;32mYES[0m      │ Before tool call; deny or modify input before exec    │
  │ PostToolUse           │ no       │ After tool succeeds; inject feedback into context      │
  │ SessionStart          │ no       │ On startup/resume/clear/[1;32mcompact[0m — inject context        │
  │ Stop                  │ [1;32mYES[0m      │ When Claude finishes; prompt/agent hook can continue  │
  │ PermissionRequest     │ [1;32mYES[0m      │ Before permission dialog; auto-allow or auto-deny     │
  │ PreCompact/PostCompact│ no       │ Before/after context compaction                       │
  │ UserPromptSubmit      │ no       │ When prompt submitted; add context before Claude sees  │
  │ ConfigChange          │ [1;32mYES[0m      │ When settings/skills change; can block                │
  │ SubagentStart/Stop    │ no       │ When subagent spawns or finishes                       │
  [1;36m└──────────────────────┴──────────┴───────────────────────────────────────────────────────┘[0m

  Exit codes: 0 = allow, 2 = block (stderr → Claude feedback), other = allow with error notice.
  Structured JSON to stdout (exit 0) for fine-grained control: deny, ask, allow, or modify input.

  [1;32mSessionStart (compact) re-injection:[0m  Use matcher:"compact" to re-inject STATUS.md and
  critical state after context compaction — CLAUDE.md survives compaction but conversation
  context does not. Hook stdout is added to Claude's context automatically.

  [1;32mStop hook verification:[0m  A type:"prompt" Stop hook sends a question to a Haiku model.
  If the model returns {ok:false, reason:...}, Claude continues and uses the reason as its
  next instruction. Guard against loops: check stop_hook_active field in hook JSON input.

  [1;32mPreToolUse input modification (v2.0.10+):[0m  Hook can rewrite tool input JSON before
  execution — redirect "grep ..." commands to ripgrep, add flags, sanitize paths.

[1;36m  RECOMMENDED ENFORCEMENT HOOKS FOR THIS PROJECT[0m

  [1;36m┌────┬────────────────────────────────────┬──────────────────────┬──────────────────────────┐
  │ #  │ Enforcement Goal                   │ Hook Event           │ Mechanism                │
  ├────┼────────────────────────────────────┼──────────────────────┼──────────────────────────┤[0m
  │ E1 │ [1;32mBlock Bash grep/cat/find/head/tail[0m  │ PreToolUse (Bash)    │ Pattern match; exit 2    │
  │ E2 │ [1;32mRe-inject STATUS.md after compact[0m   │ SessionStart:compact │ cat STATUS.md to stdout  │
  │ E3 │ [1;32mVerify STATUS.md updated at Stop[0m    │ Stop (prompt type)   │ Haiku checks mtime       │
  │ E4 │ [1;32mBlock rm -rf / force-push[0m           │ PreToolUse (Bash)    │ Pattern match; exit 2    │
  │ E5 │ Remind about STATUS.md after Edit  │ PostToolUse (Edit)   │ Inject feedback message  │
  │ E6 │ Log all bash commands              │ PostToolUse (Bash)   │ Append to .claude/cmd.log│
  │ E7 │ Notify when Claude needs input     │ Notification         │ notify-send (Linux)      │
  [1;36m└────┴────────────────────────────────────┴──────────────────────┴──────────────────────────┘[0m

[1;36m  SESSION MANAGEMENT COMMANDS[0m

  [1;36m┌──────────────────────┬────────────────────────────────────────────────────────────────┐
  │ Command              │ Effect and When to Use                                         │
  ├──────────────────────┼────────────────────────────────────────────────────────────────┤[0m
  │ /compact             │ Summarize history; free context. Use at every unit boundary.   │
  │ /compact <focus>     │ Guide what survives: /compact Focus on API and test results    │
  │ /clear               │ Full reset. Use when switching to unrelated work.              │
  │ /cost                │ Show token usage breakdown for current session (API users).    │
  │ /context             │ Visualize what is consuming context space right now.           │
  │ /memory              │ Browse CLAUDE.md, rules, and auto memory; toggle auto memory.  │
  │ /rewind              │ Restore conversation and code to a previous checkpoint.        │
  │ /effort low          │ Reduce extended thinking depth for simple tasks.               │
  │ /model               │ Switch model mid-session (Opus → Sonnet → Haiku).             │
  │ /hooks               │ Browse all configured hooks (read-only; edit settings.json).  │
  [1;36m└──────────────────────┴────────────────────────────────────────────────────────────────┘[0m

[1;36m  COST AND TOKEN REDUCTION STRATEGIES[0m

  Difficulty:  [1;32m●○○○○[0m easy to apply   [1;33m●●●○○[0m requires setup   [1;31m●●●●●[0m significant implementation

  [1;36m┌────────────────────────────────────┬──────────────┬──────────┬────────────────────────────┐
  │ Strategy                           │ Token Savings│ Effort   │ How to Apply               │
  ├────────────────────────────────────┼──────────────┼──────────┼────────────────────────────┤[0m
  │ [1;32mSTATUS.md as session navigator[0m      │ very high    │ [1;32m●○○○○[0m    │ Read it first; never skip  │
  │ [1;32m/compact at unit boundaries[0m         │ very high    │ [1;32m●○○○○[0m    │ After every completed unit │
  │ [1;32mhead_limit on all Grep calls[0m        │ high         │ [1;32m●○○○○[0m    │ Default 250; use always    │
  │ [1;32mGrep before reading files[0m           │ high         │ [1;32m●○○○○[0m    │ Find symbol first; read 2nd│
  │ [1;32mStore bench results in files[0m        │ very high    │ [1;32m●○○○○[0m    │ Benchmarks write to disk   │
  │ [1;32mOne unit per session[0m                │ very high    │ [1;32m●○○○○[0m    │ Update STATUS.md; stop     │
  │ Hooks block Bash grep/cat/find     │ very high    │ [1;33m●●●○○[0m    │ .claude/settings.json hook │
  │ SessionStart:compact re-injection  │ high         │ [1;33m●●●○○[0m    │ Hook cats STATUS.md        │
  │ Path-scoped rules in .claude/rules/│ moderate     │ [1;33m●●○○○[0m    │ C rules only load for .c   │
  │ MAX_THINKING_TOKENS=8000           │ high         │ [1;33m●●○○○[0m    │ In settings.json env block │
  │ Skills for workflow docs           │ moderate     │ [1;33m●●●○○[0m    │ On-demand vs always-loaded │
  │ Subagents with model: haiku        │ high         │ [1;33m●●○○○[0m    │ Research, doc fetch, logs  │
  │ PreToolUse hook filter test output │ high         │ [1;31m●●●●○[0m    │ Grep ERROR before Claude   │
  │ /clear between unrelated tasks     │ high         │ [1;32m●○○○○[0m    │ Manual discipline          │
  [1;36m└────────────────────────────────────┴──────────────┴──────────┴────────────────────────────┘[0m

  [1;32m★ Session protocol:[0m  (1) Read STATUS.md → (2) Grep/Glob for target → (3) Read only what
    will be edited → (4) Edit → (5) Minimal verify (smoke test or single unit test) →
    (6) Update STATUS.md → (7) /compact before next task or ending session.
    Use plan mode (Shift+Tab) before complex multi-file changes to catch wrong-direction early.
    Press Escape immediately if heading wrong; use /rewind to restore checkpoint.

  Code quality rules (RULES.md):
    · Comments must match current code exactly — update them when code changes.
    · Terminal output: neat, formatted tables with fixed-width columns; not raw dumps.
    · Benchmark over parameter ranges; store results in timestamped files.
    · System calls to discover hardware sizes; never hardcode architecture constants.
    · Trade compute time for tokens: run longer benchmarks rather than reading more files.

