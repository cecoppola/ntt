## Phase Progress
| Phase | Name          | Status      |
|-------|---------------|-------------|
| 1     | Design & Plan | done        |
| 2     | CPU Reference | done        |
| 3     | GPU Kernel    | done        |
| 4     | MI300A Tuning | pending     |

## Component Status
| File | Status | Notes |
|------|--------|-------|
| REFERENCE.md | done | Research reference: HIP/C, NTT math, MI300A HW, PQC, Claude efficiency |
| .claude/settings.json | done | Hooks: block Bash builtins, block destructive cmds, compact re-injection |
| .claude/hooks/ | done | 5 hook scripts: block-bash-builtins, block-destructive, compact-reinject, stop-verify, notify |
| .claude/rules/c-style.md | done | Path-scoped C/HIP rules; loads only when editing .c/.h/.hip |
| PLAN.md | done | 6-segment algorithm tables; ~49% optimal gain identified; Phase 2 stack chosen |
| TESTS.md | done | CPU tests PASS, Stockham PASS, polymul PASS; GPU/cross-val pending HW |
| ntt.h | done | Shared header: ntt_params_t, full API declarations for lazy + Montgomery |
| ntt_cpu.c | done | CT-DIT, lazy reduction, selftest PASS (ML-KEM + ML-DSA); ~204k NTT/s |
| ntt_mont.c | done | Montgomery butterfly; selftest PASS; 0.73× vs lazy on x86_64 (expected) |
| ntt_stockham.c | done | Stockham auto-sort; all 6 tests PASS; ~247k NTT/s (fastest CPU algorithm) |
| ntt_bench.c | done | Side-by-side CPU sweep n=64..4096; Stockham wins n=64..2048; Montgomery wins n=4096 |
| ntt_polymul.c | done | Cyclic polymul (cyclic conv, omega^n=1); all 8 tests PASS; ~76k/s; notes on negacyclic |
| ntt_gpu.hip | done | CT-DIT per-stage kernels; builds clean gfx1100+gfx942; selftest deferred to 6900XT |
| ntt_gpu_stockham.hip | done | Stockham GPU: double-buffer ping-pong, no bit-reversal; builds clean; selftest deferred |
| ntt_cross_verify.hip | done | 7-test CPU vs GPU verifier; builds clean gfx1100+gfx942; run on 6900XT |
| Makefile | done | All CPU+GPU targets; bench-sweep, polymul, gpu-stok-{6900xt,mi300a} targets added |
| mi300a_probe.sh | done | 10-section MI300A environment probe; run on target to guide tuning |

## API Surface
```c
/* ntt_cpu.c, ntt_stockham.c, ntt_gpu.hip, ntt_gpu_stockham.hip — identical signatures */
uint64_t *ntt_alloc_twiddles(const ntt_params_t *p);
uint64_t *ntt_alloc_twiddles_inv(const ntt_params_t *p);
void ntt_forward(uint64_t *a, const uint64_t *twiddles, const ntt_params_t *p);
void ntt_inverse(uint64_t *a, const uint64_t *twiddles_inv, const ntt_params_t *p);

/* ntt_polymul.c */
void polymul_ntt(const uint64_t *f, const uint64_t *g, uint64_t *c,
                 const uint64_t *tw, const uint64_t *twi, const ntt_params_t *p);
```

## Deviations from Design Doc
- ntt_gpu.hip uses `__restrict__` not `restrict` — hipcc compiles .hip as C++; recorded in c-style.md.
- GPU ntt_forward uses per-stage reduction (not lazy) for device-code safety (q < 2^32 constraint).
- GPU selftest deferred: no ROCm device on 5950X build machine; must run on 6900XT.
- Montgomery standalone (ntt_mont.c) is 0.73× vs lazy on x86_64: REDC overhead exceeds hardware mulq.
  ntt_bench.c mnt_ntt wrapper is faster (~1.15× vs CT-DIT at n=4096) due to fused enter/butterfly/exit
  with no separate reduction passes. Bug fixed: removed redundant %q on butterfly input (was division).
- Stockham is ~20% FASTER than CT-DIT on CPU (unexpected): eliminates bit-reversal pass,
  more sequential memory access pattern. GPU benefit expected to be even larger.
- polymul_ntt computes cyclic convolution (mod X^n−1), not negacyclic (mod X^n+1).
  Standard Stockham/CT-DIT NTT computes cyclic regardless of whether omega has order n or 2n.
  ML-DSA cyclic test uses omega=3073009=1753² (order 256). Negacyclic twisted-NTT is Phase 4.

## Stockham Auto-Sort Index Formula
- p=2^s, qs=n/2^(s+1), half=n/2
- u=src[j+k*p], v=src[j+k*p+half] (j∈[0,p), k∈[0,qs))
- dst[j+k*2p]=u+wv, dst[j+k*2p+p]=u+q-wv, twiddle=tw[j*qs]
- GPU: tid in [0,n/2), j=tid%p (=tid&(p-1)), k=tid/p (=tid>>s)

## NTT Convolution Type
- omega^n ≡ 1 (mod q) → n-th root → cyclic NTT → pointwise mul gives cyclic conv (X^n−1)
- omega^n ≡ −1 (mod q) → 2n-th root → STILL cyclic NTT (twiddles sample same n points)
- Negacyclic (X^n+1, required for PQC): needs pre-multiply by ψ^i, cyclic NTT, post-multiply (Phase 4)

## Next Step
Hardware: run `make cross-verify` on 6900XT → verify both CT-DIT and Stockham GPU kernels.
GPU: also run `./ntt_gpu_stockham_6900xt 256 3329 17 1000` for GS1–GS6 tests.
CPU: implement twisted-NTT negacyclic polymul (`ntt_polymul_negacyclic.c`) — required for true
PQC multiplication and as Phase 4 baseline.
