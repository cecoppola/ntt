# APUMULT — the apumult study (internal reference multiplier) against ecalc

Scope: the apumult study (internal reference multiplier) against ecalc: spec comparison and the apumult measurements. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| A37CMP | apumult a37v1 spec against ecalc, item by item |
| apumult_summary |  |
| apumult_F1 | F1 — apumult §0, Category A (SSD spill/stream), Category D (trim/free/tight-size), §7 (ranked ports), §8 (architecture)  |
| apumult_F2 | apumult vs ntt/ — analysis F2: categories B (madvise), C (in-place/view/steal), E (instrumentation/safety), F (kernel/al |
| apumult_M | apumult spill study: disk and APU memory measurements (agent M) |
| apumult_R | spilling to SSD and releasing memory on MI300A (research findings) |

## A37CMP: apumult a37v1 spec against ecalc, item by item

(former file: A37CMP.md)

Tasked by the main session (read-only on code; this file is the only write). Sources: the a37v1 spec (scratchpad copy), ecalc `main`
@ 6e220db (`ecalc/*.c|h`), internal code notes, internal apumult study, `TASKS.md`, `RESULTS.md`, `results/TIME_MODEL.md §S19B`, `results/N-kernel.md`,
`results/KERNELS.md §K13b`, `results/TARGET.md §TGTBENCH2`. Labels: **(m)** measured, **(mod)** modelled, **(a)** assumed. Where unsure, the row says so.
Spec sections: the spec has no §17 (it jumps 16 -> 18); §5 and §6 are summarised in one line each in the spec itself.

#### 0. The comparison in five sentences

1. **ecalc already has almost every a37v1 technique**, and in several places a better one: the same four RNS primes (`modarith.h:76` = a37v1's "r3" set),
   a batch tier where each APU does all primes of its own products with no cross-die traffic (`rns_mul.c:837-990`), fused pointwise product, fused L^-1, no
   bit reversal, fused Garner+carry+P-add, anchored correction-form Newton, band-cut/mid/high products, a view-based ping-pong arena, a seed overlapped with the mapping.
2. **ecalc's NTT is faster than a37v1's:** 2^31 forward 99-100 ms (m, K13b, same hardware class) against 144 ms; modmul 1.31-1.34 Tmm/s (m, RESULTS §19/§31)
   against 0.94 (a37v1 in-NTT); ecalc has the register-blocked b16 body, the register-blocked b1 (1.2-1.33x), bottom-up plans that move the passes off the slow strides, and
   3*2^k lengths. a37v1's batch tier (21.0 s at 4e10, ntt 73 %, crt 23 %) is not better than ecalc's (N-kernel: scatter 1.1 + ntt 15.7 + crt 2.6 + merge 0.8 s at 4e10 (m), before NP=3/other later gains).
3. **ecalc's one-node 4e10 is 63.5 +- 1.5 s (m, TASKS "work plan after Phase 13")** against a37v1's 80.6 s. At 10-576 nodes the wall is the fabric (S19B: exchange-bound 79 % of
   the modelled 576 wall), so single-node kernel ideas can only touch init + leaf (about 61 s of 465 s at 10 nodes, 61 of 222-266 s at the 576 target (mod)).
4. **The most useful new fact is the seed:** a37v1's CPU seed is 25.9 s at 4e10, but their GPU seed kernel (N13) does the same work in **6 s** (spec §14). ecalc's TASKS 6.4 / E8 (seeds on the GPU)
   was dropped in 2026-09 because the seeds were "off the critical path"/bound init by 1 s only on aac6/aac7. On the **target** the mapping is 7x faster (MAP_RATE 0.010 s/GB (m, target), TGTBENCH2) and the model
   says the seeds bind (init 20.6 + seed wait 9.3 s = 29.9 s at 576, S18TGT). a37v1's GPU-seed pitfalls (copyout +9 s, pregrow exposed +10 s) do not apply to ecalc (numbers stay on the device).
5. **Two big a37v1 choices are rejected for ecalc on principle:** skipping the remainder check (ecalc's T1 residue verification is the only verifier at 3.7e13 digits, where no reference exists) and
   Stirling-balanced leaves (a node's term range spans < 3 % in leaf size, except rank 0).

#### 1. Summary table (all items)

Key to "ecalc status": SAME / EQUIV (different code, same effect) / DIFF (different design) / ABSENT / N/A. "Idea?" = Y (worth a task), M (measure first), N (no).
Benefit scale factors used: ecalc per-node share 6.441e10 digits = 1.61x a37v1's 4e10; terms per node about 6.1e9 (10 nodes) to 7.5e9 (576) = 1.4-1.7x a37v1's 4.35e9.

##### §1 Algorithm

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 1 | Taylor series, N = min m with lgamma(m+1)/ln10 >= d+50, bisection | SAME (`binsplit.c` `e_terms`, bisection; d+50) | N | none | - | - | done (WP3) |
| 2 | Binary splitting P(a,a+1)=1, Q=a+1; combine P=P1 Q2+P2, Q=Q1 Q2; e=1+P/Q | SAME (02 §1.1; S = P+Q in place, `ecalc.c:730`) | N | none | - | - | - |
| 3 | Division = Newton reciprocal + one Barrett quotient | DIFF: DKM two halves, half-length reciprocal, low products, corrections (`newton_db.c:372-456`) | N (see #82) | none | - | - | DKM -25.7 s at 1e11 (m, RESULTS §92) |
| 4 | Leading "2" by digit offset | EQUIV (ecalc divides S=P+Q, the 2 is in X) | N | none | - | - | - |
| 5 | Verify: 50-digit windows at 6 known offsets | DIFF: T1 (P,Q,X,R,digits mod 8 primes) + T2 windows vs `e_1e11.out`; no reference above 1e11 (01 §1.8, 04 §7) | N | none | - | - | ecalc stronger |

##### §2 / §2b Representation and primitives

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 6 | limb base 10^19, 19 digits | DIFF: base 10^18 (`EC_1E18`); digits = limbs | M (#7) | see #7 | L | high | not listed; whole-code base change, not recommended |
| 7 | 3-prime products allow B=10^19 (5.6 % more digits per point) | ABSENT (ecalc: 18 digits/pt at 3 primes) | Y-low: "P20" pack for batch levels with L <= 2^22 | <= 7.5 % of batch NTT = about 1.5-2 s at 6.4e10 (mod, from batch ~25 s) | L | medium | new. Needs p0 p1 p2 = 2^155.4 > n B^2 with B=10^20 (2^66.4): n <= 2^22.5 |
| 8 | RNS primes < 2^52 | SAME: the identical four primes (4222124650659841, 3799912185593857, 3641582511194113, 2586051348529153), `modarith.h:76` | N | - | - | - | - |
| 9 | p = 1 mod 2^33 | DIFF, ecalc superset: 3*2^44 divides p-1 (`EC_PRIMES=1`) => 2^k and 3*2^k lengths | N | none | - | - | ecalc ahead |
| 10 | Tiering: 3 primes batch (maxnl < 2^27), 4 primes mdev | DIFF: 3 primes up to 5.84e10 terms of coefficient (01 §3.6), 4 primes only in the distributed tier at the target, with **P24** (24 digits/pt at 4 primes) | N | none | - | - | ecalc stronger: 3 primes cover the whole size-1 flow; 4/24 = 3/18 per digit |
| 11 | ModCtx {p, pinv, p2, mu115} | SAME (`ec_mod`, `ec_MU115`) | N | - | - | - | - |
| 12 | hpow_ host modpow | N/A (`ec_powmod`) | N | - | - | - | - |
| 13 | lazy modadd/modsub in [0,2p), modcanon | SAME (rule 2: `ec_add/ec_sub`, `ec_fold`) | N | - | - | - | - |
| 14 | `bd_divmod128_h`: 2-shift Barrett, <= 2 corrections | EQUIV: `ec_div1e18` mu=floor(2^123/10^18), <= 3 corrections (`modarith.h:100`) | N | <= 0.1 s in the CRT (mod) | S | low | not worth a row |
| 15 | bd_add, bd_mul_school | N/A (`bi_add`, `bi_mul_school`, tests/reference only) | N | - | - | - | - |

##### §3 Flow

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 16 | Free batch buffers (-37 GB) before the division | EQUIV: plane pool tails donated to the block pool (`ecalc.c:709`), arena borrowed by dm | N | - | - | - | A2/PS notes |
| 17 | A = P shifted into a zero-on-fault buffer (33.6 GB) | DIFF, ecalc better: A is **never formed** (10dP: S=P+Q in place; RESULTS §70) | N | none | - | - | ecalc ahead |
| 18 | dm 19.4 s, str 0.5 s, 10dP 0.14 s, copyout 0.4 s at 4e10 | ecalc 4e10: total 63.5 s (m) | N | - | - | - | see #119 |

##### §4 Binary splitting arena, seed, level loop

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 19 | SPAN=352 fixed leaf | DIFF: `BS_SEED_FILL=128` picks S per run so the last span has <= 128 limbs (S about 229-239; `binsplit.c:263-290`); designed so batch products fill their 2^k | N | none | - | - | ecalc tuned (RESULTS §86, bs -21 s with BI_MUL1_FAST) |
| 20 | Stirling-balanced boundaries (all leaves about 171 limbs) | ABSENT | N | about 0: at 10-576 nodes a node's range [a0,a1) spans < 3 % in log10 k except rank 0 (mod). a37v1 needs it because one node spans k=1..4e9 (39-178 limbs) | M | low | **rejected here** (#R2) |
| 21 | cap64 / stride st per leaf | EQUIV (`seed_limbs`: per = (S ceil(log2 N)+128)/59+2) | N | - | - | - | - |
| 22 | Ping-pong arena, every BigDec a view | SAME (`g_pool[2][NR]`, parity halves; `pool_view`) | N | - | - | - | - |
| 23 | Arena sizing by level loop, HALFMUL=0.80 (sharp: <= 0.75 faults), HFLVL=8 | DIFF: cap = need + need/8; arena = 2cap + max(dm,tree)-2cap, i.e. **dm/tree need binds, not bs** (`binsplit.c:251-262`) | N | none: shrinking the bs half saves no bytes at ecalc's binding phase | - | - | PLAN L1/E2 already cover the binding term |
| 24 | ar[0] on die 0, ar[1] on die 2 | DIFF: four per-APU region arenas (NR=4), subtree per region; reading remote HBM is 93 GB/s vs 3.8 TB/s local (m, RESULTS §55) | N | none | - | - | ecalc design is the locality-correct one |
| 25 | hipMalloc 2 arenas in 2 threads, about 5 s; KFD serializes | EQUIV: VMM arena mapped in the background; "the HIP runtime serializes" (RL, RESULTS §88) | N | none | - | - | RL: `DB_POOL_VMM_PAR` saves nothing |
| 26 | PREGROW (ch_da/db 2^31, devda/db 2^29) overlapped with the seed; 21.5 s, KFD-serialized | EQUIV: `RNS_PLANES_FIRST=1`, plane pools at init, seeds started from `rns_after_staging_hook` inside `rns_init` (02 §1.3, PH9) | N | none | - | - | PH9 in force |
| 27 | hpre: hostMalloc hstage 2x(2^31-8) pinned, 11 s | DIFF: 1 GiB staging per APU + two seed buffers <= 8 GiB freed after the seeds (`binsplit.c:470-474`) | N | none | - | - | - |
| 28 | CPU seed: 96 threads write directly into the hipMalloc arena (first-touch by CPU) | DIFF: ecalc streams spans through two pinned buffers and DMA, because a VMM arena cannot be stored by the CPU (`binsplit.c:1414-1503`) | M | unknown: does the DMA/chunk handoff ever stall the seed thread? | S | low | open question Q2 |
| 29 | `bs_seed_dec_b64_s11k`: sub-range in base 2^64 (alloca), convert, tree-combine (k=32; k=4 29.0 s -> k=32 25.9 s) | ABSENT (ecalc multiplies in base 10^18 directly: `bi_span_step`, 2.6 ns/limb vs binary 0.82 (RESULTS.md line 2345)) | M: microbench | crude model (mod, low confidence): leaf 64 us today (22 s*96/3.3e7) vs about 26-45 us => seed 22 -> 14-18 s. a37v1's own scheme costs 201 us per 352-term leaf, so it is **not** evidence that b64 beats ecalc's already-fast decimal loop | S (bench) / M | low | related: TASKS 2.2, S-3. New |
| 30 | b64_to_bd peel (O(n^2) divmod128) | ABSENT | M (with #29) | included in #29 | - | - | - |
| 31 | Level loop tiers: school < 128 limbs, batch < 2^27, mdev | DIFF: no school tier (measured loss 42-59 s once pools are on the device, RESULTS §58), batch tier, then the distributed B/C tier | N | none | - | - | WP4: `BS_SCHOOL_NL=0` |
| 32 | School tier for level 0 (6.17M pairs, 0.1 s vs batch 1.4 s) | ABSENT by design | N | would be about 2 s at 6.4e10 (mod) but needs the CPU, which the seed thread saturates (about +8 s seed work to combine in cache) | M | medium | **rejected** (#R6) |
| 33 | Per pair nQ=Q1 Q2, T=P1 Q2+P2 with the add fused in the CRT | SAME (`rns_prod.x`, pair mode shares B) | N | - | - | - | WP3 |
| 34 | Odd tail: D2D memcpy, SDMA-safe | EQUIV (`rns_copy_probe`, odd-node copy; XNACK off so no SDMA hazard) | N | - | - | - | Q1 (SDMA) below |
| 35 | 24 iterations; levels 28.2 s at 4e10 | ecalc batch 25.1 s at 9.5e10 digits/node (headline phase table, 00 §1.3) | N | ecalc about 2x better per digit (indicative; different sizes) | - | - | - |
| 36 | Copyout 0.4 s (CPU seed) / 9 s (GPU seed) | N/A: ecalc keeps P,Q on device (`bs_keep_dev`, `binsplit.c:1703-1719`) | N | none | - | - | removes a37v1's reason against GPU seeds |

##### §5-§7b Tiers

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 37 | School tier (CPU omp, bd_mul_school) | ABSENT by design (#31) | N | - | - | - | - |
| 38 | Batch: common logL, tiles of M items, S stripes | SAME (`rns_mul_batch_local`: Mmax, S stripes, `k_crt_batch`) | N | - | - | - | - |
| 39 | Each die handles an item subset with ALL primes, no cross-die traffic | SAME (WP3, `rns_mul.c:831-838`, "owning device computes all primes") | N | - | - | - | WP3, 40x locality (m) |
| 40 | host-pinned pointer+len arrays per tile | DIFF: `hd[]` malloc'd, `hipMemcpyAsync` from pageable memory (synchronous staged copy) | Y-low | with #42: <= 1 s at 6.4e10 (mod) | S | low | new |
| 41 | `k_scatter_rlptr3`: Barrett canon reduce of limbs into 3 residue streams in one kernel | SAME (`k_scatter4`, np=3) | N | scatter is only 1.1 s of 26 s (m, N-kernel) | - | - | - |
| 42 | `A27_BATCH_PIPE`: pipeline Q and T computations | PARTIAL: ecalc has pair mode (B once per pair) but **one stream per die, a host sync per tile** (hd fill, H2D, `k_spill_merge`, `k_norm`, D2H lengths, `hipStreamSynchronize`; `rns_mul.c:940-975`) | Y: double-buffered tiles on two streams, pinned descriptors | <= about 2 s at 6.4e10 (mod: merge 0.84 s + descriptor gaps at 4e10, x1.6); at 576 same per-node | M | low | new |
| 43 | NTT per prime per die, grid.z = M | SAME (`x_fwd(..., M)`) | N | - | - | - | - |
| 44 | Pointwise fused into the inverse first-pass load (PW_FUSE) | SAME (`ntt_inv_pw_y`; `ntt_pw_fuse=14`) | N | - | - | - | - |
| 45 | L^-1 fused into the last inverse store | SAME (`ntt.c` scale in the last b16 pass) | N | - | - | - | - |
| 46 | Garner -> u128 cf -> carry -> write directly, +P add fused | SAME (`k_crt_batch`, `ec_words_to_dec3`: 3 divisions vs 16) | N | ecalc crt 2.6 s vs a37v1 about 4.8 s at 4e10 (m / spec) | - | - | ecalc ahead |
| 47 | GPU stripe-stitch `k_k6v3_stitch` 0.02 s/lvl | EQUIV: `k_spill_merge` on device, but followed by `k_norm` + D2H + sync | Y-low (in #42) | <= 0.5 s | S | low | new, folded into #42 |
| 48 | Mdev: prime c on die c, 4 primes | EQUIV: strategy B (prime per APU, no exchange) where planes fit, else C four-step (01 §1.2, 6.9) | N | - | - | - | TASKS 6.9 |
| 49 | GPU reads hstage zero-copy, aliased to arena (GSCAT) | N/A: operands are device-resident dbig | N | - | - | - | - |
| 50 | **FWD_FUSE**: scatter (limb -> residue) fused into the forward first-pass load | ABSENT (`ntt_load` then `x_fwd`, separate kernels) | Y-low | saves one write+read of a 17 GB plane per operand at 2^31: about 10-15 % of the first forward (mod); in the multi-node tier the pack is already `k_pack_mn24` into send buffers and the wall is fabric-bound, so <= 1 s at 576 (mod) | M | medium (touches `ntt.c` first pass, bit-identical gate) | new (a37v1 does not do it in the batch tier either) |
| 51 | 4-die CRT with XGMI peer reads, stripe d, fence | N/A (ecalc CRT is per owning APU; the dist tier is slab-local) | N | - | - | - | - |
| 52 | kxk dispatch for na+nb > 2^31 | EQUIV: `mul_grid`/`mn_grid`, `split_grid_cap`, piece-cost model `RNS_AUTO_PIECE_COST` | N | - | - | - | B3 done |
| 53 | `mul_dispatch_dec_hi`: skip sub-products below the band | SAME (`NEWTON_HIGHPROD`, `NEWTON_RECIP_CUT`, mid product) | N | - | - | - | - |
| 54 | Garner constants ginv, cf -> dcf cascade | SAME (`crt.c`) | N | - | - | - | - |

##### §8 / §8b NTT and modmul

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 55 | Forward DIF in place, bit-reversed out; inverse DIT; no bit-reversal pass | SAME (`ntt.h:263-278`) | N | - | - | - | - |
| 56 | Pass structure: b16 STG=7 passes + b1, 4 launches at 2^31 | SAME count (plan 121: b1 on 2^12 + 3 b16 at s_lo 26,19,12), a better plan than a37v1's {30,23,16,9} | N | ecalc ahead | - | - | K13b |
| 57 | b16 tile: 256 thr, 128x16, LDS stride 17 | SAME (`BP 17`, `THREADS 256`) plus register-blocked body (8 rows/thread, 2 LDS exchanges) | N | ecalc ahead | - | - | N-kernel, RESULTS §43 |
| 58 | A28_S1: twiddle seed from WbPowTab (cooperative), W16 chain, Ws squaring | EQUIV: `tlo[4096]` x `thi[]` product (one modmul per column), per-stage squaring (`ntt.c:883-891`, 176-185) | M | a37v1 reports it as an optimisation; unknown gain here | S | low | not listed; check once with a pass-level bench (Q4) |
| 59 | b1 pass: contiguous 2^STG per block | DIFF, ecalc ahead: `k_b1r` register-blocked, XOR-swizzled LDS; 1.20-1.33x | N | - | - | - | NTT_B1R=3 default |
| 60 | Inverse mirrors; first pass PW, last pass L^-1 | SAME | N | - | - | - | - |
| 61 | Batch grid.z = M | SAME | N | - | - | - | - |
| 62 | 2^31 fwd 144 ms / inv 149 ms (1 die, 1 prime) | ecalc 99-100 ms fwd (m, K13b); inverse similar | N | ecalc 1.45x faster | - | - | - |
| 63 | NOP-modmul decomposition: 51 ms HBM + 50 ms structure + 43 ms modmul | no equivalent breakdown for ecalc | M | would show whether ecalc's remaining about 2x over the 51 ms floor is structure or modmul | S | none | open question Q4 |
| 64 | 4-step NTT 2.4x slower at 2^31 | CONSISTENT: ecalc uses four-step only across nodes/slabs, never inside one APU | N | - | - | - | - |
| 65 | Modmul: Dekker, rint quotient, 2-up/1-down correction; precondition one lazy operand | DIFF: `NTT_MODMUL=1`: quotient from the two-term reciprocal (pinv+pinvl), `floor`, one correction to [0,2p) (`ntt.c:45-71`). `rint`==`floor` in ecalc's own test: 1328 vs 1331 Gmm/s (m, RESULTS §31) | N | none | - | - | tried (D1) |
| 66 | `asm volatile` barrier against FMA contraction of hi | ABSENT; ecalc relies on exact tests (`t_ntt`, `t_crt`) and the ISA dumps (`isa/`); HIP clang default contracts only within an expression | M | correctness hardening only | S | none | Q5: confirm in ISA that `hi` is never fused |
| 67 | Modmul 940 Gmm/s in-NTT (1101 stand-alone) vs Barrett 885 | ecalc 1.31-1.34 Tmm/s per APU stand-alone (m, §19/§31) | N | ecalc ahead | - | - | - |
| 68 | Reduced-correction vs full Barrett in-kernel (VGPR pressure) | SAME finding (RESULTS: Barrett/Shoup variants slower in-kernel; engine 2 rejected) | N | - | - | - | 05 register |

##### §9 Newton division

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 69 | K = A.n - Q.n + 2 | SAME (k = P.n+1+dl-n_Q+1) | N | - | - | - | - |
| 70 | Correction-form r' = r + r(1 - q r/B^f) | SAME (newton.h:4-13) | N | - | - | - | - |
| 71 | Doubling chain computed backwards from K | SAME (`NEWTON_ANCHOR=1`: targets k, k/2, k/4...) | N | - | - | - | RESULTS §53 |
| 72 | take = 2j+3 top limbs of Q; e vs B^{2j}, sign, d in place | SAME (take = 2j+2) | N | - | - | - | - |
| 73 | Seed from top 3 limbs via long double, 1-2 limbs | EQUIV (Knuth D from top 4 limbs) | N | - | - | - | - |
| 74 | Products Qt x r (6.0 s) and r x d (1.2 s) as truncated | SAME + more: band cut, middle product (`NEWTON_RECIP_CUT/MID`) | N | ecalc ahead | - | - | RESULTS §83 |
| 75 | Last iteration 8.5 s; total recip 14.5 s at 4e10 | ecalc recip DKM to k/2 (one doubling fewer) | N | - | - | - | PH/§92 |
| 76 | A_mul_hi = high product of A x r skipping low pieces, 5.1 s | SAME (`NEWTON_HIGHPROD`) | N | - | - | - | - |
| 77 | **Skip the remainder check** (A27_SKIP_XQ=1): no X*Q, no R, no corrections | ABSENT: ecalc computes X_hi*Q and X_lo*Q (mod B^w) and the R_i corrections (`newton_db.c:388,436`); that is about 116 s of the 169-185 s division at 10 nodes (m, S19B §1) | N (verification) | model (mod, low confidence): skipping needs the full-length reciprocal, whose extra doubling costs about the same as the two low products. Net <= 0.4u = 5-18 s at 10 nodes, and T1 loses R | - | high: no verifier at 3.7e13 | **rejected** (#R1) |
| 78 | Tail of X fixed up by T2 only | ecalc: T1 on 8 primes + T2 + deferred +-dx patch (`ECALC_CORR_PATCH=2`) | N | - | - | - | - |
| 79 | Early reciprocal chain on the GPU dispatcher (30 iterations, j 2 -> 1052M) | ecalc: each node runs the chain to 34126 limbs (`NEWTON_MN_SPLIT`) in **2.28 s minimum, 6.8-22.2 s typical (15 runs, mean 12.4 s)** (m, S19B §2). The excess is a wait, but the 2.28 s floor is large for sizes where a product takes microseconds | M | up to about 2 s (floor), the 10 s mean is separate (S19B §5 #4) | S (measure) | low | S19B §5 #4 covers the wait; the floor is new |

##### §10-§11 Output, memory

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 80 | `bd_to_str_par`: 2-digit table d2[200], 9 lookups per limb, 0.5 s at 4e10 (= 1.2 ns/digit-core) | DIFF: `fmt18` (`mn_out.c:90`) peels 18 digits with %10 and /10; packed default writes limbs as they are. The model constant `DC_FMT_MN` = 0.061 s per 1e9 digits (m, size 1) = 5.8 ns/digit-core; S19B `dc` = 2.8-4.1 s at 6.44e10 | Y: table-based `fmt18` after a profile of `dc` | <= about 3 s per node (mod: 3.9 s -> about 0.8 s if `dc` is formatting). **Unsure** whether `dc` in the no-write runs is formatting, the D2H fetch or T1 residues (`packed_mods`) | S | low (bit-identical output) | new |
| 81 | Memory layout at d48: arenas 43.6 GB x2, ch/dev planes, hstage 16 GB x2, Pout/Qout, A, r/rd | ecalc 363.5 GB device / 405.6 GB node at 6.44e10 (mod, S18TGT), 5.6 B/digit; no A, no Pout/Qout, no 32 GB hstage | N | no transferable saving found | - | - | PLAN L1/E2 (-51.7 GB at 1e11 mod), DM_MN_LEAN, DC8, S-1 |
| 82 | Alloc order: preinit -> arenas -> pregrow/hpre -> seed -> join -> levels | EQUIV (planes first, arena VMM background, seeds from the hook) | N | - | - | - | - |
| 83 | KFD serializes hipMalloc across dies | CONFIRMED independently (RL: VMM calls serialized) | N | - | - | - | - |

##### §12 Environment flags (every flag)

| # | flag | ecalc analogue / status | idea? | note |
|---|---|---|---|---|
| 84 | `HIP_VISIBLE_DEVICES=0,1,2,3` | launch line (mnrun.sh) | N | - |
| 85 | `HSA_XNACK=1` | **opposite**: ecalc runs XNACK off; "HSA_XNACK=1 doubles the pipeline time (10^10: 80 -> 159 s)" (m, RESULTS §55, 2026-09, older host-memory design) | M | Q3: the 2026-09 test used registered host pools; a37v1's hipMalloc arenas might not pay it. Only worth retesting if hipHostRegister/staging cost matters; not recommended |
| 86 | `OMP_NUM_THREADS=96` | `mnrun.sh` thread count / NUMA pinning (pinning the seeds costs 30 %, RESULTS §55) | N | - |
| 87 | `RNS_PREGROW_LOGL=31` | `POOL_LOG` 31 default, pools at init | N | SAME |
| 88 | `PW_FUSE=1` | `ntt_pw_fuse=14` | N | SAME |
| 89 | `CRTCAR_APU_DIRECT=1` | CRT writes into the arena in place | N | SAME |
| 90 | `FWD_FUSE=1` | ABSENT (#50) | Y-low | - |
| 91 | `A27_BDG_NOZ=1` (no explicit zero, mmap zero-on-fault) | ecalc: `MEM_NO_DEV_MEMSET` exists (unset = zero); VMM-mapped memory is zeroed by the driver. Zeroing 90 GB per APU at 3 TB/s is 30 ms | N | no host A buffer exists |
| 92 | `BS_DEC_SCHOOL=128` | `BS_SCHOOL_NL=0` (never; measured loss) | N | #31/#32 |
| 93 | `BINSPLIT_DEC_SPAN=352` | `BS_SEED_FILL=128` (S about 229-239) | N | #19 |
| 94 | `A27_BATCH_PIPE=1` | PARTIAL (#42) | Y | - |
| 95 | `A27_NP3=1`, `A27_K6=3` | `ECALC_NP=3` default at size 1 | N | SAME |
| 96 | `A27_SKIP_XQ=1` | ABSENT, rejected (#77) | N | - |
| 97 | `A27_POOL_PP=1` | meaning not stated in the spec (probably the ping-pong pool); ecalc has parity pools | N | cannot compare precisely |
| 98 | `A27_K6_GSCA=1`, `A27_K6_GSCAT=1` (GPU scatter vs CPU repack) | ecalc scatter is always on the GPU (`k_scatter4`) | N | SAME |
| 99 | `A27_MDEV_KCC=4`, `A27_MDEV_GSCAT=1` | #48, #49, #51 | N | - |
| 100 | `BS_DEC_MDEV=2^27` | `BS_MDEV_LOGL` / `RNS_BATCH_LOGL_MAX` (batch top level at 2^30) | N | - |
| 101 | `A27_P7_HALFMUL=0.80` | none; bs half does not bind (#23) | N | - |
| 102 | `A27_R37T=1` (3-prime fused scatter) | `k_scatter4` | N | SAME |
| 103 | `BSPRE=1` | meaning not stated in the spec | - | cannot compare |
| 104 | `A28_S1=1` | #58 | M | - |
| 105 | `A28_S2=1` (P add fused in the CRT) | SAME (`rns_prod.x`) | N | - |
| 106 | `A28_S8=1` (stitch) | #47 | Y-low | - |
| 107 | `A29_S9=1` | meaning not stated in the spec | - | cannot compare |
| 108 | `A30_S11=32` (seed k-way split) | #29 | M | - |
| 109 | `A30_PT2=10` (defer ar[1] CPU pretouch to level 10) | N/A (VMM arena zero-mapped, no CPU pretouch) | N | - |
| 110 | `A30_NOPT=1` (suppress pre-seed pt_th) | N/A | N | - |
| 111 | `A31_N4=1` (batch LDS swizzle) | SAME (XOR swizzle, `b1r_swz`; D13 +0-6 % noise-level) | N | - |
| 112 | `A31_19=1` (r3 prime set) | SAME primes (#8) | N | - |
| 113 | `A32 HHM` (Stirling boundaries) | #20 rejected | N | - |
| 114 | `A33_STR=1` (digit-pair table; async hstage pregrow) | #80 / #27 | Y | - |
| 115 | Default-off: `A28_S7 A29_S10 A30_HMS A32_N13 A32_SDS A32_DMPT A33_AR4 A34_MODMUL A36_NTIM A37_GCO/P1/R1` | `A32_N13` = GPU seed (see #29, #R-GPUseed); `A34_MODMUL` = a modmul variant; `A37_GCO 2` = the hipHostRegister race. The others are not described in the spec: cannot compare | M (N13) | N13 is the one that matters |
| 116 | `NTT_B16_STG=7` | `ntt_stg=7` | N | SAME |
| 117 | `NTT_REGBLOCK=0` (transposes add traffic when not HBM-bound) | ecalc's register-blocked body is a different thing (in-register radix stages, no extra traffic) and is the default; measured win | N | the names collide, the techniques do not |
| 118 | `A29_HFLVL=8`, `A29_ARENA_CAP_GB=92`, `MUL_SPLIT_LIMBS_DEC=2^31`, `A32_SDS_CHUNK=256` | `ECALC_PLANE_CAP=2^31`; device edge 373.44 GB/node (m, target) | N | - |

##### §13-§14 Timing and ceilings

| # | spec item | ecalc | idea? | benefit | effort | risk | note |
|---|---|---|---|---|---|---|---|
| 119 | bs 60.5 s = arena 5 + max(seed 25.9, pg 21.5, hpre 11) + levels 28.2 | ecalc 4e10 total 63.5 s (m); at 6.44e10 per node: init 25 + leaf 36 s at 10 nodes (m, S19B §1) | - | - | - | - | init floor is a fixed ref init 16.5 s + seeds |
| 120 | Ceilings: scale 2510, triad 3017, read 3495, copy 2998 GB/s; FP64 57.7 TF | ecalc 3.0 TB/s copy, b16 passes 1.3-1.6 TB/s (m, K13b) | - | - | - | - | consistent |

##### §15 Pitfalls

| # | pitfall | ecalc status | idea? | benefit | effort | risk | note |
|---|---|---|---|---|---|---|---|
| 121 | SDMA + migratable SVM -> ring hang | N/A with XNACK off. TASKS B7/H5 ("SDMA for X fetches and checkpoint writes") is a **risk** if ever combined with XNACK | N | - | - | - | note on H5 |
| 122 | KFD alloc serialization | known (#25) | N | - | - | - | - |
| 123 | `hipMemPrefetchAsync` no-op on hipMalloc | not used | N | - | - | - | - |
| 124 | hipHostRegister of fresh anon-mmap + concurrent GPU write -> 1/18 corruption: pretouch first | ecalc pretouches in `mem.c:136-142,257,289` and `comm_ofi.c:179`. **Not verified** for `comm_shmem.c:318`, which registers `S.pool` returned by the library's `shmem_malloc` | Y: touch the pool before `hipHostRegister` | removes a hypothetical rare corruption/hang source (unquantified; T1 would show it as VERIFY FAILED, not silently) | S | low (touch cost: pool 8 GiB, about 0.1-0.3 s at 30-80 GB/s (a)) | new; Q6 |
| 125 | Managed + CPU-preferred arena -> +48 s | ecalc does not use managed memory (RESULTS §55 "unified paging is out") | N | - | - | - | - |
| 126 | GPU-seed copyout tax 8-9 s | N/A (P,Q stay on device) | N | - | - | - | #36 |
| 127 | >100 GB x 2 GPU processes -> OOM cascade (flock + pgrep) | ecalc: one process per node, `mnrun.sh`, AGENT_PROTOCOL "one network program per node", hold nodes exclusively | N | - | - | - | covered |
| 128 | `kill -9` of >50 GB in flight leaves KFD inconsistent | `tools/rundriver.sh` already does SIGTERM + 60 s grace (line 156-161). `a14_soak.sh:32` and `g13d_hang.sh` still `kill -KILL` | Y-tiny | avoids a poisoned node after soak scripts | S | none | new, ops only |

##### §16 / §18

| # | item | ecalc status | idea? | note |
|---|---|---|---|---|
| 129 | T2 windows at 6 offsets; harness t_ntt4 / t_ntt4p / t_dec | ecalc: `tests/t_ntt`, `t_crt`, `t_mul`, `t_p24`, `mnaccept.sh`, T1 + T2 | N | ecalc stronger |
| 130 | Don't: 4-step NTT inside an APU | consistent (#64) | N | - |
| 131 | Don't: SPAN < 352 (seed up, OOM) | not applicable; ecalc's span rule is #19 | N | - |
| 132 | Don't: Barrett modmul in the NTT (VGPR pressure) | consistent (NTT_MODMUL=1 reduced correction is the default; Shoup and engine 2 measured slower) | N | - |
| 133 | Don't: STG 7 + regblock | different technique under the same name (#117) | N | - |
| 134 | Don't: GPU seed alone (seed -18 s, copyout +9 s, pg exposed +10 s) | **does not transfer**: no copyout in ecalc; "pg exposed" is the target's init floor, which is small there (MAP_RATE 0.010) | M | GPU seed is the top recommendation (R1) |
| 135 | Don't: managed arena | consistent (#125) | N | - |
| 136 | Don't: seed convert D&C/AVX | consistent: ecalc has no conversion step; relevant only if #29 is built | N | - |
| 137 | Don't: 2-subtree seed / level overlap | neutral in a37v1; ecalc already overlaps seeds with mapping and the batch tier starts after the join | N | - |
| 138 | Don't: A37 GCO 2 (hipHostRegister race) | see #124 | Y | - |
| 139 | Don't: "correct design, 4 impl" | process note; ecalc's rule "every behavior behind a switch, digits byte-identical" is the safeguard | N | - |

Count: **139 rows** compared (spec §1-§16 and §18, including all 35 flag/variant rows of §12, all 7 pitfalls of §15 and all 9 "do not" items of §18); the spec has no §17. Roughly 80 are the same or equivalent, 14 ecalc is ahead, the rest differ without benefit, cannot be compared (flag meaning not in the spec) or are new ideas. 9 new/revived tasks are shortlisted (R1-R9) and 6 items are rejected outright.

#### 2. Ranked shortlist of recommended NEW task-list additions

Ranked by expected seconds at the target (576 nodes) first, then at 10 nodes. All are new relative to TASKS §§1-7, S-1..S-6, PLAN, the register and S19B §5, except R1, which is
an old item (TASKS 6.4 / E8 / B1, register PH9, S24) that new evidence reopens.

| rank | name | what | benefit (label) | effort | risk | why now |
|---|---|---|---|---|---|---|
| **R1** | **Revive the GPU seed kernel (TASKS 6.4 / E8), as an A/B behind a switch** | compute each 229-term span on the GPU (one thread or one wave per span), writing straight into the region arenas; keep the CPU path as fallback; first a bench-only kernel that reports spans/s | modelled **-7…-12 s at 576** (init 20.6 + seed wait 9.3 = 29.9 s today; floor about 16.5-20.6 s plus 3-10 s of kernel); **about 0…-4 s at 10 nodes** on aac7 (the mapping binds there: "seeds wait 4.5 s for the region map"). Evidence: a37v1's N13 does 4.35e9 terms in 6 s (m, spec §14) => 7.5e9 terms about 10 s (mod, linear), against an ecalc CPU seed of about 22 s (mod, from SC's 31.75 s at 9.17e10) | L (2-4 d) | medium: ordering against the VMM mapping, HIP calls from a second thread queue behind allocations (TASKS 6.4); results are exact integers, so byte-identity is checkable per span | S24 dropped it because aac7 mapping masked the seeds; the target's measured mapping (TGTBENCH2) unmasks them. a37v1's reasons against GPU seeds (copyout, pregrow) do not apply to ecalc |
| **R2** | **Microbench: b64 sub-range seed vs `bi_span_step`** | single-thread, CPU-only, no node needed: compare ecalc's decimal span (2.6 ns/limb) with a37v1's base-2^64 sub-ranges + convert + combine on 229-term spans of 128 limbs | modelled seed 22 -> 14-18 s (low confidence; my crude count says 26-45 us vs 64 us per leaf); a37v1's own leaf cost (201 us/352 terms) says it may not beat ecalc | S (0.5 d) | none | decides whether R1 (L) is needed or a CPU rewrite (M) is enough; shares the "seeds bind on the target" argument |
| **R3** | **Pre-touch the SHMEM pool before `hipHostRegister`** (`comm_shmem.c:318`) | touch every page of `S.pool` (or verify the library already did) before registering | removes a hypothetical rare corruption/hang source: a37v1 saw 1 corrupt run in 18 with fresh anonymous pages + concurrent GPU writes (m, spec §15); ecalc's A1 (1 hang in 31) and the Cray SHMEM crashes are not shown to be related (unsure) | S (1 h) | low | the other three registered pools in ecalc are already pre-touched |
| **R4** | **Profile `dc` (2.8-4.1 s) and, if it is formatting, a 2-digit-table `fmt18`** | S19B `dc`; `fmt18` is 18 divides per limb; a37v1 formats 4e10 digits in 0.5 s | <= about 3 s per node (mod; 1.3 % of the 576 wall, 0.7 % at 10 nodes); 0 if `dc` is T1/D2H | S (2 h) | low, bit-identical | cheap; also speeds `tools/unpack_digits` (off the clock) |
| **R5** | **Batch tile pipeline** (two streams, pinned descriptors, fold `k_norm` + stitch) | A27_BATCH_PIPE analogue: overlap tile i's CRT/merge with tile i+1's scatter/NTT | <= about 2 s per node (mod: merge 0.84 s + gaps at 4e10, x1.6) | M (1-2 d) | low | a37v1 lists it as default on; ecalc has one stream and a sync per tile |
| **R6** | **Reciprocal chain floor** (j < 34126: 2.28 s minimum) | time the first 15 doublings; a one-APU (or CPU) path for small j instead of the four-APU dist products | <= about 2 s (m floor; mod gain); the 10 s mean excess is the wait already in S19B §5 #4 | S (measure) then M | low | a37v1 runs its first 17 iterations in negligible time (not itemised in the spec; unsure) |
| R7 | FWD_FUSE for the B-strategy top levels | fuse limb->residue canon into the first forward pass | <= 1 s (mod) | M | medium | low value in a fabric-bound run |
| R8 | "P20" three-prime pack for batch levels with L <= 2^22 | 10 limbs of 10^18 -> 9 points of 20 digits | <= 1.5-2 s (mod) | L | medium | low value |
| R9 | `kill -KILL` -> SIGTERM + grace in `a14_soak.sh`, `g13d_hang.sh` | ops hygiene (KFD) | avoids a poisoned node | S | none | trivial |

#### 3. Rejected items (one line each)

- **R1 (#77) skip the remainder check / SKIP_XQ:** T1 residue verification of X needs R and is the only verifier at 3.7e13 digits (no reference above 1e11); and the full-length reciprocal's extra doubling costs about what the two low products do, so the model gain is at most 0.4u (5-18 s at 10 nodes, low confidence).
- **R2 (#20) Stirling-balanced leaf boundaries:** a node's term range spans < 3 % in log10 k except rank 0, so leaf sizes are already uniform; `BS_SEED_FILL` already fits the last span to 2^k.
- **R3 (#6) global base 10^19:** a whole-code change (kernels, T1, packed format, P24) for 5.6 % in the 3-prime batch tier only; ecalc's 4-prime P24 is already denser (24 digits/pt).
- **R4 (#85) HSA_XNACK=1:** ecalc measured 2x slower (RESULTS §55) and gains nothing it lacks; keep XNACK off (and it removes the SDMA hazard of #121).
- **R5 (#24) arenas on two dies:** loses ecalc's subtree-per-APU locality (3.8 TB/s local vs 93 GB/s remote (m)).
- **R6 (#32) CPU school tier for level 0:** the seed thread already saturates the CPU and RESULTS §58 measured the school tier as a net loss (42-59 s) with device pools.
- Also not adopted because ecalc is already equal or ahead: the Dekker/rint modmul (#65: rint == floor, ecalc faster), the 4-step NTT (#64), the stride-17 tile (#57), PW/L^-1 fusion (#44-45), the Newton details (#69-76), the batch tier structure (#38-39, #41, #46), the ping-pong arena (#22), and A never formed (#17).

#### 4. Open questions needing measurement

1. **Q1 Seed on the target.** Does the seed thread (about 22 s (mod) at 6.44e10) really bind init at 576 nodes, or does the 16.5 s reference init bind? The model says seed wait 9.3-12.5 s; the only measurement is the SC rehearsal at 9.17e10 (seed ends 31.75 s vs mapping 30.62 s, aac6). A single-node `ECALC_INIT_TL=1` run at 6.44e10 on the target would settle R1's value; on aac7 it cannot (mapping is slow there).
2. **Q2 Seed stream handoff.** Does the DMA/chunk handoff (two pinned 8 GiB buffers, `binsplit.c:1458-1503`) ever stall the seed threads? A per-thread wait counter in `seeds_stream` would show it.
3. **Q3 XNACK retest.** Only if hipHostRegister/pinning cost shows up in a profile; a37v1 runs XNACK=1 without the 2x loss ecalc saw in 2026-09, so the old measurement may be design-specific (unsure).
4. **Q4 Pass-level NTT breakdown.** Run the a37v1-style NOP-modmul build on ecalc's 2^31 forward (HBM floor 51 ms (a37v1) vs ecalc 99 ms) to see whether the remaining gap is structure or modmul; also whether a WbPowTab-style twiddle seed (#58) matters.
5. **Q5 FMA contraction.** Confirm in the ISA of `k_b16r` that `hi = a*b` is never fused (a37v1 needed an asm barrier).
6. **Q6 SHMEM pool pre-touch.** Does Cray/SOS `shmem_malloc` fault in its symmetric heap before `hipHostRegister`? (R3.)
7. **Q7 What is `dc`?** Split S19B's 2.8-4.1 s into D2H fetch, formatting and T1 residues.
8. **Q8 Reciprocal chain floor and wait** (R6): per-doubling timestamps for j < 34126 (`ECALC_LOG_CLOCKS`, S19B §5 #4).
9. **Q9 Batch tier at the target share.** a37v1's batch tier is 21.0 s at 4e10; ecalc's last full by-phase breakdown is at 9.5e10 (25.1 s). A fresh by-phase run at 6.44e10 (10 nodes, `BS_LAYOUT`/phase timers) is needed before any R5/R7/R8 gain is trusted; all their numbers are scaled from 4e10 N-kernel data.

#### 5. Caveats

- ecalc numbers for the NTT (K13b), batch tier (N-kernel) and RESULTS §19/§31 are from earlier code and aac6/aac7 nodes; a37v1's from the spec's own node. Same GPU model, different ROCm and clocks (a37v1 does not state them): treat "1.45x faster" as indicative.
- The division model in #77 uses unit costs inferred from S19B's per-product walls; low confidence.
- I did not run anything; no code or branch was touched. Rows marked "cannot compare" are flags whose meaning the spec does not give.

Decisions for the user: none are forced. The only judgment call is R1 (about 2-4 days for -7…-12 s modelled at the target, with R2 as a half-day gate).

## apumult_summary

(former file: apumult/apumult_summary.md)

#### 0. Executive Summary

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

#### 1. Category A: SSD Spill/Stream (working-set to disk)

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

#### 2. Category B: madvise(DONTNEED) Techniques

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

#### 3. Category C: In-place / View / Steal

*ntt/ has SOME of these (dbig views, block-pool donation) but not all.*

| Env | M# | Mechanism | ntt/ status |
|---|---|---|---|
| `DM_R_INPLACE=1` | M38 | R.limb = A.limb view | **UNIQUE** — ntt/ dm has R separate (device) |
| `BS_TP_INPLACE=1` | M24a | bs Tp/T→nxtP alias-add | **EQUIV** — their bs writes into region pool in-place |
| `BS_STEAL_PQ=1` | M50 | Result P/Q ← cur.buf steal | **EQUIV** — their region→block-pool donation |
| `NEWTON_QTVIEW=2` | M36 | Qt view (no copy) | **EQUIV** — newton_db uses views |
| `P10_SWAP=1` | M23b | pow10 swap-not-copy | **EQUIV** (trivial) |

#### 4. Category D: Trim / Free / Tight-size

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

#### 5. Category E: Instrumentation & Safety

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

#### 6. Category F: Kernel/Algorithm Variants

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

#### 7. Top-Value Items to Port apumult → ntt/ (ranked by d_max impact)

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

#### 8. Architectural Note

*apumult's memory techniques were developed for a **host-resident** pipeline (numbers in host RAM, staged to device per-multiply). ntt/ is **device-resident** (numbers in dbig quarters on device, host is staging-only). Porting requires adapting the pattern:*

- apumult spills **host BigInt.limb** → SSD → restore/stream
- ntt/ would spill **dbig quarters** (device) → bounce-buffer → SSD → restore

  The bounce-buffer path already exists (`dbig.c` 1GB pinned chunks, 1.6-2 GB/s). The spill/restore is a straightforward extension of their checkpoint code (`binsplit.c` ckpt writes region pools). So the port is: **generalize BS_CKPT into a per-buffer spill primitive** callable from newton_db/rns_dist, not just binsplit.

## apumult_F1 — F1 — apumult §0, Category A (SSD spill/stream), Category D (trim/free/tight-size), §7 (ranked ports), §8 (architecture) against ntt/

(former file: apumult/apumult_F1.md)

Source read: `~/apucode/apumult.md` (all 130 lines). Our side: `ecalc/{binsplit.c, newton_db.c, dbig.c, mem.c, rns_dist.c, mn.c, todec.c, ecalc.c, mem_model.py}`, `RESULTS.md` §71, §77–§82, `results/{M13,P13b,G13d,WP7,A-ckpt,N13}.md`.
Labels: **measured** (aac6 runs, file named), **modelled** (`mem_model.py`, which equals the C layout request to the byte and the measured device totals to ±0.05 % — M13), **assumed** (mine). No code was changed.

Context from the integrator (arrived mid-task): apumult's numbers were taken on the target's own MI300A nodes with a node-local `/ssd0`, so its 235e9-per-node figure is target-node evidence; the goal is adoption, so every item below ends in "how to port it here and what it gives".

---

#### Decision table — digits against time, per option (the user's criterion: %Δtime per %Δdigits; rejected before: ≈ 1.6 %/1 %; the compute floor ≈ 1.1 %/1 %)

Method (all **modelled** unless marked): ceiling = the largest size whose modelled node peak fits the measured 524 GB one-node edge (P13b) or the 480 GB target budget; wall at the ceiling = in-core compute by d log² d from the nearest measured point + the spill I/O that is **not** hidden under compute; elasticity = (wall/today's wall − 1)/(digits/today's digits − 1). The writes hide under compute (the background writer exists: `bs_ckpt_bg_*`, N13 measured it hidden at ≳ 1.1 GB/s at 4e10); the **reads are exposed** (P before S = P + Q, Q before the low product) because at the ceiling there is no free space to prefetch into. The hidden writes need ≈ 0.6 GB/s (P under the reciprocal at 1.7e11), ≈ 1.3 GB/s (Q under the second product + high product), ≈ 0.8 GB/s at 576 (the P share): satisfied at every rate below.

**One node, cap 2³¹ (default).** Today: 1.306e11 (measured 1.30), wall 432 s (d log² d from the measured 413 s at 1.252e11, G13d).

| option | ceiling | Δdigits | compute at ceiling | wall / elasticity at 2 GB/s | 5 GB/s | 10 GB/s | exposed read | passes the 1.6 criterion? |
|---|---|---|---|---|---|---|---|---|
| V2: P spill under the reciprocal + tail policy | **1.678e11** | +28.5 % | 566 s (el 1.09) | 604 s, **1.39** | 581 s, **1.21** | 574 s, **1.15** | 75 GB (P) | yes at every rate |
| V3: V2 + Q spill (2nd product → high product), r2 late, division reorder | **1.697e11** | +29.9 % | 573 s (el 1.09) | 649 s, 1.67 | 603 s, **1.32** | 588 s, **1.21** | 151 GB (P, Q) | at ≥ 5 GB/s; marginal at 2 |
| D: trim the 10 % bound margin on Q, S (no I/O) | 1.332e11 | +2.0 % | 442 s | 442 s, 1.08 | same | same | 0 | yes (small) |

**One node, cap 2³⁰ (the largest-size cap).** Today: 1.443e11 (measured 1.44), wall 1120 s (measured 1086–1139 s at 1.42–1.44e11, G13d).

| option | ceiling | Δdigits | compute | 2 GB/s | 5 GB/s | 10 GB/s | exposed read | passes? |
|---|---|---|---|---|---|---|---|---|
| V2 | **1.794e11** | +24.3 % | 1416 s | 1456 s, **1.23** | 1432 s, **1.15** | 1424 s, **1.12** | 80 GB | yes |
| V3 | **1.882e11** | +30.4 % | 1491 s | 1575 s, **1.33** | 1525 s, **1.19** | 1508 s, **1.14** | 167 GB | yes |
| D trim | 1.472e11 | +2.0 % | 1144 s | 1144 s, 1.08 | same | same | 0 | yes |

**576 nodes** (the design's switches; the tree's top level binds at 294 GB per node once the dm need drops — section 6). Today's 480 GB ceiling 8.105e10 per node = 4.67e13, wall 259 s by d log² d from the modelled 234 s at 7.38e10 (`estimate.py --target`). **The grid steps are excluded from the compute column**: 4.90e13 lies past the modelled steps at 4.30e13 (+5 %) and 4.40e13 (+12.6 %) (RESULTS §81), which by themselves are elasticity ≈ 9 — the same objection the user raised before applies to any ceiling past 4.29e13, spill or not.

| option | per node | total | Δdigits | compute | 2 GB/s | 5 GB/s | 10 GB/s | exposed read per node |
|---|---|---|---|---|---|---|---|---|
| V2 (P share) | 8.51e10 | **4.90e13** | +5.0 % | 273 s | 292 s, 2.54 | 280 s, **1.67** | 277 s, **1.37** | 38 GB |
| V3 (P, Q shares) | 8.51e10 | 4.90e13 | +5.0 % (the tree caps it) | 273 s | 311 s, 4.00 | 288 s, 2.25 | 280 s, 1.67 | 76 GB |
| V3 + tree-level spill (**assumed** cold set ≈ 60 GB per top level) | ≈ 9.6e10 | ≈ **5.5e13** | +18.4 % | 311 s | 413 s, 3.24 | 352 s, 1.95 | 331 s, **1.52** | ≈ 205 GB |
| for comparison, built and off: `MN_T_CHUNK_MB=1024` | 9.24e10 | 5.32e13 (M13, 502 GB) | +14 % | — | +0.24 min modelled (§79: ≈ +6 %), i.e. el ≈ 0.5 + compute 1.1 | | | 0 |

Reading: on one node the P spill with the tail policy (V2) is the only option that is both large and cheap — +28 % digits at 1.15–1.39 %/1 %, inside the user's criterion at any NVMe rate ≥ 2 GB/s; V3 adds 1–5 % more digits for a second exposed read and is worth it only at ≥ 5 GB/s (2³¹) or at the 2³⁰ cap. At 576 the spills buy 5 % (the tree's top level is the next wall) at 1.4–2.5 %/1 %, worse than the built chunking switch; at the chosen 4.25e13 target they buy margin (452 → 437 GB per node), not digits. Nothing reaches apumult's 2.35e11 per node: the V3 floor is ≈ 2.3 bytes/digit (planes + host + 4.2 B), and a run at 2e11+ per node takes 15–30 min.

---

#### 0. The one fact everything hangs on: what sets our one-node peak

Our device memory is mapped **at init** and does not move: planes (103.7 GB at cap 2³¹, 60.7 at 2³⁰; measured P13b) + **one arena per APU** = `max(bs regions, dm need)` (`binsplit.c binsplit_pregrow` → `arena_get`; `dm_layout` lines 318–339) + 0.6 GB tables, beside a 12–17 GB host HWM. Above ≈ 5e10 digits the arena is the **dm need**, which is the larger of two formulas per device (in units of B = 8·n_Q bytes = the bytes of Q ≈ 0.444 bytes per digit; n_Q = Q's limbs):

| term in `dm_layout` | what it is | size |
|---|---|---|
| **v2** (the reciprocal's peak) | Q (+10 % bound margin) + S=P+Q (same) + r + r2 (k+4 limbs each, k ≈ n_Q) + t1 (max(n_Q+k, 2k)+8 ≈ 2 n_Q: the reserved "hole"/tail) + one grid piece (2^pool_log limbs: 17.2 GB/node at 2³¹, 8.6 at 2³⁰) + slack min(1/8, 1 GiB) | ≈ 6.2 B + piece + slack |
| **v3** (the bs top level beside the tail) | the last bs level's inputs (2 P, 2 Q of n_Q/2, +10 %) + outputs (P, Q of n_Q, +10 %) + 1/8 fit **+ the hole (t1's 2 B, reserved so the outputs do not fragment it)** | ≈ 4.95 B + 2 B ≈ 6.95 B |

At 1e11 (modelled, = measured 431/414 GB within 0.1 %): v2 = 298 GB, **v3 = 310 GB binds**; arena 310, planes 104, host 14 → 428 GB. The measured edge is device + host HWM ≲ 524 GB (P13b), so the ceiling is (524 − 15 − planes)/(≈ 3.1 bytes/digit): **1.30e11 at 2³¹, 1.44e11 at 2³⁰ (measured, P13b)**; the model at 524 GB gives 1.306 / 1.443e11 (this run of `mem_model.py`).

Consequences for every spill item:
1. A buffer that is cold during the reciprocal frees bytes **only if the arena formula is shrunk by it** (the block returns to the pool's extent list; the OS never sees anything; there is no madvise on hipMalloc'd HBM). The port is therefore always: spill + `db_free` + a smaller `dm_layout` + restore, not a spill alone.
2. **Spilling P alone gives zero**, because v3 (the bs top level + reserved hole) binds, not v2. The hole policy has to change with it (below, "V2").
3. Beyond the dm phase the tree's top level binds at 576 nodes (tree need 294 GB per node at 7.38e10 vs dm need 309: modelled below), so the same spills cap out at +5 % there unless the tree level is spilled too.

The variants I modelled (each includes the previous; all are changes to `dm_layout`'s formula plus the spill code):

| variant | what is spilled / changed | v2 (recip/div peak) | v3 (bs top) |
|---|---|---|---|
| V0 | today | 6.2 B + piece | 4.95 B + hole |
| V1 | P spilled under the reciprocal, restored before S = P + Q | 5.1 B + piece | unchanged |
| V2 | V1 + the tail policy: t1's contiguous block = the parity half that held the top level's dead inputs, so no hole is reserved beside the top level | 5.1 B + piece | 4.95 B |
| V3 | V2 + r2 reserved only after the first product; Q spilled from the last iteration's second product through the division's high product, restored for the low product; the window Aw formed and S freed before the low product | max(Q+r+t1+piece, r+d+t1, S+µ+t, Q+X+xq+Aw) ≈ 4.2 B + piece | 4.95 B |
| V4 | V3 + the bs top level frees each input as consumed | same | ≈ 4.33 B |
| V5 | V4 + the fresh Q and P₂ spilled during T = P₁Q₂ (apumult M56) | same | ≈ 2.5 B |

**One-node ceilings (modelled at the measured 524 GB edge; 502 GB in brackets), three primes, host fitted:**

| cap | V0 | V1 (P only) | V2 (P + tail policy) | V3 (+Q, r2, div reorder) | V4/V5 (bs top) |
|---|---|---|---|---|---|
| 2³¹ (default) | **1.306e11** (1.236; measured 1.30 / OOM-free 1.252, fragmentation fail at 1.245) | 1.306e11 (no change) | **1.678e11** (1.583) | **1.697e11** (1.683) | 1.697e11 (no change: v2 binds) |
| 2³⁰ | **1.443e11** (1.373; measured 1.44, 1.46 OOM) | 1.443e11 | **1.794e11** (1.739) | **1.882e11** (1.794) | 1.882e11 |

Per-size node totals (modelled, 2³¹; the arena in brackets): 1e11: V0 428 (310) → V2 373 (256) → V3 361 (243); 1.4e11: 553 (434) → 466 (347) → 447 (328); 2e11: 741 (621) → 598 (478) → 585 (465); 2.35e11: 851 (729) → 685 (563) → 676 (554). At 2³⁰ subtract 43 GB. So 2e11 and 2.35e11 on one node need ≈ 585 / 676 GB even with V3: **not reachable by spilling P/Q/µ** on our layout; the floor with V3 is the reciprocal's own r + r2 + t1 (4 B) and the low product's Q + X + xq + Aw (4.2 B). apumult's 2.02 bytes/digit (475/235e9) is below our V3 floor of ≈ 2.3 bytes/digit at 2³⁰ (planes 61 + host 15 + 4.4 B·0.444 d); reaching it needs the two further changes listed under "V6" (band-sized product outputs, the window in chunks) that apumult's `mul_dispatch_hi/lo/band` embodies.

GB freed from the node peak by V3, at each size (modelled): 1e11 −67 GB, 1.4e11 −106, 2e11 −156, 2.35e11 −175 (2³¹); 2³⁰ within 1 GB of the same.

---

#### 1. Category A — SSD spill/stream, item by item

##### A1. `DM_Q_SPILL` (M29e): Q spilled after bs, restored for the low product (~98 GB at d235)
*What it does.* Q (= N! in the run's base, n_Q limbs, 0.444 bytes/digit) is written to the SSD once bs is over and its host buffer released; it is read back only for the low product X·Q of the division. In apumult's host flow the reciprocal is fed from a "prerecip" (their divisor-cache µ) so Q is not needed in memory meanwhile; the restore is a full sequential read.
*Our analogue.* `bs_Qd` (the top-level dbig, `ecalc.c` line ~496; the mdb share `Qm.sh` at size > 1). In our reciprocal (`newton_db.c recip_db2`, lines 62–117) Q is an operand of the **first product of every iteration** (Q_t = the top `take = 2j+2` limbs, `db_view(Qd, nq − take, take)`; at the last iteration take = n_Q: all of Q), so Q cannot leave during the reciprocal; it is dead from the last iteration's second product (r·d → t1) through the final add and the division's high product A_h·µ (`newton_db_divmod_shifted` line 243), and needed again at the low product (line 251) and the window corrections (lines 257–268: `db_cmp(&Rd, Qd)`, `db_sub(&Rd, &Rd, Qd)`).
*Port.* Spill Q right after the last iteration's first product (`recip_db2` after line 82, condition `take == nq`), `db_free` it; restore into a fresh block before `rns_mul_low_db` (line 251). Transfer: `ckpt_dbig_io` (`binsplit.c:596`) already moves any dbig range device ↔ file through the 1 GiB pinned chunk per APU (`CKPT_CHUNK`, `rns_hstage`); wrap it as `db_spill/db_restore`. Size > 1: the same on `Q->sh` in `newton_mn_divmod` (`newton_db.c` ~706–746; the existing `newton_mn_pq_hook(1, Q)` at line 746 is where the top-set writer already takes Q's share).
*GB and time.* 1.1 B: 44 GB at 1e11, 62 at 1.4e11, 75 at 1.7e11 (per node). It lowers v2 only together with V2's tail policy and the r2 change (part of V3): V2 → V3 is +1 % at 2³¹, +5 % at 2³⁰ (1.79 → 1.88e11). Traffic 2 × Q per run: at 1.7e11 150 GB → 150 / 75 / 50 s at 1 / 2 / 3 GB/s; the write can hide under the second product + high product (needs ≳ 2 GB/s), the read is exposed before the low product (no free space to prefetch into at the ceiling).
*Verdict:* worth testing as part of V3, not alone.

##### A2. `DM_P_SPILL` (M32): P spilled around the reciprocal (~98 GB)
*What it does.* P is dead from the end of bs until S = P + Q (their 10dP); it goes to disk under the reciprocal and comes back for the add.
*Our analogue.* Exactly the same dead interval: `bs_Pd` from the residues (`ecalc.c` ~491) to `db_add(&bs_Pd, &bs_Pd, &bs_Qd)` (line 513). **The write half already exists**: `ECALC_CKPT_TOP` runs `bs_ckpt_bg_start` (`binsplit.c:815`) writing P under the reciprocal and Q under the division, with `bs_ckpt_bg_release(topbg, 0)` before P is overwritten (`ecalc.c:512`); at size > 1 `mn_pq_hook` (`mn.c:351`) does the same for the shares. What is missing: `db_free(&bs_Pd)` once the writer's P part is done (`bs_ckpt_bg_done(b, 0)`), the read-back into a new block before line 513, and the smaller arena (v2 − 1.1 B).
*GB and time.* P = 1.1 B (with the bound's margin): 49 GB at 1e11, 68 at 1.4e11. **Alone it changes the ceiling by 0** (v3 binds; table above). With the tail policy (V2) it is the single biggest step: **1.31 → 1.68e11 (2³¹), 1.44 → 1.79e11 (2³⁰)**. Traffic 2 × P: 89 GB at 1e11, 149 GB at 1.7e11 → read exposed 75 / 37 / 25 s at 1 / 2 / 3 GB/s on a run of ≈ 580–840 s at 1.7e11 (compute extrapolated from 413 s at 1.25e11: d log² d gives 575 s, the measured local exponent 2.3 from 7.64e10 → 1.25e11 gives 838 s) = +3–13 %; the write hides under the reciprocal if the SSD sustains ≈ 0.6 GB/s (75 GB over the ≈ 120 s reciprocal; the aac6 NVMe wrote 0.8–1.5 GB/s with fsync, WP7/A-ckpt measured; 0.31 GB/s on its slow path, §77).
*Verdict:* **worth testing first**, as V2 (P spill + tail policy); it is the one that moves the ceiling.

##### A3. `DM_MU_SPILL` (M33a): µ spilled around 10dP (~98 GB)
*What it does.* In apumult µ is computed before A = 10^d (P + Q) is formed (their 10dP is a real host phase with a large A), so µ is cold during it.
*Our analogue.* None: the decimal device flow has no 10dP (A = S·B^dl is implicit, RESULTS §70 I3; `10dP 0.0 s`); µ is produced by the reciprocal (it takes r's block, `recip_db2` line 108) and consumed by the high product right after S = P + Q, an add of 1–2 s. Its only cold interval is that add, which is not a peak.
*Verdict:* not applicable.

##### A4. `DM_MU_STREAM` (M49p): the high product preads µ in chunks, no full restore (~33 GB)
*What it does.* µ never comes back whole; their `mul_dispatch_hi` reads µ's chunk for each band from disk.
*Our analogue.* The high product is a piece grid over fixed planes (`rns_mul_high_db` → `rns_dist.c` grid, ka × kb pieces of 2^pool_log limbs; at 1.44e11 the division's grids reach 18 × 22, G13d). A streamed operand piece is read **once per row it is combined with** unless the other operand's transforms are cached: kb pieces × ka rows = ka × |µ| bytes of reads per product (10–20× µ at the top sizes: 0.5–1.5 TB per product at 1–3 GB/s = hours). The transform cache (`RNS_DIST_CACHE`, 17 GB per slot per APU, off at size 1) would have to hold a whole row of transforms to make streaming read each piece once, which costs the memory the stream saves. Not the same product structure as apumult's dispatch.
*GB.* Would remove µ (1 B) from the high product's set (S + µ + t = 4.1 B) — a phase that does not bind (the reciprocal's 4 B + piece and the low product's 4.2 B do).
*Verdict:* not applicable to the grid; superseded by restoring µ whole (it is never spilled here).

##### A5. `DM_Q_STREAM` (M52k): the low product streams Q from the spill (~20 GB)
*Same structure as A4*: the low product `rns_mul_low_db(&xq, Xp, Qd, w)` is a grid with the pieces above w skipped; streaming Q's pieces reads them ka times. The window corrections need Q whole (`db_cmp`, `db_sub` over quarters) — those could be chunked (the add kernel is already chunked, `dbig.c addsub_core2`, CH = 4096 limbs with a host carry scan), which is the part of this item that would port: a `db_sub_streamed(R, R, file)`.
*GB.* Q (1.1 B) out of the low-product set Q + X + xq + Aw = 4.2 B → 3.1 B, but then the reciprocal's r + r2 + t1 + piece ≈ 4.4 B binds: the ceiling moves ≈ 0.
*Verdict:* not applicable now; only relevant after a band-sized t1 (V6).

##### A6. `DM_RECIP_RSPILL` (M51) + M51-fix2: the final iteration spills r after r², the shift-left streams from the spill (~30 GB)
*What it does.* In the r' = 2r − Q r² form, r is dead between forming r² and the final 2r − …; they spill it and stream it into the final shifted subtraction.
*Our analogue.* Our form (k-anchored, `recip_db2`): t1 = Q_t·r; d = |B^2j − u| → r2; t1 = r·d; r' = (r << j) ± corr with corr a view of t1. r is an operand of both products and of the final add: **no dead r at the peak**. The doc's "T2 stream-shl" would apply to `db_sub_shifted(&r2, &r, j, &corr)` (line 95), whose kernel is chunked and could read r from a bounce buffer, but r is live for r·d one step earlier anyway.
*What does apply from the same iteration:* **r2 is reserved for the whole reciprocal** (`db_reserve(&r2, k + 4)` at line 72) though it is only written after the first product; reserving it after `rns_mul_dist_db(&t1, &qt, &r)` makes the first product's live set Q + r + t1 instead of Q + r + r2 + t1 (−1 B = −44 GB at 1e11). That is the item's real counterpart here, included in V3. Risk: the block must be found again each iteration (the pool's best-fit with the reserved tail, `dbig.c ext_take`); G13d's 1.245e11 failure (a 27.67 GB request refused with 41 GB free in two extents) shows the edge is fragile.
*Doc's numbers.* "block-pool 121.5 GB @ 4e10" is not in our results: the measured 4e10 arena is 132.3 GB (M13, RESULTS §77/§78), the Phase 10 pool was 141.0 (`mem_model.py MEASURED`). "+2–3e10": not transferable — the r-spill is 0 here; the r2 change is worth ≈ +1 % at 2³¹, +5 % at 2³⁰ on top of V2.
*Verdict:* the r-spill itself: not applicable (different Newton form); the r2 late reservation: worth testing inside V3.

##### A7. M56 (designed): the bs top level spills the fresh Q and P₂ between MUL1 and MUL2 (~19 GB at d240)
*What it does.* Top level: Q = Q₁Q₂ (MUL1), T = P₁Q₂ (MUL2), P = T + P₂. After MUL1 the new Q and P₂ are cold until after MUL2.
*Our analogue.* The size-1 top level runs on device numbers in the region pools of the two parities (`binsplit.c` level loop; inputs in one parity's pool, outputs in the other; `node_p/node_q` views, `pool_view`). The pools are bump allocators donated whole (`donate_pools`), so nothing is freed per node: the level's live set is inputs + outputs = 4.4 B (+1/8) — the v3 term. Per-node freeing (V4) and this spill (V5) cut v3 to ≈ 4.33 B / 2.5 B, but **at size 1 v2 binds once V2/V3 are in** (identical ceilings in the table), so they buy < 1 %.
*At 576 nodes* the counterpart is the tree level (`mn.c tree_level_k`, line 462: Horner (P, Q) ← (P_i Q + P, Q_i Q) over the group): the live set per node is the child pair (74 GB at 7.38e10) + the running pair (49) + the new pair (74) + the top product's scratch (80) = 277 → 294 GB with the 1/16 (modelled = the C formula `tree_need_dev`). During P_i × Q_run the child's Q_i and the running P are cold (≈ 60 GB): spilling them is what would move the 576-node ceiling after V2 (section 6).
*Verdict:* size 1: not worth it after V2/V3; 576: worth modelling in `tree_need_dev` and testing after V2.

**Category A total.** The doc's "~200 GB freed at d235" is apumult's host-resident sum; on our layout the reachable sum is V3's −67 GB at 1e11 … −175 GB at 2.35e11 (modelled), and the binding number is the ceiling: **1.30 → 1.70e11 (2³¹), 1.44 → 1.88e11 (2³⁰)**, +30 %, not 3.4×.

---

#### 2. Category D — trim / free / tight-size

| item | apumult | ours | GB | verdict |
|---|---|---|---|---|
| `DC_TRIM*` (M31/M48d): free the divisor cache g_dc[h ≤ d/N] before the reciprocal, rebuild later | binary dc | the decimal path formats 10¹⁸-limbs directly (`mn_out.c`); `todec.c`'s `struct divisor` (T = 10^h, µ) exists only on the binary test path (`LIMB_BASE=2`, RESULTS §67 kept it as a check, not the code) | 0 on the default path | not applicable |
| `DM_TRIM`: dm buffer trim | trims host buffers | `recip_db2` reserves r, r2 at k+4 and t1 at tcap exactly ("no other temporaries"); the margins left in `dm_layout` are the +10 % on Q and S (n_Q/10), the +1/8 level fit and the min(1/8, 1 GiB) slack. Trimming the 10 % to the true bound (Q is exactly n_Q limbs by construction; the +10 % is the bound's margin for P > Q) would free ≈ 0.2 B = 9 GB at 1e11 | ≈ 2 % of the ceiling | already mostly equivalent; a 2 % item, after V2 |
| `DEC_AXPOOL_FREE_N` | binary dc pool | no dc pool | 0 | not applicable |
| `DEVDAB_MID_FREE` (M15b): free the device planes between phases | host-resident: planes only needed per multiply | our planes are pools for the whole run (`rns_init` → `rns_shutdown`); every peak phase (reciprocal, division, bs top) multiplies, so the planes are needed exactly when the peak is; the output stage needs none but is past the peak | 0 | already equivalent (`rns_dist_cache_release` at the last grid product; `db_release_pools` after the output stage) |
| `MDEV_RELEASE_DC` | release mdev buffers at dc | region pools donated to the block pool (`donate_pools`), pool released after output | 0 | already equivalent |
| `BATCH_DEVDAB_TIGHT` (M42h): planes sized to a G-multiple, not pow2 | fine-grained plane size | `ECALC_PLANE_CAP` = {2³⁰, 3·2²⁹, 2³¹, 3·2³⁰, fit} (P13b): four steps, each 17–52 GB for 6–9 s at 4e10 measured; at the ceiling the top products exceed the cap and are gridded, so the plane is fully used — sizing below the cap only adds pieces (the 2³⁰ row: −43 GB for 1.5–2.7× the wall at 1.42e11, P13b/G13d measured) | 0 beyond the cap switch | superseded by `ECALC_PLANE_CAP` |
| `DC_NXT_TIGHT`, `P10I_STORE_CAP/EARLY_CLEAR` | pow10 intermediates | no pow10 (A = S·B^dl implicit) | 0 | not applicable |
| `DEC_BFS_TOP_N`, `DEC_LEAF_THRESH` (M55) | binary dc tuning | `todec.c dec_leaf_u_max = 256` is a constant on the binary path (no env) | 0 | not applicable (decimal) |

---

#### 3. §7 — the ranked port list, checked

| # | doc's item and claim | verdict here | what it gives (modelled) |
|---|---|---|---|
| 1 | SSD spill P/Q/µ between phases: "7e10 → 15–20e10", "dm overflows the block pool @ 7e10 (§71)", "frees ~200 GB" | The premise is stale (§71 is Phase 8, 2026-09-18; since M11's tail arena the in-phase hipMalloc is 0 at 4e10/8e10/1e11, M13 measured; the ceiling is 1.30/1.44e11 measured). The port is V2 (+V3): the P spill needs the tail policy change to count; µ has no cold interval. | **1.30 → 1.68e11 (2³¹) / 1.44 → 1.79e11 (2³⁰) for V2; 1.70 / 1.88e11 for V3.** Effort: the transfer exists (`ckpt_dbig_io`, `bs_ckpt_bg_*`); the work is `dm_layout` + the tail policy (`arena_get`, `db_pool_set_tail`) + `mem_model.py`, and the fragmentation risk at the edge. |
| 2 | Recip r-spill + stream-shl: "+2–3e10"; "block-pool 121.5 GB @ 4e10 scales" | Not applicable to our Newton form (A6); the counterpart is r2's late reservation (inside V3). The 121.5 GB figure is not ours (132.3 measured). | ≈ +1–5 % on top of V2 |
| 3 | bs-mdev madvise/spill: "+3–5e10", "their bs top levels overflow @ 8e10" | "Overflow @ 8e10" is wrong: 8e10 runs at 369.1 GB with hipMalloc 0 (M11/M13), and G13d ran every size to 1.163e11 clean on the defaults (the region pools' in-phase growth aborts since Phase 12 R unless `RNS_POOL_GROW=1`, and no run needed it). Size 1: v3 stops binding after V2, so V4/V5 give < 1 %. 576: the tree level's cold shares are the second lever. | size 1 ≈ 0; 576 ≈ +1.5e10/node after V2 (section 6, assumed cold set) |
| 4 | pollv3 phase-aware RSS watchdog | Not in my categories; note the facts: our edge runs died by the kernel OOM killer in bs (P13b jobs 21064, 21069) and by a refused hipMalloc (G13d 1.245e11); `ECALC_INIT_ONLY=1` and `BS_LAYOUT_ONLY` predict the init peak without a run (P13b) and `mem_report` prints per phase. A watchdog is cheap and independent. | safety |
| 5 | `DC_TRIM_D8_PRE` + rebuild: "+2–3e10 (bin only)" | not applicable (decimal; no divisor cache) | 0 |
| 6 | madvise(DONTNEED) between dbig ops: "+1–2e10" | Not applicable to hipMalloc'd HBM: our device blocks live in one arena mapping per APU (`arena_get`) and a `db_free` only returns the extent to our allocator; the OS is not involved and the arena's size is fixed at init by `dm_layout`. The equivalent lever is the formula (V2/V3), which is item 1. | 0 by itself |
| 7 | hwm-poller + [pk] probes | observability; `mem_report` per phase + `MEM_REPORT_DEVS=1` exist; a continuous poller is a shell script | — |
| 8 | `BATCH_DEVDAB_TIGHT`: "+0.5–1e10" | superseded by `ECALC_PLANE_CAP` (Category D) | 0 beyond the cap steps |
| 9 | `ENV_L3/ENV_DMAX` split | workflow; `estimate.py --target`, `design_table.py`, the M-run blocks serve it | — |
| 10 | `mul_dispatch_band` + operand madvise for the host-resident 10dP: "+2–3e10" | no 10dP in the decimal flow. The band idea's device counterpart is "V6": size the reciprocal's t1 to the top band actually used (u = t1 >> (take − j), corr = t1 >> j: the top ≈ n_Q limbs of a 2 n_Q product; `rns_mul_high_db` already skips the low pieces' computation but the output dbig is full length) → the reciprocal's floor 4 B → 3 B; then the low product's Q + X + xq + Aw (4.2 B) binds until the window is done in chunks (A5). Together they are what would take the floor from ≈ 2.3 to apumult's ≈ 2.0 bytes/digit. | after V3: ≈ 1.9 → 2.1–2.3e11 (assumed) |

"Projected combined 7e10 → 20–25e10 with items 1–6": on our layout items 1–6 reach **1.70e11 (2³¹) / 1.88e11 (2³⁰)** modelled; 2.0–2.3e11 needs V6 as well, and the run at that size takes 15–30 min per node (section 5).

---

#### 4. §8 — the architectural note, checked

- Right: numbers are device-resident dbig quarters (one per APU; mdb shares at size > 1 are dbigs too, `x->sh`), so the spill path is device → pinned bounce → SSD.
- The bounce path: `dbig.c` `BOUNCE` = 2 × 1 GiB pinned per APU (`hipHostMalloc`, line 244; the comment quotes pinned `hipMemcpy` ≈ 50 GB/s) — it serves `db_from_bi/db_to_bi`. The checkpoint transfer is a different buffer, the 1 GiB `rns_hstage` per APU (`CKPT_CHUNK`, `ckpt_buf` line 557). The "1.6–2 GB/s" the doc attributes to dbig.c is WP7's measured NVMe DMA rate (write with fsync ≈ 1 GB/s sustained; A-ckpt: 0.8–1.5 GB/s single node, 0.4–0.7 per node when four processes share the disk).
- "Generalize BS_CKPT into a per-buffer spill primitive": **`ckpt_dbig_io(FILE*, dbig*, lo, hi, buf, write, io, part)` (`binsplit.c:596`) already is a per-dbig-range primitive in both directions**, and `bs_ckpt_bg_start/done/release` (lines 815–831) is a background writer over a P and a Q with per-part completion. What has to be added: (i) `db_free` on completion and `db_reserve` + read-back, four threads on their NUMA-local staging (`mem.c mem_hstage_alloc`); (ii) the callers' order changes in `recip_db2`, `newton_db_divmod_shifted`, `newton_mn_divmod`, `ecalc.c` (P restore before line 513); (iii) the formula: `dm_layout` v2 minus the spilled terms and v3 without the hole, the tail moved to the dead parity's half (`arena_half_range`, `db_pool_set_tail`), mirrored in `mem_model.py dm_layout`; (iv) a spill directory switch (the target's `/ssd0`; aac6's `BS_CKPT_DIR` disk) and the layout report (`BS_LAYOUT_ONLY`) printing the spilled variant so the M-run can compare before running.
- Not in the note but decisive: the memory is freed by the formula, not by the spill (section 0), and the pool's fragmentation at the edge (G13d) is the risk the tail policy must handle; `ECALC_INIT_ONLY=1` + `BS_LAYOUT_ONLY` give the check before a 20-minute run.

---

#### 5. Time cost (one node)

Traffic per run (P, Q ≈ 0.444 bytes/digit each; writes + reads):

| digits | V2 (P w+r) | V3 (P, Q w+r) | compute (measured / extrapolated) |
|---|---|---|---|
| 7.64e10 | 68 GB → 68 / 34 / 23 s at 1 / 2 / 3 GB/s | 136 GB → 136 / 68 / 45 s | 133–138 s measured (§80, G13d) |
| 1e11 | 89 GB → 89 / 44 / 30 s | 178 GB → 178 / 89 / 59 s | 207 s (3·2³⁰, P13b), 225 s (defaults, G13d) measured |
| 1.4e11 | 124 GB → 124 / 62 / 41 s | 249 GB → 249 / 124 / 83 s | 1086–1139 s at 2³⁰ measured (G13d); 2³¹ does not fit today |
| 1.7e11 (V3 ceiling, 2³¹) | 151 GB → 151 / 75 / 50 s | 302 GB → 302 / 151 / 101 s | 575 s (d log² d) – 838 s (local exponent 2.3) extrapolated from 413 s at 1.252e11 |
| 2e11 | 178 GB → 178 / 89 / 59 s | 356 GB → 356 / 178 / 119 s | 685–1218 s extrapolated; does not fit even with V3 (585 GB at 2³¹, 548 at 2³⁰) |
| 2.35e11 | 209 GB → 209 / 104 / 70 s | 418 GB → 418 / 209 / 139 s | 815–1766 s extrapolated; 676 / 641 GB with V3: does not fit |

Exposed vs hidden: the writes go under the reciprocal (P; the writer thread exists and W measured it hidden at ≳ 1.1 GB/s at 4e10, N13) and under the second product + high product (Q); the reads are exposed (P before S = P + Q, Q before the low product; at the ceiling there is no free space to prefetch into). So V2 costs ≈ one P read = **+3–13 % of the wall at 1.7e11** (25–75 s at 3–1 GB/s), V3 ≈ two reads = **+6–26 %**. aac6's rate is a lower bound (the integrator's note); at the target's `/ssd0` rate (unknown to us; another agent measures aac6) the exposed part scales inversely.

---

#### 6. 576 nodes

Per node at the target (7.38e10 average, 7.64e10 top node; modelled with the design's switches, this run of `mem_model.py`): **452 GB = planes 103.7 + arena 309.2 + exchange 12.3 + host 30.2** (SHMEM pool 8.6), where the arena = max(bs regions 185, **dm need 309 = 229 (the shares' v2/v3, same structure as size 1 with n_Q/576) + 80 top-product scratch**, tree 294).

| variant | dm need per node | arena (= max with tree 294) | node at 7.38e10 | 576 ceiling at 480 GB (502) | total digits |
|---|---|---|---|---|---|
| V0 | 309 | 309 | 452 | 8.11e10 (8.62) | 4.67e13 (4.97) |
| V1 (P share spilled) | 309 (v3 binds) | 309 | 452 | 8.11e10 | 4.67e13 — **no change** |
| V2 (+ tail policy) | 270 | **294 (the tree binds)** | 437 | 8.51e10 (9.13) | 4.90e13 (5.26) |
| V3 / V5 | 243 / 237 | 294 | 437 | 8.51e10 | 4.90e13 — the tree caps it |
| + tree-level spill (the child's Q_i and the running P cold during P_i·Q_run, ≈ 60 GB; assumed) | 243 | ≈ 240 | ≈ 385 | ≈ 9.6e10 (assumed: −65 GB at the marginal 4.2 bytes/digit of the 452 → 463 GB step §82) | ≈ 5.5e13 |

So: spilling does **not** raise the 576-node ceiling the way it raises the one-node one. The per-node budget at 576 also carries the top product's scratch (80 GB, which `MN_T_CHUNK_MB=1024` — built, off — cuts to ≈ 50: M13), the exchange and the SHMEM pool, and the tree's top level (294 GB) is the second wall right behind the dm need. The cheaper lever there is the built chunking switch (5.32e13 modelled, M13/§78) before any spill. At the chosen target (4.25e13, 7.38e10/node) a V2 spill converts to **margin** (452 → 437 GB per node; with the tree spill → ≈ 385), which is relevant for the open SHMEM-pool sizing (T0) and the exchange-stacking question (M13 open issue 2), not to digits.

Time at 576: V2 = P share 36 GB written under the reciprocal (hidden if `/ssd0` ≥ ≈ 0.7 GB/s over the ≈ 50 s reciprocal; assumed) + 36 GB read exposed = **+12–36 s on the 234 s wall (+5–15 %)**; V3 doubles it; the tree spill adds ≈ 2 × 60 GB per node exposed at each top level: +40–120 s. All per node, in parallel over the nodes, each on its own `/ssd0` (assumed).

---

#### 7. The doc's claims about ntt/ — current / stale / wrong

| claim | status | evidence |
|---|---|---|
| single-node d_max 7e10 "in-core, ~440 GB" | **stale** (Phase 8, §71 of 2026-09-18) | measured ceilings at three primes: 1.44e11 (2³⁰), 1.30e11 (2³¹), 1.14e11 (3·2³⁰) — P13b jobs 21072/21069/21059, VERIFY OK; the defaults run 1.252e11 (G13d) |
| µ @ 4e10 = 73.1 s | **stale** | 63.5 ± 1.5 s (five runs, §80 job C13c); 67.8 ± 1.9 with three primes alone (§78) |
| bytes/digit 6.3 (440/70) | **stale** | 340.7/7.64e10 = 4.46 (§80); 507.6/1.44e11 = 3.52 (2³⁰), 507.1/1.30e11 = 3.90 (2³¹) (P13b) |
| "7e10/node × 576 = 4.0e13" | **mixed**: 4.0e13 is the modelled 576 ceiling at 6.95e10/node (M13/§78), coincidentally ≈ 7e10; the one-node ceiling is not what limits 576 | current: target 4.25e13 (7.38e10/node) in 234 s at 452 GB (§82); design-table ceilings 4.66e13 (defaults, 480 GB), 5.5e13 (recommended), 6.4e13 (largest) (§79/§80) |
| "235e9/node × 576 = 1.35e14" | **wrong** on two counts | (i) the 576 per-node ceiling is 55–70 % of the one-node one (tree top level, top scratch, exchange, SHMEM pool: section 6); (ii) time: apumult is 3.7× slower per digit at 4e10 (234.8 vs 63.5 s) and our own run at 1.44e11 takes 18 min per node (G13d); a 2.35e11-per-node run is ≥ 30 min per node by either extrapolation, against a 234 s target |
| "dm phase overflows the block pool @ 7e10 (§71)" | **stale** | zero in-phase hipMalloc at 4e10, 8e10, 1e11 since M11's tail arena (M13 calibration table); the only in-phase failure is fragmentation at 1.245e11 (G13d) |
| "their bs top levels overflow @ 8e10" | **wrong** | 8e10: 369.1 GB, hipMalloc 0 (M11/M13); every default run 5.12e10 … 1.163e11 clean (G13d); region growth aborts unless `RNS_POOL_GROW=1` (Phase 12 R) and none was needed |
| "recip peak block-pool 121.5 GB @ 4e10" | **not ours** | measured arena 132.3 GB at 4e10 (M13/§77); Phase 10's pool 141.0 (`mem_model.py MEASURED`) |
| "zero spill/stream/trim of working set; the only disk write is BS_CKPT_*" | **current**, with a nuance | `ECALC_CKPT_TOP` (off by default) writes P under the reciprocal and Q under the division and releases P before S = P + Q (§77, N13): the write half of a P spill exists; the buffers are never freed for memory, and Q is *held* +17.8 GB when the writer lags (§78) |
| "1 madvise (HUGEPAGE), disabled" | **roughly current** | four `MADV_HUGEPAGE` sites (`bigint.c:17`, `mem.c:134/230/252`); all off the default path (`BI_HUGE=0` measured harmful; the mem.c ones are in the non-default mmap/hreg forms) |
| "dbig.c 1 GB pinned chunks, 1.6–2 GB/s" | **half right** | dbig.c's bounce is 2 × 1 GiB per APU for host↔device copies (≈ 50 GB/s pinned); the checkpoint path uses the 1 GiB `rns_hstage` chunk; 1.6–2 GB/s is WP7's NVMe DMA figure, the write with fsync ≈ 1 GB/s, aac6's slow path 0.31 GB/s |
| "ntt/ divisor cache has no trim/rebuild" | **misleading** | no divisor cache on the decimal path at all |
| "their planes are fixed 2³¹/3·2³⁰" | **stale** | `ECALC_PLANE_CAP` with four caps and `fit` (P13b); 2³¹ the default (§80) |
| "ntt/ has no OOM guard" | **current** | edge runs were killed by the kernel (P13b) or a refused hipMalloc (G13d); the guards are pre-run (`BS_LAYOUT_ONLY`, `ECALC_INIT_ONLY`) |
| "§66 grid-split, no madvise" | current (the grid is `rns_dist.c` mn/mul grids; no madvise on device memory is possible) | — |

---

#### 8. What to test, ranked

1. **V2 — P spill under the reciprocal + the tail policy** (`ecalc.c` 491–513, `bs_ckpt_bg_*` with `db_free` on part 0 done, restore via `ckpt_dbig_io`; `dm_layout` v2 − 1.1 B and v3 − hole; tail = the dead parity's half). Gate: `BS_LAYOUT_ONLY` shows the new arena; `ECALC_INIT_ONLY` maps at 1.5e11 (2³¹); a real 1.5e11 run VERIFY OK on aac6 (today's edge is 1.30e11). Expected: **1.30 → ≈ 1.68e11 at 2³¹, 1.44 → ≈ 1.79e11 at 2³⁰ (modelled)** for one exposed P read (25–75 s). This is also the one that pays at 576 (+5 % digits, or −15 GB per node of margin).
2. **V3 — r2 reserved after the first product; Q spilled from the last iteration's second product through the high product; Aw formed and S freed before the low product** (`recip_db2`, `newton_db_divmod_shifted`, `newton_mn_divmod`). Expected +1 % (2³¹) / +5 % (2³⁰) over V2; the reciprocal's r + r2 + t1 + piece and the low product's Q + X + xq + Aw are then the floor (≈ 4.2 B + piece).
3. **576: `MN_T_CHUNK_MB=1024` first** (built, off; −30 GB per node of top scratch, 5.3e13 modelled, M13) — cheaper than any spill there; then the tree-level spill (child's Q_i, running P during P_i·Q_run in `tree_level_k`) modelled in `tree_need_dev` before writing it; expected ≈ 9.6e10/node (assumed).
4. **The 10 % bound margin on Q and S in `dm_layout`** (≈ 2 %): trivial, verify the bound first.
5. **V6 (band-sized t1, the window in chunks)** only if 2e11+ per node is wanted: the remaining ≈ 15 % to apumult's 2.0 bytes/digit, at 15–30 min per node.
6. Not worth porting: µ spill/stream, Q stream into the grid products, the r-spill/stream-shl, madvise(DONTNEED), the DC/pow10 trims, `BATCH_DEVDAB_TIGHT` (superseded by `ECALC_PLANE_CAP`).

## apumult_F2 — apumult vs ntt/ — analysis F2: categories B (madvise), C (in-place/view/steal), E (instrumentation/safety), F (kernel/algorithm variants)

(former file: apumult/apumult_F2.md)

Source read in full: `~/apucode/apumult.md` (130 lines). apumult's source is not available; every "what it
does" below is reconstructed from the row's name and the doc's one-line mechanism. Every ntt/ claim is grepped/read in
the tree at `~/apucode/ntt` (file:line). Labels: **measured** (a RESULTS/results number with its job),
**modelled** (`ecalc/mem_model.py` / `binsplit.c dm_layout`, the same formula to the byte per M13), **assumed** (code
reading, no run). No code was changed.

Sizes used throughout (n_Q = Q's limbs × 8 B per node, modelled by `mem_model.dm_layout`, decimal, 3 primes, 2³¹ cap):

| size | n_Q (limbs) | n_Q (GB, whole node) | k_µ (GB) | t1 cap (GB) | bs regions (GB) | dm need = arena (GB) | planes (GB) | node peak modelled (GB) |
|---|---|---|---|---|---|---|---|---|
| 7.4 × 10¹⁰ per node (the 576 share) | 4.11 × 10⁹ | 32.9 | 32.9 | 65.8 | 173.7 | 229.6 | 103.7 | 346.5 at size 1; **452** at 576 (RESULTS §82: + top scratch, exchange, SHMEM, host 30) |
| 10¹¹ one node | 5.56 × 10⁹ | 44.4 | 44.4 | 88.9 | 242.9 | 310.3 | 103.7 | 427.9 (measured 431.2 device at 4 primes, M11) |
| 1.3 × 10¹¹ one node (the measured 2³¹ edge, P13b) | 7.22 × 10⁹ | 57.8 | 57.8 | 115.6 | 296.5 | 403.4 | 103.7 | 522.0 (measured 522.8, P13b) |
| 2.35 × 10¹¹ one node (apumult's d_max) | 1.31 × 10¹⁰ | 104.4 | 104.4 | 208.9 | 553.9 | 729.2 | 103.7 | 851 — 1.6 × the node |

#### 0. The fact that decides every row in B and C

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
| `hipFree` + later `hipMalloc` | Yes, between phases (the pool does it for its own `kind == 2` regions at `db_release_pools`, `dbig.c:187-194`) | `hipFree` ≈ 3 ms/GB (RESULTS §22: 0.023 s per 8 GiB, measured); re-`hipMalloc` of memory the same process freed **0.035 s/GB** (results/MEMORY.md §I:28-30, measured: the TTM page pool keeps the cleared pages), a fresh mapping 0.057–0.072 s/GB (measured, §77 I). So a 100 GB release-and-remap costs ≈ 4 s | usable, but pointless unless another consumer needs the bytes *at that moment* (see below) |
| HIP virtual memory management (`hipMemAddressReserve` / `hipMemCreate` / `hipMemMap` / `hipMemSetAccess` / `hipMemUnmap` / `hipMemRelease`) | In principle: reserve the arena's VA once, back it with physical chunks (2 MiB granularity) and unmap/release dead sub-ranges, re-back later without moving anything | `hipMemCreate` pays the same page clearing as `hipMalloc` (the kernel clears on allocation: 0.057–0.072 s/GB fresh, 0.035 warm — assumed equal, not measured); `hipMemRelease` ≈ `hipFree` | **untested and the most interesting form** — not for releasing bytes but because it decouples contiguity from physical placement: the reserved-tail/hole policy (`dbig.c:60-105`, M11 §1) and the deterministic OOM at 1.245 × 10¹¹ (G13d: 41.2 GB free in 2 extents, largest 27.5, request 27.67) exist only because a block must be one contiguous `hipMalloc` range (`dbig.c:38`). With VMM the pool could back any contiguous VA range from any free physical chunks. Open: whether the API exists on ROCm 7.2.4 for the APU's HBM (it is in HIP since ROCm 5.x as the CUDA-VMM equivalent — assumed), whether peer access (the dist tier's xGMI pulls, `rns_dist.c:313`) works through `hipMemSetAccess` mappings, and its clearing cost. A `vmm` form in `ecalc/tests/t_alloc.c` (the harness already times alloc/fill/bw/ntt/d2d-peer/free per form) is ≈ 2 h |

**Who would consume released bytes?** On one node: nobody — the planes are live in every phase (the dm's products use all
of pool 0 and 3q of pool 1, results/MEMORY.md §M:84-86), the host is 12–16 GB, and there is one process. At 576: the SHMEM pool
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

#### 1. Category B — madvise(DONTNEED) rows

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

#### 2. Category C — in-place / view / steal

| env / M# | what it does | our analogue (file:line) | doc verdict | our verdict | GB | worth testing? |
|---|---|---|---|---|---|---|
| `DM_R_INPLACE` M38 — R.limb = A.limb view | the remainder R is formed in A's own limbs (the window), no separate R buffer | `newton_db_divmod_shifted`: the window Aw is formed by `db_set_shifted_low` (`newton_db.c:254`) and **R is Aw itself or xq itself** after the subtraction (`:257-262`: `Rd = Aw; db_init(&Aw)` / `Rd = xq; db_init(&xq)`) — no R buffer; R's residues are taken on the device and R freed (`:272-274`). The separate `bi_reserve(R, w + 1)` is the older host-output path `newton_db_divmod` (`:184`), not the default flow (`ovl3`, `ecalc.c:539`) | **UNIQUE** — "ntt/ dm has R separate (device)" | **wrong — EQUIV** on the default path | 0 (already had) | no |
| `BS_TP_INPLACE` M24a — bs Tp/T → nxtP alias-add | P = P1·Q2 + P2 accumulated into the product buffer, no temporary | device tier: `db_add(o->pd, o->pd, &P2)` in place (`binsplit.c:1215`); batch tier: the add on the host then one DMA (`:1246-1248`); the grid itself accumulates pieces in place (`rns_dist.c:906 db_add_shifted(Cd, &t, oa + ob, Cd)`) | EQUIV | **EQUIV** | 0 | no |
| `BS_STEAL_PQ` M50 — result P/Q ← cur.buf steal | the final level's P, Q take the level buffer instead of being copied out | `bs_Pd = *nxt.nd[0].pd; bs_Qd = *nxt.nd[0].qd` (`binsplit.c:1227`, the block moves, no copy); the copy exists only in the non-device flow (`:1318-1319`) and at size > 1 for the leaf hand-over (`:1306`: a deliberate copy so the regions can be donated before the tree, B6) | EQUIV | **EQUIV** | 0 | no |
| `NEWTON_QTVIEW=2` M36 — Q_t view (no copy) | the top `take` limbs of Q used as an operand without copying | `dbig qt = db_view(Qd, nq - take, take)` (`newton_db.c:78`); views are not owning (`dbig.c:701-704`) and the dist tier packs them at their offset (`rns_dist.c:46, 201`) | EQUIV | **EQUIV** | 0 | no |
| `P10_SWAP` M23b — pow10 swap-not-copy | the pow10 tower's buffers swapped instead of copied | no pow10 on decimal; on binary `pow10_big` (`ecalc.c:519`) — not looked at, the path is not the target | EQUIV (trivial) | N/A-decimal | 0 | no |

Category C is fully covered on our side; the doc's one UNIQUE is a misreading of the older host-output routine.

#### 3. Category E — instrumentation & safety

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
| `pollv3.sh` phase-aware RSS watchdog (TH = 372 GB "ch-live", 452 GB "sticky-DEEP"; kills before OOM) | a sidecar polling RSS with thresholds per phase, killing the run before the kernel's OOM killer does | none at run time; the guard is **pre-flight**: the layout formula equals the C request to the byte at 35 points (M13) and the measured device total to 0.05 % at 10¹⁰–10¹¹, the budget 480/524 GB (internal target notes:30`), `fit`. The two failure modes seen: (a) a refused `hipMalloc` → `mem_oom`, clean exit 1 with the report (G13d 1.245 × 10¹¹, deterministic, pool fragmentation); (b) the kernel OOM killer (SIGKILL) when device + host HWM ≈ 529 GB, twice in P13b (1.46 × 10¹¹ at 2³⁰, 1.16 × 10¹¹ at 3·2³⁰), both **in bs right after the seeds**, where the host RSS grows 12 → 16 GB — the one term the model carries as a constant (`BS_HOST_INIT_BYTES`, `binsplit.c:381`) | UNIQUE — "critical for pushing d_max safely" | **SUPERSEDED for the device part** (a run that will not fit is refused before it maps; the mapped total is known at init and does not grow), **residual for the host part** (+4 GB in the seeds, the SHMEM pool T0, TCP staging at size > 1: M13 open issue 1). "cpuset-OOM would wipe": on slurm the job step is its own cgroup, and in P13b the kernel killed `ecalc` itself, not the node; on a shared node (the M-run's three processes at 10¹⁰) it could take a neighbour. A killer does not improve the outcome at 576: a SIGTERM'd rank leaves the other 575 in a collective as much as a SIGKILL'd one — the fix there is a collective abort on a memory check, which `mn_barrier`/exit-5 machinery could carry | a *sampler* yes (next row); a killer no — keep the budget margin (452 modelled of 480) and the pre-flight instead |
| hwm-poller-v2 (5 s RSS + HWM + state + log tail) | continuous sampling | phase-boundary tables only; the intra-phase pool HWM is captured (`pl:peak`) but RSS between boundaries is not, and nothing samples the driver's used bytes over time | UNIQUE (continuous) | **agree** — the host RSS growth in the seeds (P13b) and the device total during the tree at size > 1 are seen only at the boundaries today | **yes, S (≈ 1 h)**: a thread in `mem.c` sampling `VmRSS` and `hipMemGetInfo` per APU every N s into the log (`MEM_SAMPLE_S`), or the same as a shell sidecar in `g13d_run.sh` (`ps`/`rocm-smi --showmemuse` — the hang capture already runs this once). Attribution when pushing the edge on the target |
| `[bspk]`/`[dcpk]` intra-level RSS probes | per-level RSS/HWM print | per-level lines at `ECALC_VERBOSE=2` (times, tiers, grids) without a memory column; `NEWTON_VERBOSE` has pool GB per doubling (`newton_db.c:104`) but **pool bytes, not live bytes** | UNIQUE (finer) | **partly agree** — one column short: add `g_live_bytes` (already accounted, `dbig.c:44`) to the per-level and per-doubling lines. This is also what would *measure* the used-at-peak fractions the B estimates assume | yes, T (10 lines); do it before any B work |
| `[batch]`/`[mdev]` per-call sub-timers | ntt/cc/rp/h2d/d2h/join per call | per-call `dist_db`/`dist_mn` lines and per-phase sums of load/ntt/crt/spills (`newton_db.c:108`, `rns_dist.c:914, 1527`), `DIST_STATS` per part, `COMM_LAYER_STATS` stage timeline | "Partial (DIST_STATS, less granular)" | **wrong — EQUIV or finer** (the G13d 2³⁰ analysis used exactly these: 452 vs 48 s of load over 510 calls) | no |
| `subphase_sum.sh` post-run Σ | a script summing the sub-timers | `archive:results_raw_2026-10-19/g13d/g13d_table.py` (tables from the logs: pieces, dist calls, phases), the `recip(db)`/`divmod(dev)` sum lines themselves | UNIQUE | mostly covered; a generic `Σ` over the `RNS_VERBOSE` lines would be a 30-line script | T, when needed |
| ENV_L3 / ENV_DMAX split | two env files: the µ benchmark vs the d_max push | one default (13c) plus internal target notes' launch line and `DESIGN_TABLE.md`'s "largest" row (`B4, 2^30, both chunkings, depth 1`) | UNIQUE | a documentation convenience; the design table already names the d_max profile | T, optional |
| d-wall progression table (per-d HWM by phase, d160–d240) | measured points along d | **G13d §(a): 22 sizes from 5.12 × 10¹⁰ to 1.252 × 10¹¹ with device GB, phases, pieces, dist calls, every run VERIFY OK** (`results/TARGET.md §G13d:65-92`); P13b's edge table at four caps | "Model-only (`mem_model.py`)" | **wrong — we have it, measured** | no |
| `DC_DMN_PROBE` | a probe in the dc divmod node | binary-only, N/A | UNIQUE | N/A on the target path | no |

#### 4. Category F — kernel / algorithm variants

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

#### 5. Ranked: what to test (F2's categories only; F1 owns the spill items)

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

#### 6. Doc verdicts corrected (summary)

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

#### 7. Porting plan — how each adoptable item goes into the device-resident code, and what it gives

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
(RESULTS §22, §77 I, results/MEMORY.md §I); the VMM API's presence and cost on ROCm 7.2.4 — assumed; the digit gains — modelled
from the node-peak slope, not run.

## apumult_M — apumult spill study: disk and APU memory measurements (agent M)

(former file: apumult/apumult_M.md)

Measured 2026-09-23, 22:58 to 23:22 CDT, on aac6 node **aac6-spx-a** (job 21121, `-p PPAC_MI300A_SPX -N1 --gpus=4`).
Nothing in the repo was touched. Sources are in `scratchpad/diskbench/` (`diskio.c`, `hipmem.hip`). They were built on the node:
`gcc -O2 -pthread diskio.c -o diskio`; `module load rocm; hipcc -O2 hipmem.hip -o hipmem -lpthread`.
Every file written to the node disk was removed (`rm`), and `~/diskbench` on aac6 was deleted afterwards. Disk free stayed at 1.3 TB throughout.

**Node facts (measured):** the NVMe is a **Samsung SSD 980 PRO 2TB**, not a 970 EVO Plus. It runs at PCIe 16 GT/s x4 with the mq-deadline scheduler, and ext4 is mounted on `/` (so `/tmp` is on it). 505 GB were used and 1.3 TB free.
MemTotal is 502 GiB and MemAvailable about 493 GiB at idle. `vm.dirty_ratio` = 20, `dirty_background_ratio` = 10. liburing and fio are not installed, so queue depth comes from threads.
Note: aac6 is only a lower bound for disk bandwidth. The target system has its own `/ssd0`. The APU memory findings (sections 2 to 4) carry over, because the node architecture is the same.

#### 1. Sequential file I/O on /tmp (1 GiB blocks)

| test (measured) | size | threads | GB/s | memory effect |
|---|---|---|---|---|
| O_DIRECT write + fsync | 32 GiB | 1 | **2.07** | Cached flat at 0.8 GiB |
| O_DIRECT write + fsync | 32 GiB | 4 | 1.95 | none |
| **O_DIRECT write + fsync, sustained** | **200 GiB** | 1 | **2.00** (flat 1.6 to 2.15 in every 2 s window, 0 to 200 GiB; no SLC cliff seen within 200 GiB) | none |
| O_DIRECT read | 32 GiB | 1 | **5.92** | none |
| O_DIRECT read | 32 GiB | 4 | 5.37 | none |
| buffered write + fsync | 64 GiB | 1 | **0.65** (0.70 before fsync). Dirty reaches about 15 GiB, then the writer is throttled to about 0.55 GB/s writeback | Cached +64 GiB (0.8 to 64.8) |
| buffered write + fsync | 24 GiB | 4 | 1.79 into the cache, **0.92 including fsync** | Cached +24 GiB, Dirty 24 GiB |
| buffered write, flush + DONTNEED every 2 GiB (`-k 2`) | 24 GiB | 1 | 0.50 | Cached peak 3.3 GiB |
| buffered read (cold) | 32 GiB | 1 | 2.82 | Cached +32 GiB |
| buffered read (cold) | 200 GiB | 1 | 2.76 | Cached +200 GiB |

Summary: the sustained spill rate on aac6 is **2.0 GB/s write and 5.9 GB/s read, with O_DIRECT**. Buffered writes are 2 to 4 times slower (0.5 to 0.9 GB/s), because ext4 writeback is the bottleneck.
More threads do not help O_DIRECT here: with 1 GiB requests, one thread already saturates the drive.
Commands: `./diskio -w -d -f /tmp/dbm.bin -s 200 -y`, `./diskio -r -d -f /tmp/dbm.bin -s 32 [-t 4]`, `./diskio -w -f /tmp/dbm.bin -s 64 -y`, `./diskio -w -k 2 -y -a -f ... -s 24`, `./diskio -r -f ... -s 200`.

#### 2. Does buffered I/O eat HBM on the APU?

| moment (measured) | MemFree | MemAvailable | Cached | hipMemGetInfo free (each APU) |
|---|---|---|---|---|
| idle | 494.4 | 493.2 | 0.8 | 128.0 / 128.0 |
| after 64 GiB buffered write + fsync | **428.6** | 492.4 | **64.8** | **128.0** (blind to it) |
| holding 4 x 100 GiB hipMalloc (+memset) on top of that | 36.1 | 89.6 | 54.8 (10 GiB evicted) | 27.8 |
| after hipFree | 437.7 | 491.2 | 54.8 | 127.8 |
| after `posix_fadvise(DONTNEED)` on the file | **494.7** | 493.6 | **0.8** | 128.0 |
| after 200 GiB buffered read (cache full) | ~293 | 492.7 | **200.1** | 128.0 |
| holding **4 x 112 = 448 GiB** hipMalloc (+memset) | 10.1 | 41.7 | **33.8** (166 GiB evicted) | 15.8 |

Findings (measured):
- **Yes, the page cache is HBM.** A buffered write or read of N GiB takes N GiB out of MemFree and does not give it back on its own (Cached stays after fsync). MemAvailable counts that cache as reclaimable.
- **`fsync` + `posix_fadvise(POSIX_FADV_DONTNEED)` gives all of it back at once** (Cached 64.8 to 0.8, and 200 to 0.3). DONTNEED only drops clean pages, so fsync (or `sync_file_range` WAIT) must come first. The `-k` mode, which flushes and drops every 2 GiB, keeps the cache under 3.3 GiB but writes at only 0.50 GB/s.
- **O_DIRECT avoids the cache entirely**: Cached and Dirty stayed flat at 0.3 to 0.8 GiB for 200 GiB written and read.
- **A clean page cache does not block a large hipMalloc.** 448 GiB across 4 APUs succeeded with 200 GiB of clean cache present, and the kernel evicted 166 GiB of cache to make room. The cost is time: 448 GiB of hipMalloc + memset took **76.9 s with the cache full, against 36.5 s with it empty**.
  Dirty pages would first have to be written back at about 0.5 GB/s. That is the real danger of buffered spills: up to 20 % of RAM (dirty_ratio) can be dirty.
- **Trap: `hipMemGetInfo` is useless as a free-memory gauge on the APU.** It reports 128 GiB minus this process's own hipMalloc on that APU. It ignores page cache, host malloc, and even memory the process holds on other APUs (while holding 224 GiB it said 71.8 GiB free per APU = 287 GiB, but MemFree was 68.6 GiB). Use `/proc/meminfo` MemFree / MemAvailable instead.
- **Trap: overcommitting hipMalloc does not return an error; the process gets OOM-killed.** Holding 200 GiB of touched host memory and then asking for 4 x 112 GiB of hipMalloc + memset produced `srun: task 0: Killed`, not hipErrorOutOfMemory.

#### 3. Spilling device (hipMalloc) memory to NVMe

`./hipmem spill G mode ndev`: hipMalloc G GiB on each APU, filled by a kernel, written in 1 GiB chunks to `/tmp/spill_d.bin` (O_DIRECT) + fsync, then the buffer is zeroed, read back, and checked by a kernel. The file is unlinked afterwards.

| mode (measured) | APUs x size | write+fsync GB/s | read GB/s | verified |
|---|---|---|---|---|
| **direct**: O_DIRECT `pwrite()` straight from the hipMalloc pointer | 1 x 16 GiB | **fails: `pwrite: Bad address` (EFAULT)** | not run | n/a |
| **bounce**: hipHostMalloc 2 x 1 GiB, hipMemcpyAsync D2H double-buffered, then O_DIRECT write (reverse: O_DIRECT read, then H2D) | 1 x 16 GiB | **2.24** | **6.22** | 0 mismatches |
| bounce, 4 threads (one per APU, one file each) | 4 x 16 GiB | **2.05 aggregate** (0.52 each) | **5.03 aggregate** (1.3 each) | 0 mismatches |
| cpucopy: CPU `memcpy` from the hipMalloc pointer into a posix_memalign buffer, then O_DIRECT | 1 x 16 GiB | 1.26 | 3.25 | 0 mismatches |
| managed: `hipMallocManaged` buffer, O_DIRECT `pwrite()/pread()` directly on it | 1 x 8 GiB | **2.57** | **6.24** | 0 mismatches |

Findings (measured):
- **O_DIRECT from a hipMalloc'd pointer does not work.** The CPU can load and store it (cpucopy works), but the kernel cannot pin those pages for DMA (get_user_pages), so the call fails with EFAULT. A bounce buffer is needed.
- **The bounce path runs at full disk speed** (2.2 GB/s write, 6.2 GB/s read, one APU). The D2H/H2D copy is hidden behind the disk by double-buffering. Four APUs in parallel share the one NVMe and get the same aggregate (2.05 / 5.0 GB/s).
- The CPU memcpy path costs about 2x, because CPU reads of device-coarse-grained memory are slow. Do not use it.
- `hipMallocManaged` memory *can* be the direct target or source of O_DIRECT (no bounce, 2.6 / 6.2 GB/s). It is an option only if the buffer can be allocated managed; the pipeline's dbig pools use hipMalloc.
- Spill cost per GB at aac6 rates: about 0.49 s/GB to write and 0.17 s/GB to read back, so a 100 GB round trip takes about 66 s on this disk.

#### 4. madvise(MADV_DONTNEED) on host memory, and hipMalloc

| step (measured, `./hipmem madv 200`, `./hipmem madvalloc 200 110 1`, `./hipmem madvalloc 200 56`) | MemFree GiB | hipMemGetInfo free | hipMalloc result |
|---|---|---|---|
| idle | 494.0 | 128.0 each | not run |
| 200 GiB mmap'd (or malloc'd) and touched | 293.9 | **128.0 (unchanged)** | not run |
| after `madvise(MADV_DONTNEED)` (5.5 s for 200 GiB) | **493.9** | 128.0 | not run |
| then hipMalloc + memset 4 x 112 = 448 GiB in the same process | 43.1 | 15.8 | **OK, 36.5 s** |
| (other run) 200 GiB held, then 4 x 56 = 224 GiB hipMalloc | 68.6 | 71.8 | OK but **51.6 s** |
| same after madvise | 268.7 | 71.8 | OK, **17.5 s** |
| 200 GiB held, then 4 x 112 GiB hipMalloc (does not fit) | not shown | not shown | **process OOM-killed** |

Findings: **madvise(DONTNEED) returns host memory to the pool that hipMalloc draws from, immediately and in full.** MemFree goes back by 200 GiB, and 448 GiB of hipMalloc then succeeds. hipMemGetInfo does not show this, because it never saw the host allocation in the first place (see the trap in section 2).
hipMalloc into a node under memory pressure is about 3x slower (51.6 s against 17.5 s for 224 GiB), because the kernel has to reclaim and compact first.

#### Bottom line for the proposal
1. The spill rate on aac6 is **2.0 GB/s write and 5.9 GB/s read, sustained, with O_DIRECT.** Buffered writes run at only 0.5 to 0.9 GB/s. Use O_DIRECT, or at least fsync + fadvise DONTNEED.
2. **Buffered I/O does eat HBM** (the page cache lives in the same 512 GB). A clean cache is reclaimable, but reclaiming it doubles hipMalloc time. A dirty cache also costs writeback time. O_DIRECT or fsync+DONTNEED removes the problem.
3. **O_DIRECT straight from hipMalloc memory fails (EFAULT).** Use a double-buffered hipHostMalloc bounce, which reaches full disk speed, or hipMallocManaged buffers, which work directly.
4. **madvise(DONTNEED) on host buffers really returns HBM to hipMalloc.** Measure with /proc/meminfo, never hipMemGetInfo. Overcommitting hipMalloc means an OOM kill, not an error code.

## apumult_R: spilling to SSD and releasing memory on MI300A (research findings)

(former file: apumult/apumult_R.md)

Agent R, 2026-09-24. Scope: what the apumult techniques (SSD spill, madvise(DONTNEED)) can
do for a **device-resident** e pipeline on MI300A nodes, and what they cost. The research used
web sources plus the Linux v6.8 kernel source (amdgpu/TTM/mm), since several answers are
settled by kernel code rather than documentation. No GPU was available to this agent, so every
claim marked "verify" comes with a 1-minute test to run on aac6 (see §8).

Confidence scale: **H** = documented or read in kernel source; **M** = strong inference from
source or docs, not tested on MI300A; **L** = plausible, not confirmed.

---

#### 0. Answers in brief

| Question | Answer | Conf. |
|---|---|---|
| (a) Can an APU pipeline spill to disk without the page cache taking the freed HBM? | **Yes, but only with O_DIRECT (or RWF_DONTCACHE on kernel ≥ 6.14).** Buffered I/O puts the page cache in the same HBM, and 2 MiB `hipMalloc` allocations will **not** reclaim it (they fall back to smaller pages). `posix_fadvise(DONTNEED)` only drops pages that are already clean. | H |
| (b) Realistic spill bandwidth | **aac6 (970 EVO Plus 2 TB, PCIe 3.0 x4):** about 3.3 GB/s write for the first ~78 GB, then **~1.5–1.75 GB/s** (drops to ~0.8 GB/s if the drive is the later "Elpis" revision). Read is **~3.3–3.5 GB/s**. **Target (EX255a-class):** one optional M.2 per node, PCIe Gen4. Expect **~2–4 GB/s write, ~5–7 GB/s read**. The model is not public, so **measure /ssd0 with fio**. HBM moves 3.5 TB/s per APU. | H for aac6, L–M for target |
| (c) Does HIP VMM give a cheap "release dead pages" for device buffers? | **Yes, it works functionally.** `hipMemUnmap` + `hipMemRelease` of a chunk returns its pages. `hipMemCreate` + `hipMemMap` puts pages back later. The cost is kernel page zeroing, **~35–40 ms/GiB** (the same as hipMalloc). It is **still Beta** and has open issues on gfx94x. **Chunked hipMalloc/hipFree gives the same saving** with no Beta API. `madvise` on a hipMalloc pointer **cannot** release anything: the kernel rejects it with EINVAL. | H (mechanism), M (cost) |

---

#### 1. MI300A memory model

##### 1.1 What backs each allocator

- **`hipMalloc` → TTM "GTT" buffer object made of ordinary system pages. On an APU there is no VRAM.** In the kernel, KFD sees `is_app_apu` and changes a VRAM request into `AMDGPU_GEM_DOMAIN_GTT`. The allocation is counted against both the system-memory limit and the **TTM limit** (`ttm_mem_needed = size`). **(H)** Source: `drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c` (v6.8), lines ~199 and ~1656:
  <link: see archive:results_merged_2026-10-19/apumult_R.md>
  - Pages come from a per-NUMA-partition TTM pool (`amdgpu_ttm_pools_init`, one pool per memory partition with its NUMA node). **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md>
  - This TTM limit is the **96 GiB-of-128 default** for device allocators. It is raised with `modprobe amdttm pages_limit=134217728 page_pool_size=134217728`, counted in 4 KiB pages. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md>
  - hipMalloc allocates up front, gives the most contiguous pages (158 K TLB misses against about 1 M for the other allocators), reaches the best bandwidth (**3.5–3.6 TB/s**), and costs **~37 ms per GiB** to allocate. **(H)** "Dissecting CPU-GPU Unified Physical Memory on AMD MI300A APUs", <link: see archive:results_merged_2026-10-19/apumult_R.md>
- **`hipHostMalloc`** is up front and costs 200–400 ms/GiB, with 2.1–2.2 TB/s GPU bandwidth in the paper above. AMD says "host allocations are not affected" by the 96 GiB limit. **(H)** Our own measurement agrees: 460 GiB through hipHostMalloc gets past the amdttm cap, at 14.0 TB/s per node (memory note `apu-ntt-measured-facts`). Getting past the TTM cap means it is **not** a TTM GTT BO. The kernel code points to a USERPTR/SVM mapping of ordinary anonymous pages. **(M)**
- **`hipMallocManaged`**: with XNACK off it is up front (2.1–2.2 TB/s). With `HSA_XNACK=1` it allocates on first touch (1.8–1.9 TB/s). **(H)** arXiv 2508.12743; <link: see archive:results_merged_2026-10-19/apumult_R.md>
- **`malloc`**: ordinary anonymous memory. The GPU can reach it only with `HSA_XNACK=1` plus an HMM-capable driver, through GPU page faults of ~18 µs each and up to ~1.1 M pages/s. On MI300A, "physical page migration is neither needed nor useful, however, handling page-faults via XNACK is still necessary". LLNL measures a ~15 % GPU penalty from 4 KiB pages unless THP is on. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md>
- AMD **requires** `transparent_hugepage=always` and proactive compaction (`compaction_proactiveness=20`) on MI300A, because "the APU dynamically shares memory between the CPU and GPU". **(H)** the AMD Instinct documentation system-optimization page above.

##### 1.2 Does madvise(MADV_DONTNEED) give back HBM that a later hipMalloc can use?

- **On `malloc`/`mmap` anonymous memory: yes.** The pages go straight back to the buddy allocator. hipMalloc's TTM pool allocates from that same buddy allocator on the same NUMA node, so the pages are usable, with two caveats:
  1. Release whole 2 MiB-aligned ranges. With THP=always, a partial range splits a huge page and leaves 4 KiB fragments, and hipMalloc then gets poorer TLB fragments.
  2. Do not use MADV_FREE. It is lazy, and RSS stays up until the kernel is under memory pressure.
  **(H** for the kernel mechanism, **M** for "no fragmentation penalty on the next hipMalloc")
- **With `HSA_XNACK=1` and the GPU touching that malloc memory**, the MMU notifier invalidates the SVM range and the pages are freed. The next GPU access faults in a zero page. This is the clean path for host-resident code such as apumult's. **(M)**
- **On a `hipMalloc` pointer: no.** The CPU mapping of a TTM BO is `VM_PFNMAP|VM_IO` (`ttm_bo_vm.c` line 481), and `madvise_dontneed_free_valid_vma()` forbids `VM_PFNMAP`, so the call returns **EINVAL**. Even without that check, it would only zap CPU PTEs. The BO would stay allocated. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> (lines 820–832), <link: see archive:results_merged_2026-10-19/apumult_R.md>
- **On `hipHostMalloc` memory: not recommended.** If it is a pinned USERPTR BO, the invalidation evicts the process's GPU queues, and the restore worker faults the pages back in, so nothing is saved and there is a stall. **(L, verify)**
- **Trap: freed hipMalloc pages do not return to "free".** `hipFree` hands pages back to the TTM pool, which **clears them** (`clear_page` in `ttm_pool_type_give`). The pool keeps up to `page_pool_size` pages, and AMD's recommended setting is all 128 GiB, until the TTM shrinker releases them under pressure. So after a hipFree, `MemFree` in /proc/meminfo stays low. An RSS or MemFree watchdog such as apumult's pollv3 will misread the situation. Pooled pages are, however, the first ones a later hipMalloc reuses. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> (lines 225–240, 541)

##### 1.3 The page cache lives in the same HBM

- MI300A has no DDR: "GPUs, CPUs, OS, ram disks (i.e., /tmp) all share same HBM3". On El Capitan-class systems, **/tmp is a RAM disk in HBM**, so never spill to /tmp. **(H)** LLNL El Capitan Getting Started, March 2026: <link: see archive:results_merged_2026-10-19/apumult_R.md>
- Buffered `write()` fills the page cache: up to `vm.dirty_ratio` (default 20 %, about 100 GB on a 512 GB node) of dirty pages, and after writeback **the clean pages stay** until something reclaims them. They sit on the NUMA node (APU) of the thread doing the writing. **(H**, standard Linux)
- **Why this matters for hipMalloc specifically:** TTM tries high-order (2 MiB) pages with `__GFP_NORETRY | __GFP_KSWAPD_RECLAIM`. That means **no direct reclaim or compaction** for large pages. It only drops to order-0, which does reclaim, when the large pages run out. So a node full of page cache does not make hipMalloc fail. It makes hipMalloc return **fragmented 4 KiB–64 KiB pieces** and the kernels run slower. **(H** code, `ttm_pool.c` lines 88–94 and 430–505; **M** for the performance effect.) Our own note agrees: a run right after a 40 GB digit comparison was 5–15 % slower until the file was evicted (memory note `apu-ecalc-phase8-state`).
- **How to avoid it:**
  1. **O_DIRECT** (buffer, offset and length aligned to 4 KiB; ideally 1–8 MiB I/Os). y-cruncher makes raw I/O mandatory for the same reason, the "thrash of death" in which "the OS is so aggressive with disk caching that it pages itself out". **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md>
  2. `posix_fadvise(POSIX_FADV_DONTNEED)` **only drops clean pages**. Dirty pages are skipped, so the order must be `sync_file_range(…WAIT_BEFORE|WRITE|WAIT_AFTER)` or `fdatasync`, then fadvise. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md>
  3. `RWF_DONTCACHE` (uncached buffered I/O, through `preadv2`/`pwritev2` or io_uring `rw_flags`) was merged for **Linux 6.14**. It needs no alignment and prunes pages after the I/O. aac6 runs 6.8, so it is not available there, and the target kernel is unknown. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md>

---

#### 2. HIP virtual memory management (VMM) on MI300A

- **API:** `hipMemCreate` (physical), `hipMemAddressReserve` (VA), `hipMemMap`, `hipMemSetAccess`, `hipMemUnmap`, `hipMemRelease`, `hipMemAddressFree`. Support is checked with `hipDeviceAttributeVirtualMemoryManagementSupported`. The docs list growing or shrinking a buffer without copying as the intended use. `hipMemAllocationProp` supports only `hipMemLocationTypeDevice` in 6.4. Newer releases add host-NUMA locations. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md>
- **Status:** the API is documented as **Beta**: "feature is complete, it can change and might have outstanding issues". It has existed since ROCm 5.x/6.0 on Linux. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md>
- **Known issues:**
  - On gfx94X CI (MI300 family), hip-tests show intermittent "hipMemCreate reports out of memory" failures for 64–128 MiB requests. The root cause is unknown. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md>
  - ROCm 7.14 opens **one file descriptor per hipMemCreate**. With `ulimit -n 1024` this shows up as a false OOM at about 1017 handles, and a fix has been submitted. Use large chunks or raise `ulimit -n`. **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md>
  - Unlike `hipFree`, release does not synchronise the device, so the caller must synchronise the stream itself before unmapping. **(H)** docs above.
- **Granularity:** query `hipMemGetAllocationGranularity`. Physical backing is **2 MiB**. Reported values vary by version (7.1.1 reports 4 KiB as recommended, 7.14 reports 2 MiB). Sizes that are not multiples of the granularity return hipErrorOutOfMemory. **(H** for dGPU, **M** for MI300A, verify)
- **Can it release part of a buffer and put pages back later? Yes.**
  1. Reserve one large VA range.
  2. Back it with N chunk handles (e.g. 256 MiB–1 GiB each).
  3. To release: `hipMemUnmap(chunk)` + `hipMemRelease(handle)`. The pages return to the TTM pool (cleared) or the buddy allocator.
  4. To put pages back: `hipMemCreate` + `hipMemMap` + `hipMemSetAccess` at the same VA.
  Kernels keep using one flat pointer. This is the device-memory equivalent of madvise DONTNEED. **(H** mechanism, CUDA-compatible semantics)
- **Cost:** on an APU, hipMemCreate(device) takes the same KFD path as hipMalloc: a TTM GTT BO with zeroed pages (`__GFP_ZERO` on fresh pages, or `clear_page` on pool return). Expect **~35–40 ms/GiB**, i.e. ~25–30 GB/s, single-threaded in the kernel, the same as hipMalloc's 37 ms/GiB, plus the GPU page-table update. Putting back 100 GiB therefore costs about 4 s, which is small next to 100 GiB of NVMe reads (30–60 s). **(M, verify)**
- **VMM memory still counts against the amdttm `pages_limit`** (KFD VRAM path, `ttm_mem_needed`). It is not a way past the 96 GiB default. **(H** code)
- **Cheaper alternative with the same saving:** allocate large buffers as K separate `hipMalloc` chunks with chunk-aware indexing (or a pointer table), and `hipFree` the dead chunks. VMM adds only the single contiguous VA. The zeroing cost is identical. **(M)**
- **VMM/hipMalloc memory cannot be an O_DIRECT target** (see §3). Spilling a VMM chunk still needs a bounce buffer.

---

#### 3. Moving device data to and from NVMe quickly

- **O_DIRECT into a hipMalloc pointer fails (EFAULT).** Direct I/O pins the user buffer with GUP, and `check_vma_flags()` returns `-EFAULT` for `VM_IO|VM_PFNMAP`, which is how TTM maps BOs. The same applies to hipMemCreate memory. **(H** code: <link: see archive:results_merged_2026-10-19/apumult_R.md> line 1039; **verify on aac6.)** A *buffered* `write()` from a hipMalloc pointer does work, because `copy_from_user` handles PFNMAP, but it fills the page cache.
- **O_DIRECT from malloc or hipHostMalloc memory works**: ordinary pages, and GUP succeeds. On the APU, **those buffers are GPU-accessible** (hipHostMalloc always; malloc with XNACK=1 or `hipHostRegister`). So a spill buffer that lives in hipHostMalloc memory is written to NVMe **with zero copies**, which is the APU equivalent of GPU-direct storage. **(M, verify)** Our measurement says hipHostMalloc memory runs at full kernel bandwidth (14.0 TB/s per node), while the arXiv paper reports 2.1–2.2 TB/s per APU for hipHostMalloc. Re-check with the NTT kernel before moving hot buffers.
- **For hipMalloc (or VMM) buffers use a bounce ring:** 2–4 pinned hipHostMalloc buffers of 64–256 MiB each.
  - Copy each chunk device→ring with an async copy or a copy kernel, then issue O_DIRECT pwrite from the ring, overlapped.
  - On an APU the copy is HBM→HBM: hipMemcpy/SDMA reaches ~58–90 GB/s, and a copy kernel much more. That is at least 20× faster than the SSD, so the bounce is free. <link: see archive:results_merged_2026-10-19/apumult_R.md>
  - The ntt/ `dbig.c` bounce path running at 1.6–2 GB/s (apumult.md §8) is disk-limited, not copy-limited.
- **Submission:** 2–4 threads doing `pwrite`/`pread` with O_DIRECT and 1–8 MiB requests, or io_uring with queue depth 8–32, saturate one NVMe device. For large sequential transfers io_uring saves CPU but no bandwidth. y-cruncher's advice is to keep I/O sequential and large. **(H** general; y-cruncher guide above)
- **AMD "GPU-direct storage":** **hipFile** ("AMD Infinity Storage", AIS) provides direct-to-GPU I/O and automatically falls back to POSIX.
  - Early-access technology preview, "not recommended" for production.
  - Requires ROCm ≥ 7.2, amdgpu-dkms ≥ 30.20.1, `CONFIG_PCI_P2PDMA`, local NVMe only (no multipath), ext4 (data=ordered) or xfs.
  - <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md>
  - **On MI300A it buys nothing** beyond O_DIRECT from hipHostMalloc memory, since device and host memory are the same HBM. It would matter only if the target forces hipMalloc buffers and you want to avoid the bounce, and the bounce is free anyway. aac6's default ROCm 7.2.4 meets its version floor, so it could be tried there. **(M)**
- There is also **ROCm XIO** (beta 0.1.0): GPU-initiated NVMe doorbells. It is research-grade and not needed here. <link: see archive:results_merged_2026-10-19/apumult_R.md>

---

#### 4. y-cruncher out-of-core ("Swap Mode"): lessons

**How it works**
- Swap Mode is true out-of-core arithmetic. Disk is "far memory", and all large operands, including the multiplication working sets, live there. RAM (by default ~94 % of physical memory) is the cache.
- It uses raw I/O (O_DIRECT / `FILE_FLAG_NO_BUFFERING`) to avoid the OS cache.
- Buffers are 64 MB per worker for HDDs and 1 MB for SSDs. Strided access should reach 1/3–1/2 of sequential bandwidth.
- Data is checksummed (0.1 % overhead) because silent disk errors happen.
- Source: <link: see archive:results_merged_2026-10-19/apumult_R.md>
- It pre-faults, locks, and fills its RAM with random data so the OS will neither swap nor compress it. Paging "even a small part" of it to disk degrades performance "by orders of magnitude". <link: see archive:results_merged_2026-10-19/apumult_R.md>

**Bandwidth requirement**
- Storage needs about **1/4 of memory bandwidth** before it stops being the bottleneck, with diminishing returns above ~1/3.
- In Yee's words, that is ~20 GB/s for a 7950X desktop and ~200 GB/s for a 256-core Bergamo.
- The 100 T pi run was "8-to-1 bottlenecked by storage", and no pi record has yet had enough bandwidth.
- The storage cannot be tiered: all of it must be fast.
- Source: <link: see archive:results_merged_2026-10-19/apumult_R.md>
- With too little bandwidth, CPU utilisation falls below 25 % in the swap phases, and core count stops mattering. <link: see archive:results_merged_2026-10-19/apumult_R.md>

**I/O volume**
- StorageReview's 100 T pi run (19 QLC SSDs in RAID 0, >38 GB/s write, 1.5 TB RAM) **read 35.7 PiB and wrote 31.5 PiB**. That is about **350 bytes written per digit**, roughly 29 TB per drive per day for 54 days. <link: see archive:results_merged_2026-10-19/apumult_R.md>
- The 314 T pi run used 34 Micron 6550 Gen5 SSDs (about 2.1 PB) for a 110-day run. <link: see archive:results_merged_2026-10-19/apumult_R.md>

**e records**
- **35 T digits**, Jordan Ranous, 2023-12-24: 2× Xeon Platinum 8460H, **512 GB RAM**, **94.5 h** compute and 92.5 h verify. The storage configuration is not published.
- 12 T, D. Christle, 2020: 252 GB RAM, **25 hard drives**, 19.4 days.
- 1 T, S. Kondo, 2010: 12 GB RAM, 8 × 1 TB, 224 h.
- Source: <link: see archive:results_merged_2026-10-19/apumult_R.md>
- In 2010, 500 G digits of e on a 12 GB machine with 4 HDDs (~480 MB/s) took 12.8 days. Yee noted that with enough drives, swap runs are "no longer dominated by disk access". <link: see archive:results_merged_2026-10-19/apumult_R.md>

**Lessons for us (inference, M)**
1. Our per-node ratio is SSD 2–7 GB/s against HBM 4 × 3.5 TB/s ≈ 14 TB/s, i.e. **~1/2000–1/7000**, while y-cruncher needs ≥ 1/4. **True out-of-core multiplication (streaming NTT operands from disk on every product) is therefore hopeless** on these nodes. It would be thousands of times slower than in-core.
2. Only **"park dead-until-later buffers"** spilling is viable. Each spilled byte crosses the disk twice per phase boundary, not hundreds of times per digit. That is exactly the apumult pattern (Q, P and µ spilled between phases), and its cost is simply `bytes/BW_write + bytes/BW_read`, which can partly overlap with compute.
3. Choose spill candidates by (bytes freed) ÷ (seconds of I/O not hidden by compute), and put them only at phase boundaries.
4. Use raw I/O, checksums on spill files (y-cruncher sees silent errors), and keep one file per buffer on a local XFS/ext4.

---

#### 5. Node-local storage on MI300A HPE Cray EX nodes

**EX255a blade**
- Two 4-APU node cards per blade, 4–8 Slingshot-11 200G ports per node.
- **"1 local NVMe M.2 SSD per node (up to 2 per blade)"**, optional, through the "HPE Cray EX255a SSD M.2 Support Kit" (S0N59A). **(H)** SDSC Cosmos PEARC25 slides: <link: see archive:results_merged_2026-10-19/apumult_R.md> ; <link: see archive:results_merged_2026-10-19/apumult_R.md> ; <link: see archive:results_merged_2026-10-19/apumult_R.md>
- HPE describes the in-system SSDs as attached "by PCIe 4". **(M)**
- **HPE's pages that would give the drive model and capacity (psnow datasheets, QuickSpecs) returned 403 or timed out.** The drive model and bandwidth are **unknown (L)**. A Gen4 x4 M.2 enterprise drive (e.g. 1.92–3.84 TB) is typically 5–7 GB/s read and 2–4 GB/s sustained write.
- A search snippet about HLRS Hunter claimed two M.2 drives, ~3.5 TB, 8/4 GB/s read/write. The Hunter KB page itself confirms only "some EX255a nodes feature a node-local NVMe drive", with the 3.5 TB drives on 16 *CPU* nodes and no bandwidth figure, so treat 8/4 GB/s as unverified. <link: see archive:results_merged_2026-10-19/apumult_R.md>
- **The target's `/ssd0` is presumably this M.2 drive (apumult ran there). Measure it; see §8.**

**El Capitan / Tuolumne / RZAdams (LLNL)**
- Compute blades have **no local SSD in use**. Local storage is **Rabbit** near-node flash: one Rabbit 4U per chassis holding 16 (+2 spare) SSDs, PCIe Gen4, direct-attached to 8 compute blades (16 nodes) through bulkhead cables, plus an EPYC Rabbit-P storage node. That is about **one SSD's worth per node**, shared, with an x8 PCIe path per node according to Next Platform.
- The default is **`/l/ssd` ≈ 200 GB per node, XFS, wiped at job end**. Larger allocations are available through Flux `#DW jobdw type=xfs capacity=1TiB`. There are 720 Rabbits for 11,520 nodes.
- The Lustre file system is 401 PB at 2.6+ TB/s for the whole machine, i.e. about 0.23 GB/s per node when all nodes write at once.
- `/tmp` is a RAM disk in HBM.
- **(H)** Sources: <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md>
- Rabbit per-node bandwidth is not published. **(L)**

**Other MI300A systems**
- SDSC Cosmos (42 nodes) uses VAST (~300–600 TB) and optional M.2.
- HLRS Hunter (188 APU nodes) has Lustre at ~155 GiB/s across the whole system.

**Implication for a 576-node target**
- Node-local M.2 is **optional hardware**. If present, expect **one Gen4 drive per node (about 2–4 GB/s write, 5–7 GB/s read, 1–4 TB)**.
- Lustre is not a spill target at scale: a system-wide 150–2600 GB/s shared by 576 nodes gives 0.3–4.5 GB/s per node, contended, and runs over the same Slingshot links as the SHMEM traffic.
- **Confirm the device, capacity and fio numbers on the target before planning d_max.** At 1.35×10¹⁴ digits the spill volume per node is several hundred GB, which must fit on /ssd0.

---

#### 6. Samsung 970 EVO Plus 2 TB (aac6 compute nodes)

**Datasheet and review figures**
- PCIe 3.0 x4, Phoenix controller, 2 GB LPDDR4.
- **3,500 MB/s read / 3,300 MB/s write** (spec). **1,200 TBW**, 5-year warranty.
- **Intelligent TurboWrite (SLC cache) ~78 GB** on the 2 TB model.
- Once the cache is exhausted, direct-to-TLC writes run at **~1.5–1.75 GB/s**.
- **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md> , Samsung datasheet (smaller capacities: 42 GB cache/1 TB, 600 TBW/1 TB) <link: see archive:results_merged_2026-10-19/apumult_R.md>

**Revision risk**
- Units made after about 2021 use the **Elpis** controller: ~3× larger SLC cache, but **sustained writes fall to ~0.8 GB/s** after it, against ~1.5 GB/s for the original, and the drive runs ~10–20 °C hotter (up to ~100 °C).
- Check the PSID/model string with `nvme id-ctrl`. The old part is `MZVLB2T0HALB`, the new one `MZVL22T0HBLB`. These names follow the published 1 TB pattern MZVLB1T0HBLR / MZVL21T0HBLU and are inferred for 2 TB, so check them.
- **(H)** <link: see archive:results_merged_2026-10-19/apumult_R.md> , <link: see archive:results_merged_2026-10-19/apumult_R.md>

**Throughput per spill**

| Spill size | Write time | Read back | Round trip |
|---|---|---|---|
| 200 GB (Phoenix) | ~24 s at 3.3 GB/s + ~80 s at 1.5 GB/s ≈ **105 s** | ~60 s at ~3.3 GB/s | **~165 s** |
| 200 GB (Elpis, estimate) | ~150–250 s | ~60 s | ~210–310 s |

- The SLC cache only refills while the drive is idle, so back-to-back spills get the slow rate.

**Endurance**
- 1,200 TBW ÷ 0.2–0.5 TB written per run gives **2,400–6,000 runs**, so endurance is **not a concern** for development use.
- Keep the drive below ~80 % full: a full TLC drive shrinks the dynamic SLC cache and slows sustained writes.
- Thermal throttling is possible under minutes of sustained writes if the chassis has no airflow over the M.2.

**aac6 caveat**
- On the node recorded in `aac6-environment.md`, `/` is `/dev/md0p1`: **XFS, 800 G partition, ~293 G free**. So the *capacity* available for spill may be well under the drive size, and the md layout (RAID0 vs RAID1) changes bandwidth.
- Run `lsblk` and `mdadm --detail` on a compute node before relying on either figure.

---

#### 7. Implications

##### (a) Can an APU pipeline spill to disk without the page cache taking the freed memory?

Yes, provided the spill code follows these rules:
1. Use **O_DIRECT** for every spill write and read-back, or RWF_DONTCACHE on kernel ≥ 6.14. Never use buffered I/O, and never use `/tmp`, which is HBM.
2. Do the O_DIRECT from a **pinned hipHostMalloc bounce ring**, or from buffers that are themselves hipHostMalloc memory. **A hipMalloc pointer cannot be the O_DIRECT buffer (EFAULT).**
3. If anything is written buffered (e.g. final digit output), follow it with `fdatasync` and then `posix_fadvise(DONTNEED)`, as the ecalc scripts already do for the reference file.
4. Release device memory with `hipFree` of chunks or VMM unmap/release, not madvise. madvise is right only for malloc'd (host) memory.
5. Watchdogs must count TTM-pool pages as reclaimable, or read `/sys/kernel/debug/ttm/page_pool` or the amdgpu memory counters, not just MemFree.

##### (b) Realistic spill bandwidth

- **aac6:** ~1.5–1.75 GB/s sustained write after 78 GB (possibly ~0.8 GB/s on an Elpis unit), ~3.3 GB/s read. That is about **165 s per 200 GB round trip**, not hideable behind a 112 s run at 4×10¹⁰, and it gets worse as d grows. Treat aac6 as a place to test correctness, not to judge performance.
- **Target:** unknown model. Likely one Gen4 M.2 at **2–4 GB/s write, 5–7 GB/s read**, about 50–100 s per 200 GB round trip.
- **Budget rule:** spill time ≈ Σ(bytes)/BW_w + Σ(bytes)/BW_r. Only spills whose I/O overlaps a compute phase of similar length are cheap.
- apumult's own figures fit this picture: 3.4× digits but 3.2× slower at 4×10¹⁰ (234.8 s against 73.1 s).

##### (c) Does HIP VMM give a cheap "release dead pages" for device buffers?

- **Functionally, yes.** Unmap and release chunks of a reserved VA range, then create and map them back later. The pages come back to the system and are clean.
- **Cost:** kernel zeroing at ~35–40 ms/GiB when pages come back, the same as hipMalloc. That is small next to any disk traffic (seconds per 100 GiB, against tens of seconds of I/O).
- **Cost in maturity:** Beta API, open gfx94x issues, an fd-per-handle bug in 7.14, and it is still counted against the amdttm pages_limit.
- **Recommendation:** implement "release" as **chunked device allocations** (hipMalloc/hipFree of ≥ 256 MiB chunks behind the existing dbig block-pool). Adopt VMM only if a flat contiguous VA is truly needed by the NTT kernels.
- Either way, **release is cheap. The expensive part of the apumult techniques is the SSD round trip, not the page release.**

---

#### 8. Tests to run on aac6 (each ~1 minute)

1. **VMM support.** Print `hipDeviceAttributeVirtualMemoryManagementSupported` and `hipMemGetAllocationGranularity` (MINIMUM and RECOMMENDED). Then time a loop of 64 × 1 GiB `hipMemCreate`+`hipMemMap`+`hipMemSetAccess`, followed by unmap and release, and check `free -g` in between.
2. **O_DIRECT targets.** Open a file with `O_DIRECT` and `pread` 64 MiB into (i) a hipMalloc pointer, where EFAULT is expected, (ii) a hipHostMalloc pointer, where OK is expected, and (iii) posix_memalign memory, where OK is expected.
3. **madvise.** `madvise(MADV_DONTNEED)` on a hipMalloc pointer should return EINVAL. On hipHostMalloc memory, check whether RSS or MemFree actually drops (it probably does not).
4. **Disk.**
   `fio --name=w --filename=/path/on/ssd/f --rw=write --bs=4M --iodepth=16 --ioengine=io_uring --direct=1 --size=200G`, then the same with `--rw=read`. Record the bandwidth curve (`--write_bw_log`) to see where the SLC cache runs out. Run `nvme id-ctrl /dev/nvme0` to identify the controller revision. Repeat on the target's `/ssd0`.
5. **Page-cache effect.** Write 100 GB buffered, then hipMalloc 100 GiB and time an NTT pass. Repeat after `fadvise(DONTNEED)`. This checks the fragment-fallback prediction in §1.3.
