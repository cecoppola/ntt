# ntt — Number Theoretic Transform for AMD MI300A

High-performance NTT in pure **C + HIP/ROCm**, targeting the AMD MI300A APU.
Implements Cooley-Tukey DIT, Stockham auto-sort, and Montgomery-domain variants
with a unified API so CPU and GPU kernels are drop-in replacements for each other.

Designed for post-quantum cryptography workloads: ML-KEM (Kyber), ML-DSA
(Dilithium), FALCON, and lattice-based schemes requiring fast polynomial
multiplication over NTT-friendly prime fields.

---

## Algorithms

| Algorithm | File | Description |
|-----------|------|-------------|
| CT-DIT | `ntt_cpu.c` | Cooley-Tukey decimation-in-time, lazy reduction |
| Stockham | `ntt_stockham.c` | Auto-sort double-buffer; no bit-reversal |
| Montgomery | `ntt_mont.c` | CT-DIT with REDC butterfly; GPU-optimal on CDNA3 |
| CT-DIT GPU | `ntt_gpu.hip` | Per-stage kernels; `gfx1100` + `gfx942` |
| Stockham GPU | `ntt_gpu_stockham.hip` | Ping-pong double-buffer; no bit-reversal |
| Poly-mul | `ntt_polymul.c` | Cyclic convolution via NTT; schoolbook reference |

---

## Performance — CPU Reference (AMD Ryzen 9 5905X)

**14-prime sweep at min(max\_n, 1024) — forward NTT throughput**

| Prime | Form | n | CT-DIT | Stockham | Montgomery |
|-------|------|--:|-------:|---------:|-----------:|
| Fermat-8 | 2⁸+1 | 256 | 233k/s | **235k/s** | 220k/s |
| ML-KEM | 13·2⁸+1 | 256 | 209k/s | **223k/s** | 220k/s |
| ML-KEM-v0 | 15·2⁹+1 | 512 | 92k/s | 95k/s | **101k/s** |
| FALCON | 3·2¹²+1 | 1024 | 24k/s | 42k/s | **47k/s** |
| Proth-5-13 | 5·2¹³+1 | 1024 | 41k/s | 41k/s | **47k/s** |
| Fermat-16 | 2¹⁶+1 | 1024 | 50k/s | **51k/s** | 46k/s |
| ML-DSA | 2²³−2¹³+1 | 1024 | 39k/s | 39k/s | **47k/s** |
| CRT-lo | 5·2²⁵+1 | 1024 | 42k/s | 42k/s | **47k/s** |
| CRT-mid | 7·2²⁶+1 | 1024 | 41k/s | 42k/s | **47k/s** |
| CRT-hi | 119·2²³+1 | 1024 | 42k/s | 42k/s | **48k/s** |
| FHE-RNS | 15·2²⁷+1 | 1024 | 41k/s | 42k/s | **48k/s** |
| Solinas-60 | 2⁶⁰−2¹⁸+1 | 1024 | 37k/s | **39k/s** | N/A¹ |
| Solinas-61 | 2⁶¹−2⁵⁴+1 | 1024 | 40k/s | **41k/s** | N/A¹ |
| Goldilocks | 2⁶⁴−2³²+1 | 1024 | **36k/s** | 35k/s | N/A¹ |

¹ Montgomery with R=2³² requires R>q; not valid for q ≥ 2³².

**Polynomial multiplication (cyclic, n=256)**

| Modulus | Scheme | polymul/s | µs/op |
|--------:|--------|----------:|------:|
| 3,329 | ML-KEM | 76,253 | 13.1 |
| 8,380,417 | ML-DSA | 76,186 | 13.1 |

---

## Supported Moduli

All 14 primes are registered in `src/ntt_moduli.h` with fast reduction functions.
Primitive roots ω are computed at runtime — nothing hardcoded.

| q | Form | max n | Reduction | Scheme / Use |
|--:|------|------:|-----------|--------------|
| 257 | 2⁸+1 | 2⁸ | Fermat (exact) | Toy / reference |
| 3,329 | 13·2⁸+1 | 2⁸ | generic | ML-KEM (NIST FIPS 203) |
| 7,681 | 15·2⁹+1 | 2⁹ | generic | ML-KEM v0 (Kyber round 1) |
| 12,289 | 3·2¹²+1 | 2¹² | generic | FALCON-512/1024, NewHope |
| 40,961 | 5·2¹³+1 | 2¹³ | generic | Small Proth; NTT testing |
| 65,537 | 2¹⁶+1 | 2¹⁶ | Fermat (exact) | General-purpose Fermat |
| 8,380,417 | 2²³−2¹³+1 | 2¹³ | Solinas (exact) | ML-DSA (NIST FIPS 204) |
| 167,772,161 | 5·2²⁵+1 | 2²⁵ | generic | CRT-NTT triple (low) |
| 469,762,049 | 7·2²⁶+1 | 2²⁶ | generic | CRT-NTT triple (mid) |
| 998,244,353 | 119·2²³+1 | 2²³ | generic | CRT-NTT triple (hi) |
| 2,013,265,921 | 15·2²⁷+1 | 2²⁷ | generic | Large FHE RNS chain |
| 2⁶⁰−2¹⁸+1 | Solinas-60 | 2¹⁸ | Solinas-60 (exact) | STARK / large field |
| 2⁶¹−2⁵⁴+1 | Solinas-61 | 2⁵⁴ | generic | STARK / large field |
| 2⁶⁴−2³²+1 | Goldilocks | 2³² | 2-step exact | ZK / Plonky2 / FRI/STARK |

Reduction notes: **Fermat** uses `(t & mask) − (t >> m)`.
**ML-DSA Solinas** uses `B + (A<<13) − A` since 2²³ ≡ 2¹³−1 (mod q).
**Solinas-60** uses a 2-pass reduction: 2⁶⁰ ≡ 2¹⁸−1 (mod q); no 128-bit divide.
**Goldilocks** uses a 2-step reduction: 2⁶⁴ ≡ 2³²−1 (mod q); no 128-bit divide.
**CRT-NTT triple** (5·2²⁵+1, 7·2²⁶+1, 119·2²³+1): use all three together for
polynomial multiplication over ℤ (covers products up to ~3.9×10²⁵).
All Proth primes use hardware 128-bit divide (K²RED is FPGA-only, not software).

---

## Target Hardware

| System | GPU | Wavefront | Memory |
|--------|-----|-----------|--------|
| Dev (CPU) | AMD Ryzen 9 5950X | — | DDR4 |
| Dev (GPU) | AMD Radeon RX 6900 XT | 32 (RDNA3) | 16 GB GDDR6 |
| Production | AMD MI300A APU | 64 (CDNA3) | 128 GB HBM3 unified |

The MI300A is an APU with unified HBM3 — no PCIe transfers. Host↔device copies
are near-zero cost. Block size, wavefront width, and LDS tile sizes are queried
at runtime via `hipGetDeviceProperties`; nothing is hardcoded.

---

## Build

### CPU only

```sh
make all          # ct-dit, stockham, montgomery, bench, polymul
make bench-sweep  # sweep all sizes and write results/cpu_sweep.csv
```

### GPU — RX 6900 XT (gfx1100)

```sh
make gpu-6900xt         # CT-DIT GPU kernel
make gpu-stok-6900xt    # Stockham GPU kernel
make cross-verify       # CPU vs GPU correctness check (7 tests)
```

### GPU — MI300A (gfx942)

```sh
module load PrgEng-cray-amd/8.5.0 rocm/7.0.3 craype-accel-amd-gfx942
make gpu-mi300a gpu-stok-mi300a
```

Build requires: `cc` (C99), `hipcc` (ROCm ≥ 6.0), `-Wall -Wextra` clean.

---

## API

All implementations share one interface (swap the object file to switch CPU↔GPU):

```c
/* Allocate twiddle tables — call once, reuse across transforms */
uint64_t *ntt_alloc_twiddles(const ntt_params_t *p);
uint64_t *ntt_alloc_twiddles_inv(const ntt_params_t *p);

/* Forward and inverse NTT — in-place, over Z_q */
void ntt_forward(uint64_t *a, const uint64_t *twiddles,     const ntt_params_t *p);
void ntt_inverse(uint64_t *a, const uint64_t *twiddles_inv, const ntt_params_t *p);

/* Parameter struct — fill n, q, omega; call init to derive the rest */
typedef struct {
    uint64_t    n, q, omega;       /* transform size, modulus, primitive root */
    uint64_t    omega_inv, n_inv;  /* derived: omega^{-1} mod q, n^{-1} mod q */
    uint32_t    log2_n;            /* derived: log2(n) */
    reduce_fn_t reduce;            /* derived: fast mod-q reduction function   */
} ntt_params_t;

int ntt_params_init(ntt_params_t *p);  /* returns 0 on success; wires p->reduce */
```

Montgomery variants (`ntt_forward_mont` / `ntt_inverse_mont`) are drop-in
replacements with the same signature; use `ntt_alloc_twiddles_mont` twiddle tables.

---

## Quick Start

```c
#include "ntt.h"
#include <stdlib.h>

int main(void) {
    ntt_params_t p = { .n = 256, .q = 3329, .omega = 17 };
    ntt_params_init(&p);

    uint64_t *tw  = ntt_alloc_twiddles(&p);
    uint64_t *twi = ntt_alloc_twiddles_inv(&p);
    uint64_t  a[256];
    for (int i = 0; i < 256; i++) a[i] = i % p.q;

    ntt_forward(a, tw,  &p);   /* forward NTT  */
    ntt_inverse(a, twi, &p);   /* inverse NTT  */
    /* a[i] == i % p.q  for all i */

    free(tw); free(twi);
}
```

```sh
cc -O2 example.c src/ntt_stockham.c -I src -o example && ./example
```

---

## Selftests

Each binary runs a built-in selftest when invoked:

```sh
./bin/ntt_cpu      256 3329 17       # round-trip test
./bin/ntt_mont     256 3329 17       # lazy == montgomery, round-trip
./bin/ntt_stockham 256 3329 17       # round-trip, vs CT-DIT ref, impulse
./bin/ntt_polymul  256 3329 17       # 8 tests: identity, schoolbook, commutativity, cyclic
./bin/ntt_bench                      # 15-prime sweep table
./bin/ntt_cross_verify_6900xt        # 7 CPU-vs-GPU tests (requires ROCm device)
```

All CPU selftests pass on Ryzen 9 5950X. GPU tests pending hardware.

---

## MI300A Parameter Optimization

`mi300a_probe.sh` runs at deployment to query hardware capabilities:

```sh
bash mi300a_probe.sh 2>&1 | tee results/mi300a_probe.txt
```

The planned `ntt_gpu_tune` binary (see `BENCH_PLAN.md`) will sweep block size,
batch size, twiddle source (HBM vs LDS), and algorithm variant — printing a ranked
results table and writing `results/gpu_tune.csv`. Runs in tiered modes:

```sh
./ntt_gpu_tune --sweep-block --quick     # ≤5 min: find optimal block size
./ntt_gpu_tune --sweep-batch --block 128 # ≤5 min: find optimal batch size
./ntt_gpu_tune --full                    # overnight: full parameter grid
```

---

## Repository Layout

```
src/
  ntt.h                  shared API header (includes ntt_moduli.h)
  ntt_moduli.h           15-prime table, reduction functions, addmod/submod
  ntt_cpu.c              CT-DIT CPU (lazy reduction)
  ntt_mont.c             CT-DIT CPU (Montgomery butterfly)
  ntt_stockham.c         Stockham CPU (double-buffer, no bit-reversal)
  ntt_bench.c            15-prime algorithm × size sweep
  ntt_polymul.c          cyclic polynomial multiplication
  ntt_gpu.hip            CT-DIT GPU kernel
  ntt_gpu_stockham.hip   Stockham GPU kernel
  ntt_cross_verify.hip   CPU vs GPU correctness verification
bin/                     compiled binaries (generated by make)
mi300a_probe.sh          hardware capability probe
Makefile                 all CPU + GPU targets
```

---

## Convolution Type

The current NTT computes **cyclic convolution** (mod X^n − 1) for all algorithms
and all choices of ω. True **negacyclic convolution** (mod X^n + 1, required by
ML-KEM and ML-DSA for polynomial multiplication) needs a twisted-NTT with
pre/post multiply by ψ^i where ψ² = ω. This is planned for Phase 4.

---

## License

MIT
