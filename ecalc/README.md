# ecalc — e to 4 × 10¹⁰ digits on one MI300A node, and over several (PLAN.md §8, §15, §17)

The default configuration at size 1 (one process, four APUs) is the Phase 9 result (RESULTS.md §74):
decimal limbs of 10¹⁸, the binary-splitting levels and the Newton division on device-resident numbers
through the four-APU distributed transform, 3·2ᵏ transform lengths, the paired batch tier and the
register-blocked transform body, the seeds and T1's recurrence overlapped with init and bs, the digits
streamed to the file in chunks — **4 × 10¹⁰ digits in 86.4 ± 1.3 s wall / 70.3 s of phases at 48.8 GB
of host memory** (init 16, bs 37, dm 33; 7 × 10¹⁰ in 164 s / 76.5 GB), digits verified against the
reference. Phase 7's 134 s / 154 GB and Phase 8's 112 s are in RESULTS §63–72. `LIMB_BASE=2` reproduces the
paper's binary-limb pipeline (167 s / 192 s / 233 GB); `main` carries this code, the Phase 4 reproduction
is tag `phase4-accepted`. Several node-processes run the same program over the TCP communicator
(`mnrun.sh`, below): the leaf tree per node, the top levels, the division and the output distributed.

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
| `ntt_dist.h/.c`, `comm*.c` | the distributed four-step transform and the rank communicators (local, sim4, xGMI, TCP, layered = xGMI inside a node × TCP between nodes) |
| `mn.h/.c`, `mdb.h` | the node-processes: meshes and groups per tree level, the top levels of the tree as distributed products over node groups (M3), the tree-level checkpoints (M6); `mdb` = a number sharded over the nodes of a group |
| `mn_out.h/.c` | the output stage per node: X's digits formatted and written in chunks, T1's recurrence over the node's terms, the residues and T2 windows joined over the nodes (M5) |
| `mem.c` (`mem_report`) | the memory accounting per phase and node (M9) |
| `mnrun.sh`, `mnaccept.sh`, `accept.sh`, `variance.sh` | the multi-process launcher, the standing regression, the Phase 4 acceptance sweep, the 4 × 10¹⁰ variance series |
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
Results: RESULTS.md §38 (steps 0–4), §39 (steps 5–8, end-to-end runs); the multi-node sets below.

## The multi-node run (Phase 8–10: PLAN.md §17, results/M3.md, results/A-*.md)

`size` node-processes (one per node on the target machine; several per node on aac6, sharing its
four APUs) compute e together: each node's leaf tree over its own range of terms (`[bs_a0, bs_b1)`,
N/size terms), then the top log₂ size levels of the tree as distributed products over node groups
(node groups pair at every level; P and Q end up sharded over all nodes as `mdb` numbers), then the
reciprocal and the division over the whole machine on the sharded numbers, then every node formats
and writes its part of the digits and checks its residues. Every node prints its own `T1`/`T2`/`VERIFY`
lines prefixed `mn: node r:` and node 0 prints `mn: all n nodes: VERIFY OK` from an all-reduce.

    SLURM_JOB_ID=<id> ./mnrun.sh <procs> [env VAR=... ] ./ecalc <digits> [outfile]

`mnrun.sh` spreads `<procs>` processes over the nodes of the allocation (the largest node count that
divides `<procs>`, several per node otherwise) with `srun`, and sets the communicator's environment:

| variable | meaning |
|---|---|
| `COMM_RANK`, `COMM_SIZE` | this process's rank and the number of node-processes (set by `mnrun.sh` from Slurm; `COMM_SIZE` unset or 1 = the single-node program) |
| `COMM_HOSTS` | the comma-separated host of every rank in rank order (the placement must match Slurm's) |
| `COMM_PORT` | the TCP port base (`mnrun.sh` picks a random one per run so a straggler of a killed run cannot catch the next run's connections; M3 uses base … base + 6656) |
| `MN_COMBINE=host` | M2's combine instead of M3's distributed tree: the leaf results are sent to node 0 and multiplied on the host mdev tier (keeps the paper's staging and pools) |
| `MN_DM=host` | the division on node 0 alone (the sharded P, Q gathered to its host) instead of the distributed reciprocal and division |
| `MN_OUT_CHUNK_MB` | the digit chunk of the streamed writer per node (256) |
| `MN_LAYERED_RAW`, `MN_LAYERED_LOCAL`, `MN_DEBUG` | debugging of the layered communicator's self-test |
| `DIST_CHUNKS` | slabs in flight in the pipelined distributed transform (4); `DIST_STATS=1` prints the exposed exchange per part |
| `MEM_REPORT_DEVS=1` | `mem_report` adds per-APU rows (driver used/total) to the per-phase memory table every node prints (`mem[rank]` at size > 1) |
| `POOL_LOG` | the plane pools' 2ᵏ points per APU (31): with several node-processes on one node use 27–29 so their pools and arenas fit the node (10⁸ at 27, 10⁹ at 29, 10¹⁰ at size 4 at 29) |
| `NEWTON_MN_SPLIT`, `BS_MDEV_LOGL`, `BS_DEV_MDEV`, `BS_BALANCE_N`, `ECALC_ARENA_GB`, `ECALC_DM_POOL`, `ECALC_DM_POOL_K` | tuning of the distributed division's split, the leaf's device-number levels, the small levels' layout, the region arenas and the dm block pool (results/A-div.md, A-mem.md) |

**Verification (Phase 11 V, results/V.md).** The T1 moduli are the first eight primes above 2⁶² (until Phase 10
seven of the eight were composite; two of them divided every Q, which blinded T1 to Q and X there). Switches:
`ECALC_RES_LOG=1` prints every residue the checks use and cross-checks each (the kernel against a host Horner,
the background recurrence against the main thread, each node's leaf P_r, Q_r against the recurrence over its
terms, from level `ECALC_RES_LOG_LEVEL` (17) on every node of every leaf level, the leaf hand-over copy, each
share before the cross-node reduction); `ECALC_LEAF_DUMP=<dir>` writes the leaf P_r, Q_r as raw limbs;
`MEM_DPOOL_FILL=1|2` fills a grown (every) plane pool with 0xA5 (a test of zero-memory assumptions).
`ECALC_RECHECK=1 ./ecalc <digits> <outfile>` (through `mnrun.sh <size>` at size > 1, the same `BS_CKPT_DIR`) is
the standalone recheck of a finished run: the run writes `<outfile>.t1` (node 0: the residues it checked with and
the computed digits after d_out); the recheck recomputes the digit residues from the file (part files), X mod q
from them, P and Q mod q from the checkpointed top-level shares (the tree set at size > 1; at size 1 the level-0
tree set written by a run with `ECALC_CKPT_TOP=1`), the term recurrence and the T2 windows, and runs T1 with the
run's R residues — RECHECK OK / FAILED per node. Deleted in Phase 11 (E1): `DIST_PLANE2` (`dist_fwd2/inv2`),
`BS_SEED_DIRECT=0`, `ECALC_OVERLAP_COPY`.

**Part files.** With `<outfile>` at size > 1 every node writes `<outfile>.part<k>`, k = size − 1 − rank
zero-padded to four digits, in file order (part 0000 holds "2." and the leading digits, the last part
the digits down to the requested count and the newline): `cat <outfile>.part*` (the glob sorts) is
byte-identical to the single-file output. At size 1 the file is `<outfile>` itself, streamed the same
way (no 40 GB string). Without `<outfile>` nothing is written; the checks still run.

**Checkpoints per node (M6, results/A-ckpt.md).** With `BS_CKPT_DIR` and several node-processes the
leaf sets are `n<rank>_level_LLL.*` (the WP7 sets per node, header v2 with size, rank and the term
range — a set from a run of another size or rank aborts the restart), written by the leaf loop every
`BS_CKPT_EVERY` levels from `BS_CKPT_MIN_LEVEL` on; the tree levels add `n<rank>_tree_LLL.*` — the
node's shares of P and Q after tree level LLL — every `BS_CKPT_TREE_EVERY` levels (default 1) and
always at the top level (`BS_CKPT_TREE=0`: none). All nodes may share one directory (the names carry
the rank) or each use its local disk. A tree set's predecessors (the lower tree sets, the leaf sets)
are removed only after every node has it: the barrier is taken at the next tree set (after that level's
product, before its write — so it waits for the nodes' compute, not for the slowest write) or, for
the last set, after the run's final barrier. `BS_RESTART=1` on every node: the nodes agree on the
lowest "highest complete tree level" over all nodes (present on every node by the rule above) and
resume above it, or, with no tree set anywhere, every node inside its own leaf tree from its own
latest leaf set (nodes without one recompute their leaf tree) — bit-identical either way. Test hooks:
`BS_CKPT_ABORT=<leaf level>`, `BS_CKPT_ABORT_TREE=<tree level>` exit(3) right after that set is
written, `BS_CKPT_ABORT_NODE=<rank>` on one node only. Sizes: a leaf set ≈ 35 GB / size per node at
4 × 10¹⁰, a tree set the same; ≈ 1 GB/s per node to local NVMe.

## The standing regression (`mnaccept.sh`, PLAN.md §21 D3)

    sbatch -p PPAC_MI300A_SPX -N1 --gpus=4 -t 0:45:00 -J acc --wrap "sleep 2700"    # then, on the login node, from ecalc/:
    ./mnaccept.sh <jobid> [--full] [--only unit,e9,mn,ckpt,full]

Runs on the allocation, in order: the unit tests (`t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`,
`t_newton 20`, `t_verify`, `t_out`, `t_mn_grid 1 28` at 2 node-processes); 10⁹ at size 1 in both
limb bases against `ref/e_1000000000.txt`; 10⁸ at sizes 2, 3, 4 and 10⁹ at sizes 2, 4 on one node
(the part files concatenated and compared); a checkpoint + restart at 10⁸ size 2 (`BS_CKPT_ABORT=6`
on every node, then `BS_RESTART=1`); with `--full` one 4 × 10¹⁰ at size 1 with the reference evicted
from the page cache, its wall printed and its digits compared against `results/e_4e10.out`. One
`PASS`/`FAIL` line per step, a summary line at the end, exit status = the number of failures; the
logs in `results/mnaccept/<jobid>/`. References: the clone's `ref/`, else `ECALC_REF`
(`~/ntt/ecalc/ref`) and `ECALC_REF_4E10` (`~/ntt/ecalc/results/e_4e10.out`). About 25 minutes
(`t_ntt` alone 10), 30 with `--full`. `accept.sh` is the Phase 4 sweep (every unit test, 10⁶–4 × 10¹⁰ at
size 1), `variance.sh N` the N-run 4 × 10¹⁰ series with amd-smi sampling.
