# NTT / MI300A — Test Plan

Tests are grouped by phase. Run each group after completing the corresponding phase.
All tests must pass before advancing. Benchmark baselines are recorded in `bench_*.txt`.

---

## Unit Tests — CPU Reference (`ntt_cpu.c`)

Run: `make test-cpu` or `./ntt_cpu 256 3329 17 1`

| # | Test | Input | Expected output | Verify by |
|---|------|-------|-----------------|-----------|
| U1 | mod_pow correctness | 17^256 mod 3329 | 1 (ω^n = 1) | add assertion in main |
| U2 | mod_inv correctness | inv(17, 3329) | 1175 | 17 * 1175 mod 3329 = 1 |
| U3 | mod_inv correctness | inv(256, 3329) | 3316 | 256 * 3316 mod 3329 = 1 |
| U4 | twiddle table length | ntt_alloc_twiddles(n=256) | 128 values | tw[0]=1, tw[127]=ω^127 |
| U5 | bit_reverse_perm n=8 | [0,1,2,3,4,5,6,7] | [0,4,2,6,1,5,3,7] | manual check |
| U6 | delta → all-ones | a=[1,0,...,0] | [1,1,...,1] | ntt_forward, check all ==1 |
| U7 | all-ones → [n,0,...] | a=[1,1,...,1] | [256,0,...,0] | ntt_forward, check a[0]==256, rest 0 |
| U8 | round-trip ML-KEM | a=[0,1,...,255] mod q | original | INTT(NTT(a))==a |
| U9 | round-trip ML-DSA | a=[0,1,...,255] mod q | original | q=8380417, ω=1753 |

Run status: **U1–U9 PASS** (verified by selftest in main; see `make test-cpu`)

---

## Unit Tests — Stockham CPU (`ntt_stockham.c`)

Run: `make test-stockham` or `./ntt_stockham 256 3329 17 1`

| # | Test | Input | Expected output | Status |
|---|------|-------|-----------------|--------|
| S1 | round-trip ML-KEM | random a | INTT(NTT(a))==a | **PASS** |
| S2 | vs CT-DIT reference (ML-KEM) | random a | Stockham==CT-DIT element-wise | **PASS** |
| S3 | impulse → all-ones (ML-KEM) | a=[1,0,...] | [1,1,...,1] | **PASS** |
| S4 | round-trip ML-DSA | random a | INTT(NTT(a))==a | **PASS** |
| S5 | vs CT-DIT reference (ML-DSA) | random a | Stockham==CT-DIT element-wise | **PASS** |
| S6 | impulse → all-ones (ML-DSA) | a=[1,0,...] | [1,1,...,1] | **PASS** |

Run status: **S1–S6 PASS**

Benchmark result (5950X): ~241k NTT/s at n=256 — ~20% faster than CT-DIT at all sizes
(Stockham is faster because its memory access pattern avoids the bit-reversal pass and
has more sequential cache access than CT-DIT's stride pattern).

---

## Unit Tests — Polynomial Multiplication (`ntt_polymul.c`)

Run: `./ntt_polymul 100000`

| # | Test | Input | Expected | Status |
|---|------|-------|----------|--------|
| P1 | f * 1 = f (ML-KEM) | f=random, g=[1,0,...] | f unchanged | **PASS** |
| P2 | vs schoolbook (ML-KEM) | random f, g | cyclic conv matches | **PASS** |
| P3 | commutativity f*g=g*f (ML-KEM) | random f, g | equal | **PASS** |
| P4 | X^(n/2)^2 = X^n ≡ 1 (ML-KEM) | f=g=[0,...,1,...,0] at n/2 | c[0]=1, rest 0 | **PASS** |
| P5 | f * 1 = f (ML-DSA, ω_cyc) | ω=3073009=1753² | f unchanged | **PASS** |
| P6 | vs schoolbook (ML-DSA, ω_cyc) | random f, g | cyclic conv matches | **PASS** |
| P7 | commutativity (ML-DSA, ω_cyc) | random f, g | equal | **PASS** |
| P8 | X^(n/2)^2 = 1 (ML-DSA, ω_cyc) | f=g at n/2 | c[0]=1, rest 0 | **PASS** |

Run status: **P1–P8 PASS**

Notes:
- Cyclic convolution (mod X^n − 1): valid when omega^n ≡ 1. ML-KEM omega=17 (256th root) ✓.
- ML-DSA omega=1753 has order 512 (omega^256=-1, a 512th root): our standard NTT gives
  cyclic convolution only when omega^n=1. For ML-DSA cyclic tests we use omega_cyc=1753²=3073009.
- True ML-DSA PQC negacyclic convolution (mod X^n + 1) requires twisted-NTT with pre/post
  multiply by powers of a 2n-th root. This is a Phase 4 extension.

---

## Unit Tests — GPU Kernels (`ntt_gpu.hip`)

Run: `make test-gpu` (requires 6900XT)

| # | Test | Kernel | Input | Expected | Status |
|---|------|--------|-------|----------|--------|
| G1 | bit-reversal correctness | cv_bitrev_kernel | [0..255] natural | bit-reversed order | pending |
| G2 | single stage butterfly | cv_stage_kernel (stage 1) | matches CPU stage 1 output | element-wise == | pending |
| G3 | delta → all-ones | gpu_ntt | a=[1,0,...] | [1,1,...,1] | pending |
| G4 | all-ones → [n,0,...] | gpu_ntt | a=[1,1,...] | [256,0,...,0] | pending |
| G5 | round-trip GPU | gpu_ntt + ntt_inverse | a=[0..255] mod q | original | pending |

Run status: **pending — requires 6900XT**

---

## Unit Tests — GPU Stockham (`ntt_gpu_stockham.hip`)

Run: `./ntt_gpu_stockham_6900xt 256 3329 17 1000` (requires 6900XT)

| # | Test | Description | Expected | Status |
|---|------|-------------|----------|--------|
| GS1 | GPU round-trip (ML-KEM) | INTT(NTT(a))==a on GPU | all match | pending |
| GS2 | GPU vs CPU Stockham (ML-KEM) | element-wise == | 0 mismatches | pending |
| GS3 | impulse → all-ones (ML-KEM) | a=[1,0,...] | [1,1,...,1] | pending |
| GS4 | GPU round-trip (ML-DSA) | INTT(NTT(a))==a on GPU | all match | pending |
| GS5 | GPU vs CPU Stockham (ML-DSA) | element-wise == | 0 mismatches | pending |
| GS6 | impulse → all-ones (ML-DSA) | a=[1,0,...] | [1,1,...,1] | pending |

Run status: **pending — requires 6900XT**

---

## Cross-Validation Tests (`ntt_cross_verify.hip`)

Run: `make cross-verify` (requires 6900XT) — **Phase 3 exit criterion**

| # | Test | Description | Pass condition | Status |
|---|------|-------------|----------------|--------|
| C1 | delta → all-ones | CPU and GPU both produce [1,...,1] | cpu==expected AND gpu==expected AND cpu==gpu | pending |
| C2 | all-ones → [n,0,...] | CPU and GPU both produce [256,0,...,0] | all three conditions above | pending |
| C3 | CPU round-trip | INTT(NTT(a))==a on CPU | all elements match | pending |
| C4 | GPU round-trip | INTT(NTT(a))==a on GPU | all elements match | pending |
| C5 | cross-validate seed 0x1 | CPU NTT == GPU NTT element-wise | 0 mismatches | pending |
| C6 | cross-validate seed 0xDEAD | random-ish input | 0 mismatches | pending |
| C7 | cross-validate seed 0xCAFE | random-ish input | 0 mismatches | pending |

Run status: **pending — requires 6900XT**

Also run with ML-DSA parameters: `./ntt_cross_verify_6900xt 256 8380417 1753`

---

## Regression Tests (run after any kernel change)

After every modification to `ntt_cpu.c`, `ntt_stockham.c`, or `ntt_gpu*.hip`:

```sh
make clean && make cpu stockham gpu-6900xt verify-6900xt
make test-cpu                                     # must PASS
make test-stockham                                # must PASS
./ntt_polymul 1                                   # must PASS (all 8 tests)
make cross-verify                                 # must: 7 passed, 0 failed
make cross-verify N=256 Q=8380417 OMEGA=1753      # ML-DSA
```

---

## Performance Baselines

Benchmark sweep: `./ntt_bench` — runs all CPU algorithms n=64..4096.

| Platform | Algorithm | n | q | NTT/s | ns/butterfly |
|----------|-----------|---|---|-------|--------------|
| 5950X | CT-DIT | 256 | 3329 | ~204,000 | ~4.6 ns |
| 5950X | Stockham | 256 | 3329 | ~247,000 | ~3.8 ns |
| 5950X | Montgomery | 256 | 3329 | ~199,000 | ~4.7 ns |
| 5950X | polymul (Stockham) | 256 | 3329 | ~76,000 | 13.2 µs/call |
| 6900XT GPU | CT-DIT | 256 | 3329 | pending | pending |
| 6900XT GPU | Stockham | 256 | 3329 | pending | pending |
| MI300A GPU | Stockham | 256 | 3329 | pending | pending |

Benchmark commands:
```sh
./ntt_bench                         # CPU sweep: all algorithms, n=64..4096
make bench-cpu                      # CT-DIT CPU benchmark
make bench-stockham                 # Stockham CPU benchmark
make bench-mlkem-gpu                # 6900XT baseline
make bench-gpu-mi300a N=256 Q=3329 OMEGA=17   # MI300A baseline
```

---

## Phase 4 Optimization Targets (after baselines established)

Each optimization: implement → cross-validate → benchmark → update table above.

| Optimization | Expected gain | Test to add |
|---|---|---|
| GPU Stockham (no bit-reversal) | −5% vs CT-DIT GPU | GS1–GS6 + cross-validate |
| Twisted-NTT for negacyclic | required for PQC | new negacyclic polymul tests |
| LDS-cached twiddles (n=256) | −1.5% | same cross-validate |
| Fused multi-stage kernel | −1.6% | same cross-validate |
| Warp-level batching (n=256) | −2.55% | batch round-trip |
