# apumult vs ntt/ — analysis F2: categories B (madvise), C (in-place/view/steal), E (instrumentation/safety), F (kernel/algorithm variants)

Source read in full: `/home/machinus/apucode/apumult.md` (130 lines). apumult's source is not available; every "what it
does" below is reconstructed from the row's name and the doc's one-line mechanism. Every ntt/ claim is grepped/read in
the tree at `/home/machinus/apucode/ntt` (file:line). Labels: **measured** (a RESULTS/results number with its job),
**modelled** (`ecalc/mem_model.py` / `binsplit.c dm_layout`, the same formula to the byte per M13), **assumed** (code
reading, no run). No code was changed.

Sizes used throughout (n_Q = Q's limbs × 8 B per node, modelled by `mem_model.dm_layout`, decimal, 3 primes, 2³¹ cap):

| size | n_Q (limbs) | n_Q (GB, whole node) | k_µ (GB) | t1 cap (GB) | bs regions (GB) | dm need = arena (GB) | planes (GB) | node peak modelled (GB) |
|---|---|---|---|---|---|---|---|---|
| 7.4 × 10¹⁰ per node (the 576 share) | 4.11 × 10⁹ | 32.9 | 32.9 | 65.8 | 173.7 | 229.6 | 103.7 | 346.5 at size 1; **452** at 576 (RESULTS §82: + top scratch, exchange, SHMEM, host 30) |
| 10¹¹ one node | 5.56 × 10⁹ | 44.4 | 44.4 | 88.9 | 242.9 | 310.3 | 103.7 | 427.9 (measured 431.2 device at 4 primes, M11) |
| 1.3 × 10¹¹ one node (the measured 2³¹ edge, P13b) | 7.22 × 10⁹ | 57.8 | 57.8 | 115.6 | 296.5 | 403.4 | 103.7 | 522.0 (measured 522.8, P13b) |
| 2.35 × 10¹¹ one node (apumult's d_max) | 1.31 × 10¹⁰ | 104.4 | 104.4 | 208.9 | 553.9 | 729.2 | 103.7 | 851 — 1.6 × the node |

## 0. The fact that decides every row in B and C

apumult is host-resident: P, Q, µ, r, r², A, T are separate `malloc`'d host bigints, each mapped when first touched and
each holding its pages until freed; "dead" pages therefore cost RSS until `madvise(MADV_DONTNEED)` returns them, and the
RSS *is* the peak. ntt/ is device-resident with **one arena per APU mapped at init** (`binsplit.c:227-236 arena_get`,
sized by `dm_layout` `binsplit.c:318-338` as max(2 bs parities, dm need, tree need)) that is **donated whole to the
block pool** (`binsplit.c:141-162 donate_one`, `dbig.c:149-156 db_donate_ext`), from which every dbig quarter is carved
(`dbig.c:107-124 q_alloc_locked`) and to which it returns on `db_free` (`dbig.c:127-133`, coalescing extents). Since M11
v4 the pool does **zero hipMalloc inside the phases** at every size measured (M11: 4 × 10¹⁰, 8 × 10¹⁰, 10¹¹; M13 to
0.05 %). So on our side:

* "dead pages" of a device number are already *free pool bytes* the moment `db_free` runs — nothing to release to the OS,
  and releasing would not lower the peak: **the node peak is the mapped capacity, fixed at init** (planes + arena +
  tables + host), not a running RSS.
* the only way a B-class technique lowers our ceiling is by lowering **the arena's formula** — i.e. by making the dm
  phase's *capacity* peak smaller: reserving temporaries at their used size instead of their final size, and freeing
  operands at their last use *inside* a phase. Those are the rows worth anything below; the madvise mechanism itself
  is not.
* what apumult buys with madvise on *partly* dead buffers (µ[d/2], the tail of an axpool) is on our side a
  `db_reserve`-at-the-right-size question (`dbig.c:200-238`): a dbig's capacity is one block per quarter, and a quarter
  is carved to exactly ceil(limbs/4) rounded to `DB_ALIGN`, no size classes — the pool already supports arbitrary sizes.

**Can device memory be "released" on the APU at all?** (question 3 of the brief)

| mechanism | applies to our buffers? | cost | verdict |
|---|---|---|---|
| `madvise(MADV_DONTNEED)` on a `hipMalloc` pointer | **No.** A `hipMalloc` range is an amdgpu BO mapped by the driver (a `VM_PFNMAP`/driver VMA on the CPU side, where it is CPU-visible at all); `MADV_DONTNEED` on such a VMA fails with `EINVAL` or drops only the CPU mapping — the BO's HBM backing stays allocated. It works only on kernel-managed anonymous memory, i.e. our `MEM_ALLOC=mmap`/`host` forms, which RESULTS §77 (I) disqualified (`hipMemcpy` 21 GB/s over SDMA). The one madvise we have is `MADV_HUGEPAGE` on host bigints (`bigint.c:17`, `mem.c:134,230,252`) | — | N/A (reasoning, not measured; no ROCm on this box to test) |
| `hipFree` + later `hipMalloc` | Yes, between phases (the pool does it for its own `kind == 2` regions at `db_release_pools`, `dbig.c:187-194`) | `hipFree` ≈ 3 ms/GB (RESULTS §22: 0.023 s per 8 GiB, measured); re-`hipMalloc` of memory the same process freed **0.035 s/GB** (results/I.md:28-30, measured: the TTM page pool keeps the cleared pages), a fresh mapping 0.057–0.072 s/GB (measured, §77 I). So a 100 GB release-and-remap costs ≈ 4 s | usable, but pointless unless another consumer needs the bytes *at that moment* (see below) |
| HIP virtual memory management (`hipMemAddressReserve` / `hipMemCreate` / `hipMemMap` / `hipMemSetAccess` / `hipMemUnmap` / `hipMemRelease`) | In principle: reserve the arena's VA once, back it with physical chunks (2 MiB granularity) and unmap/release dead sub-ranges, re-back later without moving anything | `hipMemCreate` pays the same page clearing as `hipMalloc` (the kernel clears on allocation: 0.057–0.072 s/GB fresh, 0.035 warm — assumed equal, not measured); `hipMemRelease` ≈ `hipFree` | **untested and the most interesting form** — not for releasing bytes but because it decouples contiguity from physical placement: the reserved-tail/hole policy (`dbig.c:60-105`, M11 §1) and the deterministic OOM at 1.245 × 10¹¹ (G13d: 41.2 GB free in 2 extents, largest 27.5, request 27.67) exist only because a block must be one contiguous `hipMalloc` range (`dbig.c:38`). With VMM the pool could back any contiguous VA range from any free physical chunks. Open: whether the API exists on ROCm 7.2.4 for the APU's HBM (it is in HIP since ROCm 5.x as the CUDA-VMM equivalent — assumed), whether peer access (the dist tier's xGMI pulls, `rns_dist.c:313`) works through `hipMemSetAccess` mappings, and its clearing cost. A `vmm` form in `ecalc/tests/t_alloc.c` (the harness already times alloc/fill/bw/ntt/d2d-peer/free per form) is ≈ 2 h |

**Who would consume released bytes?** On one node: nobody — the planes are live in every phase (the dm's products use all
of pool 0 and 3q of pool 1, results/M.md:84-86), the host is 12–16 GB, and there is one process. At 576: the SHMEM pool
(8.6 GB, fixed), the exchange scratch and the top-level slabs peak *in the same dm phase* as the pool. So "release" is
never the lever on the APU; **"reserve less" is** — and that needs no OS mechanism at all, only the pool we have.

**Where the dm phase's capacity is dead at its peak** (assumed from code reading; the per-iteration `NEWTON_VERBOSE`
line `newton_db.c:104` prints pool bytes, not live bytes, so the used fraction is not measured):

| moment | live blocks (capacity) | of which dead or unused at that moment | file:line |
|---|---|---|---|
| reciprocal, last doubling j = ⌈k/2⌉ → k, during Q_t·r and r·\|d\| | P 1.1 n_Q (the bs result, untouched until S = P + Q), Q 1.1 n_Q, r cap k+4, r2 cap k+4, t1 cap max(n_Q+k, 2k)+8 ≈ 2 n_Q, the grid's piece temporary 2³¹+8: **≈ 6.2 n_Q + 2³¹** = the `dm_layout` need (`binsplit.c:329`) | **P** (needs a spill, not a release — F1's Category A); **r** holds j ≈ k/2 of k+4; **t1** holds Q_t·r = n_Q + k/2 ≈ 1.5 n_Q of 2 n_Q (its cap `newton_db.c:70-71` is the division's Ah·µ size, 2k, reused through the pool); r2 holds d (≤ j+1 limbs) then r′ (k). Used ≈ 4.2 n_Q of 6.2; releasable by tight reservation (no spill): **1.0 n_Q** (r's top half + t1's top quarter) | `newton_db.c:67-72` "scratch at its final capacity, once"; the products `:81-82, :89` |
| division, t = A_h·µ | S 1.1, Q 1.1, µ k+1, t 2k, piece: **5.2 n_Q** | t's low k limbs are never written (B3's low cut skips those pieces, `rns_dist.c:851, 862-863`) but are allocated (`rns_dist.c:876 db_reserve(Cd, nc + 8)`): **1.0 n_Q** dead, a "dbig with a base offset" would drop it (effort M) | `newton_db.c:241-247` |
| division, low product X·Q | S 1.1, Q 1.1, X k+1, xq reserved nc+8 = 2 n_Q, piece: **5.2 n_Q** | **S**: only its low w − dl limbs are needed for the window (`newton_db.c:254 db_set_shifted_low(&Aw, S, w - dl, dl, w)`), and w − dl = n_Q + 2 − dl ≈ **5 limbs** (dl = ⌈(log₁₀N! − 50)/18⌉ + 1 against n_Q = ⌈log₁₀N!/18⌉ + 2): S is 1.1 n_Q of dead capacity through the whole low product (the driver owns it: `ecalc.c:539 db_free(&bs_Pd)` after the call); **xq** is reserved for the full product though only w + one piece can be written (`rns_dist.c:876` again): ≈ 1.0 n_Q − 2³¹ limbs dead | `newton_db.c:248-256` |
| bs, the last device-tier level (the v3 "top" term that actually binds the arena at our sizes: 77.2 GB per APU at 10¹¹ against the reciprocal's 74.2) | 2 P, 2 Q of n_Q/2 (inputs), P, Q of n_Q (outputs), + 1/8, + the hole (t1's quarter kept free for contiguity): **≈ 6.95 n_Q** | the children are freed only after the whole level (`binsplit.c:1223-1224`); after P = P1·Q2 + P2 (`:1213-1215`) P1 and P2 are dead while Q = Q1·Q2 (`:1216`) runs: **1.1 n_Q** releasable per pair | `binsplit.c:1208-1226`, `dm_layout` v3 term `binsplit.c:333-335` |

## 1. Category B — madvise(DONTNEED) rows

| env / M# | what it does (apumult, host-resident) | our analogue (file:line) | doc verdict | our verdict | GB at our sizes (per node; modelled unless said) | worth testing? |
|---|---|---|---|---|---|---|
| `DM_A_MADVISE` M30 — madvise A after mul_hi, defer Q-restore | A = 10^d (P+Q) is dead after the high product A_h·µ except its low window; its pages are dropped before the low product X·Q, and Q's restore from spill is delayed to after that | We never form A (`ecalc.c:512` S = P + Q in place, "A = S B^dl is never formed"); A_h is a **view** of S (`newton_db.c:242`). But S plays A's role and is **held through the low product** although only ≈ 5 limbs of it are needed (`newton_db.c:254`) | "N/A (A never exists in dec l3)" | **wrong — APPLICABLE**: free S (the driver's `bs_Pd`) once `Aw` is extracted, before `rns_mul_low_db`. On the sharded path the window is `mdb_shift(S, −dl, w)` (A-div) — the same holds there, not checked in `newton_mn` | S = 1.1 n_Q: **36 / 49 / 115 GB** at 7.4e10 / 1e11 / 2.35e11. Visible in the ceiling only once the reciprocal's capacity peak (6.2 n_Q) is below the division's (5.2 → 4.1 n_Q): a second-order item on its own | yes, as part of the "tight dm" bundle (below); alone it moves nothing |
| `DC_CORR_MADVISE` M20e-v4, `DC_AXTAIL_MADV` M33b, `DC_MU_MADV` M47a, `DC_AXPOOL_MADV_B2` A23, `DC_TOP_VAL_MADV` M43 | radix-conversion (dc) buffers: the correction scratch, the tail of the per-level pool, the top half of µ after mul_hi, the pool after the second level, the TOP values | The decimal default has no radix conversion; the binary path's `todec.c` (305 lines: divisor cache, TOP/MID/DEEP levels, `k_leaf`) keeps its level pools whole and frees the prewarm scratch at once (`todec.c:150 newton_free_scratch(); rns_free_scratch()`) | N/A-decimal; UNIQUE-binary | **agree**: N/A on the target path. The binary path is the regression oracle only (RESULTS §67 decision) — not worth work | 0 on the default path | no |
| `NEWTON_R2MADV` M36 — r² madvise post-use | the squared iterate's buffer is released after each use inside the Newton loop | r2 is reserved once at k+4 and held for the whole reciprocal (`newton_db.c:71`), freed to the pool at the end (`:117`); u, d, corr are views into t1 (`:85, :90`) — the doc's "uses views" is right for those, wrong for r2 | "Their recip uses views (superseded?)" | **PARTLY UNIQUE**: the equivalent on our side is reserving r, r2, t1 per doubling at their used size (r at j, t1 at n_Q + j) instead of "final capacity, once". The pool coalesces, the copies on growth are 2k limbs in all (HBM speed) | r's unused half 0.5 n_Q + t1's unused quarter 0.5 n_Q = **1.0 n_Q: 33 / 44 / 104 GB**. This is the first-order item: it is the reciprocal's capacity that sets `dm_layout`'s need (`binsplit.c:329`), and the dm need exceeds the bs regions by 56 / 67 / 175 GB at these sizes, so all of it lands in the arena until the bs regions bind | **yes — the top B item**; a sizing change, bit-identical by construction (`t_newton`, e9 regression); ≈ 1 day incl. the `dm_layout` formula and `mem_model.py` |
| `BS_CUR_MADVISE` M46 — bs-mdev per-pair madvise curQ+P | in the top (mdev) levels, each pair's consumed children are released as soon as their two products are done, not at the end of the level | device-tier top levels: children freed after the whole level (`binsplit.c:1223`), the region parity donated after it (`:1224`); the batch tier levels write the other parity and the consumed parity is donated only at the first device level (`:1210`) | UNIQUE | **agree (not done)**, and it matters because the v3 top term is what binds the arena at our sizes (77.2 vs 74.2 GB per APU at 10¹¹, `binsplit.c:333-335`): free P1, P2 after P = P1·Q2 + P2 (`:1214-1215`), Q1, Q2 after Q (`:1216`) — per pair, and at the top level's single pair | 1.1 n_Q on the top term: **41 / 55 / 129 GB** (× 1.125 of the fit slack) — but the arena is max(top, recip, bs), so the gain realised = the smaller of this and the recip item: together they lower the arena by ≈ 1.0 n_Q | **yes**, with the r/t1 item (they must land together or the other term binds) |
| `BATCH_PAIR_INMADV` M42i — bs-batch per-tile curQ madvise | in the batch levels, the inputs of a finished tile are released while the level runs | our batch levels read parity A and write parity B, both mapped in the arena (2 halves, `binsplit.c:232`); parity A is dead after the level but its space is reused only two levels on | UNIQUE | **agree, but the payoff is elsewhere**: releasing a consumed tile helps only if the *output parity can grow into it*, i.e. a ring layout of one parity + a tile instead of two parities — a redesign of the WP3 region layout (`region_need`, `place_node` `binsplit.c:170-183`) and of the batch tier's write offsets. And the bs regions do not bind the arena at our sizes (173.7 < 229.6 at 7.4e10, 242.9 < 310.3 at 10¹¹), so it pays only after the dm items above | up to ≈ ½ of the bs regions: 87 / 121 GB — realised 0 until the dm need is below the bs regions; after the two items above the arena would be bs-bound at 10¹¹ (243 vs ≈ 253–266) and this becomes the next lever | later; L effort |
| `BS_CURQ1_MADV=268M` M40G — bs-mdev-lvl0 Q1 madvise | the first mdev level's Q1 released early (a special case of the per-pair rule with a size threshold) | as `BS_CUR_MADVISE` | UNIQUE | folded into the per-pair item | included above | with the per-pair item |
| `MDISP_KXK_BAND` M17f — mul_dispatch band-write + T/P madvise (~76 GB at 10dP) | the 10dP product T·(P+Q) formed as a k×k grid writing only the band of limbs the division needs, with the operands' consumed pieces madvised | **there is no 10dP product on the decimal path** (a limb shift, `ecalc.c:512-513`); on binary it is `pow10_big` + `rns_mul` through the host Karatsuba split (`ecalc.c:519-521`, `rns_mul.c:508`). The band capability itself exists in our grid: `grid_piece_skipped(oa, ob, la, lb, lowcut, w)` takes both cuts (`rns_dist.c:851`) and `rns_mul_dist_mn_cut(lowcut, highcut)` exposes both at the mn tier (`:1545-1551`) | "Partial (§66 grid-split, no madvise)" | **N/A on the default path** (the doc mis-files it: the 10dP phase is 0 s on decimal, RESULTS §80's tables). The band product as a *capability* is EQUIV | 0 | no (see F for the band's real use: inside the reciprocal) |

**B, the net at our sizes (modelled from the `dm_layout` formulas; the "used" fractions assumed from the code):** the
two first-order items (tight r/t1 in `recip_db`; per-pair release in the device-tier top level) lower the arena's need
by ≈ 1.0 n_Q per node while the bs regions stay below it: arena **229.6 → ≈ 197 GB at 7.4 × 10¹⁰ (−33), 310 → ≈ 266 at
10¹¹ (−44), 729 → ≈ 625 at 2.35 × 10¹¹ (−104, still 1.5 × the node)**. At the node-peak slope of ≈ 3.1 GB per 10⁹
digits per node (mem_model between 7.4e10 and 1e11), −33 GB at 576 is ≈ **+1.0 × 10¹⁰ digits per node (7.4 → ≈ 8.4 ×
10¹⁰; 4.25 → ≈ 4.8 × 10¹³ total)** before the grid steps and the exchange terms are re-checked; on one node at the 2³¹ cap
the measured edge 1.30 × 10¹¹ (P13b) would move to ≈ 1.4 × 10¹¹. The second-order bundle (free S before the low product,
xq at w + one piece, t without its dead low half) is another ≈ 1 n_Q but only once the first bundle is in and the hole
rule (`binsplit.c:322-323`, t1's quarter reserved for contiguity) is re-derived from the smaller t1. **The doc's "madvise
between dbig ops: +1–2e10" (its item 6) is therefore about right in size, wrong in mechanism**: no madvise, a sizing
rule.

## 2. Category C — in-place / view / steal

| env / M# | what it does | our analogue (file:line) | doc verdict | our verdict | GB | worth testing? |
|---|---|---|---|---|---|---|
| `DM_R_INPLACE` M38 — R.limb = A.limb view | the remainder R is formed in A's own limbs (the window), no separate R buffer | `newton_db_divmod_shifted`: the window Aw is formed by `db_set_shifted_low` (`newton_db.c:254`) and **R is Aw itself or xq itself** after the subtraction (`:257-262`: `Rd = Aw; db_init(&Aw)` / `Rd = xq; db_init(&xq)`) — no R buffer; R's residues are taken on the device and R freed (`:272-274`). The separate `bi_reserve(R, w + 1)` is the older host-output path `newton_db_divmod` (`:184`), not the default flow (`ovl3`, `ecalc.c:539`) | **UNIQUE** — "ntt/ dm has R separate (device)" | **wrong — EQUIV** on the default path | 0 (already had) | no |
| `BS_TP_INPLACE` M24a — bs Tp/T → nxtP alias-add | P = P1·Q2 + P2 accumulated into the product buffer, no temporary | device tier: `db_add(o->pd, o->pd, &P2)` in place (`binsplit.c:1215`); batch tier: the add on the host then one DMA (`:1246-1248`); the grid itself accumulates pieces in place (`rns_dist.c:906 db_add_shifted(Cd, &t, oa + ob, Cd)`) | EQUIV | **EQUIV** | 0 | no |
| `BS_STEAL_PQ` M50 — result P/Q ← cur.buf steal | the final level's P, Q take the level buffer instead of being copied out | `bs_Pd = *nxt.nd[0].pd; bs_Qd = *nxt.nd[0].qd` (`binsplit.c:1227`, the block moves, no copy); the copy exists only in the non-device flow (`:1318-1319`) and at size > 1 for the leaf hand-over (`:1306`: a deliberate copy so the regions can be donated before the tree, B6) | EQUIV | **EQUIV** | 0 | no |
| `NEWTON_QTVIEW=2` M36 — Q_t view (no copy) | the top `take` limbs of Q used as an operand without copying | `dbig qt = db_view(Qd, nq - take, take)` (`newton_db.c:78`); views are not owning (`dbig.c:701-704`) and the dist tier packs them at their offset (`rns_dist.c:46, 201`) | EQUIV | **EQUIV** | 0 | no |
| `P10_SWAP` M23b — pow10 swap-not-copy | the pow10 tower's buffers swapped instead of copied | no pow10 on decimal; on binary `pow10_big` (`ecalc.c:519`) — not looked at, the path is not the target | EQUIV (trivial) | N/A-decimal | 0 | no |

Category C is fully covered on our side; the doc's one UNIQUE is a misreading of the older host-output routine.

## 3. Category E — instrumentation & safety

What we have (grep-verified): the per-phase memory tables `mem_report` at init/bs/tree/recip/division/dm/end with device
categories planes / regions / pool:donated / borrowed / hipMalloc / **live / peak-live** / free / tables and host RSS /
HWM / staging / X / digits (`mem.c:359-406`; summary table `:407-421`); `MEM_REPORT_DEVS=1` adds per-APU rows with the
driver's used/total (`mem.c:394-400`, `hipMemGetInfo`); `mem_oom` prints the whole accounting on a refused allocation
(`mem.c:163-167`) and the pool guard does the same before `exit(1)` on an in-phase growth (`binsplit.c:213`,
`rns_mul.c:215`); the pool logs every in-phase `hipMalloc` with free/extents/largest/live (`dbig.c:111-116`,
`DB_POOL_VERBOSE`); the intra-phase pool high-water mark `g_peak_live` (`dbig.c:44-45`, the `pl:peak` column); the
per-iteration Newton trace (`NEWTON_VERBOSE`, `newton_db.c:104`: j, take, r.n, pool GB); per-call product lines
(`RNS_VERBOSE`: `dist_db … : s` `rns_dist.c:914`, `dist_mn` `:1527`) and per-phase sub-timer sums (`recip(db): dist N
calls, load / ntt / crt / spills; dbig shift / addsub / maxidx / reserve` `newton_db.c:108-110`; `divmod(dev): µ, A µ +
shift, X out + low product, window + corrections, R residues` `:277-278`); `DIST_STATS`, `COMM_LAYER_STATS`
(`comm_layered.c:37-49`); `RESULT` lines (`ecalc.c:42`); `VmRSS` after init / before dc (`ecalc.c:163, 327`); the
G13d batch runner with a time watchdog (`g13d_run.sh:24`, `timeout -s TERM 3·est + 120`) and the gdb variant that
captures `rocm-smi --showmemuse`, per-thread `wchan` and the log tail on a hang (`g13d_hang.sh:33`); pre-flight sizing
without a device (`BS_LAYOUT_ONLY`, `ECALC_INIT_ONLY`, `ECALC_PLANE_CAP=fit` against `ECALC_NODE_GB`, `estimate.py
--target`, `MN_PLAN_ONLY`).

| item | apumult | our analogue | doc verdict | our verdict | worth it? |
|---|---|---|---|---|---|
| `pollv3.sh` phase-aware RSS watchdog (TH = 372 GB "ch-live", 452 GB "sticky-DEEP"; kills before OOM) | a sidecar polling RSS with thresholds per phase, killing the run before the kernel's OOM killer does | none at run time; the guard is **pre-flight**: the layout formula equals the C request to the byte at 35 points (M13) and the measured device total to 0.05 % at 10¹⁰–10¹¹, the budget 480/524 GB (`docs/TARGET.md:30`), `fit`. The two failure modes seen: (a) a refused `hipMalloc` → `mem_oom`, clean exit 1 with the report (G13d 1.245 × 10¹¹, deterministic, pool fragmentation); (b) the kernel OOM killer (SIGKILL) when device + host HWM ≈ 529 GB, twice in P13b (1.46 × 10¹¹ at 2³⁰, 1.16 × 10¹¹ at 3·2³⁰), both **in bs right after the seeds**, where the host RSS grows 12 → 16 GB — the one term the model carries as a constant (`BS_HOST_INIT_BYTES`, `binsplit.c:381`) | UNIQUE — "critical for pushing d_max safely" | **SUPERSEDED for the device part** (a run that will not fit is refused before it maps; the mapped total is known at init and does not grow), **residual for the host part** (+4 GB in the seeds, the SHMEM pool T0, TCP staging at size > 1: M13 open issue 1). "cpuset-OOM would wipe": on slurm the job step is its own cgroup, and in P13b the kernel killed `ecalc` itself, not the node; on a shared node (the M-run's three processes at 10¹⁰) it could take a neighbour. A killer does not improve the outcome at 576: a SIGTERM'd rank leaves the other 575 in a collective as much as a SIGKILL'd one — the fix there is a collective abort on a memory check, which `mn_barrier`/exit-5 machinery could carry | a *sampler* yes (next row); a killer no — keep the budget margin (452 modelled of 480) and the pre-flight instead |
| hwm-poller-v2 (5 s RSS + HWM + state + log tail) | continuous sampling | phase-boundary tables only; the intra-phase pool HWM is captured (`pl:peak`) but RSS between boundaries is not, and nothing samples the driver's used bytes over time | UNIQUE (continuous) | **agree** — the host RSS growth in the seeds (P13b) and the device total during the tree at size > 1 are seen only at the boundaries today | **yes, S (≈ 1 h)**: a thread in `mem.c` sampling `VmRSS` and `hipMemGetInfo` per APU every N s into the log (`MEM_SAMPLE_S`), or the same as a shell sidecar in `g13d_run.sh` (`ps`/`rocm-smi --showmemuse` — the hang capture already runs this once). Attribution when pushing the edge on the target |
| `[bspk]`/`[dcpk]` intra-level RSS probes | per-level RSS/HWM print | per-level lines at `ECALC_VERBOSE=2` (times, tiers, grids) without a memory column; `NEWTON_VERBOSE` has pool GB per doubling (`newton_db.c:104`) but **pool bytes, not live bytes** | UNIQUE (finer) | **partly agree** — one column short: add `g_live_bytes` (already accounted, `dbig.c:44`) to the per-level and per-doubling lines. This is also what would *measure* the used-at-peak fractions the B estimates assume | yes, T (10 lines); do it before any B work |
| `[batch]`/`[mdev]` per-call sub-timers | ntt/cc/rp/h2d/d2h/join per call | per-call `dist_db`/`dist_mn` lines and per-phase sums of load/ntt/crt/spills (`newton_db.c:108`, `rns_dist.c:914, 1527`), `DIST_STATS` per part, `COMM_LAYER_STATS` stage timeline | "Partial (DIST_STATS, less granular)" | **wrong — EQUIV or finer** (the G13d 2³⁰ analysis used exactly these: 452 vs 48 s of load over 510 calls) | no |
| `subphase_sum.sh` post-run Σ | a script summing the sub-timers | `results/g13d/g13d_table.py` (tables from the logs: pieces, dist calls, phases), the `recip(db)`/`divmod(dev)` sum lines themselves | UNIQUE | mostly covered; a generic `Σ` over the `RNS_VERBOSE` lines would be a 30-line script | T, when needed |
| ENV_L3 / ENV_DMAX split | two env files: the µ benchmark vs the d_max push | one default (13c) plus `docs/TARGET.md`'s launch line and `DESIGN_TABLE.md`'s "largest" row (`B4, 2^30, both chunkings, depth 1`) | UNIQUE | a documentation convenience; the design table already names the d_max profile | T, optional |
| d-wall progression table (per-d HWM by phase, d160–d240) | measured points along d | **G13d §(a): 22 sizes from 5.12 × 10¹⁰ to 1.252 × 10¹¹ with device GB, phases, pieces, dist calls, every run VERIFY OK** (`results/G13d.md:65-92`); P13b's edge table at four caps | "Model-only (`mem_model.py`)" | **wrong — we have it, measured** | no |
| `DC_DMN_PROBE` | a probe in the dc divmod node | binary-only, N/A | UNIQUE | N/A on the target path | no |

## 4. Category F — kernel / algorithm variants

| item | apumult | ours (file:line) | doc verdict | our verdict | new to us? |
|---|---|---|---|---|---|
| `GPUCRT_COOP` C4b — 16-group cooperative `cf_place` in the CRT carry | a cooperative-groups carry placement | `k_crt_batch`: Garner per coefficient into LDS for 256 coefficients, the 3-limb carry window by thread 0 over LDS, coalesced store, contributions past the block into a spill buffer resolved by the chunk-flag carry kernels (`rns_mul.c:697-708`, `rns_dist.c:594, 700`; `dbig.c` chunk flags) | SUPERSEDED (ours "likely faster") | **different; unmeasurable against theirs** — ours is measured in the transform budget (crt column of every `recip(db)` line); nothing to port without their source | no |
| `GPUCRT_RLNUMA` C6 — `numa_alloc_onnode` for the CRT output | NUMA-local host buffers | `mem_hstage_alloc`: first-touch by threads pinned to the APU's NUMA node + `hipHostRegister` (`mem.h:5-8`, `mem.c:129`) | EQUIV | **EQUIV**, and moot on the decimal device flow (the CRT output never reaches the host; the staging serves seeds and checkpoints only, `rns_mul.c:190-198`) | no |
| `CRTCAR_GPU_MDEV` C1 — mdev-tier GPU CRT carry (broken there) | | dist and batch tiers: CRT + carry on the device; the CPU `crt_carry_par4` (`crt.c:133`) only in the host mdev tier (binary top levels, tests) | "Different; ntt/'s dist-tier CRT is device" | agree | no |
| GPU-LEAF v4 A19.22 — Barrett ÷10¹⁸ leaf kernel | | `k_leaf` with `div1e18`: µ = ⌊2¹²³/10¹⁸⌋, q_est off by ≤ 3 (`todec.c:22-38`) — the paper's kernel | EQUIV | **EQUIV** (binary only) | no |
| `mul_dispatch_hi/lo/band` M14/M52k/M17f — truncated products with operand madvise | hi = the top limbs only, lo = the low limbs only, band = both cuts; consumed operand pages madvised | `rns_mul_low_db` (w cut, `rns_dist.c:784-791`), `rns_mul_high_db` (low cut, `:863`), both cuts in one predicate (`grid_piece_skipped` `:851`) and exposed together at the mn tier (`rns_mul_dist_mn_cut` `:1545`); the division uses both by default (`NEWTON_LOWPROD`, `NEWTON_HIGHPROD` on unless 0, `newton_db.c:18, :243, :251`) | "UNIQUE band+madvise variant" | **EQUIV as a capability** (madvise part N/A). **But the reciprocal does not use any cut**: `Q_t·r` (`newton_db.c:81-82`) is formed whole though only `t1 >> (take − j)` is read (`:85`), and `r·\|d\|` (`:89`) whole though only `t1 >> j` (`:90`). A low cut at take − j and at j would skip the pieces wholly below the cut: at the last doubling ≈ 1/6 of the Q_t·r pieces and ≈ 1/4–1/3 of the r·d pieces (the lower-left triangle of the grid) | **yes — the doc's `NEWTON_MULLO` row says "EQUIV", which is true for the division and false for the reciprocal.** Exactness: a skipped piece lowers the view by ≤ (skipped + 1) units at the cut limb (A-div's B3 argument); in the correction form that perturbs r′ in its lowest limb, healed by the next doubling and, at the last one, absorbed by the division's ±Q corrections (≤ 64, `newton_db.c:259`). Needs the argument written and `t_newton` + e9/4e10 identical; changes the repeat/overshoot statistics, not the digits. Gain: the reciprocal is 23 s at 7.7 × 10¹⁰ and 47–78 s at 1.03–1.16 × 10¹¹ (G13d, measured); ≈ 20 % of its pieces skipped → −5 … −15 s at 10¹¹ (assumed) |
| `MDISP_KARA/3X3` A23 — Karatsuba / 3×3 at the mdev-serial split | the over-plane product as 3 (2×2) or 5–6 (3×3, Toom-3) products instead of 4 / 9 | host tier: `mul_karatsuba` / `mul_chunked` for products over 2^POOL_LOG points (`rns_mul.c:499-596`, the Q1 decision, RESULTS §35); **device tier: the cost-minimising grid, k_a × k_b full piece products** (`split_grid_cap` `rns_dist.c:800-813`, `mul_grid` `:866-919`); Karatsuba in the device tier was sized once for the 2×2 case only and shelved because decimal's half-sums did not fit a plane (RESULTS §60 lines 2467-2470, DECISIONS I5 `DECISIONS.md:310`, PLAN I5 "binary only −2.5 s") | "Different-but-EQUIV" | **not equivalent, and the shelving argument is stale.** A k×k grid is k² piece products; Karatsuba recursion over the same pieces is k^1.585 (8×8: 27 vs 64; 4×4: 9 vs 16), Toom-3 fewer. The "half-sums do not fit a plane" objection applied when the product was 2 pieces; at 10¹¹ the reciprocal's and division's products are 6×9 … 8×10 pieces (G13d) and at 576 the critical path is 185 pieces (§82) — the sums of two *pieces* are one limb longer than a piece and fit. Cost: dbig adds/subs (HBM-bound, cheap next to a 2³¹-point product at ≈ 1.2 s) and **memory**: the half-sums and the z1 temporaries ≈ 1–2 n_Q of pool at the top level — it trades bytes for time, the opposite of B | **yes, as a time item for the µ configuration, not for d_max**: a 2×2 Karatsuba layer on top of `mul_grid` (3 sub-grids of half operands instead of 4 quadrants; recursive on the half-products) is ≈ 1–2 days. Model first with D2's per-piece calibration (`mn_model.py`: 1.15–1.22 × per one-plane product + 0.08 s per extra piece per 2³¹ limbs) — at 8×10 → ≈ 35 products the dm phase at 10¹¹ (113 s measured) could lose ≈ 30–40 % (assumed). At 576 the tree's k-way levels are Horner chains of unequal operands (`tree_need_dev`, `binsplit.c:296-316`), where Karatsuba applies only to the square parts |
| `NEWTON_MULLO=1` — Newton uses mul_lo | | see the band row: the division's X·Q is the low product (`NEWTON_LOWPROD`, default on); the reciprocal's products are whole | EQUIV | **EQUIV for the division, NOT for the reciprocal** | yes (above) |
| `NEWTON_RTRUNC=1` M37 — r truncated at iteration 34 for the STEP fix | a fixed truncation at one iteration to land on the target precision | the anchored chain lands exactly on k (`NEWTON_ANCHOR=1`, `newton_chain_next` `newton_db.c:48, :75`); the trailing `db_shr_limbs` for j > k is the fallback (`:106`) | SUPERSEDED (k-anchored) | **agree** | no |
| `DM_PRERECIP=1`, `DM_MHS_HOOK` — µ pre-computed, mhs hooks | the reciprocal computed before the dividend exists (memory), with hooks | "dm part 1: the reciprocal of Q first, while A does not exist yet (memory peak)" (`ecalc.c:485-506`); hooks: `newton_db_x_hook` (the writer on X before the low product, `newton_db.c:124-126`), `bs_after_seeds_hook`, `rns_after_staging_hook`; on binary the seeded prewarm `todec.c:132-141` | EQUIV | **EQUIV** | no |
| `PW_FUSE=1`, `FWD_FUSE=1` | the pointwise product fused into the inverse's first pass; the repack fused into the forward | `ntt_pw_fuse = 14` (`ntt.c:19`), the fused inverse in the batch tier (`rns_mul.c:841`), `DIST_PW_FUSE` in the dist tier (`rns_dist.c:40-43`, bit-identical), `NTT_B1R`/`NTT_PLAN` (13b K) | EQUIV | **EQUIV** | no |
| `DIVCACHE_MUSQ` A21 — µ(2h) = µ(h)² + touch | the divisor cache's reciprocals by squaring the previous | `todec.c:134-141` (`dec_seed_prewarm`, "one doubling, repeats if it has to"); §39 | EQUIV | **EQUIV** (binary only) | no |

## 5. Ranked: what to test (F2's categories only; F1 owns the spill items)

1. **Tight reservation in `recip_db` + per-pair release in the device-tier top level, and `dm_layout` re-derived from
   it** (B: `NEWTON_R2MADV` + `BS_CUR_MADVISE` analogues). −1.0 n_Q of arena per node: **−33 GB at the 576 share
   (452 → ≈ 419 modelled), −44 GB at 10¹¹ on one node**; ≈ +1.0 × 10¹⁰ digits per node at 576 (modelled, before the
   grid steps). Bit-identical by construction. Effort ≈ 1 day; gate: `t_newton`, e9 both bases, 4 × 10¹⁰ identical,
   `mem_model.py --check-c` exact again, an 8 × 10¹⁰ run with `MEM_REPORT_DEVS=1` showing the smaller arena and
   hipMalloc 0.
2. **Live-bytes column on the per-doubling and per-level lines** (E, T): 10 lines using `g_live_bytes`; it turns the
   "used at peak" fractions of §0 from assumed into measured and decides how much of item 1 is real. Do it first.
3. **The reciprocal's two products with a low cut** (F: `NEWTON_MULLO`/band): time, not memory; −5…−15 s of the
   reciprocal at 10¹¹ (assumed, ≈ 20 % of its pieces); needs the exactness argument and the repeat statistics checked.
   Effort ≈ ½ day behind a switch (`NEWTON_RECIP_CUT`).
4. **A `vmm` form in `tests/t_alloc.c`** (B, the APU memory-API question): reserve/map/unmap/release timings, peer
   access through `hipMemSetAccess`, the transform rate. ≈ 2 h. If it works, the block pool's contiguity constraint
   (the hole/tail policy, the deterministic OOM at 1.245 × 10¹¹) can go — a fragmentation fix, not a byte saving.
5. **Karatsuba over the device grid** (F): a µ item that costs memory; model it with D2's per-piece calibration
   before writing anything (≈ 2 h of `mn_model.py`), then a 2×2 layer in `mul_grid` if the model says ≥ 20 % of the dm
   phase.
6. **Continuous memory sampler** (E, S ≈ 1 h): RSS + `hipMemGetInfo` per APU every few seconds; for the target's edge
   runs and the host-side residuals (seeds +4 GB, SHMEM pool, TCP staging).
7. The second-order dm bundle (free S after the 5-limb window, xq at w + one piece, t without its dead low half):
   another ≈ 1 n_Q but only after item 1 and a new hole rule; effort M. Defer until item 1 is measured.
8. Not worth it: an OOM-killing watchdog (pre-flight sizing is the guard; a kill helps no one at 576), the batch-tier
   in-place ring (L effort, pays only after the dm need is below the bs regions), everything `DC_*` (binary path), the
   ENV split (documentation).

## 6. Doc verdicts corrected (summary)

| row | doc | ours |
|---|---|---|
| `DM_A_MADVISE` | N/A | applicable: S is held through the low product for a 5-limb window |
| `NEWTON_R2MADV` | "views (superseded?)" | partly unique: r, r2, t1 are reserved at final capacity, once |
| `MDISP_KXK_BAND` | Partial | N/A on decimal (no 10dP product); the band capability exists |
| `DM_R_INPLACE` | UNIQUE | EQUIV (R formed in the window, `newton_db.c:257-262`) |
| pollv3 watchdog | UNIQUE, critical | superseded by pre-flight sizing for the device part; residual host terms only |
| per-call sub-timers | Partial | EQUIV or finer |
| d-wall progression | model-only | measured (G13d, 22 sizes) |
| `mul_dispatch band` / `NEWTON_MULLO` | UNIQUE band / EQUIV | capability EQUIV; the reciprocal's products are uncut — a real gap |
| `MDISP_KARA/3X3` | Different-but-EQUIV | not equivalent (k² vs k^1.58 products); our shelving argument predates the many-piece grids |

## 7. Porting plan — how each adoptable item goes into the device-resident code, and what it gives

**Target-node evidence (per the integrator's update: apumult ran on the target's own MI300A nodes, with `/ssd0`).**
What their numbers tell us that our aac6 runs could not: (i) a host-resident process reached 475 GB of RSS at d235 and
their watchdog thresholds were 372 / 452 GB of RSS — so on the target the *node* budget usable by one process is at
least 475 GB with the host side at 2 B/digit, consistent with our measured edge "device + host HWM ≲ 524 GB" (P13b) and
above our 480 GB design budget; (ii) `madvise(MADV_DONTNEED)` on host memory returns pages on those nodes at a rate
their techniques found worth ~150 GB — fine, but for us the relevant consequence is the **same-HBM rule**: on the APU the
host's RSS and the device's `hipMalloc` draw from one physical pool (that is why our edge is a *sum*). So a byte released
host-side is a byte the device can map, and vice versa — but *moving* a buffer host-side saves nothing by itself, and a
host-resident operand is read by the device tiers more slowly (RESULTS §55/56: device pools beat registered host pools;
§77 I: host-backed copies 21 GB/s). Placement is therefore never the port; the port is one of three mechanisms:

* **P1 pool-level release** — `db_free`, or a new `db_shrink(x, n)` that returns the quarters above limb n (a dbig's
  quarter d holds limbs [d·qc, (d+1)·qc), `dbig.c:488-492`; the pool coalesces any size, `dbig.c:127-133`): this is
  exactly "madvise the tail of a buffer" for a dead *top*, at quarter granularity, no OS call, 0 cost. It lowers the
  ceiling only through `dm_layout`'s formula (the arena is sized at init), so every P1 item comes with its formula term.
* **P2 HIP VMM** — the arena as reserved VA backed by physical chunks (`hipMemAddressReserve` + `hipMemCreate`/`hipMemMap`
  + `hipMemSetAccess` for the 4 APUs; `hipMemUnmap`/`hipMemRelease` for a dead sub-range, re-map later). Cost per GB
  released and re-backed ≈ the warm re-allocation 0.035 s/GB + `hipFree`-class 3 ms/GB (measured for `hipMalloc`;
  assumed equal for VMM). Gives bytes back to the node pool (host or device) *and* frees the block pool from contiguity.
* **P3 host-side + DONTNEED** — only for buffers that are host-resident anyway (the seeds' staging, the writer's chunks,
  the checkpoint staging): all already sized to their use or released at their last use (`ECALC_STAGING`, RESULTS §72;
  `rns_release_staging` `ecalc.c:491`). Nothing left to port here.

| item (apumult) | port into ntt/ | mechanism | gives (per node, modelled) | effort / risk |
|---|---|---|---|---|
| `NEWTON_R2MADV` (r² released per use) | `recip_db` (`newton_db.c:70-72`): reserve r, r2 at j + 4 and t1 at n_Q + j + 8 per doubling (grow through `db_reserve`, which copies the live limbs: ≈ 2k limbs over the whole chain), keep the final-iteration sizes r2 = k + 4, t1 = n_Q + ⌈k/2⌉ + 8; `dm_layout` (`binsplit.c:329`) and `mem_model.dm_layout` take the new capacity (P + Q + r/2 + r2 + 1.5 n_Q + piece); the tail (`hole`) becomes t1's new quarter | P1 | arena −1.0 n_Q: **−33 GB at 7.4e10, −44 at 1e11, −104 at 2.35e11**; ≈ +1.0 × 10¹⁰ digits per node at 576 | 1 d; bit-identical (the values are the same, only capacities change); risk = fragmentation at the last doubling's growth (the tail policy handles the one large block; `DB_POOL_VERBOSE` shows any fallback) |
| `BS_CUR_MADVISE` / `BS_CURQ1_MADV` (per-pair release of consumed children) | device-tier top levels (`binsplit.c:1208-1226`): `db_free` P1 and P2 after `P = P1·Q2 + P2` (`:1214-1215`), Q1, Q2 after `Q = Q1·Q2` (`:1216`), per pair; at the *first* device level the children are region-pool views (`node_p/node_q`, `:91-92`) inside a parity that is donated whole after the level (`:1224`): donate the parity at the level's start with every child's range registered as a live block (`live_add` on `[po, po+pn)`, `[qo, qo+qn)`), then `db_free` the ranges as consumed — the pool's extents merge within one region record (`ext_insert`, `dbig.c:47-56`); `dm_layout`'s v3 top term (`:333-335`) becomes outputs 2 n_Q + the larger surviving pair n_Q/2 + 1/8 + hole. At 576 the top levels are `mn.c tree_level_k` (Horner over shares): whether a consumed child's share is freed per product is **not checked** here | P1 | top term −1.1 n_Q × 1.125: **−41 / −55 / −129 GB**; realised jointly with the row above (the arena is the max of the two terms) | ½–1 d; the region-as-blocks trick touches the pool's accounting (`g_live` table 8192 entries — a level has ≤ 16 nodes at the device tier, fine) |
| `DM_A_MADVISE` (A dead after mul_hi) | `newton_db_divmod_shifted` (`newton_db.c:248-256`): after `db_set_shifted_low(&Aw, S, …)` the caller's S is dead — pass ownership (`newton_db_free_S` flag, the driver's `db_free(&bs_Pd)` at `ecalc.c:539` moves inside), or `db_shrink(S, w − dl)` keeping quarter 0 only; sharded path: free S's share after `mdb_shift(S, −dl, w)` (A-div) | P1 | the division's low-product peak 5.2 → 4.1 n_Q (**−36 / −49 / −115 GB** at that moment); counts toward the ceiling only when the division's term is the arena's max, i.e. after the two rows above and the `t`/`xq` row | ½ d; no numeric change |
| `DC_MU_MADV` (µ's dead top half) | our µ is freed at its last use already (`newton_db.c:245 db_free(&mu)` right after A_h·µ); the *product* buffers carry the dead halves instead: `t` (2k, low k never written under B3) and `xq` (nc + 8 reserved for a w-limb result): in `mul_grid` (`rns_dist.c:876`) reserve `min(nc, w + pa + pb) + 8` when w < nc (one line); for `t`'s low half a dbig whose limbs below `lowcut` are not stored (a base offset in `dv`, `dbig.c:25`; `db_shl_limbs`/`db_add_shifted` honour it) | P1 | xq: ≈ 1.0 n_Q − 2³¹ limbs (**−16 / −27 / −87 GB**); t: 1.0 n_Q (**−33 / −44 / −104**) at the A_h·µ moment | xq 1 h; t's offset-dbig 1–2 d, medium risk (every dbig primitive) |
| `BATCH_PAIR_INMADV` (consumed tiles released during a batch level) | a ring layout of the batch levels: one parity plus a tile's worth of slack instead of two parities (`arena_get` `binsplit.c:227-236`, `region_need`/`place_node` `:170-183`), the tile's outputs written over its consumed inputs after the tile's scatter (`rns_mul_batch_local`) | P1 (layout) | bs regions ≈ −½: **−87 / −121 GB**; realised only when the bs regions bind the arena, which is after the dm rows above (at 7.4e10: 173.7 against the tightened dm ≈ 197) | L (1 week), high risk (WP3 subtree ownership, checkpoints write region pools `binsplit.c:598`) — last |
| any dead range not at a quarter boundary; the pool's contiguity failures | `MEM_ALLOC=vmm` form for the arenas (`mem.c:117-149 mem_dev_malloc`): reserve VA once, back with 1 GiB chunks; the pool's `ext_take` no longer needs a block inside one `hipMalloc` (`dbig.c:38`); dead sub-ranges (a parity after bs, r/t1's top) released by `hipMemUnmap` + `hipMemRelease`, re-backed by `hipMemCreate` + `hipMemMap` when the pool needs them | P2 | bytes: the same as P1 (the physical pool is what counts), *plus* the dm extra (56 / 67 GB) need not be mapped at init (init −3…−4 s at 0.057 s/GB, measured cost of mapping) and the reserved-tail policy and the 1.245 × 10¹¹ fragmentation OOM disappear | probe 2 h in `t_alloc.c` (API presence on ROCm 7.2.4, clearing cost, peer access, transform rate); the pool change 2–3 d if the probe passes |
| `MDISP_KXK_BAND` (band product + operand madvise) | the band capability exists (`grid_piece_skipped` with both cuts, `rns_dist.c:851`); the port is to *use* the cut in the reciprocal: `rns_mul_high_db(&t1, &qt, &r, take − j)` for Q_t·r (only `t1 >> (take − j)` is read, `:85`) and `rns_mul_high_db(&t1, &r, &r2, j)` for r·\|d\| (only `t1 >> j`, `:90`), behind `NEWTON_RECIP_CUT`; the operand-madvise half is P1's `db_free` of r2's d after the product (it is already reused for r′) | time, not bytes | ≈ 20 % of the reciprocal's pieces skipped: −5…−15 s of 47–78 s at 1.0–1.16 × 10¹¹ (assumed); at 576 the reciprocal is `recip_mn`'s share-level products, the same cut applies (`rns_mul_dist_mn_cut`) | ½ d + the exactness note; digits expected identical, the repeat/overshoot counters may change — gate on `t_newton`, e9, 4 × 10¹⁰ |
| `MDISP_KARA/3X3` | a Karatsuba layer over `mul_grid`'s pieces (3 half-products per 2×2, recursive), sums as dbig adds (`db_add`), differences in place; memory +1–2 n_Q of pool at the top level, so only under a `RNS_KARA=1` µ-profile switch, never in the d_max profile | P1 (adds scratch) | time: k² → k^1.585 piece products; at 8×10 pieces (10¹¹) ≈ −30…−40 % of the dm's products (assumed; model with `mn_model.py`'s per-piece calibration first) | 1–2 d after the model; medium risk (the grid's cache slots and cuts interact) |
| `pollv3` watchdog / hwm-poller / `[pk]` probes | (a) a sampler thread in `mem.c` (`MEM_SAMPLE_S`): VmRSS, `hipMemGetInfo` per APU, the current phase, every N s, one log line; (b) `g_live_bytes` on the per-doubling (`newton_db.c:104`) and per-level (`ECALC_VERBOSE=2`) lines; (c) at size > 1 a *collective* budget check at each `mem_report` boundary (device + RSS against `ECALC_NODE_GB`), exiting through the existing refused-restart path (exit 5 on every node) rather than a local kill — the only form of "kill before OOM" that helps 576 nodes | instrumentation | attribution at the edge; (b) turns this document's assumed used-at-peak fractions into measurements; (c) prevents a one-rank SIGKILL from stranding the job | (a) 1 h, (b) 10 lines, (c) ½ d |
| `ENV_L3`/`ENV_DMAX`, `subphase_sum.sh`, d-wall table | env files `ecalc/env_mu.sh` / `env_dmax.sh` from `DESIGN_TABLE.md`'s fastest / largest rows; a 30-line Σ over `RNS_VERBOSE` lines; the d-wall table exists (G13d) | docs | workflow only | T |
| `DM_R_INPLACE`, `BS_TP_INPLACE`, `BS_STEAL_PQ`, `NEWTON_QTVIEW`, `P10_SWAP`, `GPU-LEAF`, `PW/FWD_FUSE`, `DIVCACHE_MUSQ`, `RLNUMA`, `PRERECIP` | nothing to port (EQUIV, cited in §2 and §4) | — | 0 | — |
| `DC_*` rows | binary path only; not the target | — | 0 | — |

**What the whole F2 bundle gives if every P1 row lands** (arena = max(bs regions, top', recip', division')): at the
576 share the arena falls from 229.6 to ≈ 174–197 GB (the bs regions then bind at 173.7), i.e. **−33 … −56 GB per node
of the modelled 452**, ≈ +1.0 … 1.8 × 10¹⁰ digits per node; on one node at 2³¹ the edge 1.30 × 10¹¹ moves to ≈
1.4–1.5 × 10¹¹. apumult's 2.35 × 10¹¹ per node stays out of reach without F1's spill items (the bs regions alone are
554 GB there). All modelled from the `dm_layout` formulas with assumed used-at-peak fractions; the first measurement to
take is row (b) of the instrumentation port.

Label recap: n_Q, k, t1, bs regions, dm need — modelled (`mem_model.dm_layout`, = the C request to the byte, M13);
device totals at 4 × 10¹⁰ / 8 × 10¹⁰ / 10¹¹ / 1.3 × 10¹¹ — measured (M11, P13b); the reciprocal's used-at-peak
fractions and the division's dead ranges — assumed from `newton_db.c` and `rns_dist.c`; the allocation costs — measured
(RESULTS §22, §77 I, results/I.md); the VMM API's presence and cost on ROCm 7.2.4 — assumed; the digit gains — modelled
from the node-peak slope, not run.
