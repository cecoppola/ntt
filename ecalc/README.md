# ecalc — digits of e on MI300A nodes

Scope: the settings reference (the **Switches** table: one row per environment switch the code reads, with its default
and evidence), the notes that belong to single settings, and the file map of `ecalc/`. | Maintained: updated in the same
merge as any change to a switch, its default or its evidence, or to the files of `ecalc/`.

Elsewhere: what ecalc does and the build/run quick start are in `../README.md`; how it computes in `../ALGORITHM.md`;
how to check a change (gate, standing regression, unit tests, timing) in `../TESTING.md`; the defaults, launch lines
and status in `../PLAN.md`; what is in force and its evidence in `../RESULTS.md`; machine profiles in
`../profiles/README.md`.

**Machine notes.**
- aac7: `source aac7env.sh` (ROCm 7.2.4). The QOS allows 6 nodes per user (`sacctmgr show qos normal format=maxtrespu`; the queue runner `tools/aac7q.sh` reads this limit from Slurm every 10 min and runs items side by side while they fit, largest unit first (a GROUP counts as the sum of its members; an item waiting over 45 min reserves the earliest time running items free its nodes, and later items start only if they end by then, by their optional `EXPECT_MIN` header else `EST_MIN`, or fit in the nodes that are still extra at that time); it excludes the hosts listed in the private files `aac7q/exclude` (always) and `aac7q/exclude_slow` (unless the item sets `ALLOW_SLOW=1`); a missing file excludes nothing).

**Notes on single settings.**
- `mnrun.sh` (the multi-node launcher; usage in `../README.md`) sets `COMM_RANK`, `COMM_SIZE` and the transport from
  Slurm, exports `FI_UNIVERSE_SIZE`, `FI_LOG_LEVEL=warn` and `FI_MR_CACHE_MONITOR=userfaultfd` when unset (it refuses
  `disabled`), and `MNRUN_EXCLUDE_HOSTS` drops flagged nodes. `MNRUN_GRES=auto|on|off` (default auto; `gres.sh`, shared by
  `mnrun.sh`, `mnaccept.sh`, `tools/gate.sh`, the kit, ladder, bench items and `remote/`): the `--gpus*` and `--gpu-bind` options are passed
  only if the nodes list a gpu GRES (`scontrol show node`, else `sinfo %G`; cached in `MNRUN_GRES_RESOLVED`), because a site with `Gres=(null)`
  rejects them; aac7 lists gpu:4 and is unchanged; `off` / `on` force. A login `SLURM_HINT=nomultithread` is not passed to srun (it pins each
  task to its first cores and OpenMP then runs 1-2 threads; `MNRUN_KEEP_HINT=1` keeps it). The Makefile finds the PMI library directory (`PMI_LIBDIR`, `CRAY_PMI_PREFIX`)
  and the SHMEM / DSMML prefixes (`CRAY_SHMEM_PREFIX` or `CRAY_SHMEM_DIR` for cray-shmem 12.1; `CRAY_OPENSHMEMX_PREFIX`, `CRAY_DSMML_PREFIX`) from
  the loaded modules, else the aac7 defaults (`../remote/FACTS.md` Known problems). `MN_PLAN_ONLY=<digits>:<nodes>` prints the plan and the
  memory check on the login node (`../ALGORITHM.md` §7, §11).
- Part files, the packed format and `ECALC_OUT_PACKED`: `../ALGORITHM.md` §9. Verification and `ECALC_RECHECK`:
  `../ALGORITHM.md` §10.
- Checkpoints: only the top-level set exists (`ECALC_CKPT_TOP`, for `ECALC_RECHECK`): each node's P and Q after the last tree level (`n<rank>_tree_LLL.*`; size 1: `tree_000.*`) in `BS_CKPT_DIR` or `<outfile>.top`. There is no restart inside a run (the user's decision D11, 2026-10-12; the leaf and tree-level restart sets were removed and archived).

## Files

| file | what |
|---|---|
| `ecalc.c` | the driver: sizing, init, the run in order (`../ALGORITHM.md` §7) |
| `modarith.h` | the primes c · 2⁴⁴ + 1, FP64-Barrett modmul, roots for 2ᵏ and 3 · 2ᵏ lengths |
| `ntt.c/h`, `ntt3.c` | the tiled NTT kernels (DIF forward, DIT inverse, fused pointwise and scale); the radix-3 layer |
| `crt.c/h`, `p24.h`, `p24_crt.c` | CRT reconstruction; 24 digits per point in four-prime products |
| `rns_mul.c/h`, `rns_int.h` | the batch product tiers of binary splitting, the GPU CRT, the plane pools |
| `rns_dist.c`, `ntt_dist.c/h` | the distributed products: forms B and C on a node, the multi-node four-step (`mn_core`), grids, transform cache |
| `bigint.c/h` | host limb arrays: add/sub, schoolbook, shifts |
| `dbig.c/h`, `mdb.h` | device numbers in four quarters, the block pool; numbers sharded over node groups |
| `binsplit.c/h` | binary splitting: seeds, regions, levels, the top levels as device numbers, checkpoints, the layout |
| `newton.c/h`, `newton_db.c` | the reciprocal and the two-half division on device and sharded numbers |
| `mn.c/h`, `mn_plan.c/h` | node-processes: meshes, groups, the tree levels; `MN_PLAN_ONLY` plan and check |
| `comm.h`, `comm_*.c` | communicators: `xgmi` (the node's four APUs), `xshmem` (the inter-node transport over `xshmem.h`), `layered` (xGMI × fabric), `copy` (GPU copy kernel), `util`; `tests/comm_local.c`, `tests/comm_sim4.c` are test-only |
| `xshmem.h`, `xshmem_ofi.c`, `xshmem_boot.c/h`, `xshmem_cray.h`, `xshmem_round.h`, `pe_geo.c/h`, `vpe.c/h` | the xshmem API and its two backends (libfabric `XSHMEM=ofi`, Cray SHMEM `XSHMEM=cray`), the PE geometry, the one-process-per-GPU PE identity |
| `mn_out.c/h`, `packed_fmt.h`, `verify.c/h` | output per node (packed part files, the correction patch), T1 residues and T2 windows |
| `mem.c/h`, `memsample.c/h`, `spill.c/h` | memory pools, VMM arenas, accounting and sampling; O_DIRECT file writes |
| `fatal.c/h`, `segv_trace.c/h`, `config_line.c/h` | named exits, the crash-trace handler, the `config: hash=` line |
| `switches.tsv`, `switches_manual.tsv`, `switches_gen.h` | the switch registry (`tools/switch_registry.py`; never hand-merge the generated files) |
| `mnrun.sh`, `aac7env.sh` | the multi-process launcher (both xshmem paths); the aac7 environment (the aac7 launch line is in `profiles/aac7.conf`) |
| `mnaccept.sh`, `digcmp.sh` | the standing regression; digit compare |
| `../target/kit/target_kit.sh` | the target session in stages (`-h`; `--dry-run` prints every command): environment, build, uffd, plan, memory edge, node screen (slow nodes; NIC count vs `SHMEM_OFI_NUM_NICS`, `--want-nics`), A3/A4 fabric tests, init check; one `KIT_SUMMARY.txt` |
| `mn_model.py` (with its `estimate` subcommand), `mem_model.py`, `aac7_fit.json` (the refit and design-table generators are archived: `archive:scripts_2026-10-23/`) | time and memory models and their fits (`mn_model.py estimate --fabric target_fit`: the profile fitted to the target's 2/64/576-node runs, results/TIME_MODEL.md §target_fit; the model's `XSHMEM_ROUND_MB` is the code's 128) |
| `ref/`, `tests/` | reference generator; unit tests (`tests/harness.h`) |

## Switches

Every environment variable the code reads (`grep -ho 'getenv("[A-Z0-9_]*")\|env_int("[A-Z0-9_]*"' *.c *.h`), one row each: the
default, the scope (launch-line, built-in default, experiment, test-only, unclassified), one sentence and the `results/` evidence.
The digits never depend on any of them: everything is bit-identical by construction and checked so by the regression (the
GMP reference digits up to 10¹¹ and T1/T2/digit residues, `../TESTING.md` §3, §5). The table is generated: the values are
in `switches.tsv` (from the source), the hand-checked defaults, scope, evidence and sentences in `switches_manual.tsv`, the
owners and review dates in `switches_review.tsv`; after any change run `python3 ../tools/switch_registry.py` (it rewrites the
tables below, `switches.tsv` and `switches_gen.h`; `--check` verifies them).


<!-- switch-table:begin (generated by tools/switch_registry.py from the switch registry; do not edit) -->

| switch | default | scope | what | evidence |
|---|---|---|---|---|
| `BS_ARENA_ROOM` | 0.14 | built-in default | with DB_POOL_VMM: f > 0 lays every arena out in whole VMM chunks and adds f x the hole (the division's largest block) to the dm need (0.14). | AS15.md+DL15.md |
| `BS_ARENA_TRIM_GB` | 0 | unclassified | trims the dm need by this many GB per APU before the arenas are rounded to whole chunks (`binsplit.c dm_layout`; twin `mem_model.py arena_trim`, so the planner's fit agrees). | - |
| `BS_BALANCE_N` | unset | built-in default | levels with at most this many nodes are balanced over the four regions; 0 = never (16) | - |
| `BS_CKPT_BG_CHUNK_MB` | 256 | built-in default | the top-level set's background writer: its DMA chunk (256; bounds the time to stop); *test*: emulate a disk of this many MB/s (unset) | N13.md |
| `BS_CKPT_BG_MBS` | 0 | test-only | the top-level set's background writer: its DMA chunk (256; bounds the time to stop); *test*: emulate a disk of this many MB/s (unset) | N13.md |
| `BS_CKPT_DIR` | unset | launch-line | the directory of the top-level set (`ECALC_CKPT_TOP`); unset = `<outfile>.top`. | W.md+WP7.md |
| `BS_LAYOUT_ONLY` | unset | launch-line | `D:g[,D:g…]` — print the arena request `binsplit_pregrow` would make for D digits per node over g node-processes (one `layout:` line each) and exit; `mem_model.py --check-c <log>` compares ... | MS15.md+B7ACCT.md |
| `BS_MDEV_LOGL` | derived | built-in default | products above 2^this limbs go to the mdev tier; lower it to test at 10⁹ (30); 29 on target runs from 1e13 digits (balances the 4 APUs, `results/MEMORY.md` §B4; `remote/ROUTE.md` S6) | M3.md+A-ckpt.md |
| `BS_SEED_CHUNK_MB` | derived | built-in default | the seed stream's buffered chunk per region (8192 with `DB_POOL_VMM`, the default; 2048 without) | H.md+R114.md |
| `BS_SEED_FILL` | unset | built-in default | the seed span chosen per run as the largest S whose last span has at most this many limbs, so the batch tier's products fill their lengths: 128 fills 2ᵏ (S = 239 at 4 × 10¹⁰, 229 at 10¹¹), ... | T215.md+DOC15.md |
| `BS_SEED_TERMS` | derived | built-in default | the seed span in terms (256; set in the environment it turns `BS_SEED_FILL`'s default off) | T215.md+WP7.md |
| `BS_SEED_THREADS` | derived | built-in default | threads of the seed pass: a number, `auto` (default since 2026-10-26: 3/4 of the CPUs the thread may run on, from the affinity mask) or `omp` (the old default omp_get_max_threads(), which is 1-2 when srun gave the task one core, e.g. `SLURM_HINT=nomultithread` without `-c`: target 2 n init 150 s vs 13-16 s, aac7 1 n 8e9 156 s vs 15.4 s, m) | I.md+S22.md |
| `COMM_HANG_DETAIL` | unset | built-in default | `1`: the xshmem allgather-handshake HANG message (after 900 s) also names the members that have not arrived (member index and global PE, at most 16, and their count) and this host; read only at the failure (`comm_xshmem.c hang_handshake_detail`); the default 0 prints the old message only. | - |
| `COMM_LAYER_VSLOT_MB` | derived | built-in default | - | - |
| `COMM_OFI_NICS` | unset | launch-line | the NIC list per device, `;` between devices (indexed by the node-local GPU), `,` between one device's NICs (`0;1;2;3`; the target's form `0,4;1,5;2,6;3,7`), read by `pe_geo.c`; lane k of `XSHMEM_LANES` uses NIC k of the PE's list (unset: the cxi devices on the GPU's NUMA node). | TUNE17.md+TGTBENCH2.md |
| `COMM_OFI_POOL_MB` | 0 | launch-line | MiB of the transport's per-device pool (the name is from the removed comm_ofi): the planner sets it from the modelled need when unset, it is counted in the budget, and it sizes the xshmem DATA heap per PE when `XSHMEM_HEAP_MB` is 0 (the pool / 4 + 256 MiB rule of `comm_xshmem.c`) | STD17.md+S18KITFIX.md |
| `COMM_RANK` | 0 | launch-line | this process's rank and the number of node-processes (set by `mnrun.sh`; unset or 1 = the single-node program) | A-comm.md+WP6.md |
| `COMM_SEQ_CHECK` | 0 | built-in default | diagnostic: `1` = every collective writes its per-communicator call number, kind and size next to its signals, and a mismatch between members is fatal naming both calls (a divergent call sequence); costs two words per signal; the default 0 changes nothing. | - |
| `COMM_SHMEM_POOL_MB` | 8192 | launch-line | the pool figure of the pool rule (8192 when nothing sets it): `mnrun.sh` sets it, when unset, to the run's modelled need from `MN_PLAN_ONLY=<digits>:<procs>`'s `plan pool` line; the Cray library heap is pool + 512 MiB (`SHMEM_SYMMETRIC_SIZE`), and unless `COMM_OFI_POOL_MB` or `XSHMEM_HEAP_MB` is set the xshmem DATA heap follows it; a hand value wins. | STD17.md+P214.md |
| `COMM_SIZE` | 1 | launch-line | this process's rank and the number of node-processes (set by `mnrun.sh`; unset or 1 = the single-node program) | MS15.md+S16.md |
| `COMM_TRANSPORT` | unset | launch-line | `xshmem`, the only multi-node transport (since Q103, 2026-10-25; `shmem` was removed): the default build includes the engine where libfabric and the PMI2 header are found (`make XSHMEM=ofi`, path 1; `XSHMEM=cray`, path 2), and `mnrun.sh` sets it for more than one process or under `ECALC_PE_PER_GPU=1`; unset = one process, no transport. | S.md+OFI17.md |
| `DB_POOL_VERBOSE` | 0 | launch-line | the dbig block pool's growth and how the reserved tails were used (follows `RNS_VERBOSE`) | T215.md+S18KITFIX.md |
| `DB_POOL_VMM` | derived | built-in default | the bs/dm arena of every APU as a HIP virtual-memory range (`hipMemAddressReserve` at `DB_POOL_VMM_RESERVE` (3) × the arena; 2 GiB chunks `hipMemCreate`d and mapped with access for the four ... | R114.md |
| `DB_POOL_VMM_CHUNK_GB` | 2 | built-in default | with `DB_POOL_VMM`: the chunk size in GiB (2) and the VA reserved as a multiple of the arena (3) | S46.md+R114.md |
| `DB_POOL_VMM_RESERVE` | 3 | built-in default | with `DB_POOL_VMM`: the chunk size in GiB (2) and the VA reserved as a multiple of the arena (3) | R114.md |
| `DIST_CHUNKS` | derived | built-in default | slabs in flight in the pipelined distributed transform, 1–16 (**8** since 2026-10-07, was 4: the user's approval, digits identical; at 3.71 × 10¹³ on 576 nodes modelled device 370.9 → 357.5 ... | S19B.md+A-comm.md |
| `DIST_GATHER_RAW_MARGIN_GB` | 2 | built-in default | rns_dist.c; K1b, GB per APU kept free when DIST_GATHER_RAW=2 takes its plane | - |
| `DIST_GEN` | 0 | test-only | the general (any-g) transform at a power-of-two group size too (0) | X13b.md+S36.md |
| `DIST_LOGN_TEST` | derived | test-only | a lower plane cap so the grid split runs at small sizes (31) | G12.md+P13b.md |
| `DIST_STATS` | derived | test-only | per-part timing of the distributed transform, the exposed exchange per part (unset) | G515.md+CX15.md |
| `DM_MN_WRAP` | auto (fit) | built-in default | Automatic since 2026-10-13 (user decision D26, results/LAUNCH_DEFAULTS.md §FIT): unset = 1 when the device peak with the wrap is at most `ECALC_DEVICE_GB` − `ECALC_DEVICE_MARGIN_GB` (the ... | - |
| `DM_TIGHT` | derived | built-in default | the reciprocal's scratch reserved once at the last doubling's sizes instead of the final ones — r at ⌈k/2⌉ + 4 limbs, r2 at 2⌈k/2⌉ + 4, t1 at Q_t·r's size there — with r' copied into r's ... | L114.md+T114.md |
| `ECALC_ARENA_GB` | 0 | built-in default | the region arena per device, in GB, instead of the layout's own sizing (auto) | M11.md+A-mem.md |
| `ECALC_BG_THREADS` | 48 | built-in default | the background OpenMP team of the recurrence and the writer (48) | - |
| `ECALC_BUDGET_CHECK` | unset | built-in default | `1` = after init every rank models its peak (device in use or the model's planes + arena, whichever is larger, + the host terms or VmRSS) and the ranks decide together (an allgather, the ... | C314.md+V314.md |
| `ECALC_CHECKPOINT` | 0 | launch-line | `1` = development and testing: the budgeted top set (`ECALC_CKPT_TOP=2`) unless `ECALC_CKPT_TOP` is set; the headline / record runs leave it unset (0) | P16.md+DOC15.md |
| `ECALC_CKPT_TOP` | auto | launch-line | the top-level P, Q on disk for the recheck (above): `BS_CKPT_DIR` or `<outfile>.top` (**off by default** — record timing runs, the user's decision of 2026-09-27; `ECALC_CHECKPOINT=1` makes ... | N13.md |
| `ECALC_CKPT_TOP_SLACK` | 1 | built-in default | `ECALC_CKPT_TOP=2`: the seconds a release may wait for the writer (1) | N13.md+IO15.md |
| `ECALC_CORR_PATCH` | 2 | built-in default | the division's ±1 corrections (/dx/ ≤ 64) made after the size-1 writer started on X (the hook before the low product) are **not** applied to X: T1 takes X's residues from the writer thread ... | KP15.md |
| `ECALC_CORR_PATCH_ZONE` | 4096 | built-in default | with `ECALC_CORR_PATCH`, the digits at the end whose T2 windows are checked after the patch instead of by the writer (4096) | K15.md |
| `ECALC_DEVICE_GB` | 373.44 | built-in default | the device edge per node, GB, the memory fit checks (default 373.44 = the target's, 93.36 GB per APU, m, results/TARGET.md §TGTBENCH2; profiles/aac7.conf: 444.0, m, the aac7 t_edge VMM ... | - |
| `ECALC_DEVICE_MARGIN_GB` | 5 | built-in default | the safety margin per node (default 5.0 GB, the tighter of CAP17's two bars) under `ECALC_DEVICE_GB` that `DM_MN_WRAP` automatic keeps: the wrap is taken only when the device with it is at ... | - |
| `ECALC_FATAL_ABORT` | derived | built-in default | `1` = a fatal error ends in `abort()` (a core file, rc 134) instead of `_exit(code)` (0). | S18W.md+C314.md |
| `ECALC_FATAL_TEST` | unset | test-only | `1` = the four bs worker threads fail together right after the seeds (the in-phase guards' pattern): one message, rc 3 (0) | C314.md |
| `ECALC_FATAL_WAIT` | 60 | built-in default | seconds the first failing thread may spend on its report (the memory accounting) before a watchdog `_exit`s (60) | C314.md |
| `ECALC_FIT_PER_APU` | unset | built-in default | `1`: the planner's device fit also compares the busiest APU (a quarter of the planes + the largest region's arena + the device pool's share + the v-slots; region 0 holds the top batch levels) against its quarter of ECALC_DEVICE_GB and refuses above it, and prints a `per APU (max)` line; the default 0 keeps the node-mean fit and the output unchanged (`binsplit.c binsplit_fit`; twin `mem_model.py` key `dev_apu_max`). | - |
| `ECALC_INIT_ONLY` | unset | test-only | - | L114.md+P13b.md |
| `ECALC_INIT_TL` | 0 | launch-line | 1 = init's timeline as `tl <s since rns_init began>` lines: rns_init's steps, each arena's mapping with the HIP calls split (create / map / access / zero), the background mappers (start, ... The xshmem engine adds `tl <s since the engine init began>  xshmem:` lines (rank 0 only, at most 12 per kind plus a summary): init phases (boot/fabric open, lane contexts, CTRL heap, DATA heap, workers), each communicator created (id, members, s), the first-call allgather handshakes, and mn_init's transport and mesh seconds. Also: a `process age at main` line (process start to main, from /proc/self/stat), an `init = plan/fit + mn_init + rns_init + mn_selftest + region pools + other` line that sums to the init figure, and (xshmem built on libfabric) an `ofi init` split (PMI init and topology, endpoints and domains with the first HIP call and kernel, PMI address exchange, `fi_av_insert`); the value 2 prints them on every node leader, 1 on rank 0 only (other ranks print a LATE START line when their process age at main exceeds 30 s). | MAP15.md |
| `ECALC_MEM_GUARD_GB` | 0 | launch-line | the sampler stops the run (rc 9, `EC_RC_MEMGUARD`) through `ec_fatal` — one line naming rank, host, the last phase boundary and the figures — when MemAvailable falls below this many GB; ... | IO15.md+DOC15.md |
| `ECALC_MEM_SAMPLE` | derived | test-only | seconds between memory samples (RSS, MemFree, MemAvailable, Cached, Dirty, Writeback from `/proc/meminfo`, the pool's live bytes per APU), a summary line at exit (minimum MemAvailable, ... | IO15.md+S114.md |
| `ECALC_NODE_GB` | 480 | built-in default | - | P16.md+C314.md |
| `ECALC_NP` | auto | launch-line | the number of NTT primes: 3 (Phase 13b step 0: −17 % wall, −25.8 GB of planes; `ecalc.c` sets it when unset; 4 was the binary-limb count, archived with archive:PLAN §28.5). | P15.md |
| `ECALC_NP_AUTO_TERMS` | derived | test-only | under `ECALC_NP=auto`, the switch-over in terms (1 … 58 424 467 928, the three-prime bound): a lower value makes small runs form both counts (the forced tests of results/PRIMES_STRATEGY.md ... | NP15.md |
| `ECALC_ODIRECT` | 1 | built-in default | 1 = the checkpoint files (the top-level set, in the background) and the output file are written with O_DIRECT through aligned buffers (`spill.c`'s `sp_file`; the last partial block padded, ... | S114.md+KP15.md |
| `ECALC_OMP_TEAM` | omp | experiment | omp_team.h: size of the host OpenMP teams (bs_nthreads, packed_mods residue, verify.c, mem.c hstage): omp = omp_get_max_threads() (latched from the ... | S22.md |
| `ECALC_OUT_MODE` | derived | built-in default | the part file's write mode: `direct` (O_DIRECT), `buffered` (page cache, no sync: the pre-Phase-14 form), `sync` (buffered, `fsync` + `posix_fadvise(DONTNEED)` at close), `drop` (buffered, ... | IO15.md |
| `ECALC_OUT_ODIRECT` | derived | built-in default | with `ECALC_ODIRECT=1`, 0 keeps the output file buffered (1) | S114.md+KP15.md |
| `ECALC_OUT_PACKED` | 2 | built-in default | (`packed_fmt.h` version 2): the same header and checks, the limbs (< 2^60) as a little-endian 60-bit stream, 15 bytes per 2 limbs (7.5 B per 18 digits = 0.4167 B/digit, -6.25 % bytes ... | KP15.md+IO15.md |
| `ECALC_PE_PER_GPU` | 0 | experiment | (default 0 = off, byte-identical to before): 1 = ONE PROCESS PER GPU (PE): `mnrun.sh` launches 4 tasks per node (`MNRUN_PPG_PPN`, default the node's GPU count; block distribution, ... | - |
| `ECALC_PLANE_CAP` | derived | experiment | the plane cap, `2^30` or `2^31` (or `30`, `31`) or `fit` (the larger whose node total fits `ECALC_NODE_GB`, default 480) or `run` (2^30 when the run's largest product, the division's 2 k + 8 limbs, fits one plane of that cap: about 9.6e9 digits per node and below; else 2^31; 2^31 with `ECALC_PE_PER_GPU`; prints one `ECALC_PLANE_CAP=run:` line, `run (default):` when unset); the default is `run` (2026-10-25, user): `2^31` forces the old cap; with `POOL_LOG` or `DIST_LOGN_TEST` given the knobs stay as given, and `fit` is the default with `ECALC_PE_PER_GPU=1` and `XSHMEM_PES_PER_GPU` > 1; sets `POOL_LOG` and `DIST_LOGN_TEST` and refuses a conflicting one; ... | P13b.md+CAP17.md |
| `ECALC_PLANE_CAP_IDX` | derived | test-only | set by `ecalc.c` for the bs sizing pass (the plane-cap index in use); do not set | - |
| `ECALC_RECHECK` | 0 | launch-line | 1: the standalone recheck of a finished run's files instead of a run (0). | V.md+KP15.md |
| `ECALC_RESIDUE` | off | experiment | residue_shoup.h: the digit residues of packed_mods (mn_out.c) and vf_digits_mods (verify.c): shoup = Shoup precomputed-quotient multiply per Horner ... | S22.md |
| `ECALC_SEGV_TRACE` | 0 | launch-line | `1` = install a SIGSEGV/SIGBUS/SIGABRT handler right after `main` knows `COMM_RANK` (`segv_trace.c`): on the signal it prints, with `write(2)` only (async-signal-safe, no malloc/printf), ... | S18W.md+S30.md |
| `ECALC_TEST_CORR` | 0 | test-only | - | DKM15.md+KP15.md |
| `ECALC_TEST_DIE_PE` | -1 | test-only | ecalc.c; kill test (ECALC_PE_PER_GPU gate): PE <n> dies by ec_fatal after the bs phase; unset = off | - |
| `ECALC_VERBOSE` | 1 | launch-line | 1: the phase lines; 2: per-level, init and memory detail, the pool-rule lines and every PE's init (`COMM_SHMEM_VERBOSE` was removed 2026-10-19). | PC15.md+M13.md |
| `ECALC_VMM_BG` | unset | built-in default | `0` = with `DB_POOL_VMM`, map the whole arena synchronously in the calling thread (no `vmm_bg_map` thread; init is slower by the mapping time the thread hid); default `1` = unchanged. | S34.md |
| `ECALC_VPE_LANES` | 4 | experiment | PEs per node where there is no launch (planners, `MN_PLAN_ONLY`, unit-test views; default 4, 1..8); with `ECALC_PE_PER_GPU=1` the launch's tasks per node win and a different value is fatal | - |
| `ECALC_WINDOWS` | derived | test-only | a file of extra T2 windows (`<offset> <digits>` per line) added to the built-in table (unset) | A-out.md+H.md |
| `MDB_SHIFT_CHUNK_MB` | 1024 | built-in default | the sharded division's `mdb_shift` exchanged in rounds of about this many MB per APU per slab (sb, rb) instead of the whole share / 4 (71 GB per node at 576 × 8 × 10¹⁰); bit-identical; 0 = ... | P214.md |
| `MEM_DPOOL_FILL` | 0 | test-only | fill a grown (1) or every (2) plane pool with 0xA5 — a test of zero-memory assumptions (0) | V.md+R.md |
| `MEM_REPORT_DEVS` | 0 | launch-line | `mem_report` adds per-APU rows (driver used/total) to the per-phase memory table; *B7ACCT*: at size > 1 each row also prints the terms outside `in use` -- the layout's v-slots ... | B7ACCT.md+TC15.md |
| `MN_COMM_MARK` | 0 | launch-line | 1 prints, on node 0, a `comm-mark` line at the meshes' creation, the end of each tree level, the reciprocal and the division: this PE's all-to-all exchanges, GB received, post-to-completion ... | D16.md |
| `MN_GROUPS` | unset | launch-line | the tree's level → group-size schedule, e.g. | T114.md+SC15.md |
| `MN_NONPOW2_CAP` | auto | built-in default | *T2 (D57), default auto since Q58 (user 2026-10-20): on from `MN_NONPOW2_MIN` digits per node-process, off below (the larger pools cost a fixed +7 s of init at 3 n 6e10; crossover about ... | - |
| `MN_NONPOW2_MIN` | 4.5e10 | built-in default | digits per node-process (total digits / `COMM_SIZE`) from which `MN_NONPOW2_CAP` auto is on; default 4.5e10 (results/DIVISION.md §NPCAP crossover, mod); `MN_PLAN_ONLY` and the memory fit ... | - |
| `MN_OUT_CHUNK_MB` | 256 | built-in default | the streamed writer's digit chunk per node (256) | IO15.md+A-out.md |
| `MN_OUT_DKM_HI_TEST_CARRY` | unset | test-only | - | EW15.md |
| `MN_OUT_EARLY` | 1 | built-in default | at size > 1 the part file starts at the division's hook (X formed, before the low product: `newton_mn_x_hook` → `mn_out_early_start`) in a background thread over the node's share, as the ... | IO15.md |
| `MN_OUT_EARLY_ALWAYS` | 0 | experiment | ecalc.c mn_hooks_set: 1 = the early output hook (MN_OUT_EARLY) also without an output file: the writer computes the digit residues only, overlapped ... | S22.md |
| `MN_OUT_STRIPE` | 1 | built-in default | `<count>[:<stripe MB>[:<OSTs>]]` — each part file created with that Lustre layout (`lfs setstripe`); with `<OSTs>` the first OST is `(part × count) mod OSTs`. | IO15.md+SC15.md |
| `MN_OUT_THREADS` | 8 | built-in default | pwrite threads per chunk of the part file (8) | IO15.md+TARGET_HW_REVIEW.md |
| `MN_PLAN_ONLY` | unset | launch-line | `<total digits>:<g>` — the plan of a run (every large product's grid, pieces, planes, from the code's own decisions; no device) and exit. | P15.md+T215.md |
| `MN_TREE_LOGN_TEST` | 0 | test-only | the tree's levels at a lowered plane cap so the grids run at 10¹⁰ on one node; the division at its own cap (0) | G12.md+M13.md |
| `MN_T_CHUNK_MB` | 1024 | launch-line | a piece product's window (the temporary T and the result slab rbO of `mn_core`, and `mdb_add_shifted`'s T) in rounds of this many MB per APU instead of the whole share of C (35 GB per node ... | P214.md+S22.md |
| `MN_WAIT_STATS` | 0 | launch-line | per-communicator wait-time statistics (`comm_util.c`) (0) | S27.md+S22.md |
| `NEWTON_DKM_TEST_HI` | 0 | test-only | - | DKM15.md+EW15.md |
| `NEWTON_MN_GROUPS` | 1 | built-in default | at size > 1 the reciprocal's early doublings on the smallest subgroup the model prefers (X1); 0 = the full group (1) | X.md+G13d.md |
| `NEWTON_MN_RECIP_TS` | 0 | test-only | S30, print only (off): one line per node, `recip ts node N: ...`, with the stages of recip_mn's single-node part (shift, gather, chain, broadcast) and the wall clock (s mod 1000) | S30.md |
| `NEWTON_MN_SPLIT` | derived | built-in default | at size > 1 the reciprocal starts single-node on the top limbs of Q up to this many limbs (65536) | A37CMP.md+A-div.md |
| `NEWTON_RECIP_GUARD` | unset | built-in default | the reciprocal's cut (always on): limbs kept below the read band (the cut is at band − guard; ≥ 1 makes the band's error ≤ 1 unit for any number of pieces) (1) | R114.md |
| `P24_CHECK` | 0 | diagnostic | results/PRIMES_STRATEGY.md §P24 audit | - |
| `P24_CHECK_TEST` | 0 | diagnostic | results/PRIMES_STRATEGY.md §P24 audit | - |
| `P24_CHECK_TEST_K` | 1 | diagnostic | results/PRIMES_STRATEGY.md §P24 audit | - |
| `P24_CHECK_TEST_S` | -1 | diagnostic | results/PRIMES_STRATEGY.md §P24 audit | - |
| `P24_CRT_ANY` | 1 | built-in default | results/PRIMES_STRATEGY.md §P24 audit | - |
| `PE_GEO_SYSFS_ROOT` | - | test-only | PEs per GPU (default 1; the 2-PEs-per-GPU fallback of design 2.2) / a prefix for every `/sys` path (mock trees of `tests/t_pe_geo`) | - |
| `POOL_LOG` | 31 | launch-line | the plane pools' 2ᵏ points per APU (31; 27–29 with several node-processes on one node) | A-mem.md+M13.md |
| `RNS_BATCH_LOCAL_MIN` | unset | built-in default | below this many products the striped batch path is used (16) | P3.md+R.md |
| `RNS_BATCH_SPLIT` | 0 | experiment | automatic under `ECALC_PE_PER_GPU=1` (Q110: no launch line sets it): a batch product larger than the one device's plane pool is split by operand pieces; 20..29 forces pieces of at most 2^k limbs (tests). | - |
| `RNS_BATCH_TILE_GB` | unset | built-in default | the batch tiers' plane budget per device in GB (a + b planes), capped by the pools (15) | P.md |
| `RNS_DIST_CACHE_MARGIN_GB` | 24 | built-in default | free device memory kept when sizing the cache (24) | TC15.md+M13.md |
| `RNS_POOL1_GB` | unset | built-in default | plane pool 1 per device in GB (auto: the dist tier's 3q + 16 limbs) | R.md+T.md |
| `RNS_POOL_GROW` | 0 | test-only | a region pool that would grow inside bs aborts with the accounting unless 1 (the stress recipe sets it) (0) | B16.md+R.md |
| `RNS_STRATEGY` | derived | built-in default | the single-node product's strategy (`dist_core`: top levels, reciprocal, division): `C` four-step; `B` prime-per-APU (planes that do not fit the pools from an extra per-APU buffer); `auto` ... | B13b.md+G13d.md |
| `RNS_VERBOSE` | 0 | launch-line | 1 per-call times, 2 CRT re-runs; also the pool allocations' times (unset) | PS15.md+M.md |
| `XSHMEM_COLL` | flat | experiment | experiment, default flat: tree = dissemination (ceil(log2 n) rounds, offsets only with the log n partners) instead of every member to every member; digits identical; results/XSHMEM.md | - |
| `XSHMEM_CRAY_GPU_PRETOUCH` | 0 | experiment | diagnosis, default 0 (XSHMEM=cray): 1 = at init one GPU kernel on the PE's device reads a byte of every 4 KiB page of the hipHostRegister'ed DATA heap (populates the GPU page tables; tests ... | - |
| `XSHMEM_CRAY_HEAP_THP` | 0 | experiment | diagnosis, default 0 (XSHMEM=cray): 1 = madvise(MADV_HUGEPAGE) on the DATA heap before it is zeroed and MADV_COLLAPSE after; 2 = only report; each PE prints the heap mapping's ... | - |
| `XSHMEM_CRAY_NOMEMSET` | 0 | experiment | diagnosis, default 0 (XSHMEM=cray): 1 = skip the engine's zeroing of the DATA heap (a fresh libsma zero mapping); with `XSHMEM_LANE_TRACE` set, the init prints shmem_init_thread and heap ... | - |
| `XSHMEM_CRAY_NUMA` | 0 | experiment | default 0 in the engine; `mnrun.sh` sets 1 and `MNRUN_PPG_CPUBIND=ldom` by default for a Cray build under `ECALC_PE_PER_GPU=1` (explicit values win; ofi builds and layout D unchanged; the ... | - |
| `XSHMEM_CRAY_PUTSIG_SPLIT` | 0 | experiment | runtime override of the compiled xshmem_cray.h selectors (signal set 0..3, signal test 0/1, split put+fence+signal 0/1); default = the compiled value | - |
| `XSHMEM_CRAY_SIGSET` | 0 | experiment | runtime override of the compiled xshmem_cray.h selectors (signal set 0..3, signal test 0/1, split put+fence+signal 0/1); default = the compiled value | - |
| `XSHMEM_CRAY_TEST` | 0 | experiment | runtime override of the compiled xshmem_cray.h selectors (signal set 0..3, signal test 0/1, split put+fence+signal 0/1); default = the compiled value | - |
| `XSHMEM_CTRL` | heap | experiment | `heap` (default; control words in the device heap) or `host` (control words in a host region, design 1.3 b) | - |
| `XSHMEM_HANG_S` | 900 | built-in default | diagnostic: the seconds after which a handshake or small collective wait is reported as HANG (fatal); raise it (e.g. 3600) to tell a slow member (imbalance: the run then completes) from a lost one; the default keeps the old 900 s. | - |
| `XSHMEM_HEAP_MB` | 0 | experiment | MiB of the per-PE symmetric heap (0 = backend default). | - |
| `XSHMEM_HEAP_SLACK_MB` | 0 | experiment | (default 0 = off, nothing changes): MiB added to each loopback PE's DATA heap in `comm_xshmem.c`; the Cray backend (`make XSHMEM=cray`) takes the control words and alignment out of the same ... | - |
| `XSHMEM_HS_BATCH` | 0 | experiment | experiment, default 0: k > 0 = the flat handshake / small collectives visit their members in rotated order (no common first target) and quiet every k signals, instead of one ascending burst; digits identical; results/XSHMEM.md | - |
| `XSHMEM_LANES` | 1 | experiment | lanes (SHMEM contexts) per PE, default 1; the engine creates them right after init in order (lane k = NIC k); may exceed the PE's NICs (virtual lanes on aac7); at most 4 (larger values are ... | - |
| `XSHMEM_LANE_CTX` | serialized | experiment | diagnosis, default serialized: `private`, `none` or `serialized` context options for lanes | - |
| `XSHMEM_LANE_POLL` | 0 | experiment | diagnosis, default off: poll the lane contexts' progress while waiting | - |
| `XSHMEM_LANE_TRACE` | 0 | experiment | diagnosis, default off (any layout): value = seconds; exchanges slower than that print one `SLOW` line (post split into staging, copies, stream sync, default quiet; per-lane quiet; last two ... | - |
| `XSHMEM_LOOPBACK_PES` | 1 | experiment | PEs one process hosts in the libfabric backend (default 1 = one PE per process, the only value the flow runs: `ECALC_PE_PER_GPU=1` refuses another; `mnrun.sh` sets it under ... | - |
| `XSHMEM_PEER` | -1 | experiment | (default: on with one PE per process, Q100 2026-10-25; `0` = off, the NIC path; the default layout D is unaffected): same-node PEs exchange payload by device copies (`hipMemcpyAsync` on one ... | - |
| `XSHMEM_PEER_STREAMS` | 8 | experiment | XSHMEM_PEER=1 device-copy streams per PE (default 8 = one per peer; peer pe uses stream pe mod k; 1 = every copy of a round on one stream); measurement aid for the copy-engine A/B, digits identical; results/SINGLE_NODE.md §PEER | - |
| `XSHMEM_PES_PER_GPU` | 1 | experiment | PEs per GPU (default 1; the 2-PEs-per-GPU fallback of design 2.2) / a prefix for every `/sys` path (mock trees of `tests/t_pe_geo`) | - |
| `XSHMEM_ROUND_MB` | 128 | experiment | the xshmem engine (`COMM_TRANSPORT=xshmem`) carries an all-to-all whose pair exceeds this / (n − 1) MiB in rounds that stage at most this many MiB each way (the self slab is not staged; one ... | - |
| `XSHMEM_ROUND_STATS` | 0 | diagnostic | diagnostic, default 0 (no clock reads when off): 1 = every PE prints at exit (and every 5000 rounds) the round path's phase times: exclusive worker seconds per phase (ROFF post, staging copy, copy issue, copy quiet, signal post, signal quiet, receive copy, ROFF poll, signal poll), wall seconds of the two waits (these include the worker's turns on other pending exchanges) the pending exchanges per round (mean, max), a gpu line per PE (HIP-event GPU stream time between one exchange's end and the next post, per phase tag, with host, GPU index and PCI bus id on every line), a sync line per PE (whether the caller stream was idle at the post, event sync vs remaining stream sync, a device-wide sync probe: the probe adds its wait to the post), and a second line per PE with the exchange timeline (post, worker wake, worker span with its unclassified gaps, late, notice, end: they sum to the post-to-completion total; caller blocked and working times; post to the first ROFF write); digits identical; results/SINGLE_NODE.md §PEER | - |
| `XSHMEM_SIG_LANES` | 0 | experiment | diagnosis, default off: signals on every lane instead of lane 0 only | - |
| `XSHMEM_SPARSE` | 0 | experiment | experiment, default 0: 1 = a peer with zero bytes in one direction gets no offset word / signal / wait / round for that direction (no offset word, no signal, no wait, no round), so a shift moves messages only between the nodes that overlap; counts must agree on both sides (a mismatch then hangs instead of a count fatal); digits identical; results/XSHMEM.md | - |
| `XSHMEM_THP_DISABLE` | 0 | experiment | diagnosis, default 0: 1 = prctl(PR_SET_THP_DISABLE) in the engine init, so this process takes no THP faults (and no direct-compaction stalls) | - |
| `XSHMEM_THREAD_LEVEL` | 3 | experiment | default 3 (MULTIPLE): thread level of the engine's SHMEM init; 2 = SERIALIZED as the ofi backend | - |

<!-- switch-table:end -->

Switches read only by tests are listed in `switches.tsv` (source `tests`); `mnaccept.sh` reads `ECALC_REF` and `ECALC_REF_4E10`
(the reference files).

**Review dates (owner and expiry of every non-default setting; TASKS 27, results/TOOLS_PROFILES.md §T27)**

Every experiment, diagnostic and test-harness switch, and every fallback value kept after a default flipped, has an owner
and a review date in `switches_review.tsv` (hand-written; columns `owner` and `review` of the generated `switches.tsv`).
`tools/switch_registry.py --check` (so `tools/doccheck.py` and `tools/gate.sh --build`) prints a `WARN` line for a review
date in the past, for an experiment or test-only switch without a row, and for a row whose switch is gone; warnings never
fail. At the review the owner removes the switch (or the fallback value), proposes adoption, or moves the date with a reason.
The table below is generated from `switches_review.tsv` by `tools/switch_registry.py` (do not edit it by hand).

<!-- switch-review:begin (generated by tools/switch_registry.py; edit ecalc/switches_review.tsv) -->

| class | owner | review by | switches |
|---|---|---|---|
| experiment | coordinator (TASKS S) | 2026-11-11 | `BS_ARENA_TRIM_GB`, `ECALC_PLANE_CAP` |
| experiment | T2 (D57) | 2026-11-19 | `MN_NONPOW2_CAP`, `MN_NONPOW2_MIN` |
| experiment | TARGET1 (B1) | 2026-12-31 | `XSHMEM_CTRL`, `XSHMEM_HEAP_MB`, `XSHMEM_LANES`, `XSHMEM_PES_PER_GPU` |
| experiment | TARGET1 (S2) | 2026-12-31 | `ECALC_VPE_LANES` |
| experiment | TARGET1 (S3a) | 2026-12-31 | `XSHMEM_LOOPBACK_PES`, `XSHMEM_ROUND_MB` |
| experiment | TARGET12 (S2) | 2026-12-31 | `ECALC_PE_PER_GPU`, `RNS_BATCH_SPLIT` |
| experiment | TARGET12 (S22) | 2026-12-31 | `ECALC_OMP_TEAM`, `ECALC_RESIDUE`, `MN_OUT_EARLY_ALWAYS` |
| experiment | TASK4 peer | 2026-12-31 | `XSHMEM_PEER`, `XSHMEM_PEER_STREAMS` |
| experiment | TGTSHAPE | 2026-12-31 | `XSHMEM_HEAP_SLACK_MB` |
| experiment | comm | 2026-12-31 | `XSHMEM_COLL`, `XSHMEM_HS_BATCH`, `XSHMEM_SPARSE` |
| experiment | craylane | 2026-12-31 | `XSHMEM_CRAY_GPU_PRETOUCH`, `XSHMEM_CRAY_HEAP_THP`, `XSHMEM_CRAY_NOMEMSET`, `XSHMEM_CRAY_NUMA`, `XSHMEM_CRAY_PUTSIG_SPLIT`, `XSHMEM_CRAY_SIGSET`, `XSHMEM_CRAY_TEST`, `XSHMEM_LANE_CTX`, `XSHMEM_LANE_POLL`, `XSHMEM_LANE_TRACE`, `XSHMEM_SIG_LANES`, `XSHMEM_THP_DISABLE`, `XSHMEM_THREAD_LEVEL` |
| fallback | P1/DEF (D22, results/DIVISION.md §P1, results/LAUNCH_DEFAULTS.md §DEF) | 2026-11-11 | `DM_MN_WRAP` |
| fallback | coordinator (Q130) | 2026-11-11 | `P24_CRT_ANY` |
| fallback | coordinator (TASKS S) | 2026-11-11 | `ECALC_OUT_PACKED` |
| diagnostic | TARGET12 (S2) | 2027-04-12 | `ECALC_TEST_DIE_PE` |
| diagnostic | comm | 2027-04-12 | `COMM_SEQ_CHECK`, `XSHMEM_HANG_S` |
| diagnostic | diagnostics (TESTING.md) | 2027-04-12 | `BS_CKPT_BG_MBS`, `DIST_GEN`, `DIST_LOGN_TEST`, `DIST_STATS`, `ECALC_FATAL_TEST`, `ECALC_INIT_ONLY`, `ECALC_MEM_SAMPLE`, `ECALC_NP_AUTO_TERMS`, `ECALC_TEST_CORR`, `ECALC_WINDOWS`, `MEM_DPOOL_FILL`, `MN_OUT_DKM_HI_TEST_CARRY`, `MN_TREE_LOGN_TEST`, `NEWTON_DKM_TEST_HI`, `NEWTON_MN_RECIP_TS`, `P24_CHECK`, `P24_CHECK_TEST`, `P24_CHECK_TEST_K`, `RNS_POOL_GROW` |
| test | ecalc/tests/t_alloc.c | 2027-04-12 | `T_ALLOC_NTT`, `T_ALLOC_PROBE`, `T_ALLOC_SEED` |
| test | ecalc/tests/t_dist.c | 2027-04-12 | `DIST_BIG`, `DIST_BIG_K`, `DIST_BIG_PRIMES`, `DIST_H4`, `DIST_H4_MB`, `DIST_H4_REPS`, `DIST_LAYERED`, `DIST_LBENCH`, `DIST_LREPS`, `DIST_TBENCH`, `DIST_TINV`, `DIST_XGMI` |
| test | ecalc/tests/t_patch.c | 2027-04-12 | `T_PATCH_DIR`, `T_PATCH_UNPACK` |
| test | ecalc/tests/t_strategy.c | 2027-04-12 | `TSTRAT_BUDGET_GB`, `TSTRAT_GMP` |
| internal | planner channel (binsplit.c / ecalc.c setenv) | 2027-04-12 | `ECALC_PLANE_CAP_IDX` |
| test-only | TARGET1 (B3) | 2026-12-31 | `PE_GEO_SYSFS_ROOT`, `TMPDIR` |

<!-- switch-review:end -->
