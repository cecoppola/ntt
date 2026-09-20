# A-ckpt — M6, per-node checkpoints of the binary-splitting phase (PLAN.md §19)

Branch `m6-ckpt` (from `main` at a75474d). Files: `ecalc/binsplit.c` (the WP7 checkpoint
functions and the level-loop hooks), `ecalc/binsplit.h`, `ecalc/mn.c` / `mn.h` (`mn_tree` only:
a tree-level save/restore and the restart-level agreement; the group, mesh and product code is
untouched), `ecalc/ackpt_test.sh` (the gate's runs). Nothing outside these files was touched;
`ecalc.c` is unchanged (the restart at a tree level is expressed through `binsplit_e` returning
empty P, Q and `mn_tree` reloading its shares).

## 1. Design

### What is saved, where

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

### When the previous set goes, and which level a restart takes

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

### Why the restart is bit-identical

The leaf restart is WP7's argument (same node table, same limbs, same `which` and level; the
device-number levels reload P, Q with the same n and limbs, and the dbig operations do not
depend on the quarter placement). The tree restart reloads the exact (n, N, g0, g) descriptors
and share limbs of every node after level L*, which is the entire state `mn_tree` carries
between levels (the group meshes are opened on first use); every later step is M3's
deterministic code.

### Test hooks

`BS_CKPT_ABORT=<leaf level>` (WP7) and `BS_CKPT_ABORT_TREE=<tree level>` exit with status 3
right after that level's set is written (before the barrier, for the tree); `BS_CKPT_ABORT_NODE=<rank>`
restricts either to one node-process, which leaves the others running or blocked in the barrier
(a `timeout` on the run kills them) — the mixed state the minimum rule is for.

## 2. Tests (jobs 20711, 20732, 20737, one node each, 2026-09-19; `ecalc/ackpt_test.sh <job> [s1 s1v1 s1dev m8 m9 mdev]`)

All runs: `ECALC_VERBOSE=2`, decimal limbs (the default), `BS_CKPT_DIR` on the node's local
`/tmp`, digits `cmp`'d against `ref/e_<digits>.txt` and against the run's own uninterrupted
run; every completed run printed `VERIFY OK` (T1, T2). Logs in `~/ntt-A-ckpt/out/` on aac6
(`summary_<job>.txt` is the script's one-line-per-run digest, `run1/` the first batch).
`BS_CKPT_EVERY=2 BS_CKPT_MIN_LEVEL=2` everywhere except the device-level runs
(`BS_CKPT_EVERY=1`), so that many sets are written and every kill lands between two of them.

### Batch 1 (job 20711, s24-30, 10:35–11:05): single node, and the multi-node leaf restarts

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

### Batch 2 (job 20732, s24-26, 14:56–15:27; e0fc9cc): the gate

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

### Batch 3 (job 20737, s24-30): m8 again, after the m8.4d port collision

All 20 runs of m8.2 and m8.4 (a, b, c→d, e→f, g→h, t→u at both sizes) again: every restart
`VERIFY OK` and identical to the reference and to (a); m8.4c→d (the pair that collided in batch
2) restarted every node from its leaf level 6 set (227 nodes) — identical. The `timeout 6`
kills of this batch landed while the tree level 1 sets were being written (all nodes had the
set: restart from tree level 1), the batch 2 ones inside the leaf tree; both kinds restart.

**Gate**: at sizes 2 and 4, 10⁸ and 10⁹, runs killed by a wall-clock `timeout` and by the exit
hooks (all nodes, one node, at a leaf level, at a tree level) restart to digits identical to
the uninterrupted run and to the reference; restart from a tree-level set at sizes 2 and 4
(levels 1 and 2); the single-node 10⁹ restart is WP7's (identical, and from `main`'s own set).

### Checkpoint sizes and write rates

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

## 3. Open issues

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
