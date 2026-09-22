# What is open after Phase 12 — the remaining choices, with costs and benefits

State: `main` @ 3524146 (RESULTS §77). One node: 4 × 10¹⁰ digits in ≈ 81 s / 12.1 GB
host / 277–322 GB device; 8 × 10¹⁰ at 382 GB; 10¹¹ in 263 s at 445 GB of 502; nothing
mapped inside any phase. Multi-node: SHMEM transport verified in its target forms,
every tree level gridded over fixed planes on the `2,4,…,64,192,576` schedule, the
regression 21/21 including a forced-growth stress step. Modelled at 576 nodes:
≈ 3.9 × 10¹³ digits in ≈ 4.0 minutes.

Phase 12 closed the eleven items of DECISIONS.md and the nine of DECISIONS2.md by
measurement. What follows is what those measurements left open, plus what they
newly revealed. Same format: what it is, why it is a decision, options with cost
and benefit, recommendation.

---

## 1. The top-level checkpoint: default, and whether the division should wait

### What it is
`ECALC_CKPT_TOP` writes the top-level P and Q (35.6 GB at 4 × 10¹⁰) so a finished run
can be re-verified from its files (`ECALC_RECHECK=1`, 46 s) without recomputing.
Agent W measured the write as hidden behind the reciprocal and the division on disks
writing at 1.13–1.59 GB/s. On aac6's slower path it runs at **0.31 GB/s**: 113 s of
background write that the division waits for, taking the 4 × 10¹⁰ wall from 81 to
164 s (digits identical). I set the default to **off** during integration on that
evidence; the feature is one switch away.

### Why it is a decision
The waiting is structural, not accidental: the driver releases Q when the division
finishes, and the writer still needs it. W's own open issue names the fix — release
Q only after the output stage, so the write has the whole output phase to finish in.

### Options
- **(a) Leave the default off** (today). No cost; a large run that later needs
  re-verification must be recomputed unless the operator sets the switch.
- **(b) Fix the wait** (release Q after the output stage; ≈ half a day) and then
  turn the default on: the write gets ≈ 160 s of output phase instead of ≈ 30 s of
  division, so 35.6 GB at 0.31 GB/s still does not fit, but at ≥ 0.5 GB/s it does.
- **(c) Size the decision by the measured disk**: time the first chunk, keep the
  checkpoint only if the projected write fits the remaining phases. A quarter day,
  and it never costs wall time.

### Recommendation
(c), with (b) as the enabling change. On the target this matters more than here: the
parallel file system is fast, but 576 nodes write 20 TB of top sets at once.

---

## 2. The init floor is now the seed thread, not the mapping

### What it is
Agent I established that the mapping floor is the driver's page clearing (~14 GB/s
per core, serialised across the four APUs by one lock) and that nothing — pooled,
managed, host-backed or huge-page allocation — beats `hipMalloc` in a fresh process;
host-backed memory additionally drops `hipMemcpy` to 21 GB/s. With the larger planes
now on, init is 22.3 s and the seed thread inside it takes ≈ 21 s: **the seeds are
now the critical path of initialisation**, where before the mapping was.

### Options
- **(a) Make the seeds faster** (PLAN §22's I8: two limbs per step or a
  Montgomery-style reduction in the decimal single-limb multiply, ≈ 1 day). If the
  seeds drop below the mapping, init returns to ≈ 18 s and the wall to ≈ 77 s.
- **(b) Start the seeds earlier** — they need only N; the arenas they write into are
  what they wait for. Overlapping the seed computation with the *plane* mapping
  (which happens first) would hide more of them. Half a day, no numerics change.
- **(c) Leave it**: 22 s of 81 is initialisation, and the run is dominated by GPU
  phases.

### Recommendation
(b) then (a): together they are the largest single-node item left (≈ 4 s), and
neither touches the numerics.

---

## 3. Whether to keep both memory models

### What it is
Two independent accountings now exist: `mem_model.py` (Q's, with G's line-by-line
port of the C sizing function) and the tree's own accounting inside `binsplit.c`.
They agree on the arena exactly but differ on the per-node ceiling at 576 nodes —
6.7 × 10¹⁰ against 7.1 × 10¹⁰ digits — because the model counts exchange scratch that
the tree's accounting does not.

### Options
- **(a) Reconcile them** (half a day): decide which scratch is live simultaneously
  and make the C function and the model agree; the ceiling becomes one number.
- **(b) Keep both** and quote the conservative one (6.7 × 10¹⁰).

### Recommendation
(a) before the target run — the difference is 2 × 10¹² digits, and a run planned at
the wrong ceiling fails at init on 576 nodes at once.

---

## 4. The SHMEM forms to enable on the target

### What it is
Sandia OpenSHMEM on aac6 verified: contexts per APU thread without a lock,
put-with-signal, device-memory symmetric heap, pool-resident slabs. Cray SHMEM on the
target supports all four, but its thread support, heap placement and signal
implementation must be confirmed on the machine.

### Options
- **(a) Enable all four from the start** on the target, with the fallbacks one
  switch away (`COMM_SHMEM_SERIAL=1`, `COMM_SHMEM_ORDER=fence`, host heap, staged
  slabs). Fastest path; a fault at 576 nodes is harder to diagnose.
- **(b) Bring them up in order** at 2, 4, 64 nodes as `docs/TARGET.md` prescribes,
  then enable at 576.

### Recommendation
(b) — the runbook exists precisely for this, and the cost is one hour.

---

## 5. What the fabric measurements should decide

### What it is
The model's fabric assumptions are a per-message cost of 2 µs and a part-file
bandwidth of 2 GB/s per node. The sensitivity is known: at 20 µs the 576-node wall
goes 4.0 → 4.3 min, and the dragonfly's third layer (built, off) becomes worth
enabling somewhere around that point.

### Options
- **(a) Measure both in the first target session** (`t_comm` ping-pong at the
  message sizes the exchanges use; one node's part-file write) and re-run
  `estimate.py`. An hour.
- **(b) Run at the modelled size and adjust afterwards.**

### Recommendation
(a). It is the only thing that turns the estimate into a plan.

---

## 6. Housekeeping that Phase 12 left

- **The host-flow stand-ins** (`MN_DM=host`, `MN_COMBINE=host`) were kept as the
  cross-check while the race was open. The race is closed and the cause is
  understood; they can go (half a day, W's file list says exactly what). *Recommend
  delete after one more multi-node regression.*
- **`MEM_ALLOC`'s five losing forms** are kept behind the switch as the evidence for
  the allocation study. *Recommend keep* — they cost nothing and the study is the
  answer to "why hipMalloc?".
- **The 4 KiB-mapping result** (0.68 s/GB and the GPU at 60 %) is a trap worth a line
  in the README for anyone who tries `mmap` without huge pages. *Recommend add.*
- **`<outfile>.top`** (35.6 GB at 4 × 10¹⁰, ≈ 20 TB at the target) is left on disk for
  the operator. *Recommend: the runbook says to delete it after the recheck.*

---

## 7. The next session's shape

- **Single node** (items 2, 6): ≈ 77 s at 4 × 10¹⁰, a cleaner tree. 1–2 days.
- **Target readiness** (items 1, 3, 4, 5 + the two-real-node SHMEM run that never
  found two idle nodes): the ceiling agreed, the checkpoint decided, the runbook
  exercised. 1–2 days.
- **Beyond**: the only structural item left in the pipeline is the seeds' arithmetic
  (item 2a) and, at scale, the part-file write (C7) — everything else the models now
  cover.

### Recommendation
Both tracks with two agents, as in Phases 11 and 12; they touch disjoint files.

---

## Summary

| # | choice | recommendation |
|---|---|---|
| 1 | top checkpoint default and the division's wait | size it by the measured disk; release Q after the output stage |
| 2 | init floor is now the seeds | overlap them with the plane mapping, then make them faster |
| 3 | two memory models disagree by 4 × 10⁹ digits per node | reconcile before the target run |
| 4 | SHMEM forms on the target | bring up in order per `docs/TARGET.md` |
| 5 | fabric assumptions | measure message cost and file bandwidth first, re-run `estimate.py` |
| 6 | housekeeping | delete the stand-ins; keep the allocation evidence; document the 4 KiB trap |
| 7 | next session | two agents, single-node and target-readiness |
