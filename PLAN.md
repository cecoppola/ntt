# PLAN — the design of ecalc and the plan to the target run

This is the current design document: what ecalc computes, how, what is verified, and what remains before the target
run. It was rewritten on 2026-10-12. The phase-by-phase plan and status log it replaces (phases 1-17, the benchmark suite
and the paper reproduction, 2026-09-12 to 2026-10-03) is kept outside the repository as
`archive:PLAN_HISTORY_2026-10-12.md`; "PLAN §n" citations in older notes point there.

Where else to look: every environment switch with its default and evidence is in `ecalc/README.md`; open work is in
`TASKS.md`; what is in force and why is in `RESULTS.md`; agent reports are in `results/`.

Labels: **m** measured, **mod** modelled, **a** assumed.

## 1. Goal

- Compute ⌊10^d · e⌋ in decimal, for d = **3.82 × 10¹³** digits, on **576 nodes** of four AMD MI300A APUs each (user
  decision 2026-10-12). The target machine is never accessed from this project; the user runs the test kit there.
- Constraints per node: device memory ≤ **373.44 GB** (the measured `hipMalloc` edge on the target, 93.36 GB per APU, m),
  host total ≤ **480 GB** (the budget check's default). Each node has 8 NICs (two per APU) on the target and one per APU on aac7.
- The digits must be right without a reference: every run checks itself (§8).
- Standing estimate: ≈ **182 s** without the write at 3.82 × 10¹³ on 576 nodes (mod, profile `target_l4`, at an assumed
  fabric rate); device peak on rank 0 about 360.5 GB, a margin of 12.9 GB (mod) with the current launch line.
- Measured today: 6.441 × 10¹⁰ digits on one node in ≈ 76-78 s; 10¹¹ digits on one node in 129 s; 2 / 4 / 6 nodes at
  6.441 × 10¹⁰ digits per node in 139 / 182 / 294 s (m; `RESULTS.md`).

## 2. What is computed

- e = 1 + P/Q with P/Q = Σ_{k=1..N} 1/k!, P and Q formed by **binary splitting**. N is the smallest m with
  log₁₀ m! ≥ d + 50: N ≈ 3.17 × 10¹² terms at 3.82 × 10¹³ digits (computed with the code's formula).
- X = ⌊10^d · (P + Q) / Q⌋ by a **Newton reciprocal** of Q and a **two-half division** (GMP's mu_div form, "DKM").
  The product 10^d · (P + Q) is never formed: the division works on S = P + Q and the shift.
- Numbers are stored in **base-10¹⁸ limbs** (decimal limbs). X's limbs are its digits, so there is no radix conversion.
  The binary-limb pipeline of the original paper was removed on 2026-10-12 (it was 1.9-2.2 × slower, m; results/H5.md).

## 3. Arithmetic

**Transform.** Every large product is a number-theoretic transform (NTT) product modulo three or four primes
p = c · 2⁴⁴ + 1 with 3 | c (c = 240, 216, 207, 147), all below 2⁵². Lengths are 2ᵏ or 3 · 2ᵏ (a radix-3 layer, `ntt3.c`),
so a product pads to at most 1.5 × instead of 2 ×.

**Modular multiply.** FP64 Barrett with a Dekker split (`modarith.h`), the reduced-correction form inside the kernels.
About 1.31-1.34 × 10¹² modmul/s per APU (m).

**Kernels** (`ntt.c`). Tiled decimation-in-frequency forward and decimation-in-time inverse, no bit reversal:
register-blocked radix-16 passes, then one LDS pass of 2¹⁰-2¹² points. The pass plan avoids the slow strides; the
pointwise product and the scaling are fused into the inverse.

**Prime count.** A transform coefficient of a product of decimal limbs fits three primes while the shorter operand has
fewer than 58 424 467 928 terms (the three-prime bound). One-node products use three primes. Multi-node products use
`ECALC_NP=auto`: three primes, or four above the bound, which the target needs at its top levels.

**Reconstruction.** Garner CRT on the GPU (`k_crt_batch`), carries propagated into the limbs in stripes.

**P24.** Multi-node four-prime products carry 24 digits per transform point (4 limbs = 3 points) instead of 18
(`p24.h`, `p24_crt.c`): each piece is 3/4 as long (-38 % at 10¹⁰ on 2 processes, m; results/P2415.md).

**Product tiers.** Every product goes to the cheapest tier that can hold it:

| tier | used for | how the four APUs share it | file |
|---|---|---|---|
| batch | the many small products of the lower binary-splitting levels | one APU does all primes of the products it owns (or the primes striped over a tile) | `rns_mul.c` |
| dist, form B | a node's large products when one prime's planes fit an APU | prime p on APU p, no exchange | `rns_dist.c` (`b_core`) |
| dist, form C | a node's large products otherwise | four-step over the node's xGMI: each prime's plane split in row blocks over the 4 APUs, 3 all-to-alls per prime | `rns_dist.c` (`dist_core`) |
| mn | products over a group of g nodes (tree top, reciprocal, division) | four-step over 4g ranks, prime by prime, layered all-to-all | `rns_dist.c` (`mn_core`), `comm_layered.c` |

`RNS_STRATEGY=auto` (the default) picks B where its planes fit the plane pools sized at init, else C; it never allocates.

**Grids.** The largest single transform is the plane cap, 2³¹ points per node by default (`ECALC_PLANE_CAP`); for g nodes
it is 2^(31 + ⌊log₂ g⌋). A product above the cap is cut into a grid of ka × kb piece products chosen by a cost model
(`mul_grid`, `mn_grid`); pieces wholly outside the needed output range (the low product of the division, the band of the
reciprocal) are skipped. The grid shape jumps at some sizes (a "grid step", e.g. above 3.82 × 10¹³ the model wall is
about +20 s, mod; results/S52.md); the target size is chosen just below a step. Forward transforms of an operand reused
by several pieces are kept in a transform cache, sized and counted in the budget (`RNS_DIST_CACHE_FIT=1`).

## 4. Distribution

**Processes.** One process per node; it drives the four APUs through four OpenMP threads, one per APU (a process per
APU was 3.7 × slower at 8 nodes, m). "Size" counts node-processes. aac6 can run several processes on one node for tests.

**Numbers.** A device number (`dbig`, `dbig.c`) lives as four quarters, one per APU. A number sharded over a node group
(`mdb`, `mdb.h`) gives each node a contiguous share of limbs; P, Q and X are never gathered on one node.

**Four-step transform.** n = R × C: row NTTs, twiddle, all-to-all, column NTTs, with block-cyclic ownership
(point m = i + R·j) so a transform needs one all-to-all and a product three. The slab pipeline cuts each exchange into
8 chunks (`DIST_CHUNKS=8`) to overlap transfer and compute. Group sizes that are not powers of two (3, 6, 192, 576) use
the **general map** with an `alltoallv` of per-pair slabs.

**Layered exchange** (`comm_layered.c`). The multi-node all-to-all runs in four stages: an xGMI push among the node's
four APUs, a block transpose, one message per remote node over "mesh d" (APU thread d of this node to APU d of every other
node), and a transpose back. Fabric bytes equal a flat all-to-all's; messages per APU fall from 4g-1 to g-1; the on-node
quarter never touches a NIC. Two exchanges are in flight (depth 2), so one exchange's xGMI stage hides under the
previous one's fabric stage. The general map's exchange slots are counted in the node budget, shared between the two
directions and released after use (launch-line settings, §7).

**Transports.**
- `comm_xgmi.c`: the four APUs of a node (peer reads and writes over the xGMI mesh).
- `comm_ofi.c` (default wherever a cxi NIC is present): the device exchanges over libfabric, every NIC of the node, one
  domain per APU thread; × 2.1 over SHMEM at 10 nodes (m, results/OFI17.md).
- `comm_shmem.c`: OpenSHMEM (Cray OpenSHMEMX on aac7 and the target) for setup, collectives and the fallback data path
  (`COMM_OFI=0`). Multi-node builds need it (`make SHMEM_CRAY=1`).
- `comm_tcp.c`: the non-SHMEM multi-process path (aac6 tests); approved for removal in the current code reduction.

**The tree** (`mn.c`). Each node first computes its own 1/size of the terms (the leaf tree). The top levels then combine
nodes in groups following the `MN_GROUPS` schedule: by default the powers of two dividing the size, then the odd part's
prime factors in ascending order. At 576 that is 2, 4, 8, 16, 32, 64, 192, 576: two 3-way steps at the top. A 2-way level
forms P = P_A·Q_B + P_B and Q = Q_A·Q_B; a k-way level is Horner, 2(k-1) products in sequence. Each product runs over the
whole group through `mn_grid` → `mn_core`. Group structure follows the rack layout (user decision 2026-10-12); uneven last
groups are correct but not faster (results/B06.md).

## 5. The run, in order

One process per node, four APU threads. `ecalc <digits> [outfile]`.

| # | phase | what runs | why |
|---|---|---|---|
| 0 | plan | `MN_PLAN_ONLY=<digits>:<nodes>` and `BS_LAYOUT_ONLY` print the piece counts, the SHMEM pool and the node bytes without a device (login node) | the pool is sized before `shmem_init`; the layout is checked against the memory edges |
| 1 | transport | SHMEM, the meshes and the communicators open before any HIP thread exists | Cray `shmem_init_thread` segfaulted 8/78 when opened after the arenas (m) |
| 2 | pools | per APU: plane pools 0/1, then the region arena (HIP virtual memory, 2 GiB chunks) mapped in the background | nothing is allocated inside a later phase |
| 3 | seeds | the term range cut into spans of about 229 terms; each span's P, Q computed on the CPU and pulled into the arenas during init | the seeds hide under the mapping |
| 4 | self-tests | the transport and layered-exchange self-tests (size > 1) | a broken transport fails in seconds, not after an hour |
| 5 | budget check | modelled peak against `ECALC_NODE_GB` (480) and MemAvailable, all-gathered; rc 8 if over | a node dying mid-run would hang the others |
| 6 | leaf | level-synchronous binary splitting of the node's terms: batch tier per region, then the top levels as device numbers through the dist tier | an APU reads its own HBM at 3.8 TB/s against 93 GB/s remote (m) |
| 7 | tree | the `MN_GROUPS` levels as sharded products over node groups | P, Q stay sharded |
| 8 | reciprocal | μ ≈ B^(n+k)/Q to half length by an anchored Newton iteration: each node repeats the one-node chain to 2¹⁶ limbs, then the sharded doublings run on the cheapest prefix group | two products per doubling, band and middle product |
| 9 | S = P + Q | in place | memory |
| 10 | division | step 1: X_hi = top(S·10^d)·μ, then the low product X_hi·Q mod B^w and corrections; step 2 the same for X_lo; X stays sharded | the half-length reciprocal saves a doubling (-25.7 s at 10¹¹, m) |
| 11 | output | each node writes its part file(s) from a background writer started by the division (after step 1 with `MN_OUT_DKM_HI=1`), packed, O_DIRECT | the write hides under the division |
| 12 | verify | T1 and T2 (§8), joined over the nodes; deferred ±1 corrections patched into the file's tail | no reference exists at the target size |

At size 1 the same flow runs without the tree and with the dist tier in place of the mn tier.

## 6. Memory

- **Plane pools** (per APU, sized once at init from the plane cap): pool 0 holds np planes of q limbs and pool 1 3q + 16,
  with q = 2^(pool_log-2). Planes are mapped first because the last allocations near the edge get small pages.
- **Region arena → block pool.** Per APU one HIP virtual-memory range. Binary splitting places each level's numbers in
  regions (subtrees) of two alternating parities; after binary splitting the arena becomes the `dbig` block pool of the
  reciprocal and the division. A request with no contiguous free extent is met by remapping free chunks, not by copying.
  The arena's tail is reserved for the division's largest block ("hole"), plus a room of 0.16 × the hole.
- **Window rounds.** A piece product's temporary is processed in rounds of `MN_T_CHUNK_MB` per APU (code default 1024,
  launch lines 2048).
- **Budget and guard.** Before compute: the modelled peak (device, host, exchange slots) against 480 GB and MemAvailable
  (rc 8). During the run: `ECALC_MEM_GUARD_GB` stops with rc 9 when MemAvailable falls below 6 GB. rc 6 means a pool grew
  inside a phase.
- **Models.** `mem_model.py` reproduces the C layout exactly (`--check-c`); `BS_LAYOUT_ONLY` and `MN_PLAN_ONLY` print the
  layout on the login node. Measured layouts agree with the plan within 0.6 GB (335.6 vs 335.0 GB at 4 nodes, m).

## 7. Defaults and launch lines

The code defaults are the tested configuration; nothing has to be set for a one-node run. The full list, with the date
and evidence of each, is the switch table in `ecalc/README.md`.

The multi-node launch line sets, in addition: the transport (`COMM_TRANSPORT=shmem`, Cray contexts per thread, the
symmetric heap in device memory, SHMEM rounds of 1 GiB), `ECALC_NP=auto`, the transform-cache fit and partial cache,
the early write after division step 1, the lean division, `MN_T_CHUNK_MB=2048`, the X2 exchange (`COMM_LAYER_INTER2`,
`COMM_LAYER_VSLOT_POOL`) with slot sharing and release, the memory guard, the crash-trace handler and the verbose memory
lines. The two lines are kept in:

| system | line | source |
|---|---|---|
| aac7 | the `LINE` of `ecalc/e16_headline.sh`; profile `profiles/aac7.conf` | `tools/gate.sh --line aac7` parses it |
| target | `profiles/target.conf.template` (site values `__FILL__`); the filled copy stays out of git | `ecalc/target_kit.sh` |

`mnrun.sh` also sets, when unset, `FI_UNIVERSE_SIZE` (≥ 4 × tasks), `FI_LOG_LEVEL=warn` and
`FI_MR_CACHE_MONITOR=userfaultfd`; it refuses `FI_MR_CACHE_MONITOR=disabled` (silent wrong exchanges, m).

Profiles (`profiles/*.conf`, loaded by `tools/profile_load.py`, precedence: explicit environment > profile > code default)
hold what is machine-specific. Settings adopted on both lines become code defaults (TASKS 27).

## 8. Verification

Every run checks itself; a run is good only with `VERIFY OK` on every node.

- **T1.** P, Q, X, the remainder R and the digit string are reduced modulo the first eight primes above 2⁶², joined over
  the nodes, and checked against the identity X·Q + R = 10^d·(P + Q) and the term recurrence. T1 runs alongside binary
  splitting and the write; it catches any wrong limb.
- **T2.** 50-digit windows of the output recomputed independently.
- **Corrections.** The division's ±1 corrections that arrive after the writer started are applied as a patch of the
  file's tail, then re-checked.
- **RECHECK.** `ECALC_RECHECK=1` re-verifies finished files from the `<out>.t1` sidecar (and the top-level P, Q set when
  written with `ECALC_CKPT_TOP=1`); it detects a single flipped digit (tested by `mnaccept.sh`).
- **Off the clock.** `tools/unpack_digits` makes the ASCII file from the packed parts and re-checks each part's residues;
  `ecalc/digcmp.sh` compares with a reference (GMP references up to 10¹¹ digits).

**Gates for a code change.** New behaviour goes behind a switch, off by default, until the user adopts it on measured
data; digits must be identical.
- Always: `tools/gate.sh --build --nodes 1` and, for multi-node code, `--nodes 2` on aac7 (build with SHMEM, 10⁹ digits,
  VERIFY OK, digits identical).
- Engine changes: `ecalc/mnaccept.sh <hold> --only unit,e9` plus the touched steps (`mn`, `grid`, `ckpt`, `sched`,
  `recheck`, `corr`); `t_newton`, `t_mul`; 4 × 10¹⁰ identical to the reference; `mem_model.py --check-c` exact if sizing
  changed.
- Speed claims: paired ABBA rounds with a stop rule (p < 0.05, a CI bound or a round cap), reported as the mean
  difference with its 95 % CI.

## 9. Models

| model | what | use |
|---|---|---|
| `ecalc/estimate.py` | wall time by phase for a size, node count and fabric profile; `--target` prints the standing estimate | sizes, schedules, what-if |
| `ecalc/mn_model.py` | the cost model behind it (all-to-all rate per APU thread, hiding fractions, round costs) | |
| `ecalc/mem_model.py` | the memory layout per node; `--check-c` compares with the C layout | sizing, margins |
| `ecalc/calib_table.py` | run logs → one row per run; `--fit`, `--ab` (rate-adjusted paired A/B) | calibration, A/B analysis |
| `tools/groups_table.py` | `MN_GROUPS` schedules and node counts at a fixed size | schedule choice |

Fabric profiles: `target_l4` (default; the target fabric with the L4 refit, results/L4.md), `aac7` (the S48 refit,
results/S48.md). The 4-node total fits 80.8 s + 1388 / r (r = all-to-all GB/s per APU thread, m/mod; results/B07.md).
The model is calibrated on aac7 up to 6 nodes; the target's per-NIC rate and its fall-off are assumed.

## 10. Plan to the target run

**State.** The code runs and verifies at 1-6 nodes on aac7 and at one node on aac6; the 576-node layout fits with
12.9 GB of device margin (mod); the target has run the memory and exchange benchmarks (results/TGTBENCH2.md) but not
ecalc.

**Remaining work in this repository** (`TASKS.md`):
1. Code reduction (wave 2): remove the switches whose losing paths are off at the current defaults (83 approved), make the
   six launch-line settings code defaults, packed 60-bit output as the default.
2. Fix the forced-grid correction defect at 4 nodes (TASKS 23), which may be a latent scale defect in the sharded
   division with many pieces.
3. Performance on aac7 (wave 4), profile-driven, one change at a time: the distributed division multiply (+94 s at 6
   nodes against 1, m, the largest multi-node cost), `k_gather`, the init mapping.
4. Documentation that matches the code (this rewrite and a final pass after wave 2).

**Target session** (the user runs it; the steps are in `ecalc/target_kit.sh -h`):
1. Fill `profiles/target.conf` from the template (modules, ROCm, NICs, partition, memory); `tools/profile_check.sh`.
2. Kit preflight: environment record, build (`-DCOMM_SHMEM`), libfabric memory monitor (uffd), plan check
   (`MN_PLAN_ONLY` must end with `plan check ... OK`), the memory edge, a node screen (`tools/nodescreen.sh`).
3. Measurements: A3 (`comm_ofi` injection per NIC, one vs two NICs per APU) and A4 (all-to-all scaling with node count).
   These replace the assumed fabric rate in the model; re-run `estimate.py` and recheck the size against the grid steps.
4. A/B on the target of `MN_T_CHUNK_MB` 1024 vs 2048 and X2 on vs off (aac7 evidence only so far).
5. Growing runs (a few nodes, then larger groups), each VERIFY OK; then the 576-node run with the write, RECHECK and
   `unpack_digits` off the clock, and a prefix compare against a 10¹¹ reference.

**Not rehearsed on aac7** (results/A06.md): the 2-NIC A3 stage, an 8-node A4, the s2chk stage, the `--spares` path and
the plan-REFUSED path.

**Risks.**

| risk | handling |
|---|---|
| fabric rate at 576 nodes below the assumption | the kit measures it first; the size can drop to the next grid step |
| a slow or faulty node | node screen and `initchk.py` before the run; exclusion only (`MNRUN_EXCLUDE_HOSTS`), no spares; any 572-576 nodes pass the plan check |
| device memory near the edge | budget check (rc 8) before compute; guard (rc 9) during; 12.9 GB margin (mod) |
| hangs in the ROCr / libfabric memory registration | `FI_MR_CACHE_MONITOR=userfaultfd` (0/104 hangs against 22/105, m) |
| crash without a trace | `ECALC_SEGV_TRACE=1` on the launch line |
| wrong digits | T1/T2 in the run, RECHECK and `unpack_digits` after it |

## 11. Glossary

| term | meaning |
|---|---|
| APU / node / node-process | one MI300A is one APU and one HIP device; a node has 4 APUs on an xGMI mesh; one node-process drives them |
| quarter / share | a `dbig` lives as four quarters, one per APU; an `mdb` gives each node of a group a contiguous share of limbs |
| plane | one prime's residues of one operand, 8 bytes per point |
| plane cap / grid / pieces | the largest single transform; a product above it is cut into a grid of piece products |
| B / C | the one-node product forms: prime per APU without exchange (B), four-step over xGMI (C) |
| four-step / general map | the distributed transform; the general map serves group sizes that are not powers of two |
| layered exchange / mesh d | the multi-node all-to-all in xGMI and fabric stages; mesh d links APU d of every node |
| X2 | the layered exchange with two inter-node stages and pooled slots (`COMM_LAYER_INTER2`, `COMM_LAYER_VSLOT_POOL`) |
| v-slots | the general map's exchange buffers, counted in the node budget |
| P24 | 24 digits per transform point in four-prime products |
| DKM | the division in two quotient halves with a half-length reciprocal |
| T1 / T2 / RECHECK | residue check mod eight 62-bit primes / digit windows / re-verification of finished files |
| packed | part files of raw 10¹⁸ limbs behind an `ECPACK18` header with the part's residues; 0.444 bytes per digit |
| seeds / span | the leaf P, Q of about 229-term spans, computed on the CPU during init |
| region / arena / block pool | per-APU virtual-memory arena; binary-splitting regions, then the block pool of the division |
| slow mode | a per-launch drop of the inter-node all-to-all rate on aac7 (about -21 %), cause open |
