# Benchmarking & Parameter Optimization Plan

## Objective

Build a benchmarking infrastructure that:

1. Produces structured, machine-readable performance data from every algorithm
2. Automatically computes CPU→GPU speedup when both are available
3. Provides parameter optimization loops that sweep GPU tuning variables and
   emit a ranked results table — runnable by the user on MI300A without an agent
4. Stores all results in versioned CSV files so performance changes are visible
   across development sessions

This is a pre-deployment requirement: all parameter optimization loops must be
implemented and tested on CPU/6900XT before the MI300A session.

---

## Design Principles

- **No new languages or frameworks.** Pure C + HIP + shell. No Python, no Make
  targets that require network access, no external plotting tools.
- **Structured output first.** Every benchmark binary writes a CSV row to a
  results file _and_ prints a human-readable table. The CSV is the ground truth.
- **Self-contained binaries.** Each benchmark binary runs standalone via a
  single command with no config files. All knobs are command-line arguments.
- **Reproducible.** Results files include hostname, date, ROCm version, device
  name. Two runs of the same binary produce directly comparable rows.

---

## Parameter Space

### Transform parameters (all algorithms)

| Parameter | Symbol | Sweep values |
|-----------|--------|--------------|
| Transform size | `n` | 64, 128, 256, 512, 1024, 2048, 4096 |
| Modulus | `q` | 3329 (ML-KEM), 8380417 (ML-DSA) |
| Iterations | `iters` | auto-scaled: `max(100, 20_000_000 / n)` |

### GPU tuning parameters (MI300A-specific)

| Parameter | Symbol | Sweep values | Notes |
|-----------|--------|--------------|-------|
| Launch block size | `B` | 64, 128, 256 | Must be multiple of wavefront (64) |
| Min blocks/CU hint | `M` | 1, 2, 4 | `__launch_bounds__(B, M)` at compile time; runtime sweep uses fixed upper bound |
| Twiddle source | `TWSRC` | HBM (current), LDS-cached | Per-stage vs preloaded in shared mem |
| Stages per kernel | `SPK` | 1 (current), 2, 4 | Kernel fusion: amortizes launch overhead |
| Batch size | `BATCH` | 1, 4, 16, 64 | Simultaneous NTTs per kernel launch |

### CPU tuning parameters

| Parameter | Symbol | Sweep values |
|-----------|--------|--------------|
| Algorithm | `ALG` | ct-dit, stockham, montgomery |
| Compiler opt level | `OPT` | -O2, -O3, -O3 -march=native |

---

## Output Format

### CSV schema (one row per measurement)

```
timestamp,hostname,device,algorithm,n,q,omega,block_size,batch,iters,
elapsed_s,ntts_per_s,ns_per_butterfly,gb_per_s,notes
```

- `device`: CPU model string or GPU name from `hipGetDeviceProperties`
- `algorithm`: `ct-dit-cpu`, `stockham-cpu`, `montgomery-cpu`,
  `ct-dit-gpu`, `stockham-gpu`, `stockham-gpu-lds`, `stockham-gpu-fused2`,
  `stockham-gpu-fused4`, `polymul-cpu`, `polymul-gpu`
- `gb_per_s`: memory bandwidth; for CPU = estimated from cache model;
  for GPU = measured via `n * sizeof(uint64_t) * 2 * iters / elapsed_s`
- `notes`: free text, e.g. `"block=128 min_blocks=2 lds_twiddles=yes"`

### Results files

| File | Written by | Content |
|------|-----------|---------|
| `results/cpu_sweep.csv` | `ntt_bench` | CPU algorithm × n × q sweep |
| `results/gpu_sweep.csv` | `ntt_gpu_bench` | GPU algorithm × n × q × B sweep |
| `results/gpu_tune.csv` | `ntt_gpu_tune` | Full GPU parameter grid search |
| `results/compare.csv` | `bench_compare.sh` | CPU + GPU side by side with speedup column |
| `results/polymul.csv` | `ntt_polymul` | End-to-end polymul benchmark |

All files are append-only. Each row has a timestamp, so multiple runs produce
a history. Directory `results/` is gitignored (data, not code).

---

## New Files

### `ntt_timing.h` — shared timing and result struct

Inline header included by all benchmark binaries. Provides:

```c
typedef struct {
    char      algorithm[32];
    char      device[128];
    uint64_t  n, q, omega;
    int       block_size;
    int       batch;
    uint64_t  iters;
    double    elapsed_s;
    double    ntts_per_s;
    double    ns_per_butterfly;
    double    gb_per_s;
    char      notes[128];
} bench_result_t;

/* Timing */
void bench_clock_start(struct timespec *t);
double bench_clock_elapsed(const struct timespec *t);

/* Output */
void bench_print_table(const bench_result_t *r);  /* pretty terminal table  */
void bench_write_csv(const bench_result_t *r,     /* append one CSV row     */
                     const char *outfile);

/* Auto-scale iters for a target ~2s wall time at the given n */
uint64_t bench_iters_for_n(uint64_t n);
```

### `ntt_gpu_bench.hip` — GPU algorithm × parameter sweep

Standalone binary. Sweeps n × q × block_size for both `ct-dit-gpu` and
`stockham-gpu`. Usage:

```
./ntt_gpu_bench_mi300a [--csv results/gpu_sweep.csv] [--n 256] [--all-n]
                       [--block 128] [--all-blocks] [--iters 0 (auto)]
```

With `--all-n --all-blocks` it runs the full sweep grid (~42 configurations)
and writes every result as a CSV row, printing a sorted table at the end.

### `ntt_gpu_tune.hip` — MI300A parameter optimization loop

This is the primary tool for deployment on MI300A. Runs a grid search over:

- `n` ∈ {64, 128, 256, 512, 1024}
- `block_size` ∈ {64, 128, 256}
- `batch` ∈ {1, 4, 16, 64}
- `algorithm` ∈ {ct-dit, stockham, stockham-lds (twiddles in LDS)}

For each configuration, runs `iters` forward NTTs, records result, writes CSV.
At the end: prints ranked table (best NTT/s first), emits recommendation:

```
╔══════════════════════════════════════════════════════════════════════╗
║  MI300A Parameter Optimization — 180 configurations × 2 param sets  ║
╠══════════════════════════════════════════════════════════════════════╣
║  OPTIMAL  n=256 q=3329:  stockham-lds  block=128  batch=16          ║
║           486,200 NTT/s  (2.39× vs CPU baseline 203,600 NTT/s)      ║
╚══════════════════════════════════════════════════════════════════════╝
```

All 180 rows written to `results/gpu_tune.csv`. User reads this file to
set the optimized parameters for the production kernel.

Usage:
```
./ntt_gpu_tune_mi300a [--outfile results/gpu_tune.csv] [--quick] [--full]
  --quick: n=256 only, 3×3×4 = 36 configs (~3 min on MI300A)
  --full:  all n, all configs, ~180 configs (~15 min on MI300A)
```

### `bench_compare.sh` — CPU vs GPU comparison runner

Shell script. Runs `ntt_bench` (CPU) and `ntt_gpu_bench` (GPU), reads their
CSV output files, joins on (n, q), computes speedup column, prints comparison
table. No Python required — uses `awk`.

Usage:
```
bash bench_compare.sh [--n 256] [--q 3329]
```

Output:
```
┌──────────────┬──────┬──────────┬──────────────┬──────────────┬─────────┐
│ Algorithm    │  n   │    q     │  CPU NTT/s   │  GPU NTT/s   │ Speedup │
├──────────────┼──────┼──────────┼──────────────┼──────────────┼─────────┤
│ ct-dit       │  256 │   3329   │    203,600   │    312,000   │  1.53×  │
│ stockham     │  256 │   3329   │    247,000   │    486,200   │  1.97×  │
│ polymul      │  256 │   3329   │     75,900   │    195,400   │  2.58×  │
└──────────────┴──────┴──────────┴──────────────┴──────────────┴─────────┘
```

The speedup column is the primary optimization signal. It is populated
automatically whenever both a CPU and GPU result exist in the CSV files.

---

## Changes to Existing Files

### `ntt_cpu.c`, `ntt_stockham.c`, `ntt_mont.c`

- Replace ad-hoc timing code with calls to `bench_timing.h` helpers
- Add `--csv <outfile>` flag; default behavior unchanged
- Remove per-binary timestamped output files (superseded by `results/cpu_sweep.csv`)
- Add `--sweep` mode: runs all (n, q) combinations and writes one CSV row each

### `ntt_bench.c`

- Replace current custom timing with `bench_timing.h`
- Add `--csv results/cpu_sweep.csv` output
- Already does the algorithm × n sweep; align column names to CSV schema

### `ntt_polymul.c`

- Add `--csv results/polymul.csv` output
- Add `--sweep` mode: runs ML-KEM and ML-DSA at n ∈ {64, 128, 256}

### `ntt_gpu.hip`, `ntt_gpu_stockham.hip`

- Add `bench_timing.h` output (HIP side: use `hipEvent_t` for device-side timing
  and `clock_gettime` for wall-clock timing, report both)
- Add `--csv results/gpu_sweep.csv` and `--sweep` flags
- GPU timing note: measure wall time around the full `gpu_stockham()` call
  (includes H→D copy, kernel, D→H copy) AND kernel-only time via `hipEvent_t`.
  Report both as separate columns: `wall_ntts_per_s` and `kernel_ntts_per_s`.

### `Makefile`

New targets:
```makefile
bench-sweep:     run ntt_bench --sweep → results/cpu_sweep.csv
gpu-bench-sweep: run ntt_gpu_bench --sweep → results/gpu_sweep.csv (requires GPU)
gpu-tune:        run ntt_gpu_tune --quick → results/gpu_tune.csv (requires GPU)
compare:         run bench_compare.sh
results-clean:   rm results/*.csv
```

### `.gitignore`

Add `results/` directory.

---

## MI300A Deployment Sequence (no agent)

The user runs these commands in order after logging into MI300A:

```bash
# 1. Build all GPU targets
module load PrgEng-cray-amd/8.5.0 rocm/7.0.3 craype-accel-amd-gfc942
make gpu-stok-mi300a gpu-bench-mi300a gpu-tune-mi300a

# 2. Run the environment probe (already exists)
bash mi300a_probe.sh 2>&1 | tee results/mi300a_probe.txt

# 3. Run the quick parameter optimization loop (~3 min)
./ntt_gpu_tune_mi300a --quick --outfile results/gpu_tune.csv

# 4. View the recommendation (terminal table printed by gpu_tune)
# Optimal block_size and batch printed at the end

# 5. Run the full sweep with optimal parameters
./ntt_gpu_bench_mi300a --all-n --all-blocks --csv results/gpu_sweep.csv

# 6. Compare against CPU baseline (if ntt_bench was built)
bash bench_compare.sh

# 7. Optionally run the full tune (~15 min)
./ntt_gpu_tune_mi300a --full --outfile results/gpu_tune_full.csv
```

After step 3, the user has everything needed to set compile-time constants
for the production kernel (block_size, LDS config, batch size). After step 7,
the full parameter sensitivity surface is in `results/gpu_tune_full.csv`.

---

## Phase Mapping

| Phase | Bench work | Blocking on |
|-------|-----------|-------------|
| Now (CPU-only) | Implement `ntt_timing.h`; refactor existing CPU benches; add `--sweep`/`--csv` to all CPU binaries; `ntt_bench.c` alignment | Nothing |
| On 6900XT | Build and test `ntt_gpu_bench.hip`; validate `bench_compare.sh` speedup column | 6900XT |
| On MI300A | Run `ntt_gpu_tune_mi300a --quick`; apply optimal params to production kernel | MI300A |

---

## Open Questions for Review

1. **GPU timing granularity**: should kernel-only time (`hipEvent_t`) be the
   primary metric, or wall time (includes H↔D transfers)?
   - Recommendation: report both; use kernel-only for optimization decisions
     (the MI300A APU has unified memory so H↔D copies will be near-zero there,
     but 6900XT has PCIe — wall time is meaningful on 6900XT, misleading on MI300A)

2. **Batch dimension**: should `ntt_gpu_tune` sweep batch sizes, or is that
   a separate Phase 4 kernel design decision?
   - Recommendation: include batch=1 and batch=16 at minimum; the tuner should
     flag if batching gives >10% gain (it likely will, especially for small n)

3. **CSV append vs overwrite**: append means the file grows across sessions and
   shows history; overwrite means the file always reflects the latest run.
   - Recommendation: append with timestamp; `results-clean` target for reset

4. **`ntt_bench.c` scope**: it currently runs all 3 CPU algorithms in one binary.
   Should it be split into per-algorithm binaries for `--sweep` mode, or stay
   combined?
   - Recommendation: keep combined (it is the CPU comparison tool); add `--csv`
     flag that writes one row per (algorithm, n, q) combination

5. **Stages-per-kernel fusion** (the `SPK` parameter above): this requires
   writing a new fused kernel variant, not just passing a runtime parameter.
   Should the tuner cover it, or defer to a separate Phase 4 task?
   - Recommendation: defer; `ntt_gpu_tune` sweeps only runtime parameters
     (block_size, batch, LDS flag). Kernel fusion is a separate implementation.
