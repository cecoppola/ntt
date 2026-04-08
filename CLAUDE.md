# NTT / MI300A Project

<!-- Human maintainer note: this file is loaded into Claude's context every session.
     Keep it under 200 lines. Detailed rules live in .claude/rules/. Build and
     library info lives in BUILD.md (imported below). -->

Goal: a highly-performant, elegant C implementation of the Number Theoretic Transform
that exploits the hardware architecture of the AMD MI300A APU, compiled in HIP/ROCm
and cross-compiled from a different development system.

## Key Constraints

- Language: C only. No C++ features anywhere — not in kernels, not in host code.
- Target chip: AMD MI300A (CDNA3). Wavefront = 64 threads. Unified HBM3 memory.
- Dev sequence: 5950X (CPU reference) → 5905X + 6900XT (GPU dev) → MI300A (final).
- All major parameters (n, q, block size, algorithm choice) must be runtime-configurable.
- Query hardware at runtime via system calls; never hardcode architecture constants.
- Design for modularity: CPU and GPU kernels share the same function signature so one
  can replace the other without changing any calling code.

## Session Start Protocol

1. Read STATUS.md first — it is the session navigator. Do not read other files speculatively.
2. Proceed directly to the unit listed under "Next Step" in STATUS.md.
3. Use Grep and Glob to locate symbols; use Read only for files you are about to edit.
4. After completing a unit: update STATUS.md, verify build, then stop or compact.

## Tool Discipline

Use dedicated tools, not Bash shell commands, for file operations:
- Search file contents → Grep tool (not grep/rg in Bash)
- Read a file → Read tool (not cat/head/tail in Bash)
- Find files by pattern → Glob tool (not find/ls in Bash)
- Edit a file → Edit tool (not sed/awk in Bash)
Reserve Bash for: build, compile, run, profile, and git commands only.

## Formatting Style

Be on the lookout for interesting and neat formatting styles (ANSI color schemes, table
layouts, box-drawing characters, Unicode art). Try them out in project documents or ask
the user when something looks promising.

## Compact Instructions

When compacting, preserve:
- The "Next Step" entry from STATUS.md and the current implementation unit
- Any API signatures already implemented (from the API Surface section of STATUS.md)
- Deviations from the design doc recorded in STATUS.md
- Active build errors or test failures being investigated

Discard: exploratory investigation results, superseded approaches, verbose tool output,
and any context that is already captured in STATUS.md or the source files.

## Imported Rules and Build Environment

@RULES.md
@BUILD.md

## Project Files

<!-- Update this list when files are added or removed. -->
MD files:  CLAUDE.md, BUILD.md, PLAN.md, RULES.md, TESTS.md, STATUS.md, REFERENCE.md
Reference: table_style.md, code_style.c
Hooks:     .claude/settings.json, .claude/hooks/
Rules:     .claude/rules/c-style.md
