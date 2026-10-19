# CHECKPOINT — checkpoint and restart of the binary-splitting phase

Scope: checkpoint and restart of the binary-splitting phase: per-node checkpoints, hardening, restart under another MN_GROUPS. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| WP7 | checkpoint and restart of the binary-splitting phase |
| A-ckpt | M6, per-node checkpoints of the binary-splitting phase (PLAN.md §19) |
| N13 | Phase 13a agent N — checkpoint hardening (TASKS 1.7, 4.1, 1.4) |
| G6 | (TASKS 1.6 / 1.7): checkpoint restart under another MN_GROUPS / node count |
| CKPT | the recheck could not read the top-level set of the 6-node 3.8646 × 10¹¹ run |

## WP7 — checkpoint and restart of the binary-splitting phase

(former file: WP7.md)

Branch `wp7-checkpoint` (from `wp1-decimal-base`). Files: `ecalc/binsplit.c`,
`ecalc/binsplit.h`, `ecalc/ecalc.c` (env pass-through and one summary line),
`ecalc/README.md`. No change to the level algorithm, the tiers, the pools or
any numerical code: with `BS_CKPT_DIR` unset the level loop is the WP3 one.

#### 1. Design

**What a checkpoint is.** The bs phase (`binsplit_e`) is a loop over levels;
at the top of an iteration its whole state is

- the node table of the current level (`struct node` = P/Q offsets and
  normalised lengths and the region, `cur.n` entries),
- the used limbs of the four region pools — node i of a level of n nodes
  lives in region ⌊4i/n⌋ (RESULTS.md §56), the level's layout pass places
  the nodes of a region back to back, so region r's used extent is exactly
  `offr[r]` limbs from the start of its pool,
- the pool parity `which` (the two alternating level pools) and the level
  number,
- for the mdev-tier levels (the top levels at 10¹⁰ and above): the fact that
  the level's "regions" are four pointers into one host pool `g_hpool[which]`
  (`offr[r] + 2` limbs apart) instead of the four device regions.

Nothing else carries across levels (the tier is chosen from the node
lengths, `rns_st` is reset per level, the other pool is fully rewritten by
the next level's layout).

**Files.** In `BS_CKPT_DIR`: `level_LLL.hdr` — a 112-byte header (magic,
N, base, `BS_SEED_TERMS`, level, `which`, mdev-host flag, `off`, `offr[4]`)
followed by the node table — and `level_LLL.r0 … r3`, region r's `offr[r]`
limbs. Each file is written to a `.tmp` name, fsync'd and renamed, the header
last; the previous level's set is unlinked only after the new header is in
place, and the directory is fsync'd. So at any instant at least one complete
set exists: restart takes the highest level whose header is present and whose
four region files have exactly the sizes the header names. A fresh
(non-restart) run clears any `level_*` sets in the directory first.

**Copying.** The region pools are device memory (`BS_DEVICE_POOLS=1`), so
each region goes through the pinned staging buffer of its own APU
(`rns_hstage(r)`, idle between levels) in 1 GiB chunks by `mem_dev_copy`
(DMA), four regions in parallel (one OpenMP thread per region, as the seed
copy does); host pools (mdev levels, or `BS_DEVICE_POOLS=0`) are written and
read directly. Restart places the pools exactly as the loop would have
(`pool_get(which, r, offr[r] + 2)` or `hpool_get(&g_hpool[which], …)`),
reads the files back the same way, sets `bs_st.levels`, and enters the loop.

**When.** Every `BS_CKPT_EVERY` levels (default 4), from level
`BS_CKPT_MIN_LEVEL` (default 8) on, or earlier as soon as a level's pool
exceeds 1 GiB; never the top level (the loop ends there) and never the
`top_direct` mdev level (its result goes straight into P and Q).

**The rule kept.** This is a periodic snapshot, not a streamed working set:
the pools stay in device/host memory, a checkpoint is one extra
device→host→disk pass every few levels, and its cost is reported separately
(`bs checkpoints: …` line, `RESULT ecalc bs_ckpt`).

**Why the result is bit-identical.** The restarted loop sees the same node
table, the same limbs in the same pools (only limbs inside `[po, po+pn)` and
`[qo, qo+qn)` are ever read by the next level, and those are inside the saved
extent), the same `which` and level number; every later step is the
deterministic WP3 code path. The evidence is in §3.

#### 2. Measurements (s24-16, job 20644, 2026-09-18 00:50–00:56, `/tmp` = local NVMe)

Checkpoints land at levels 8, 12, 16 (`BS_CKPT_EVERY=4`, min level 8); the
top level is never written. Sizes are the four regions' used extents plus the
node table; the level pool is ≈ constant across levels, so every checkpoint
of a run has the same size. Times are the wall time of the snapshot inside
the bs phase (DMA out of the device regions through the pinned staging +
write + fsync + rename, the four regions in parallel).

| run | level | nodes | bytes | time | rate |
|---|---:|---:|---:|---:|---:|
| 10⁸ binary | 8 / 12 | 227 / 15 | 0.083 GB | 0.11 / 0.10 s | ≈ 0.8 GB/s |
| 10⁹ binary | 8 / 12 / 16 | 1987 / 125 / 8 | 0.831 / 0.830 / 0.830 GB | 0.53 / 0.80 / 0.87 s | 1.6 / 1.0 / 1.0 GB/s |
| 10⁹ decimal | 8 / 12 / 16 | 1987 / 125 / 8 | 0.889 GB each | 0.45 / 0.68 / 0.79 s | 2.0 / 1.3 / 1.1 GB/s |

Per checkpoint at 10⁹: **0.83 GB (binary) / 0.89 GB (decimal) in
0.5–0.9 s**; the three of a run add 2.2 s (binary) / 1.9 s (decimal) to a
4.1 / 4.2 s bs phase (the level loop at 10⁹ is short: 1.0 s of batch tier).
The pass is bound by the NVMe write with fsync (≈ 1 GB/s sustained), not
by the DMA (the first snapshot, absorbed by the page cache, runs at
1.6–2 GB/s). Restart load (read + DMA into the regions): 0.13–0.22 s.
The cost is printed separately (`bs checkpoints: n written, GB, s` and
`RESULT ecalc bs_ckpt`); it is inside the `bs` timer, so compare
`bs − bs_ckpt` with uncheckpointed runs.

Extrapolation to 10¹⁰ (level pool ≈ 8.6 GB binary / 9.2 GB decimal, ≈ 23
levels, snapshots at 8, 12, 16, 20): ≈ 9 s per checkpoint at the measured
rate, ≈ 36 s for four — the same order as the 10¹⁰ bs phase itself (10.6 /
18.9 s, RESULTS.md §56). At 4 × 10¹⁰ (pool ≈ 35 GB) ≈ 35 s per checkpoint
against a 48–75 s bs phase. `BS_CKPT_EVERY=8`, or `BS_CKPT_MIN_LEVEL` set to
a level near the tier change, halves or better that; the default (every 4
from level 8) is the bound the task asked for, not a recommendation for the
production run.

Not run at 10¹⁰: the borrowed node (job 20644) was limited to the 10⁸/10⁹
tests and my own allocation (20651) did not start within the session (nodes
s24-30/35 down, s24-26 partly taken, estimated start 03:39–07:00); it was
cancelled. The mdev-tier host-pool path (levels whose products exceed 2³⁰
limbs, only at 10¹⁰ and above) is therefore implemented and read, but not
exercised by a run — see §4.

#### 3. Restart correctness

For each of three configurations (10⁸ binary, 10⁹ binary, 10⁹ decimal),
four runs on the same node with `ECALC_VERBOSE=2 BS_CKPT_DIR=/tmp/wp7ckpt_<tag>`:

- (a) uninterrupted run with checkpointing (sets at levels 8, 12, 16; only
  the last remains on disk);
- (b) `BS_RESTART=1`: resumes from (a)'s last set (level 12 at 10⁸, level 16
  at 10⁹: `bs: restart from /tmp/wp7ckpt_d9b2: level 16, 8 nodes, pool 0.83 GB,
  loaded in 0.14 s`);
- (c) `BS_CKPT_ABORT=8`: the run exits (`_exit(3)`) right after writing the
  level 8 set, as a crashed run would (no digits written);
- (d) `BS_RESTART=1`: resumes from (c)'s level 8 set (1987 nodes at 10⁹),
  runs levels 9–19 including the later checkpoints, and writes digits.

All runs print `VERIFY OK` (T1: T(P+Q) == XQ + R and P, Q mod eight primes;
T2: windows, digits == X mod q). `cmp` on the digit files
(`~/ntt-wp7/out/<tag>_{a,b,d}.txt` on aac6; full log
`~/ntt-wp7/out/wp7_verify_20644.log`):

| | a vs b | a vs d | a vs ref | b vs ref | d vs ref |
|---|---|---|---|---|---|
| 10⁸ binary (`ref/e_100000000.txt`) | identical | identical | identical | identical | identical |
| 10⁹ binary (`ref/e_1000000000.txt`) | identical | identical | identical | identical | identical |
| 10⁹ decimal (`ref/e_1000000000.txt`) | identical | identical | identical | identical | identical |

(`cmp` printed nothing and returned 0 in all 15 cases.) The restarted runs
— from the last set of a complete run, and from the set of a run killed at
level 8 — are bit-identical to the uninterrupted run and to the GMP
reference in both bases. The restart also reproduces the later checkpoints
(d writes levels 12 and 16 again, same sizes), and the bs phase resumed
from level 16 takes 0.58 s instead of 4.08 s.

#### 4. Caveats

- **mdev-tier levels not exercised.** At 10¹⁰ and above the top two or three
  levels put their results in the host pool `g_hpool[which]`; the checkpoint
  header records `mdev_host`, the four region pointers are rebuilt
  `offr[r] + 2` limbs apart in the same pool, and the region files are read
  straight into host memory (same path as the device one minus the staging).
  Before relying on it for the 4 × 10¹⁰ run: one 10¹⁰ run with
  `BS_CKPT_EVERY=1 BS_CKPT_MIN_LEVEL=<first mdev level> BS_CKPT_ABORT=<that
  level>` (the level lines say `mdev`), then `BS_RESTART=1`, and `cmp` the
  digit file against an uninterrupted run's.
- A set from another run (different N, base or `BS_SEED_TERMS`) in the
  directory aborts a restart with a message; a fresh run (no `BS_RESTART`)
  first clears the directory's `level_*` files. Use one directory per run.
- Only the bs phase is covered. The last set is not deleted after bs, so a
  failure in 10dP/dm/dc can restart from the top-most set; the later phases
  are single-node work on P and Q.
- Cost: one pass device → pinned staging → NVMe at ≈ 1 GB/s with fsync; a
  4 × 10¹⁰ checkpoint (≈ 35 GB) needs ≈ 35 s and 35 GB of local disk (the
  node's `/tmp` had 1.3 TB free). `BS_CKPT_EVERY` bounds the total.
- `BS_CKPT_ABORT=<level>` is a test hook (exit 3 right after that level's
  checkpoint); inert when unset.

## A-ckpt — M6, per-node checkpoints of the binary-splitting phase (PLAN.md §19)

(former file: A-ckpt.md)

Branch `m6-ckpt` (from `main` at a75474d). Files: `ecalc/binsplit.c` (the WP7 checkpoint
functions and the level-loop hooks), `ecalc/binsplit.h`, `ecalc/mn.c` / `mn.h` (`mn_tree` only:
a tree-level save/restore and the restart-level agreement; the group, mesh and product code is
untouched), `archive:drivers/ecalc/ackpt_test.sh` (the gate's runs). Nothing outside these files was touched;
`ecalc.c` is unchanged (the restart at a tree level is expressed through `binsplit_e` returning
empty P, Q and `mn_tree` reloading its shares).

#### 1. Design

##### What is saved, where

**The leaf tree** (per node-process, `binsplit_e`): exactly WP7 — the node table of the level, the
used limbs of the four region pools (or the mdev host pool), `which` and the level number, every
`BS_CKPT_EVERY` levels from `BS_CKPT_MIN_LEVEL` on. Two additions:

- *Levels held as device numbers* (the leaf's dev_mdev top levels, `bs_keep_dev`; WP7 skipped
  them) are snapshotted too. File r holds, node by node, the limbs [pn r/4, pn (r+1)/4) of P and
  the same quarter of Q — a limb range, not a device quarter, so the reader's placement
  (`db_reserve(pn + 8)`, as `rns_mul_dist_db`'s results) need not match the writer's `qc`; the
  runs inside one device quarter are DMA'd on that quarter's device through the thread's pinned
  staging (1 GiB chunks), four files in parallel. The header's `offr[r]` is file r's limbs, so
  `ckpt_find`'s size check is unchanged. On restart the region pools (allocated by
  `binsplit_pregrow`) are donated to the block pool first, as the loop had done by then.
- *Multi-node names and identity*: with `COMM_SIZE > 1` the files are `n<rank>_level_LLL.{hdr,r0..r3}`
  so several node-processes share one `BS_CKPT_DIR`; the header is v2 (magic `ECBSCKP2`: the v1
  header followed by `size, rank, dev_nodes, kind, a0, b1, tree[10]`), and a set whose size,
  rank or term range differs from the run's aborts the restart like a wrong N. Single-node runs
  keep WP7's names and write the v1 header whenever it says it all (no device-number level), and
  read v1 sets — a set written by `main`'s ecalc restarts with this code (§3, run v1).

**The tree levels** (`mn_tree`): at the end of every tree level ℓ (a product level, or a level
carried up unchanged at a non-power-of-two size — so every node has every level) the node writes
`n<rank>_tree_LLL.{hdr,r0..r3}`: the mdb descriptors of P and Q (`n, N, g0, g, sh.n`) in the
header, the share's limbs in the four files (quarter r by limb range of P's share, then of Q's,
through the same `ckpt_dbig_io`). `BS_CKPT_TREE=0` turns it off. The top level is written too
(there is nothing after it in the node's bs; at size 2 it is the only tree level).

**Atomicity**: as WP7 — every file to a `.tmp` name, fsync, rename, the header last, the
directory fsync'd; a set is complete when its header is present and the four files have the
sizes it names.

##### When the previous set goes, and which level a restart takes

Per node the leaf sets are independent (level l's set removes l − 1's after the header is in
place, as WP7). The tree levels are collective, so the removal is deferred: a node writes level
ℓ, then all nodes meet at `mn_barrier()`, then the superseded set (tree ℓ − 1, or at ℓ = 1 all of
the node's leaf sets) is removed. Hence at any instant the *lowest* over the nodes of "my
highest complete tree level" is present on every node (a node cannot have removed level m − 1
before everyone had level m). Restart (`BS_RESTART=1`): `mn_ckpt_tree_level` all-gathers each
node's highest complete tree level over mesh 0 and takes the minimum L*, once; sets above L* are
stale and cleared.

- L* ≥ 1: `binsplit_e` skips the leaf tree (returns P = Q = ∅, `bs_Pd.n = 0`), `mn_tree` loads
  the level-L* shares and descriptors and continues at L* + 1 (at L* = the top level: straight
  to the gather).
- L* = 0: every node restarts inside its own leaf tree from its highest complete leaf set, or
  from scratch if it has none (`bs: no checkpoint for this run: full run`); nodes that had
  already finished their leaf tree recompute the levels above their last leaf set — redundant,
  bit-identical.

A fresh run (no `BS_RESTART`) clears the node's own sets (leaf and tree) first; the directory
is shared by name only.

##### Why the restart is bit-identical

The leaf restart is WP7's argument (same node table, same limbs, same `which` and level; the
device-number levels reload P, Q with the same n and limbs, and the dbig operations do not
depend on the quarter placement). The tree restart reloads the exact (n, N, g0, g) descriptors
and share limbs of every node after level L*, which is the entire state `mn_tree` carries
between levels (the group meshes are opened on first use); every later step is M3's
deterministic code.

##### Test hooks

`BS_CKPT_ABORT=<leaf level>` (WP7) and `BS_CKPT_ABORT_TREE=<tree level>` exit with status 3
right after that level's set is written (before the barrier, for the tree); `BS_CKPT_ABORT_NODE=<rank>`
restricts either to one node-process, which leaves the others running or blocked in the barrier
(a `timeout` on the run kills them) — the mixed state the minimum rule is for.

#### 2. Tests (jobs 20711, 20732, 20737, one node each, 2026-09-19; `archive:drivers/ecalc/ackpt_test.sh <job> [s1 s1v1 s1dev m8 m9 mdev]`)

All runs: `ECALC_VERBOSE=2`, decimal limbs (the default), `BS_CKPT_DIR` on the node's local
`/tmp`, digits `cmp`'d against `ref/e_<digits>.txt` and against the run's own uninterrupted
run; every completed run printed `VERIFY OK` (T1, T2). Logs in `~/ntt-A-ckpt/out/` on aac6
(`summary_<job>.txt` is the script's one-line-per-run digest, `run1/` the first batch).
`BS_CKPT_EVERY=2 BS_CKPT_MIN_LEVEL=2` everywhere except the device-level runs
(`BS_CKPT_EVERY=1`), so that many sets are written and every kill lands between two of them.

##### Batch 1 (job 20711, s24-30, 10:35–11:05): single node, and the multi-node leaf restarts

Single node, 10⁹, `POOL_LOG=29` (`./ecalc 1000000000`, sets at levels 2, 4, …, 18; the level
pool is 0.89 GB, so every set is 0.889 GB):

| run | what | result |
|---|---|---|
| s1a | uninterrupted with checkpoints: 9 sets, 8.01 GB, 7.99 s (0.89 s each, 0.8–1.5 GB/s) | identical to ref |
| s1b | `BS_RESTART=1` from s1a's last set (level 18, 2 nodes): loaded in 0.22 s, bs 0.67 s | identical to ref and s1a |
| s1c | `BS_CKPT_ABORT=8`: exit(3) after the level 8 set (0.889 GB, 0.98 s) | (no digits, as intended) |
| s1d | `BS_RESTART=1` from level 8 (1987 nodes, loaded 0.12 s), levels 9–19 with 5 more sets | identical to ref and s1a |
| v1c | **`main`'s ecalc** (`~/ntt`, a75474d) with `BS_CKPT_ABORT=8`: a v1 set (`level_008.*`, 111 368-byte header) | |
| v1d | this branch, `BS_RESTART=1` from main's set | identical to ref |

So the single-node behaviour and file format are WP7's (the v1 header is still written when a
single-node set holds no device-number level, and read either way).

Multi-node on one node, `./mnrun.sh <p> … ./ecalc <d>`, sizes 2 and 4; 10⁸ with `POOL_LOG=27`
(m8) and 10⁹ with `POOL_LOG=29` (m9). Runs per (digits, size): (a) uninterrupted; (c)
`BS_CKPT_ABORT=6` (every node exits after its leaf level 6 set) then (d) `BS_RESTART=1`; (e)
`BS_CKPT_ABORT_TREE=1` (every node exits after writing tree level 1, before the barrier) then
(f) restart; (g) `BS_CKPT_ABORT=6 BS_CKPT_ABORT_NODE=1` under `timeout 60` (node 1 exits at leaf
level 6, the others run on to their last leaf set or their tree set and are killed) then (h)
restart; (t) `timeout 6` (10⁸) / `timeout 9` (10⁹) on the whole run — a wall-clock kill in the
middle of the leaf tree, while a set was being written (`.tmp` files left behind) — then (u)
restart; (b) restart from (a)'s final tree-level sets.

| | m8.2 | m8.4 | m9.2 | m9.4 |
|---|---|---|---|---|
| a: sets written per node / bytes / time | 8 / 0.34 GB / 0.64 s | 8 / 0.17 GB / 0.58 s | 9 / 3.87 GB / 5.4 s (0.43–0.46 GB each, 0.6 s) | 10 / 2.08 GB / 4.5 s (0.2 GB, 0.45 s) |
| a: digits | identical | identical | identical | identical |
| d: restart from leaf level 6 on every node (453 / 227 / 3974 / 1987 nodes, loaded ≤ 0.03 s) | identical (= a) | identical | identical | identical |
| h: mixed — node 1 from leaf level 6, the others from leaf 14 / 12 / 16 (agreed tree level 0) | identical | (see below) | identical | (see below) |
| t: killed at | leaf level 15 (a `tree_001.r1.tmp` half written) | tree level 1 being written | leaf level 10 (`level_012.r*.tmp`) | leaf level 10 |
| u: restart from | leaf 14 | (see below) | leaf 10 on both | leaf 8 on all four |
| u: digits | identical | (see below) | identical | identical |
| b, f (restart from a tree-level set) | **refused** | refused | refused | refused |

The leaf restarts at sizes 2 and 4 are right in every combination (the same node at a lower
level than the others, a kill in the middle of a write). The tree-level restarts of this batch
all failed the identity check: `checkpoint …/n000_tree_001.hdr is from another run (N 0 …)` —
`mn_tree` has no N and the tree writer was handed 0. Fixed in 78b0b5f (`bs_N`, set by
`binsplit_e`); m8.4h and m8.4u hit the same refusal because in those runs the agreed level was a
tree level. The device-number-level runs of this batch (s1dev at `BS_MDEV_LOGL=25`, mdev at 10⁸)
exercised nothing new: at those sizes the only mdev level is the top one, which is never
snapshotted (s1devd restarted from level 18, identical); batch 2 lowers `BS_MDEV_LOGL`.

##### Batch 2 (job 20732, s24-26, 14:56–15:27; e0fc9cc): the gate

The same runs (a–h, t, u) at 10⁸ sizes 2 and 4 (`POOL_LOG=27`) and 10⁹ sizes 2 and 4
(`POOL_LOG=29`), plus the device-number levels (s1dev: single node 10⁹ `BS_MDEV_LOGL=23`, the
leaf's levels 17–19 on the device tier; mdev.2 / mdev.4: 10⁹ at sizes 2 and 4 with
`BS_MDEV_LOGL=22`, leaf levels 16–18 on the device tier, `BS_CKPT_EVERY=1`, `BS_CKPT_ABORT=16`).
Every completed run: `VERIFY OK`, digits identical to the reference and to its (a) run.

| run | m8.2 | m8.4 | m9.2 | m9.4 |
|---|---|---|---|---|
| a uninterrupted: leaf sets per node / total / time | 8 / 0.34 GB / 1.05 s | 8 / 0.17 GB / 0.65 s | 9 / 3.87 GB / 5.4 s | 10 / 2.08 GB / 5.0 s |
| a: tree sets per node (level: GB, s, GB/s) | 1: 0.044, 0.15, 0.3 | 1: 0.021, 0.06, 0.3; 2: 0.022, 0.11, 0.2 | 1: 0.444, 0.63, 0.7 | 1: 0.214–0.231, 0.56, 0.4; 2: 0.222, 0.53, 0.4 |
| a digits | identical | identical | identical | identical |
| b restart from the final tree sets (leaf skipped, straight to the gather) | identical (tree level 1, share 2 777 781 limbs, loaded 0.01 s) | identical (level 2) | identical (level 1, 27 777 781 limbs, 0.08 s) | identical (level 2, 13 888 890 limbs, 0.07 s) |
| c `BS_CKPT_ABORT=6` (all nodes exit) → d restart from leaf level 6 | identical | **port collision** (see below) | identical | identical |
| e `BS_CKPT_ABORT_TREE=1` (all nodes exit after writing tree level 1, before the barrier: leaf and tree sets both present) → f restart from tree level 1 | identical | identical | identical | identical |
| g node 1 exits at leaf level 6, the others killed at their last leaf set / tree set → h | identical (node 1 from leaf 6, node 0 from leaf 14; agreed tree level 0) | identical (node 1 from 6, the others from 12) | identical (6 and 16) | identical (6 and 16) |
| t `timeout` 6 s / 9 s (a kill in the middle of the leaf tree, `.tmp` files of the set being written left behind) → u | identical (killed at leaf 15, restarted from 14) | identical (killed while tree level 1 was written; restarted from tree level 1 — every node had it) | identical (killed at leaf 8, restarted from 6) | identical (killed at 12, restarted from 10) |

m8.4d did not start: `bind: Address already in use` / `comm_tcp: cannot connect` in `mn_init` —
`mnrun.sh`'s random port base landed on a port still held by the run killed by `timeout` just
before (batch 1's m8.4d and batch 3's re-run of the same pair were identical); not a checkpoint
matter.

Device-number levels:

| run | result |
|---|---|
| s1deva (single node, 10⁹, `BS_MDEV_LOGL=23`, `BS_CKPT_EVERY=1`): 17 sets, 15.1 GB, 13.7 s; levels 17 (4 nodes) and 18 (2 nodes) are mdev on the device tier and their sets are written (0.889 GB in 0.93 / 0.82 s — the four quarter files 222 222 2xx bytes each) | identical |
| s1devc `BS_CKPT_ABORT=17` → s1devd restart from the level-17 set (4 device numbers, loaded in 0.12 s; levels 18, 19 on the device tier again) | identical |
| mdev.2a (size 2, `BS_MDEV_LOGL=22`): leaf levels 16, 17 mdev on the device tier, sets at 15, 16, 17 (0.43 / 0.46 GB per node, 0.7 s) | identical |
| mdev.2c `BS_CKPT_ABORT=16` → mdev.2d restart from the device-number level 16 on both nodes | identical |
| mdev.4a / c / d, the same at size 4 (sets 0.20–0.23 GB per node) | identical |

##### Batch 3 (job 20737, s24-30): m8 again, after the m8.4d port collision

All 20 runs of m8.2 and m8.4 (a, b, c→d, e→f, g→h, t→u at both sizes) again: every restart
`VERIFY OK` and identical to the reference and to (a); m8.4c→d (the pair that collided in batch
2) restarted every node from its leaf level 6 set (227 nodes) — identical. The `timeout 6`
kills of this batch landed while the tree level 1 sets were being written (all nodes had the
set: restart from tree level 1), the batch 2 ones inside the leaf tree; both kinds restart.

**Gate**: at sizes 2 and 4, 10⁸ and 10⁹, runs killed by a wall-clock `timeout` and by the exit
hooks (all nodes, one node, at a leaf level, at a tree level) restart to digits identical to
the uninterrupted run and to the reference; restart from a tree-level set at sizes 2 and 4
(levels 1 and 2); the single-node 10⁹ restart is WP7's (identical, and from `main`'s own set).

##### Checkpoint sizes and write rates

- A leaf set is the level pool's used limbs (≈ constant over the levels): 0.889 GB at 10⁹ on
  one node, 0.43–0.46 GB per node at size 2, 0.20–0.23 GB at size 4; 0.04–0.05 GB (size 2) and
  0.02 GB (size 4) at 10⁸. Written at **0.8–1.5 GB/s** single-node (the NVMe with fsync, as
  WP7 measured), 0.6–0.7 GB/s per node at size 2 and 0.4–0.5 GB/s per node at size 4 when
  several node-processes write to the same disk at once (the aggregate stays ≈ 1–2 GB/s).
- A tree set is the node's shares of P and Q: 2 × N/size limbs — 0.444 GB per node at 10⁹
  size 2, 0.22 GB at size 4 (0.044 / 0.022 GB at 10⁸), at 0.4–0.7 GB/s per node; the dbig
  quarters go through the pinned staging by DMA runs like the regions, so the write is again
  disk-bound.
- Restart loads: 0.01–0.12 s for everything tested here (a 0.9 GB set in 0.12 s).
- At 4 × 10¹⁰ on `size` nodes a leaf set is ≈ 35 GB / size per node and a tree set ≈ 2 × 2.2 ×
  10⁹ × 8 / size bytes = 35 GB / size per node; at the rates above (≈ 1 GB/s per node when the
  disks are separate) each is ≈ 35 / size s. `BS_CKPT_EVERY` bounds the leaf sets; the tree
  levels are log₂ size sets.

#### 3. Open issues

- **Not run at 10¹⁰ / 4 × 10¹⁰.** Sizes and rates are measured at 10⁸–10⁹; the extrapolation
  above is by the measured per-node rate. The single-node mdev *host*-pool path (`mdev_host`,
  `BS_DEV_MDEV=0` or 10¹⁰ without device pools) is still WP7's, unexercised as in WP7 §4.
- **The tree levels write every level.** With `BS_CKPT_EVERY` bounding only the leaf sets, a
  run at size 2048 writes 11 tree sets per node (each 35 GB / size ≈ 17 MB at 4 × 10¹⁰: cheap
  there; at size 2, 17 GB per set — `BS_CKPT_TREE=0` turns them off). A `BS_CKPT_TREE_EVERY`
  would be a two-line change; the barrier per tree level (over mesh 0, all nodes) is one more
  collective per level.
- **The barrier holds the run while the slowest node writes**: the previous set is removed only
  after every node has the new one, so a node's write time is on the critical path once per
  tree level (0.1–0.6 s here). Removing the barrier would need the restart to tolerate nodes
  at different levels (recompute one level on the nodes below), not done.
- **Kill during `mn_init`**: a restart whose `mnrun.sh` port base collides with a socket still
  held by the killed run fails to connect (batch 2, m8.4d); `mnrun.sh`'s random base makes
  it rare, a retry is the fix. Not a checkpoint matter.
- **Non-power-of-two sizes**: a node without a sibling group at a level re-saves its unchanged
  shares (so every node has every level); tested only at sizes 2 and 4 (the target sizes are
  powers of two).
- `ecalc.c`'s summary line prints `restart from level <leaf level>`; a tree-level restart is
  reported by `mn.c`'s own line (`mn: node r: restart from tree level L …`) and by
  `bs: restart at tree level L: the leaf tree is skipped`. The `bs_ckpt` RESULT lines include
  the tree sets (counted into `bs_st`).
- The A-out agent's per-node output and the A-div distributed division start from `mn_tree`'s
  P, Q shares; the top tree level's set is exactly that state, so a restart of the division
  from it is possible once those phases are restartable (nothing consumes it today beyond the
  gather).

## N13 — Phase 13a agent N — checkpoint hardening (TASKS 1.7, 4.1, 1.4)

(former file: N13.md)

Branch `p13-N` (base `203733b`). Files: the checkpoint functions of `binsplit.c`/`.h`, `mn.c`'s tree-set write and restart
level, the output tail of `ecalc.c`, a new `archive:drivers/ecalc/n13_ckpt_test.sh`. Outside N's files, each minimal and commented:
`newton_db.c`/`newton.h` (a two-call hook in `newton_mn_divmod`), `mn_out.c` (one line: the recheck's top level),
`mn.h` (declarations), `README.md` (the switch rows).

**Defaults are unchanged.** A run without checkpoint switches executes no new code (the hook pointer is null, no
writer starts). All changes are in paths behind `BS_CKPT_DIR`, `BS_RESTART` or `ECALC_CKPT_TOP`, which are off by default.

#### Design

##### 1.7 — the restart trap (`MN_GROUPS`, node count)

Tree sets `n<rank>_tree_LLL.*` are indexed by the schedule's level number. Before this change a restart under another
`MN_GROUPS` read level `l` of the old schedule as level `l` of the new one. With 2,4 → 4 at size 4, the level-1 shares
(groups of 2) became the "top" of a one-level schedule and went straight into the division.

- **The header records the schedule.** Tree sets are now written with magic `ECBSCKP3`: the v2 header plus
  `struct ckpt_sched { size, nlev, gs[32] }`, which is the output of `mn_groups_parse` at write time. Leaf sets keep v1/v2
  because they do not depend on the schedule. The recheck and the restart still read v2 tree sets written before
  Phase 13: the restart accepts them with a printed warning that no schedule was recorded.
- **The check is collective.** `bs_ckpt_restart_scan` reads every tree and leaf header of this node without aborting. It
  returns the highest complete tree level and a verdict: 0 = fine, 1 = a set of another run (N, base, seeds, node
  count, rank or term range), 2 = a tree set written under another schedule. It also returns a message that names the
  file and both schedules. `mn_ckpt_tree_level` all-gathers (level, verdict). If any node objects, every node exits
  together: exit 5 for a schedule mismatch, exit 4 for another run's sets. Node 0 prints
  `mn: BS_RESTART refused: k of n nodes hold checkpoint sets in <dir> written under another schedule … Restart with the
  MN_GROUPS and node count of the run that wrote them, or start afresh`. Each objecting node prints its own
  file-level line. Before this change a foreign set made the nodes that held one call `abort()`, while nodes without
  sets waited in the all-gather.
- **A fresh run clears its old tree sets.** `mn_tree` calls `bs_ckpt_tree_clear(0)` when `BS_RESTART` is unset. This
  follows the documented intent of that function. Without it, a level that an earlier schedule wrote above this run's
  top level would survive, and a legitimate restart of this run would then be refused.
- **The recheck fix in `mn_out.c`.** The recheck assumed the top level is ⌈log₂ size⌉. With a non-power-of-two size or
  `MN_GROUPS` it silently fell back to the sidecar residues. It now uses `bs_ckpt_tree_find`, the highest complete set.

##### 4.1 — the checkpoint's wait on the division

The Phase 12 size-1 writer (`ecalc.c`, `ckpt_top_*`) waited for P's parts before S = P + Q. It then joined the thread,
including the fsync, before Q's block was released at the end of the division. At 0.31 GB/s that cost 113 s.

- **One writer for both sizes.** `binsplit.c` gains `bs_ckpt_bg_start / release(part) / done(part) / join`. The writer
  thread has its own DMA buffers of `BS_CKPT_BG_CHUNK_MB` (256 MB), so the pinned staging can be released. It also
  keeps counters of the bytes handed to the kernel (the measured rate), a cancel flag, and a count of device reads in
  flight. It holds the dbig descriptors by value, so the owner may move them: `newton_mn_divmod` does `S = *P; memset(P)`.
- **Q is released after the output stage.** After the division the driver frees Q only if the writer is past it.
  Otherwise Q's block is held (size 1: `bs_Qd`; size > 1: the hook takes `Q->sh`) and released after `out_stage`.
  VERIFY OK, the phase lines and `total` are all printed before that point. The join (fsync, then the header) follows
  and is printed as `waited … for P before S = P + Q, … for Q after the output stage, … at the join`.
- **The write is sized from the measured rate (`ECALC_CKPT_TOP=2`).** At each release, if the part is not yet
  written, the driver projects the rest of the part at the measured rate. When the projection exceeds
  `ECALC_CKPT_TOP_SLACK` (1 s) less the time already waited, the set is dropped. The driver sets cancel and waits
  until no device read is in flight (at most one DMA of one chunk). Writer threads stop before their next read, and
  their temporaries and any region file already renamed are removed. No header is written, so no partial set remains.
  The line reads `checkpoint: the top-level set … ABANDONED (ECALC_CKPT_TOP=2): x of y GB handed over in t s (r GB/s
  measured); the P part projected … s more against a slack of 1.0 s`.
  - `ECALC_CKPT_TOP=1` keeps its meaning: the set is always complete, and the driver waits for P if it must. Only
    the Q wait moves past the output stage.
  - Why a budget rather than a better ordering: P must be on disk before S = P + Q overwrites it, and the reciprocal
    gives about 16 s at 4 × 10¹⁰. P is 17.8 GB, so it fits only at ≳ 1.1 GB/s. At 0.31 GB/s P alone needs 57 s
    (**modelled**), so no ordering hides the set on that disk short of keeping an extra 17.8 GB copy, which the memory
    budget forbids. The budgeted mode therefore drops the set on a slow disk instead of stalling the run.
- **Rewrites and failures.** A tree set being rewritten (a rerun into the same `<outfile>.top`) loses its old header
  before the new region files land, so an old header can never describe new files. A failed or dropped set leaves no
  files.
- **Test knob `BS_CKPT_BG_MBS`** caps the writer at an emulated disk rate for all four files together. It lets aac6's
  0.31 GB/s path be reproduced on any node.

##### 1.4 — the synchronous top set at size > 1

`mn_tree` starts the same background writer for the top level (`mn_ckpt_bg_mode`, set by `ecalc.c` in the distributed
division path; `BS_CKPT_TREE_BG=0` restores the synchronous write). The shares are read while the reciprocal and the
division run. `newton_mn_divmod` calls `newton_mn_pq_hook(0, P)` before S = P + Q, which releases P. It calls
`newton_mn_pq_hook(1, Q)` before `mfree(Q)`; when the writer still needs Q, the hook takes `Q->sh`.
`mn_ckpt_top_finish()` runs on every node after `out_stage` and before the block pool goes. It releases Q, joins the
writer and all-gathers whether every node has the set. Only then does it let `mn_finalize` remove the superseded sets
(the C6 rule), so a node whose set failed or was dropped keeps everyone's lower sets. `BS_CKPT_ABORT_TREE` at the top
level joins the writer before `_exit(3)`.

#### Tests

`archive:drivers/ecalc/n13_ckpt_test.sh <job> [--only restart,bg,budget,time]` runs on one node from the login node. Logs go to
`results/n13/<job>/` and there is one PASS/FAIL line per test.

All results below are **measured**. They come from job 21017 on s24-26 at `6392f78`, on the node-local NVMe `/tmp`. The
later commits are docs plus `a7884f0`, which changes only the rate printed on a dropped set's line: the rate is now
measured up to the drop instead of up to the join.

##### Gate

| test | command / configuration | result |
|---|---|---|
| **1.7 the trap, before** | `ecalc.base` (the base commit): 10⁸ at size 4, `BS_CKPT_ABORT_TREE=1` under the default schedule 2,4, then `BS_RESTART=1 MN_GROUPS=4` | every node restarts "from tree level 1" as if it were the top: **segfault on 3 of 4 nodes (rc 139), digits DIFFER** |
| **1.7 schedule mismatch** | the same sets, this branch, `MN_GROUPS=4` | **refused on all 4 nodes, rc 5, in 4 s**: `mn: BS_RESTART refused: 4 of 4 nodes hold checkpoint sets in … written under another schedule`; each node names the file and both schedules |
| 1.7 node-count mismatch | the size-4 sets restarted at size 2 | refused, rc 4, in 3 s (`… is from another run (… 4 node-processes …)`), no hang |
| 1.7 matching restart | the size-4 sets, default schedule | identical to `e_100000000.txt`; all 4 nodes VERIFY OK |
| **1.7 gate: 10⁹ size 2** | `POOL_LOG=29 BS_CKPT_ABORT_TREE=1` (the top set, written by the background writer, joined before the exit) → restart at size 4 refused (rc 4) → matching restart at size 2 | **identical** to `e_1000000000.txt`, restart from tree level 1, total 23.6 s |
| 1.4 background top set at size > 1 | mnaccept `recheck e8 size 2` (`ECALC_CKPT_TOP=1`, now the background writer) | run identical; recheck: 3 RECHECK OK with P, Q from the set on both nodes; corrupted copy RECHECK FAILED. The writer's line: 0.04 GB, waited 0.00 s for P, 0.00 s for Q, 0.00 s at the join |
| **regression** | `./mnaccept.sh 21017 --only unit,e9,ckpt,recheck` | **13 passed, 0 failed** (1263 s): 8 unit tests, e9 size 1 in both bases identical, ckpt e8 size 2 identical, recheck e9 size 1 and e8 size 2 |

##### 4.1 timing at 4 × 10¹⁰, size 1, disk emulated at 310 MB/s (aac6's slow path, `BS_CKPT_BG_MBS=310`)

The emulation plus the real disk gave an effective **0.22 GB/s**. Digits were identical to `results/e_4e10.out` in
every run, and the reference was evicted from the page cache before each run.

| configuration | total (to VERIFY) | dm | wait for P before S = P + Q | wait for Q | set | exit |
|---|---|---|---|---|---|---|
| Phase 12, `ECALC_CKPT_TOP=1` at 0.31 GB/s (RESULTS §77, job 20964; **measured then**) | 164 s | 113 s | — | division waited for the whole set | kept | — |
| this branch, `ECALC_CKPT_TOP=1` at 0.22 GB/s | **128.2 s** | 25.8 s | **45.6 s** | **0.00 s** after the division; 10.65 s at the join, **after** VERIFY | kept (35.56 GB in 162.9 s) | 222 s |
| this branch, `ECALC_CKPT_TOP=2` (budgeted, slack 1 s) at 0.22 GB/s | **79.6 s** | 26.3 s | 0.00 s: dropped at the P release (projected 56.5 s more) | 0.00 s | **dropped**, no files left | 160 s |

What this shows:
- **Q is no longer waited for.** The Phase 12 wait on Q at the end of the division is gone; what remains of it is 10.7 s
  at the join, after VERIFY.
- **P's wait is structural at this rate.** P's 17.8 GB must be on disk before S = P + Q, and the reciprocal gives about
  16 s. At 0.22–0.31 GB/s, `=1` waits about 45 s for P. That is why `=1` still costs 128 s against about 80 s.
- **`=2` never stalls.** The run is as fast as one without a set: 79.6 s against the ~80 s default series. The set is
  lost on a disk that slow.
- The two runs used different disk rates: 0.22 GB/s emulated here, 0.31 GB/s real in Phase 12.
- **Not measured, for lack of node time:**
  - the same pair on the node's real disk, where W measured 1.1–1.6 GB/s and `=1` should wait about 0 s (**expected**,
    not measured);
  - `ecalc.base` against this branch on the same disk;
  - the `budget` and `bg` sections of `n13_ckpt_test.sh` at 10⁹ (the emulated drop at size 2, and size 3 with the recheck).

Recommendation (the decision is the user's): `ECALC_CKPT_TOP=2` is the safe default candidate. It keeps the set wherever
the disk can take it under the reciprocal (≳ 1.1 GB/s at 4 × 10¹⁰, **modelled**) and costs ≤ 1 s elsewhere. The default
is still off.

#### Open issues

- **Memory while Q is held.** With `=1` or `=2`, holding Q past the division (only when the writer still needs it) keeps
  17.8 GB at 4 × 10¹⁰ in the block pool through the output stage. That fits at 4 × 10¹⁰; it has not been checked at 10¹¹.
- **Size > 1 was tested only at small digit counts.** The background top set there was tested at 10⁸ and 10⁹, where the
  write takes 0.1 s, so the P and Q waits were never exercised at scale. The hook path is covered only by correctness,
  not by timing.
- **Old v2 tree sets.** A v2 tree set (written before Phase 13) has no recorded schedule. A restart accepts it with a
  warning.
- **Behaviour changes outside the default, which the integrator should know about:**
  - A fresh run with `BS_CKPT_DIR` at size > 1 now deletes its own earlier tree sets there.
  - A failed synchronous tree-set write now removes its partial files.
  - The size > 1 top set is written in the background whenever one is written (`BS_CKPT_TREE_BG=0` restores the
    synchronous write).

#### Files outside N's list

- `newton_db.c` / `newton.h`: `newton_mn_pq_hook`, 5 lines. It is null unless `mn_tree` started a writer.
- `mn_out.c`: one line. The recheck takes the highest complete tree level instead of ⌈log₂ size⌉.
- `mn.h`: declarations only.
- `README.md`: switch rows.

#### RESUME

Done. Gate met at `6392f78` (job 21017, cancelled); branch head `a7884f0` + this file. Nothing is live.

## G6 (TASKS 1.6 / 1.7): checkpoint restart under another MN_GROUPS / node count

(former file: G6.md)

Date 2026-10-12. Branch `g6` (off main a448d73). Labels: measured / modelled / assumed.

#### Finding: the check already exists (Phase 13 N, results/CHECKPOINT.md §N13, A05.md); this task adds the automated tests
- Tree sets are written as `ECBSCKP3`: the v2 header plus the node count and the `MN_GROUPS` level list (`ckpt_sched` in `ecalc/binsplit.c`).
- `bs_ckpt_restart_scan` compares them on every node; `mn.c` then makes all nodes fail together: schedule mismatch exits rc 5, node-count or
  other-run mismatch rc 4, with a FATAL-style line `mn: BS_RESTART refused: k of n nodes hold checkpoint sets in <dir> written under another
  schedule ...` and per node a line naming the file and both schedules.
- Old v2 sets (no schedule field) are accepted with a warning (`records no schedule ... assumed to match`), as asked.
- No change to `ecalc/` code: digits and timing are untouched (nothing in the compute path changed). Not behind a switch (it is a check).
- 1 node: no schedule exists (`mn_groups_parse` returns 0 at size 1; `mn.c` skips the scan at size <= 1), so the pair needs >= 2 processes.

#### Added: `mnaccept.sh --only sched` (10^8 at 4 node-processes; `MNACCEPT_SCHED_PROCS`, `MNACCEPT_SCHED_ALT` override)
1. write: default schedule 2,4, `BS_CKPT_ABORT_TREE=1` (exits after the tree level 1 set);
2. negative: `BS_RESTART=1 MN_GROUPS=4` must be refused (rc != 0, "written under the schedule" message, no VERIFY OK, set files untouched);
3. negative: restart at 2 processes on the 4-process sets must be refused ("BS_RESTART refused");
4. positive: `BS_RESTART=1 MN_GROUPS=2,4` (same schedule) restarts, digits identical to the reference, no refusal.
The old `ckpt` and `recheck` steps are unchanged.

#### Tests (filled in from the drivers' summaries; see Results below)
- aac6, one PPAC_MI300A_SPX hold (45 min): `tools/gate.sh --build --nodes 1 --line none`, then `mnaccept.sh --only ckpt,sched,recheck`.
- aac7 queue item `76_g6_gate.sh` (2 nodes): `tools/gate.sh --build --nodes 2` (SHMEM build, `-DCOMM_SHMEM` checked), then
  `mnaccept.sh --only ckpt,sched` (ckpt = the real 2-node abort/restart; sched = 4 processes, 2 per node, so the schedules 2,4 and 4 differ).
  At exactly 2 nodes `MN_GROUPS` cannot differ in effect (any value >= 2 gives the schedule [2]); hence sched uses 4 processes.

#### Results
aac7 (queue item 76, 2-node hold, 2026-10-11 ~23:45-23:54 ET; measured; marker `DONE`):

| test | result |
|---|---|
| `tools/gate.sh --build --nodes 2`, 1e9 | PASS: VERIFY OK, digits identical, segv 0, ecalc total 19.95 s; build log has 2 `-DCOMM_SHMEM` lines |
| `mnaccept` `ckpt` (2-node abort at leaf level 6, restart, 1e8) | **FAIL as scored by the script**: "3 nodes exited; restart rc 0; identical". The restart itself succeeded (rc 0) and the digits are identical to the reference; the failing test is the script's count of abort messages (3 matching lines where it expects 2 at 2 nodes), the extra line is the `config:` header, which lists the set variable `BS_CKPT_ABORT=6` (measured in the aac6 raw log: 3 lines match `BS_CKPT_ABORT`, 2 match the exit message). Not a restart defect. Fixed: the counter now matches `BS_CKPT_ABORT: exiting after the level` (exactly the 2 exit messages, one per node; the test is not weakened: still requires 2). Verified by replaying on the saved aac6 log (3 -> 2) and `bash -n`; no rerun needed |
| `sched`: refuse `MN_GROUPS=4` at size 4 sets | PASS: rc 5, 4 node messages naming both schedules, sets untouched |
| `sched`: refuse another node count (size 2 on size 4 sets) | PASS: rc 4, "BS_RESTART refused: 2 of 2 nodes hold checkpoint sets ..." |
| `sched`: same `MN_GROUPS` restarts (size 4) | PASS: identical; "node 0: restart from tree level 1" |

Reading: the new `sched` tests (what G6 adds) pass 3 of 3 on 2 nodes / 4 processes: a restart under another schedule or node count is refused with a message naming both schedules, and the same schedule restarts bit-identically. The `ckpt` FAIL is a test-script counting issue in a test that already existed (see the Finding above), to be fixed before the suite is called green. The aac6 summary (`~/g6out/`) was not collected here. No step left in the hold (only `batch` / `extern`).

##### aac6 (hold 22987, 1 node, 2026-10-12 ~01:40-01:42 CT; measured; `~/g6out/summary.txt`)

| test | result |
|---|---|
| `tools/gate.sh --build --nodes 1 --line none`, 1e9 | PASS (VERIFY OK, digits identical, segv 0, ecalc total 11.53 s) |
| `mnaccept` `ckpt` e8 size 2 | scored FAIL ("3 nodes exited; restart rc 0; identical"): the known counter false FAIL (the `config:` line also names `BS_CKPT_ABORT`), already fixed on g6 (89373fa); the restart itself succeeded with identical digits |
| `sched` refuse `MN_GROUPS=4` (size 4) | PASS (rc 5, 4 messages naming both schedules, sets untouched) |
| `sched` refuse node count (size 2 on size 4 sets) | PASS (rc 4) |
| `sched` same `MN_GROUPS` restarts (size 4) | PASS (identical; restart from tree level 1) |
| `recheck` e9 size 1 / e8 size 2 | PASS / PASS (RECHECK OK from the checkpoint; corrupted copy gives RECHECK FAILED) |

The hold ran from `batch`/`extern` only at the end (no step of ours left).

#### RESUME
Collect both summaries (bounded `cat`), paste the PASS/FAIL lines above, commit, then the coordinator merges. If `sched` fails on the
write step, check that `BS_CKPT_ABORT_TREE=1` writes a tree set at 4 processes without `BS_CKPT_EVERY`/`BS_CKPT_MIN_LEVEL` (N13.md did so at 10^8 size 4).
Remove the aac6 clone `~/ntt-g6` and `~/g6.bundle` after merge.

## CKPT — the recheck could not read the top-level set of the 6-node 3.8646 × 10¹¹ run

(former file: CKPT.md)

**Question.** After the 6-node 3.8646 × 10¹¹ run with `ECALC_CKPT_TOP=1` (2026-10-13, VERIFY OK, first 10¹¹ digits
identical), `ECALC_RECHECK=1` on its files printed `bs: checkpoint: short read from <out>.top/n00X_tree_002.rK` for 16 of
the 24 files (all 6 nodes, tree level 2), fell back to the sidecar's P, Q residues, and was killed at its 1800 s timeout.
Was the set written incompletely, read with the wrong size or layout, or is it the file system? Why so slow?

**Change / branch.** Branch `ckpt` (from `main` at 39346d0). Only the top-set writer and reader use `sp_file`
(`ecalc/spill.c`), so nothing else changes:
- `pread_need`: one `pread` asks for at most 64 MiB (`SP_READ_MAX`; it asked for up to 512 MiB); EIO / ETIMEDOUT / EAGAIN /
  ENOMEM: the same read again after 1, 4, 16 s, said on stderr (3 failures in a row fail it); an O_DIRECT read that
  comes back short at a partial block before the bytes needed is read again from that block (it was taken as the end
  of the file).
- `pwrite_full`: the same retry on those errors (no size cap: the writes did not fail).
- `ecalc/binsplit.c`: a failed read names errno, the offset, the bytes got / needed and the file size.
No switch: a correctness fix of the reader (reads that come back whole return the same bytes as before).

**Method.**
1. Code and logs: the writing run's `checkpoint: the top-level P, Q ... 57.25 GB in 1027 s` on every node (the header is
   written only after all four files close without error); the recheck found the level-2 set through `ckpt_find_kind`,
   which checks each file's size against the header — so the files were complete and the sizes right.
2. Queue item `40_ckpt_repro` (2 nodes; a diagnostic build = main + the named failure): a write with `ECALC_CKPT_TOP=1`,
   each file's size against its header, then `ECALC_RECHECK=1` on the same files; 10¹⁰, then 2 × 10¹⁰.
3. Item `41_ckpt_mount` (1 node): the compute node's mount options of the home file system.
4. Items `42_ckpt_gate` (16 MiB cap) and `43_ckpt_gate2` (64 MiB, the merged state): `tools/gate.sh --build --nodes 2`
   and `--nodes 1` (`--line aac7`), then the 2-node write + recheck at 2 × 10¹⁰.

**Results.**

| run | set per node | files vs header | recheck | part-file pass | top-set read | label |
|---|---|---|---|---|---|---|
| 2 nodes, 10¹⁰, main + diagnostics | 4.44 GB, 4 × 1.1 GB | 8/8 equal | RECHECK OK, 0 short reads, 281 s | 164–170 s (12.5 MB/s per node) | 102–108 s (41–44 MB/s) | m |
| 2 nodes, 2 × 10¹⁰, main + diagnostics | 8.89 GB, 4 × 2.2 GB | 8/8 equal | RECHECK OK, 0 short reads, 527 s | 332–336 s (12.5 MB/s) | 180–185 s (48–49 MB/s) | m |
| 2 nodes, 2 × 10¹⁰, 16 MiB cap | 8.89 GB | — | RECHECK OK, 0 short reads, 0 retries, 679 s | 317–370 s | 300–353 s | m |
| 2 nodes, 2 × 10¹⁰, 64 MiB cap (merged) | 8.89 GB | — | RECHECK OK, 0 short reads, 0 retries, 693 s | 480–481 s | 203–204 s | m |
| 6 nodes, 3.8646 × 10¹¹ (2026-10-13) | 57.25 GB, 4 × 14.3 GB | equal (found by the size check) | 16 of 24 files short read, killed at 1800 s | | | m |
| 6 nodes, 3.8646 × 10¹¹, the recheck's need | | | ≈ 1 h at the 2-node rates (26.8 GB of parts at 12.5 MB/s ≈ 2140 s + 57.25 GB at 48 MB/s ≈ 1190 s); slower when 6 nodes share the server | | | mod |

Gates: 16 MiB (commit e339948) and 64 MiB (commit 544b863): 2 nodes PASS (10⁹, VERIFY OK, digits identical, P24 in use) and
1 node PASS, both times (m). Single runs on a shared file system (other queue items ran beside them): the part-file pass,
whose code did not change, took 317–481 s across the three 2 × 10¹⁰ rechecks, so read times compare only roughly; 16 MiB
read the set ≈ 1.7 × slower than 512 MiB while the part pass was unchanged, 64 MiB ≈ 1.1 × (m, one run each, no CI).

The cause: the compute nodes mount the home file system NFS **soft** (`timeo=600,retrans=2`, NFSv3, rsize 64 KiB; the
login node mounts it hard). A soft mount fails an RPC that gets no answer within its timeouts with EIO. The reader had
four threads per node, each with one O_DIRECT `pread` of up to 512 MiB in flight: 2 GiB per node, 12 GiB over 6 nodes,
against a server that delivered ≈ 0.3 GB/s in all (the 6-node write: 56 MB/s per node, m) — about 40 s of queue on
average (mod), so the tail of the RPCs passed the 60 s timeout and the reads failed with EIO, which the old message
called a short read. At 2 nodes the queue is a third of that and nothing failed. With 64 MiB requests the 6 nodes have
1.5 GiB in flight, about 5 s of queue (mod). The nodes of the large run had been rebooted, so their kernel logs could
not be checked: the mechanism is inferred (mount options, load), not seen.

The 30 minutes: the recheck reads every part file (26.8 GB per node at 3.8646 × 10¹¹, packed) and then the top set
(57.25 GB per node); the recurrence over the node's terms takes about a second (1.1 s at 2 × 10¹⁰, m; ≈ 7 s at
3.86 × 10¹¹, mod). The sidecar fallback adds nothing: the time is the file system. 1800 s could not have been enough.

**Verdict.** A bug in the reader (it trusted one huge read on a soft-mounted NFS), not in the written set: fixed by
smaller reads plus retries, merged as a correctness fix. The recheck of a large run needs a timeout of ≥ 3 h on aac7
(TESTING.md §3).

**Open issues.**
- Not reproduced at scale: the fix is checked at 2 nodes (no failure before or after); the proof at 6 nodes is the next
  large run's recheck with a ≥ 3 h timeout (`spill: read at offset ...` lines would show retries).
- The part-file pass reads at 12.5 MB/s per node, a quarter of the top set's O_DIRECT rate: the recheck's main cost
  (a candidate for TASKS "Proposed").
- The writer waited 499 s for P before S = P + Q at 3.8646 × 10¹¹ (the 57 GB set at 56 MB/s per node): `ECALC_CKPT_TOP=1`
  costs run time on this file system; `=2` (budgeted) drops the set instead.

**RESUME.** Merged to `main` (branch `ckpt`, last code commit 544b863). Confirmed at scale on 2026-10-14 (section
below; commit 34175cf). Nothing open for this item; the part-file pass rate is a TASKS "Proposed" candidate.

#### 2026-10-14 proof at scale

The same 6-node 3.8646 × 10¹¹ digits as on 2026-10-13, with the fix (commit 34175cf, `ECALC_CKPT_TOP=1`, 6 nodes,
one run each, measured):

| step | result | label |
|---|---|---|
| write (the whole run's time) | 1473.26 s total; VERIFY OK; first 10¹¹ digits identical to the reference | m |
| standalone `ECALC_RECHECK=1` on the written files | RECHECK OK on all nodes (7 OK lines, 0 FAILED) | m |
| `spill: read at offset` retries during the recheck | 0 | m |
| recheck wall time | 7380 s (2 h 3 min) | m |
| recheck, model of 2026-10-13 (≈ 1 h at the 2-node rates) | ≈ 3330 s (2140 s parts + 1190 s set) | mod |

The recheck took about 2.2 × the model, so the model is low for this size: the file-system rates at 6 nodes are
lower than the 2-node rates it was built on (mod). The fix holds at this size: the 16 short reads of 2026-10-13 did not
recur, and no read needed a retry.

TESTING.md §3 asks for a recheck timeout of at least twice the model. Twice the model is 2 h, which the measured
2 h 3 min would exceed, so TESTING.md §3 keeps ≥ 3 h for the 6-node large run.
