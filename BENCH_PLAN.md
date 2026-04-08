# Benchmarking & Parameter Optimization Plan

## Objective

Build a benchmarking infrastructure that:

1. Produces structured, machine-readable performance data from every algorithm
2. Automatically computes CPU→GPU speedup when both are available
3. Provides **single-parameter sweep tools**: pick one variable, run a sweep,
   get a ranked output file that explains the result and names interaction effects
4. Supports tiered run modes — quick (≤5 min), standard (≤15 min), full (overnight)
5. Stores results in append-only CSV files for cross-session comparison

Pre-deployment requirement: all sweep tools must be implemented and smoke-tested on
CPU/6900XT before the MI300A session. No agent will be available on MI300A.

---

## Design Principles

- **No new languages or frameworks.** Pure C + HIP + shell. No Python, no Make
  targets that require network access, no external plotting tools.
- **Single-parameter sweeps.** Each sweep tool fixes all variables except one,
  runs the full range, prints a ranked table, and writes a CSV + a human-readable
  `.txt` report that names the top-3 interaction variables.
- **Tiered run time.** Every sweep tool accepts `--quick`, `--standard`, `--full`.
  Quick mode: ≤5 min, covers the most important configurations.
  Standard: ≤15 min, full grid for the primary parameter.
  Full: overnight, all cross-product combinations.
- **Self-contained binaries.** Single command, no config files. All knobs are
  command-line arguments.
- **Reproducible.** Results include hostname, date, ROCm version, device name.

---

## Parameter Space

### Transform parameters

| Parameter | Symbol | Values |
|-----------|--------|--------|
| Transform size | `n` | 64, 128, 256, 512, 1024, 2048, 4096 |
| Modulus | `q` | see table below |
| Iterations | `iters` | auto: `max(100, 20_000_000 / n)` |

### Moduli

| Modulus | Name | Form | Use case |
|---------|------|------|----------|
| 3329 | ML-KEM | 13·2^8+1 | NIST FIPS 203 (Kyber) |
| 8380417 | ML-DSA | 2^23−2^13+1 | NIST FIPS 204 (Dilithium) |
| 12289 | FALCON/NewHope | 3·2^12+1 | FALCON-512/1024, NewHope KEM |
| 1073479681 | TFHE-NTT | 2^30−2^18+1 | TFHE bootstrapping, ~1.07B |
| 998244353 | NTT-prime | 119·2^23+1 | Competitive programming, radix-2 to n=2^23 |
| 469762049 | NTT-prime-2 | 7·2^26+1 | BFV/CKKS RNS component |
| 786433 | FHE-small | 3·2^18+1 | BFV small modulus RNS chain |

Each prime is NTT-friendly (q = k·2^m + 1, supporting power-of-2 transforms up to
2^m). Omega values for each are computed at runtime via `mod_pow(primitive_root, (q-1)/n, q)`.

### GPU tuning parameters

| Parameter | Symbol | Quick | Standard | Full | Notes |
|-----------|--------|-------|----------|------|-------|
| Block size | `B` | 128 only | 64, 128, 256 | 64, 128, 256 | Must be multiple of wavefront |
| Batch size | `BATCH` | 1, 16 | 1, 4, 16, 64 | 1, 4, 16, 64 | Simultaneous NTTs per launch |
| Twiddle source | `TWSRC` | HBM | HBM, LDS | HBM, LDS | LDS fits for n≤256 (2KB) |
| Algorithm | `ALG` | stockham | ct-dit, stockham | ct-dit, stockham, stockham-lds | |

### CPU tuning parameters

| Parameter | Symbol | Values |
|-----------|--------|--------|
| Algorithm | `ALG` | ct-dit, stockham, montgomery |
| Modulus | `q` | all 7 from table above |
| Transform size | `n` | 64..4096 |

---

## Output Format

### CSV schema (one row per measurement)

```
timestamp,hostname,device,algorithm,n,q,omega,block_size,batch,iters,
elapsed_s,ntts_per_s,ns_per_butterfly,gb_per_s,notes
```

- `device`: CPU model string or GPU name from `hipGetDeviceProperties`
- `algorithm`: `ct-dit-cpu`, `stockham-cpu`, `montgomery-cpu`, `ct-dit-gpu`,
  `stockham-gpu`, `stockham-gpu-lds`, `polymul-cpu`, `polymul-gpu`
- `gb_per_s`: GPU = `n * 8 * 2 * iters / elapsed_s`; CPU = estimated from cache model
- `notes`: free text, e.g. `"block=128 batch=16 lds_twiddles=yes"`

### Results files

| File | Written by | Content |
|------|-----------|---------|
| `results/cpu_sweep.csv` | `ntt_bench` | CPU algorithm × n × q |
| `results/gpu_sweep.csv` | `ntt_gpu_bench` | GPU algorithm × n × q × block |
| `results/gpu_tune.csv` | `ntt_gpu_tune` | Full GPU parameter grid search |
| `results/compare.csv` | `bench_compare.sh` | CPU + GPU side-by-side with speedup |
| `results/polymul.csv` | `ntt_polymul` | End-to-end polymul benchmark |

All files are append-only with timestamps. `results/` is gitignored.

### Per-sweep report format (`.txt`)

Each sweep tool writes `results/<param>_sweep_<timestamp>.txt`:

```
╔══════════════════════════════════════════════════════════════════╗
║  Block Size Sweep  —  n=256, q=3329, algo=stockham, batch=16     ║
║  MI300A  2026-04-08  ROCm 7.0.3                                  ║
╠══════════╦═══════════╦══════════════╦═════════════╦═════════════╣
║ block    ║  NTT/s    ║  ns/butterfly║  GB/s       ║  vs best    ║
╠══════════╬═══════════╬══════════════╬═════════════╬═════════════╣
║   128 ★  ║  486,200  ║     0.52     ║  15.2       ║  1.00×      ║
║   256    ║  441,000  ║     0.57     ║  13.8       ║  0.91×      ║
║    64    ║  312,000  ║     0.81     ║   9.8       ║  0.64×      ║
╚══════════╩═══════════╩══════════════╩═════════════╩═════════════╝
  Recommendation: block=128
  Top interaction effects:
    1. batch size (batch=16 was fixed; try --sweep-batch to see its effect)
    2. twiddle source (LDS twiddles not tested; add --lds to enable)
    3. transform size (result is n=256 specific; rerun with --all-n for full picture)
```

The "Top interaction effects" section names the next variables worth sweeping,
so the user knows which optimization cycle to run next.

---

## New Files

### `ntt_timing.h` — shared timing and result struct

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

void bench_clock_start(struct timespec *t);
double bench_clock_elapsed(const struct timespec *t);
void bench_print_table(const bench_result_t *r);     /* terminal table  */
void bench_write_csv(const bench_result_t *r, const char *outfile);
void bench_write_report(const bench_result_t *results, int count,
                        const char *swept_param, const char *fixed_params,
                        const char *outfile);         /* .txt ranked report */
uint64_t bench_iters_for_n(uint64_t n);              /* auto-scale iters  */
```

### `ntt_gpu_bench.hip` — GPU sweep binary

Sweeps n × q × block_size for both `ct-dit-gpu` and `stockham-gpu`.

```
./ntt_gpu_bench [--csv results/gpu_sweep.csv]
                [--n 256] [--all-n]
                [--q 3329] [--all-q]
                [--block 128] [--all-blocks]
                [--algo stockham] [--all-algos]
                [--iters 0]
                [--quick | --standard | --full]
```

With `--quick`: n=256, q=3329 only, block=128 (~1 min).
With `--standard`: all n, q=3329, all blocks (~8 min).
With `--full`: all n × all q × all blocks (~45 min).

### `ntt_gpu_tune.hip` — MI300A parameter optimization loop

Grid search over all GPU tuning parameters. Accepts a `--sweep-<param>` flag
to fix all others and sweep one:

```
./ntt_gpu_tune [--outfile results/gpu_tune.csv]
               [--quick | --standard | --full]
               [--sweep-block]    # sweep block size, fix others
               [--sweep-batch]    # sweep batch size, fix others
               [--sweep-n]        # sweep transform size, fix others
               [--sweep-q]        # sweep modulus, fix others
               [--sweep-algo]     # sweep algorithm variant, fix others
               [--n 256] [--q 3329] [--block 128] [--batch 16] [--algo stockham]
```

Each `--sweep-<param>` run:
1. Runs all values of that parameter, holding all others at their defaults
2. Prints ranked table to terminal
3. Appends rows to `--outfile` CSV
4. Writes `results/<param>_sweep_<timestamp>.txt` with ranked table + interaction notes

Quick mode per sweep: ≤5 min. Standard: ≤15 min. Full: all cross-products.

Default values (pre-optimized for MI300A):
- n=256, q=3329, block=128, batch=16, algo=stockham

### `bench_compare.sh` — CPU vs GPU comparison

Shell script using `awk`. Reads `results/cpu_sweep.csv` and `results/gpu_sweep.csv`,
joins on (algorithm_base, n, q), computes speedup, prints table.

```
bash bench_compare.sh [--n 256] [--q 3329] [--all]
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

---

## Changes to Existing Files

### `ntt_bench.c`
- Add `--csv results/cpu_sweep.csv` flag; append one row per (algorithm, n, q)
- Add `--sweep` mode: all algorithm × n × q combinations
- Add `--all-q` flag: sweep all 7 moduli from the table above

### `ntt_cpu.c`, `ntt_stockham.c`, `ntt_mont.c`
- Add `--csv <outfile>` and `--sweep` flags (consistent with bench interface)
- Replace inline timing with `bench_timing.h` helpers

### `ntt_polymul.c`
- Add `--csv results/polymul.csv` and `--sweep` flags
- Add `--all-q` flag to test all 7 moduli

### `ntt_gpu.hip`, `ntt_gpu_stockham.hip`
- Add `--csv results/gpu_sweep.csv` and `--sweep` flags
- Report both wall time (H↔D + kernel) and kernel-only time via `hipEvent_t`
- GPU timing note: MI300A unified HBM → H↔D ≈ 0; 6900XT PCIe → wall time matters

### `Makefile`

```makefile
bench-sweep:        ntt_bench --sweep --all-q → results/cpu_sweep.csv
gpu-bench-sweep:    ntt_gpu_bench --standard  → results/gpu_sweep.csv (GPU required)
gpu-tune-quick:     ntt_gpu_tune --quick       → results/gpu_tune.csv (GPU required)
gpu-tune-full:      ntt_gpu_tune --full        → results/gpu_tune_full.csv
compare:            bench_compare.sh
results-clean:      rm results/*.csv results/*.txt
```

### `.gitignore`
Add `results/`.

---

## MI300A Deployment Sequence

```bash
# 1. Build
module load PrgEng-cray-amd/8.5.0 rocm/7.0.3 craype-accel-amd-gfc942
make gpu-stok-mi300a gpu-bench-mi300a gpu-tune-mi300a

# 2. Environment probe
bash mi300a_probe.sh 2>&1 | tee results/mi300a_probe.txt

# 3. Quick block size sweep (~3 min) — start here
./ntt_gpu_tune_mi300a --sweep-block --quick

# 4. Read the recommendation; set optimal block size
# e.g. optimal is block=128 → use --block 128 in subsequent runs

# 5. Batch size sweep with optimal block (~3 min)
./ntt_gpu_tune_mi300a --sweep-batch --block 128 --quick

# 6. Modulus sweep (which q benefits most from GPU?) (~4 min)
./ntt_gpu_tune_mi300a --sweep-q --block 128 --batch 16 --quick

# 7. Transform size sweep (~3 min)
./ntt_gpu_tune_mi300a --sweep-n --block 128 --batch 16 --quick

# 8. Compare all results against CPU baseline
bash bench_compare.sh --all

# 9. Optional: full grid (~15-45 min overnight)
./ntt_gpu_tune_mi300a --full --outfile results/gpu_tune_full.csv
```

Each step ≤5 min. After step 4, optimal block size is known. Each subsequent step
applies the findings from previous steps. The sweep `.txt` report names what to run next.

---

## Phase Mapping

| Phase | Work | Blocking |
|-------|------|---------|
| Now (CPU-only) | `ntt_timing.h`; `--csv`/`--sweep` for all CPU binaries; `ntt_bench.c` `--all-q` | Nothing |
| On 6900XT | `ntt_gpu_bench.hip`; validate `bench_compare.sh`; test all moduli on GPU | 6900XT |
| On MI300A | Run deployment sequence; apply optimal params to production kernel | MI300A |

---

## Open Questions

1. **Kernel-only vs wall time**: use kernel-only (`hipEvent_t`) as primary optimization
   metric; report wall time as secondary. MI300A APU: H↔D ≈ 0 so they converge.
   6900XT PCIe: wall time is the real-world cost.

2. **Batch dimension**: include batch=1 and batch=16 in quick mode; all 4 values
   in standard. Flag if batching gives >10% gain (likely for small n).

3. **CSV append vs overwrite**: append with timestamp; `results-clean` for reset.

4. **Stages-per-kernel fusion** (SPK parameter): defer to Phase 4 — requires new
   fused kernel variant, not a runtime parameter.

5. **RNS multi-modulus chains** (BFV/CKKS): add moduli 469762049 and 786433 to
   the sweep table. RNS chains use simultaneous NTTs at multiple primes — the
   batch sweep will naturally reveal whether this pattern is GPU-efficient.
