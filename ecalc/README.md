# ecalc — e to 4 × 10¹⁰ digits on one MI300A node (PLAN.md §8, §15)

The default configuration is the Phase 7 result (RESULTS.md §63, §66–67): decimal limbs of 10¹⁸, the
binary-splitting levels and the Newton division on device-resident numbers through the four-APU
distributed transform, 3·2ᵏ transform lengths — 4 × 10¹⁰ digits in 109 s of phases / 134 s wall at
154 GB of host memory, digits verified (RESULTS §68: 112 s wall with the overlapped flow, the default). `LIMB_BASE=2` reproduces the paper's binary-limb pipeline
(167 s / 192 s / 233 GB); `main` carries this code, the Phase 4 reproduction is tag `phase4-accepted`.

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
| `binsplit.h/.c` | e = Σ 1/k!: seed spans, level loop with batch / mdev tiers; level pools as four device regions with subtree ownership (WP3); the top levels as device numbers through the distributed tier (§64); checkpoint/restart (WP7) |
| `todec.h/.c` | radix conversion (binary limbs only): divisor cache, TOP/MID/DEEP levels, GPU LEAF kernel by 10¹⁸ |
| `verify.h/.c` | tier-1 residues mod eight 62-bit primes (incl. the digit string), tier-2 windows |
| `dbig.h/.c` | device bigint: four quarters, one per APU; chunk-flag carries, views, block pool fed by the bs regions (WP5) |
| `rns_dist.h/.c` | the distributed product tier (four-step over the four APUs, grid split above one 2³¹-point plane) |
| `newton_db.c` | the reciprocal and division on device numbers |
| `ntt_dist.h/.c`, `comm*.c` | the distributed four-step transform and the rank communicators (sim4, xGMI, TCP) |
| `ntt3.c` | the radix-3 layer for 3·2ᵏ lengths (WP8) |
| `ecalc.c` | driver: `./run ecalc <digits> [outfile]` (env POOL_LOG, NTT_B16_STG, PW_FUSE, RNS_CRT_LAYOUT, ECALC_VERBOSE=2 for per-level lines) |
| `tests/` | one GMP-checked program per module; `harness.h` (VERIFY, generators, GMP bridges, META/RESULT) |
| `ref/` | `gen_e.c` and the reference digit files |

Env knobs: `NTT_B16_STG` (via `ntt_stg`), `PW_FUSE` (`ntt_pw_fuse`), `RNS_CRT_LAYOUT` (0 plane per node, 1 quartered), `RNS_VERBOSE` (1 per-call times, 2 CRT re-runs).
Phase 7 switches: `LIMB_BASE` (10 default = base-10¹⁸ limbs, no 10dP and no radix conversion, WP1; 2 = the paper's 64-bit limbs), `NEWTON_ANCHOR` (1 default: doubling sequence anchored at the target precision; 0 = Phase 4 powers of two), `NEWTON_VERBOSE` (per-iteration trace with RSS), `BS_DEVICE_POOLS` (1 default: level pools as four device regions, WP3; 0 = registered host), `RNS_BATCH_LOCAL` (1 default: locality-aware batch tier) and `RNS_BATCH_LOCAL_MIN` (16: fewer products use the striped path), `MEM_PIN` (0 default: pin OpenMP threads to their node), `ECALC_STOP_AFTER_BS` (exit after bs), `BS_SEED_TERMS` (256), `BS_SCHOOL_NL` (0), `RNS_R3` (1: 3·2ᵏ transform lengths), `NEWTON_DEVICE` (1: the reciprocal and division on device-resident numbers — `dbig.c`, `rns_dist.c`, `newton_db.c`; the bs regions are donated to the dbig block allocator and, in decimal, the pinned staging is released for dm), `DIST_STATS` (1: per-part timing of the distributed transform), `ECALC_OVERLAP` (1 default: Phase 8 overlap, PLAN §18 and RESULTS §68–70 — parallel per-APU init, the seeds during init's allocations, T1's term recurrence during bs, the decimal division entirely on the device (P, Q, S, R never on the host; residues by kernel), digits/T2 during the low product; 0 = the sequential flow; `ECALC_BG_THREADS` = the background OpenMP team, 48; `BS_SEED_THREADS`), `NEWTON_LOWPROD` (1 default: the low product X Q as the grid with the pieces above the window skipped; 0 = the full product truncated). Multi-node (WP5/6): `comm.h` rank abstraction, `comm_sim4` (four synthetic ranks), `comm_xgmi` (the four real APUs), `comm_tcp` (`COMM_RANK/SIZE/HOSTS/PORT`, `wp6run.sh`), `ntt_dist` (distributed four-step, `tests/t_dist` with `DIST_XGMI=1` / `COMM_RANK` modes), `dbig` (device bigint, `tests/t_dbig`), `rns_dist` (the distributed product tier, `tests/t_mul ... dist`; products over one 2³¹-point plane run as a cost-minimising grid of piece products, `DIST_LOGN_TEST` lowers the cap for `tests/t_dbig 0 big`).
Checkpoint/restart of bs (WP7, `results/WP7.md`): `BS_CKPT_DIR=<dir>` writes a snapshot of the
level loop at the end of a level — the node table (`level_LLL.hdr`) and the used limbs of the
four region pools (`level_LLL.r0..r3`; the mdev levels' host pool is saved the same way) — every
`BS_CKPT_EVERY` levels (default 4), from level `BS_CKPT_MIN_LEVEL` (default 16: the top levels, where the time is — a snapshot is a full pass of the pools, ≈ 5.7 s per 8 GB) on or as soon as a
level's pool exceeds 64 GiB; only the latest level is kept (files written to `.tmp` names and
renamed, header last, the previous set removed after). `BS_RESTART=1` with the same `<dir>`,
digits and base resumes from the latest complete set (seeds and the levels below it are
skipped; the digits are bit-identical to an uninterrupted run); a set from another run (N, base
or `BS_SEED_TERMS` differ) aborts. Snapshots are periodic, not a streamed working set; a 10⁹
checkpoint is ≈ 1 GB, a 10¹⁰ one ≈ 10 GB, so point `<dir>` at local disk with room.
Results: RESULTS.md §38 (steps 0–4), §39 (steps 5–8, end-to-end runs).
