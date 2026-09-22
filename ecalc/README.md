# ecalc — e to 4 × 10¹⁰ digits on one MI300A node, and over several (PLAN.md §8, §15, §17, §25)

The default configuration at size 1 (one process, four APUs) is the Phase 11 result (RESULTS.md §76):
decimal limbs of 10¹⁸, the binary-splitting levels and the Newton division on device-resident numbers
through the four-APU distributed transform, 3·2ᵏ transform lengths, the paired batch tier and the
register-blocked transform body, the seeds and T1's recurrence overlapped with init and bs, the digits
streamed to the file in chunks — **4 × 10¹⁰ digits in 81.5 ± 1.4 s wall at 11.7 GB of host memory, 10¹¹
digits on one node in 263 s (445 GB)**, digits verified against the reference (Phase 9: 86.4 s / 48.8 GB;
Phase 7's 134 s / 154 GB and Phase 8's 112 s are in RESULTS §63–72). `LIMB_BASE=2` reproduces the
paper's binary-limb pipeline; `main` carries this code, the Phase 4 reproduction is tag `phase4-accepted`.
Several node-processes run the same program over the TCP or SHMEM communicator (`mnrun.sh`, below): the
leaf tree per node, the top levels, the division and the output distributed. Every environment switch
the code reads is listed once, with its default, in **Switches** at the end.

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

(The switches these files read — `LIMB_BASE`, `POOL_LOG`, `ECALC_OVERLAP`, `NEWTON_DEVICE`, `BS_DEVICE_POOLS`, the tuning
and debugging knobs — are all in **Switches** below, one line each with the default. Multi-node pieces (WP5/6):
`comm.h` rank abstraction, `comm_sim4` (four synthetic ranks), `comm_xgmi` (the four real APUs), `comm_tcp` /
`comm_shmem` (`COMM_*`), `ntt_dist` (distributed four-step, `tests/t_dist`), `dbig` (device bigint, `tests/t_dbig`),
`rns_dist` (the distributed product tier; products over one 2³¹-point plane run as a cost-minimising grid of piece
products).)
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

`COMM_RANK`, `COMM_SIZE`, `COMM_HOSTS`, `COMM_PORT` and `COMM_TRANSPORT` (set by `mnrun.sh` from Slurm), the
per-run knobs (`POOL_LOG` — with several node-processes on one node use 27–29 so their pools fit: 10⁸ at 27, 10⁹ at
29, 10¹⁰ at size 4 at 29 — `MN_GROUPS`, `MN_TOPO_GROUP`, `MN_OUT_CHUNK_MB`, the division's `NEWTON_MN_*`) and the
stand-ins `MN_COMBINE=host` / `MN_DM=host` are in **Switches** below.

**Verification (Phase 11 V, results/V.md).** The T1 moduli are the first eight primes above 2⁶² (until Phase 10
seven of the eight were composite; two of them divided every Q, which blinded T1 to Q and X there). Switches:
`ECALC_RES_LOG=1` prints every residue the checks use and cross-checks each (the kernel against a host Horner,
the background recurrence against the main thread, each node's leaf P_r, Q_r against the recurrence over its
terms, from level `ECALC_RES_LOG_LEVEL` (17) on every node of every leaf level — by the kernel, or with
`ECALC_RES_LOG_CPU=1` by the CPU reading the regions, no stream synchronisation — the leaf hand-over copy, each
share before the cross-node reduction); `ECALC_LEAF_DUMP=<dir>` writes the leaf P_r, Q_r as raw limbs and, with the
level check, the first wrong node of a level with its four children (`bs_n<rank>_l<level>_i<node>_{P,Q,P1,Q1,P2,Q2}.bin`);
`MEM_DPOOL_FILL=1|2` fills a grown (every) plane pool with 0xA5 (a test of zero-memory assumptions).
`ECALC_RECHECK=1 ./ecalc <digits> <outfile>` (through `mnrun.sh <size>` at size > 1) is the standalone recheck of a
finished run, without pools or computation: the run writes `<outfile>.t1` (node 0: the residues it checked with and
the computed digits after d_out) and, **by default above 10¹⁰ digits, the top-level P and Q** into `<outfile>.top/`
(`ECALC_CKPT_TOP`, Phase 12 W: at size 1 the level-0 tree set `tree_000.*`, at size > 1 the top tree set
`n<rank>_tree_LLL.*`; `BS_CKPT_DIR`, when set, is used instead — `ECALC_CKPT_TOP=0` turns the write off, `=1` turns it
on at any size). In the device flow the set is written by a background thread — P's quarters while the reciprocal
runs, Q's while the division runs (the driver waits for P's parts before S = P + Q overwrites P, and for Q's before
Q's block is released; the wait is printed) — so at 4 × 10¹⁰ the 35.6 GB cost little wall (results/W.md). The recheck
reads the digit file(s) in chunks (the residues by Horner, the T2 windows), forms X mod q, takes P and Q mod q from
the top-level set (`BS_CKPT_DIR`, else `<outfile>.top`; without either from the sidecar, and says so), recomputes the
term recurrence, and runs T1 with the run's R residues — RECHECK OK / FAILED per node and `mn: all n nodes: …`. It
reads the part files where they were written (node-local disks mean the same nodes). Delete `<outfile>.top` when the
recheck is done (2 × 17.8 GB at 4 × 10¹⁰). `mnaccept.sh`'s `recheck` step exercises it at 10⁹ size 1 and 10⁸ size 2
(RECHECK OK, and RECHECK FAILED on a copy with one digit flipped); `--full` rechecks the 4 × 10¹⁰ run from its files.
Deleted in Phase 11 (E1): `DIST_PLANE2` (`dist_fwd2/inv2`), `BS_SEED_DIRECT=0`, `ECALC_OVERLAP_COPY`.

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
    ./mnaccept.sh <jobid> [--full] [--only unit,e9,mn,ckpt,recheck,full]

Runs on the allocation, in order: the unit tests (`t_ntt 24`, `t_mul 20`, `t_bs`, `t_dbig 0`,
`t_newton 20`, `t_verify`, `t_out`, `t_mn_grid 1 28` at 2 node-processes); 10⁹ at size 1 in both
limb bases against `ref/e_1000000000.txt`; 10⁸ at sizes 2, 3, 4 and 10⁹ at sizes 2, 4 on one node
(the part files concatenated and compared); a checkpoint + restart at 10⁸ size 2 (`BS_CKPT_ABORT=6`
on every node, then `BS_RESTART=1`); the recheck mode at 10⁹ size 1 and 10⁸ size 2 (`ECALC_CKPT_TOP=1`, then
`ECALC_RECHECK=1` on the files: RECHECK OK with P, Q from the checkpoint on every node, RECHECK FAILED on a
copy with one digit flipped); with `--full` one 4 × 10¹⁰ at size 1 with the reference evicted from the page
cache, its wall printed and its digits compared against `results/e_4e10.out`, then its recheck from its files. One
`PASS`/`FAIL` line per step, a summary line at the end, exit status = the number of failures; the
logs in `results/mnaccept/<jobid>/`. References: the clone's `ref/`, else `ECALC_REF`
(`~/ntt/ecalc/ref`) and `ECALC_REF_4E10` (`~/ntt/ecalc/results/e_4e10.out`). About 25 minutes
(`t_ntt` alone 10), 30 with `--full`. `accept.sh` is the Phase 4 sweep (every unit test, 10⁶–4 × 10¹⁰ at
size 1), `variance.sh N` the N-run 4 × 10¹⁰ series with amd-smi sampling.

## Switches

Every environment variable the code reads (`grep -ho 'getenv("[A-Z0-9_]*")\|env_int("[A-Z0-9_]*"' *.c *.h`), one line
each, the default in parentheses; `on/off` switches take 1/0. Those marked *debug* or *test* change no result. The digits
never depend on any of them except `LIMB_BASE` (the same digits by another pipeline) — everything else is bit-identical by
construction and checked so by the regression. Switches marked *Phase 12* were added by the other agents of that session
(their `results/<agent>.md` has the measurements); the integrator's merge is the final word on those lines.

**The run (`ECALC_`)**

| switch | meaning (default) |
|---|---|
| `ECALC_VERBOSE` | 1: the phase lines; 2: per-level, init and memory detail; 0: quiet (1) |
| `ECALC_OVERLAP` | the Phase 8 overlapped flow — parallel per-APU init, the seeds during init's allocations, T1's recurrence during bs, the division on the device, digits/T2 during the low product; 0 = the sequential flow (1) |
| `ECALC_BG_THREADS` | the background OpenMP team of the recurrence and the writer (48) |
| `ECALC_STAGING` | the pinned staging per APU in the decimal device flow: 1 = the checkpoints' 1 GiB chunk, 2 = sized to the seeds (pre-B2), 0 = the paper's 8·2^POOL_LOG bytes (1) |
| `ECALC_TAIL` | the arena's tail for the dm phase mapped at init (Phase 11 M); 0 = the Phase 10 layout, the block pool growing by hipMalloc inside the phase (1) |
| `ECALC_ARENA_GB` | the region arena per device, in GB, instead of the layout's own sizing (auto) |
| `ECALC_DM_POOL` | *deleted in Phase 12 (agent I)*: C3's block-pool pre-growth is a no-op with `ECALC_TAIL` (M11's reserved tail leaves nothing to pre-grow) |
| `ECALC_SEED_ORDER` | *Phase 12 (I)*: `overlap` (the seeds alongside the plane pools' mapping) / `first` (regions, seeds, then the planes) / `after` (the seeds synchronous in bs) (overlap) |
| `ECALC_COPY_PROBE` | *Phase 12 (R), debug*: times the odd-node copy at a level transition against the next level (0) |
| `ECALC_B_SNAPSHOT` | *Phase 12 (R), debug*: in the striped grpB tier every device snapshots the shared operand B right before reading it; the snapshots are compared after the level (0) |
| `ECALC_DM_POOL_K` | the arena sized for the dm phase at init as k × the digit limbs per device, `binsplit_pregrow` (0 = off) |
| `ECALC_POOL_GROW_GB` | GB of plane pool grown per device in the background thread during bs; off: hipMalloc there stalls the GPU levels (0) |
| `ECALC_STOP_AFTER_BS` | exit after bs with its line (unset) |
| `ECALC_CKPT_TOP` | the top-level P, Q on disk for the recheck (above): `BS_CKPT_DIR` or `<outfile>.top` (**off by default**; `=1` writes it at any size). Hidden under the reciprocal and the division only where the disk writes at ≳ 1 GB/s: at 0.31 GB/s (aac6's slower path) the 35.6 GB set costs 113 s that the division waits for |
| `ECALC_RECHECK` | 1: the standalone recheck of a finished run's files instead of a run (0) |
| `ECALC_WINDOWS` | a file of extra T2 windows (`<offset> <digits>` per line) added to the built-in table (unset) |
| `ECALC_RES_LOG` | *debug*: every residue the checks use printed and cross-checked — the kernel against a host Horner, the recurrence against the main thread, each node's leaf against the recurrence (0) |
| `ECALC_RES_LOG_LEVEL` | *debug*: with `ECALC_RES_LOG`, every node of every leaf level from this level on checked against the recurrence (17) |
| `ECALC_RES_LOG_CPU` | *debug*: the per-level check reads the regions with the CPU — no kernel, no stream synchronisation (0) |
| `ECALC_LEAF_DUMP` | *debug*: a directory for the leaf P_r, Q_r as raw limbs and, with the level check, the first wrong node with its children (unset) |

**Limbs and pools (`LIMB_BASE`, `POOL_LOG`, `BI_`, `MEM_`)**

| switch | meaning (default) |
|---|---|
| `LIMB_BASE` | 10 = decimal limbs of 10¹⁸ (no 10dP, no radix conversion); 2 = the paper's 64-bit limbs with todec (10 in `ecalc`; the tests default to 2) |
| `POOL_LOG` | the plane pools' 2ᵏ points per APU (31; 27–29 with several node-processes on one node) |
| `BI_HUGE` | transparent huge pages for the host bigints; measured harmful on this node (THP defrag stalls) (0) |
| `MEM_PIN` | pin the OpenMP threads to their NUMA node; costs the decimal seeds 30 % (0) |
| `MEM_REPORT_DEVS` | `mem_report` adds per-APU rows (driver used/total) to the per-phase memory table (unset) |
| `MEM_NO_DEV_MEMSET` | *test*: skip the hipMemset that maps a device pool's pages at allocation (unset) |
| `MEM_DPOOL_FILL` | *test*: fill a grown (1) or every (2) plane pool with 0xA5 — a test of zero-memory assumptions (0) |
| `MEM_ALLOC` | *Phase 12 (I)*: the form of every large device allocation — `hipmalloc` / `fine` / `uncached` / `managed` / `host` / `mmap` (hipmalloc) |
| `MEM_COPY_NOWAIT` | *Phase 12 (R), test*: the pre-fix `mem_dev_copy_on` (no wait on the caller's stream) for the witness runs (0) |

**The transform and the product tiers (`NTT_`, `PW_FUSE`, `RNS_`)**

| switch | meaning (default) |
|---|---|
| `NTT_B16_STG` | the tiled kernel's stage split (7) |
| `NTT_B16_BODY` | the register-blocked transform body (Phase 9 N-kernel, bit-identical) (1) |
| `NTT_B16_XCHG` | the register-blocked body's last exchange by `ds_swizzle` instead of LDS (Phase 9 B4) (0) |
| `PW_FUSE` | the stage from which the pointwise product is fused into the transform (14) |
| `RNS_ENGINE` | 1 = the paper's four 52-bit FP64 primes; 2 = two 62-bit primes / 45-bit points (1) |
| `RNS_R3` | 3·2ᵏ transform lengths (WP8) (1 when the prime set has the radix-3 roots) |
| `RNS_CRT_LAYOUT` | 0 = one plane per node (the paper's); 1 = quartered node-local planes (no gain) (0) |
| `RNS_REPACK_WIDE` | one full-width copy per device in the repack (slower) (0) |
| `RNS_BATCH_LOCAL` | the locality-aware batch tier (1) |
| `RNS_BATCH_LOCAL_MIN` | below this many products the striped batch path is used (16) |
| `RNS_BATCH_PAIR` | products 2i, 2i+1 sharing B (the tree's P₁Q₂ + P₂, Q₁Q₂) transform B once (Phase 9 B1) (1) |
| `RNS_STRIPED_PAIR` | the paired, 3·2ᵏ form of the striped batch path as a unit (A2) (1) |
| `RNS_BATCH_TILE_GB` | the batch tiers' plane budget per device in GB (a + b planes), capped by the pools (15) |
| `RNS_POOL1_GB` | plane pool 1 per device in GB (auto: the dist tier's 3q + 16 limbs) |
| `RNS_POOL_GROW` | *Phase 12 (R)*: a region pool that would grow inside bs aborts with the accounting unless 1 (the stress recipe sets it) (0) |
| `RNS_PLANES_3Q30` | 3·2³⁰-point planes for the top levels and the dm phase, sized at init: 1 / 0 / `auto` (on below 5 × 10¹⁰ at 2³¹ pools); the mapping costs more than the products gain (0) |
| `RNS_DIST_CACHE` | transform-cache slots of the single-node dist tier (0: the planes' mapping costs more than the transforms saved) |
| `RNS_DIST_CACHE_MN` | transform-cache slots over shares at size > 1 (2) |
| `RNS_DIST_CACHE_HOLD` | keep a cached operand pinned across products (0) |
| `RNS_DIST_CACHE_MARGIN_GB` | free device memory kept when sizing the cache (24) |
| `RNS_VERBOSE` | *debug*: 1 per-call times, 2 CRT re-runs; also the pool allocations' times (unset) |
| `DB_POOL_VERBOSE` | *debug*: the dbig block pool's growth and how the reserved tails were used (follows `RNS_VERBOSE`) |
| `DBIG_SERIAL` | *debug*: drive the four quarters from one thread (0) |
| `DBIG_WARM` | *test*: touch every 2 MiB page of each quarter from every other device at allocation (unset) |

**Binary splitting (`BS_`)**

| switch | meaning (default) |
|---|---|
| `BS_SEED_TERMS` | the seed span in terms (256) |
| `BS_SEED_THREADS` | threads of the seed pass; fewer than all leaves cores to init's allocations (all) |
| `BS_SEED_CHUNK_MB` | the seed stream's buffered chunk per region (2048) |
| `BS_SCHOOL_NL` | CPU schoolbook tier below this many limbs; 0 = never (0) |
| `BS_DEVICE_POOLS` | the level pools as four device regions (WP3); 0 = registered host (1) |
| `BS_DEV_MDEV` | the top levels as device numbers through the distributed tier (§64) (1) |
| `BS_MDEV_LOGL` | products above 2^this limbs go to the mdev tier; lower it to test at 10⁹ (30) |
| `BS_BALANCE_N` | levels with at most this many nodes are balanced over the four regions; 0 = never (16) |
| `BS_REGION_FLAT` | the flat paper-era region sizing instead of the simulated layout (C4) (0) |
| `BS_REGION_SLACK` | the flat sizing's slack, 4 or 16 (4) |
| `BS_CKPT_DIR` | the checkpoint directory: leaf sets of the level loop, the tree sets at size > 1, the top-level set (unset = none) |
| `BS_CKPT_EVERY` | a leaf set every this many levels (4) |
| `BS_CKPT_MIN_LEVEL` | leaf sets from this level on, or once a level's pool exceeds 64 GiB (16) |
| `BS_CKPT_TREE` | tree sets at size > 1 with `BS_CKPT_DIR`; 0 = none (1) |
| `BS_CKPT_TREE_EVERY` | a tree set every this many tree levels, the top always (1; 64 when `ECALC_CKPT_TOP` supplies the directory) |
| `BS_RESTART` | resume from the latest complete set in `BS_CKPT_DIR` (0) |
| `BS_CKPT_ABORT`, `BS_CKPT_ABORT_TREE`, `BS_CKPT_ABORT_NODE` | *test*: exit(3) right after the leaf set of that level / the tree set of that level, on every node or the one named (unset) |

**The division (`NEWTON_`)**

| switch | meaning (default) |
|---|---|
| `NEWTON_DEVICE` | the reciprocal and division on device-resident numbers; the bs regions donated to the block pool, the staging released (1) |
| `NEWTON_ANCHOR` | the doubling sequence anchored at the target precision; 0 = powers of two (1) |
| `NEWTON_LOWPROD` | the low product X Q as the grid with the pieces above the window skipped; 0 = the full product truncated (1) |
| `NEWTON_MN_SPLIT` | at size > 1 the reciprocal starts single-node on the top limbs of Q up to this many limbs (65536) |
| `NEWTON_MN_GROUPS` | at size > 1 the reciprocal's early doublings on the smallest subgroup the model prefers (X1); 0 = the full group (1) |
| `NEWTON_MN_BW`, `NEWTON_MN_LAT`, `NEWTON_MN_FIXED`, `NEWTON_MN_SHARE` | that model's constants: GB/s per APU, seconds per message, seconds per exchange, the APU sharing (100, 2 µs, 0, auto) |
| `MN_MODEL_TCP` | the model's aac6 loopback numbers instead of the target fabric's (0) |
| `NEWTON_VERBOSE` | *debug*: the per-iteration trace with RSS (0) |

**Node-processes (`COMM_`, `MN_`)**

| switch | meaning (default) |
|---|---|
| `COMM_RANK`, `COMM_SIZE` | this process's rank and the number of node-processes (set by `mnrun.sh`; unset or 1 = the single-node program) |
| `COMM_HOSTS` | the host of every rank, comma-separated in rank order (set by `mnrun.sh`) |
| `COMM_PORT` | the TCP port base (`mnrun.sh` picks a random one per run) |
| `COMM_TRANSPORT` | `tcp` or `shmem` (Phase 11 S: the meshes as strided PE sets over SHMEM) (tcp) |
| `COMM_SHMEM_POOL_MB` | the SHMEM transport's symmetric pool (8192) |
| `COMM_SHMEM_SERIAL` | every SHMEM call under one process-wide lock; 0 = per-thread contexts on a `SHMEM_THREAD_MULTIPLE` library (1) |
| `COMM_SHMEM_DEVHEAP` | the symmetric heap in device memory: 1 on an implementation whose `shmem_malloc` is device memory or SOS with the external-heap patch, 2 managed (0; *Phase 12 (S)*) |
| `COMM_SHMEM_FENCE` | Phase 11's form of `COMM_SHMEM_ORDER=fence` (0) |
| `COMM_SHMEM_ORDER` | *Phase 12 (S)*: how the signal is ordered after the data — `putsig` (`shmem_putmem_signal_nbi`, 1.5), `fence` (1.4, the spec), `quiet` (OSHMEM 4.1) (putsig on 1.5 headers, else quiet) |
| `COMM_SHMEM_KEEP_STAGING` | *Phase 12 (S)*: keep the staging copies across exchanges instead of releasing them per exchange (0) |
| `COMM_SHMEM_THREAD` | *Phase 12 (S)*: always post the puts from the helper thread (0) |
| `COMM_SHMEM_SPIN_US` | *Phase 12 (S)*: how long the sender spins for a peer's receive offset before handing the put to the helper thread (2000) |
| `COMM_SHMEM_NOSYM` | *Phase 12 (S), test*: `comm_sym_alloc` returns nothing — every buffer is staged (0) |
| `DIST_SYM_SLABS` | *Phase 12 (S)*: `ntt_dist`'s slab buffers allocated from the symmetric pool (put straight from and into them); 0 = hipMalloc'd and staged (1) |
| `COMM_SHMEM_RING_KB` | the point-to-point ring per (dest, source) (256) |
| `COMM_SHMEM_TRACE` | *debug*: trace the SHMEM transport (unset) |
| `COMM_PUSH64` | the xGMI push by 64-bit stores; 0 = 16-byte vectors (1) |
| `COMM_PUSH_BLOCKS` | blocks per peer of the xGMI push (76) |
| `MN_GROUPS` | the tree's level → group-size schedule, e.g. `2,4,8,16,32,64,576` (on `w12`: the powers of two up to the size, then the size; *Phase 12 (G, Q's decision)*: the powers of two dividing the size, then the odd part's prime factors ascending — 576 → …, 64, 192, 576) |
| `MN_TOPO_GROUP` | nodes per dragonfly group: the exchanges of a transform group layered intra/inter group (0 = the plain mesh) |
| `MN_OUT_CHUNK_MB` | the streamed writer's digit chunk per node (256) |
| `MN_COMBINE=host` | *stand-in*: M2's combine — the leaf results sent to node 0 and multiplied on its host mdev tier — instead of the distributed tree |
| `MN_DM=host` | *stand-in*: the division on node 0 alone (the sharded P, Q gathered to its host) instead of the distributed one; the cross-check of the sharded division (kept while DECISIONS2 #1 is open) |
| `MN_TOPO_TRACE`, `MN_LAYERED_RAW`, `MN_LAYERED_LOCAL`, `MN_DEBUG` | *debug*: the topology layer's trace; the layered communicator's self-test over the raw xGMI mesh / a size-1 inter communicator; where a wrong self-test value came from (unset) |

**The distributed transform (`DIST_`)**

| switch | meaning (default) |
|---|---|
| `DIST_CHUNKS` | slabs in flight in the pipelined distributed transform, 1–16 (4) |
| `DIST_STATS` | per-part timing of the distributed transform, the exposed exchange per part (unset) |
| `DIST_PW_FUSE` | the pointwise product fused into the column inverse's first pass (bit-identical) (1) |
| `DIST_LOGR_DELTA` | the four-step split logR = logn/2 + this, −3…3 (0) |
| `DIST_R3` | 3·2ᵏ lengths in the distributed tier (follows `RNS_PLANES_3Q30`) |
| `DIST_LOGN_TEST` | *test*: a lower plane cap so the grid split runs at small sizes (31) |
| `DIST_GEN` | *test*: the general (any-g) transform at a power-of-two group size too (0) |

The tests alone read `DIST_XGMI`, `DIST_LAYERED`, `DIST_BIG`, `DIST_TINV` (`t_dist`'s modes), `T_COMM_TRACE` (`t_comm`) and
*Phase 12 (I)* `T_ALLOC_NTT`, `T_ALLOC_PROBE`, `T_ALLOC_SEED` (`t_alloc`). The SHMEM transport also reads the launcher's
`SLURM_NTASKS` / `PMI_SIZE` (a PE-count hint) and the Makefile `SHMEM_HOME` (Phase 12 S: the SOS build). `mnaccept.sh`
reads `ECALC_REF` and `ECALC_REF_4E10` (the reference files).
