## Phase Progress
| Phase | Name          | Status  |
|-------|---------------|---------|
| 1     | Design & Plan | done    |
| 2     | CPU Reference | done    |
| 3     | GPU Kernel    | done    |
| 4     | MI300A Tuning | pending |

## Component Status
| File | Status | Notes |
|------|--------|-------|
| REFERENCE.md | done | Research reference: HIP/C, NTT math, MI300A HW, PQC, Claude efficiency |
| .claude/settings.json | done | Hooks: block Bash builtins, block destructive cmds, compact re-injection |
| .claude/hooks/ | done | 5 hook scripts: block-bash-builtins, block-destructive, compact-reinject, stop-verify, notify |
| .claude/rules/c-style.md | done | Path-scoped C/HIP rules; loads only when editing .c/.h/.hip |
| PLAN.md | done | 6-segment algorithm tables; ~49% optimal gain identified; Phase 2 stack chosen |
| TESTS.md | done | 9 CPU unit tests (PASS), 5 GPU tests (pending HW), 7 cross-val tests (pending HW) |
| ntt.h | done | Shared header: ntt_params_t, full API declarations for lazy + Montgomery |
| ntt_cpu.c | done | CT-DIT, lazy reduction, selftest PASS (ML-KEM + ML-DSA); 269k NTT/s |
| ntt_mont.c | done | Montgomery butterfly; selftest PASS both param sets; CPU slower (0.73×) than lazy — expected on x86_64; GPU will flip this |
| ntt_gpu.hip | done | CT-DIT per-stage kernels; builds clean (gfx1100 + gfx942); selftest deferred to 6900XT |
| Makefile | done | Targets: cpu, gpu-6900xt, gpu-mi300a, verify-6900xt, verify-mi300a, test/bench presets |
| ntt_cross_verify.hip | done | 7-test CPU vs GPU verifier; builds clean gfx1100+gfx942; run on 6900XT |

## API Surface
```c
/* ntt_cpu.c and ntt_gpu.hip — identical public signatures */
uint64_t *ntt_alloc_twiddles(const ntt_params_t *p);
uint64_t *ntt_alloc_twiddles_inv(const ntt_params_t *p);
void ntt_forward(uint64_t *a, const uint64_t *twiddles, const ntt_params_t *p);
void ntt_inverse(uint64_t *a, const uint64_t *twiddles_inv, const ntt_params_t *p);
```

## Deviations from Design Doc
- ntt_gpu.hip uses `__restrict__` not `restrict` — hipcc compiles .hip as C++; recorded in c-style.md.
- GPU ntt_forward uses per-stage reduction (not lazy) for device-code safety (q < 2³² constraint).
- GPU selftest deferred: no ROCm device on 5950X build machine; must run on 6900XT.
- Montgomery is slower than lazy on x86_64 (0.73×): hardware mulq+div beats REDC overhead.
  Expected to reverse on GPU where no 128-bit multiply instruction exists.

## Next Step
CPU-side: implement Stockham FFT variant (ntt_stockham.c) — eliminates bit-reversal,
GPU-preferred algorithm, same public API. Verify vs lazy CPU output.
Hardware: run `make cross-verify` on 6900XT when available (Phase 3 close).
