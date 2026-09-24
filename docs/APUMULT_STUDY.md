# APUMULT_STUDY.md — what the apumult techniques are, what they give us, and how to test them

Written 2026-09-24 from a study of `apucode/apumult.md` (a 130-line summary of another code, "apumult", that computes e
by the same algorithm). The user reports that its measurements were taken **on the target system's nodes** (MI300A,
node-local SSD at `/ssd0`), so its numbers are treated as target-node evidence and the goal is **adoption where
possible**. The apumult source was not available; each technique was reconstructed from the summary.

Four agents did the work: F1 and F2 (Fable) decoded the six categories against our source; R did online research on
MI300A memory and storage; M measured the disk and memory behavior on an aac6 node (job 21121). Their reports are
`apumult_{F1,F2,R,M}.md` in the session scratchpad; the numbers below cite them. Every number is labeled
**measured**, **modelled** (`mem_model.py`, which matches the C layout to the byte and measured device totals to
0.05–0.1 %) or **assumed**.

The user's decision rule for trading time for digits (from the Phase 13 design table): **% more time per 1 % more
digits**. The floor with unlimited memory is ≈ 1.1 (the work grows like d·(log d)²); options at ≈ 1.6 were rejected;
crossing a grid step costs ≈ 9.

---

## 1. Conclusions

1. **The memory techniques are real and new to us, but they act differently here.** apumult keeps its numbers in host
   memory and frees pages one buffer at a time. We map all device memory **once, at init**, in one arena per APU,
   sized by a formula for the worst phase (`binsplit.c dm_layout`). Freeing a buffer only returns it to our pool, so a
   spill or a release saves memory **only if the init-time formula shrinks with it**. The lever is "reserve less", and
   every port below is "change the formula and move the data", never "release pages".
2. **One node: +28 % digits at a good price.** Spilling P to SSD during the reciprocal, together with a change to where
   the reciprocal's scratch is placed (V2 below), raises the one-node ceiling from 1.30 to 1.68 × 10¹¹ digits (cap 2³¹)
   and from 1.44 to 1.79 × 10¹¹ (cap 2³⁰), modelled. The cost is **1.15–1.39 % time per 1 % digits** at 10 down to
   2 GB/s of disk, inside the user's threshold at every disk rate measured or plausible. The writes hide behind
   compute; the read-back is the visible cost.
3. **A no-disk saving that also helps the target.** Tighter reservations in the reciprocal and a pair-by-pair release in
   the tree's top level (apumult's `NEWTON_R2MADV` and `BS_CUR_MADVISE`, translated) cut ≈ 1 unit of Q's size from the
   arena: **−33 GB per node at the target (452 → ≈ 419 GB), −44 GB at 10¹¹ on one node**, bit-identical, no time
   cost, modelled. That is ≈ +1 × 10¹⁰ digits per node of headroom at 576.
4. **576 nodes: spilling buys margin, not digits.** At 576 the tree's top level becomes the limit (≈ 294 GB per node) as
   soon as the division's memory drops, so the same spills give only +5 % per node (1.4–2.5 % time per 1 % digits),
   and any size past 4.29 × 10¹³ also crosses the grid steps. **apumult's projection of 235 × 10⁹ × 576 = 1.35 × 10¹⁴
   does not hold for our code**: our per-node ceiling at 576 is 55–70 % of the one-node ceiling (exchange scratch, the
   SHMEM pool, the tree's top-level products), and a node at 2 × 10¹¹ would compute for 15–30 minutes.
5. **apumult's density, 2.0 bytes per digit, is reachable only with a deeper change** (V6: the reciprocal's product
   written only for the band that is used, and the division's low product done in chunks). With V2/V3 our floor is
   ≈ 2.3 bytes per digit.
6. **Two speed ideas in the summary are real gaps**: the reciprocal's two products are computed in full though only a
   band of each is read (a cut skips ≈ 20 % of their pieces), and Karatsuba over the product grid is worth
   re-modelling now that grids reach 6 × 9 to 8 × 10 pieces.
7. **Two side findings from the measurements** (not in the summary, both cheap):
   - **Our checkpoint writer should use O_DIRECT.** Buffered writes run at 0.5–0.65 GB/s on aac6 (writeback
     throttling) against 2.0 GB/s with O_DIRECT, which likely explains the 0.31 GB/s checkpoint rate of RESULTS §77–§78
     (not yet confirmed by a checkpoint run with O_DIRECT).
   - **Reading or writing big files through the page cache costs HBM** and slows the next large allocation (a 448 GiB
     `hipMalloc` took 76.9 s instead of 36.5 s with 200 GiB of clean cache present). `fsync` + `posix_fadvise(DONTNEED)`
     returns it at once; O_DIRECT never uses it. This is the likely cause of the 5–15 % slowdowns seen after the 40 GB digit
     comparisons (consistent, not proven).

---

## 2. Facts about the node that decide every port (measured on aac6 unless marked)

| fact | evidence |
|---|---|
| Host and device memory are the same HBM. The page cache and `/tmp` (on aac6, `/` is the NVMe) are HBM too | M: a 64 GiB buffered write took MemFree 494 → 429 GiB |
| Buffered file I/O fills the page cache with HBM. `fsync` then `posix_fadvise(DONTNEED)` gives it back immediately; O_DIRECT never touches it | M: Cached 64.8 → 0.8 GiB, 200 → 0.3 GiB; flat under O_DIRECT |
| A large `hipMalloc` still succeeds with clean cache present (the kernel evicts it) but takes 2× as long; dirty pages must first be written back at ≈ 0.5 GB/s | M: 448 GiB in 76.9 s vs 36.5 s |
| Oversubscribing kills the process (OOM killer), it does not return an error | M; also P13b jobs 21064, 21069 |
| `hipMemGetInfo` does **not** see the page cache or host memory on the APU: use `/proc/meminfo` | M |
| O_DIRECT **cannot** use a `hipMalloc` pointer (EFAULT) | M, R |
| A double-buffered pinned bounce (2 × 1 GiB `hipHostMalloc`) moves device data at full disk speed: 2.24 GB/s write, 6.22 GB/s read (one APU); 2.05 / 5.03 GB/s with four APUs sharing the disk | M, every read-back verified |
| `hipMallocManaged` memory can be the O_DIRECT buffer directly: 2.57 / 6.24 GB/s | M |
| aac6's disk (Samsung 980 PRO 2 TB): O_DIRECT 2.0 GB/s write sustained over 200 GiB, 5.9 GB/s read; one thread saturates it | M |
| `madvise(MADV_DONTNEED)` on host memory returns HBM that a later `hipMalloc` can use (200 GiB back in 5.5 s) | M |
| `madvise` cannot free `hipMalloc` memory | R, F2 |
| HIP's virtual memory API (`hipMemAddressReserve/Create/Map/Unmap/Release`) can release and re-back parts of a reserved range; re-backing costs ≈ 35–40 ms per GiB (page zeroing, like `hipMalloc`); it is still a Beta API with open issues on MI300 | R (not yet tested here) |
| The target's `/ssd0`: model and rate not found (HPE datasheets blocked). A PCIe Gen4 M.2 drive of that class: ≈ 2–4 GB/s write, 5–7 GB/s read | R, assumed — **measure on the target** |
| Storage fast enough to stream multiplication operands would need ≈ ¼ of memory bandwidth (y-cruncher's rule); we have 1/2,000–1/7,000 of it. Only "park a buffer that is dead until a later phase" is viable, which is apumult's pattern | R |

---

## 3. The six categories, item by item

### A. SSD spill and stream (apumult's M-series)

| apumult item | what it does | our counterpart | verdict |
|---|---|---|---|
| `DM_P_SPILL` (M32) | P is not needed during the reciprocal: write it out, read it back for S = P + Q | P is live through the reciprocal in our arena too | **adopt**, as V1 + the tail policy = **V2** (§4). P alone gives zero here: the bs top level's formula (v3) binds until the reciprocal's reserved "hole" moves into the dead half of the top level's inputs |
| `DM_Q_SPILL` (M29e) | Q is cold between the last products and the division's low product | Q is cold from the last reciprocal iteration's second product through the division's high product | **adopt in V3**, worth it at ≥ 5 GB/s (2³¹) or at the 2³⁰ cap |
| `DM_MU_SPILL` (M33a), `DM_MU_STREAM` (M49p) | µ spilled around a "10dP" step, streamed into the high product | we have no 10dP step; µ is consumed right after it is made | not applicable |
| `DM_Q_STREAM` (M52k) | the low product streams Q from the spill | each grid piece would read Q once per grid row: exposed I/O | not now; part of V6 |
| `DM_RECIP_RSPILL` + stream-shl (M51) | r is dead between r² and 2r − Qr² | our k-anchored reciprocal has no dead r; the real counterpart is **r2 reserved at full size for the whole reciprocal though written only after the first product** | adopt the late r2 reservation (in V3, and in §4's E2) |
| M56 (designed) | spill the bs top level's fresh Q and P₂ between its two multiplies | the same exists at our top level | < 1 % on one node after V2; the lever at 576 (E8) |

### B. Releasing dead pages (`madvise(DONTNEED)`)

Our buffers are `hipMalloc` arena memory: `madvise` does nothing to them, and the arena size is fixed at init. The
translation is **tighter reservation** plus a smaller `dm_layout`:

| apumult item | our counterpart (F2, file:line in its report) | verdict |
|---|---|---|
| `NEWTON_R2MADV` (M36) | r, r2, t1 reserved at final capacity for the whole reciprocal (`newton_db.c` 67–72) though the last doubling uses ≈ 4.2 of 6.2 units | **adopt (E2)** |
| `BS_CUR_MADVISE` (M46), `BS_CURQ1_MADV` (M40G) | the device tier's top level frees its children only after the whole level (`binsplit.c` 1223) | **adopt (E2)**: release pair by pair |
| `BATCH_PAIR_INMADV` (M42i) | the batch tier's per-tile buffers | pays only once the bs regions bind; not now |
| `DM_A_MADVISE` (M30) | no A buffer in the decimal flow | not applicable |
| `MDISP_KXK_BAND` (M17f) | no 10dP product | not applicable; its "band" idea is V6 |
| `DC_*` (five items) | the binary pipeline's radix conversion | not applicable to the decimal default |

### C. In place, views, steal

All already equivalent in our code. The one row the summary marks unique, `DM_R_INPLACE`, is not: our remainder R is
formed in the window itself (`newton_db.c` 257–262).

### D. Trim, free, tight sizing

| apumult item | verdict |
|---|---|
| `DC_TRIM*` (divisor-cache trim and rebuild), `DEC_AXPOOL_FREE_N`, `DC_NXT_TIGHT`, `P10I_*`, `DEC_BFS_TOP_N` / `DEC_LEAF_THRESH` | binary pipeline only; not applicable to the decimal default |
| `BATCH_DEVDAB_TIGHT` (plane sizing) | superseded by `ECALC_PLANE_CAP` (four caps plus `fit`) |
| `DEVDAB_MID_FREE`, `MDEV_RELEASE_DC` | equivalent: planes are released after the division, pools are donated |
| `DM_TRIM` | our counterpart is the 10 % bound margin on Q and S: trimming it gives +2 % digits at no cost (E4) |

### E. Instrumentation and safety

| apumult item | verdict |
|---|---|
| phase-aware RSS watchdog that kills before the OOM killer | not needed as a killer: our device memory is sized before the run (`BS_LAYOUT_ONLY`, `ECALC_INIT_ONLY`, `ECALC_PLANE_CAP=fit`, `estimate.py`). What we lack is a **collective** budget check at 576 (one node over budget should stop the job cleanly, with its rank named) — fold into TASKS A3 |
| continuous memory sampler | **adopt** (≈ 1 h): RSS + `/proc/meminfo` (not `hipMemGetInfo`, which cannot see the page cache) every few seconds, for edge runs |
| per-level RSS probes | **adopt** as a live-bytes column on the per-doubling and per-level lines (≈ 10 lines): it turns the "used at the peak" fractions behind E2 from assumed into measured. **Do first.** |
| per-call sub-timers, measured d-wall table | we have them (`newton_db.c` 108, `rns_dist.c` 914; G13d measured 22 sizes) |
| separate configs for speed and for maximum size | workflow; `estimate.py --target` and `design_table.py` serve it |

### F. Kernel and algorithm variants

Confirmed equivalent or superseded: the GPU CRT variants, the GPU leaf kernel, the fused passes, the seeded divisor
cache, `NEWTON_RTRUNC` (our k-anchored form is better), `DM_PRERECIP`. **Two are real gaps:**
- **`NEWTON_MULLO` / band**: our division cuts its products, but the reciprocal's two products are computed in full
  (`newton_db.c` 81–82, 89) though only `t1 >> (take − j)` and `t1 >> j` are read. A cut skips ≈ 20 % of their pieces:
  −5…−15 s of the reciprocal at 10¹¹ (assumed). Needs the exactness argument (E5).
- **`MDISP_KARA/3X3`**: our device grid costs k² piece products; Karatsuba needs k^1.585. It was shelved when grids
  were 2 × 2 (RESULTS §60); at 10¹¹ they are 6 × 9 to 8 × 10. Costs memory (+1–2 units); model before writing (E7).

---

## 4. The proposal: experiments in order

Every experiment keeps the digits bit-identical and goes behind a switch, off by default; adoption is the user's.
Gates for every code change: `t_newton`, the e9 step in both bases, a 4 × 10¹⁰ run identical to the reference,
`mem_model.py --check-c` exact, the regression, and the target of each item measured with `MEM_REPORT_DEVS=1`.

| # | experiment | gives (modelled unless marked) | cost | effort |
|---|---|---|---|---|
| **E1** | **Live-bytes column** on the per-doubling and per-level lines | measures how much of each reservation is used at its peak: decides E2's real size | none | ≈ 1 h |
| **E2** | **Tight reservation**: r2 reserved only when first written, r and t1 sized per doubling, the top tree level released pair by pair; `dm_layout` and `mem_model.py` re-derived | **−33 GB per node at the target (452 → ≈ 419 GB); −44 GB at 10¹¹ on one node; ≈ +1 × 10¹⁰ digits per node of headroom at 576** | none (no disk) | ≈ 1 day |
| **E3** | **O_DIRECT for the checkpoint writer and every large file** (the top set, the output, the reference compare), or `fsync` + `posix_fadvise(DONTNEED)` | checkpoint writes 0.3–0.6 → ≈ 2 GB/s on aac6 (measured rates); no page cache left to slow the next allocation | none | ≈ ½ day |
| **E4** | **A spill primitive**: generalize `ckpt_dbig_io` (`binsplit.c` 596) and the background writer (`bs_ckpt_bg_*`) into spill/restore of a dbig range: O_DIRECT through the pinned bounce, four APUs in parallel, a `ECALC_SPILL_DIR` switch (the target's `/ssd0`), `db_free` on completion, `db_reserve` + read-back on restore | the tool for E5–E6 | — | ≈ 1 day |
| **E5** | **V2**: P spilled during the reciprocal, restored for S = P + Q; the reciprocal's hole placed in the top level's dead half; `dm_layout` without the P term and the reserved hole | **one node 1.30 → 1.68 × 10¹¹ (2³¹), 1.44 → 1.79 × 10¹¹ (2³⁰)**, +28 %; at 576, margin 452 → 437 GB | **1.15 (10 GB/s) – 1.39 (2 GB/s) % time per 1 % digits**; 75 GB read back exposed at 1.7 × 10¹¹ | ≈ 1 day after E4 |
| **E6** | **V3**: E5 + Q spilled from the last reciprocal iteration to the division's low product + the division reordered (the window formed, S freed before the low product) | one node 1.70 × 10¹¹ (2³¹), 1.88 × 10¹¹ (2³⁰) | 1.32 at 5 GB/s, 1.67 at 2 GB/s (2³¹); 1.19 / 1.33 at 2³⁰; 151–167 GB read back | ≈ 1 day |
| **E7** | **The reciprocal's products cut to the band read** (switch `NEWTON_RECIP_CUT`) | −5…−15 s of the reciprocal at 10¹¹ (assumed; ≈ 20 % of its pieces) | none | ≈ ½ day + the exactness proof |
| **E8** | **HIP VMM form in `tests/t_alloc.c`**: reserve/map/unmap/release timings, peer access, the transform rate on VMM memory | if it works, the pool's contiguity limit goes: the deterministic out-of-memory at 1.245 × 10¹¹ with 41 GB free in pieces | none | ≈ 2 h |
| **E9** | **Karatsuba over the device grid**: model with D2's per-piece cost, then a 2 × 2 layer in `mul_grid` if the model shows ≥ 20 % of the division phase | fewer piece products at large grids | +1–2 units of memory | 2 h model, then ≈ 1 day |
| **E10** | **At 576**: model a tree-level spill (apumult M56) in `tree_need_dev` before writing it; compare with the built `MN_T_CHUNK_MB=1024` (5.3 × 10¹³ at ≈ +6 % time) | ≈ 5.5 × 10¹³ with V3 (assumed cold set) | 1.5 at 10 GB/s, ≥ 1.95 below | 2 h model |
| **E11** | **V6** (apumult's density): the reciprocal's t1 written only for the used band; the low product's window in chunks | one node ≈ 2.1–2.3 × 10¹¹ (assumed) | runs of 15–30 min per node | several days; only if > 2 × 10¹¹ per node is wanted |
| **E12** | **Memory sampler** (RSS + `/proc/meminfo` every few seconds) and, at 576, a collective budget check that stops the job cleanly | safety at the edge | none | ≈ 1 h + TASKS A3 |

**Suggested order**: E1 → E2 → E3 (no disk, and E2 helps the target directly) → E4 → E5 (the one-node ceiling) →
E7, E8 → E6 if the target's SSD is ≥ 5 GB/s → E9, E10 (model first) → E11 only on request. E12 alongside TASKS A3.

**Where this sits in PLAN §33**: E1–E3, E8 and E12 belong with Phase A (they help the target run's margin and
robustness); E4–E7 and E9 with Phase B; E10 and E11 after C1's model refit. **On the target** (`docs/TARGET_TASKS.md`):
measure `/ssd0`'s O_DIRECT write and read rates first; the elasticities of E5–E6 and E10 depend on them.

---

## 5. The summary's statements about our code

The summary's comparison figures for "ntt/" are from our Phase 8–11 records, not the current code:

| the summary says | now | source |
|---|---|---|
| single-node ceiling 7 × 10¹⁰, in-core | 1.30 × 10¹¹ (2³¹) / 1.44 × 10¹¹ (2³⁰), in-core, measured | P13b, G13d |
| 73.1 s at 4 × 10¹⁰ | 63.5 ± 1.5 s | RESULTS §80 |
| 6.3 bytes per digit at the ceiling | 3.5–4.5 | F1 |
| "dm overflows the block pool at 7 × 10¹⁰", "bs top levels overflow at 8 × 10¹⁰" | no in-phase allocation since Phase 11; every default run to 1.163 × 10¹¹ clean | M13, G13d |
| "block pool 121.5 GB at 4 × 10¹⁰" | 132.3 GB arena | M13 |
| "planes fixed 2³¹ / 3·2³⁰" | four caps plus `fit` | P13b |
| "zero spill", "no OOM guard", "zero madvise" | correct | — |
| the dbig bounce path runs at 1.6–2 GB/s | that is the NVMe rate (WP7); the bounce copy itself runs at ≈ 50–90 GB/s | F1, R |
| 576 × 235 × 10⁹ = 1.35 × 10¹⁴ | not transferable: our 576-node per-node ceiling is 55–70 % of the one-node one | F1 |

So the "3.4× more digits per node" rests largely on the stale 7 × 10¹⁰ figure; against our measured 1.30–1.44 × 10¹¹
the gap is ≈ 1.6–1.8×; E5 (V2) closes a little under half of it (1.30 → 1.68 × 10¹¹ against 2.35 × 10¹¹), and V6 (E11)
is what would close the rest.
