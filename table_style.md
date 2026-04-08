[1;37m╔════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║        G E N E R A L   N U M B E R   F I E L D   S I E V E   —   A L G O R I T H M S   G U I D E                  ║
╚════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝[0m

[1;33m  View with:  cat ALGORITHMS.md   or   less -R ALGORITHMS.md[0m

  Factoring an n-digit semiprime N = p·q with GNFS requires six coordinated stages. Each stage offers
  multiple algorithm choices that trade implementation complexity for runtime performance. This guide
  presents those choices with practical metrics for a Rust implementation in C style (no OOP).

[1;36m  RUNTIME DISTRIBUTION  (typical n ≈ 100 digit semiprime)[0m
  ┌──────────────────────────────────────┬──────────┬────────────────────────────────────────────────┐
  │ Segment                              │  Share   │ Notes                                          │
  ├──────────────────────────────────────┼──────────┼────────────────────────────────────────────────┤
  │ 1. Polynomial Selection              │   ~5 %   │ One-time; result reused for entire factoring   │
  │ 2. Factor Base Construction          │   ~1 %   │ Cheap when small_primes are already built      │
  │ 3. Sieving                           │  ~70 %   │ Dominant cost; embarrassingly parallel         │
  │ 4. Matrix / Linear Algebra           │  ~20 %   │ Scales as B² where B = factor base size        │
  │ 5. Algebraic Square Root             │   ~3 %   │ GCD-heavy; rarely the bottleneck               │
  │ 6. Factor Recovery                   │   ~1 %   │ Trivial once square root pair is in hand       │
  └──────────────────────────────────────┴──────────┴────────────────────────────────────────────────┘

  Difficulty:  [1;32m●○○○○[0m easy   [1;33m●●●○○[0m moderate   [1;31m●●●●●[0m expert
  Seg Perf:    ×1.0 = simplest option baseline within that segment.
  Overall:     effect on total factoring wall-time (Seg 3 dominates at ~70%).
  LOC †:       remaining new code to write; existing poly.rs + poly_select.rs already written.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 1 — POLYNOMIAL SELECTION[0m  (≈5% runtime)

  Goal: find a degree-d polynomial f(x) ∈ Z[x] with a root m mod N and small coefficients,
  plus linear g(x) = x − m.  Smaller coefficients → smoother sieve values → more relations.

  A better polynomial reduces the number of (a,b) pairs the sieve must inspect to collect enough
  relations.  Since sieving is ~70% of runtime, polynomial quality has a large OVERALL impact.
  Rule of thumb: Murphy-E gain of ×5 in poly quality ≈ ×3–4 reduction in sieve work overall.

  [1;36m┌────┬──────────────────────────────────┬───────────────────────────┬──────────┬───────┬──────────┬──────────────┬──────────────────┐
  │ #  │ Option                           │ Complexity                │ Diff     │  LOC  │ Seg Perf │ Overall      │ Memory           │
  ├────┼──────────────────────────────────┼───────────────────────────┼──────────┼───────┼──────────┼──────────────┼──────────────────┤[0m
  │ 1a │ Base-m (Knuth-Schroeppel)        │ O(N^(1/d))                │ [1;32m●○○○○[0m    │[0;37m   50  [0m│  ×1      │ baseline     │ < 1 KB           │
  │ 1b │ Kleinjung / Montgomery skew      │ O(N^(1/d) · log skew)     │ [1;33m●●●○○[0m    │[0;37m  180  [0m│  ×2      │ ~×1.3 total  │ < 10 KB          │
  │ 1c │[1;32m Murphy-E α-scoring+search✓       [0m│ O(N^(1/d) · α scoring)    │ [1;33m●●●●○[0m    │[1;32m  369  [0m│  ×6      │ ~×2–4 total  │ < 50 KB          │
  │ 1d │ CADO-NFS full polynomial sel.    │ sub-exp in log N          │ [1;31m●●●●●[0m    │[0;37m 2800  [0m│  ×20     │ ~×5–8 total  │ hundreds KB      │
  [1;36m└────┴──────────────────────────────────┴───────────────────────────┴──────────┴───────┴──────────┴──────────────┴──────────────────┘[0m

  [1;32m★ Selected:[0m    [1;32m1c[0m Murphy-E α-scoring + rotation search — implemented in poly_select.rs.
    Base-m: choose m = round(N^(1/d)), express N in base m → coefficients are the digits.
    Degree d = 4 for N < 80 digits, d = 5 for N = 80–120 digits, d = 6 for N > 120 digits.
    dashu IBig handles the N^(1/d) root and coefficient arithmetic directly.
    Three-stage search + dense m-grid. Stage 1: root-sieving proxy score (30 primes) across
    201 bases (m’=m+k, k∈[-100,+100]) × 10× wider rotation range. Stage 2: full
    Murphy-E for global top-k candidates. disc<0 enforced; disc_omega N_CHARS formula.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 2 — FACTOR BASE CONSTRUCTION[0m  (≈1% runtime)

  Goal: enumerate primes p ≤ B; for each, find all roots r of f(r) ≡ 0 (mod p).
  These (p, r) pairs are the algebraic factor base.  Also build rational base: all p ≤ B.

  This segment is ~1% of runtime, so even a ×15 within-segment gain moves total time by < 0.2%.
  The CHOICE of B, however, has a large overall impact (see note below table).

  [1;36m┌────┬──────────────────────────────────┬───────────────────────────┬──────────┬───────┬──────────┬──────────────┬──────────────────┐
  │ #  │ Option                           │ Complexity                │ Diff     │  LOC  │ Seg Perf │ Overall      │ Memory           │
  ├────┼──────────────────────────────────┼───────────────────────────┼──────────┼───────┼──────────┼──────────────┼──────────────────┤[0m
  │ 2a │ Trial root-finding mod p         │ O(B · d · p)              │ [1;32m●○○○○[0m    │[0;37m   10† [0m│  ×1      │ < 0.1% total │ O(B) integers    │
  │ 2b │ Tonelli-Shanks (degree-2 f)      │ O(B · log² p)             │ [1;32m●●○○○[0m    │[0;37m   70† [0m│  ×3      │ < 0.1% total │ O(B) integers    │
  │ 2c │[1;32m Berlekamp mod p (general deg)✓   [0m│ O(B · d² log p)           │ [1;33m●●●○○[0m    │[1;32m  265  [0m│  ×5      │ < 0.1% total │ O(B · d)         │
  │ 2d │ NTT-based batch root-finding     │ O(B log B log log B)      │ [1;33m●●●●○[0m    │[0;37m  300† [0m│  ×15     │ < 0.1% total │ O(B log B)       │
  [1;36m└────┴──────────────────────────────────┴───────────────────────────┴──────────┴───────┴──────────┴──────────────┴──────────────────┘[0m

  [1;32m★ Selected:[0m    [1;32m2c[0m — Berlekamp handles arbitrary degree, is well-documented, and reuses your
    existing small_primes vector.  build_factor_base parallelised over primes via Rayon.  GNFS-optimal: B ≈ exp(0.96×(ln N)^{1/3}×(ln ln N)^{2/3}).
    Practical: ~640 at 40-bit N, ~3800 at 64-bit, ~10400 at 80-bit, ~120000 at 128-bit.
    concrete-ntt makes 2d viable: reduced from +600 to ~+400 LOC; use over Z/pZ directly.
    Note: B choice affects Seg 3 sieve density AND Seg 4 matrix size (B² cost) — optimise jointly.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 3 — SIEVING[0m  (≈70% runtime)

  Goal: find (a, b) pairs with gcd(a,b)=1 such that both F(a,b) = b^d·f(a/b) and G(a,b) = |a + bm|
  are B-smooth (all prime factors ≤ B).  Each such pair is a relation.  Collect ≥ B + margin relations.

  Sieving dominates total runtime.  Within-segment gains translate almost 1:1 to overall wall-time
  reduction.  A ×40 within-segment speedup ≈ ×28 overall (40 × 70% = 28pp saved of total).

  [1;36m┌────┬──────────────────────────────────┬───────────────────────────┬──────────┬───────┬──────────┬──────────────┬──────────────────┐
  │ #  │ Option                           │ Complexity                │ Diff     │  LOC  │ Seg Perf │ Overall      │ Memory           │
  ├────┼──────────────────────────────────┼───────────────────────────┼──────────┼───────┼──────────┼──────────────┼──────────────────┤[0m
  │ 3a │ Trial division (no sieve)        │ O(R · B / ln B)           │ [1;32m●○○○○[0m    │[0;37m   45† [0m│  ×1      │ baseline     │ O(B)             │
  │ 3b │ Line sieve — rational side only  │ O(R · ln B)               │ [1;32m●●○○○[0m    │[0;37m  165† [0m│  ×20     │ ~×14 total   │ O(B + sieve)     │
  │ 3c │ Line sieve — both sides          │ O(R · ln B)               │ [1;33m●●●○○[0m    │[0;37m  315† [0m│  ×40     │ ~×28 total   │ O(B + sieve)     │
  │ 3d │ Log sieve + threshold test       │ O(R · ln B)               │ [1;33m●●●○○[0m    │[0;37m  255† [0m│  ×80     │ ~×56 total   │ O(B + sieve)     │
  │ 3e │ Special-q lattice sieve✓         │ O(R · ln B / q)           │ [1;33m●●●●○[0m    │  767  │  ×200    │ ~×140 total  │ O(B + L1 seg)    │
  │ 3f │[1;32m Special-q + bucket sieve (CADO)  [0m│ O(R · ln B / q)           │ [1;31m●●●●●[0m    │[1;32m 1289  [0m│  ×500    │ ~×350 total  │ S6+S5: +210 LOC  │
  [1;36m└────┴──────────────────────────────────┴───────────────────────────┴──────────┴───────┴──────────┴──────────────┴──────────────────┘[0m

  Sieve inner-loop sub-options (additive; combine with any row above):

  [1;36m┌────┬──────────────────────────────────┬───────────────────────────┬──────────┬───────┬──────────┬──────────────┬──────────────────┐
  │ #  │ Sub-option                       │ Description               │ Diff     │ +LOC  │ Seg Perf │ Overall +    │ Notes            │
  ├────┼──────────────────────────────────┼───────────────────────────┼──────────┼───────┼──────────┼──────────────┼──────────────────┤[0m
  │ S1 │[1;32m Binary sieve (mark 0/1)✓         [0m│ exact; like your π(x) code│ [1;32m●○○○○[0m    │[1;32m   50  [0m│  ×1      │ —            │ impl (bench)     │
  │ S2 │[1;32m Log sieve (accumulate log p)✓    [0m│ approx; threshold test    │ [1;32m●●○○○[0m    │[1;32m   75  [0m│  ×3      │ ~×2.1 total  │ standard GNFS    │
  │ S3 │[1;32m L1-cache-blocked segments✓       [0m│ fits cache; better TLB    │ [1;32m●●○○○[0m    │[1;32m   50  [0m│  ×5      │ ~×3.5 total  │ reuse your seg   │
  │ S4 │[1;32m Rayon parallel over special-q✓   [0m│ embarrassingly parallel   │ [1;32m●●○○○[0m    │[1;32m   25  [0m│  ×cores  │ ×cores total │ reuse rayon      │
  │ S5 │[1;32m 1LP partial relations✓           [0m│ persistent LP accumulator │ [1;32m●●●●○[0m    │[1;32m   70  [0m│  ×1 fill │ ~×1.3–1.5    │ impl (this proj) │
  │ S6 │[1;32m Bucket sieve; med+large primes✓  [0m│ sort by L2-block; apply   │ [1;32m●●●●●[0m    │[1;32m   55  [0m│  ×2–5    │ ~×1.5–3 total│ impl (this proj) │
  │ S7 │[1;32m SIMD fill, small primes (p≤19)✓  [0m│ stride ≤ 19; SIMD fill    │ [1;32m●●●●●[0m    │[1;32m  150  [0m│  ×1.25   │ ~×1.1–1.4 tot│ impl (this proj) │
  │ S8 │[1;32m Skew-adjusted Gauss reduction✓   [0m│ weights by polynomial skew│ [1;32m●●○○○[0m    │[1;32m   35  [0m│  ×1      │ better yield │ impl (this proj) │
  │ S9 │[1;32m Auto-scale lp_bits (log2 B)✓     [0m│ threshold slack: log2 B   │ [1;32m●○○○○[0m    │[1;32m    5  [0m│  ×1      │ better yield │ impl (this proj) │
  │S10 │[1;32m Native-int smoothness (i128)✓    [0m│ i128 arithmetic; skip IBig│ [1;32m●●○○○[0m    │[1;32m   60  [0m│  ×1      │ ×1.0–1.1 tot │ impl (this proj) │
  │S11 │[1;32m SIMD fill enhancements (S11+S12)✓[0m│ S11 incr k0+S12 AVX2 32B  │ [1;32m●●○○○[0m    │[1;32m   40  [0m│  ×1.0    │ part of -20% │ impl (this proj) │
  │S13 │[1;32m SIMD harvest scan (movemask)✓    [0m│ movemask scan; 16B/iter   │ [1;32m●●○○○[0m    │[1;32m   35  [0m│ ×1.0–1.1 │ part of -20% │ impl (this proj) │
  │S14 │[1;32m u64 trial division fast path✓    [0m│ fval≤u64::MAX; u64 div    │ [1;32m●○○○○[0m    │[1;32m    8  [0m│ ×1.0–1.1 │ x1.05 smooth │ impl (this proj) │
  │S15 │[1;32m Cofactor primality check (S5)✓   [0m│ prime test each cofactor  │ [1;32m●○○○○[0m    │[1;32m   20  [0m│  ×1.0    │ ×1.1 partial │ impl (this proj) │
  │S16 │[1;32m Trial div: early exit p^2>cof✓   [0m│ p^2>fv&&gv; both prime    │ [1;32m●○○○○[0m    │[1;32m    4  [0m│ ×1.0–1.1 │ ×1.87 trial  │ impl (this proj) │
  │S17 │[1;32m Trial div: libdivide reciprocal✓ [0m│ precomputed recip; no DIV │ [1;32m●●○○○[0m    │[1;32m   30  [0m│ ×4.9×    │ ×4.9× trial  │ direct; no crate │
  │S18 │[1;32m Batch GCD smoothness test✓       [0m│ prod/rem tree; N>>B·logB  │ [1;32m●●●●○[0m    │[1;32m  150  [0m│ ×3–5 N>>B│ ×1.5–2 total │ N>>50k cands     │
  │S19 │ GPU cofactorization (ECM/ROCm)   │ ECM on GPU; sieve on CPU  │ [1;31m●●●●●[0m    │[0;37m  4000 [0m│ ×2–3 cof │ ×1.2–1.5 tot │ ROCm/HIP; N>70b  │
  │S20 │[1;32m 2D Lattice Sieve (strip)✓        [0m│ strip projection; N>80b   │ [1;31m●●●●●[0m    │[1;32m  206  [0m│ ×2–3 N>80b│ ×1.5–2 total │ impl (sieve_2d)  │
  [1;36m└────┴──────────────────────────────────┴───────────────────────────┴──────────┴───────┴──────────┴──────────────┴──────────────────┘[0m

  [1;32m★ Selected:[0m    [1;32m3f[0m + S2+S3+S4 — log sieve, L1-blocked, Rayon. S1–S18, S20 implemented.
    Threshold: sieve_val ≥ log₂ F(a,b) − slack, where slack ≈ log₂(large-prime bound).
    S5+S6+3f+S15+S16 now implemented. S15: Miller-Rabin cofactor primality. S16: early exit p^2>cof.
    S17+S18 implemented (×4.9× trial div, Bernstein batch GCD). Parallel trial div via par_iter added.
    Sieve fill uses block-level early exit to prevent over-collection at small N (replaces j_size cap).
    Threshold calibration fixed: log_G = max_b·log2 + m_log (was missing m contribution).
    S19 pending: GPU ECM cofactorization. 3f+optimizations: 1456 LOC total.
    Dynamic batch GCD check: skipped when max_norm < lp_bound² (norms
    computable from sieve geometry; product tree screens nothing when norms are tiny).
    Rational pre-filter (p_filter): historically skipped 1LP, now disabled at typical B.
    Parallel harvesting: par_iter over all candidates (no HashMap bottleneck).
    128-bit target: 3f + S2+S3+S4+S5+S6+S7, Block Lanczos (4c) for ~10K×10K GF(2) matrix.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 4 — MATRIX / LINEAR ALGEBRA  (GF(2))[0m  (≈20% runtime)

  Goal: given R relations, each represented as a binary vector v ∈ GF(2)^B encoding prime exponent
  parities, find a non-empty subset S such that ⊕_{r∈S} v(r) = 0 (a "dependency" mod 2).

  Matrix is ~20% of total runtime.  Within-segment gains are multiplied by 0.20 for overall impact.
  For very large N (> 100 digits) the matrix may grow to rival sieving; gains matter more at scale.

  [1;36m┌────┬──────────────────────────────────┬───────────────────────────┬──────────┬───────┬──────────┬──────────────┬──────────────────┐
  │ #  │ Option                           │ Complexity                │ Diff     │  LOC  │ Seg Perf │ Overall      │ Memory           │
  ├────┼──────────────────────────────────┼───────────────────────────┼──────────┼───────┼──────────┼──────────────┼──────────────────┤[0m
  │ 4a │ Dense GF(2) Gaussian elim.       │ O(B² · R/64)              │ [1;32m●●○○○[0m    │[0;37m  150  [0m│  ×1      │ baseline     │ O(B·R/8) bytes   │
  │ 4b │ Sparse Gaussian elimination      │ O(B² · density)           │ [1;33m●●●○○[0m    │[0;37m  240  [0m│  ×5      │ ~×1.8 total  │ O(R · nnz)       │
  │ 4c │[1;32m Block Lanczos over GF(2)✓        [0m│ O(B · R · nnz/row)        │ [1;33m●●●○○[0m    │[1;32m  825  [0m│  ×40     │ ~×3.5 total  │ O(B + R + nnz)   │
  │ 4d │ Block Wiedemann                  │ O(B · R · nnz/row)        │ [1;33m●●●●○[0m    │[0;37m  650  [0m│  ×15     │ ~×3.8 total  │ O(B + R)         │
  │ 4e │ Block Wiedemann + MPI            │ O(B · R · nnz / nodes)    │ [1;31m●●●●●[0m    │[0;37m 1800  [0m│  ×N·15   │ ×N×3 total   │ distributed      │
  [1;36m└────┴──────────────────────────────────┴───────────────────────────┴──────────┴───────┴──────────┴──────────────┴──────────────────┘[0m

  [1;32m★ Selected:[0m    [1;32m4c[0m Block Lanczos over GF(2) — implemented in matrix.rs.
    Key trick: sparse column bitsets and bit-sliced Krylov vectors for vectorized 64-way XOR accumulation.
    Relations R should be ~10–20% more than B to guarantee a non-trivial null space.
    u64 fast path: S17 reciprocal parity tracking (no IBig) in factor_row when G fits
    in u64; falls back to IBig only when G > 2^64. p=2 via trailing_zeros; odd p via inv_p+lim_p.
    Singleton pruning: Efficient O(nnz) singleton pruning during build_matrix reduces dimensions drastically.
    Quadratic characters: poly-aware quadratic character rows (matrix.rs) enforce principal-square conditions
    at split primes q>B.  Required ≥ 2-rank(Cl(Q(√disc(f)))).
    d=2: max(2ω(|disc|)+20,150); d=3: max(2ω(|disc_cubic|)+40,300).
    For d=3 partially-split q, also adds P₂ character Leg(a²-g₁ab+g₀b²,q).

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 5 — ALGEBRAIC SQUARE ROOT[0m  (≈3% runtime)

  Goal: given dependency set S, form X = ∏_{(a,b)∈S} (a + bα) in Z[α]/f(α) and prove it is a
  perfect square.  Compute its square root γ, then evaluate γ(m) mod N for the rational side.

  Only ~3% of total runtime; within-segment gains have negligible overall impact.
  dashu IBig handles all large-operand polynomial and integer arithmetic natively.

  [1;36m┌────┬──────────────────────────────────┬───────────────────────────┬──────────┬───────┬──────────┬──────────────┬──────────────────┐
  │ #  │ Option                           │ Complexity                │ Diff     │  LOC  │ Seg Perf │ Overall      │ Memory           │
  ├────┼──────────────────────────────────┼───────────────────────────┼──────────┼───────┼──────────┼──────────────┼──────────────────┤[0m
  │ 5a │ Couveignes / Montgomery lifting  │ O(d² · |S| · log² N)      │ [1;33m●●●○○[0m    │[0;37m  220† [0m│  ×1      │ < 0.1% total │ O(d · log N)     │
  │ 5b │ p-adic Newton lifting            │ O(d² · |S| · log² N)      │ [1;33m●●●●○[0m    │[0;37m  370† [0m│  ×1      │ < 0.1% total │ O(d · log N)     │
  │ 5c │[1;32m inert-prime Newton lifting✓      [0m│ O(d² · |S| · log² N)      │ [1;32m●●○○○[0m    │[1;32m  581  [0m│  ×2      │ < 0.1% total │ dashu alloc      │
  [1;36m└────┴──────────────────────────────────┴───────────────────────────┴──────────┴───────┴──────────┴──────────────┴──────────────────┘[0m

  [1;32m★ Selected:[0m    [1;32m5c[0m inert-prime p-adic Newton lifting — implemented in alg_sqrt.rs.
    Find one inert prime p>B (f irreducible mod p); Newton-lift √S₂ to Z/pᵋZ via R←R(3−S₂R²)/2.
    Product trees for exact polynomial multiplication are parallelised with Rayon.
    Center γ coefficients into (−pᵋ/2, pᵋ/2]; evaluate γ(m) mod N; verify y²≡g_prod (mod N).
    Bad deps (S=−γ₀² for imaginary K): large coefficients signal failure; filtered by quadratic chars.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;35m  SEGMENT 6 — FACTOR RECOVERY[0m  (≈1% runtime)

  Goal: from square roots X (rational side) and Y (algebraic side), compute gcd(X − Y, N).
  This equals p or q with probability ~1/2 per dependency vector; try multiple if needed.

  Only ~1% of total runtime; no option here materially affects overall wall-time.
  Use dashu IBig for X − Y arithmetic; num-integer::gcd() only works on primitives.

  [1;36m┌────┬──────────────────────────────────┬───────────────────────────┬──────────┬───────┬──────────┬──────────────┬──────────────────┐
  │ #  │ Option                           │ Complexity                │ Diff     │  LOC  │ Seg Perf │ Overall      │ Memory           │
  ├────┼──────────────────────────────────┼───────────────────────────┼──────────┼───────┼──────────┼──────────────┼──────────────────┤[0m
  │ 6a │[1;32m GCD(X − Y, N) direct✓            [0m│ O(log² N)                 │ [1;32m●○○○○[0m    │[1;32m   20  [0m│  ×1      │ < 0.1% total │ negligible       │
  │ 6b │[1;32m Multiple dependency vectors✓     [0m│ O(k · log² N)             │ [1;32m●○○○○[0m    │[1;32m   30  [0m│  ×1      │ < 0.1% total │ negligible       │
  │ 6c │[1;32m Large-prime relation recovery✓   [0m│ O(log² N · extra pairs)   │ [1;33m●●●○○[0m    │[1;32m  134  [0m│  ×3      │ ~×1.7 total* │ hash table       │
  │ 6d │[1;32m Pairwise XOR dependencies✓       [0m│ O(k² · log² N)            │ [1;32m●●○○○[0m    │[1;32m   40  [0m│  ×1      │ < 0.1% total │ negligible       │
  [1;36m└────┴──────────────────────────────────┴───────────────────────────┴──────────┴───────┴──────────┴──────────────┴──────────────────┘[0m

  [1;32m★ Selected:[0m    [1;32m6a + 6b + 6c + 6d[0m — all implemented in factor_rec.rs.
    fallback.  If GCD = 1 or N, try the next dependency.  gcd() is already in your functions.rs.
    * 6c "Overall ×1.7" means it indirectly boosts yield so fewer sieve relations are needed;
      this is a relation-count speedup, not a direct runtime reduction of Seg 6 itself.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;33m  RECOMMENDED BUILD PATHS[0m

  [1;37m── PATH A: Minimal / Educational[0m  (target: < 1 500 LOC · factors 30–50 digit numbers)
     1a  +  2a  +  3b + S1  +  4a  +  5a  +  6a+6b
     · No parallelism.  Flat rectangle sieve.  Dense bit-matrix.  Pure safe Rust, no unsafe.
     · Good for understanding the algorithm end-to-end before optimising.
     · Crates: rayon (present), dashu (installed).

  [1;37m── PATH B: Balanced[0m  (target: ~4 000 LOC · factors 60–80 digit numbers)
     1b  +  2c  +  3d + S2 + S3 + S5  +  4c  +  5a  +  6a+6b
     · Log sieve in L1-sized segments, Rayon parallelism (reuse your existing infrastructure).
     · Block Lanczos GF(2) for matrix.  Berlekamp for polynomial roots mod p.
     · Crates: rayon (present), dashu (installed), concrete-ntt (installed).

  [1;37m── PATH C: Performance[0m  (target: 10 000+ LOC · factors 50–80-bit N)
     1c  +  2c  +  3e + S2 + S3 + S7 + S8 + S5  +  4c  +  5a  +  6a+6b
     · Lattice sieve; block-SIMD fill (S7) + bucket sieve for medium/large primes (S8).
     · Block Lanczos GF(2) for the matrix.  Parallelise with Rayon.
     · Crates: rayon, dashu, concrete-ntt, rand — all installed.

  [1;37m── PATH D: Production (128-bit)[0m  (target: 20 000+ LOC · factors 100–128-bit N)
     1d  +  2c+2d  +  3f + S2 + S3 + S7 + S8 + S6 + S5  +  4c  +  5a  +  6a+6b+6c+6d
     · CADO-NFS style bucket sieve (3f): cache-optimal for B ≈ 120 000 at 128-bit.
     · Two large primes (S6) essential: boosts smooth yield 5–10× at this scale.
     · Block Lanczos over a 10K×10K GF(2) matrix (~11 000 primes in FB).
     · Polynomial selection (1d) is the dominant quality driver above 80 bits.
     · Crates: rayon, dashu, concrete-ntt, rand — all installed.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;33m  DEPENDENCY FLOW[0m

  [1;36m  [Seg 1][0m Poly Selection                  [1;36m[Seg 2][0m Factor Base
    f(x), m, d ─────────────────────────────► (p, r) pairs, p ≤ B
         │                                          │
         │    (small_primes already built)          │
         │    ◄── reuse build_small_primes() ───────┘
         │                                          │
         └──────────────────────────────────────────►[1;36m [Seg 3][0m Sieving
                                                     │ relations (a,b): F·G smooth
                                                     ▼
                                               [1;36m[Seg 4][0m Matrix GF(2)
                                                     │ dependency sets S
                                                     ▼
                                               [1;36m[Seg 5][0m Algebraic Sqrt
                                                     │ X, Y  with X²≡Y² (mod N)
                                                     ▼
                                               [1;36m[Seg 6][0m Factor Recovery
                                                     │
                                                     ▼
                                              p = gcd(X−Y, N),   q = N/p

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;33m  RUST IMPLEMENTATION NOTES[0m

  [1;37mBig integers[0m   Use [1;36mdashu[0m (installed: 0.4.2) — IBig for arbitrary-precision arithmetic.
                Karatsuba/FFT multiplication; much faster than num-bigint for large operands.
                dashu::IBig::gcd() for Seg 6; num-integer::gcd() only works on primitives.

  [1;37mPolynomials[0m    concrete-ntt (installed: 0.2.0) for NTT-based polynomial multiplication.
                Use for batch root-finding (2d) and poly operations in Seg 5.
                Operates over Z/pZ — choose auxiliary primes to avoid overflow.

  [1;37mSIMD[0m           wide (removed; not needed until S7) — portable, safe Rust, no unsafe blocks needed.
                f32x8 / u32x8 for sieve inner loop; replaces raw std::arch AVX2 intrinsics.
                Reduces S4 difficulty from ●●●●○ to ●●●○○ and saves ~80 LOC vs unsafe path.

  [1;37mGF(2) matrix[0m   Sparse bitsets for columns; bit-sliced Krylov vectors for SpMV.
                64-way XOR accumulations give massive speedups for Block Lanczos.

  [1;37mSieve reuse[0m    mark_composites + count_segment generalize directly to GNFS.
                Replace count_segment with a threshold-check-and-record step.
                seg_size from detect_l1_cache_size() is already the right segment size.

  [1;37mParallelism[0m    Sieving: embarrassingly parallel over b-values or special-q.
                Reuse spawn_broadcast exactly as in main.rs.
                Matrix: each matrix-vector product in Lanczos parallelizes independently.

  [1;37mC-style Rust[0m   All segments can be free functions with explicit state passed by &mut ref.
                No impl blocks required — consistent with your existing style.

  [1;37mTesting path[0m   3 127 = 53 × 59  (6 digits, trivial)
                1 099 511 627 689 = 1 048 583 × 1 048 601  (13 digits, good smoke test)
                Test each segment independently before integration.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;33m  KEY REFERENCES[0m

  Pomerance (1996)         "A Tale of Two Sieves"                     — accessible intro to QS and NFS
  Lenstra & Lenstra (1993) "The Development of the NFS"               — original GNFS paper collection
  Crandall & Pomerance     "Prime Numbers: A Computational Perspective" §6.2  — full GNFS treatment
  Cohen                    "A Course in Computational Algebraic Number Theory" §4.3  — algebraic sqrt
  Briggs (2006)            "An Introduction to the GNFS"              — gentle walkthrough, pseudocode
  CADO-NFS source          gitlab.inria.fr/cado-nfs/cado-nfs          — production reference impl.

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

[1;37m══════════════════════════════════════════════════════════════════════════════════════════════════════════[0m

  [1;35mPERFORMANCE BENCHMARKS[0m  (see BENCHMARK.md for full run data)

  Command: cargo run --example gnfs_sieve --release -- <N>  (release build, Rayon)
  Date:    2026-03-06.  gauss_reduce infinite-cycle bug fixed before benchmarking.

  [1;36m┌───────────────────┬────────────────┬────────────────┬──────────────────────┐[0m
  [1;36m│[0m Variant             [1;36m│[0m sieve_one (ms) [1;36m│[0m relations (ms) [1;36m│[0m Notes                 [1;36m│[0m
  [1;36m├────────────────────┼────────────────┼────────────────┼──────────────────────┤[0m
  [1;36m│[0m S1+S3 binary        [1;36m│[0m      2.14       [1;36m│[0m      10.5       [1;36m│[0m fill=1, scalar harv   [1;36m│[0m
  [1;36m│[0m S2+S3 scalar        [1;36m│[0m      1.18       [1;36m│[0m       5.5       [1;36m│[0m log fill, scal harv   [1;36m│[0m
  [1;36m│[0m S2+S3+S4 SIMD*       [1;36m│[0m      1.21       [1;36m│[0m       5.5       [1;36m│[0m SIMD harvest (+0.03)  [1;36m│[0m
  [1;36m│[0m S2+S3+S5 Rayon      [1;36m│[0m       —         [1;36m│[0m       5.5       [1;36m│[0m ×1.9× vs S2+S3 single-thr [1;36m│[0m
  [1;36m│[0m S2+S3+S4+S5         [1;36m│[0m       —         [1;36m│[0m       5.5       [1;36m│[0m all options           [1;36m│[0m
  [1;36m└────────────────────┴────────────────┴────────────────┴──────────────────────┘[0m

  Historical benchmark (S4f removed; scalar = S2+S3+S5; S4f was S2+S3+S4f+S5):

  [1;36m┌────────…──────────────────────────────────────────────┐[0m
  [1;36m│[0m bits │ B     │ |FB| │ scalar sieve │ S4f sieve  │ ratio  │ smooth/200 �[1;36m│[0m
  [1;36m├…┼……………┼…………┼……………………┼………………………┼…………………┼………………………┤[0m
  [1;36m│[0m  12  │   300 │   62 │     59 ms    │[0;37m    39 ms   [0m│  1.51x │  200/200   [1;36m│[0m
  [1;36m│[0m  13  │   300 │   62 │     42 ms    │[0;37m    31 ms   [0m│  1.37x │  200/200   [1;36m│[0m
  [1;36m│[0m  31  │  2000 │  303 │   3939 ms    │[0;37m  3956 ms   [0m│  1.00x │  137/200   [1;36m│[0m
  [1;36m│[0m  40  │  2000 │  303 │   3737 ms    │[0;37m  4988 ms   [0m│  0.75x │    8/200   [1;36m│[0m
  [1;36m│[0m  51  │ 10000 │ 1229 │    ~300 s     │[0;37m   DNF     [0m│   --   │     unk   [1;36m│[0m
  [1;36m└…┴……………┴…………┴……………………┴………………………┴…………………┴………………………┘[0m

  DNF = S4f killed at 143 min (see BENCHMARK.md for full analysis).

  Findings:
  • S2 log sieve: ×1.8 vs S1 at N=3127. Projected ×3 (large N): benefit comes from
    better threshold accuracy → fewer false positives → fewer expensive is_smooth calls.
  • S4 SIMD harvest: negligible gain (1.21 vs 1.18 ms); harvest is NOT the bottleneck at any N.
    Fill ∝ ln(B)×harvest at all N, so fill dominates MORE as B grows with N.
    True S4 gain requires SIMD in mark_root (variable-stride scatter; removed; use S7  ).
    LOC cost: ~40 lines (wide crate, harvest only).
  • S5 Rayon: ~×2 (multi-core); measured ×1.9× on single-q, ×2.1× across all q pairs.
  • Selected variant S2+S3+S5 achieves 5.5 ms for the full q∈[60,120] set (N=3127 smoke test).

  [1;33mNote:[0m S-table ×speedups for S2/S3/S5 are projections for large N (>50 digits).
  At N=3127 the sieve fits in L1; S3 blocking and S2 filtering gains are both suppressed.
  S4 harvest SIMD shows no gain at any N: fill cost ≈ ln(B)×harvest cost, growing with N.
