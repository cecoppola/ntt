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

## Performance — CPU Reference (AMD Ryzen 9 5950X)

**Forward NTT throughput across sizes, q = 3329 (ML-KEM)**

| n | CT-DIT | Stockham | Montgomery |
|--:|-------:|---------:|-----------:|
| 64 | 1,064k/s | **1,251k/s** | 979k/s |
| 128 | 476k/s | **548k/s** | 429k/s |
| 256 | 204k/s | **244k/s** | 197k/s |
| 512 | 91k/s | **111k/s** | 94k/s |
| 1,024 | 42k/s | **51k/s** | 42k/s |
| 2,048 | 19k/s | **23k/s** | 20k/s |
| 4,096 | 8.8k/s | 9.5k/s | **10.2k/s** |

Stockham wins at all sizes up to n=2048 (~20% faster than CT-DIT) due to
elimination of the bit-reversal pass and a more sequential memory access pattern.
Montgomery becomes competitive at n=4096 where cache pressure equalises the field.
GPU results pending hardware access.

**Polynomial multiplication (cyclic, n=256)**

| Modulus | Scheme | polymul/s | µs/op |
|--------:|--------|----------:|------:|
| 3,329 | ML-KEM | 76,253 | 13.1 |
| 8,380,417 | ML-DSA | 76,186 | 13.1 |

---

## Supported Moduli

| q | Form | Scheme |
|--:|------|--------|
| 3,329 | 13·2⁸+1 | ML-KEM (NIST FIPS 203) |
| 8,380,417 | 2²³−2¹³+1 | ML-DSA (NIST FIPS 204) |
| 12,289 | 3·2¹²+1 | FALCON-512/1024, NewHope |
| 998,244,353 | 119·2²³+1 | General NTT (n up to 2²³) |
| 469,762,049 | 7·2²⁶+1 | BFV/CKKS RNS component |
| 786,433 | 3·2¹⁸+1 | BFV small-modulus RNS chain |
| 1,073,479,681 | 2³⁰−2¹⁸+1 | TFHE bootstrapping |

All ω values are computed at runtime via modular exponentiation — no hardcoded roots.

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
module load PrgEng-cray-amd/8.5.0 rocm/7.0.3 craype-accel-amd-gfc942
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
    uint64_t n, q, omega;          /* transform size, modulus, primitive root */
    uint64_t omega_inv, n_inv;     /* derived: omega^{-1} mod q, n^{-1} mod q */
    uint32_t log2_n;               /* derived: log2(n) */
} ntt_params_t;

int ntt_params_init(ntt_params_t *p);  /* returns 0 on success */
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
cc -O2 example.c ntt_stockham.c -o example && ./example
```

---

## Selftests

Each binary runs a built-in selftest when invoked:

```sh
./ntt_cpu      256 3329 17       # round-trip test
./ntt_mont     256 3329 17       # lazy == montgomery, round-trip
./ntt_stockham 256 3329 17       # round-trip, vs CT-DIT ref, impulse
./ntt_polymul                    # 8 tests: identity, schoolbook, commutativity, cyclic
./ntt_cross_verify_6900xt        # 7 CPU-vs-GPU tests (requires ROCm device)
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
ntt.h                    shared API header
ntt_cpu.c                CT-DIT CPU (lazy reduction)
ntt_mont.c               CT-DIT CPU (Montgomery butterfly)
ntt_stockham.c           Stockham CPU (double-buffer, no bit-reversal)
ntt_bench.c              side-by-side algorithm × size sweep
ntt_polymul.c            cyclic polynomial multiplication
ntt_gpu.hip              CT-DIT GPU kernel
ntt_gpu_stockham.hip     Stockham GPU kernel
ntt_cross_verify.hip     CPU vs GPU correctness verification
mi300a_probe.sh          hardware capability probe
Makefile                 all CPU + GPU targets
BENCH_PLAN.md            benchmarking and parameter optimization plan
PLAN.md                  algorithm design and phase roadmap
TESTS.md                 test matrix with pass/fail status
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
