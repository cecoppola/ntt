## Phase Progress
| Phase | Name          | Status  |
|-------|---------------|---------|
| 1     | Design & Plan | done    |
| 2     | CPU Reference | done    |
| 3     | GPU Kernel    | pending |
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

## API Surface
```c
/* ntt_cpu.c */
uint64_t *ntt_alloc_twiddles(const ntt_params_t *p);
uint64_t *ntt_alloc_twiddles_inv(const ntt_params_t *p);
void ntt_forward(uint64_t *a, const uint64_t * restrict twiddles, const ntt_params_t *p);
void ntt_inverse(uint64_t *a, const uint64_t * restrict twiddles_inv, const ntt_params_t *p);
```

## Deviations from Design Doc
None.

## Next Step
Begin Phase 3: write ntt_gpu.c — HIP kernel wrapping ntt_forward with the same public signature, targeting gfx1100 (6900XT). Exit criterion: builds clean with hipcc, selftest PASS on GPU matching CPU output element-wise.
