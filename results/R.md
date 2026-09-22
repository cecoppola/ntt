# R — the race at the batch tier's level transition (Phase 12, PLAN.md §27 row R; D5)

Branch `r12` (from `main` @ 7aded87; the aac6 clone `~/ntt-r`). Files: `ecalc/rns_mul.c` (the batch tiers, `rns_dpool`),
the level loop of `ecalc/binsplit.c` (`pool_get`, the odd-node copy), `ecalc/mnaccept.sh` (`--stress`), `ecalc/tests/t_copy_order.c`.
Touched outside my list, minimal and commented `Phase 12 R`: `ecalc/mem.c` (the fix: one wait in `mem_dev_copy_on`, plus the
`MEM_COPY_NOWAIT` knob for the witness runs), `ecalc/rns_mul.h` (declarations), `ecalc/Makefile` (the test's rule).

## The transition, named

**The odd node's copy between two levels, when its source and destination are in the same region (the same APU).**
A leaf level with an odd number of nodes carries its last node up unchanged: `region_copy(NODE_P(nxt, o), NODE_P(cur, a), …)`
in the level loop, which for ≥ 2²⁰ limbs is `mem_dev_copy` → `mem_dev_copy_on(dst's device)` → one `hipMemcpy(…,
hipMemcpyDefault)` issued with the destination's device current, i.e. on that device's null stream. On this ROCm (7.2.4,
MI300A) **a device-to-device `hipMemcpy` whose source and destination lie on the copying device returns to the host at
once — the copy is only queued on that device's null stream** — while a copy between two devices is host-synchronous
(`tests/t_copy_order`, below). Nothing in the level loop waited for it, and the next level's kernels run on the four
devices' own (blocking) streams: only the copying device's stream is ordered after its null stream; **the other three
APUs' kernels are not**. Two exposures follow, both at 10¹⁰ over four processes (the odd carries there put the carried node
in the same region as its source at levels 14, 16 and 18, `place_node`'s round robin at n ≤ 16 nodes: 34 → 17 (region 3 → 3),
8 → 4 (region 0 → 0)):

1. **Level 16 → 17 (the failure seen).** The odd node 34 of level 16 (≈ 4.1 M limbs each of P and Q, 34–36 MB) is copied
   within region 3 (APU 3). Level 17 pairs it as node 17 with node 16: its Q is the shared operand *b* of both products
   (P = P₁₆ Q₁₇ + P₁₇, Q = Q₁₆ Q₁₇), read by the striped tier's scatter kernel on **all four** APUs 0.1–0.2 ms after the copy
   was issued, while the copy on APU 3's null stream is still writing it (it completes 0.2–3 ms after issue: the probe
   table below). APUs 0–2 read the old contents of that slot from the limb where their sweep overtakes the copy; APU 3's
   read is ordered and right. The four residue planes disagree from that limb on, the CRT reconstructs garbage from it to
   the top, and the carried node 8 of level 17 takes the wrong product through levels 18–20 into the leaf's top product.
2. **Level 18 → 19 (the other exposure, same mechanism, not the one caught).** The odd node 8 of level 18 is copied within
   region 0; level 19's CRT stripes on APUs 1–3 write the level's Q result over the copy's source slot while the copy may
   still be reading it (160–171 MB, completing 0.2–48 ms after issue; the CRT comes ≈ 0.2 s later, so this one needs a
   stalled copy engine — the four processes share the APUs' queues).

**Why the signature is what it is.** Every failing run (V's 5, my 2 unfixed ones) has the wrong leaf node's P wrong from a
limb 2¹⁷ + a few pages (131 072, 131 584, 134 400) and its Q wrong from that limb + 3 961 286 (±1): the stale operand is Q₁₇
from limb *j* ≈ 1 MiB into it — the scatter's linear sweep overtook the copy 1 MiB into Q (after all of P, which precedes
Q in the slot; P₁₇ is read only later, by the CRT, and is right); the level-17 product P is then wrong from *j* (P₁₆ has no
zero low limbs), Q from *j* + zeros(Q₁₆); carried to the top, P_leaf = P_A Q₈ + P₈ is wrong from *j* and Q_leaf = Q_A Q₈ from
*j* + zeros(Q_A) + zeros(Q₁₆) = *j* + 3.96 × 10⁶ (Q_A over 2.68 × 10⁸ terms is divisible by 10^(6.7 × 10⁷): 3.7 × 10⁶ zero
limbs; Q₁₆ over 1.7 × 10⁷ terms adds 2.3 × 10⁵) — the constant offset seen in every failure, on node 0 or 1 alike, and
the reason "P and Q wrong from different limbs" pointed at one wrong operand shared by both products. It is not the pool
growth: **the recipe `POOL_LOG=29 RNS_POOL1_GB=3.2213` grows nothing** (the local tier's pair tiles need ≤ 2 GiB of pool 1
and the striped tier caps its tiles by the pools; no growth line appears with `RNS_VERBOSE`, and the `RNS_POOL_GROW`
invariant below never trips on it) — what the recipe does is run 10¹⁰ over four processes on one node, where the odd
carries land in the same region and the four processes' contention stretches the copies. Any per-level probe hid it
because it synchronised the devices between the levels (V's 31/31): the copy had landed before the next level's read.

## Evidence, run by run

`tests/t_copy_order` (job 20904, one node): a 512 MB copy issued behind a 0.2 s kernel on the copying device's null stream —
- device 0 → device 2: `hipMemcpy` returned after 0.197 s (host-synchronous); source overwritten by the source device's
  stream / destination read by a third device before the copy: 0 of 5 / 0 of 5.
- **device 0 → device 0: `hipMemcpy` returned after 0.000 s; a kernel on device 1's stream overwrote the source before the
  copy read it in 5 of 5** (the destination held the later value).
- with `hipStreamSynchronize(0)` after the copy (the fix): 0.191 s, 0 of 5 for all three cases. VERIFY OK (3 checks).

The reproducer (`v11_d5.sh <job> 13 shard`, 10¹⁰ at size 4, `POOL_LOG=29 RNS_POOL1_GB=3.2213`, `ECALC_RES_LOG_LEVEL=99`
so that no per-level probe runs; `ECALC_COPY_PROBE=1` records an event on the copying device's null stream right after the
copy and a thread polls it — no wait, no synchronisation — against the time of the next level's first kernel launch):

| batch (job) | build | runs | result |
|---|---|---|---|
| 1 (20889) | unfixed, copy probe | 13 | 12 identical; **p1shard6: node 0's leaf wrong**, P from limb 134 400, Q from 4 095 686 (offset 3 961 286). Probe: every same-region copy of levels 14, 16, 18 completed 0.2–48 ms after issue, the next level's first kernel launched 0.1–0.9 ms after issue — UNORDERED in 47 of 52 such lines over the 13 runs; the cross-device copies of levels 19, 20 complete before the call returns |
| 2 (20904) | fixed build with `MEM_COPY_NOWAIT=1` (the old copy), probe + `ECALC_B_SNAPSHOT=1` | 13 | 12 identical; **w2shard5: node 1's leaf wrong, P from 131 072, Q from 4 092 359 (offset 3 961 287); its level-16 copies completed 0.2 / 1.4 / 3.0 ms after issue with level 17 launched at 0.1–0.2 ms; the snapshot of the top level's shared operand B on every APU equals memory (the wrong data was already in the copied node: the corruption is in the copy chain, not in the top level's read) |

## The fix

`mem_dev_copy_on` (mem.c) waits for the copy it issued: `hipStreamSynchronize(0)` on the copying device after the
`hipMemcpy`. Every caller of `mem_dev_copy` / `mem_dev_copy_on` (the level loop's odd node, the checkpoints, the leaf
dumps, the mdev staging) assumes the copy is complete when it returns; the cross-device case already was. Cost: nothing
measurable (the copies are 34–171 MB, a few ms, at four levels of a run). `MEM_COPY_NOWAIT=1` restores the old behaviour
for the witness runs only. Size 1 is bit-identical (the regression below): the wait changes ordering, not values.

Also fixed on the way (rns_mul.c, the batch-local grpB tile): the pool-1 request was `L` limbs where the tile uses
`EC_NP` planes of `L` (`db + p * L`, p < 4) — reached only with `RNS_BATCH_LOCAL_MIN ≤ 2` (V's batch 5 ran with it at 10¹⁰:
a 4 GiB tile on a 3 GiB pool, writing 1 GiB past it); the request is now `EC_NP * L`.

## The invariant: no pool grows inside a phase

`rns_dpool` (the plane pools, rns_mul.c) and `pool_get` (the region pools, binsplit.c) abort with the memory accounting
(`mem_report("GROW")`, `mem_report_summary()`, as `mem_oom` does) when an existing pool would have to grow while a phase
is running, unless `RNS_POOL_GROW=1`. With it the growth is counted (`rns_pool_n_grow`, `bs_st.n_grow`) and logged under
`RNS_VERBOSE`. "While a phase is running" is the flag `mem_pool_guard` (mem.c): `binsplit_e` clears it around the level-0
layout — a second `binsplit_e` in one process legitimately lays its regions out again, which is what `t_bs` does — and
sets it for the level loop, where it stays for the reciprocal and the division that follow. (The first version guarded
`pool_get` unconditionally and `t_bs` tripped it on its second run: job 20944, the one failing step of that regression.)
The defaults never trip it: the whole regression (10⁹ both bases, 10⁸ sizes 2/3/4, 10⁹ sizes 2/4, the checkpoint restart,
4 × 10¹⁰) and every forced-growth-recipe run at 10¹⁰/4 ran with the invariant armed and none of them grew anything.

## `mnaccept.sh --stress`

10 runs of 10⁹ at size 4 with `RNS_POOL_GROW=1 POOL_LOG=27 RNS_POOL1_GB=0.4 RNS_BATCH_LOCAL_MIN=2 MEM_DPOOL_FILL=1`:
pool 1 at 381 MiB. At 10⁹ over four processes every batch-local tile is capped by pool 0 (1 GiB at `POOL_LOG=27`) to
`EC_NP × Mmax × L × 8` = 1 GiB of A planes and therefore asks for 512 MiB of B planes (`EC_NP × L × 8` in grpB at the top
leaf level, L = 2²⁴; `EC_NP × Mmax/2 × L × 8` in pair mode below it) — measured from a run: the first batch level already
asks for it. So pool 1 grows inside bs at the first level (the growth line is required in every run's log, else the step
fails) and the grown pool is filled with 0xA5 first. (The first attempt, `RNS_POOL1_GB=0.8054` = 0.75 GiB, was above that
512 MiB and grew nothing: job 20937, 10 of 10 identical but 0 growths — the step correctly failed.) Every run
must be identical to the reference with all 4 nodes VERIFY OK. Opt-in like `--full`; ≈ 10 min.

So **2 failures in 26 unfixed runs** (V had 5 in 26 on the same recipe; the pooled rate is 7 in 52 ≈ 13 %), and with the
fix **42 in 42** identical:

| batch (job) | build | runs | result |
|---|---|---|---|
| f3 (20913) | fixed (`f80e517`), no probe of any kind | 14 | 14 identical, all 5 VERIFY OK lines per run |
| f4 (20917) | fixed, same | 14 | 14 identical |
| f5 (20922) | fixed, same | 14 | 14 identical |

At the unfixed rate (7 in 52) the chance of 42 clean runs is 0.865⁴² ≈ 2.4 × 10⁻³.

## Gate runs

- **42 of 42** forced-growth runs at 10¹⁰ over 4 processes (`v11_d5.sh <job> 14 shard`, `POOL_LOG=29
  RNS_POOL1_GB=3.2213`, `ECALC_RES_LOG_LEVEL=99` so no per-level probe runs) identical to
  `~/ntt/ecalc/results/e_1e10.out`, every node VERIFY OK, in three batches on the fixed build at `f80e517`
  (jobs 20913, 20917, 20922). Against 24 of 26 on the unfixed build in the same configuration (jobs 20889, 20904).
- **`--stress` 10/10 in three separate batches** (jobs 20940, 20942, 20943, each its own allocation, `mnaccept.sh <job>
  --only stress --stress`): 30 of 30 runs identical to `ref/e_1000000000.txt`, all 4 nodes VERIFY OK, **16 pool growths
  logged per run** (pool 1 on each APU, at four levels). A fourth pass inside the regression below: 10/10.
- **The regression** `./mnaccept.sh 20948 --full --stress` on the final build (`fa73163`): **18 passed, 0 failed in
  2096 s** — t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify (334), t_out, t_mn_grid at 2 procs; 10⁹ size 1
  base 10 (13.7 s) and base 2 (24.0 s) identical; 10⁸ sizes 2/3/4 and 10⁹ sizes 2/4 identical with every node VERIFY OK;
  the checkpoint restart identical; **4 × 10¹⁰ size 1 identical to `results/e_4e10.out`, total 79.64 s** (161 s with the
  write); the stress step 10/10. (17 steps + the new stress step; the earlier run, job 20944, was 16/17 — the `t_bs`
  invariant bug above, fixed in `fa73163`.)
- So size 1 is bit-identical at 10⁹ in both bases and at 4 × 10¹⁰, and the wall clock is unchanged (79.6 s against
  Phase 10's 83.0 / Phase 11's 81.5 ± 1.4 s at 4 × 10¹⁰).

## Files, switches, open issues

New switches: `RNS_POOL_GROW=1` (allow in-phase growth: the stress recipe), `MEM_COPY_NOWAIT=1` (the unfixed copy, for
witness runs only), `ECALC_COPY_PROBE=1` (the odd-node copy against the next level's launch, plus the copy-vs-source
check), `ECALC_B_SNAPSHOT=1` (each APU's read of the shared operand B against memory). New test `tests/t_copy_order`
(in `make all`; needs 3 APUs, ≈ 10 s). New step `mnaccept.sh --stress`.

Open issues:
- The other exposure of the same copy (level 18 → 19, the CRT of the next level writing over the copy's source) was
  never observed failing; the fix closes it too, but it has no separate witness.
- `mem_dev_copy_on` now waits on the null stream of the copying device. If a caller ever wants the copy asynchronous it
  must use `mem_dev_copy_async` (Phase 10 H) and `mem_dev_copy_wait`, which were always the asynchronous pair.
- `ECALC_COPY_PROBE` reports only the last odd-node copy before a batch level (one per level); levels whose odd node is
  copied across devices are reported as "ordered" because that copy is host-synchronous on this ROCm.
