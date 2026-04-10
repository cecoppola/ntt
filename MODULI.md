# NTT Moduli Reference

Complete reference for all 14 primes in the benchmark sweep.
For each prime: algebraic structure, reduction path, NTT size limits, and use cases.

ω values for n=256 are computed via `ntt_modulus_omega(m, 256)` at runtime.
For any valid n: ω_n = g^((q−1)/n) mod q.

---

## Master Table

| q | Form | g | Max n | Reduction | Reduction cost | Scheme / Use |
|--:|------|:-:|------:|-----------|---------------|-------------|
| 257 | 2⁸+1 | 3 | 2⁸ | Fermat exact | 1 sub | Toy / reference |
| 3,329 | 13·2⁸+1 | 3 | 2⁸ | generic % | hw divide | ML-KEM (FIPS 203) |
| 7,681 | 15·2⁹+1 | 3 | 2⁹ | generic % | hw divide | ML-KEM v0 (Kyber rd1) |
| 12,289 | 3·2¹²+1 | 11 | 2¹² | generic % | hw divide | FALCON-512/1024, NewHope |
| 40,961 | 5·2¹³+1 | 3 | 2¹³ | generic % | hw divide | Small Proth, NTT testing |
| 65,537 | 2¹⁶+1 | 3 | 2¹⁶ | Fermat exact | 1 sub | General Fermat |
| 8,380,417 | 2²³−2¹³+1 | 10 | 2¹³ | Solinas exact | shift+add | ML-DSA (FIPS 204) |
| 167,772,161 | 5·2²⁵+1 | 3 | 2²⁵ | generic % | hw divide | CRT-NTT triple (low) |
| 469,762,049 | 7·2²⁶+1 | 3 | 2²⁶ | generic % | hw divide | CRT-NTT triple (mid) |
| 998,244,353 | 119·2²³+1 | 3 | 2²³ | generic % | hw divide | CRT-NTT triple (hi) |
| 2,013,265,921 | 15·2²⁷+1 | 31 | 2²⁷ | generic % | hw divide | FHE large RNS |
| 1,152,921,504,606,584,833 | 2⁶⁰−2¹⁸+1 | 3¹ | 2¹⁸ | Solinas-60 exact | 2-pass | STARK / large field |
| 2,287,828,610,704,211,969 | 2⁶¹−2⁵⁴+1 | 3¹ | 2⁵⁴ | generic % | hw divide | STARK / large field |
| 18,446,744,069,414,584,321 | 2⁶⁴−2³²+1 | 7 | 2³² | Goldilocks exact | 2-step | ZK / Plonky2 / FRI |

¹ g=3 for large Solinas primes is unverified — confirm round-trip before production use.

---

## CRT-NTT Triple

{167,772,161, 469,762,049, 998,244,353} — all have primitive root g = 3.

Product of all three: 167,772,161 × 469,762,049 × 998,244,353 ≈ 7.9×10²⁵

Use these three primes together for polynomial multiplication over ℤ via CRT:
1. Compute NTT of f and g under each of the three primes
2. Pointwise multiply in each residue domain
3. Recover integer coefficients via CRT (Garner's algorithm)

Covers polynomial products with coefficients up to ~3.9×10²⁵ without any coefficient overflow.

---

## Reduction Path Details

### Fermat primes: q = 2^m + 1

**q = 257** (m=8) and **q = 65,537** (m=16).

For t = A·2^m + B with A = t>>m, B = t&(2^m−1):
```
t mod (2^m+1) = B - A     (add q if negative)
```
Cost: 1 shift, 1 subtract, 1 conditional add. No multiply, no divide.
Implemented in: `reduce_fermat8`, `reduce_fermat16` in `ntt_moduli.h`.

---

### Solinas prime: q = 8,380,417 = 2²³−2¹³+1 (ML-DSA)

2²³ ≡ 2¹³−1 (mod q). For t = A·2²³ + B:
```
t mod q = B + A·(2^13 - 1) = B + (A<<13) - A
```
Implemented in: `reduce_dilithium` in `ntt_moduli.h`.

---

### Solinas-60: q = 2⁶⁰−2¹⁸+1 = 1,152,921,504,606,584,833

2⁶⁰ ≡ 2¹⁸−1 (mod q). Exact 2-pass reduction — no 128-bit divide.

**Pass 1** (120→79 bits): A = t>>60, B = t&(2⁶⁰−1); r = A·(2¹⁸−1) + B
**Pass 2** (79→61 bits): A₂ = r>>60, B₂ = r&(2⁶⁰−1); r₂ = A₂·(2¹⁸−1) + B₂

One conditional subtract yields result in [0, q). A₂ < 2¹⁹ so the second pass uses
only uint64_t arithmetic (A₂·(2¹⁸−1) < 2³⁷; r₂ < 2⁶¹ < 2q).

Implemented in: `reduce_solinas_60` in `ntt_moduli.h`.

Max NTT size n = 2¹⁸ (2-adic valuation of q−1 = 2¹⁸·(2⁴²−1)).

---

### Solinas-61: q = 2⁶¹−2⁵⁴+1 = 2,287,828,610,704,211,969

q−1 = 2⁵⁴·127. Max NTT size = 2⁵⁴ (theoretical); bench caps at n = 1024.

2⁶¹ ≡ 2⁵⁴−1 (mod q). A Solinas reduction converges only 7 bits per pass (61−54=7),
requiring ~8 passes from 2¹²². This is not faster than hardware `%` in software.
Uses `reduce_generic` (128-bit divide). Primary appeal is theoretical: a 61-bit prime
with extremely large NTT support and a Proth-like Solinas form.

---

### Goldilocks: q = 2⁶⁴−2³²+1

2⁶⁴ ≡ 2³²−1 (mod q). Exact 2-step reduction from 128-bit input.

For t = a·2⁶⁴ + b:
- Step 1: r = a·(2³²−1) + b  (computed in __uint128_t; result < 2⁹⁶)
- Step 2: c = r>>64, d = (uint64_t)r; s = c·(2³²−1) + d  (may overflow once)
- One overflow correction + one conditional subtract.

Implemented in: `reduce_goldilocks` in `ntt_moduli.h`.
Montgomery is not valid for Goldilocks (requires R > q; R=2³² < q≈2⁶⁴).

---

### Proth primes (generic reduction)

All Proth primes (q = k·2^m + 1) use `reduce_generic` (__uint128_t % q).

The K²RED formula B−k·A = t−A·(2^m+k) is NOT ≡ t (mod q) in software.
It requires FPGA bounded-width arithmetic to be exact. Until a correct Barrett
reduction is implemented for each prime, `reduce_generic` is the correct path.

| q | Form | k | m | Note |
|--:|------|--:|--:|------|
| 3,329 | 13·2⁸+1 | 13 | 8 | ML-KEM standard |
| 7,681 | 15·2⁹+1 | 15 | 9 | ML-KEM v0 / Kyber round 1 |
| 12,289 | 3·2¹²+1 | 3 | 12 | FALCON / NewHope; smallest odd k |
| 40,961 | 5·2¹³+1 | 5 | 13 | Small Proth test prime |
| 167,772,161 | 5·2²⁵+1 | 5 | 25 | CRT triple lo |
| 469,762,049 | 7·2²⁶+1 | 7 | 26 | CRT triple mid |
| 998,244,353 | 119·2²³+1 | 119 | 23 | CRT triple hi; competitive prog |
| 2,013,265,921 | 15·2²⁷+1 | 15 | 27 | FHE large RNS |

---

## Implemented Reduction Functions

| Function | Location | Prime(s) |
|----------|----------|---------|
| `reduce_generic` | `ntt_moduli.h` | All Proth primes, Solinas-61 (fallback) |
| `reduce_fermat8` | `ntt_moduli.h` | q = 257 |
| `reduce_fermat16` | `ntt_moduli.h` | q = 65,537 |
| `reduce_dilithium` | `ntt_moduli.h` | q = 8,380,417 (ML-DSA Solinas) |
| `reduce_solinas_60` | `ntt_moduli.h` | q = 1,152,921,504,606,584,833 |
| `reduce_goldilocks` | `ntt_moduli.h` | q = 2⁶⁴−2³²+1 |

All functions share the signature:
```c
uint64_t reduce_fn(__uint128_t product, uint64_t q);
```
and are wired into `p->reduce` by `ntt_params_init()` via `ntt_modulus_find()`.

---

## Notes

**Montgomery**: the bench wrapper (`mnt_ntt`) uses Montgomery with R=2³² and is
skipped for any prime with q ≥ 2³² (CRT-hi excluded: q=998,244,353 < 2³⁰ ✓;
bench marks N/A for Solinas-60, Solinas-61, and Goldilocks).

**Primitive roots**: g=3 for the two large Solinas primes is a likely candidate
but unverified. To check: run `bin/ntt_cpu 256 <q> <omega>` where omega is
`ntt_modulus_omega(&NTT_MODULI[i], 256)`, then verify the round-trip passes.

**Negacyclic NTT**: all algorithms here compute cyclic convolution (mod X^n−1).
Negacyclic (mod X^n+1, required for ML-KEM and ML-DSA polynomial multiplication)
needs a twisted NTT with pre/post multiply by ψ^i where ψ² = ω. Planned Phase 4.
