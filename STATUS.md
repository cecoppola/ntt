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
| TESTS.md | pending | Stub only; test framework not yet written |
| ntt_cpu.c | done | CT-DIT, lazy reduction, selftest PASS (ML-KEM + ML-DSA); 269k NTT/s |
| ntt_gpu.hip | done | CT-DIT per-stage kernels; builds clean; selftest deferred to 6900XT |

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

## Next Step
Phase 4 (MI300A tuning) blocked pending 6900XT runtime verification. Immediate: write Makefile
covering both targets (gfx1100 and gfx942), then run GPU selftest on 6900XT to close Phase 3.
