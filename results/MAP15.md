# MAP15 — Phase 15 Batch 2, row 5 (N2): the arena's mapping, the seeds and level 1

Agent MAP, branch `p15-MAP` from `int15d` 3e0a0db (= main B1 f184d51 + NP + AS + N3x, all off by default). aac6 clone
`~/ntt-MAP15`, logs `~/MAP15/<batch>/` (copied to `results/MAP15/`). Times Eastern (aac6 logs are Central: +1 h). Numbers are
labelled **measured** (a run's log), **modelled** or **assumed**.

## 1. Feasibility note (written first, PLAN §36.5; 2026-09-27 22:55 EDT)

**The measured timeline at 10¹¹** (job 21659, s24-30, 2add00f = int15d + `ECALC_INIT_TL=1`, without the digit file; seconds since
`rns_init` began; `results/MAP15/f1/`). Arena chunks are 2 GiB; "p0" = the parity-0 half (the seeds' target, mapped inside
`binsplit_pregrow` by four threads, one per APU), "p1" = the parity-1 half (level 1's outputs; mapped by the four background threads
after `rns_init`, one chunk at a time under the global mapper lock), "rest" = the dm extra past the halves.

| run | planes done | p0 mapped (64 chunks) | seed thread: 2 chunks done → waits for the pools | bg: p1 + rest | seeds end | level 1 starts | level 1 done |
|---|---|---|---|---|---|---|---|
| d0 (int15d defaults) | 6.88 | 7.25 → 19.13 (**0.186 s/chunk**) | 12.22 → 19.14 (**6.92 s idle**) | 19.41 → 33.62 (66 chunks, 0.215 s/chunk) | 35.49 | **35.96** | 40.74 |
| r0 (`BS_ARENA_ROOM=0.16`) | 7.46 | 8.28 → 18.48 (0.159) | 12.18 → 18.48 (6.30) | 18.61 → 34.67 (71, 0.226) | 34.62 | **35.07** | 39.50 |
| t96 (room + `BS_SEED_THREADS=96`) | 6.54 | 7.09 → 16.96 (0.154) | 12.06 → 16.96 (4.90) | 17.10 → 31.28 (71, 0.200) | 36.01 (spans 19.94 s vs 16.63) | 36.46 | 41.22 |
| aft (room + `ECALC_SEED_ORDER=after`: no seeds during p0) | 6.85 | 7.38 → 15.78 (**0.131**) | — (seeds from 17.86, in bs) | 15.90 → 32.19 (71, 0.229) | 37.19 | 37.90 | 42.49 |

(all measured). Per chunk, the background mapper holds its lock 0.15–0.24 s: `hipMemCreate` 0.12–0.16 s, `hipMemSetAccess`
≈ 0.058 s, `hipMemMap` and the zeroing ≈ 0; the four background threads queue 9–12 s each for the lock, and the lock is not fair
(one APU maps 4–7 chunks in a row while another waits). In p0 the four threads call the runtime at once and the call times overlap
(the runtime serializes them: per-APU sums of 20–36 s for an 8–12 s phase).

**What binds.** Level 1 needs p0 and p1 of every APU: **124 chunks** (266 GB) of the 129–135 (the rest, 5–11 chunks, is the dm
extra, first used after bs). The seeds end at 34.6–36.0 s and the p1 mapping at 31.3–34.7 s: both are on the critical path, 1–4 s
apart. The seed thread's own work is ≈ 20 s (buffers 1.4–1.6 + spans 16.6–17.1 + DMA and issue ≈ 2.6), but it starts at ≈ 7 s and
**sits idle 4.9–6.9 s waiting for all four p0 halves**; the mapping runs at 0.13 s/chunk when the CPU is free and 0.16–0.23 s/chunk
while 192 seed threads compute.

**What can overlap** (modelled from the measured parts):
1. **The seeds need only the chunks they write, in order** (region 0's first 8 GiB, then the next …). Mapping p0 in the seeds'
   order and letting each DMA wait for its own chunks removes the 4.9–6.9 s idle wait: the seed thread's work (≈ 20 s from ≈ 7 s)
   ends at **≈ 27 s**.
2. **One mapping stream in need order**: p0 in the seeds' order, then p1 of all APUs, then the rest — started right after the
   planes (`RNS_PLANES_FIRST` kept), without the main thread blocking in `binsplit_pregrow`, several chunks per call and the four
   APUs' calls issued together as in p0 today (0.13–0.16 s/chunk against the background's 0.20–0.23). 124 chunks from ≈ 7 s:
   **≈ 23 s (uncontended rate) … 27 s (0.16 s/chunk)** for level 1's chunks.
3. **The seed threads (b)**: 96 threads make the spans ×1.2 slower in the run (16.6 → 19.9 s, measured) but the mapping ×1.1–1.2
   faster; which wins depends on which of 1 and 2 binds after the change — measured, both ways.
4. **Level 1 waiting for its own chunks** (not the whole p1 half) overlaps level 1's 4.9–5.2 s with the last chunks: up to ≈ −2…−3 s
   more; needs level 1 split into sub-batches in output order (a small edit in the level loop, outside the seed code: named); risk:
   the mapping calls delay level 1's launches (RL: the runtime serializes its VMM calls). Built second, only if 1–2 leave the
   mapping on the critical path.

**Expected gain**: level 1 from 35.1–36.5 s to ≈ 27–30 s: **−5…−8 s at 10¹¹** (modelled), the same order at the target's top node
(its init + seeds have the same structure, V3 §1; assumed). With `BS_ARENA_ROOM=0.16` the same (its extra chunks are in "the rest").

**Risks**: a DMA into a chunk not yet mapped is a device fault (every seed DMA waits for its chunks: the wait is the existing
`db_vmm_arena_wait`, which is a no-op when the chunks are there); the budget check and `mem_report` count the arena by its
registered range (unchanged); the remap path already waits for the whole arena. Digits cannot depend on the mapping order.

**Test plan** (the prompt's): `t_dbig 0`; `mnaccept --stress --only unit,e9,mn,stress` with the switches on; 10¹¹ identical via
`digcmp.sh`; timing at 10¹¹ off/on ≥ 3 + 3 without the digit file and 2 + 2 with it, and one 4 × 10¹⁰ pair; all with and without
`BS_ARENA_ROOM=0.16`.

**Verdict: worth it.** Build 1 + 2 (+ the thread count as a measured option), then decide on 4.

## RESUME

- 5162a8f: `ECALC_INIT_TL=1` (print only), README row. 2add00f: `tests/map_sweep.sh`, `tests/map_plan_f1.txt`. d67bc66: §1.
- 18af9e9 + 2880e30 + 9dfe39d: **`DB_POOL_VMM_STREAM=<W>`** (dbig.c: `vmm_stream_worker`, `stream_pick`, `db_vmm_wait_range`;
  binsplit.c seed code: `seed_wait_map` before each seed DMA, the seeds' own `pool_get(0)` without waiting; binsplit.c `pool_get`:
  parity 0 waits for its half outside pregrow — a one-line edit outside the seed code, named). Builds on aac6. README row: to do.
- f1 done (job 21659, s24-30; `~/MAP15/f1/summary.txt`). s1 chained: `~/MAP15/chain_s1.sh` waits for f1's "done", then runs
  `map_sweep.sh 9dfe39d ~/MAP15/s1 ~/MAP15/map_plan_s1.txt` from the scratch clone `~/MAP15/build/w` (MAP_CLONE; detached at
  9dfe39d). Log `~/MAP15/s1.out`. Next: read s1, decide W and the seed threads, then the gates and the timing series.
