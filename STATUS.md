## Phase Progress
| Phase | Name          | Status  |
|-------|---------------|---------|
| 1     | Design & Plan | pending |
| 2     | CPU Reference | pending |
| 3     | GPU Kernel    | pending |
| 4     | MI300A Tuning | pending |

## Component Status
| File | Status | Notes |
|------|--------|-------|
| REFERENCE.md | done | Research reference: HIP/C, NTT math, MI300A HW, PQC, Claude efficiency |
| .claude/settings.json | done | Hooks: block Bash builtins, block destructive cmds, compact re-injection |
| .claude/hooks/ | done | 5 hook scripts: block-bash-builtins, block-destructive, compact-reinject, stop-verify, notify |
| .claude/rules/c-style.md | done | Path-scoped C/HIP rules; loads only when editing .c/.h/.hip |
| (source files) | pending | No C source written yet |

## API Surface
<!-- None implemented yet. -->

## Deviations from Design Doc
None.

## Next Step
Begin Phase 1: create PLAN.md — identify all NTT segments, estimate runtime fractions, and populate algorithm option tables. Exit criterion: PLAN.md reviewed and approved before any source is written.
