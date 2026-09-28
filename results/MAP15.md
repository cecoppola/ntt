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

## 2. The stream (`DB_POOL_VMM_STREAM=<W>`)

### 2.1 Design (18af9e9, 2880e30, 9dfe39d, 47ccca1)

- `dbig.c`: under the switch `db_vmm_arena_alloc` only reserves the VA and registers the arena (no mapping in
  `binsplit_pregrow`: `rns_init` returns at ≈ 8 s instead of ≈ 18 s). From `db_vmm_bg_release` (after the plane pools:
  `RNS_PLANES_FIRST` kept) W worker threads (1–4, detached) map units of `DB_POOL_VMM_STREAM_BATCH` (2) chunks in need order:
  phase 0 the parity-0 halves by APU (the seeds' region order), phase 1 the parity-1 halves round robin, phase 2 the rest (the
  dm extra) — released at bs's first device top-tier level (`db_vmm_stream_rest`, one call in the level loop) or by any waiter
  that needs chunks past the parity-1 half (donation, `vmm_make_room`, release); `DB_POOL_VMM_STREAM_REST=0` maps it at once.
  At most one worker per APU, so each APU's chunks map in increasing order and `mapped` keeps its meaning for every existing waiter.
  Each unit is zeroed (`hipMemsetAsync` on the worker's stream + sync) before it counts, as the background thread did.
- `binsplit.c` seed code: every seed DMA first waits for its own chunks (`seed_wait_map` → `db_vmm_wait_range`, counted in "waited
  for the regions"); the seeds' own `pool_get(0)` inside `rns_init` does not wait.
- `binsplit.c` outside the seed code (named): `pool_get` for parity 0 outside pregrow waits for the parity-0 half (a no-op without
  the stream: it is mapped inside pregrow), so any other first use of the regions (a checkpoint restart) is safe; the
  `db_vmm_stream_rest()` call at the first device top-tier level.

### 2.2 First runs (job 21661, s24-26, 9dfe39d, 22:44–22:54 EDT; all measured)

| run | setting | seeds end | level 1's chunks mapped | level 1 starts | level 1 | total | note |
|---|---|---|---|---|---|---|---|
| e9s | 10⁹, W = 4 | — | — | — | — | 12.41 | VERIFY OK |
| r0c | room, stream off | 34.26 | 35.06 (bg) | 34.68 | 4.2 s | 179.07 | VERIFY OK |
| s4r | room, **W = 4** | 58.38 | 44.03 | 58.84 | 4.3 s | 216.91 | VERIFY OK; **one seed DMA launch blocked 32.9 s** behind the four workers; 0.27 s/chunk |
| s1r | room, **W = 1** | **28.33** (no wait for the regions; issue 2.2 s as before) | 34.93 (0.216 s/chunk) | 34.93 | 12.9 s (6.1 s waiting for APU0's p1; batch 4.45 s while the rest mapped) | — | **the node rebooted** during the run (after bs level 4, ≥ 12 s after the last mapping call; Slurm NODE_FAIL, "Node unexpectedly rebooted") |

Findings: (1) four concurrent workers starve the seed thread's kernel launches (the reason for the old mapper lock) and map
slower, 0.27 s/chunk: **W = 1** is the form. (2) With W = 1 the seeds are done 6.6 s before level 1's chunks: the mapping rate
(0.216 s/chunk with 192 seed threads computing; 0.131 with the CPU free, run `aft`) is now the floor, so the seeds can give up
threads to it (b). (3) The rest mapped beside level 1's batch cost ≈ 1.5 s of it: deferred to the device top tier in 47ccca1.

**The node failure** (s24-26, BootTime 22:53:14 EDT; job 21661 NODE_FAIL). The run had passed bs level 4 at ≈ 47 s; the stream had
finished every chunk at 37.6 s and its worker had exited. The project has two earlier NODE_FAILs on unchanged code (job 20629
s24-16 2026-09-17, job 20945 s24-30 2026-09-22; RESULTS.md), so a coincidence is plausible, but it is **not proven**. Rule adopted:
the next series repeats W = 1 first on s24-30; a second failure stops all node work with the switch. s24-26 stays down (needs the
admin).

### 2.3 The repeat on s24-30, and the second node failure (job 21662, 47ccca1, 22:57–23:03 EDT; measured)

`s1r` again (room, W = 1, the rest now deferred to the device top tier): the seeds ended at 29.23 s (no wait for the regions),
level 1's chunks at 35.90 s, level 1 started 35.90 (11.4 s: 6.2 s waiting for APU0's parity-1 half). The rest (3 chunks on APUs 1–3)
was released at 70.5 s by the first device top-tier level and mapped by 72.7 s while the GPUs ran it. From there the run
degraded: **the mdev tier 39.6 s (≈ 27 s normally), the reciprocal 59.7 s (≈ 38 s)**, and the node rebooted in dm (Slurm
NODE_FAIL "Node unexpectedly rebooted", 23:02:51 EDT); the five runs after it failed at launch (rc 233). **No further node work
with the switch.**

| | W = 4 (s4r) | W = 1 (s1r, job 21661) | W = 1 (s1r, job 21662) |
|---|---|---|---|
| mapping while GPU kernels ran (beyond the seed DMAs) | none: every chunk mapped by 44.0 s, level 1 at 58.8 s | the rest (11 chunks) during level 1 (34.9–37.6 s) | the rest (9 chunks) during bs's device top tier (70.5–72.7 s) |
| outcome | VERIFY OK | node reboot after bs level 4 | slow mdev tier and reciprocal, then node reboot in dm |

Every other run of this report (15, stream off or W = 4, 2add00f–9dfe39d) finished with VERIFY OK. The common factor of the two
failures is **VMM chunks created, mapped and given access while the GPUs run the bs products** (the old code maps the arena's rest
while only the seed DMAs run; its dm remaps and growths happen between products). The mechanism is not known (driver or firmware;
no node-side log is available to me). 47ccca1's `DB_POOL_VMM_STREAM` therefore refuses to run without `DB_POOL_VMM_STREAM_ACK=1`
(b94072c).

## 3. Conclusions

**(a) The arena's second half in the background, in chunks — not worth it as built, and not safe.**
- The seeds were the obvious waste (idle 4.9–6.9 s for all four parity-0 halves); the stream removes that wait exactly as
  modelled: **seeds end 28.3 / 29.2 s instead of 34.3–35.5 s** (measured, W = 1).
- But level 1 does not move (34.9 / 35.9 s against 34.7–35.4 s on the old path, measured), because **the mapping itself is the
  floor**: level 1 needs 124 chunks (266 GB), and the mapping runs at ≈ 0.2 s per 2 GiB chunk whoever calls it (one worker:
  0.216 s/chunk; the old four pregrow threads 0.16–0.19 and the old background mappers 0.20–0.23 with the seeds computing;
  0.131 only with the CPU idle). `hipMemCreate` is ≈ 70 % of it, `hipMemSetAccess` ≈ 30 %, `hipMemMap` and the zeroing ≈ 0.
- The runtime serializes the calls (RL); four workers at once map slower (0.27 s/chunk) and starve other threads' launches
  (32.9 s for one seed DMA): there is no concurrency to buy.
- Letting the bs levels wait only for their own chunks (item 4 of §1) would overlap level 1 with the mapping — exactly the
  regime that preceded both node failures. Not built.
- With `BS_ARENA_ROOM=0.16` the same (its extra chunks are in the rest, not on level 1's path).

**(b) The seed threads.** On the old path (f1, measured): 192 threads (the default): level 1 at 35.07 / 35.40 s; 96: 36.46 s (spans
16.6 → 19.9 s; the mapping ×1.1 faster does not make up for it); 48: 47.63 s (spans 35.7 s). **Keep all threads.** The freed threads
would only help the mapping, which the seeds then wait for; with the stream (seeds 6 s early) 96 / 128 threads were in the plan but
not run (the node failure).

**(c) What else the timeline shows** (measured, `ECALC_INIT_TL=1`, 5162a8f — the only part of this branch recommended for merging):
- rns_init: staging and contexts 1.5–2.5 s, the plane pools 4.2–5.6 s (hipMalloc, ≈ 0.045 s/GB), then the arenas.
- The old background mapper lock (`g_vmm_map_mx`) is not fair: one APU maps 4–7 chunks in a row while the other three queue
  (9–12 s each); harmless for level 1 (it needs every APU's half) but it is why the per-APU "done" times spread 4–15 s.
- The seed thread's pinned buffers (2 × 8 GiB) cost 1.4–1.6 s at its start, on its path.
- **The lever that remains is the mapping rate, and it is outside the VMM path**: hipMalloc maps at 0.036–0.05 s/GB against
  hipMemCreate's 0.077–0.1 (R114 §6b, measured there). R114 measured the VMM arena at **+8 s of init at 10¹¹** against the
  hipMalloc arena; it was adopted for the memory ceiling (no in-phase hipMalloc fallback, 1.4 × 10¹¹ at cap 2³¹). With
  `BS_ARENA_ROOM=0.16` the division at 10¹¹ remaps 0 times (AS15), so **`DB_POOL_VMM=0` at the sizes where memory allows** may be
  worth one paired series (existing switches, no code; untested here — the nodes went down). Under `DB_POOL_VMM=0` the seeds also
  store directly into the regions (no buffers, no DMA: R114's other +8 s of bs).

**Expected gain of this branch at 10¹¹ and at the target: 0** (the stream is refused; the timeline switch is print only).

## 4. Tests

| test | command | size | result | when |
|---|---|---|---|---|
| timeline, stream off | `map_sweep.sh 2add00f ~/MAP15/f1 map_plan_f1.txt` (job 21659, s24-30) | 10¹¹ × 7 (defaults, room, 96 / 48 seed threads, `ECALC_SEED_ORDER=after`) | **7 × VERIFY OK**; totals 187.1–199.5 s (`results/MAP15/f1/summary.txt`) | 22:21–22:44 EDT |
| stream, e9 | `DB_POOL_VMM_STREAM=4 ./ecalc 1e9` (job 21661, s24-26) | 10⁹ | VERIFY OK, 12.41 s | 22:44 |
| stream off, room | r0c (job 21661) | 10¹¹ | VERIFY OK, 179.07 s | 22:47 |
| stream W = 4, room | s4r (job 21661) | 10¹¹ | VERIFY OK, 216.91 s (seed launch starved) | 22:51 |
| stream W = 1, room | s1r (job 21661, s24-26) | 10¹¹ | **node reboot** | 22:53 |
| stream W = 1, room, rest deferred | s1r (job 21662, s24-30) | 10¹¹ | **node reboot** | 23:02 |
| build | `make` on the aac6 login node, b94072c | — | builds; `DB_POOL_VMM_STREAM=1` refused at the first arena | 23:05 |

**Not run** (stopped after the second node failure; two of the three nodes down, the third G5's): `t_dbig 0`,
`mnaccept --stress --only unit,e9,mn,stress`, 10¹¹ via `digcmp.sh`, the timing series. With the stream off every new code path is
print only or a wait that returns at once (`pool_get` parity 0, `seed_wait_map`, `db_vmm_stream_rest`), and the f1 / r0c runs on
this branch (10¹¹ × 8, VERIFY OK) exercised them; the gates are still owed before anything here merges:
`./mnaccept.sh $J --stress --only unit,e9,mn,stress` and `./tests/t_dbig 0` at b94072c, stream off.

## 5. Open issues

- **Two nodes down** (s24-26, s24-30: "Node unexpectedly rebooted", 22:53 and 23:02 EDT) — they need the admin to resume them. The
  cause is not established; the correlation with VMM mapping during GPU compute is strong (2 of 2 against 0 of 15).
- If the stream is ever tried again: a unit test that maps chunks on one APU while kernels run on all four, on a node the admin can
  spare, before any full run.
- The old path also maps during compute at some sizes (the background rest when the seeds end before it, and the dm growths): no
  failure is known there, but the same question applies.
- `DB_POOL_VMM=0` with `BS_ARENA_ROOM=0.16` at 10¹¹ (§3 c): an untested idea for the init floor.

**Outside my files** (named): `binsplit.c` outside the seed code — `pool_get`'s parity-0 wait (one line) and the
`db_vmm_stream_rest()` call in the level loop (one line), plus `ECALC_INIT_TL` marks at level 1–3 (print only); `dbig.h` prototypes.
`tests/map_sweep.sh`, `tests/map_plan_*.txt` are new.

## 576-node estimate (standing rule)

Unchanged by this work (nothing here is on by default or recommended): **4.25 × 10¹³ digits in ≈ 256 s (4.3 min) without the write,
≈ 286 s (4.8 min) with the packed write at 0.6 GB/s per node** (modelled, RESULTS §87 / DOC15, as AS15 quotes); node memory 416 GB of
480 on the defaults (modelled).

## RESUME

- **Stopped** after the second node failure (§2.3). Branch `p15-MAP` at b94072c + this report; aac6 clone `~/ntt-MAP15` at b94072c
  (built); scratch clone `~/MAP15/build/w` (47ccca1). Logs in `results/MAP15/{f1,s1,s2}`. No job of mine running or pending.
- Recommended for merging: `ECALC_INIT_TL` only (5162a8f; print only). `DB_POOL_VMM_STREAM` stays behind its ACK guard; do not adopt.
- Owed before any merge: the gates of §4 at b94072c with the stream off. For the user: the two nodes need the admin.
