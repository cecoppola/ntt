## 0. Executive Summary

| Axis | apumult | ntt/ | apumult's unique value |
|---|---|---|---|
| **Single-node d_max** | **235e9** (475 GB, SSD-spilled) | 7e10 (in-core, ~440 GB) | **3.4× more digits per node** |
| µ @ 4e10 | 234.8 s | 73.1 s (P3-dec) | (ntt/ wins 3.2×) |
| Bytes/digit @ ceiling | **2.02** (475/235) | 6.3 (440/70) | **3.1× denser** (via spilling) |
| Multi-node d_max implication | 235e9/node × 576 = **1.35×10¹⁴** | 7e10/node × 576 = 4.0×10¹³ | **3.4× the modeled record** |
| Disk-spill techniques | ~30 (M-series) | 0 (checkpoint-only) | **Entire category unique** |
| madvise-based reduction | ~12 techniques | 0 (only HUGEPAGE, disabled) | **Entire category unique** |
| **The headline:** ntt/ is 3.2× faster but hits an in-core wall at 7e10/node. apumult's spill/madvise/stream stack pushes to 235e9/node at 2 B/digit. Ported into ntt/'s device-resident architecture, this could raise ntt/'s 576-node target from 4.0×10¹³ to ~1.3×10¹⁴ digits. | | | |
| **Verified:** \`grep madvise | SPILL | STREAM | TRIM<br>*ntt/ecalc/.c → 1 madvise (HUGEPAGE), zero spill/stream/trim of working set. Their only disk-write is BS_CKPT_* (restart snapshots, not memory reduction).* |

## 1. Category A: SSD Spill/Stream (working-set to disk)

*None of these exist in ntt/. Each moves a large buffer to `/ssd0` between phases and streams it back when needed. Cumulative effect: rss peak 356→~200 GB range at d235.*

| Env | M# | Mechanism | GB saved @ d235 | ntt/ analogue |
|---|---|---|---|---|
| `DM_Q_SPILL=/ssd0` | M29e | Spill Q post-bs, restore for dm-M6l | ~98 ( | Q |
| `DM_P_SPILL=1` | M32 | Spill P around prerecip | ~98 ( | P |
| `DM_MU_SPILL=1` | M33a | Spill µ around 10dP | ~98 ( | µ |
| `DM_MU_STREAM=1` | M49p | dm-mul_hi pread µ chunked (no full restore) | ~33 | NONE |
| `DM_Q_STREAM=1` | M52k | dm-M6l lo-stream + Q-stream from qspill | ~20 | NONE |
| `DM_RECIP_RSPILL=1` | M51 | Recip final-iter spill r post-r², madvise | ~30 ( | r |
| M51-fix2 (code) | M51f2 | T2 stream-shl from r-spill (no restore) | (enables above) | NONE |
| M56 (designed) | — | bs-lvl0 spill nxtQ+P2 between MUL1/MUL2 | ~19 (@d240) | NONE |

**Total unique GB freed @ d235: ~200+ GB via disk.** ntt/ never touches disk for working set.

## 2. Category B: madvise(DONTNEED) Techniques

*ntt/ has zero `MADV_DONTNEED`. Each of these releases pages of a buffer that's dead-until-later or partially-dead. Cumulative: another ~150 GB at d235.*

| Env | M# | Mechanism | GB @ d200-235 | ntt/ analogue |
|---|---|---|---|---|
| `DM_A_MADVISE=1` | M30 | madvise A post-mul_hi, defer Q-restore | ~30 | N/A (A never exists in dec l3) |
| `DC_CORR_MADVISE=1` | M20e-v4 | dc-corr madvise cur+XQ page-safe | (dc-only) | N/A-decimal; UNIQUE for binary |
| `DC_AXTAIL_MADV=1` | M33b | axpool-tail madvise post-XQ | ~30 (dc-ce) | N/A-decimal; UNIQUE-binary |
| `DC_MU_MADV=1` | M47a | madvise µ[d/2] post-mul_hi | ~79 (dc-ce) | N/A-decimal; UNIQUE-binary |
| `DC_AXPOOL_MADV_B2=1` | A23 | dc axpool madv post-b2 | (dc-only) | N/A-decimal; UNIQUE-binary |
| `DC_TOP_VAL_MADV=1` | M43 | dc-TOP val madvise (harmless-shelved) | — | N/A-decimal |
| `NEWTON_R2MADV=1` | M36 | Recip r² madvise post-use | ~22 | Their recip uses views (superseded?) |
| `BS_CUR_MADVISE=1` | M46 | bs-mdev per-pair madvise curQ+P | ~154 (bs-mdev) | **UNIQUE** — their bs-mdev is device-dbig |
| `BATCH_PAIR_INMADV=64` | M42i | bs-batch per-tile curQ madvise | ~83 (bs-batch) | **UNIQUE** (their batch is device-region) |
| `BS_CURQ1_MADV=268M` | M40G | bs-mdev-lvl0 Q1 madvise | ~31 | UNIQUE |
| `MDISP_KXK_BAND=1` | M17f | mul_dispatch band-write + T/P madvise | ~76 (10dP) | Partial (§66 grid-split, no madvise) |

**Note:** Most `DC_*` madvise are N/A in ntt/ decimal (dc doesn't exist). But **UNIQUE for ntt/ binary** and for any host-resident phase

## 3. Category C: In-place / View / Steal

*ntt/ has SOME of these (dbig views, block-pool donation) but not all.*

| Env | M# | Mechanism | ntt/ status |
|---|---|---|---|
| `DM_R_INPLACE=1` | M38 | R.limb = A.limb view | **UNIQUE** — ntt/ dm has R separate (device) |
| `BS_TP_INPLACE=1` | M24a | bs Tp/T→nxtP alias-add | **EQUIV** — their bs writes into region pool in-place |
| `BS_STEAL_PQ=1` | M50 | Result P/Q ← cur.buf steal | **EQUIV** — their region→block-pool donation |
| `NEWTON_QTVIEW=2` | M36 | Qt view (no copy) | **EQUIV** — newton_db uses views |
| `P10_SWAP=1` | M23b | pow10 swap-not-copy | **EQUIV** (trivial) |

## 4. Category D: Trim / Free / Tight-size

| Env | M# | Mechanism | ntt/ status |
|---|---|---|---|
| `DC_TRIM=2, DC_TRIM_D4=1, DC_TRIM_D8_PRE=1` | M31/M48d | Free g_dc[h≤d/N] pre-recip, rebuild later | **UNIQUE** — ntt/ divisor cache has no trim/rebuild |
| `DM_TRIM=1` | — | dm buffer trim | UNIQUE |
| `DEC_AXPOOL_FREE_N` | — | Free axpool below N | N/A-decimal; UNIQUE-binary |
| `DEVDAB_MID_FREE=1` | M15b | Free dev_da/db between phases | **EQUIV** — their planes released post-dm |
| `MDEV_RELEASE_DC=1` | — | Release mdev bufs at dc | EQUIV (device pool donation) |
| `BATCH_DEVDAB_TIGHT=64M` | M42h | devdab pow2→G-multiple sizing | **UNIQUE** — their planes are fixed 2<sup>31/3·2</sup>30 |
| `DC_NXT_TIGHT=1` | M22b | dc nxt.stride /2 | N/A-decimal |
| `P10I_STORE_CAP, P10I_EARLY_CLEAR` | M23a/M11 | pow10 intermediate cap/clear | N/A-decimal |
| `DEC_BFS_TOP_N=16, DEC_LEAF_THRESH=1024` | M55 | dc TOP/LEAF boundary tuning | N/A-decimal; **UNIQUE-binary** (their todec has no such knobs) |

## 5. Category E: Instrumentation & Safety

| Item | apumult | ntt/ | Verdict |
|---|---|---|---|
| **pollv3.sh** phase-aware rss watchdog | TH=372 ch-live / 452 sticky-DEEP; kills before OOM | NONE (cpuset-OOM would wipe) | **UNIQUE** — critical for pushing d_max safely |
| **hwm-poller-v2** | 5s rss+hwm+state+logtail | mem-phase table (per-phase, not continuous) | UNIQUE (continuous) |
| `[bspk]`/`[dcpk]` intra-lvl rss probes | Per-level rss/hwm print | mem-phase table only | UNIQUE (finer) |
| `[batch]`/`[mdev]` per-call sub-timers | ntt/cc/rp/h2d/d2h/join per call | Partial (DIST_STATS, less granular) | UNIQUE (per-call) |
| `subphase_sum.sh` | Post-run Σ ntt/cc/rp/join | NONE (RESULT lines only) | UNIQUE |
| **ENV_L3 / ENV_DMAX split** | Separate µ vs d_max configs | Single config | UNIQUE |
| **d-wall progression table** | Per-d hwm by phase (d160-d240) | Model-only (`mem_model.py`) | UNIQUE (measured) |
| `DC_DMN_PROBE` | dc divmod-node probe | NONE | UNIQUE |

## 6. Category F: Kernel/Algorithm Variants

| Item | apumult | ntt/ | Verdict |
|---|---|---|---|
| `GPUCRT_COOP` (C4b) | 16-group coop cf_place in crtcar | k_crt: 256-coeff LDS-chunk (different, likely faster) | **SUPERSEDED** by ntt/ approach |
| `GPUCRT_RLNUMA` (C6) | numa_alloc_onnode(d) rl_gpu4 | mem.c NUMA-local staging (equiv concept) | EQUIV |
| `CRTCAR_GPU_MDEV` (C1) | mdev-tier GPU crtcar (broken currently) | mdev via CPU crt_carry_par4 or dist-tier CRT | Different; ntt/'s dist-tier CRT is device |
| **GPU-LEAF v4** (A19.22) | Barrett-÷1e18 kernel for dc-leaf | todec.c leaf: kernel `k_leaf` (similar) | EQUIV |
| `mul_dispatch_hi/lo/band` (M14/M52k/M17f) | Truncated hi/lo/band with madvise | Newton truncated (correction-form, no madvise) | **UNIQUE band+madvise variant** |
| `MDISP_KARA/3X3` (A23) | Kara/3×3 at mdev-serial split | Grid-split (§66) — different, roughly equiv | Different-but-EQUIV |
| `NEWTON_MULLO=1` | Newton uses mul_lo | newton_db low-product (equiv) | EQUIV |
| `NEWTON_RTRUNC=1` (M37) | r-trunc at iter-34 for STEP fix | k-anchored (superior) | **SUPERSEDED** |
| `DM_PRERECIP=1, DM_MHS_HOOK` | µ pre-computed, mhs hooks | Their prerecip = seeded prewarm (equiv) | EQUIV |
| `PW_FUSE=1, FWD_FUSE=1` | pw→inv-b1, repack→fwd-b16 | pw_fuse=14, fwd fuse (equiv) | EQUIV |
| `DIVCACHE_MUSQ` (A21) | µ(2h)=µ(h)²+touch | §39 seeded prewarm (same) | EQUIV |

## 7. Top-Value Items to Port apumult → ntt/ (ranked by d_max impact)

| # | Item | Est per-node d_max gain in ntt/ | Effort | Notes |
|---|---|---|---|---|
| 1 | **SSD spill P/Q/µ between phases** (M29e/M32/M33a pattern) | 7e10 → ~15-20e10 | M (~8h) | ntt/ dm phase overflows block-pool @ 7e10 (§71); spilling P after bs, Q after prerecip frees ~200GB. Compose with their dbig: spill dbig quarters to /ssd. |
| 2 | **Recip r-spill + stream-shl** (M51+f2 pattern) | +2-3e10 | M (~4h) | Their recip peak (block-pool 121.5GB @ 4e10) scales; r-spill at final iters. |
| 3 | **bs-mdev madvise/spill** (M46/M42i/M56 pattern) | +3-5e10 | M (~6h) | Their bs top-levels overflow @ 8e10; per-pair spill of curQ+P between products. Applies to dbig. |
| 4 | **pollv3 phase-aware watchdog** | (safety) | S (~1h) | ntt/ has no OOM guard; 17 collateral-wipes in apumult history show why it matters. |
| 5 | **DC_TRIM_D8_PRE + rebuild** (M48d pattern) — for BINARY dc | +2-3e10 (bin only) | S (~2h) | Their binary dc host peak 233GB is the wall in binary mode. |
| 6 | **madvise(DONTNEED) between dbig ops** | +1-2e10 | S (~2h) | Generic: any dbig buffer dead-until-later. |
| 7 | **hwm-poller + [pk] probes** | (observability) | S (~1h) | Attribution when pushing d_max. |
| 8 | **BATCH_DEVDAB_TIGHT** (G-mult sizing) | +0.5-1e10 | T | Their planes are fixed; regions could tighten. |
| 9 | **ENV_L3/ENV_DMAX split** | (workflow) | T | Separate configs for µ-benchmark vs d_max-push. |
| 10 | **mul_dispatch_band + operand madvise** (M17f) | +2-3e10 | M (~4h) | For their host-resident 10dP (binary). |

**Projected combined:** ntt/ single-node d_max **7e10 → ~20-25e10** with items 1-6 (~25h). At 576 nodes: 4.0e13 → **~1.2-1.4×10¹⁴**.

## 8. Architectural Note

*apumult's memory techniques were developed for a **host-resident** pipeline (numbers in host RAM, staged to device per-multiply). ntt/ is **device-resident** (numbers in dbig quarters on device, host is staging-only). Porting requires adapting the pattern:*

- apumult spills **host BigInt.limb** → SSD → restore/stream
- ntt/ would spill **dbig quarters** (device) → bounce-buffer → SSD → restore

  The bounce-buffer path already exists (`dbig.c` 1GB pinned chunks, 1.6-2 GB/s). The spill/restore is a straightforward extension of their checkpoint code (`binsplit.c` ckpt writes region pools). So the port is: **generalize BS_CKPT into a per-buffer spill primitive** callable from newton_db/rns_dist, not just binsplit.
