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

## Unit Tests — GPU Kernels (`ntt_gpu.hip`)

Run: `make test-gpu` (requires 6900XT)

| # | Test | Kernel | Input | Expected |
|---|------|--------|-------|----------|
| G1 | bit-reversal correctness | cv_bitrev_kernel | [0..255] natural | bit-reversed order |
| G2 | single stage butterfly | cv_stage_kernel (stage 1) | matches CPU stage 1 output | element-wise == |
| G3 | delta → all-ones | gpu_ntt | a=[1,0,...] | [1,1,...,1] |
| G4 | all-ones → [n,0,...] | gpu_ntt | a=[1,1,...] | [256,0,...,0] |
| G5 | round-trip GPU | gpu_ntt + ntt_inverse | a=[0..255] mod q | original |

Run status: **pending — requires 6900XT**

---

## Cross-Validation Tests (`ntt_cross_verify.hip`)

Run: `make cross-verify` (requires 6900XT) — **Phase 3 exit criterion**

| # | Test | Description | Pass condition |
|---|------|-------------|----------------|
| C1 | delta → all-ones | CPU and GPU both produce [1,...,1] | cpu==expected AND gpu==expected AND cpu==gpu |
| C2 | all-ones → [n,0,...] | CPU and GPU both produce [256,0,...,0] | all three conditions above |
| C3 | CPU round-trip | INTT(NTT(a))==a on CPU | all elements match |
| C4 | GPU round-trip | INTT(NTT(a))==a on GPU | all elements match |
| C5 | cross-validate seed 0x1 | CPU NTT == GPU NTT element-wise | 0 mismatches |
| C6 | cross-validate seed 0xDEAD | random-ish input | 0 mismatches |
| C7 | cross-validate seed 0xCAFE | random-ish input | 0 mismatches |

Run status: **pending — requires 6900XT**

Also run with ML-DSA parameters: `./ntt_cross_verify_6900xt 256 8380417 1753`

---

## Regression Tests (run after any kernel change)

After every modification to `ntt_cpu.c` or `ntt_gpu.hip`:

```sh
make clean && make cpu gpu-6900xt verify-6900xt
make test-cpu                                     # must PASS
make cross-verify                                 # must: 7 passed, 0 failed
make cross-verify N=256 Q=8380417 OMEGA=1753      # ML-DSA
```

---

## Performance Baselines

Recorded in `bench_*.txt`. Update after any kernel change.

| Platform | n | q | NTT/s (baseline) | ns/butterfly |
|----------|---|---|-----------------|--------------|
| 5950X CPU (dev) | 256 | 3329 | ~269,000 | ~3.6 ns |
| 5950X CPU (dev) | 256 | 8380417 | ~271,000 | ~3.6 ns |
| 6900XT GPU | 256 | 3329 | pending | pending |
| MI300A GPU | 256 | 3329 | pending | pending |

Benchmark commands:
```sh
make bench-mlkem-cpu              # 5950X baseline
make bench-mlkem-gpu              # 6900XT baseline
make bench-gpu-mi300a N=256 Q=3329 OMEGA=17   # MI300A baseline
```

---

## Phase 4 Optimization Targets (after baselines established)

Each optimization: implement → cross-validate → benchmark → update table above.

| Optimization | Expected gain | Test to add |
|---|---|---|
| Stockham (no bit-reversal) | −5% vs CT | round-trip + cross-validate |
| Montgomery multiplication | −6% vs lazy | round-trip + cross-validate |
| LDS-cached twiddles (n=256) | −1.5% | same cross-validate |
| Fused multi-stage kernel | −1.6% | same cross-validate |
| Warp-level batching (n=256) | −2.55% | batch round-trip |
