# ecalc — the e-paper's pipeline on MI300A (PLAN.md §8)

    module load rocm && make          # ref/gen_e, ntt.o mem.o crt.o bigint.o rns_mul.o, tests/t_*
    salloc -p PPAC_MI300A_SPX -N1 --gpus=4 -t 6:00:00 --no-shell
    ./run tests/t_modarith 1000       # -> results/t_modarith.txt
    ./run tests/t_ntt 31
    ./run tests/t_mul 20              # part 1 at a 2^20 pool, then the 2^31 pool; "0 big" for the 10dP-size product; "0 batch"
    ./run tests/t_crt 30
    ./run ref/gen_e 1000000000 ref    # e_1000000000.txt + .sha256 (442 s, 32 threads)

| file | what |
|---|---|
| `modarith.h` | four primes, FP64-Barrett modmul (one lazy operand!), canon64, Shoup alternative, roots |
| `ntt.h/.c` | tiled DIF forward / DIT inverse, scale and pointwise fusions, broadcast pointwise, load+canon |
| `bigint.h/.c` | limb arrays; parallel add/sub, schoolbook, shifts |
| `mem.h/.c` | NUMA-pinned registered staging, registered host pools, grow-only device pools, RSS |
| `crt.h/.c` | CPU Garner + 3-limb carry window, plain and quartered layouts |
| `rns_mul.h/.c` | tiers: schoolbook, mdev, mdev_pair, batch (GPU S-stripe CRT, staged fallback), grpB, Karatsuba/chunked split |
| `newton.h/.c` | Newton reciprocal (self-correcting doubling), Barrett divmod with corrections, Knuth D |
| `binsplit.h/.c` | e = Σ 1/k!: seed spans, level loop with schoolbook / batch / mdev_pair tiers, registered level pools |
| `todec.h/.c` | radix conversion: divisor cache, TOP/MID/DEEP levels, GPU LEAF kernel by 10¹⁸ |
| `verify.h/.c` | tier-1 residues mod eight 62-bit primes (incl. the digit string), tier-2 windows |
| `ecalc.c` | driver: `./run ecalc <digits> [outfile]` (env POOL_LOG, NTT_B16_STG, PW_FUSE, RNS_CRT_LAYOUT, ECALC_VERBOSE=2 for per-level lines) |
| `tests/` | one GMP-checked program per module; `harness.h` (VERIFY, generators, GMP bridges, META/RESULT) |
| `ref/` | `gen_e.c` and the reference digit files |

Env knobs: `NTT_B16_STG` (via `ntt_stg`), `PW_FUSE` (`ntt_pw_fuse`), `RNS_CRT_LAYOUT` (0 plane per node, 1 quartered), `RNS_VERBOSE` (1 per-call times, 2 CRT re-runs).
Results: RESULTS.md §38 (steps 0–4), §39 (steps 5–8, end-to-end runs).
