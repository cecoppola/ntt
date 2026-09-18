# WP7 — checkpoint and restart of the binary-splitting phase

Branch `wp7-checkpoint` (from `wp1-decimal-base`). Files: `ecalc/binsplit.c`,
`ecalc/binsplit.h`, `ecalc/ecalc.c` (env pass-through and one summary line),
`ecalc/README.md`. No change to the level algorithm, the tiers, the pools or
any numerical code: with `BS_CKPT_DIR` unset the level loop is the WP3 one.

## 1. Design

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

## 2. Measurements (s24-16, job 20644, 2026-09-18 00:50–00:56, `/tmp` = local NVMe)

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

## 3. Restart correctness

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

## 4. Caveats

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
