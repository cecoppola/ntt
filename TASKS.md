# TASKS — live task list (open work, idea lists, dated status)

> Ranked opportunities and big-picture goals: [docs/OPPORTUNITIES.md](docs/OPPORTUNITIES.md) (2026-10-10).

*Rewritten 2026-10-10.* The Phase 12-13 task log (the old front of this file, sections 1-7 and the "Suggested order") is
history: `archive/docs/TASKS_HISTORY.md` (citations such as "TASKS 1.3" or "TASKS §6.1" refer to it). Target-only work
(T1-T14) is `docs/TARGET_TASKS.md`; every decision and its status is `docs/code/05_DECISION_REGISTER.md`; measurements are
`RESULTS.md` (latest §127). Labels: (m) measured, (mod) modelled.

## Open work (2026-10-10)

State: main has the kernel defaults `DBIG_ADDSUB2=1`, `DBIG_MAXIDX_TOP=1`, `DBIG_QSEL=1`, `DIST_CHUNKS=8`; one node at 6.441e10
digits ≈ 78 s (was ≈ 94 s), 4 nodes −50.0 s (m, S43). The 576-node plan: device 351.7 of 373.44 GB, node ≈ 395.0 of 480 GB
(mod, S45); no-write time ≈ 180 s at an assumed 47 GB/s per APU (mod; the new kernels are not yet in the time model).

| # | item | status | where |
|---|---|---|---|
| 1 | **Multi-node segfault** in `hipMemMap` from `vmm_bg_map` (about 1 % of 2-node runs, 3.5 % of 10-node runs, m); a fix is required before the target | **deferred** by the user to a later session; candidates `ECALC_VMM_BG=0` (on main, +20 s, 0/420 vs 4/422) and `ECALC_VMM_BG=2` (branch s35, no evidence yet) | "Deferred" section below |
| 2 | **`DBIG_QSEL=1` hang**: one in 64 aac6 runs (n1_r6_4B, after init), not reproduced in 44 repeats (0/44; `=0` 0/14); cause unknown | open; watch for start-up stalls in every soak | S42 (results/S42.md), RESULTS §127 |
| 3 | Next kernel candidates (S39): `k_addsub2` tile / wave-scan (1.0-1.5 s, mod), `k_modq` 128-bit `%` (≈ 1.4 s GPU, mod), `k_crt_batch` small grid (≈ 0.8 s, mod) | not started | results/S39.md |
| 4 | X2 cut-group pool-offset asymmetry found by S45 (not a target risk) | understand and document | results/S45.md |
| 5 | Scaling study at 2 / 4 / 6 nodes; put the new kernels into the time model (`mn_model.py` / `estimate.py`) | later | |
| 6 | Handoff document | later | |
| 7 | Target-side options (need a target session): `MN_T_CHUNK_MB=2048` A/B (T11b), division overlap gate (T12, S-2), X2 try (T13), kit `nodechk` rehearsal (T14) | open | docs/TARGET_TASKS.md |
| 8 | Repo clean-up: moved drivers, historical docs and raw results to `archive/` (done 2026-10-10, `archive/MANIFEST.md`); unmerged branches s24, s25, s35, s37, p14-*/p15-* and merged worktrees/branches: user's decision | partly open | AUDIT_REPO (session scratchpad) |
| 9 | `docs/AGENT_PROTOCOL.md` holds the aac6 login password in plain text; moving it out of the tree is the user's decision | open (user) | |

Environment facts: aac7's QOS is 6 nodes per user (no 10-node runs); aac6 SH5 (`SH5_MI300A_SPX`) shows one device (ecalc
cannot run), its CPX nodes (6 XCDs of 22.9 GB) run up to 4e9 digits with `RNS_INIT_POOL_LOG=29` (S44).

## Idea sweep of past sessions (2026-10-07)

Two bounded sweeps of past transcripts (the main session and its subagents, and six
home-directory sessions) produced the ideas below. Only ideas **not** already in §§1–7,
PLAN, TARGET_TASKS, TARGET_WISHLIST, the decision register or S19B §5 are listed. Ideas
that were already listed or rejected are not repeated here. Confidence in the sweep is
low to medium: the search used greps and targeted reads, not full reads. Labels: measured
(m), modelled (mod), assumed (assumed).

| # | idea | what | claimed saving (label) | source | effort | risk | overlap / where it goes |
|---|---|---|---|---|---|---|---|
| S-1 | **Release v-slots after each distributed tree level** | The general-map v-exchange slots stay resident for the whole tree. Free each level's slots once its exchange is consumed (lifetime and order must keep digits identical) | −11.5 GB per node device at 576 nodes (mod, from a transcript table); not quantified at 10 nodes. Probably smaller once DC8 is in, since DC8 halves the slot size | 2026-10-06/07 (docs session, "Find the opportunities") | M | medium: needs a switch and a digits check | Overlaps **DIST_CHUNKS=8** (now default, commit 0adf205; S19B §5 #5, −13.4 GB per node, and ME26). Overlaps the tree-level tight reservations in **PLAN L1 / E2** (−51.7 GB mod at 10¹¹, 50 GB m). Size it after DC8 is measured at 10 nodes |
| S-2 | **Division-specific overlap** (not only the reciprocal) — **SAVED as a target-gated option (user 2026-10-08, TARGET_TASKS T12)** | The division waits entirely on exchanges: 232 of 432 s at 10 nodes. S19B §5 #4 covers only the reciprocal's wait; §5 #2 covers general "other" time. The division's own overlap is not listed | up to the 232 s wait (upper bound, assumed); realistic share unknown | 2026-10-07 (docs session, candidate table B) | M (assumed) | medium | Partly listed (S19B §5 #2, #4). Sizing: the D3 `MN_WAIT_STATS` run S20 (main session's note; see the caveat under the table). Candidate A of the E1 work in that note, not to be confused with §7 E1 (`t_primes`). **Status 2026-10-07 (S20, results/S20.md §3, RESULTS §120): sized.** comm_wait about 200 s per thread (a: 4 threads) = 43 % of bs wall and 55 % of dm+recip wall; skew bound 13-30 s per thread in dm, about 0 in recip; recommendation GO on a prototype (user's decision); a ready counter is not needed first. **Status 2026-10-08 (S22, results/S22.md §2, RESULTS §122): re-sized, GO revised to NO-GO for now.** Scaling n = 2..10: wait per thread 30 % -> 53 % of wall; the fabric is busy 84 % of the division, so the ceiling of a perfect division + reciprocal overlap is 43 s of 453 at 10 nodes (mod), 25-34 s at 576 on the target fabric (mod), realistic 6-17 s (a), below the 30 s threshold. The `ready` counter counts data-arrival signals too (skew + transfer). Decision is the user's; one target `MN_WAIT_STATS` run would decide it |
| S-3 | **Barrett constant division for decimal seeds** (further tuning) | Replace the compiler's constant division in the decimal seed `mul_1` path with a Barrett constant. The "further tuning still on the table" item | ≈ −10 s on one node (assumed; no measurement in the transcript) | 2026-09-17 (main session) | S | low | Child of **§2.2** and PLAN WP4 (§58, Barrett `mul_1` done, 2.6 ns/limb). Measure first with the seed-thread timers; it moves the seed wall, which 2.1 caps **Measured, dropped (S37, results/S37.md).** |
| S-4 | **E2 fixed-slot allocator table** | If the tight-reservation layout (E2, L1) still fragments, replace the allocator with an explicit table of fixed slots | removes one 22 GB mid-phase allocation: ≈ 1.3 s at 10¹¹ (m; cost of that allocation in batch 4); memory-neutral; removes the fragmentation risk | 2026-09-24 (main session) | M | medium: layout correctness | A contingency **inside PLAN L1 / E2**, not a new line. Do not start unless the L1/E2 layout fragments. Note: the sweep's "E2" is the allocator layout, not §7 E2 (`t_cap`) |
| S-5 | **Push-form broadcast for B** | Push instead of pull for the B tier's plane broadcast. Each APU writes its quarters to the other three; the Phase-2 bench gave 697 GB/s node-wide for the push, and B's pull ran at ≈ 51 GB/s per APU | not quantified (no model or run) | 2026-09 (S13, "not built"; date not resolved) | M | low | Overlaps **B6** (B4's transfer as a push overlapped with the transform) and **§7 H4** (xGMI push saturation). Treat as a question to answer with H4, not an item |
| S-6 | **HIP graphs for small-dispatch tiers** | Capture repeated small launches (batch and LEAF tiers) as a graph to cut per-dispatch cost. Graphs were never tested; the note says this may lower the "fuse kernels under 30 µs" threshold | upper bound ≈ 4 µs × dispatches per run (assumed; the dispatch count is not measured). Per-dispatch cost itself is measured at 3.5–4.0 µs on the MI300A microbench (m) | undated (2fe672bd, MI300A microbench, "worth adding" table) | M | low: needs a switch; re-capture for variable sizes | Overlaps **§7 H6** (occupancy and launch configuration). Count the dispatches first (one bounded run); if the count times 4 µs is under 1 s, drop it **Measured, dropped (S37, results/S37.md).** |

**Caveat on the cross-references.** The E1 "candidate A" and the `MN_WAIT_STATS` run "S20"
named for S-2 were not found in the repo's docs at this commit. Before S-2 is sized, confirm
that run exists. The ID E1 in §7 means `t_primes`, and E2 in §7 means `t_cap`, so the
sweep's E-labels must be read in their own context.

Not added (already listed or rejected; see the sweep reports): the squaring path, the
reciprocal warm start, the lazy reduction, matrix cores, the RCCL all-to-all, the
per-device remap lock and the other items already in the lists.

## a37v1 comparison (A37CMP, 2026-10-07) — accepted by the user

Source: `results/A37CMP.md` (the a37v1 spec read item by item against ecalc). The user accepted the
recommendations of its §2 shortlist on 2026-10-07 (~20:35 EDT) and rejected the §3 items (decision register §2.4).
Labels: measured (m), modelled (mod), assumed (assumed). Run order: A37-R2 first, then A37-Q1 / Q4 / Q9 as the
measurements their items need; A37-R1 only after both its gates.

| id | what | benefit (label) | effort | gate / dependency | source |
|---|---|---|---|---|---|
| **A37-R2** (FIRST) | CPU microbench of a37v1's base-2^64 sub-range seed (with convert and combine) against ecalc's decimal seed loop (`bi_span_step`, 2.6 ns/limb), on 229-term spans of 128 limbs, one thread, no node | decides whether the seed can be fixed on the CPU; modelled seed 22 → 14–18 s at 6.44e10 (mod, low confidence); a37v1's leaf cost (201 us / 352 terms) suggests it may not beat ecalc (assumed) | S (½ day) | none; gates A37-R1 | A37CMP §2 R2 |
| **A37-R1** (SHELVED by the user 2026-10-08; was gated) | Revive the GPU seed kernel (TASKS 6.4 / E8 / B1; register PH9, S24): one thread or wave per 229-term span, written straight into the region arenas; CPU path kept as fallback; first a bench-only kernel reporting spans/s; behind an off-by-default switch | −7…−12 s at 576 (mod; init 20.6 s + seed wait 9.3 s today); about 0…−4 s at 10 nodes on aac7 (mod, the mapping binds there). Evidence: a37v1 N13 does 4.35e9 terms in 6 s (m, its spec §14), so 7.5e9 terms ≈ 10 s (mod, linear), against an ecalc CPU seed of ≈ 22 s (mod) | L (2–4 d) | **GATED** on A37-R2 (CPU rewrite must not already close the gap) and on A37-Q1 (one-node init timeline at 6.44e10 confirming the seed binds init). Risks: ordering against the VMM mapping; HIP calls from a second thread queue behind allocations (TASKS 6.4). Digits: exact integers, byte-identity checkable per span | A37CMP §2 R1 |
| **A37-R4** | Profile `dc` (2.8–4.1 s, S19B) to split D2H fetch, formatting and T1 residues (Q7); if formatting, a two-digit-table `fmt18` (a37v1 formats 4e10 digits in 0.5 s, m) | ≤ −3 s per node (mod; 0 if `dc` is T1 or D2H); also speeds `tools/unpack_digits` (off the clock) | S (2 h) | none; bit-identical | A37CMP §2 R4, §4 Q7 |
| **A37-R6** | Time the reciprocal chain per doubling for j < 34126 (`ECALC_LOG_CLOCKS`, S19B §5 #4; floor 2.28 s, m), then, if the timings show it, a one-APU (or CPU) small-j path in place of the four-APU distributed products | ≤ −2 s (mod; the 10 s mean excess is the wait already in S19B §5 #4) | S (measure), then M | pairs with D3 / S-2 (division overlap); measurement is A37-Q8 | A37CMP §2 R6, §4 Q8 |
| **A37-R5** (LOW PRIORITY) | Batch tile pipeline: two streams, pinned descriptors, fold `k_norm` and stitch (A27_BATCH_PIPE analogue: overlap tile i's CRT/merge with tile i+1's scatter/NTT) | ≤ −2 s per node (mod: merge 0.84 s + gaps at 4e10, ×1.6) | M (1–2 d) | **needs A37-Q9** (by-phase batch-tier run at 6.44e10) before any gain is trusted | A37CMP §2 R5, §4 Q9 |
| **A37-R9** | Ops hygiene: `archive/drivers/ecalc/a14_soak.sh` and `archive/drivers/ecalc/g13d_hang.sh` send SIGTERM to ecalc, wait up to 60 s, and SIGKILL only if still alive (KFD poisoning risk on a `kill -9`) | avoids a poisoned node (qualitative) | S (done 2026-10-07) | none | A37CMP §2 R9 |

**Measurements** (no code change; each is a bounded run on the target's share, one job at a time):

- **A37-Q1** One-node init timeline at 6.44e10 with `ECALC_INIT_TL=1` on the target. Settles whether the seed
  thread (≈ 22 s mod) binds init at 576 nodes, or the 16.5 s reference init does. Gates A37-R1. (On aac7 it cannot
  answer this: the mapping is slow there.)
- **A37-Q4** NOP-modmul build of the 2^31 forward NTT (a37v1 HBM floor 51 ms; ecalc 99 ms, m) to split the remaining
  gap into structure against modmul; also whether a WbPowTab-style twiddle seed (a37v1 #58) matters. No gate; informs
  the NTT work only.
- **A37-Q9** By-phase batch-tier breakdown at 6.44e10 on 10 nodes (`BS_LAYOUT` / phase timers). a37v1's batch tier is
  21.0 s at 4e10 (m); ecalc's last full by-phase breakdown is at 9.5e10 (25.1 s, m). Gates A37-R5 (and the R7/R8
  gains, which are scaled from 4e10 N-kernel data).

**Status 2026-10-07 (S21, results/S21.md, RESULTS §121; recommendations only, decisions are the user's):**

- **A37-R2: DONE.** The b64k32 method gives no one-thread gain (ratio 0.86-1.07, identical digits); all-core 6-8 s vs 9-15 s (dec18). The R1 gate stays open.
- **A37-Q1: DONE (aac7 single node).** Seed thread about 15 s without the pool wait (spans 10.7 s), not 22 s; seed and mapping finish together on aac7. Modelled for the target: seed ends about 16-17 s vs the 16.5 s init floor.
- **A37-R1: recommended NO-GO now** (gain about 0-6 s, mod, not 7-12); first run `ECALC_INIT_TL=1` once on the target's share.
- **A37-R4: DONE.** `dc` loop 4.1 s = fetch 26 %, reversal 12 %, T1 residues 55 %, T2 8 %; packed output means no formatting on the clock: fmt18 not worth it.
- **A37-R6: measured on one node.** j < 34126 totals about 0.02 s; no one-APU path justified; the 10-node floor (2.28 s) needs A37-Q8 (per-doubling clocks at 10 nodes). Odd: chain start-up 4.14 s vs 0.43 s between two runs.
- **A37-Q4: DONE.** 2^31 forward real 90.4 ms, NOP 76.7 ms (modmul 13.7 ms, 15 %); 4 passes, the extra pass (about 17-20 ms, mod) is the remaining lever. **A37-Q9** and **A37-R5**: not run.

**Status 2026-10-08 (S27 / S25 / S26, results/S27.md, RESULTS §123; recommendations only):**

- **XEFF X1 (rot): NULL** (ABBA B-A +3.1 s, CI -1.0..+7.2). **X3 not built.** **X2 (INTER2 + VSLOT_POOL): in S28.** COMM_XSTATS done: direct exchanges 101 s per thread, 61 % skew; node 1 enters the reciprocal ~20 s late (new item below).
- **NEW XEFF-6a: reciprocal-chain lateness of node 1 (~19 s idle on nine nodes, m).** Diagnose with one `ECALC_LOG_CLOCKS=1 COMM_XSTATS=1` 10-node run, then replicate/overlap the chain (switch, off by default). Top recommendation for the 10-node hold.
- **A37-Q9: DONE** (batch tier 21.3 s at 10 n, 20.6 s at 1 n); **A37-R5: not worth it** (realistic -1.5..-3 s). S25 ladder 6.441/7.0/7.64e10: 410.6/464.4/528.5 s VERIFY OK; memory 28.4 GB per 1e10 + 173 GB fixed; 8.1e10 blocked by the vslot rule. A1 soaks: 975 of 975 ok.

**Status 2026-10-08 (S22 / S23 / S24, results/S22.md, RESULTS §122; recommendations only, decisions are the user's):**

- **D2 / MN_T_CHUNK_MB=2048: DONE on aac7.** S22's 16 new ABBA rounds: 16 of 16 negative, mean -40.6 s (CI -51.7 .. -29.5); pooled with S20 24 rounds, mean -42.3, median -35.1, 22 of 24 negative. Recommended: adopt on aac7 profile lines, target line unchanged (pool law 1024) until a target A/B.
- **A37-Q1: DONE x 6 (aac7).** Seed ends 0.55 s after the last mapping (co-bind); seed threads 96: total unchanged; BS_SEED_FILL=0 +7.6 s. **A37-R1: NO-GO** (target seed ends about 15.8 s vs 16.5 s init floor, mod; gain 0-1 s). One `ECALC_INIT_TL=1` run on the target is the open check (build only if the seed ends after about 18 s). **A37-R2** b_seed64 x 3: digits identical, all-core b64k32 1.15-3x shorter than dec18, no 1-thread gain.
- **A37-Q4 sweep and NTT_SIZE_STATS: DONE.** L24-L31 real / NOP table in S22.md §4.1; no whole 2^31 transform at 6.441e10 per node; whole >= 2^27 transforms 2.5 s per APU at 10 nodes.
- **NTT3P / 3-pass 2^31 NTT: E0 done, NO-GO.** 13/9/9 forward 90.6 ms vs today 89.9 (gate <= 80 ms); 9-stage passes 32.4 / 28.5 ms vs model 22.5; correctness of b1r<13> swizzle OK (468 checks, plan equals production at 2^22, 2^31). Stop unless the user wants a 2-3 day tuning try of the 9-stage body.
- **A37-R6 / dm variance:** S21's 4.14 s Newton start-up did not recur in 6 runs (0.44-0.61 s; dm 33.4-34.3 s); 10-node per-doubling clocks (A37-Q8) still not run.


**The user's decisions of 2026-10-08 (on the S22 / S23 / S24 analysis):**

- **`MN_T_CHUNK_MB=2048`: ADOPTED on the aac7 base line** (`DM_MN_LEAN=1`, `DIST_CHUNKS` 8, `MN_T_CHUNK_MB=2048`; `ecalc/e16_headline.sh`, README, 00_OVERVIEW updated). Evidence: S22 D2, 24 paired ABBA rounds, mean −42.3 s, median −35.1 s, p≈0.04 (m). **The target launch line stays 1024**; optional target A/B "2048 vs 1024, 2 runs each" is `docs/TARGET_TASKS.md` T11b.
- **S-2 division overlap: SAVED as an option** (not built). Gate: build only if the target wait-stats run shows the exposed division/reciprocal wait above about 30 s per APU thread (modelled at 576: ceiling 25–34 s, realistic gain 6–17 s, below the bar). Recorded in `docs/TARGET_TASKS.md` T12.
- **A37-R1 GPU seed: SHELVED** (user 2026-10-08). S22 §3: n = 6 timelines, seed ends 0.55 s after the last mapping on aac7 (co-bind), seed threads halved changes the total by 0; on the target the seed is modelled to end at about 15.8 s against the 16.5 s init floor (gain 0–1 s). Reopen only if the target check (T12) shows the seed ending after about 18 s and more than 2 s after the last mapping.
- **NTT3P / 3-pass 2^31 NTT: SHELVED** (user 2026-10-08). E0: 13/9/9 forward 90.6 ms vs 89.9 ms today (gate ≤ 80 ms); 9-stage passes 32.4 / 28.5 ms vs 22.5 modelled.
- **Target check stage:** the user wants the target to check S-2 and A37-R1 anyway: `ecalc/target_kit.sh --only build,s2chk` (opt-in; 2 and 8 nodes, 6.441e10 digits/node, `ECALC_INIT_TL=1 MN_WAIT_STATS=1`, verdict lines in `KIT_SUMMARY.txt`). Needs an `ecalc` with `MN_WAIT_STATS` (branch s22 / `d3-wait-stats`, not yet in `main`).


**Not in this list.** Items rejected on 2026-10-07 (A37-R3, R7, R8, and the six a37v1 choices) are in the
decision register §2.4, with the reason for each. A37-R3 stays a caution: the a37v1 pitfall (1 corrupt run in 18
with fresh anonymous pages and concurrent GPU writes, m) is not shown to apply to ecalc, whose runs use
`COMM_SHMEM_DEVHEAP=1` and whose target uses `comm_ofi`.

## Status 2026-10-08 (S30/S31 analysis, RESULTS 124)
- S30 (chain broadcast): done, null; cause is a ~20 s stall of node 1 before the chain (results/S30.md). Open: host-vs-rank test + db_from_bi timestamps.
- Segfaults d30_r4s2_A / x2_r3s2_B: open; set ECALC_SEGV_TRACE=1 in all 10-node drivers; candidate race in dbig.c vmm_bg_map tail.

## Status 2026-10-09 (S31/S32/S33, RESULTS 125)
- S31 (X2): done: -34.1 s all rounds / -31.7 s clean at 10 nodes, +0.8 GB; adoption is the user's decision (results/S31.md).
- S32 (host vs rank): done: the 20-25 s stall follows host x9000c1s0b1n0; eviction untested (dd did not drop the cache). Parallel soak still running at 08:20 EDT.
- S33 (2-node VMM_SAFE=2 soak): running; so far base 1 of 142 crashes, VMM_SAFE=2 0 of 141; with S32, VMM_SAFE=2 shows no benefit (3 of 731 vs 1 of 732).
- Open: addr2line of the SEGV traces, synchronous-mapping soak, exclude x9000c1s0b1n0.

## Status 2026-10-10 (S27-S34 wrap-up, RESULTS 126)

- `main` contains s34 and s29 (--no-ff). X2 is adopted on the aac7 line (`ecalc/e16_headline.sh`); optional target try = TARGET_TASKS T13. Rejected: X1 rot, X3 DC off, CHAIN_BCAST, VMM_SAFE=1/2 (docs/code/05_DECISION_REGISTER.md 2.6).
- `ECALC_VMM_BG=0`: final soak 4/422 vs 0/420 segfaults (Fisher p 0.063 one-sided, 0.124 two-sided, suggestive), +20 s; off by default, undecided (results/S34.md).
- Slow host x9000c1s0b1n0: opt-in `MNRUN_EXCLUDE_HOSTS` (mnrun.sh, rundriver.sh); `docs/AAC7_ADMIN_NOTE.md` for the admins (the user sends it). Low MemAvailable is not unique to that host (7 of 13 nodes ~441 GB, 6 ~520 GB).
- Kit: opt-in stage `nodechk` (TARGET_TASKS T14), not yet rehearsed on aac7.
- aac7 QOS is now 6 nodes per user: no 10-node runs.


## Deferred to a future session: the multi-node segfault (user, 2026-10-10)

The current focus is optimising for the node counts we can get on aac7 at once (QOS: 6 per user). The crash work
is parked here:
- **Symptom:** segfault in `hipMemMap` called from the background mapper `vmm_bg_map` (`ecalc/dbig.c`), about 1 % of
  2-node runs (4/422) and about 3.5 % of 10-node runs (3/~85) (m); about 0.4–0.5 % per node per run, so at 576 nodes a
  run would almost always crash (≥ 90 %, mod, if the per-node rate holds). **A fix is required before the target.**
- **Candidates:** `ECALC_VMM_BG=0` (synchronous mapping, branch s34, on main): 0/420 vs 4/422 (Fisher p 0.063 one-sided),
  +20 s per 2-node run (m). `ECALC_VMM_BG=2` (branch s35 015994a: one process-wide lock around the VMM calls and 24
  wrapped HIP calls): gate passed, 0 crashes in 18 A / 17 C runs so far (~/s35_part1, ~/s35_part2; no evidence yet).
- **Next step:** a fast reproducer (e.g. smaller map chunks → many more `hipMemMap` calls per run) so a fix can be judged
  in hours; a plain soak needs ≈ 600 runs per arm (≈ 55 h on 4 nodes) to tell a ≥ 90 % reduction from none.


## Status 2026-10-10 evening (RESULTS 127)

**Done:**
- S35 crash-fix build (`ECALC_VMM_BG=2`, gate passed), S36 VSLOT_SHARE (adopted), S37 (S-3/S-6 dropped), S38/S40/S41 kernel defaults (ADDSUB2, MAXIDX_TOP, QSEL on), S39 survey, S43 multi-node check (2 and 4 nodes: -50.0 s at 4 nodes), S44 aac6 CPX coverage, S45 576-node plan and host harness.
- Single node at 6.441e10 digits: 94 s -> about 78 s (m).

**Open:**
- 10-node runs are impossible under the aac7 QOS (6 nodes per user).
- QSEL confirmation on aac6 with 4 APUs: done (S42, merged; see the next bullet and open item 2 above).
- Next kernel candidates from S39: `k_addsub2` tile / wave-scan (1.0-1.5 s, mod), `k_modq` 128-bit `%` (about 1.4 s GPU, mod), `k_crt_batch` small grid (about 0.8 s, mod).
- X2 cut-group pool-offset asymmetry found by S45 (not a target risk): understand and document.
- Crash work (hipMemMap in `vmm_bg_map`) deferred; the fix is still required before the target (see the deferred section above).
- Scaling study at 2 / 4 / 6 nodes: later. Handoff document: later.
- 2026-10-10 late: aac6 S42 (results/S42.md) confirms ADDSUB2 (−11.6/−12.5 s), MAXIDX (−2.8/−2.6 s), QSEL (−1.49/−1.09 s) on two 4-APU nodes. The one QSEL=1 hang (n1_r6_4B, after init) did not recur in 44 repeat runs (0/44 QSEL=1, 0/14 QSEL=0): 1 hang in 64 QSEL=1 runs on aac6, cause unknown; watch for start-up stalls in future soaks.
