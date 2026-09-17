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
| `binsplit.h/.c` | e = Σ 1/k!: seed spans, level loop with schoolbook / batch / mdev_pair tiers; level pools as four device regions with subtree ownership (WP3), mdev levels in a host pool |
| `todec.h/.c` | radix conversion: divisor cache, TOP/MID/DEEP levels, GPU LEAF kernel by 10¹⁸ |
| `verify.h/.c` | tier-1 residues mod eight 62-bit primes (incl. the digit string), tier-2 windows |
| `ecalc.c` | driver: `./run ecalc <digits> [outfile]` (env POOL_LOG, NTT_B16_STG, PW_FUSE, RNS_CRT_LAYOUT, ECALC_VERBOSE=2 for per-level lines) |
| `tests/` | one GMP-checked program per module; `harness.h` (VERIFY, generators, GMP bridges, META/RESULT) |
| `ref/` | `gen_e.c` and the reference digit files |

Env knobs: `NTT_B16_STG` (via `ntt_stg`), `PW_FUSE` (`ntt_pw_fuse`), `RNS_CRT_LAYOUT` (0 plane per node, 1 quartered), `RNS_VERBOSE` (1 per-call times, 2 CRT re-runs).
Phase 7 switches: `LIMB_BASE` (2 default, 10 = base-10¹⁸ limbs: 10dP and dc vanish, WP1), `NEWTON_ANCHOR` (1 default: doubling sequence anchored at the target precision; 0 = Phase 4 powers of two), `NEWTON_VERBOSE` (per-iteration trace with RSS), `BS_DEVICE_POOLS` (1 default: level pools as four device regions, WP3; 0 = registered host), `RNS_BATCH_LOCAL` (1 default: locality-aware batch tier) and `RNS_BATCH_LOCAL_MIN` (16: fewer products use the striped path), `MEM_PIN` (0 default: pin OpenMP threads to their node), `ECALC_STOP_AFTER_BS` (exit after bs). Multi-node (WP5/6): `comm.h` rank abstraction, `comm_sim4` (four synthetic ranks), `comm_tcp` (`COMM_RANK/SIZE/HOSTS/PORT`, `wp6run.sh`), `ntt_dist` (distributed four-step, `tests/t_dist`).
Results: RESULTS.md §38 (steps 0–4), §39 (steps 5–8, end-to-end runs).
