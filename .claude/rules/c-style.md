---
paths:
  - "**/*.c"
  - "**/*.h"
  - "**/*.hip"
---

<!-- This file loads ONLY when Claude opens a .c, .h, or .hip source file.
     It does not consume context tokens during documentation or planning sessions. -->

# C / HIP Source File Rules

## Language

- C only. Reject all C++ constructs: no classes, templates, namespaces, references,
  RAII, or `new`/`delete`. This applies to both host code and GPU kernels.
- Compiler: hipcc or direct clang with `--offload-arch=gfx942` (MI300A) or
  `--offload-arch=gfx1100` (6900XT). Enable `-Wall -Wextra`; fix all warnings.
- Build must be clean (zero warnings) before a unit is marked done.

## Modular Design

- Every algorithm segment (butterfly, twiddle generation, modular reduction, bit-reversal,
  kernel dispatch) must be its own function or file.
- CPU kernel and GPU kernel must share the same public function signature so one can
  replace the other by changing only the dispatch layer.
- No global mutable state. Pass all parameters explicitly.

## Parameterization

- Transform size n, modulus q, primitive root ω, block size, and algorithm variant
  must all be runtime parameters — command-line arguments or config file entries.
- No hardcoded architecture constants. Query via `hipGetDeviceProperties()` at startup
  and store in a config struct passed to all kernels.

## Hardware Measurement

- Use system calls and HIP queries at program start to measure: warpSize,
  sharedMemPerBlock, regsPerBlock, multiProcessorCount, totalGlobalMem.
- Print a formatted hardware summary table to stdout at startup.
- Use measured values to select algorithm variant, block size, and LDS tile size.

## GPU Kernel Rules

- Block size must be a multiple of 64 (MI300A wavefront width). Use `prop.warpSize`
  at runtime — RDNA3 (6900XT) has warpSize=32; CDNA3 (MI300A) has warpSize=64.
- All read-only kernel pointer arguments must be declared `const __restrict__`.
- Use `__launch_bounds__(MAX_THREADS, MIN_BLOCKS)` to control VGPR allocation.
- LDS arrays: pad by 1 element to avoid bank conflicts, e.g. `__shared__ uint64_t
  tile[TILE][TILE + 1]`.
- Twiddle factor tables: precompute on CPU in Montgomery form; store read-only in HBM.

## Modular Arithmetic

- Use Montgomery multiplication for sustained multiply chains (NTT butterfly body).
- Use lazy/deferred reduction (no mod every step) when 64-bit headroom permits.
- No integer division in hot paths — replace with Montgomery or Barrett reduction.

## Comments

- Every function must have a header comment stating: purpose, inputs, outputs,
  algorithm reference (paper/section), and any invariants assumed or maintained.
- Inline comments must accurately describe the current code. Update them whenever
  the code changes. Stale comments are bugs.
- Comment any non-obvious constant: `uint32_t Q = 3329; /* ML-KEM prime, 13*2^8+1 */`

## Output and Diagnostics

- All stdout output must be in formatted, fixed-width tables — no raw variable dumps.
- Benchmark results must be written to a timestamped file in addition to stdout.
- Print: kernel name, n, q, block size, achieved bandwidth (GB/s), throughput (NTT/s).
