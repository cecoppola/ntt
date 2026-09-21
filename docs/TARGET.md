# TARGET.md — the runbook for the 576-node target (PLAN.md §25; Phase 12 agent Q, 2026-09-21)

The target: 576 MI300A nodes (4 APUs each, 2 304 APUs), HPE Slingshot-2 dragonfly (diameter 3, groups all-to-all
inside and globally), two 400 Gb/s NICs per APU (100 GB/s per APU, 400 GB/s per node), SHMEM (Cray OpenSHMEMX
expected; rocSHMEM or a SOS-class OpenSHMEM acceptable), `srun`. Everything below was checked against the code at
`main` 7aded87 plus the Phase 12 branches; every environment variable named here exists in the code (§9 lists the
grep). The numbers come from `ecalc/estimate.py` (the model of `mn_model.py` + `mem_model.py`; results/Q.md) and are
labelled **measured** (a recorded aac6 run), **modelled** (arithmetic on measured inputs) or **assumed** (a target
parameter no aac6 measurement can give).

## 1. What to expect (the standing estimate, tree form `grid` = the Phase 12 top product)

| per node | digits total | per-node wall | node peak | fits |
|---|---|---|---|---|
| **3.8 × 10¹⁰** (every level product one piece) | 2.19 × 10¹³ | **2.0 min** | 395 GB | yes, with margin |
| **6.1 × 10¹⁰** — the safe size (480 GB) | **3.5 × 10¹³** | **3.8 min** | 479 GB | yes, with margin |
| **6.7 × 10¹⁰** — the ceiling (502 GB) | **3.9 × 10¹³** | **4.0 min** | 501 GB | no margin |
| 7.7 × 10¹⁰ (Phase 11's headline) | 4.4 × 10¹³ | 4.7 min | 541 GB | **no** |

Per-node compute measured (one node: 4 × 10¹⁰ in 81.5 s, 8 × 10¹⁰ in 195.5 s, 10¹¹ in 262.9 s); the distributed
levels, the sharded division and the memory at g > 1 modelled; the fabric's 100 GB/s per APU, 2 µs per message and
the part file's 2 GB/s per node assumed. The exposed communication is 15–18 % of the wall; the distributed tree
levels are ≈ 35 % of it. The table assumes the two Phase 12 forms: agent G's gridded top product (`--tree grid`)
and agent S's pool-resident slabs (`--staging resident`). **With the code as merged at 7aded87 (`estimate.py
--as-is`) the ceiling is 9.5 × 10⁹ per node = 5.5 × 10¹² digits**: the tree's arena request (trap 6) alone caps at
1.9 × 10¹⁰ per node, and the SHMEM transport's staging (trap 11: every live communicator keeps its largest
exchange's send + receive staging in the pool, 345 GB per node at 576) alone at 1.9 × 10¹⁰ too. Freeing the
staging after every exchange (`--staging per_exchange`, a small change in `comm_shmem.c`) gives 5.6 × 10¹⁰ per
node (3.2 × 10¹³ digits, 3.3 min) without S's resident slabs.

Sensitivity (576 × 6.1 × 10¹⁰; `estimate.py --g 576 --D 6.1e10 ...`): per-message cost 2 → 20 µs: 3.8 → 4.3 min;
injection 100 → 50 GB/s per APU: 4.6 min; part files at 1 GB/s: 4.3 min; global links at half the injection
(`--taper 0.5`): 4.1 min; the third layer (`--layers 3`): 4.5 min (it doubles the NIC bytes; it pays only if the
per-message cost is the limit).

## 2. Build

```
cd ecalc
make                      # SHMEM=1 is the default where `oshcc` exists (aac6: OpenMPI OSHMEM); on the target set the flags:
make SHMEM=1 SHMEM_CFLAGS="-DCOMM_SHMEM $(cc --cray-print-opts=cflags)" SHMEM_LIBS="$(cc --cray-print-opts=libs) -lsma"
                          # Cray: the SHMEM headers/libs from the cc wrapper (module load cray-openshmemx); or for SOS/rocSHMEM:
make SHMEM=1 SHMEM_CFLAGS="-DCOMM_SHMEM -I$SOS/include" SHMEM_LIBS="-L$SOS/lib -lsma"
make SHMEM=0              # without the transport (COMM_TRANSPORT=shmem then aborts at start; TCP is the fallback)
```

`HIPCC`, `ARCH` (gfx942) as in the Makefile; `hipcc` links with lld, so a SHMEM library that does not carry its own
dependencies needs them on `SHMEM_LIBS` (aac6's OSHMEM: `-lopen-rte -lopen-pal`). Check the build once:
`./tests/t_comm` under `srun -N1 -n2` (§4) and `./ecalc 1000000000 /tmp/e9.txt` on one node, `cmp` against
`ref/e_1000000000.txt` (identical, 14–16 s).

## 3. Environment — every variable that matters, with the target's value and why

Transport (`comm_shmem.c`, `mn.c`; results/S.md):

| variable | target | why |
|---|---|---|
| `COMM_TRANSPORT=shmem` | set | selects the SHMEM transport (default TCP: `COMM_HOSTS`/`COMM_PORT`, the aac6 correctness path) |
| `COMM_SHMEM_SERIAL=0` | set (Cray / SOS) | one context per communicator and blocking `wait_until`; the default 1 is one process-wide lock around every library call (OSHMEM 4.1's `SHMEM_THREAD_MULTIPLE` is nominal). The code falls back to serial if `shmem_init_thread` does not provide MULTIPLE |
| `COMM_SHMEM_DEVHEAP=1` | set where the symmetric heap is device memory (Cray on the APU, rocSHMEM); 0 with a host heap | skips the `hipHostRegister` of the pool; the staging copies become D2D. Untested on aac6 (no such implementation): run `t_comm` and `t_dist` first (§4) |
| `COMM_SHMEM_POOL_MB` | **the staging the transport needs**, from `estimate.py` (the `pool` column): with S's pool-resident slabs 8192 (the default) is enough; with the staging freed per exchange ≈ 60 000 at 6 × 10¹⁰ per node (the largest single exchange: `mdb_shift`'s share/4 limbs per APU thread, 6.8 GB each way, four threads); with the transport as at 7aded87 ≈ 350 000 (every level's mesh keeps its largest exchange's staging — trap 11), which no node has | the symmetric pool holds the control blocks and the staging of every exchange (`comm_shmem.c` `staging()`: grown per communicator, never freed); the pool aborts the run when an exchange does not fit (`comm_shmem: pe r: the symmetric pool … cannot hold …`) — size it from the model, in the node's HBM whether host-registered or a device heap (`COMM_SHMEM_DEVHEAP=1`): the memory model counts it |
| `SHMEM_SYMMETRIC_HEAP_SIZE` | `COMM_SHMEM_POOL_MB` + 512 MiB (`mnrun.sh` sets it) | the library's heap must hold the pool; the name is OpenSHMEM's, Cray reads `XT_SYMMETRIC_HEAP_SIZE` too — set both |
| `COMM_SHMEM_FENCE=1` | set on a conforming implementation | orders the data before its signal with `shmem_ctx_fence` (one call) instead of `quiet`; OSHMEM 4.1.6's fence does not order nbi puts (trap 2) — verify with `t_comm` before switching |
| `COMM_SHMEM_RING_KB` | 256 (default) | the point-to-point ring per (source, dest); only small values flow through it |
| `COMM_SHMEM_TRACE=1` | debugging only | per-exchange trace on stderr |
| `MN_TOPO_GROUP` | **0** (off) until measured; then the machine's nodes per dragonfly group (64 if the cabling says so) | the third layer of the all-to-all (APU × node-in-group × group): fewer messages (1.28 M → 0.40 M per APU at 4 × 10¹⁰ × 576), twice the NIC bytes; the model says it does not pay at 2 µs per message and starts to at ≈ 15–20 µs. The tree's node groups must be contiguous in the node numbering for the groups to coincide with dragonfly groups: `--distribution=block` and a node list ordered by group |
| `MN_GROUPS` | `2,4,8,16,32,64,192,576` (the model's pick, results/Q.md §2) or `2,4,8,16,32,64,576` | the tree's level → group-size schedule (results/L.md); the code's default at 576 is `2,4,…,512,576` (a 512 + 64 join at the top). The three cost the same within the model's error (44–50 s of levels at 4 × 10¹⁰); the two explicit ones keep the six doublings inside a 64-node dragonfly group. **Check that the schedule is wired before relying on it** (§9: at 7aded87 `mn_groups_parse` is defined but `mn_tree` still walks binary levels — agent G's tree) |
| `MN_TOPO_TRACE=1` | debugging only | prints the in-group / cross-group mesh creation |

Layout and the distributed product (`rns_dist.c`, `ntt_dist.c`, `newton_db.c`):

| variable | target | why |
|---|---|---|
| `POOL_LOG` | 31 (default) | the 2³¹-point plane pools per APU (17 + 12 GiB); one process per node on the target — the aac6 values 27–29 are for several processes on one node |
| `DIST_CHUNKS` | 4 (default) | slabs in flight in the pipelined transform; the exchange of chunk k under the row pass of chunk k+1 |
| `DIST_STATS=1` | on for the calibration runs (§6), off after | per-part timing of the distributed transform: the exposed exchange time is the number that calibrates `--bw` |
| `DIST_LOGR_DELTA` | 0 (default) | the plane's R/C balance; with G's exact spills it no longer moves memory |
| `RNS_DIST_CACHE_MN` | 2 (default) | the transform cache over shares: two slots (B's piece across A's pieces, A's piece 0 across B's) |
| `RNS_DIST_CACHE_HOLD` | 0 (default) | X3 (holding Q's pieces from the reciprocal into the division): unmeasured, leave off |
| `NEWTON_MN_GROUPS` | 1 (default) | X1: the reciprocal's early doublings on the smallest prefix group by a cost rule |
| `NEWTON_MN_BW`, `NEWTON_MN_LAT`, `NEWTON_MN_FIXED` | 100, 2e-6, 0 (defaults = the target); set to the measured values after §6 | the constants of X1's rule (GB/s per APU, s per message, s per exchange); `MN_MODEL_TCP=1` is aac6's loopback set — never on the target |
| `NEWTON_MN_SPLIT` | 65536 (default) | the precision below which the reciprocal chain is replicated on every node |
| `DIST_GEN=1`, `DIST_LOGN_TEST` | tests only | force the general map at a power of two / lower the plane cap so grids form at small sizes |

Memory and the single-node pipeline (`binsplit.c`, `rns_mul.c`, `ecalc.c`, `mem.c`):

| variable | target | why |
|---|---|---|
| `ECALC_TAIL` | 1 (default) | the arena with t₁'s quarter as its reserved tail: zero `hipMalloc` inside the phases (results/M11.md) |
| `ECALC_DM_POOL` | unset (on by the size rule at ≥ 5 × 10¹⁰ per node; a no-op with the tail) | PLAN §27 row I deletes it; harmless either way |
| `RNS_PLANES_3Q30` | 0 (default) | the 3·2³⁰ planes: −3.6 s of phases for +5–6 s of mapping on aac6 (results/P.md); `auto` = on below 5 × 10¹⁰ if the target's mapping is cheaper (agent I) |
| `RNS_STRIPED_PAIR` | 1 (default) | the level-22 products paired (−0.55 s) |
| `RNS_BATCH_TILE_GB` | 15 (default) | the batch tier's tile budget |
| `ECALC_OVERLAP` | 1 (default) | the Phase 8 overlap (seeds during init, T1 during bs, the digits during the low product) |
| `ECALC_BG_THREADS`, `BS_SEED_THREADS` | 48 / default | the background OpenMP team; size to the node's cores (an MI300A node: 96 cores) |
| `ECALC_STAGING` | 1 (default) | the pinned staging sized to the seeds |
| `ECALC_ARENA_GB`, `ECALC_DM_POOL_K`, `ECALC_POOL_GROW_GB`, `BS_REGION_SLACK`, `BS_BALANCE_N`, `BS_MDEV_LOGL`, `BS_DEV_MDEV`, `BS_SEED_TERMS`, `BS_SEED_CHUNK_MB` | defaults | tuning knobs of the arena, the pool, the leaf layout; nothing on the target asks for them |
| `MN_OUT_CHUNK_MB` | 256 (default) | the writer's chunk per node; the part file streams during the low product |
| `MEM_REPORT_DEVS=1` | on for the first runs | the per-APU rows of the memory table every node prints (`mem[rank]`) |
| `ECALC_VERBOSE=2`, `RNS_VERBOSE=1`, `DB_POOL_VERBOSE=1`, `NEWTON_VERBOSE=1` | on for the smoke and calibration runs | per-level lines, per-call times, the pool's fallbacks and tail statistics, the reciprocal's steps |
| `MN_DM=host`, `MN_COMBINE=host` | never | the host-flow stand-ins (a cross-check on aac6; node 0's host cannot hold the target's numbers) |
| `LIMB_BASE` | 10 (default) | decimal limbs; `2` is the paper's binary pipeline (slower, more memory) |

Checkpoints and verification (`binsplit.c`, `mn.c`, `verify.c`, `mn_out.c`; README "Checkpoints per node"):

| variable | target | why |
|---|---|---|
| `BS_CKPT_DIR=<node-local dir>` | set (the node's NVMe; one directory per node or a shared one — the names carry the rank) | leaf sets `n<rank>_level_LLL.*` and tree sets `n<rank>_tree_LLL.*`; a leaf set is ≈ 35 GB / 4 × 10¹⁰ per node, a tree set the same; at ≈ 1 GB/s per node each set costs ≈ 30–60 s of the writer thread (hidden or not by the file system — measure at 10¹⁰, §5) |
| `BS_CKPT_MIN_LEVEL` | 16 (default), `BS_CKPT_EVERY` 4 | the leaf sets from the top levels only (a snapshot is a full pass of the pools) |
| `BS_CKPT_TREE` | 1 (default with `BS_CKPT_DIR`), `BS_CKPT_TREE_EVERY` 1 → **3** at 576 | a tree set after every level costs 10 writes; every third level plus the top (always written) is enough for a restart above the leaves |
| `BS_RESTART=1` | on a restart, with the same `BS_CKPT_DIR`, digits and base on every node | the nodes agree on the lowest complete tree level and resume above it, or each inside its leaf tree; bit-identical |
| `ECALC_CKPT_TOP=1` | size 1 only (a single-node run of the same digits) | writes the top-level P, Q as a level-0 tree set so `ECALC_RECHECK` can run without a rerun; at size > 1 the top tree set is written by `BS_CKPT_TREE` anyway |
| `ECALC_RECHECK=1` | after every large run, from the same launch line (`srun … ./ecalc <digits> <outfile>` with `ECALC_RECHECK=1`) | recomputes the digit residues from the part files, X mod q from them, P and Q mod q from the checkpointed top-level shares, the term recurrence and the T2 windows, and re-runs T1 with the run's residues (`<outfile>.t1`): RECHECK OK / FAILED per node. No pools, no computation; minutes |
| `ECALC_RES_LOG=1`, `ECALC_RES_LOG_LEVEL`, `ECALC_LEAF_DUMP=<dir>` | only when a VERIFY fails | the per-level residue log and the leaf dump of the first wrong node (results/V.md) |
| `MEM_DPOOL_FILL`, `RNS_POOL_GROW` | tests only | the zero-memory probe; the forced growth of the stress step (agent R) |
| `BS_CKPT_ABORT*` | tests only | die after a set is written |

## 4. The launch line

One process per node, four APUs per process (the process drives its APUs with four threads; no task per APU):

```
export COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1
export COMM_SHMEM_POOL_MB=8192                # the `pool` column of estimate.py for the transport's form (§3, trap 11)
export SHMEM_SYMMETRIC_HEAP_SIZE=8704M XT_SYMMETRIC_HEAP_SIZE=8704M    # the pool + 512 MiB
export MN_GROUPS=2,4,8,16,32,64,192,576 MN_TOPO_GROUP=0
export BS_CKPT_DIR=/local/ckpt BS_CKPT_TREE_EVERY=3 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1
srun -N 576 --ntasks=576 --ntasks-per-node=1 --gpus-per-node=4 --distribution=block --export=ALL \
     bash -c 'export COMM_RANK=$SLURM_PROCID COMM_SIZE=$SLURM_NTASKS; exec ./ecalc 61000000000 /out/e.txt'
```

`COMM_RANK`/`COMM_SIZE` are what `mn_init` reads for the rank and the size (under SHMEM the PE number is checked
against them); `--mpi=pmix` where the SHMEM library is launched by PMIx (OSHMEM; Cray SHMEM uses the ALPS/PMI of
`srun` directly). Per node count: `-N g --ntasks=g` with the same line — the tree's schedule and every group are
derived from `COMM_SIZE`; `MN_GROUPS` must end at or above g (anything ≥ g ends the list at g). For small g the
schedule is `2,4,…` (the default) — set `MN_GROUPS` only at 576 (or a multiple of 9 · 2ᵏ). aac6's `mnrun.sh`
does the same for several processes per node (it adds `setarch x86_64 -L`, OSHMEM's MCA variables and the heap size;
none of that is needed with a proper SHMEM, trap 1).

Sanity of the transport first, on 2 and then 8 nodes: `srun -N2 -n2 ./tests/t_comm` (all-to-all 1 B – 3 MiB,
all-gathers, alltoallv, max, sum mod q, point-to-point, the strided PE sets: VERIFY OK on every PE), then
`srun -N4 -n4 env DIST_LAYERED=1 ./tests/t_dist 24` (the layered all-to-all over 4 nodes × 4 APUs against the
one-rank engine) and `srun -N9 -n9 ./tests/t_mn_grid 0.5 27` (the any-size map at 9 nodes: 200 checks per node).

## 5. The sizes to run, in order

Every run: `cat <outfile>.part* | cmp - <reference>` where a reference exists (`ref/e_10^8`, `ref/e_10^9` in the
tree; 10¹⁰ and 4 × 10¹⁰ from a single-node run of the same digits, which is bit-identical to any size), else the
`VERIFY OK` on every node and node 0's `mn: all n nodes: VERIFY OK`, then `ECALC_RECHECK=1` (§3).

| step | nodes | digits (total) | per node | expect | what it checks |
|---|---|---|---|---|---|
| 1 smoke | 2, 3, 4, 9 | 10⁸ | 2.5–5 × 10⁷ | ≈ 10 s each, identical to `ref/e_100000000.txt` | the transport, the general map (3, 9), the part files |
| 2 | 2, 4, 64 | 10⁹ | 1.6 × 10⁷ – 5 × 10⁸ | ≈ 15–20 s, identical to `ref/e_1000000000.txt` | the pipelined exchange over real NICs (2, 4), the dragonfly group (64), `DIST_STATS=1` on: **the first calibration number** (§6) |
| 3 | 64 | 6.4 × 10¹¹ | 10¹⁰ | ≈ 1.3 min (modelled); `cmp` against a single-node 10¹⁰ run | the tree at 6 levels, the checkpoints' cost (`BS_CKPT_DIR` on), the recheck |
| 4 | 576 | 10¹² | 1.7 × 10⁹ | ≈ 1 min; VERIFY OK everywhere | the whole machine at a size where everything is small: the 9-way / 3·3 level, the PE sets at 576, the collectives. Run it with each `MN_GROUPS` of §3 and keep the faster |
| 5 | 576 | 2.2 × 10¹³ | 3.8 × 10¹⁰ | **2.0 min** modelled, node peak 395 GB | the first large run: every level product one piece, 100 GB of margin. The recheck after it |
| 6 the safe size | 576 | **3.5 × 10¹³** | **6.1 × 10¹⁰** | **3.8 min** modelled, 479 GB (the 480 GB budget) | the headline run |
| 7 the ceiling | 576 | **3.9 × 10¹³** | **6.7 × 10¹⁰** | **4.0 min** modelled, 501 of 502 GB | no margin: one allocation failure on one node ends the run — only after step 6's `mem[rank]` tables agree with the model on every node (the peak within 2 % of 479 GB) |

Between 6 and 7, `estimate.py --g 576 --D <D>` gives the peak per D in 10⁹ steps; take the largest whose modelled
peak stays below 502 GB minus the measured error of step 6. Evict the reference file from the page cache before a
timed run (`posix_fadvise DONTNEED`, RESULTS §68) if the reference lives on the node; the target's part files go to
the parallel file system (`/out`), the checkpoints to node-local disk.

## 6. What to measure first, and how to feed it into the model

The model's assumptions, in the order they matter (results/Q.md §4 has the sensitivities):

1. **The injection bandwidth per APU** (assumed 100 GB/s): step 2 at 2 and 4 nodes with `DIST_STATS=1` prints,
   per part of the distributed transform, the exchange time and the exposed part; a 2³¹-point piece over 2 nodes
   sends 8 × 2²⁸ B = 2.1 GB per APU per transform exchange — the `exchange` seconds give the GB/s per APU, the
   `exposed` seconds the hidden fraction. Feed: `estimate.py --bw <GB/s>`; `NEWTON_MN_BW=<GB/s>` in the environment
   (X1's rule); if the hidden fraction over the fabric is not ≈ 100 % of the xGMI stage, `HIDDEN_XGMI` in
   `mn_model.py` (the equal path) and `GEN_HIDE` (the general map: 3 and 9 nodes in step 1) are the constants.
2. **The per-message cost** (assumed 2 µs): `t_comm` prints the all-to-all times at 1 B … 3 MiB over 2–8 PEs; the
   1 B row over 8 PEs divided by 7 is the per-message cost of a put + signal from a host thread. Feed:
   `estimate.py --lat <s>`; `NEWTON_MN_LAT=<s>`. At 20 µs the 576-node wall rises 12 % and the third layer
   (`MN_TOPO_GROUP`) is worth one measurement at step 4 (both ways, `MN_TOPO_GROUP=0` and `=64`).
3. **The part-file bandwidth** (assumed 2 GB/s per node): step 3 prints `dc` (the writer's time) per node and the
   run's `total`; the part file is D bytes of digits per node (10 GB at 10¹⁰) — the `dc` seconds give GB/s, and
   `total − phases` says whether it hid under the low product. Feed: `estimate.py --write-bw`.
4. **The checkpoint bandwidth** (assumed 1 GB/s per node to local disk): the `mn: node r: checkpoint tree level l`
   lines print GB and GB/s; `BS_CKPT_TREE_EVERY` and `BS_CKPT_MIN_LEVEL` are the knobs if it does not hide.
5. **The mapping rate of device memory** (measured 0.06–0.10 s/GB on aac6, 12–20 s of init at 4 × 10¹⁰): init's
   `pools … s` line; the target's ROCm may differ. Feed: `PHASES[*]['init']` in `mn_model.py`.
6. **The global-link taper** (assumed 1.0): the 64-node run (step 2/3) against the 576-node run at the same D per
   node (step 4 with D = 10¹⁰: `estimate.py --g 64 --D 1e10` vs `--g 576 --D 1e10`): the levels above 64 are the
   only difference; if their exposed time exceeds the model's, `--taper` moves it.

After the first 576-node run: the per-node `mem[rank]` tables against `estimate.py --verbose` (planes, arena, top
scratch, exchange, host); the levels' times against the per-level lines (`mn: node 0 level l [g0, g1): … in s`);
the reciprocal's `dist_mn` lines (pieces per product) against the model's piece counts. The model's constants are
all at the top of `mn_model.py` and `mem_model.py` with the RESULTS section each comes from.

## 7. Checkpoints and restart in practice

- `BS_CKPT_DIR` on every node; the leaf sets from level 16 every 4 levels; the tree sets every third level and at
  the top. A 6.1 × 10¹⁰-per-node run writes ≈ 55 GB per node per set: at 1 GB/s ≈ 1 min of writer per set, hidden
  behind the next level's compute if local disk keeps up — watch the `checkpoint … GB/s` lines at step 3.
- A failed run: relaunch the same line with `BS_RESTART=1`; the nodes agree on the lowest complete tree level (each
  node removes its older sets only after every node has the newer one), and resume above it; a node with no set
  recomputes its leaf tree. The digits are bit-identical.
- The recheck (`ECALC_RECHECK=1`, same line, same `BS_CKPT_DIR`, same `<outfile>`) needs the top-level tree set and
  `<outfile>.t1` — keep both until the recheck says OK on every node.

## 8. Known traps

1. **`setarch x86_64 -L` under OSHMEM only.** OpenMPI 4.1.6's OSHMEM registers every anonymous `rw-p` mapping below
   the executable's `_end` and their count must match on every PE — with ASLR's top-down layout it does not, and
   `shmem_init` crashes in the modex about half the time; the legacy bottom-up layout fixes it (results/S.md).
   `mnrun.sh` adds it for `COMM_TRANSPORT=shmem`. Not needed on Cray / SOS / rocSHMEM; harmless if kept.
2. **`shmem_ctx_fence` does not order nbi puts on OSHMEM 4.1.6** (the receiver saw partial slabs at the signal); the
   transport orders by `quiet` unless `COMM_SHMEM_FENCE=1`. Test a conforming implementation with `t_comm` before
   switching (the 3 MiB rows would fail).
3. **`SHMEM_THREAD_MULTIPLE` is nominal on OSHMEM 4.1.6** (four threads in `wait_until` crash); hence `COMM_SHMEM_SERIAL=1`
   by default. `=0` is the target's setting — verify with `t_comm` at 8 PEs, `t_dist` layered, 10⁸ at size 4.
4. **The TCP port base** (`COMM_PORT`, the fallback transport) must stay below 32768: above it a listener collides
   with an ephemeral outgoing port now and then (`bind: Address already in use`, one process exits, the rest hang).
   `mnrun.sh` draws 20000–26000. Irrelevant under SHMEM.
5. **The page-cache rule** (RESULTS §68): a timed single-node run with the reference file in the page cache is 3–5 s
   faster in init (the seeds' input) — evict it first (`posix_fadvise(…, POSIX_FADV_DONTNEED)`); on the target the
   references are elsewhere, but the same holds for any large file the node just wrote (a previous run's part file,
   a checkpoint): the first run after a write is not the timing.
6. **The arena request at size > 1 is `binsplit.c`'s `tree_need_dev`**, which at 7aded87 sizes the top level as ONE
   transform (n = 2 N_A uncapped, q = n / (4 g)) with two g × C × 4-limb spill buffers — at 576 nodes and 4 × 10¹⁰ per
   node ≈ 440 GB per node of arena, which no node has, although `rns_dist.c` would have formed capped pieces. Agent
   G's gridded tree must re-derive it (the model's `grid` form: `mem_model.tree_need_dev`), else the target run fails
   at init above ≈ 1.9 × 10¹⁰ per node whatever the pieces do.
7. **`MN_GROUPS` is parsed but, at 7aded87, not walked**: `mn_tree` (mn.c) still forms binary levels `2^l` clipped to
   the size, and `mn_group_at` the groups `[k 2^l, (k+1) 2^l)`; `mn_groups_parse` (rns_dist.c) has no caller
   (`grep -n mn_groups_parse ecalc/*.c`). Until G's tree walks the parsed schedule, `MN_GROUPS` has no effect and the
   schedule is the binary one (which the model costs within 5 % of the others).
8. **A communicator id is used once per run** (the SHMEM mailbox row is never cleared); every group is created once
   by `mn_group_at` — a restart is a new process, so no issue; a tool that creates groups in a loop would hit it.
9. **s24-30 runs 50 % slower** than the other aac6 nodes at the large sizes (results/M11.md): a measurement note —
   on the target, take the per-node phase lines of every node from the first large run and look for outliers before
   trusting a wall.
10. **The reference for a 576-node run does not exist**; the checks are T1 (eight 62-bit primes, corrected in Phase
    11), T2 (windows) and the recheck. A single-node run of the same D per node is *not* the same number.
11. **The SHMEM transport stages every exchange through its pool and keeps the staging per communicator**
    (`comm_shmem.c`: `staging(p, send, recv)` grows `sst`/`rst` to the largest exchange seen on that communicator and
    frees them only at `comm_destroy`; the tree's level meshes live to `mn_finalize`). At 576 nodes the result
    exchange of a piece at the cap is q × 8 B = 4.3 GB per APU thread each way, so each of the ten level meshes holds
    ≈ 8.6 GB per thread: ≈ 345 GB per node (`mem_model.shmem_staging`), on top of the 8 GiB default the pool would
    abort at the first level. Agent S's Phase 12 form (the callers' slabs resident in the pool, no staging) removes
    it; failing that, free the staging in `wait()` after the H2D copy (`--staging per_exchange`: 54 GB per node at
    6 × 10¹⁰) and size `COMM_SHMEM_POOL_MB` to the `pool` column of `estimate.py`.

## 9. The variables named here exist in the code (checked 2026-09-21 on the q12 branch)

`grep -ohE 'getenv\("[A-Z0-9_]+"\)' ecalc/*.c ecalc/tests/*.c | sort -u` lists every variable the code reads; every variable named in this
file is in that list or in `mnrun.sh` / `mnaccept.sh` / the Makefile (`SHMEM`, `SHMEM_CFLAGS`, `SHMEM_LIBS`, `HIPCC`,
`ARCH`; `COMM_HOSTS`/`COMM_PORT` for TCP; `SHMEM_SYMMETRIC_HEAP_SIZE` and `OMPI_MCA_memheap_base_max_segments` are the
library's, set by `mnrun.sh`; `XT_SYMMETRIC_HEAP_SIZE` is Cray SHMEM's own; `DIST_LAYERED`/`DIST_XGMI`/`COMM_RANK`
modes are `t_dist`'s; `RNS_POOL_GROW` is agent R's stress switch of this phase — grep after the merge). The check
script: `for v in $(grep -oE '`[A-Z][A-Z0-9_]+' docs/TARGET.md | tr -d '`' | sort -u); do grep -q "\"$v\"\|\b$v\b" ecalc/*.c ecalc/*.sh ecalc/Makefile || echo "NOT IN CODE: $v"; done`.
