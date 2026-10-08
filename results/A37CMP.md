# A37CMP: apumult a37v1 spec against ecalc, item by item

Tasked by the main session (read-only on code; this file is the only write). Sources: the a37v1 spec (scratchpad copy), ecalc `main`
@ 6e220db (`ecalc/*.c|h`), `docs/code/00,01,02,05`, `docs/APUMULT_STUDY.md`, `TASKS.md`, `RESULTS.md`, `results/S19B.md`, `results/N-kernel.md`,
`results/K13b.md`, `results/TGTBENCH2.md`. Labels: **(m)** measured, **(mod)** modelled, **(a)** assumed. Where unsure, the row says so.
Spec sections: the spec has no §17 (it jumps 16 -> 18); §5 and §6 are summarised in one line each in the spec itself.

## 0. The comparison in five sentences

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

## 1. Summary table (all items)

Key to "ecalc status": SAME / EQUIV (different code, same effect) / DIFF (different design) / ABSENT / N/A. "Idea?" = Y (worth a task), M (measure first), N (no).
Benefit scale factors used: ecalc per-node share 6.441e10 digits = 1.61x a37v1's 4e10; terms per node about 6.1e9 (10 nodes) to 7.5e9 (576) = 1.4-1.7x a37v1's 4.35e9.

### §1 Algorithm

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 1 | Taylor series, N = min m with lgamma(m+1)/ln10 >= d+50, bisection | SAME (`binsplit.c` `e_terms`, bisection; d+50) | N | none | - | - | done (WP3) |
| 2 | Binary splitting P(a,a+1)=1, Q=a+1; combine P=P1 Q2+P2, Q=Q1 Q2; e=1+P/Q | SAME (02 §1.1; S = P+Q in place, `ecalc.c:730`) | N | none | - | - | - |
| 3 | Division = Newton reciprocal + one Barrett quotient | DIFF: DKM two halves, half-length reciprocal, low products, corrections (`newton_db.c:372-456`) | N (see #82) | none | - | - | DKM -25.7 s at 1e11 (m, RESULTS §92) |
| 4 | Leading "2" by digit offset | EQUIV (ecalc divides S=P+Q, the 2 is in X) | N | none | - | - | - |
| 5 | Verify: 50-digit windows at 6 known offsets | DIFF: T1 (P,Q,X,R,digits mod 8 primes) + T2 windows vs `e_1e11.out`; no reference above 1e11 (01 §1.8, 04 §7) | N | none | - | - | ecalc stronger |

### §2 / §2b Representation and primitives

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

### §3 Flow

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 16 | Free batch buffers (-37 GB) before the division | EQUIV: plane pool tails donated to the block pool (`ecalc.c:709`), arena borrowed by dm | N | - | - | - | A2/PS notes |
| 17 | A = P shifted into a zero-on-fault buffer (33.6 GB) | DIFF, ecalc better: A is **never formed** (10dP: S=P+Q in place; RESULTS §70) | N | none | - | - | ecalc ahead |
| 18 | dm 19.4 s, str 0.5 s, 10dP 0.14 s, copyout 0.4 s at 4e10 | ecalc 4e10: total 63.5 s (m) | N | - | - | - | see #119 |

### §4 Binary splitting arena, seed, level loop

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

### §5-§7b Tiers

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

### §8 / §8b NTT and modmul

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

### §9 Newton division

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

### §10-§11 Output, memory

| # | spec item | ecalc status | idea? | benefit | effort | risk | listed / rejected |
|---|---|---|---|---|---|---|---|
| 80 | `bd_to_str_par`: 2-digit table d2[200], 9 lookups per limb, 0.5 s at 4e10 (= 1.2 ns/digit-core) | DIFF: `fmt18` (`mn_out.c:90`) peels 18 digits with %10 and /10; packed default writes limbs as they are. The model constant `DC_FMT_MN` = 0.061 s per 1e9 digits (m, size 1) = 5.8 ns/digit-core; S19B `dc` = 2.8-4.1 s at 6.44e10 | Y: table-based `fmt18` after a profile of `dc` | <= about 3 s per node (mod: 3.9 s -> about 0.8 s if `dc` is formatting). **Unsure** whether `dc` in the no-write runs is formatting, the D2H fetch or T1 residues (`packed_mods`) | S | low (bit-identical output) | new |
| 81 | Memory layout at d48: arenas 43.6 GB x2, ch/dev planes, hstage 16 GB x2, Pout/Qout, A, r/rd | ecalc 363.5 GB device / 405.6 GB node at 6.44e10 (mod, S18TGT), 5.6 B/digit; no A, no Pout/Qout, no 32 GB hstage | N | no transferable saving found | - | - | PLAN L1/E2 (-51.7 GB at 1e11 mod), DM_MN_LEAN, DC8, S-1 |
| 82 | Alloc order: preinit -> arenas -> pregrow/hpre -> seed -> join -> levels | EQUIV (planes first, arena VMM background, seeds from the hook) | N | - | - | - | - |
| 83 | KFD serializes hipMalloc across dies | CONFIRMED independently (RL: VMM calls serialized) | N | - | - | - | - |

### §12 Environment flags (every flag)

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

### §13-§14 Timing and ceilings

| # | spec item | ecalc | idea? | benefit | effort | risk | note |
|---|---|---|---|---|---|---|---|
| 119 | bs 60.5 s = arena 5 + max(seed 25.9, pg 21.5, hpre 11) + levels 28.2 | ecalc 4e10 total 63.5 s (m); at 6.44e10 per node: init 25 + leaf 36 s at 10 nodes (m, S19B §1) | - | - | - | - | init floor is a fixed ref init 16.5 s + seeds |
| 120 | Ceilings: scale 2510, triad 3017, read 3495, copy 2998 GB/s; FP64 57.7 TF | ecalc 3.0 TB/s copy, b16 passes 1.3-1.6 TB/s (m, K13b) | - | - | - | - | consistent |

### §15 Pitfalls

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

### §16 / §18

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

## 2. Ranked shortlist of recommended NEW task-list additions

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

## 3. Rejected items (one line each)

- **R1 (#77) skip the remainder check / SKIP_XQ:** T1 residue verification of X needs R and is the only verifier at 3.7e13 digits (no reference above 1e11); and the full-length reciprocal's extra doubling costs about what the two low products do, so the model gain is at most 0.4u (5-18 s at 10 nodes, low confidence).
- **R2 (#20) Stirling-balanced leaf boundaries:** a node's term range spans < 3 % in log10 k except rank 0, so leaf sizes are already uniform; `BS_SEED_FILL` already fits the last span to 2^k.
- **R3 (#6) global base 10^19:** a whole-code change (kernels, T1, packed format, P24) for 5.6 % in the 3-prime batch tier only; ecalc's 4-prime P24 is already denser (24 digits/pt).
- **R4 (#85) HSA_XNACK=1:** ecalc measured 2x slower (RESULTS §55) and gains nothing it lacks; keep XNACK off (and it removes the SDMA hazard of #121).
- **R5 (#24) arenas on two dies:** loses ecalc's subtree-per-APU locality (3.8 TB/s local vs 93 GB/s remote (m)).
- **R6 (#32) CPU school tier for level 0:** the seed thread already saturates the CPU and RESULTS §58 measured the school tier as a net loss (42-59 s) with device pools.
- Also not adopted because ecalc is already equal or ahead: the Dekker/rint modmul (#65: rint == floor, ecalc faster), the 4-step NTT (#64), the stride-17 tile (#57), PW/L^-1 fusion (#44-45), the Newton details (#69-76), the batch tier structure (#38-39, #41, #46), the ping-pong arena (#22), and A never formed (#17).

## 4. Open questions needing measurement

1. **Q1 Seed on the target.** Does the seed thread (about 22 s (mod) at 6.44e10) really bind init at 576 nodes, or does the 16.5 s reference init bind? The model says seed wait 9.3-12.5 s; the only measurement is the SC rehearsal at 9.17e10 (seed ends 31.75 s vs mapping 30.62 s, aac6). A single-node `ECALC_INIT_TL=1` run at 6.44e10 on the target would settle R1's value; on aac7 it cannot (mapping is slow there).
2. **Q2 Seed stream handoff.** Does the DMA/chunk handoff (two pinned 8 GiB buffers, `binsplit.c:1458-1503`) ever stall the seed threads? A per-thread wait counter in `seeds_stream` would show it.
3. **Q3 XNACK retest.** Only if hipHostRegister/pinning cost shows up in a profile; a37v1 runs XNACK=1 without the 2x loss ecalc saw in 2026-09, so the old measurement may be design-specific (unsure).
4. **Q4 Pass-level NTT breakdown.** Run the a37v1-style NOP-modmul build on ecalc's 2^31 forward (HBM floor 51 ms (a37v1) vs ecalc 99 ms) to see whether the remaining gap is structure or modmul; also whether a WbPowTab-style twiddle seed (#58) matters.
5. **Q5 FMA contraction.** Confirm in the ISA of `k_b16r` that `hi = a*b` is never fused (a37v1 needed an asm barrier).
6. **Q6 SHMEM pool pre-touch.** Does Cray/SOS `shmem_malloc` fault in its symmetric heap before `hipHostRegister`? (R3.)
7. **Q7 What is `dc`?** Split S19B's 2.8-4.1 s into D2H fetch, formatting and T1 residues.
8. **Q8 Reciprocal chain floor and wait** (R6): per-doubling timestamps for j < 34126 (`ECALC_LOG_CLOCKS`, S19B §5 #4).
9. **Q9 Batch tier at the target share.** a37v1's batch tier is 21.0 s at 4e10; ecalc's last full by-phase breakdown is at 9.5e10 (25.1 s). A fresh by-phase run at 6.44e10 (10 nodes, `BS_LAYOUT`/phase timers) is needed before any R5/R7/R8 gain is trusted; all their numbers are scaled from 4e10 N-kernel data.

## 5. Caveats

- ecalc numbers for the NTT (K13b), batch tier (N-kernel) and RESULTS §19/§31 are from earlier code and aac6/aac7 nodes; a37v1's from the spec's own node. Same GPU model, different ROCm and clocks (a37v1 does not state them): treat "1.45x faster" as indicative.
- The division model in #77 uses unit costs inferred from S19B's per-product walls; low confidence.
- I did not run anything; no code or branch was touched. Rows marked "cannot compare" are flags whose meaning the spec does not give.

Decisions for the user: none are forced. The only judgment call is R1 (about 2-4 days for -7…-12 s modelled at the target, with R2 as a half-day gate).
